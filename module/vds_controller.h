/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com> */
#ifndef VDS_CONTROLLER_H
#define VDS_CONTROLLER_H

#include <linux/types.h>

#define VDS_CONTROLLER_NO_AUDIO_FEATURE 0xff

#define VDS_CONTROLLER_HID_PACKET_SIZE 64

enum vds_controller_audio_feature {
	VDS_CONTROLLER_AUDIO_FEATURE_SPEAKER = 0,
	VDS_CONTROLLER_AUDIO_FEATURE_MIC = 1,
};

struct vds_controller_profile {
	u32 profile;
	u16 product_id;
	u16 device_version;
	u8 device_class;
	u8 device_subclass;
	u8 device_protocol;
	u8 max_packet_size0;
	u8 num_configurations;
	u8 configuration_value;
	const char *manufacturer;
	const char *product;
	const u8 *configuration_descriptor;
	u16 configuration_descriptor_size;
	const u8 *hid_descriptor;
	u16 hid_descriptor_size;
	const u8 *hid_report_descriptor;
	u16 hid_report_descriptor_size;
	u8 audio_control_interface;
	u8 audio_out_interface;
	u8 audio_in_interface;
	u8 hid_interface;
	u8 audio_out_endpoint;
	u8 audio_in_endpoint;
	u8 hid_in_endpoint;
	u8 hid_out_endpoint;
	u32 hid_in_interval_us;
	u8 speaker_feature_unit;
	u8 mic_feature_unit;
};

extern const struct vds_controller_profile vds_dualsense_profile;
extern const struct vds_controller_profile vds_dualsense_edge_profile;

static inline int
vds_controller_audio_feature_index(const struct vds_controller_profile *profile,
				   u8 entity_id)
{
	if (entity_id == profile->speaker_feature_unit)
		return VDS_CONTROLLER_AUDIO_FEATURE_SPEAKER;
	if (profile->mic_feature_unit != VDS_CONTROLLER_NO_AUDIO_FEATURE &&
	    entity_id == profile->mic_feature_unit)
		return VDS_CONTROLLER_AUDIO_FEATURE_MIC;
	return -1;
}

#endif /* VDS_CONTROLLER_H */
