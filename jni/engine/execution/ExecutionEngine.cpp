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

#include "ExecutionEngine.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace flux::execution {

const char *validation_error_name(ValidationError error) {
    switch (error) {
        case ValidationError::Ok: return "ok";
        case ValidationError::UnsafePath: return "unsafe_path";
        case ValidationError::UnsupportedCapability: return "unsupported_capability";
        case ValidationError::NotWritable: return "not_writable";
        case ValidationError::TypeMismatch: return "type_mismatch";
        case ValidationError::OutOfRange: return "out_of_range";
        case ValidationError::NotInAllowlist: return "not_in_allowlist";
    }
    return "unknown";
}

// --- ValueValidator --------------------------------------------------------

ValidationError ValueValidator::validate_path(const std::string &path) {
    if (path.empty() || path.front() != '/') return ValidationError::UnsafePath;
    if (path.find('\0') != std::string::npos) return ValidationError::UnsafePath;
    if (path.find("..") != std::string::npos) return ValidationError::UnsafePath;
    return ValidationError::Ok;
}

ValidationError ValueValidator::validate_value(const NodeDescriptor &d, const std::string &value) {
    switch (d.type) {
        case ValueType::Token:
            if (value.empty()) return ValidationError::TypeMismatch;
            if (!d.allowed.empty() &&
                std::find(d.allowed.begin(), d.allowed.end(), value) == d.allowed.end()) {
                return ValidationError::NotInAllowlist;
            }
            return ValidationError::Ok;

        case ValueType::Enum:
            if (std::find(d.allowed.begin(), d.allowed.end(), value) == d.allowed.end()) {
                return ValidationError::NotInAllowlist;
            }
            return ValidationError::Ok;

        case ValueType::IntRange: {
            if (value.empty()) return ValidationError::TypeMismatch;
            errno = 0;
            char *end = nullptr;
            const long v = std::strtol(value.c_str(), &end, 10);
            if (errno != 0 || end != value.c_str() + value.size()) return ValidationError::TypeMismatch;
            if (v < d.min || v > d.max) return ValidationError::OutOfRange;
            return ValidationError::Ok;
        }
    }
    return ValidationError::TypeMismatch;
}

// --- CapabilityRegistry ----------------------------------------------------

void CapabilityRegistry::register_node(NodeDescriptor descriptor, const NodeBackend &backend) {
    // A node is supported only if its path actually exists. A similarly-named path that is
    // absent is not silently treated as usable.
    const bool present = backend.exists(descriptor.path);
    const std::string id = descriptor.id;
    index_[id] = descriptors_.size();
    supported_[id] = present;
    descriptors_.push_back(std::move(descriptor));
}

const NodeDescriptor *CapabilityRegistry::find(const std::string &id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return &descriptors_[it->second];
}

bool CapabilityRegistry::supported(const std::string &id) const {
    auto it = supported_.find(id);
    return it != supported_.end() && it->second;
}

std::vector<std::string> CapabilityRegistry::unsupported_ids() const {
    std::vector<std::string> out;
    for (const auto &d : descriptors_) {
        if (!supported(d.id)) out.push_back(d.id);
    }
    return out;
}

// --- ExecutionPlanner ------------------------------------------------------

ExecutionPlan ExecutionPlanner::plan(const ProfilePlanSpec &spec, const CapabilityRegistry &registry,
                                     const NodeBackend &backend) const {
    ExecutionPlan out;
    std::unordered_map<std::string, std::string> chosen; // capability_id -> desired (conflict check)

    for (const auto &item : spec.items) {
        const NodeDescriptor *d = registry.find(item.capability_id);
        if (d == nullptr || !registry.supported(item.capability_id) || !d->writable) {
            // Unsupported (absent / not writable): skipped, not fatal, never a synthesised action.
            ++out.skipped_unsupported;
            continue;
        }

        if (ValueValidator::validate_path(d->path) != ValidationError::Ok) {
            out.valid = false;
            out.invalid_reason = "unsafe path for '" + d->id + "'";
            out.actions.clear();
            return out;
        }
        const ValidationError ve = ValueValidator::validate_value(*d, item.desired_value);
        if (ve != ValidationError::Ok) {
            out.valid = false;
            out.invalid_reason = std::string("invalid value for '") + d->id + "': " +
                                 validation_error_name(ve);
            out.actions.clear();
            return out;
        }

        auto seen = chosen.find(item.capability_id);
        if (seen != chosen.end()) {
            if (seen->second != item.desired_value) {
                out.valid = false;
                out.invalid_reason = "conflicting duplicate action for '" + d->id + "'";
                out.actions.clear();
                return out;
            }
            continue; // identical duplicate: dedupe
        }
        chosen[item.capability_id] = item.desired_value;

        ExecutionAction a;
        a.capability_id = d->id;
        a.path = d->path;
        a.desired_value = item.desired_value;
        a.previous_value = d->readable ? backend.read(d->path) : std::nullopt;
        a.order_group = d->order_group;
        a.critical = d->critical;
        a.reason = item.reason;
        out.actions.push_back(std::move(a));
    }

    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const ExecutionAction &l, const ExecutionAction &r) {
                         return l.order_group < r.order_group;
                     });
    out.valid = true;
    return out;
}

