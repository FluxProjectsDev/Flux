APP_ABI := arm64-v8a armeabi-v7a
APP_STL := c++_static
APP_OPTIM := release
APP_PLATFORM := android-26

# ── Release hardening: compile-side (every translation unit in this build) ────
#
# This is an Android-only ndk-build. APP_OPTIM is `release` above, and there is no
# separate debug variant here to exclude. The host test suite (jni/tests/run_tests.sh)
# builds with g++ entirely OUTSIDE ndk-build, so none of these flags reach the
# sanitizer or diagnostic builds — the ASan/UBSan runs stay byte-for-byte what they
# were. See docs/security/release-verification.md for the rationale and the limits
# of what this protects.
#
# These are the compile-side half; the link-side hardening and the export map live
# on the fluxd executable in jni/Android.mk, because a version script that hides all
# defined symbols is only correct for an executable, not for the static libraries.

# Hidden by default. A symbol lands in the dynamic table only if something explicitly
# needs it exported. fluxd is a standalone executable that exports nothing, so hiding
# every symbol's visibility means the version script on the executable has an already
# empty surface to confirm rather than a full one to suppress.
APP_CFLAGS += -fvisibility=hidden
APP_CPPFLAGS += -fvisibility-inlines-hidden

# Per-function / per-data sections so the linker's --gc-sections (see jni/Android.mk)
# can drop everything the final image does not reach. Section-level dead-code removal
# shrinks the binary and removes unreferenced code paths from what ships.
APP_CFLAGS += -ffunction-sections -fdata-sections

# Strip the build host's absolute paths out of anything the compiler embeds: assert()'s
# __FILE__, spdlog's source-location, any __builtin_FILE(). Without this a shipped,
# stripped binary can still carry `/home/<user>/…` or the CI workspace path in .rodata.
#
# FLUX_JNI_ROOT is the absolute path of this Application.mk's directory (jni/); the
# parent is the repository root. Mapping both to neutral tokens rewrites the two prefixes
# that appear in embedded paths. A prefix that does not match is simply not rewritten —
# a wrong value here cannot break compilation — and verify-native-hardening.sh fails the
# build if any developer/workspace path survives into the binary, so a miss is caught and
# corrected rather than shipped. .text and the GNU build-id are unaffected, so the
# link-identity proof in verify-native-telemetry.sh still holds.
FLUX_JNI_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
APP_CFLAGS += -ffile-prefix-map=$(FLUX_JNI_ROOT)=flux
APP_CFLAGS += -ffile-prefix-map=$(abspath $(FLUX_JNI_ROOT)/..)=.
