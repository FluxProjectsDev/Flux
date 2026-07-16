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

// Host tests for the zen (Do-Not-Disturb) capability. A fake backend stands in for the
// framework control surface so the full-integer preservation, restoration, unavailability,
// external-change and apply-failure behaviours are all covered without a device.

#include "TestFramework.hpp"

#include "ZenController.hpp"

using namespace flux::execution;

namespace {

// A programmable stand-in for the device zen surface. It records the exact integer, so a test
// can prove the controller preserves the full mode rather than a boolean.
class FakeZenBackend : public ZenBackend {
public:
    bool available_ = true;
    std::optional<int> value_ = 0;
    bool fail_set_ = false;
    int set_calls_ = 0;

    bool available() const override { return available_; }
    std::optional<int> read() const override { return value_; }
    bool set(int mode) override {
        ++set_calls_;
        if (fail_set_) return false;
        value_ = mode;
        return true;
    }
};

} // namespace

// --- Full-integer preservation across all four modes -----------------------

TEST("zen: restore returns the exact prior mode for every original") {
    for (int original = 0; original <= 3; ++original) {
        FakeZenBackend backend;
        backend.value_ = original;
        ZenController zen(backend);

        CHECK(zen.engage(2));
        CHECK(zen.engaged());
        CHECK_EQ(backend.value_.value(), 2);
        CHECK_EQ(zen.original().value(), original);

        CHECK(zen.restore());
        CHECK(!zen.engaged());
        CHECK_MSG(backend.value_.value() == original, "restore must return the exact prior mode");
    }
}

TEST("zen: every target mode 0-3 is applied") {
    for (int target = 0; target <= 3; ++target) {
        FakeZenBackend backend;
        backend.value_ = 0;
        ZenController zen(backend);
        CHECK(zen.engage(target));
        CHECK_EQ(backend.value_.value(), target);
    }
}

// --- Unavailable capability is a no-op -------------------------------------

TEST("zen: an unavailable capability is never touched") {
    FakeZenBackend backend;
    backend.available_ = false;
    backend.value_ = 1;
    ZenController zen(backend);

    CHECK(!zen.engage(2));
    CHECK(!zen.engaged());
    CHECK_EQ(backend.set_calls_, 0);
    CHECK_MSG(backend.value_.value() == 1, "an unavailable capability must be left untouched");
}

TEST("zen: restore without engage is a no-op success") {
    FakeZenBackend backend;
    backend.value_ = 1;
    ZenController zen(backend);
    CHECK(zen.restore()); // nothing engaged: trivially satisfied
    CHECK_EQ(backend.set_calls_, 0);
}

// --- External modification is respected ------------------------------------

TEST("zen: restore yields to an external user change") {
    FakeZenBackend backend;
    backend.value_ = 0;
    ZenController zen(backend);

    CHECK(zen.engage(2));
    CHECK_EQ(backend.value_.value(), 2);

    // The user flips zen themselves while Flux holds it.
    backend.value_ = 3;
    const int before = backend.set_calls_;

    // Restore must detect the divergence and leave the user's choice intact.
    CHECK(!zen.restore());
    CHECK_MSG(backend.value_.value() == 3, "an external change must not be overwritten");
    CHECK_EQ(backend.set_calls_, before);
    CHECK(!zen.engaged());
}

// --- Apply failure is reported, not hidden ---------------------------------

TEST("zen: a failed first engage leaves nothing to restore") {
    FakeZenBackend backend;
    backend.value_ = 1;
    backend.fail_set_ = true;
    ZenController zen(backend);

    CHECK(!zen.engage(2));
    CHECK(!zen.engaged());
    CHECK_MSG(!zen.original().has_value(), "a failed first engage must not leave a stale original");
    CHECK_MSG(backend.value_.value() == 1, "a failed set must not change the mode");

    // A later restore is a clean no-op because nothing was engaged.
    CHECK(zen.restore());
}

TEST("zen: a failed restore write is reported") {
    FakeZenBackend backend;
    backend.value_ = 0;
    ZenController zen(backend);

    CHECK(zen.engage(2));
    backend.fail_set_ = true; // the restore write will fail
    CHECK(!zen.restore());
}

// --- Re-engage keeps the first-seen original -------------------------------

TEST("zen: re-engaging keeps the first-seen original") {
    FakeZenBackend backend;
    backend.value_ = 1;
    ZenController zen(backend);

    CHECK(zen.engage(2));
    CHECK_EQ(zen.original().value(), 1);

    // Re-engaging to a different mode must not re-capture the (now Flux-owned) live value.
    CHECK(zen.engage(3));
    CHECK_EQ(zen.original().value(), 1);
    CHECK_EQ(backend.value_.value(), 3);

    CHECK(zen.restore());
    CHECK_EQ(backend.value_.value(), 1);
}
