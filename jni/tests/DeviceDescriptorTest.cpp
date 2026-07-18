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

// Descriptor schema, validator, and the Category A/C boundary.
//
// Everything here runs without a device: the whole point of expressing device knowledge as
// data is that it can be checked before it ever reaches hardware.

#include "TestFramework.hpp"

#include "DevicePacks.hpp"
#include "DeviceDescriptor.hpp"

#include <algorithm>
#include <string>

using namespace flux::execution;

namespace {

/// A minimal valid descriptor to mutate per-test, so each test shows exactly one defect.
CapabilityDescriptor good_descriptor() {
    CapabilityDescriptor d;
    d.schema_version = kDescriptorSchemaVersion;
    d.descriptor_id = "test.node";
    d.capability_id = "test.capability";
    d.group = CapabilityGroup::GpuPolicy;
    d.soc = SocFamily::Generic;
    d.path = "/sys/class/kgsl/kgsl-3d0/force_clk_on";
    d.value_type = NodeValueType::Integer;
    d.min = 0;
    d.max = 1;
    d.policy_values = {{"balanced", "0"}};
    d.read_back = ReadBackStrategy::Numeric;
    d.rollback = RollbackStrategy::RestoreOriginal;
    return d;
}

DevicePack pack_with(CapabilityDescriptor d) {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "test_pack";
    pack.soc = SocFamily::Generic;
    pack.descriptors.push_back(std::move(d));
    return pack;
}

bool has_error(const std::vector<DescriptorError> &errors, DescriptorErrorKind kind) {
    return std::any_of(errors.begin(), errors.end(),
                       [kind](const auto &e) { return e.kind == kind; });
}

std::string describe(const std::vector<DescriptorError> &errors) {
    std::string out;
    for (const auto &e : errors) {
        out += std::string(descriptor_error_kind_name(e.kind)) + "(" + e.field + ") ";
    }
    return out.empty() ? "<none>" : out;
}

} // namespace

TEST("descriptor: a well-formed descriptor validates cleanly") {
    const DescriptorValidator validator;
    const auto errors = validator.validate(pack_with(good_descriptor()));
    CHECK_MSG(errors.empty(), "expected no errors, got: " + describe(errors));
}

TEST("descriptor: an unsupported schema refuses the pack whole") {
    // A pack that declares a schema this build does not implement must not be half-understood:
    // a partially-interpreted tuning descriptor is a wrong write.
    const DescriptorValidator validator;
    auto pack = pack_with(good_descriptor());
    pack.schema_version = kDescriptorSchemaVersion + 99;

    const auto errors = validator.validate(pack);
    CHECK(has_error(errors, DescriptorErrorKind::UnsupportedSchema));
    CHECK_MSG(errors.size() == 1, "the pack must be refused whole, not field by field");
}

TEST("descriptor: a schema below the supported minimum is refused") {
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.schema_version = kDescriptorSchemaMin - 1;
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::UnsupportedSchema));
}

TEST("descriptor: required fields are required") {
    const DescriptorValidator validator;

    auto no_id = good_descriptor();
    no_id.descriptor_id.clear();
    CHECK(has_error(validator.validate(pack_with(no_id)), DescriptorErrorKind::MissingField));

    auto no_capability = good_descriptor();
    no_capability.capability_id.clear();
    CHECK(has_error(validator.validate(pack_with(no_capability)), DescriptorErrorKind::MissingField));

    auto no_path = good_descriptor();
    no_path.path.clear();
    CHECK(has_error(validator.validate(pack_with(no_path)), DescriptorErrorKind::MissingField));
}

TEST("descriptor: a duplicate descriptor id is rejected") {
    const DescriptorValidator validator;
    auto pack = pack_with(good_descriptor());
    auto twin = good_descriptor();
    twin.path = "/sys/class/kgsl/kgsl-3d0/bus_split"; // different node, same id
    pack.descriptors.push_back(twin);

    CHECK(has_error(validator.validate(pack), DescriptorErrorKind::DuplicateDescriptorId));
}

TEST("descriptor: two descriptors targeting the same node conflict") {
    // Both cannot be right, and which one wins would depend on apply order.
    const DescriptorValidator validator;
    auto pack = pack_with(good_descriptor());
    auto twin = good_descriptor();
    twin.descriptor_id = "test.node.other";
    pack.descriptors.push_back(twin);

    CHECK(has_error(validator.validate(pack), DescriptorErrorKind::ConflictingTarget));
}

