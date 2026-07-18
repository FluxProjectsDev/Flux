LOCAL_PATH := $(call my-dir)
ROOT_PATH := $(call my-dir)/../..

include $(CLEAR_VARS)
LOCAL_MODULE := FluxExecution

LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

# Pure: depends only on the standard library and its own headers.
# PolicyIntent maps a Decision into device-independent intents, so it needs the
# decision vocabulary (header-only usage: Decision, DecisionReason, priorities).
LOCAL_C_INCLUDES := $(ROOT_PATH)/include $(ROOT_PATH)/engine/decision

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)

LOCAL_CPPFLAGS += -fexceptions -std=c++23 -O2
LOCAL_CPPFLAGS += -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

include $(BUILD_STATIC_LIBRARY)
