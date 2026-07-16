import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import * as KernelSU from '@/helpers/KernelSU'

const configPath = '/data/adb/.config/flux'

// How many data points to keep in history (each tick = 1s → 60 points = 1 min)
const HISTORY_MAX = 60

// Must match SYNTHESIS_SCHEMA_VERSION in Flux.hpp / TelemetryContract.SCHEMA_VERSION.
const SUPPORTED_SCHEMA_VERSION = 2

// Freshness thresholds, mirrored from the C++ FreshnessPolicy so the UI's notion of
// "stale" agrees with the daemon's. The producer republishes at least every 2 s, so a gap
// past these means real trouble, not a slow tick.
const STALE_AFTER_MS = 5000
const OFFLINE_AFTER_MS = 15000

// Android PowerManager thermal status enum, as emitted in `thermal_status`.
const THERMAL_STATUS = {
  '-1': 'unknown',
  0: 'none',
  1: 'light',
  2: 'moderate',
  3: 'severe',
  4: 'critical',
  5: 'emergency',
  6: 'shutdown',
}

export const useMonitorStore = defineStore('monitor', () => {
  // ── Raw live state (schema v2) ──────────────────────────────────────────────
  const schemaVersion = ref(0)
  const sequence = ref(0)
  const daemonPid = ref(0)

  const foregroundAvailable = ref(false)
  const focusedPackage = ref('—')
  const focusedPid = ref(0)
  const focusedUid = ref(0)

  const screenAvailable = ref(false)
  const screenAwake = ref(false)

  const powerAvailable = ref(false)
  const batterySaver = ref(false)

  const chargingAvailable = ref(false)
  const charging = ref(false)

  // Thermal — real Android semantics: headroom is HIGHER when HOTTER.
  // 0.0 = no thermal pressure, 1.0 = severe-throttling threshold, >1.0 = past it.
  const thermalAvailable = ref(false)
  const thermalValid = ref(false)
  const thermalHeadroom = ref(NaN)
  const thermalStatus = ref(-1)
  const thermalAgeMs = ref(0)

  const audioAvailable = ref(false)
  const audioActive = ref(false)

  const zenAvailable = ref(false)
  const zenMode = ref(0)

  const kernelIsGki = ref(false)
  const currentProfile = ref('initializing')

  // ── History arrays (for sparkline charts) ──────────────────────────────────
  // Each entry: { t, v } where v is null for a missing/invalid sample, so the chart can
  // render an honest gap rather than drawing a fabricated 0.
  const thermalHistory = ref([])
  const profileHistory = ref([])

  // ── Internal ────────────────────────────────────────────────────────────────
  let pollInterval = null
  const isInitialized = ref(false)
  const lastError = ref('')
  const parseError = ref('')
  // Wall-clock time (ms) at which we last accepted a well-formed snapshot. The freshness
  // check runs against this — the producer's own clock cannot report that the producer died.
  const lastAcceptedAt = ref(0)

  // ── Health ──────────────────────────────────────────────────────────────────

  /**
   * Telemetry health, mirroring the daemon's TelemetryHealth. Drives every "is this real?"
   * decision in the UI: a stale or offline stream must never be shown as live data.
   */
  const health = computed(() => {
    if (lastAcceptedAt.value === 0) return 'offline'
    const age = Date.now() - lastAcceptedAt.value
    if (age >= OFFLINE_AFTER_MS) return 'offline'
    if (age >= STALE_AFTER_MS) return 'stale'
    return 'healthy'
  })

  const isLive = computed(() => health.value === 'healthy')

  // ── Thermal computed ─────────────────────────────────────────────────────────

  const thermalSupported = computed(() => thermalAvailable.value)

  /** True only when there is a usable reading to show right now. */
  const thermalReadable = computed(
    () => thermalAvailable.value && thermalValid.value && !Number.isNaN(thermalHeadroom.value),
  )

  /**
   * Thermal tier from the daemon's thresholds — with the CORRECT direction. Headroom is
   * higher when hotter, so a high value is "hot", not "cool". The previous UI had this
   * inverted, matching the inverted daemon logic.
   */
  const thermalLabel = computed(() => {
    if (!thermalAvailable.value) return 'unsupported'
    if (!thermalReadable.value) return 'unknown'
    const h = thermalHeadroom.value
    if (h >= 0.85) return 'hot'
    if (h >= 0.7) return 'warm'
    return 'cool'
  })

  const thermalStatusLabel = computed(
    () => THERMAL_STATUS[String(thermalStatus.value)] ?? 'unknown',
  )

  /**
   * Headroom as a 0–100% "thermal pressure" figure for a bar/gauge. Clamped only for
   * display; the raw headroom above 1.0 is preserved in thermalHeadroom for anyone who
   * needs the true value.
   */
  const thermalPressurePercent = computed(() =>
    thermalReadable.value ? Math.min(100, Math.round(thermalHeadroom.value * 100)) : null,
  )

  // ── Profile map (mirrors Flux.hpp FluxProfileMode) ──────────────────────────
  const profileMap = {
    0: 'initializing',
    1: 'performance',
    2: 'performance_lite',
    3: 'balanced',
    4: 'powersave',
  }

  // ── Actions ─────────────────────────────────────────────────────────────────

  async function init() {
    if (isInitialized.value) return
    isInitialized.value = true
    await tick()
    startPolling()
  }

  function startPolling() {
    stopPolling()
    pollInterval = setInterval(tick, 1000)
  }

  function stopPolling() {
    if (pollInterval) {
      clearInterval(pollInterval)
      pollInterval = null
    }
  }

  async function tick() {
    await Promise.all([readSynthesisCore(), readCurrentProfile()])
    recordHistory()
  }

  async function readSynthesisCore() {
    try {
      const raw = await KernelSU.readFile(`${configPath}/synthesis_core.json`)
      parseSynthesisCore(raw)
      lastError.value = ''
    } catch (e) {
      lastError.value = e.message
    }
  }

  /**
   * Parse the schema-v2 line-oriented telemetry snapshot into a staging object, and only
   * commit it once the schema version checks out. A snapshot from an incompatible producer
   * must not be half-applied over the live values.
   */
  function parseSynthesisCore(raw) {
    if (!raw) return

    const fields = {}
    for (const line of raw.split('\n')) {
      const trimmed = line.trim()
      if (!trimmed) continue
      const idx = trimmed.indexOf(' ')
      if (idx <= 0) continue
      fields[trimmed.slice(0, idx)] = trimmed.slice(idx + 1)
    }

    const version = parseInt(fields['schema_version'], 10)
    if (Number.isNaN(version)) {
      parseError.value = 'missing schema_version'
      return
    }
    if (version !== SUPPORTED_SCHEMA_VERSION) {
      parseError.value = `unsupported schema ${version} (this UI speaks ${SUPPORTED_SCHEMA_VERSION})`
      return
    }

    const bool = (key) => fields[key] === '1'
    const int = (key) => parseInt(fields[key], 10) || 0
    const float = (key) => {
      const v = parseFloat(fields[key])
      return Number.isNaN(v) ? NaN : v
    }

    schemaVersion.value = version
    sequence.value = int('sequence')
    daemonPid.value = int('daemon_pid')

    foregroundAvailable.value = bool('foreground_available')
    focusedPackage.value = fields['focused_package'] || '—'
    focusedPid.value = int('focused_pid')
    focusedUid.value = int('focused_uid')

    screenAvailable.value = bool('screen_available')
    screenAwake.value = bool('screen_awake')

    powerAvailable.value = bool('power_available')
    batterySaver.value = bool('battery_saver')

    chargingAvailable.value = bool('charging_available')
    charging.value = bool('charging_state')

    thermalAvailable.value = bool('thermal_available')
    thermalValid.value = bool('thermal_valid')
    thermalHeadroom.value = float('thermal_headroom')
    thermalStatus.value = int('thermal_status')
    thermalAgeMs.value = int('thermal_age_ms')

    audioAvailable.value = bool('audio_available')
    audioActive.value = bool('audio_active')

    zenAvailable.value = bool('zen_available')
    zenMode.value = int('zen_mode')

    kernelIsGki.value = bool('kernel_is_gki')

    parseError.value = ''
    lastAcceptedAt.value = Date.now()
  }

  async function readCurrentProfile() {
    try {
      const raw = await KernelSU.readFile(`${configPath}/current_profile`)
      const trimmed = raw.trim()
      currentProfile.value = profileMap[trimmed] ?? 'unknown'
    } catch {
      currentProfile.value = 'unknown'
    }
  }

  function recordHistory() {
    const now = Date.now()

    // null when there is no usable reading, so the chart shows a gap instead of a fake 0.
    thermalHistory.value.push({ t: now, v: thermalReadable.value ? thermalHeadroom.value : null })
    if (thermalHistory.value.length > HISTORY_MAX) thermalHistory.value.shift()

    const profileInt = parseInt(
      Object.keys(profileMap).find((k) => profileMap[k] === currentProfile.value) ?? '0',
      10,
    )
    profileHistory.value.push({ t: now, v: profileInt })
    if (profileHistory.value.length > HISTORY_MAX) profileHistory.value.shift()
  }

  return {
    // live state
    schemaVersion,
    sequence,
    daemonPid,
    foregroundAvailable,
    focusedPackage,
    // Back-compat alias: existing views bind `focusedApp`.
    focusedApp: focusedPackage,
    focusedPid,
    focusedUid,
    screenAvailable,
    screenAwake,
    powerAvailable,
    batterySaver,
    chargingAvailable,
    charging,
    thermalAvailable,
    thermalValid,
    thermalHeadroom,
    thermalStatus,
    thermalAgeMs,
    audioAvailable,
    audioActive,
    zenAvailable,
    zenMode,
    kernelIsGki,
    currentProfile,
    // computed
    health,
    isLive,
    thermalSupported,
    thermalReadable,
    thermalLabel,
    thermalStatusLabel,
    thermalPressurePercent,
    // Back-compat alias for existing views. Note the corrected meaning: this is headroom
    // toward the severe threshold, so a HIGH number now means HOT (it meant cool before).
    thermalPercent: thermalPressurePercent,
    // history
    thermalHistory,
    profileHistory,
    // meta
    isInitialized,
    lastError,
    parseError,
    lastAcceptedAt,
    // actions
    init,
    stopPolling,
  }
})