TEST("descriptor: a path outside the approved roots is rejected") {
    const DescriptorValidator validator;
    for (const char *path : {"/data/local/tmp/x", "/system/build.prop", "/etc/passwd",
                             "/vendor/etc/init.rc"}) {
        auto d = good_descriptor();
        d.path = path;
        const auto errors = validator.validate(pack_with(d));
        CHECK_MSG(has_error(errors, DescriptorErrorKind::PathOutsideAllowlist),
                  std::string("path '") + path + "' must be rejected, got: " + describe(errors));
    }
}

TEST("descriptor: a traversing path is rejected") {
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.path = "/sys/../data/local/tmp/x";
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::PathTraversal));
}

TEST("descriptor: a wildcard cannot widen the allowlist") {
    // The wildcard is resolved at probe time; the validator must still refuse a pattern whose
    // root is not approved, rather than letting the '*' hide it.
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.path = "/data/*/secrets";
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::PathOutsideAllowlist));
}

TEST("descriptor: executable content is rejected wherever it appears") {
    // Descriptors are data. Nothing here is ever executed, so this is a smell detector for a
    // descriptor transliterated from shell rather than expressed as data.
    const DescriptorValidator validator;

    auto in_path = good_descriptor();
    in_path.path = "/sys/class/kgsl/$(whoami)/force_clk_on";
    CHECK(has_error(validator.validate(pack_with(in_path)), DescriptorErrorKind::ExecutableContent));

    auto in_value = good_descriptor();
    in_value.value_type = NodeValueType::Token;
    in_value.policy_values = {{"balanced", "1; rm -rf /"}};
    CHECK(has_error(validator.validate(pack_with(in_value)),
                    DescriptorErrorKind::ExecutableContent));

    auto backtick = good_descriptor();
    backtick.value_type = NodeValueType::Token;
    backtick.policy_values = {{"balanced", "`id`"}};
    CHECK(has_error(validator.validate(pack_with(backtick)),
                    DescriptorErrorKind::ExecutableContent));

    CHECK(DescriptorValidator::looks_executable("$(x)"));
    CHECK(DescriptorValidator::looks_executable("a | b"));
    CHECK(DescriptorValidator::looks_executable("echo > /tmp/x"));
    CHECK(!DescriptorValidator::looks_executable("performance"));
    CHECK(!DescriptorValidator::looks_executable("0 0"));
}

TEST("descriptor: an inverted integer range is rejected") {
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.min = 10;
    d.max = 1;
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::InvalidRange));
}

TEST("descriptor: an Enum with no allowlist is rejected") {
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.value_type = NodeValueType::Enum;
    d.allowed.clear();
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::EmptyAllowlist));
}

TEST("descriptor: a policy value outside its own declared constraints is rejected") {
    const DescriptorValidator validator;

    auto out_of_range = good_descriptor();
    out_of_range.policy_values = {{"balanced", "99"}}; // declared max is 1
    CHECK(has_error(validator.validate(pack_with(out_of_range)),
                    DescriptorErrorKind::PolicyValueOutOfRange));

    auto not_allowed = good_descriptor();
    not_allowed.value_type = NodeValueType::Enum;
    not_allowed.allowed = {"performance", "powersave"};
    not_allowed.policy_values = {{"balanced", "turbo"}};
    CHECK(has_error(validator.validate(pack_with(not_allowed)),
                    DescriptorErrorKind::PolicyValueNotAllowed));

    auto not_an_int = good_descriptor();
    not_an_int.policy_values = {{"balanced", "fast"}};
    CHECK(has_error(validator.validate(pack_with(not_an_int)),
                    DescriptorErrorKind::PolicyValueOutOfRange));
}

TEST("descriptor: a critical node that cannot be read back is rejected") {
    // An unverifiable critical write can never be confirmed, which is exactly what lets a
    // profile be reported active when it is not.
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.critical = true;
    d.read_back = ReadBackStrategy::None;
    d.rollback = RollbackStrategy::None;
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::UnsupportedReadBack));
}

TEST("descriptor: promising RestoreOriginal on an unreadable node is rejected") {
    // Rollback would have to invent the original value.
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.read_back = ReadBackStrategy::None;
    d.rollback = RollbackStrategy::RestoreOriginal;
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::UnsupportedRollback));
}

TEST("descriptor: a dependency on something the pack does not define is rejected") {
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.depends_on = {"nonexistent.node"};
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::MissingDependency));
}

TEST("descriptor: a dependency cycle is rejected") {
    // No apply order can satisfy a cycle.
    const DescriptorValidator validator;
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "cyclic";

    auto a = good_descriptor();
    a.descriptor_id = "a";
    a.path = "/sys/class/kgsl/kgsl-3d0/a";
    a.depends_on = {"b"};

    auto b = good_descriptor();
    b.descriptor_id = "b";
    b.path = "/sys/class/kgsl/kgsl-3d0/b";
    b.depends_on = {"a"};

    pack.descriptors = {a, b};
    CHECK(has_error(validator.validate(pack), DescriptorErrorKind::CircularDependency));
}

