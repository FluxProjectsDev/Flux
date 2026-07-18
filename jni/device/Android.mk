LOCAL_PATH := $(call my-dir)
ROOT_PATH := $(call my-dir)/..

include $(CLEAR_VARS)
LOCAL_MODULE := FluxDevicePacks

# Declarative device packs (Category C). Data only: this module has no write path and depends
# on the execution engine purely for the descriptor types it fills in.
LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_C_INCLUDES := $(ROOT_PATH)/include $(ROOT_PATH)/engine/execution
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)

LOCAL_CPPFLAGS += -fexceptions -std=c++23 -O2
LOCAL_CPPFLAGS += -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

include $(BUILD_STATIC_LIBRARY)
