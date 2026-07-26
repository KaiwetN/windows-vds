// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "uapi/vds.h"
#include "vds/ds5_usb.h"
#include "vds_controller.h"

const struct vds_controller_profile vds_dualsense_profile = {
	.profile = VDS_PROFILE_DS5,
	.product_id = VDS_DS5_PRODUCT_ID,
	.device_version = VDS_USB_DEVICE_BCD,
	.device_class = VDS_USB_DEVICE_CLASS,
	.device_subclass = VDS_USB_DEVICE_SUBCLASS,
	.device_protocol = VDS_USB_DEVICE_PROTOCOL,
	.max_packet_size0 = VDS_USB_MAX_PACKET_SIZE0,
	.num_configurations = VDS_USB_NUM_CONFIGURATIONS,
	.configuration_value = VDS_USB_CONFIGURATION_VALUE,
	.manufacturer = VDS_USB_MANUFACTURER_STRING,
	.product = VDS_DS5_USB_PRODUCT_STRING,
	.configuration_descriptor = vds_ds5_usb_configuration_descriptor,
	.configuration_descriptor_size =
		sizeof(vds_ds5_usb_configuration_descriptor),
	.hid_descriptor = vds_ds5_usb_hid_descriptor,
	.hid_descriptor_size = sizeof(vds_ds5_usb_hid_descriptor),
	.hid_report_descriptor = vds_ds5_usb_hid_report_descriptor,
	.hid_report_descriptor_size = sizeof(vds_ds5_usb_hid_report_descriptor),
	.audio_control_interface = VDS_USB_AUDIO_CONTROL_INTERFACE,
	.audio_out_interface = VDS_USB_AUDIO_OUT_INTERFACE,
	.audio_in_interface = VDS_USB_AUDIO_IN_INTERFACE,
	.hid_interface = VDS_USB_HID_INTERFACE,
	.audio_out_endpoint = VDS_USB_AUDIO_OUT_ENDPOINT,
	.audio_in_endpoint = VDS_USB_AUDIO_IN_ENDPOINT,
	.hid_in_endpoint = VDS_USB_HID_IN_ENDPOINT,
	.hid_out_endpoint = VDS_USB_HID_OUT_ENDPOINT,
	.hid_in_interval_us = VDS_DS5_USB_HID_IN_INTERVAL_US,
	.speaker_feature_unit = VDS_USB_SPEAKER_FEATURE_UNIT,
	.mic_feature_unit = VDS_USB_MIC_FEATURE_UNIT,
};
