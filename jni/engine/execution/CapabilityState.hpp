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
#include <string>
#include <vector>

/**
 * @file CapabilityState.hpp
 * @brief The one canonical answer to "may Flux act on this capability, and if not, why?"
 *
 * Flux-owned (Category A).
 *
 * ## Why this exists
 *
 * The legacy applier had exactly one notion of a capability: the node's path existed, or it
 * did not — and even that was discarded (`[ ! -f "$2" ] && return 1`, every error to
 * /dev/null). "Present" was treated as "usable", and "unusable" was treated as "done". That
 * is how a profile gets reported as active on a device where nothing was written.
 *
 * A capability can fail to be usable in ways that call for genuinely different responses:
 * a node that is absent means this device does not have the feature; a node that is present
 * but permission-denied means it has the feature and Flux may not use it; a node whose
 * current value does not parse means Flux does not understand this device and must not
 * write a value it cannot reason about. Collapsing those into a bool throws away the
 * information a user or a maintainer needs.
 *
 * ## The invariant
 *
 * Only Supported produces an executable action. Every other state produces **no chmod and no
 * write**, and prevents the owning profile from being reported fully active. There is no
 * state that means "probably fine, try it anyway".
 */
namespace flux::execution {

/**
 * @brief Whether a capability may be acted on, and if not, the class of reason.
 *
 * These are deliberately non-overlapping: each names a distinct condition with a distinct
 * response. Resist adding a state that is merely a shade of an existing one — an ambiguous
 * state is worse than a coarse one, because nobody can act on it.
 */
enum class CapabilityState {
    /// Probed successfully and executable. The only state that may produce an action.
    Supported,

    /// This device does not have the capability: the descriptor did not match the SoC, or the
    /// node is simply not present. Not an error — most devices lack most vendor capabilities.
    Unsupported,

    /// The capability should exist here but could not be established this probe, for a reason
    /// that is not one of the more specific states below.
    Unavailable,

    /// The node exists but cannot be written, and write permission could not be granted
    /// safely. Distinct from Unsupported: the device has the feature and Flux may not use it,
    /// which is usually SELinux and is worth telling the user about.
    PermissionDenied,

    /// The node exists and is readable, but its current value does not match the declared
    /// type/range. Flux does not understand this node on this device and must not write a
    /// value whose meaning it cannot reason about.
    InvalidFormat,

    /// A required descriptor in this capability's critical group is missing or invalid, so the
    /// whole group is refused. Prevents applying half of an aggressive tuning set — the state
    /// that is usually worse than applying none of it.
    IncompleteGroup,

    /// A write reported success but read-back did not return the desired value. The node
    /// silently ignored it (a clamp, a vendor no-op, a wrong unit).
    VerificationFailed,

    /// Structurally valid and probe-able, but never confirmed on real hardware of this family.
    /// Host and NDK validation cannot clear this: they prove the descriptor is well-formed,
    /// not that the node means what the descriptor claims. Ships inert until validated.
    DeviceValidationRequired,

    /// The node exists and is readable but is inherently not writable (a read-only filesystem
    /// or an attribute the kernel exposes for reading only). Distinct from PermissionDenied:
    /// no permission change could ever help.
    ReadOnly,

    /// The descriptor's path is outside the approved roots, or is otherwise unsafe
    /// (traversal, symlink). A descriptor bug or a hostile pack — never a device condition.
    PathRejected,

    /// The node's value changed underneath Flux since it was last verified, so the cached
    /// view is not trustworthy and must be re-established before acting.
    ExternalMutation,
};

const char *capability_state_name(CapabilityState state);

/** True only for Supported. Everything else must produce no action. */
[[nodiscard]] bool capability_is_executable(CapabilityState state);

/**
 * @brief A specific, machine-readable reason, one level finer than the state.
 *
 * The state says what to do; the reason says what was seen. Diagnostics need both: "unsupported"
 * alone leaves a user unable to tell a missing feature from a typo'd path.
 */
enum class CapabilityReason {
    Ok,
    NotProbed,
    SocMismatch,
    NodeMissing,
    NodeNotRegularFile,
    NodeSymlink,
    PathOutsideAllowlist,
    PathTraversal,
    NotWritable,
    PermissionAdjustUnavailable,
    CurrentValueUnreadable,
    CurrentValueMalformed,
    DesiredValueOutOfRange,
    DesiredValueNotAllowed,
    ReadBackUnsupported,
    ReadBackMismatch,
    RollbackValueUnavailable,
    CriticalDependencyMissing,
    DescriptorInvalid,
    SchemaUnsupported,
    AwaitingDeviceValidation,
};

const char *capability_reason_name(CapabilityReason reason);

/** How confident Flux is that a descriptor set is right for real hardware. */
enum class ValidationStatus {
    DescriptorValidated,    ///< structure and constraints check out
    HostValidated,          ///< exercised against fake device fixtures
    NdkValidated,           ///< compiles and links for the target ABIs
    PhysicalDeviceRequired, ///< never confirmed on this SoC family's real hardware
    PhysicalDeviceValidated ///< confirmed on real hardware of this family
};

const char *validation_status_name(ValidationStatus status);

/**
 * @brief The complete structured result of probing one capability.
 *
 * Everything a diagnostic surface, a dry run, or a maintainer needs, without re-probing and
 * without reading the device again. Carries no free-text blob: each field is separately
 * inspectable so the future Diagnostics Channel does not have to parse prose.
 */
struct CapabilityStatus {
    std::string capability_id;
    std::string descriptor_id;
    std::string descriptor_set; ///< which pack it came from, for provenance and blame

    CapabilityState state = CapabilityState::Unsupported;
    CapabilityReason reason = CapabilityReason::NotProbed;

    bool soc_matched = false;
    int64_t probed_at_ms = 0;

    bool readable = false;
    bool writable = false;
    bool permission_adjustable = false; ///< not writable now, but could be safely
    bool current_value_valid = false;
    bool verification_available = false; ///< read-back is possible for this node
    bool rollback_available = false;     ///< an original value could be captured

    bool critical = false;
    ValidationStatus validation = ValidationStatus::PhysicalDeviceRequired;

    /// A safe, human-readable explanation. Never contains file contents, secrets, or anything
    /// read from the device beyond the node's own declared value vocabulary.
    std::string detail;

    [[nodiscard]] bool executable() const { return capability_is_executable(state); }
};

/** The probe outcome for a whole set, with the group-level verdicts already resolved. */
struct CapabilityReport {
    std::vector<CapabilityStatus> statuses;

    [[nodiscard]] size_t supported_count() const;
    [[nodiscard]] size_t executable_count() const;
    [[nodiscard]] std::vector<std::string> unsupported_ids() const;
    [[nodiscard]] std::vector<std::string> incomplete_groups() const;

    /// True when every critical capability is executable. A profile must not be reported fully
    /// active when this is false, however many optional writes succeeded.
    [[nodiscard]] bool all_critical_executable() const;
};

} // namespace flux::execution
