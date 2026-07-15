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
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file ExecutionEngine.hpp
 * @brief Flux V2 Execution Engine — capability-aware, validated, verified, and
 *        rollback-aware application of a desired profile.
 *
 * Independent Flux implementation written from the Flux decision-output semantics,
 * documented Linux sysfs behaviour, detected device capabilities, and explicit
 * safety/rollback requirements. It is not a translation of the legacy applier.
 *
 * The engine performs writes through an injectable NodeBackend, so all policy and
 * transactional logic is host-testable with an in-memory backend — no root, no
 * sysfs, no device. It selects no profiles, parses no telemetry, and detects no
 * foreground apps: it only applies a plan someone else decided on.
 */
namespace flux::execution {

// --- Node backend ----------------------------------------------------------

/** The narrow filesystem surface the engine needs. Abstracted for host testing. */
class NodeBackend {
public:
    virtual ~NodeBackend() = default;
    virtual bool exists(const std::string &path) const = 0;
    virtual std::optional<std::string> read(const std::string &path) const = 0;
    virtual bool write(const std::string &path, const std::string &value) = 0;
};

/** In-memory backend: usable in tests and anywhere a real sysfs is not present. */
class InMemoryNodeBackend : public NodeBackend {
public:
    void seed(const std::string &path, std::string value) { store_[path] = std::move(value); }
    /// Force writes to a path to fail, to exercise failure/rollback paths.
    void fail_writes_to(const std::string &path) { failing_[path] = true; }
    void clear_failures() { failing_.clear(); }
    /// Model a node that silently ignores one specific value: the write returns success but
    /// the stored value does not change, so a read-back after that write does not match.
    void ignore_value(const std::string &path, std::string value) { ignored_[path] = std::move(value); }
    [[nodiscard]] int write_count(const std::string &path) const {
        auto it = writes_.find(path);
        return it == writes_.end() ? 0 : it->second;
    }

    bool exists(const std::string &path) const override { return store_.count(path) > 0; }
    std::optional<std::string> read(const std::string &path) const override {
        auto it = store_.find(path);
        if (it == store_.end()) return std::nullopt;
        return it->second;
    }
    bool write(const std::string &path, const std::string &value) override {
        if (failing_.count(path)) return false;
        ++writes_[path];
        auto ignore = ignored_.find(path);
        if (ignore != ignored_.end() && ignore->second == value) return true; // accepted, not stored
        store_[path] = value;
        return true;
    }

private:
    std::unordered_map<std::string, std::string> store_;
    std::unordered_map<std::string, int> writes_;
    std::unordered_map<std::string, bool> failing_;
    std::unordered_map<std::string, std::string> ignored_;
};

// --- Capability registry ---------------------------------------------------

enum class ValueType { Token, Enum, IntRange };

/** Describes one controllable device node. */
struct NodeDescriptor {
    std::string id;   ///< logical capability id, e.g. "cpu.policy0.scaling_governor"
    std::string path; ///< absolute sysfs path
    bool readable = false;
    bool writable = false;
    ValueType type = ValueType::Token;
    long min = 0;                       ///< IntRange lower bound (inclusive)
    long max = 0;                       ///< IntRange upper bound (inclusive)
    std::vector<std::string> allowed;   ///< Enum/Token allowlist (empty Token == any non-empty)
    bool critical = false;              ///< a failed apply here is a critical failure
    int order_group = 0;                ///< deterministic apply ordering
    std::string source = "generic";     ///< device-config provenance label
};

/** Registry of nodes actually present/usable on this device. */
class CapabilityRegistry {
public:
    /// Probe @p descriptor against @p backend; it becomes supported only if the path exists
    /// (and, when it must be written, is marked writable). A similarly-named path is not
    /// assumed valid just because it exists — writability is required for a write target.
    void register_node(NodeDescriptor descriptor, const NodeBackend &backend);

