/*
 * Copyright (C) 2026 FebriCahyaa
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "CapabilityState.hpp"
#include "SysfsNodeBackend.hpp"

/**
 * @file DeviceDescriptor.hpp
 * @brief The versioned declarative descriptor schema, its validator, and the runtime probe.
 *
 * Flux-owned mechanism (Category A). The *data* expressed in this schema may be derived from
 * elsewhere and is kept separately under `jni/device/` with its own attribution; this file
 * knows nothing about any particular device.
 *
 * ## Data, not code
 *
 * A descriptor is a plain struct of values. It cannot carry a shell command, a callback, a
 * path built at runtime, or a mode to chmod to. It cannot reach a NodeBackend — only the
 * engine writes, and it writes only what a validated descriptor declared in advance.
 *
 * That boundary is the entire reason this schema exists. The legacy applier expressed device
 * knowledge as *executable shell*, so every new device meant new privileged code, and no
 * validation was possible before running it on the device. Here, device knowledge is inert
 * data that can be validated, diffed, dry-run and reviewed without a device present, and a
 * malformed pack is rejected at load rather than discovered at runtime.
 *
 * ## Versioning
 *
 * `schema_version` is mandatory and checked. Packs outlive the code that reads them: a pack
 * declaring a schema this build does not implement is refused as a whole, rather than being
 * partially understood — a half-understood tuning descriptor is a wrong write.
 */
namespace flux::execution {

/// The descriptor schema this build implements.
inline constexpr int kDescriptorSchemaVersion = 1;
/// The oldest schema this build still accepts.
inline constexpr int kDescriptorSchemaMin = 1;

/**
 * @brief SoC families Flux has descriptor sets for.
 *
 * The numeric values match the existing install-time `soc_recognition` codes so the two agree
 * during the migration. SoC identity is *necessary but never sufficient*: matching the family
 * only makes a descriptor a candidate, and probing decides.
 */
enum class SocFamily {
    Unknown = 0,
    MediaTek = 1,
    Snapdragon = 2,
    Exynos = 3,
    Unisoc = 4,
    Tensor = 5,
    Tegra = 6,
    /// Applies regardless of SoC — the standard kernel interfaces every device has.
    Generic = 100,
};

const char *soc_family_name(SocFamily family);
SocFamily soc_family_from_code(int code);

/** What kind of value a node holds. Decides how the value is validated. */
enum class NodeValueType {
    Integer,  ///< a decimal integer within [min,max]
    Token,    ///< one bare word, from `allowed` when non-empty
    Enum,     ///< one of `allowed`, which must be non-empty
};

/** How (and whether) a write can be confirmed. */
enum class ReadBackStrategy {
    None,       ///< the node cannot be read back; the write is unverifiable
    Exact,      ///< read must equal what was written
    Numeric,    ///< read must parse to the same number (tolerates "800" vs "800 ")
    Contains,   ///< read must contain the written token (multi-field vendor nodes)
};

/** How a node can be put back the way it was found. */
enum class RollbackStrategy {
    None,           ///< no rollback is possible; the descriptor must say so honestly
    RestoreOriginal ///< capture the value before the first write and restore it later
};

/** Broad grouping, used for rollback boundaries and diagnostics. */
enum class CapabilityGroup {
    CpuPolicy,
    GpuPolicy,
    Scheduler,
    Memory,
    TouchInput,
    Zen,
    PowerMode,
    DeviceSpecific,
};

const char *capability_group_name(CapabilityGroup group);

/** Where this descriptor's knowledge came from. Recorded per descriptor, not per file. */
enum class Provenance {
    /// Independently written by Flux from documented kernel interfaces.
    FluxAuthored,
    /// Device knowledge derived from Encore Tweaks (Apache-2.0, Rem01Gaming). Attribution is
    /// required wherever this appears. See docs/provenance-matrix.md.
    DerivedEncore,
};

const char *provenance_name(Provenance provenance);

/**
 * @brief One declarative capability descriptor. Pure data.
 *
 * Deliberately absent: any function pointer, command string, format string, or mode. If a
 * future capability seems to need one of those, that is a signal it belongs in the Category A
 * engine as a new mechanism — not smuggled into a data pack.
 */
struct CapabilityDescriptor {
    int schema_version = kDescriptorSchemaVersion;

    std::string descriptor_id; ///< unique within a pack, e.g. "mtk.gpu.power_limited"
    std::string capability_id; ///< logical capability, may be shared across families
    CapabilityGroup group = CapabilityGroup::DeviceSpecific;
    SocFamily soc = SocFamily::Generic;

    /// Absolute path, checked against the approved roots. May contain a single '*' segment
    /// wildcard: vendor devfreq nodes are enumerated by the kernel with unstable names
    /// (`/sys/class/devfreq/*ddr-lat*/...`). The wildcard is resolved at probe time and the
    /// *resolved* path is re-checked against the allowlist — a wildcard can never widen it.
    std::string path;

    NodeValueType value_type = NodeValueType::Token;
    long min = 0;
    long max = 0;
    std::vector<std::string> allowed;

    /// Desired value per policy intent. Absent intent means this descriptor has nothing to say
    /// about that policy, which is different from having something to say and it being "0".
    std::map<std::string, std::string> policy_values;

    ReadBackStrategy read_back = ReadBackStrategy::Exact;
    RollbackStrategy rollback = RollbackStrategy::RestoreOriginal;