TEST("descriptor: a negative order group is rejected") {
    const DescriptorValidator validator;
    auto d = good_descriptor();
    d.order_group = -1;
    CHECK(has_error(validator.validate(pack_with(d)), DescriptorErrorKind::InvalidOrderGroup));
}

TEST("descriptor: derived knowledge must carry its attribution") {
    // A licence obligation, so it fails the build rather than a review.
    const DescriptorValidator validator;
    auto pack = pack_with(good_descriptor());
    pack.provenance = Provenance::DerivedEncore;
    pack.attribution.clear();
    CHECK(has_error(validator.validate(pack), DescriptorErrorKind::MissingAttribution));

    pack.attribution = "derived from Encore Tweaks, Apache-2.0, (C) Rem01Gaming";
    CHECK(!has_error(validator.validate(pack), DescriptorErrorKind::MissingAttribution));
}

TEST("descriptor: a family pack colliding with the generic pack is rejected") {
    // Two SoC families never coexist, but a family and generic do.
    const DescriptorValidator validator;

    DevicePack generic;
    generic.schema_version = kDescriptorSchemaVersion;
    generic.pack_id = "generic";
    generic.soc = SocFamily::Generic;
    auto g = good_descriptor();
    g.descriptor_id = "generic.node";
    generic.descriptors.push_back(g);

    DevicePack family;
    family.schema_version = kDescriptorSchemaVersion;
    family.pack_id = "family";
    family.soc = SocFamily::Snapdragon;
    auto f = good_descriptor();
    f.descriptor_id = "family.node";
    f.soc = SocFamily::Snapdragon;
    f.path = g.path; // same node as generic
    family.descriptors.push_back(f);

    CHECK(has_error(validator.validate_all({generic, family}), DescriptorErrorKind::ConflictingTarget));
}

TEST("descriptor: two different SoC families may claim the same node") {
    // Exynos and Tensor both use devfreq_mif. They never coexist on a device, so this is not a
    // conflict and must not be reported as one.
    const DescriptorValidator validator;
    const auto errors = validator.validate_all({flux::device::exynos_pack(),
                                                flux::device::tensor_pack()});
    CHECK_MSG(!has_error(errors, DescriptorErrorKind::ConflictingTarget),
              "distinct SoC families sharing a node path must not conflict: " + describe(errors));
}

// --- the shipped packs -----------------------------------------------------

TEST("packs: every shipped device pack validates") {
    const DescriptorValidator validator;
    const auto errors = validator.validate_all(flux::device::all_packs());
    CHECK_MSG(errors.empty(), "shipped packs must validate, got: " + describe(errors));
}

TEST("packs: all six SoC families plus generic are present") {
    const auto packs = flux::device::all_packs();
    CHECK_EQ(packs.size(), static_cast<size_t>(7));

    for (SocFamily soc : {SocFamily::MediaTek, SocFamily::Snapdragon, SocFamily::Exynos,
                          SocFamily::Unisoc, SocFamily::Tensor, SocFamily::Tegra,
                          SocFamily::Generic}) {
        const bool found = std::any_of(packs.begin(), packs.end(),
                                       [soc](const auto &p) { return p.soc == soc; });
        CHECK_MSG(found, std::string("no pack for ") + soc_family_name(soc));
    }
}

TEST("packs: every derived descriptor ships awaiting physical-device validation") {
    // The gate that keeps unverified vendor tuning inert. If this ever fails, something has
    // been promoted without hardware evidence.
    for (const auto &pack : flux::device::all_packs()) {
        for (const auto &d : pack.descriptors) {
            if (d.provenance != Provenance::DerivedEncore) continue;
            CHECK_MSG(d.validation == ValidationStatus::PhysicalDeviceRequired,
                      pack.pack_id + "/" + d.descriptor_id +
                          " is derived but claims validation status '" +
                          validation_status_name(d.validation) +
                          "'; only real hardware may promote it");
        }
    }
}

TEST("packs: every Encore-derived pack carries attribution") {
    for (const auto &pack : flux::device::all_packs()) {
        if (pack.provenance != Provenance::DerivedEncore) continue;
        CHECK_MSG(!pack.attribution.empty(), pack.pack_id + " is derived but carries no attribution");
        CHECK_MSG(pack.attribution.find("Rem01Gaming") != std::string::npos,
                  pack.pack_id + " attribution must name the upstream copyright holder");
        CHECK_MSG(pack.attribution.find("Apache-2.0") != std::string::npos,
                  pack.pack_id + " attribution must name the licence");
    }
}

