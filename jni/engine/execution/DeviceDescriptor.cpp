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

#include "DeviceDescriptor.hpp"

#include <algorithm>
#include <dirent.h>
#include <set>

namespace flux::execution {

const char *soc_family_name(SocFamily family) {
    switch (family) {
        case SocFamily::Unknown: return "unknown";
        case SocFamily::MediaTek: return "mediatek";
        case SocFamily::Snapdragon: return "snapdragon";
        case SocFamily::Exynos: return "exynos";
        case SocFamily::Unisoc: return "unisoc";
        case SocFamily::Tensor: return "tensor";
        case SocFamily::Tegra: return "tegra";
        case SocFamily::Generic: return "generic";
    }
    return "unknown";
}

SocFamily soc_family_from_code(int code) {
    switch (code) {
        case 1: return SocFamily::MediaTek;
        case 2: return SocFamily::Snapdragon;
        case 3: return SocFamily::Exynos;
        case 4: return SocFamily::Unisoc;
        case 5: return SocFamily::Tensor;
        case 6: return SocFamily::Tegra;
        default: return SocFamily::Unknown;
    }
}

const char *capability_group_name(CapabilityGroup group) {
    switch (group) {
        case CapabilityGroup::CpuPolicy: return "cpu_policy";
        case CapabilityGroup::GpuPolicy: return "gpu_policy";
        case CapabilityGroup::Scheduler: return "scheduler";
        case CapabilityGroup::Memory: return "memory";
        case CapabilityGroup::TouchInput: return "touch_input";
        case CapabilityGroup::Zen: return "zen";
        case CapabilityGroup::PowerMode: return "power_mode";
        case CapabilityGroup::DeviceSpecific: return "device_specific";
    }
    return "unknown";
}

const char *provenance_name(Provenance provenance) {
    switch (provenance) {
        case Provenance::FluxAuthored: return "flux_authored";
        case Provenance::DerivedEncore: return "derived_encore";
    }
    return "unknown";
}

const char *descriptor_error_kind_name(DescriptorErrorKind kind) {
    switch (kind) {
        case DescriptorErrorKind::Ok: return "ok";
        case DescriptorErrorKind::UnsupportedSchema: return "unsupported_schema";
        case DescriptorErrorKind::MissingField: return "missing_field";
        case DescriptorErrorKind::DuplicateDescriptorId: return "duplicate_descriptor_id";
        case DescriptorErrorKind::ConflictingTarget: return "conflicting_target";
        case DescriptorErrorKind::UnsafePath: return "unsafe_path";
        case DescriptorErrorKind::PathTraversal: return "path_traversal";
        case DescriptorErrorKind::PathOutsideAllowlist: return "path_outside_allowlist";
        case DescriptorErrorKind::InvalidValueType: return "invalid_value_type";
        case DescriptorErrorKind::InvalidRange: return "invalid_range";
        case DescriptorErrorKind::EmptyAllowlist: return "empty_allowlist";
        case DescriptorErrorKind::PolicyValueOutOfRange: return "policy_value_out_of_range";
        case DescriptorErrorKind::PolicyValueNotAllowed: return "policy_value_not_allowed";
        case DescriptorErrorKind::UnsupportedReadBack: return "unsupported_read_back";
        case DescriptorErrorKind::UnsupportedRollback: return "unsupported_rollback";
        case DescriptorErrorKind::MissingDependency: return "missing_dependency";
        case DescriptorErrorKind::CircularDependency: return "circular_dependency";
        case DescriptorErrorKind::InvalidOrderGroup: return "invalid_order_group";
        case DescriptorErrorKind::ExecutableContent: return "executable_content";
        case DescriptorErrorKind::MissingAttribution: return "missing_attribution";
    }
    return "unknown";
}

namespace {

bool parse_long(const std::string &text, long &out) {
    if (text.empty()) return false;
    size_t i = 0;
    if (text[0] == '-' || text[0] == '+') i = 1;
    if (i >= text.size()) return false;
    for (size_t j = i; j < text.size(); ++j) {
        if (text[j] < '0' || text[j] > '9') return false;
    }
    try {
        out = std::stol(text);
    } catch (...) {
        return false;
    }
    return true;
}

/// The path with its single '*' segment replaced by @p replacement.
std::string substitute_wildcard(const std::string &pattern, const std::string &replacement) {
    const size_t star = pattern.find('*');
    if (star == std::string::npos) return pattern;
    const size_t seg_begin = pattern.rfind('/', star) + 1;
    size_t seg_end = pattern.find('/', star);
    if (seg_end == std::string::npos) seg_end = pattern.size();
    return pattern.substr(0, seg_begin) + replacement + pattern.substr(seg_end);
}

bool segment_matches(const std::string &pattern_segment, const std::string &name) {
    // One '*' per segment: "*ddr-lat*" -> prefix "" and suffix "". Enough for the vendor
    // devfreq naming this exists for, and small enough to reason about. A full glob engine
    // would be a lot of surface for no gain.
    const size_t star = pattern_segment.find('*');
    if (star == std::string::npos) return pattern_segment == name;

    const std::string prefix = pattern_segment.substr(0, star);
    std::string rest = pattern_segment.substr(star + 1);
    const size_t second = rest.find('*');
    std::string middle, suffix;
    if (second == std::string::npos) {
        suffix = rest;
    } else {
        middle = rest.substr(0, second);
        suffix = rest.substr(second + 1);
    }

    if (name.size() < prefix.size() + suffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    if (!suffix.empty() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    if (!middle.empty()) {
        const std::string inner = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (inner.find(middle) == std::string::npos) return false;
    }
    return true;
}

} // namespace

// --- DescriptorValidator ---------------------------------------------------

bool DescriptorValidator::looks_executable(const std::string &value) {
    // Shell metacharacters in what is supposed to be a path or a literal value. Nothing here is
    // ever executed by Flux, so this is not a sandbox — it is a smell detector for a descriptor
    // that was transliterated from shell rather than expressed as data.
    static const char *markers[] = {"$(", "`", ";", "&&", "||", "|", ">", "<", "\n", "${",
                                    "eval ", "sh -c", "system(", "exec"};
    for (const char *marker : markers) {
        if (value.find(marker) != std::string::npos) return true;
    }
    return false;
}

void DescriptorValidator::validate_value_against_constraints(
    const DevicePack &pack, const CapabilityDescriptor &descriptor, const std::string &field,
    const std::string &value, std::vector<DescriptorError> &out) const {

    if (looks_executable(value)) {
        out.push_back({pack.pack_id, descriptor.descriptor_id, field,
                       DescriptorErrorKind::ExecutableContent,
                       "value contains shell metacharacters; descriptors carry data, not commands"});
        return;
    }

    switch (descriptor.value_type) {
        case NodeValueType::Integer: {
            long parsed = 0;
            if (!parse_long(value, parsed)) {
                out.push_back({pack.pack_id, descriptor.descriptor_id, field,
                               DescriptorErrorKind::PolicyValueOutOfRange,
                               "value '" + value + "' is not an integer"});
                return;
            }
            if (parsed < descriptor.min || parsed > descriptor.max) {
                out.push_back({pack.pack_id, descriptor.descriptor_id, field,
                               DescriptorErrorKind::PolicyValueOutOfRange,
                               "value '" + value + "' is outside the declared range"});
            }
            break;
        }
        case NodeValueType::Enum:
        case NodeValueType::Token: {
            if (descriptor.allowed.empty()) break; // Token with no allowlist accepts any word
            if (std::find(descriptor.allowed.begin(), descriptor.allowed.end(), value) ==
                descriptor.allowed.end()) {
                out.push_back({pack.pack_id, descriptor.descriptor_id, field,
                               DescriptorErrorKind::PolicyValueNotAllowed,
                               "value '" + value + "' is not in the declared allowlist"});
            }
            break;
        }
    }
}

void DescriptorValidator::validate_descriptor(const DevicePack &pack,
                                              const CapabilityDescriptor &descriptor,
                                              std::vector<DescriptorError> &out) const {
    const auto err = [&](const std::string &field, DescriptorErrorKind kind,
                         const std::string &why) {
        out.push_back({pack.pack_id, descriptor.descriptor_id, field, kind, why});
    };

    if (descriptor.schema_version < kDescriptorSchemaMin ||
        descriptor.schema_version > kDescriptorSchemaVersion) {
        err("schema_version", DescriptorErrorKind::UnsupportedSchema,
            "descriptor declares schema " + std::to_string(descriptor.schema_version) +
                "; this build implements " + std::to_string(kDescriptorSchemaMin) + ".." +
                std::to_string(kDescriptorSchemaVersion));
        return; // nothing below can be trusted under an unknown schema
    }

    if (descriptor.descriptor_id.empty()) {
        err("descriptor_id", DescriptorErrorKind::MissingField, "descriptor_id is required");
    }
    if (descriptor.capability_id.empty()) {
        err("capability_id", DescriptorErrorKind::MissingField, "capability_id is required");
    }
    if (descriptor.path.empty()) {
        err("path", DescriptorErrorKind::MissingField, "path is required");
        return;
    }

    if (looks_executable(descriptor.path)) {
        err("path", DescriptorErrorKind::ExecutableContent,
            "path contains shell metacharacters; a descriptor path is a literal, not an expression");
    }

    // Check the pattern with its wildcard neutralised: the allowlist decides the root, and a
    // '*' must not be able to smuggle a path past it. The resolved path is checked again at
    // probe time.
    const std::string probe_path = substitute_wildcard(descriptor.path, "x");
    switch (policy_.check(probe_path)) {
        case NodeError::Ok: break;
        case NodeError::PathNotAllowed:
            if (descriptor.path.find("/../") != std::string::npos ||
                descriptor.path.find("//") != std::string::npos) {
                err("path", DescriptorErrorKind::PathTraversal,
                    "path contains traversal or doubled separators");
            } else {
                err("path", DescriptorErrorKind::PathOutsideAllowlist,
                    "path is not under an approved kernel virtual root");
            }
            break;
        default:
            err("path", DescriptorErrorKind::UnsafePath, "path was refused by the path policy");
            break;
    }

    if (descriptor.value_type == NodeValueType::Integer && descriptor.min > descriptor.max) {
        err("min/max", DescriptorErrorKind::InvalidRange,
            "min " + std::to_string(descriptor.min) + " exceeds max " + std::to_string(descriptor.max));
    }
    if (descriptor.value_type == NodeValueType::Enum && descriptor.allowed.empty()) {
        err("allowed", DescriptorErrorKind::EmptyAllowlist,
            "an Enum descriptor must declare its allowed values");
    }
    if (descriptor.order_group < 0) {
        err("order_group", DescriptorErrorKind::InvalidOrderGroup, "order_group must not be negative");
    }

    // A node that cannot be read back cannot be verified, so a critical write to it could never
    // be confirmed — and an unconfirmable critical write is exactly what lets a profile be
    // reported active when it is not.
    if (descriptor.critical && descriptor.read_back == ReadBackStrategy::None) {
        err("read_back", DescriptorErrorKind::UnsupportedReadBack,
            "a critical descriptor must be verifiable; declare it optional or give it a read-back "
            "strategy");
    }
    // Likewise, promising to restore a node whose original value is never captured is a
    // rollback that would invent a value.
    if (descriptor.rollback == RollbackStrategy::RestoreOriginal &&
        descriptor.read_back == ReadBackStrategy::None) {
        err("rollback", DescriptorErrorKind::UnsupportedRollback,
            "RestoreOriginal needs a readable node to capture the original from");
    }

    for (const auto &[intent, value] : descriptor.policy_values) {
        validate_value_against_constraints(pack, descriptor, "policy_values[" + intent + "]", value,
                                           out);
    }
}

std::vector<DescriptorError> DescriptorValidator::validate(const DevicePack &pack) const {
    std::vector<DescriptorError> out;

    if (pack.schema_version < kDescriptorSchemaMin || pack.schema_version > kDescriptorSchemaVersion) {
        out.push_back({pack.pack_id, "", "schema_version", DescriptorErrorKind::UnsupportedSchema,
                       "pack declares schema " + std::to_string(pack.schema_version) +
                           "; this build implements " + std::to_string(kDescriptorSchemaMin) + ".." +
                           std::to_string(kDescriptorSchemaVersion)});
        return out; // refuse the pack whole rather than understanding half of it
    }
    if (pack.pack_id.empty()) {
        out.push_back({"", "", "pack_id", DescriptorErrorKind::MissingField, "pack_id is required"});
    }

    // Derived knowledge must carry its attribution with it. This is a licence obligation, so it
    // fails the build rather than a review.
    if (pack.provenance == Provenance::DerivedEncore && pack.attribution.empty()) {
        out.push_back({pack.pack_id, "", "attribution", DescriptorErrorKind::MissingAttribution,
                       "a pack derived from Encore must carry its attribution"});
    }

    std::set<std::string> ids;
    std::set<std::string> targets;
    for (const auto &descriptor : pack.descriptors) {
        if (!descriptor.descriptor_id.empty() && !ids.insert(descriptor.descriptor_id).second) {
            out.push_back({pack.pack_id, descriptor.descriptor_id, "descriptor_id",
                           DescriptorErrorKind::DuplicateDescriptorId,
                           "descriptor_id appears more than once in this pack"});
        }
        // Two descriptors writing the same node in the same pack cannot both be right, and
        // which one wins would depend on ordering.
        if (!descriptor.path.empty() && !targets.insert(descriptor.path).second) {
            out.push_back({pack.pack_id, descriptor.descriptor_id, "path",
                           DescriptorErrorKind::ConflictingTarget,
                           "two descriptors in this pack target the same node"});
        }
        validate_descriptor(pack, descriptor, out);
    }

    // Dependencies must exist, and must not form a cycle: a cycle has no valid apply order.
    for (const auto &descriptor : pack.descriptors) {
        for (const auto &dep : descriptor.depends_on) {
            if (ids.count(dep) == 0) {
                out.push_back({pack.pack_id, descriptor.descriptor_id, "depends_on",
                               DescriptorErrorKind::MissingDependency,
                               "depends on '" + dep + "', which this pack does not define"});
            }
        }
    }

    std::map<std::string, const CapabilityDescriptor *> by_id;
    for (const auto &descriptor : pack.descriptors) by_id[descriptor.descriptor_id] = &descriptor;

    // Iterative DFS: white/grey/black. A grey node reached again is a back edge, i.e. a cycle.
    std::map<std::string, int> colour; // 0 white, 1 grey, 2 black
    std::vector<std::string> reported;
    for (const auto &descriptor : pack.descriptors) {
        if (colour[descriptor.descriptor_id] != 0) continue;
        std::vector<std::pair<std::string, size_t>> stack{{descriptor.descriptor_id, 0}};
        colour[descriptor.descriptor_id] = 1;
        while (!stack.empty()) {
            auto &[id, index] = stack.back();
            const auto it = by_id.find(id);
            if (it == by_id.end() || index >= it->second->depends_on.size()) {
                colour[id] = 2;
                stack.pop_back();
                continue;
            }
            const std::string next = it->second->depends_on[index++];
            if (colour[next] == 1) {
                if (std::find(reported.begin(), reported.end(), next) == reported.end()) {
                    reported.push_back(next);
                    out.push_back({pack.pack_id, next, "depends_on",
                                   DescriptorErrorKind::CircularDependency,
                                   "dependency cycle: no order can satisfy this pack"});
                }
                continue;
            }
            if (colour[next] == 0 && by_id.count(next)) {
                colour[next] = 1;
                stack.push_back({next, 0});
            }
        }
    }

    return out;
}

std::vector<DescriptorError> DescriptorValidator::validate_all(
    const std::vector<DevicePack> &packs) const {
    std::vector<DescriptorError> out;
    std::set<std::string> pack_ids;

    for (const auto &pack : packs) {
        auto errors = validate(pack);
        out.insert(out.end(), errors.begin(), errors.end());
        if (!pack.pack_id.empty() && !pack_ids.insert(pack.pack_id).second) {
            out.push_back({pack.pack_id, "", "pack_id", DescriptorErrorKind::DuplicateDescriptorId,
                           "two packs share this pack_id"});
        }
    }

    // Cross-pack conflicts only matter between packs that can be active together. Two SoC
    // families are mutually exclusive on any real device, so the same node appearing in both
    // is expected and correct; a family colliding with Generic is not.
    std::map<std::string, std::string> generic_targets;
    for (const auto &pack : packs) {
        if (pack.soc != SocFamily::Generic) continue;
        for (const auto &d : pack.descriptors) generic_targets[d.path] = d.descriptor_id;
    }
    for (const auto &pack : packs) {
        if (pack.soc == SocFamily::Generic) continue;
        for (const auto &d : pack.descriptors) {
            const auto it = generic_targets.find(d.path);
            if (it != generic_targets.end()) {
                out.push_back({pack.pack_id, d.descriptor_id, "path",
                               DescriptorErrorKind::ConflictingTarget,
                               "target is also claimed by generic descriptor '" + it->second +
                                   "'; both would be active on this device"});
            }
        }
    }

    return out;
}

// --- CapabilityProbe -------------------------------------------------------

std::optional<std::string> CapabilityProbe::resolve_path(const std::string &pattern) const {
    if (pattern.find('*') == std::string::npos) return pattern;

    const size_t star = pattern.find('*');
    const size_t seg_begin = pattern.rfind('/', star) + 1;
    size_t seg_end = pattern.find('/', star);
    if (seg_end == std::string::npos) seg_end = pattern.size();

    const std::string dir = pattern.substr(0, seg_begin);
    const std::string segment = pattern.substr(seg_begin, seg_end - seg_begin);

    DIR *handle = ::opendir(dir.c_str());
    if (!handle) return std::nullopt;

    std::vector<std::string> matches;
    while (const dirent *entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (segment_matches(segment, name)) matches.push_back(name);
    }
    ::closedir(handle);

    if (matches.empty()) return std::nullopt;
    // Deterministic: the kernel's readdir order is not stable, and a plan that depends on it
    // would apply to a different node run to run.
    std::sort(matches.begin(), matches.end());

    const std::string resolved = substitute_wildcard(pattern, matches.front());
    // The resolved path is re-checked: a wildcard must never be able to widen the allowlist.
    if (policy_.check(resolved) != NodeError::Ok) return std::nullopt;
    return resolved;
}

CapabilityStatus CapabilityProbe::probe(const CapabilityDescriptor &descriptor,
                                        const DevicePack &pack, const DeviceIdentity &identity,
                                        const std::string &intent, int64_t now_ms) const {
    CapabilityStatus status;
    status.capability_id = descriptor.capability_id;
    status.descriptor_id = descriptor.descriptor_id;
    status.descriptor_set = pack.pack_id;
    status.critical = descriptor.critical;
    status.validation = descriptor.validation;
    status.probed_at_ms = now_ms;
    status.soc_matched = (descriptor.soc == SocFamily::Generic) || (descriptor.soc == identity.soc);

    const auto reject = [&](CapabilityState state, CapabilityReason reason, std::string detail) {
        status.state = state;
        status.reason = reason;
        status.detail = std::move(detail);
        return status;
    };

    // --- schema ---
    if (descriptor.schema_version < kDescriptorSchemaMin ||
        descriptor.schema_version > kDescriptorSchemaVersion) {
        return reject(CapabilityState::Unsupported, CapabilityReason::SchemaUnsupported,
                      "descriptor schema is not implemented by this build");
    }

    // --- SoC applicability ---
    // Necessary, never sufficient: matching the family only makes this a candidate.
    if (!status.soc_matched) {
        return reject(CapabilityState::Unsupported, CapabilityReason::SocMismatch,
                      std::string("descriptor targets ") + soc_family_name(descriptor.soc) +
                          "; this device is " + soc_family_name(identity.soc));
    }

    // --- path safety, before touching the filesystem ---
    const std::string lexical = substitute_wildcard(descriptor.path, "x");
    if (policy_.check(lexical) != NodeError::Ok) {
        return reject(CapabilityState::PathRejected, CapabilityReason::PathOutsideAllowlist,
                      "descriptor path is not under an approved kernel virtual root");
    }

    const auto resolved = resolve_path(descriptor.path);
    if (!resolved) {
        return reject(CapabilityState::Unsupported, CapabilityReason::NodeMissing,
                      "no node matches this descriptor on this device");
    }
    if (policy_.check(*resolved) != NodeError::Ok) {
        return reject(CapabilityState::PathRejected, CapabilityReason::PathOutsideAllowlist,
                      "resolved path escaped the approved roots");
    }

    // --- presence and type ---
    if (!backend_.exists(*resolved)) {
        // exists() is false for symlinks and non-regular files too; distinguish them so the
        // diagnostic points at the real problem.
        const NodeError probe_error = backend_.probe_writable(*resolved);
        if (probe_error == NodeError::SymlinkRejected) {
            return reject(CapabilityState::PathRejected, CapabilityReason::NodeSymlink,
                          "node is a symlink; Flux refuses to write through one");
        }
        if (probe_error == NodeError::NotRegularFile) {
            return reject(CapabilityState::Unsupported, CapabilityReason::NodeNotRegularFile,
                          "node is not a regular file");
        }
        return reject(CapabilityState::Unsupported, CapabilityReason::NodeMissing,
                      "node is not present on this device");
    }

    // --- readability and current value ---
    const auto current = backend_.read(*resolved);
    status.readable = current.has_value();
    if (!status.readable) {
        return reject(CapabilityState::Unavailable, CapabilityReason::CurrentValueUnreadable,
                      "node exists but its current value could not be read");
    }

    // If Flux cannot parse what is there now, it does not understand this node on this device
    // and must not write a value whose effect it cannot reason about.
    if (descriptor.value_type == NodeValueType::Integer) {
        long parsed = 0;
        if (!parse_long(*current, parsed)) {
            return reject(CapabilityState::InvalidFormat, CapabilityReason::CurrentValueMalformed,
                          "node holds a non-integer value where the descriptor declares an integer");
        }
    } else if (descriptor.value_type == NodeValueType::Enum && !descriptor.allowed.empty()) {
        if (std::find(descriptor.allowed.begin(), descriptor.allowed.end(), *current) ==
            descriptor.allowed.end()) {
            return reject(CapabilityState::InvalidFormat, CapabilityReason::CurrentValueMalformed,
                          "node holds a value outside the descriptor's declared vocabulary");
        }
    }
    status.current_value_valid = true;

    // --- the value this intent would write ---
    const auto wanted = descriptor.policy_values.find(intent);
    if (wanted == descriptor.policy_values.end()) {
        // Nothing to say about this intent. Not a failure: it simply produces no action.
        return reject(CapabilityState::Unsupported, CapabilityReason::Ok,
                      "descriptor declares no value for intent '" + intent + "'");
    }
    if (descriptor.value_type == NodeValueType::Integer) {
        long parsed = 0;
        if (!parse_long(wanted->second, parsed) || parsed < descriptor.min || parsed > descriptor.max) {
            return reject(CapabilityState::InvalidFormat, CapabilityReason::DesiredValueOutOfRange,
                          "the value for this intent is outside the descriptor's declared range");
        }
    } else if (!descriptor.allowed.empty()) {
        if (std::find(descriptor.allowed.begin(), descriptor.allowed.end(), wanted->second) ==
            descriptor.allowed.end()) {
            return reject(CapabilityState::InvalidFormat, CapabilityReason::DesiredValueNotAllowed,
                          "the value for this intent is not in the descriptor's allowlist");
        }
    }

    // --- writability ---
    const NodeError writable = backend_.probe_writable(*resolved);
    if (writable == NodeError::Ok) {
        status.writable = true;
    } else if (writable == NodeError::PermissionDenied) {
        // Present but not currently writable. The backend can grant S_IWUSR safely at apply
        // time; whether that succeeds is only knowable then (SELinux may still refuse).
        status.permission_adjustable = true;
    } else if (writable == NodeError::ReadOnlyFilesystem) {
        return reject(CapabilityState::ReadOnly, CapabilityReason::NotWritable,
                      "node is on a read-only filesystem; no permission change can help");
    } else {
        return reject(CapabilityState::Unavailable, CapabilityReason::NotWritable,
                      std::string("node is not writable: ") + node_error_name(writable));
    }

    status.verification_available = descriptor.read_back != ReadBackStrategy::None;
    status.rollback_available =
        descriptor.rollback == RollbackStrategy::RestoreOriginal && status.readable;

    if (descriptor.rollback == RollbackStrategy::RestoreOriginal && !status.rollback_available) {
        return reject(CapabilityState::Unavailable, CapabilityReason::RollbackValueUnavailable,
                      "descriptor promises RestoreOriginal but the original cannot be captured");
    }

    // --- device validation gate ---
    // Everything structural checks out. The remaining question is whether this descriptor's
    // claims are true of real hardware, and no amount of probing answers that: the node exists
    // and parses, but whether writing it does what the descriptor says has never been confirmed
    // on this SoC family. Ship inert until someone confirms it on the metal.
    if (descriptor.validation != ValidationStatus::PhysicalDeviceValidated) {
        return reject(CapabilityState::DeviceValidationRequired,
                      CapabilityReason::AwaitingDeviceValidation,
                      "probes cleanly, but this descriptor has not been validated on real " +
                          std::string(soc_family_name(descriptor.soc)) + " hardware");
    }

    status.state = CapabilityState::Supported;
    status.reason = CapabilityReason::Ok;
    status.detail = "probed and executable";
    return status;
}

CapabilityReport CapabilityProbe::probe_pack(const DevicePack &pack, const DeviceIdentity &identity,
                                             const std::string &intent, int64_t now_ms) const {
    CapabilityReport report;
    report.statuses.reserve(pack.descriptors.size());

    for (const auto &descriptor : pack.descriptors) {
        report.statuses.push_back(probe(descriptor, pack, identity, intent, now_ms));
    }

    // --- second pass: critical-group completeness ---
    // A descriptor cannot know from its own probe whether its group-mates made it. If any
    // critical member of a group is not executable, no member of that group may act: applying
    // half of an aggressive tuning set is usually worse than applying none of it, because the
    // device ends up in a combination nobody designed or tested.
    std::map<CapabilityGroup, bool> group_broken;
    std::map<std::string, size_t> index_by_id;
    for (size_t i = 0; i < pack.descriptors.size(); ++i) {
        index_by_id[pack.descriptors[i].descriptor_id] = i;
        if (pack.descriptors[i].critical && !report.statuses[i].executable()) {
            group_broken[pack.descriptors[i].group] = true;
        }
    }

    for (size_t i = 0; i < pack.descriptors.size(); ++i) {
        const auto &descriptor = pack.descriptors[i];
        auto &status = report.statuses[i];
        if (!status.executable()) continue;

        if (group_broken[descriptor.group]) {
            status.state = CapabilityState::IncompleteGroup;
            status.reason = CapabilityReason::CriticalDependencyMissing;
            status.detail = std::string("a critical descriptor in group '") +
                            capability_group_name(descriptor.group) +
                            "' is not executable, so the whole group is withheld";
            continue;
        }

        // An unmet dependency is the same problem at descriptor granularity.
        for (const auto &dep : descriptor.depends_on) {
            const auto it = index_by_id.find(dep);
            if (it == index_by_id.end() || !report.statuses[it->second].executable()) {
                status.state = CapabilityState::IncompleteGroup;
                status.reason = CapabilityReason::CriticalDependencyMissing;
                status.detail = "depends on '" + dep + "', which is not executable here";
                break;
            }
        }
    }

    return report;
}

} // namespace flux::execution
