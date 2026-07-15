#!/usr/bin/env bash
#
# Copyright (C) 2024-2026 FebriCahyaa
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Build and run the Flux host test suite.
#
# These tests compile the *production* parser, policy and inotify watcher sources with a host
# compiler and exercise them against the real Linux kernel. inotify is a kernel facility, so
# the atomic-rename path SynthesisCore uses can be tested here for real rather than mocked.
#
# The Android daemon itself still needs the NDK; this suite deliberately covers only the parts
# that carry the safety logic, which is also the part that had the bugs.
#
# Usage:
#   jni/tests/run_tests.sh            # build and run
#   CXX=clang++ jni/tests/run_tests.sh
#   SANITIZE=1 jni/tests/run_tests.sh # with ASan + UBSan

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JNI_DIR="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${SCRIPT_DIR}/build"

CXX="${CXX:-g++}"
SANITIZE="${SANITIZE:-0}"

CXXFLAGS=(
    -std=c++17
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -g
    -O1
    # The stub logger must win over jni/include/FluxLog.hpp, so it comes first.
    -I"${SCRIPT_DIR}/stubs"
    -I"${SCRIPT_DIR}"
    -I"${JNI_DIR}/include"
    -I"${JNI_DIR}/base/SynthesisCore"
    -I"${JNI_DIR}/base/InotifyWatcher"
    -I"${JNI_DIR}/base/ProfilePolicy"
    -I"${JNI_DIR}/base/LockFile"
    -I"${JNI_DIR}/engine/decision"
)

LDFLAGS=(-pthread)

if [ "${SANITIZE}" = "1" ]; then
    CXXFLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    LDFLAGS+=(-fsanitize=address,undefined)
fi

SOURCES=(
    "${SCRIPT_DIR}/TestMain.cpp"
    "${SCRIPT_DIR}/TelemetryParserTest.cpp"
    "${SCRIPT_DIR}/ProfilePolicyTest.cpp"
    "${SCRIPT_DIR}/InotifyIntegrationTest.cpp"
    "${SCRIPT_DIR}/LockFileTest.cpp"
    "${SCRIPT_DIR}/DecisionEngineTest.cpp"
    "${SCRIPT_DIR}/DecisionParityTest.cpp"
    # Production sources, compiled as-is. Not copies.
    "${JNI_DIR}/base/SynthesisCore/SynthesisCore.cpp"
    "${JNI_DIR}/base/ProfilePolicy/ProfilePolicy.cpp"
    "${JNI_DIR}/base/InotifyWatcher/InotifyWatcher.cpp"
    "${JNI_DIR}/base/LockFile/LockFile.cpp"
    "${JNI_DIR}/engine/decision/DecisionEngine.cpp"
    "${JNI_DIR}/engine/decision/DecisionAdapter.cpp"
)

mkdir -p "${BUILD_DIR}"

echo "Compiling with ${CXX} (sanitizers: ${SANITIZE})"
"${CXX}" "${CXXFLAGS[@]}" "${SOURCES[@]}" "${LDFLAGS[@]}" -o "${BUILD_DIR}/flux_tests"

echo "Running"
"${BUILD_DIR}/flux_tests"