    [[nodiscard]] const NodeDescriptor *find(const std::string &id) const;
    [[nodiscard]] bool supported(const std::string &id) const;
    [[nodiscard]] std::vector<std::string> unsupported_ids() const;
    [[nodiscard]] size_t size() const { return descriptors_.size(); }

private:
    std::vector<NodeDescriptor> descriptors_;
    std::unordered_map<std::string, size_t> index_;
    std::unordered_map<std::string, bool> supported_;
};

// --- Validation ------------------------------------------------------------

enum class ValidationError {
    Ok,
    UnsafePath,
    UnsupportedCapability,
    NotWritable,
    TypeMismatch,
    OutOfRange,
    NotInAllowlist,
};

const char *validation_error_name(ValidationError error);

class ValueValidator {
public:
    /// Reject empty, relative, traversal ("..") and NUL-bearing paths.
    static ValidationError validate_path(const std::string &path);
    static ValidationError validate_value(const NodeDescriptor &descriptor, const std::string &value);
};

// --- Plan ------------------------------------------------------------------

struct ExecutionAction {
    std::string capability_id;
    std::string path;
    std::string desired_value;
    std::optional<std::string> previous_value;
    int order_group = 0;
    bool critical = false;
    std::string reason;
};

/** A request to move to a profile: the desired value per capability. */
struct ProfilePlanSpec {
    struct Item {
        std::string capability_id;
        std::string desired_value;
        std::string reason;
    };
    std::vector<Item> items;
};

/** An immutable, inspectable, fully-validated plan. */
struct ExecutionPlan {
    std::vector<ExecutionAction> actions;
    int skipped_unsupported = 0;
    bool valid = false;
    std::string invalid_reason;
};

class ExecutionPlanner {
public:
    /// Build a validated plan. Unsupported capabilities are skipped (counted, not fatal);
    /// unsafe paths, invalid values, and conflicting duplicate actions make the whole plan
    /// invalid so nothing is applied.
    [[nodiscard]] ExecutionPlan plan(const ProfilePlanSpec &spec, const CapabilityRegistry &registry,
                                     const NodeBackend &backend) const;
};

// --- Apply -----------------------------------------------------------------

struct ApplyResult {
    std::string requested_profile;
    std::string previous_profile;
    std::string reason;

    int action_count = 0;
    int succeeded = 0;
    int skipped_unsupported = 0;
    int skipped_idempotent = 0;
    int optional_failures = 0;
    bool critical_failure = false;

    bool rollback_attempted = false;
    bool rollback_succeeded = false;

    bool verified_active = false; ///< every critical action verified to hold its desired value
    bool degraded = false;        ///< rollback failed: runtime is in an uncertain state
    std::string degraded_capability;

    int64_t timestamp_ms = 0;
    std::string message;
};

/** One bounded history entry, the future backend for Flux Console Diagnostics. */
struct ApplyHistoryEntry {
    int64_t monotonic_ms = 0;
    uint64_t telemetry_sequence = 0;
    std::string previous_profile;
    std::string requested_profile;
    std::string reason;
    int priority = 0;
    std::string health;
    bool verified_active = false;
    bool critical_failure = false;
    bool degraded = false;
    std::string error_summary;
};

/**
 * @brief Applies plans transactionally, with verification, rollback and idempotency.
 */
class ExecutionEngine {
public:
    explicit ExecutionEngine(NodeBackend &backend, size_t history_capacity = 64)
        : backend_(backend), history_capacity_(history_capacity) {}

    /**
     * @brief Apply @p plan, moving from @p previous_profile to @p requested_profile.
     *
     * Sequence: validate -> capture previous (and first-seen originals) -> apply in order ->
     * verify critical writes -> on critical failure roll back the critical group -> publish a
     * complete ApplyResult. The active profile is the caller's to advance only when
     * verified_active is true and there was no critical failure.
     */
    ApplyResult apply(const ExecutionPlan &plan, const std::string &requested_profile,
                      const std::string &previous_profile, const std::string &reason,
                      int64_t now_ms);

    /** Restore every first-seen original value (session end, shutdown, etc.). */
    ApplyResult restore_originals(const std::string &reason, int64_t now_ms);

    /// Idempotency: forget verified values so the next apply rewrites (daemon restart,
    /// external mutation, capability invalidation, config change).
    void invalidate_all() { verified_.clear(); }
    void invalidate(const std::string &capability_id) { verified_.erase(capability_id); }

    [[nodiscard]] const std::deque<ApplyHistoryEntry> &history() const { return history_; }
    [[nodiscard]] std::optional<std::string> verified_value(const std::string &id) const;

private:
    NodeBackend &backend_;
    size_t history_capacity_;

    struct Original {
        std::string path;
        std::string value;
    };

    std::unordered_map<std::string, std::string> verified_; ///< last verified value per capability
    std::unordered_map<std::string, Original> originals_;    ///< first-seen value per capability

    std::deque<ApplyHistoryEntry> history_;

    void record_history(const ApplyResult &result);
    void capture_original(const std::string &id, const std::string &path);
};

} // namespace flux::execution
