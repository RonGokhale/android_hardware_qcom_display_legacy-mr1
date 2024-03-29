ifneq ($(filter msm7x27,$(TARGET_BOARD_PLATFORM)),)
ifeq ($(TARGET_QCOM_DISPLAY_VARIANT),mr1)
display-hals := libgralloc libgenlock libcopybit
display-hals += libhwcomposer liboverlay libqdutils libexternal libqservice
ifneq ($(TARGET_PROVIDES_LIBLIGHT),true)
display-hals += liblight
endif
include $(call all-named-subdir-makefiles,$(display-hals))
endif
endif
