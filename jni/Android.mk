LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := fluxd

LOCAL_C_INCLUDES := $(LOCAL_PATH)/include $(LOCAL_PATH)/base/ProfilePolicy

LOCAL_STATIC_LIBRARIES := rapidjson spdlog FluxTelemetry FluxDecisionEngine FluxExecution FluxDevicePacks PIDTracker InotifyWatcher LockFile GameRegistry FluxUtility DeviceInfo

LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_CPPFLAGS += -fexceptions -std=c++23 -O2 -flto
LOCAL_CPPFLAGS += -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

LOCAL_LDFLAGS += -flto

# ── Release hardening: link-side (the fluxd executable only) ──────────────────
#
# The compile-side half (hidden visibility, function/data sections, file-prefix-map)
# is in jni/Application.mk and covers every translation unit. This half hardens the
# single linked output and is scoped here rather than globally on purpose: the
# version script below hides all defined symbols, which is correct for an executable
# and would be wrong for a shared library. Nothing else in this build is linked.
#
# LTO (-flto, above) is retained: both ABIs build with it and the sanitizer suite is a
# separate g++ build, so it stays healthy. See docs/security/release-verification.md.

# Drop every section the final image does not reach (pairs with -ffunction-sections /
# -fdata-sections). Dead-code elimination at the section level.
LOCAL_LDFLAGS += -Wl,--gc-sections

# Full RELRO + immediate binding: the GOT is resolved at load and mapped read-only, so
# it cannot be overwritten to redirect a later call. -z,noexecstack marks the stack
# non-executable (the NDK default; stated explicitly so it is part of the audited flags).
LOCAL_LDFLAGS += -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

# Do not re-export any symbol pulled in from a static archive (libc++_static, spdlog,
# rapidjson, and every Flux static library). Belt-and-braces with the version script:
# this covers archive-sourced symbols, the version script covers fluxd's own.
LOCAL_LDFLAGS += -Wl,--exclude-libs,ALL

# The authoritative export map. fluxd is entered at the ELF entry point, not through a
# dynamic symbol, so it exports nothing; the map makes every defined global local. See
# jni/fluxd.map and .github/scripts/verify-native-hardening.sh, which proves the shipped
# binary's dynamic table carries no project-internal export.
LOCAL_LDFLAGS += -Wl,--version-script=$(LOCAL_PATH)/fluxd.map

include $(BUILD_EXECUTABLE)

include $(LOCAL_PATH)/external/Android.mk $(LOCAL_PATH)/base/Android.mk $(LOCAL_PATH)/engine/Android.mk $(LOCAL_PATH)/device/Android.mk
