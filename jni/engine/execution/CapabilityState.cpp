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

#include "CapabilityState.hpp"

#include <algorithm>

namespace flux::execution {

const char *capability_state_name(CapabilityState state) {
    switch (state) {
        case CapabilityState::Supported: return "supported";
        case CapabilityState::Unsupported: return "unsupported";
        case CapabilityState::Unavailable: return "unavailable";
        case CapabilityState::PermissionDenied: return "permission_denied";
        case CapabilityState::InvalidFormat: return "invalid_format";
        case CapabilityState::IncompleteGroup: return "incomplete_group";
        case CapabilityState::VerificationFailed: return "verification_failed";
        case CapabilityState::DeviceValidationRequired: return "device_validation_required";
        case CapabilityState::ReadOnly: return "read_only";
        case CapabilityState::PathRejected: return "path_rejected";
        case CapabilityState::ExternalMutation: return "external_mutation";
    }
    return "unknown";
}

bool capability_is_executable(CapabilityState state) {
    // Exhaustive on purpose rather than `state == Supported`: adding a state forces a decision
    // here about whether it may act, instead of silently defaulting to "no" and being
    // discovered later, or worse, defaulting to "yes".
    switch (state) {
        case CapabilityState::Supported: return true;
        case CapabilityState::Unsupported:
        case CapabilityState::Unavailable:
        case CapabilityState::PermissionDenied:
        case CapabilityState::InvalidFormat:
        case CapabilityState::IncompleteGroup:
        case CapabilityState::VerificationFailed:
        case CapabilityState::DeviceValidationRequired:
        case CapabilityState::ReadOnly:
        case CapabilityState::PathRejected:
        case CapabilityState::ExternalMutation:
            return false;
    }
    return false;
}

const char *capability_reason_name(CapabilityReason reason) {
    switch (reason) {
        case CapabilityReason::Ok: return "ok";
        case CapabilityReason::NotProbed: return "not_probed";
        case CapabilityReason::SocMismatch: return "soc_mismatch";
        case CapabilityReason::NodeMissing: return "node_missing";
        case CapabilityReason::NodeNotRegularFile: return "node_not_regular_file";
        case CapabilityReason::NodeSymlink: return "node_symlink";
        case CapabilityReason::PathOutsideAllowlist: return "path_outside_allowlist";
        case CapabilityReason::PathTraversal: return "path_traversal";
        case CapabilityReason::NotWritable: return "not_writable";
        case CapabilityReason::PermissionAdjustUnavailable: return "permission_adjust_unavailable";
        case CapabilityReason::CurrentValueUnreadable: return "current_value_unreadable";
        case CapabilityReason::CurrentValueMalformed: return "current_value_malformed";
        case CapabilityReason::DesiredValueOutOfRange: return "desired_value_out_of_range";
        case CapabilityReason::DesiredValueNotAllowed: return "desired_value_not_allowed";
        case CapabilityReason::ReadBackUnsupported: return "read_back_unsupported";
        case CapabilityReason::ReadBackMismatch: return "read_back_mismatch";
        case CapabilityReason::RollbackValueUnavailable: return "rollback_value_unavailable";
        case CapabilityReason::CriticalDependencyMissing: return "critical_dependency_missing";
        case CapabilityReason::DescriptorInvalid: return "descriptor_invalid";
        case CapabilityReason::SchemaUnsupported: return "schema_unsupported";
        case CapabilityReason::AwaitingDeviceValidation: return "awaiting_device_validation";
    }
    return "unknown";
}

const char *validation_status_name(ValidationStatus status) {
    switch (status) {
        case ValidationStatus::DescriptorValidated: return "descriptor_validated";
        case ValidationStatus::HostValidated: return "host_validated";
        case ValidationStatus::NdkValidated: return "ndk_validated";
        case ValidationStatus::PhysicalDeviceRequired: return "physical_device_required";
        case ValidationStatus::PhysicalDeviceValidated: return "physical_device_validated";
    }
    return "unknown";
}

size_t CapabilityReport::supported_count() const {
    return static_cast<size_t>(std::count_if(statuses.begin(), statuses.end(), [](const auto &s) {
        return s.state == CapabilityState::Supported;
    }));
}

size_t CapabilityReport::executable_count() const {
    return static_cast<size_t>(std::count_if(statuses.begin(), statuses.end(),
                                             [](const auto &s) { return s.executable(); }));
}

std::vector<std::string> CapabilityReport::unsupported_ids() const {
    std::vector<std::string> out;
    for (const auto &s : statuses) {
        if (!s.executable()) out.push_back(s.capability_id);
    }
    return out;
}

std::vector<std::string> CapabilityReport::incomplete_groups() const {
    std::vector<std::string> out;
    for (const auto &s : statuses) {
        if (s.state != CapabilityState::IncompleteGroup) continue;
        if (std::find(out.begin(), out.end(), s.descriptor_set) == out.end()) {
            out.push_back(s.descriptor_set);
        }
    }
    return out;
}

bool CapabilityReport::all_critical_executable() const {
    // Vacuously true when nothing critical was declared: a pack of purely optional tuning has
    // no critical promise to break.
    return std::all_of(statuses.begin(), statuses.end(),
                       [](const auto &s) { return !s.critical || s.executable(); });
}

} // namespace flux::execution