// --- ExecutionEngine -------------------------------------------------------

void ExecutionEngine::capture_original(const std::string &id, const std::string &path) {
    if (originals_.count(id)) return; // first-seen only; never overwrite the true original
    auto current = backend_.read(path);
    if (current) originals_[id] = Original{path, *current};
}

std::optional<std::string> ExecutionEngine::verified_value(const std::string &id) const {
    auto it = verified_.find(id);
    if (it == verified_.end()) return std::nullopt;
    return it->second;
}

ApplyResult ExecutionEngine::apply(const ExecutionPlan &plan, const std::string &requested_profile,
                                   const std::string &previous_profile, const std::string &reason,
                                   int64_t now_ms) {
    ApplyResult r;
    r.requested_profile = requested_profile;
    r.previous_profile = previous_profile;
    r.reason = reason;
    r.timestamp_ms = now_ms;
    r.skipped_unsupported = plan.skipped_unsupported;
    r.action_count = static_cast<int>(plan.actions.size());

    if (!plan.valid) {
        r.critical_failure = true;
        r.verified_active = false;
        r.message = "plan invalid: " + plan.invalid_reason;
        record_history(r);
        return r;
    }

    // Rollback scope is the critical group we successfully verified this apply.
    struct Undo {
        std::string id;
        std::string path;
        std::optional<std::string> previous;
    };
    std::vector<Undo> critical_undo;

    for (const auto &a : plan.actions) {
        // Idempotency: a value already verified in place is not rewritten.
        auto v = verified_.find(a.capability_id);
        if (v != verified_.end() && v->second == a.desired_value) {
            ++r.skipped_idempotent;
            continue;
        }

        capture_original(a.capability_id, a.path);

        const bool ok = backend_.write(a.path, a.desired_value);
        if (!ok) {
            if (a.critical) {
                r.critical_failure = true;
                r.degraded_capability = a.capability_id;
                r.message = "critical write failed: " + a.capability_id;
                break;
            }
            ++r.optional_failures;
            continue;
        }

        if (a.critical) {
            // Verify critical writes actually took effect (a write can succeed yet the node
            // clamp or reject the value).
            auto readback = backend_.read(a.path);
            if (!readback || *readback != a.desired_value) {
                critical_undo.push_back(Undo{a.capability_id, a.path, a.previous_value});
                r.critical_failure = true;
                r.degraded_capability = a.capability_id;
                r.message = "critical verify mismatch: " + a.capability_id;
                break;
            }
            critical_undo.push_back(Undo{a.capability_id, a.path, a.previous_value});
        }

        verified_[a.capability_id] = a.desired_value;
        ++r.succeeded;
    }

    if (r.critical_failure) {
        r.rollback_attempted = true;
        r.rollback_succeeded = true;
        // Roll back the critical group in reverse order.
        for (auto it = critical_undo.rbegin(); it != critical_undo.rend(); ++it) {
            if (!it->previous) {
                // No known previous value to restore: cannot safely undo this one.
                r.rollback_succeeded = false;
                r.degraded = true;
                r.degraded_capability = it->id;
                verified_.erase(it->id);
                continue;
            }
            if (backend_.write(it->path, *it->previous)) {
                verified_[it->id] = *it->previous;
            } else {
                r.rollback_succeeded = false;
                r.degraded = true;
                r.degraded_capability = it->id;
                verified_.erase(it->id);
            }
        }
        r.verified_active = false;
        if (r.message.empty()) r.message = "critical failure; rollback attempted";
    } else {
        r.verified_active = true;
        r.message = "applied " + std::to_string(r.succeeded) + " action(s), " +
                    std::to_string(r.skipped_idempotent) + " idempotent, " +
                    std::to_string(r.skipped_unsupported) + " unsupported";
    }

    record_history(r);
    return r;
}

ApplyResult ExecutionEngine::restore_originals(const std::string &reason, int64_t now_ms) {
    ApplyResult r;
    r.requested_profile = "restore";
    r.reason = reason;
    r.timestamp_ms = now_ms;
    r.action_count = static_cast<int>(originals_.size());
    r.verified_active = true;

    for (const auto &[id, original] : originals_) {
        if (backend_.write(original.path, original.value)) {
            verified_[id] = original.value;
            ++r.succeeded;
        } else {
            ++r.optional_failures;
            r.verified_active = false;
            r.degraded = true;
            r.degraded_capability = id;
        }
    }
    r.message = "restored " + std::to_string(r.succeeded) + " original value(s)";
    record_history(r);
    return r;
}

void ExecutionEngine::record_history(const ApplyResult &result) {
    ApplyHistoryEntry e;
    e.monotonic_ms = result.timestamp_ms;
    e.previous_profile = result.previous_profile;
    e.requested_profile = result.requested_profile;
    e.reason = result.reason;
    e.verified_active = result.verified_active;
    e.critical_failure = result.critical_failure;
    e.degraded = result.degraded;
    e.error_summary = result.critical_failure ? result.message : std::string();
    history_.push_back(std::move(e));
    while (history_.size() > history_capacity_) history_.pop_front();
}

} // namespace flux::execution