    int order_group = 0;                   ///< deterministic apply ordering, low first
    std::vector<std::string> depends_on;   ///< descriptor_ids that must also be executable
    bool critical = false;                 ///< a failure here fails the whole group

    Provenance provenance = Provenance::FluxAuthored;
    ValidationStatus validation = ValidationStatus::PhysicalDeviceRequired;
};

/** A named, versioned set of descriptors — one family's knowledge. */
struct DevicePack {
    int schema_version = kDescriptorSchemaVersion;
    std::string pack_id;
    SocFamily soc = SocFamily::Generic;
    Provenance provenance = Provenance::FluxAuthored;
    /// Attribution that must travel with this pack when its provenance is derived. Empty is
    /// only correct for FluxAuthored packs; the validator enforces that.
    std::string attribution;
    std::vector<CapabilityDescriptor> descriptors;
};

// --- Validation ------------------------------------------------------------

enum class DescriptorErrorKind {
    Ok,
    UnsupportedSchema,
    MissingField,
    DuplicateDescriptorId,
    ConflictingTarget,
    UnsafePath,
    PathTraversal,
    PathOutsideAllowlist,
    InvalidValueType,
    InvalidRange,
    EmptyAllowlist,
    PolicyValueOutOfRange,
    PolicyValueNotAllowed,
    UnsupportedReadBack,
    UnsupportedRollback,
    MissingDependency,
    CircularDependency,
    InvalidOrderGroup,
    ExecutableContent,
    MissingAttribution,
};

const char *descriptor_error_kind_name(DescriptorErrorKind kind);

/** One actionable validation failure. Safe to print: it quotes no device or file content. */
struct DescriptorError {
    std::string pack_id;
    std::string descriptor_id;
    std::string field;
    DescriptorErrorKind kind = DescriptorErrorKind::Ok;
    std::string explanation;
};

/**
 * @brief Validates packs deterministically, with no device and no filesystem access.
 *
 * Runs in host tests and CI, so a malformed pack fails the build rather than a user's phone.
 */
class DescriptorValidator {
public:
    explicit DescriptorValidator(PathPolicy policy = PathPolicy{}) : policy_(std::move(policy)) {}

    /// Validate one pack in isolation.
    [[nodiscard]] std::vector<DescriptorError> validate(const DevicePack &pack) const;

    /// Validate several packs, additionally checking for cross-pack conflicts.
    [[nodiscard]] std::vector<DescriptorError> validate_all(const std::vector<DevicePack> &packs) const;

    /**
     * @brief Reject strings that look like code rather than data.
     *
     * A defence in depth, not the primary boundary — the primary boundary is that the schema
     * has nowhere to *put* code: no field is ever executed, interpolated, or passed to a
     * shell. This exists to catch a descriptor that is trying to be something it is not
     * (a path with `$(...)`, a value with a `;`), which almost always means someone is
     * translating shell instead of expressing data.
     */
    [[nodiscard]] static bool looks_executable(const std::string &value);

private:
    PathPolicy policy_;

    void validate_descriptor(const DevicePack &pack, const CapabilityDescriptor &descriptor,
                             std::vector<DescriptorError> &out) const;
    void validate_value_against_constraints(const DevicePack &pack,
                                            const CapabilityDescriptor &descriptor,
                                            const std::string &field, const std::string &value,
                                            std::vector<DescriptorError> &out) const;
};

// --- Probing ---------------------------------------------------------------

/** What the probe is told about the device it is running on. */
struct DeviceIdentity {
    SocFamily soc = SocFamily::Unknown;
    std::string codename; ///< diagnostics only; never used to enable a capability
};

/**
 * @brief Decides, per descriptor, whether Flux may act — by looking, never by assuming.
 *
 * Performs reads and stat-like checks only. It never writes and never chmods: a probe that
 * mutated the device to find out whether it could mutate the device would be its own bug.
 */
class CapabilityProbe {
public:
    CapabilityProbe(SysfsNodeBackend &backend, PathPolicy policy = PathPolicy{})
        : backend_(backend), policy_(std::move(policy)) {}

    /// Probe one descriptor for @p intent. @p intent selects which desired value is checked
    /// against the declared constraints.
    [[nodiscard]] CapabilityStatus probe(const CapabilityDescriptor &descriptor,
                                         const DevicePack &pack, const DeviceIdentity &identity,
                                         const std::string &intent, int64_t now_ms) const;

    /**
     * @brief Probe a whole pack and resolve critical-group completeness.
     *
     * Two passes on purpose: a descriptor's own probe cannot know whether its group-mates
     * succeeded. The second pass demotes every member of a critical group to IncompleteGroup
     * when any critical member is not executable — so an aggressive tuning set is applied
     * whole or not at all, never half.
     */
    [[nodiscard]] CapabilityReport probe_pack(const DevicePack &pack, const DeviceIdentity &identity,
                                              const std::string &intent, int64_t now_ms) const;

    /// Resolve a single '*' wildcard segment against the backend. Returns the resolved path,
    /// or nullopt when nothing matched. The result is re-checked against the allowlist.
    [[nodiscard]] std::optional<std::string> resolve_path(const std::string &pattern) const;

private:
    SysfsNodeBackend &backend_;
    PathPolicy policy_;
};

} // namespace flux::execution
