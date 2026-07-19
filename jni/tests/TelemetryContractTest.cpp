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

// The telemetry wire contract, pinned against the SAME fixture corpus the self-test reads.
//
// Why this file exists. The Action self-test is a shell script; it cannot link the decoder below,
// so it necessarily re-implements the tokenizer. That duplication caused a real field failure: the
// self-test parsed the snapshot as `key=value` while every production consumer parses
// `key<SPACE>value`, so a perfectly healthy device reported
// "Telemetry snapshot malformed (no schema_version)". The shell fixtures had been written in the
// same wrong format, so they agreed with the bug instead of catching it.
//
// The fix is not "be more careful". It is to make ONE corpus the source of truth and run both
// parsers over it:
//
//   .github/fixtures/telemetry/*.snapshot   the corpus, in the real wire format
//   this file                               the production decoder's verdict per fixture
//   verify-installer.sh section 6           the self-test's verdict per fixture
//
// If someone changes the wire format, the tokenizer, or the schema bounds, one of the two goes
// red. A fixture that only the shell reads can never again drift from what the runtime accepts.

#include "TestFramework.hpp"

#include "TelemetryDecoder.hpp"
#include "TelemetrySnapshot.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace flux::telemetry;

namespace {

// run_tests.sh exports this; the binary's working directory is whatever the caller happened to be
// in, so a relative path here would work only when invoked from one particular place.
std::string corpus_dir() {
    const char *env = std::getenv("FLUX_TELEMETRY_CORPUS");
    return env != nullptr ? std::string(env) : std::string(".github/fixtures/telemetry/");
}

std::string read_fixture(const std::string &name) {
    std::ifstream in(corpus_dir() + name, std::ios::binary);
    // A corpus that cannot be opened must not look like a legitimately empty fixture, or
    // contract_empty_input_is_rejected would pass for entirely the wrong reason.
    if (!in) return std::string("\0CORPUS-UNREADABLE", 18);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// decode a fixture and report the error enum the production decoder produces.
DecodeError verdict(const std::string &name) {
    TelemetryDecoder decoder;
    return decoder.decode(read_fixture(name), 1000).error;
}

} // namespace

// The happy path. This is the exact text a healthy device carries, and the case that the shipped
// self-test used to call malformed.
TEST("contract: the valid schema-v2 corpus fixture decodes") {
    TelemetryDecoder decoder;
    auto r = decoder.decode(read_fixture("valid-v2.snapshot"), 1000);
    CHECK(r.ok());
    CHECK_EQ(r.snapshot.schema_version, 2);
    CHECK_EQ(r.snapshot.updated_elapsed_ms, 918273);
}

// schema_version is a TOP-LEVEL field and its position in the file is not significant. The
// self-test must find it wherever it sits, so the corpus carries a variant with it emitted last.
TEST("contract: schema_version position in the file is not significant") {
    TelemetryDecoder decoder;
    auto a = decoder.decode(read_fixture("valid-v2.snapshot"), 1000);
    auto b = decoder.decode(read_fixture("valid-v2-schema-last.snapshot"), 1000);
    CHECK(a.ok());
    CHECK(b.ok());
    CHECK_EQ(a.snapshot.schema_version, b.snapshot.schema_version);
    CHECK_EQ(a.snapshot.updated_elapsed_ms, b.snapshot.updated_elapsed_ms);
}

// A trailing CR per line is stripped, so a CRLF producer is accepted rather than being reported
// as a malformed schema value of "2\r".
TEST("contract: a trailing CR per line is stripped") {
    TelemetryDecoder decoder;
    auto r = decoder.decode(read_fixture("crlf.snapshot"), 1000);
    // Incomplete on purpose (required fields absent), but it must fail on a MISSING FIELD rather
    // than on the schema: the schema line parsed cleanly despite the CR.
    CHECK(r.error == DecodeError::MissingRequiredField);
}

// THE REGRESSION. `key=value` is not the wire format. The decoder sees a single token per line,
// finds no schema_version key, and rejects it — which is precisely what the shell self-test must
// also do, and precisely what the old fixtures asserted the opposite of.
TEST("contract: key=value is NOT the wire format (the field regression)") {
    CHECK(verdict("keyvalue-not-contract.snapshot") == DecodeError::MissingSchemaVersion);
}

TEST("contract: a missing schema_version is rejected") {
    CHECK(verdict("missing-schema.snapshot") == DecodeError::MissingSchemaVersion);
}

TEST("contract: a non-numeric schema_version is rejected") {
    CHECK(verdict("malformed-schema.snapshot") == DecodeError::MalformedInteger);
}

// Both directions of the accepted band are closed. v1 is a legacy producer and v3 a future one;
// neither is silently upgraded or downgraded into v2.
TEST("contract: legacy v1 and future v3 are both rejected") {
    CHECK(verdict("legacy-schema.snapshot") == DecodeError::UnsupportedSchema);
    CHECK(verdict("unsupported-schema.snapshot") == DecodeError::UnsupportedSchema);
    CHECK_EQ(kSchemaMin, 2);
    CHECK_EQ(kSchemaMax, 2);
}

TEST("contract: empty input is rejected") {
    CHECK(verdict("empty.snapshot") == DecodeError::EmptyInput);
}

// A duplicate key is rejected outright rather than resolved by first- or last-wins. The shell
// self-test has to agree, or it would call a snapshot healthy that the runtime refuses.
TEST("contract: a duplicate key is rejected outright") {
    CHECK(verdict("duplicate-schema.snapshot") == DecodeError::DuplicateKey);
}

// The decoder never returns a partially-filled snapshot: on any failure the snapshot is reset, so
// a caller cannot read a field that happened to parse before the error.
TEST("contract: a failed decode leaks no partial state") {
    TelemetryDecoder decoder;
    auto r = decoder.decode(read_fixture("unsupported-schema.snapshot"), 1000);
    CHECK(!r.ok());
    CHECK_EQ(r.snapshot.schema_version, 0);
    CHECK_EQ(r.snapshot.updated_elapsed_ms, 0);
}
