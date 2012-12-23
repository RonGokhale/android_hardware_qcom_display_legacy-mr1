#Common headers
common_includes := hardware/qcom/display_legacy-mr1/libgralloc
common_includes += hardware/qcom/display_legacy-mr1/libgenlock
common_includes += hardware/qcom/display_legacy-mr1/liboverlay
common_includes += hardware/qcom/display_legacy-mr1/libcopybit
common_includes += hardware/qcom/display_legacy-mr1/libqdutils
common_includes += hardware/qcom/display_legacy-mr1/libhwcomposer
common_includes += hardware/qcom/display_legacy-mr1/libexternal
common_includes += hardware/qcom/display_legacy-mr1/libqservice

ifeq ($(TARGET_USES_POST_PROCESSING),true)
    common_flags     += -DUSES_POST_PROCESSING
    common_includes += $(TARGET_OUT_HEADERS)/pp/inc
endif


#Common libraries external to display HAL
common_libs := liblog libutils libcutils libhardware

#Common C flags
common_flags := -DDEBUG_CALC_FPS -Wno-missing-field-initializers

common_flags += -DNO_HDMI -DNO_HW_OVERLAY
