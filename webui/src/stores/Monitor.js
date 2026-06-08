import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import * as KernelSU from '@/helpers/KernelSU'

const configPath = '/data/adb/.config/flux'

// How many data points to keep in history (each tick = 1s → 60 points = 1 min)
const HISTORY_MAX = 60

export const useMonitorStore = defineStore('monitor', () => {
  // ── Raw live state ──────────────────────────────────────────────────────────
  const focusedApp      = ref('—')
  const focusedPid      = ref(0)
  const focusedUid      = ref(0)
  const screenAwake     = ref(false)
  const batterySaver    = ref(false)
  const zenMode         = ref(0)
  const charging        = ref(false)
  const thermalHeadroom     = ref(-1)    // -1 = unsupported
  const audioActive         = ref(false)
  const currentProfile      = ref('initializing')
  // Reported by SynthesisCore: 1 if getThermalHeadroom() resolved at init.
  // Lets WebUI distinguish "API absent" from "API present but returning NaN".
  const thermalApiAvailable = ref(false)
  // Reported by SynthesisCore: 1 if uname -r matches the GKI "-androidXX-" pattern.
  const kernelIsGki         = ref(false)

  // ── History arrays (for sparkline charts) ──────────────────────────────────
  // Each entry: { t: timestamp_ms, v: number }
  const thermalHistory  = ref([])   // 0.0–1.0, -1 when unsupported
  const profileHistory  = ref([])   // 0–4 (FluxProfileMode int)

  // ── Internal ────────────────────────────────────────────────────────────────
  let pollInterval = null
  const isInitialized = ref(false)
  const lastError = ref('')

  // ── Computed helpers ────────────────────────────────────────────────────────

  /**
   * true when getThermalHeadroom() is both available (API resolved) AND
   * returning a valid non-negative value on this device.
   * thermalApiAvailable=false  → API absent (SDK < 31 or method missing)
   * thermalApiAvailable=true + thermalHeadroom=-1 → API present but NaN (rare)
   */
  const thermalSupported = computed(() =>
    thermalApiAvailable.value && thermalHeadroom.value >= 0
  )

  /**
   * Thermal tier label derived from the same thresholds used by the C++ daemon:
   *   < 0.20  → Hot
   *   0.20–0.35 → Warm
   *   >= 0.35 → Cool
   */
  const thermalLabel = computed(() => {
    if (!thermalSupported.value) return 'unsupported'
    const h = thermalHeadroom.value
    if (h < 0.20) return 'hot'
    if (h < 0.35) return 'warm'
    return 'cool'
  })

  const thermalPercent = computed(() =>
    thermalSupported.value ? Math.round(thermalHeadroom.value * 100) : null
  )

  // ── Profile map (mirrors Flux.hpp FluxProfileMode) ──────────────────────────
  const profileMap = {
    '0': 'initializing',
    '1': 'performance',
    '2': 'performance_lite',
    '3': 'balanced',
    '4': 'powersave',
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
   * Parse the line-oriented synthesis_core.json format:
   *   focused_app <pkg> <pid> <uid>
   *   screen_awake 1
   *   battery_saver 0
   *   zen_mode 0
   *   charging_state 1
   *   thermal_status 0.82
   *   audio_active 0
   *   thermal_api_available 1   ← new: 1 if getThermalHeadroom() resolved
   *   kernel_is_gki 1           ← new: 1 if uname -r matches GKI pattern
   */
  function parseSynthesisCore(raw) {
    if (!raw) return
    for (const line of raw.split('\n')) {
      const parts = line.trim().split(/\s+/)
      if (parts.length < 2) continue
      const key = parts[0]
      switch (key) {
        case 'focused_app':
          focusedApp.value = parts[1] ?? '—'
          focusedPid.value = parseInt(parts[2]) || 0
          focusedUid.value = parseInt(parts[3]) || 0
          break
        case 'screen_awake':
          screenAwake.value = parts[1] === '1'
          break
        case 'battery_saver':
          batterySaver.value = parts[1] === '1'
          break
        case 'zen_mode':
          zenMode.value = parseInt(parts[1]) || 0
          break
        case 'charging_state':
          charging.value = parts[1] === '1'
          break
        case 'thermal_status': {
          const v = parseFloat(parts[1])
          // parseFloat("NaN") returns NaN — treat as unsupported (-1)
          thermalHeadroom.value = (isNaN(v) || v < 0) ? -1 : v
          break
        }
        case 'audio_active':
          audioActive.value = parts[1] === '1'
          break
        case 'thermal_api_available':
          thermalApiAvailable.value = parts[1] === '1'
          break
        case 'kernel_is_gki':
          kernelIsGki.value = parts[1] === '1'
          break
      }
    }
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

    thermalHistory.value.push({ t: now, v: thermalHeadroom.value })
    if (thermalHistory.value.length > HISTORY_MAX) thermalHistory.value.shift()

    const profileInt = parseInt(
      Object.keys(profileMap).find(k => profileMap[k] === currentProfile.value) ?? '0'
    )
    profileHistory.value.push({ t: now, v: profileInt })
    if (profileHistory.value.length > HISTORY_MAX) profileHistory.value.shift()
  }

  return {
    // live state
    focusedApp,
    focusedPid,
    focusedUid,
    screenAwake,
    batterySaver,
    zenMode,
    charging,
    thermalHeadroom,
    audioActive,
    currentProfile,
    thermalApiAvailable,
    kernelIsGki,
    // computed
    thermalSupported,
    thermalLabel,
    thermalPercent,
    // history
    thermalHistory,
    profileHistory,
    // meta
    isInitialized,
    lastError,
    // actions
    init,
    stopPolling,
  }
})
