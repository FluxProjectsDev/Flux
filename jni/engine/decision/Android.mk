LOCAL_PATH := $(call my-dir)
ROOT_PATH := $(call my-dir)/../..

include $(CLEAR_VARS)
LOCAL_MODULE := FluxDecisionEngine

LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

# DecisionEngine.cpp is pure; DecisionAdapter.cpp additionally needs the daemon's
# boundary types (ProfilePolicy.hpp, header-only) and the V2 telemetry snapshot.
LOCAL_C_INCLUDES := $(ROOT_PATH)/include $(ROOT_PATH)/base/ProfilePolicy $(ROOT_PATH)/engine/telemetry

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)


LOCAL_CPPFLAGS += -fexceptions -std=c++23 -O2
LOCAL_CPPFLAGS += -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

include $(BUILD_STATIC_LIBRARY)
