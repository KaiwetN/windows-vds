/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com> */
#ifndef VDS_USB_H
#define VDS_USB_H

#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb.h>

#include "vds_controller.h"

#define VDS_USB_AUDIO_FEATURE_COUNT 2

struct vds_usb_device {
	/* Protects address/configuration/interface state and feature cache. */
	spinlock_t lock;
	u32 profile;
	u8 address;
	u8 configuration;
	u8 audio_out_altsetting;
	u8 audio_in_altsetting;
	u8 hid_idle;
	u8 hid_protocol;
	u8 audio_mute[VDS_USB_AUDIO_FEATURE_COUNT];
	s16 audio_volume[VDS_USB_AUDIO_FEATURE_COUNT];
	u16 feature_cache_len[256];
	u8 feature_cache[256][VDS_CONTROLLER_HID_PACKET_SIZE];
};

struct vds_usb_device_ops {
	int (*enqueue_frame)(void *context, u16 type, const void *payload,
			     u32 length, gfp_t gfp);
	int (*defer_feature_get)(void *context, struct urb *urb, u8 report_id);
	void (*set_status)(void *context, u32 flags, bool enabled);
};

void vds_usb_device_init(struct vds_usb_device *dev, u32 profile);
void vds_usb_device_reset_state(struct vds_usb_device *dev);
u32 vds_usb_device_profile_id(const struct vds_usb_device *dev);
int vds_usb_device_set_profile(struct vds_usb_device *dev, u32 profile);
const struct vds_controller_profile *
vds_usb_device_profile(const struct vds_usb_device *dev);
int vds_usb_device_update_feature_reply(struct vds_usb_device *dev,
					const u8 *payload, u32 length);
bool vds_usb_device_is_hid_in(const struct vds_usb_device *dev, int endpoint);
bool vds_usb_device_is_hid_out(const struct vds_usb_device *dev, int endpoint);
bool vds_usb_device_is_audio_out(const struct vds_usb_device *dev,
				 int endpoint);
bool vds_usb_device_is_audio_in(const struct vds_usb_device *dev, int endpoint);
int vds_usb_device_control_urb(struct vds_usb_device *dev, struct urb *urb,
			       const struct vds_usb_device_ops *ops,
			       void *context, gfp_t gfp);

#endif /* VDS_USB_H */