TEST("packs: the generic pack claims no derived provenance") {
    // Standard cpufreq/scheduler interfaces are documented upstream Linux, not Encore's
    // contribution. Attributing them to Encore would be as wrong as not attributing what is.
    const auto generic = flux::device::generic_pack();
    CHECK_EQ(generic.provenance, Provenance::FluxAuthored);
    for (const auto &d : generic.descriptors) {
        CHECK_MSG(d.provenance == Provenance::FluxAuthored,
                  "generic descriptor " + d.descriptor_id + " must be Flux-authored");
    }
}

TEST("packs: no descriptor anywhere carries executable content") {
    for (const auto &pack : flux::device::all_packs()) {
        for (const auto &d : pack.descriptors) {
            CHECK_MSG(!DescriptorValidator::looks_executable(d.path),
                      pack.pack_id + "/" + d.descriptor_id + ": path looks executable");
            for (const auto &[intent, value] : d.policy_values) {
                CHECK_MSG(!DescriptorValidator::looks_executable(value),
                          pack.pack_id + "/" + d.descriptor_id + " value for '" + intent +
                              "' looks executable");
            }
            for (const auto &allowed : d.allowed) {
                CHECK_MSG(!DescriptorValidator::looks_executable(allowed),
                          pack.pack_id + "/" + d.descriptor_id + ": allowlist entry looks executable");
            }
        }
    }
}

TEST("packs: an unknown SoC gets the generic pack only, never a guess") {
    const auto packs = flux::device::packs_for(SocFamily::Unknown);
    CHECK_EQ(packs.size(), static_cast<size_t>(1));
    CHECK_EQ(packs.front().soc, SocFamily::Generic);
}

TEST("packs: a known SoC gets generic plus exactly its own family") {
    const auto packs = flux::device::packs_for(SocFamily::Snapdragon);
    CHECK_EQ(packs.size(), static_cast<size_t>(2));
    CHECK(std::any_of(packs.begin(), packs.end(),
                      [](const auto &p) { return p.soc == SocFamily::Generic; }));
    CHECK(std::any_of(packs.begin(), packs.end(),
                      [](const auto &p) { return p.soc == SocFamily::Snapdragon; }));
    // No other family may come along for the ride.
    CHECK(std::none_of(packs.begin(), packs.end(), [](const auto &p) {
        return p.soc == SocFamily::MediaTek || p.soc == SocFamily::Exynos;
    }));
}

TEST("soc: the numeric codes match the existing install-time recognition") {
    // The shell's soc_recognition writes these codes; the two must not drift apart during the
    // migration.
    CHECK_EQ(soc_family_from_code(1), SocFamily::MediaTek);
    CHECK_EQ(soc_family_from_code(2), SocFamily::Snapdragon);
    CHECK_EQ(soc_family_from_code(3), SocFamily::Exynos);
    CHECK_EQ(soc_family_from_code(4), SocFamily::Unisoc);
    CHECK_EQ(soc_family_from_code(5), SocFamily::Tensor);
    CHECK_EQ(soc_family_from_code(6), SocFamily::Tegra);
    CHECK_EQ(soc_family_from_code(0), SocFamily::Unknown);
    CHECK_EQ(soc_family_from_code(99), SocFamily::Unknown);
}

TEST("capability state: only Supported may act") {
    // The invariant the whole model rests on.
    CHECK(capability_is_executable(CapabilityState::Supported));
    for (CapabilityState state :
         {CapabilityState::Unsupported, CapabilityState::Unavailable,
          CapabilityState::PermissionDenied, CapabilityState::InvalidFormat,
          CapabilityState::IncompleteGroup, CapabilityState::VerificationFailed,
          CapabilityState::DeviceValidationRequired, CapabilityState::ReadOnly,
          CapabilityState::PathRejected, CapabilityState::ExternalMutation}) {
        CHECK_MSG(!capability_is_executable(state),
                  std::string("state '") + capability_state_name(state) + "' must not be executable");
    }
}

TEST("capability report: a failed critical capability sinks the whole report") {
    CapabilityReport report;

    CapabilityStatus optional_failed;
    optional_failed.capability_id = "opt";
    optional_failed.critical = false;
    optional_failed.state = CapabilityState::Unsupported;

    CapabilityStatus critical_ok;
    critical_ok.capability_id = "crit";
    critical_ok.critical = true;
    critical_ok.state = CapabilityState::Supported;

    report.statuses = {optional_failed, critical_ok};
    CHECK_MSG(report.all_critical_executable(),
              "an optional failure must not sink the report");

    report.statuses[1].state = CapabilityState::PermissionDenied;
    CHECK_MSG(!report.all_critical_executable(),
              "a critical failure must sink the report");
}
