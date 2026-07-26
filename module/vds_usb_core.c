// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
/*
 * DualSense-compatible USB device model.
 *
 * This file intentionally has no module entry point. vds_hcd_core.c owns the
 * virtual host controller; this file owns the device state, descriptors, and
 * control-transfer behavior instantiated from dualsense.c / dualsense_edge.c.
 */

#include <linux/errno.h>
#include <linux/hid.h>
#include <linux/string.h>

#include "uapi/vds.h"
#include "vds/ds5_protocol.h"
#include "vds_usb.h"

#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_GET_IDLE 0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT 0x09
#define HID_REQ_SET_IDLE 0x0a
#define HID_REQ_SET_PROTOCOL 0x0b
#define HID_REPORT_TYPE_FEATURE 0x03

#define UAC_CS_REQ_SET_CUR 0x01
#define UAC_CS_REQ_GET_CUR 0x81
#define UAC_CS_REQ_GET_MIN 0x82
#define UAC_CS_REQ_GET_MAX 0x83
#define UAC_CS_REQ_GET_RES 0x84
#define UAC_FU_CTRL_MUTE 0x01
#define UAC_FU_CTRL_VOLUME 0x02

#define VDS_USB_NO_INTERFACE 0xff
#define VDS_USB_NO_ENDPOINT 0xff

/* UAC1 volume values are signed 8.8 dB fixed-point units. */
#define VDS_USB_SPK_VOLUME_MIN (-100 * 256)
#define VDS_USB_SPK_VOLUME_MAX 0
#define VDS_USB_SPK_VOLUME_RES 0x0100

static const struct usb_qualifier_descriptor vds_qualifier_desc = {
	.bLength = sizeof(vds_qualifier_desc),
	.bDescriptorType = USB_DT_DEVICE_QUALIFIER,
	.bcdUSB = cpu_to_le16(0x0200),
	.bDeviceClass = 0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.bNumConfigurations = 1,
};

static bool vds_usb_valid_profile(u32 profile)
{
	return profile == VDS_PROFILE_DS5 || profile == VDS_PROFILE_DSE;
}

static const struct vds_controller_profile *
vds_usb_controller_profile_for_profile(u32 profile)
{
	if (profile == VDS_PROFILE_DSE)
		return &vds_dualsense_edge_profile;
	return &vds_dualsense_profile;
}

static void vds_usb_reset_state_locked(struct vds_usb_device *dev)
{
	dev->address = 0;
	dev->configuration = 0;
	dev->audio_out_altsetting = 0;
	dev->audio_in_altsetting = 0;
	dev->hid_idle = 0;
	dev->hid_protocol = 0;
	memset(dev->audio_mute, 0, sizeof(dev->audio_mute));
	dev->audio_volume[VDS_CONTROLLER_AUDIO_FEATURE_SPEAKER] =
		VDS_USB_SPK_VOLUME_MIN;
	dev->audio_volume[VDS_CONTROLLER_AUDIO_FEATURE_MIC] =
		VDS_USB_SPK_VOLUME_MIN;
	memset(dev->feature_cache_len, 0, sizeof(dev->feature_cache_len));
}

void vds_usb_device_init(struct vds_usb_device *dev, u32 profile)
{
	memset(dev, 0, sizeof(*dev));
	spin_lock_init(&dev->lock);
	dev->profile = vds_usb_valid_profile(profile) ? profile :
							VDS_PROFILE_DS5;
	vds_usb_reset_state_locked(dev);
}

void vds_usb_device_reset_state(struct vds_usb_device *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	vds_usb_reset_state_locked(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
}

u32 vds_usb_device_profile_id(const struct vds_usb_device *dev)
{
	return READ_ONCE(dev->profile);
}

int vds_usb_device_set_profile(struct vds_usb_device *dev, u32 profile)
{
	unsigned long flags;

	if (!vds_usb_valid_profile(profile))
		return -EINVAL;

	spin_lock_irqsave(&dev->lock, flags);
	WRITE_ONCE(dev->profile, profile);
	vds_usb_reset_state_locked(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
	return 0;
}

const struct vds_controller_profile *
vds_usb_device_profile(const struct vds_usb_device *dev)
{
	return vds_usb_controller_profile_for_profile(vds_usb_device_profile_id(dev));
}

int vds_usb_device_update_feature_reply(struct vds_usb_device *dev,
					const u8 *payload, u32 length)
{
	unsigned long flags;
	u8 report_id;

	if (!payload || !length || length > VDS_CONTROLLER_HID_PACKET_SIZE)
		return -EINVAL;

	report_id = payload[0];
	spin_lock_irqsave(&dev->lock, flags);
	memcpy(dev->feature_cache[report_id], payload, length);
	dev->feature_cache_len[report_id] = length;
	spin_unlock_irqrestore(&dev->lock, flags);
	return 0;
}

static int vds_usb_endpoint_number(u8 endpoint_address)
{
	return endpoint_address & USB_ENDPOINT_NUMBER_MASK;
}

bool vds_usb_device_is_hid_in(const struct vds_usb_device *dev, int endpoint)
{
	const struct vds_controller_profile *profile =
		vds_usb_device_profile(dev);

	return endpoint == vds_usb_endpoint_number(profile->hid_in_endpoint);
}

bool vds_usb_device_is_hid_out(const struct vds_usb_device *dev, int endpoint)
{
	const struct vds_controller_profile *profile =
		vds_usb_device_profile(dev);

	return endpoint == vds_usb_endpoint_number(profile->hid_out_endpoint);
}

bool vds_usb_device_is_audio_out(const struct vds_usb_device *dev, int endpoint)
{
	const struct vds_controller_profile *profile =
		vds_usb_device_profile(dev);

	return endpoint == vds_usb_endpoint_number(profile->audio_out_endpoint);
}

bool vds_usb_device_is_audio_in(const struct vds_usb_device *dev, int endpoint)
{
	const struct vds_controller_profile *profile =
		vds_usb_device_profile(dev);

	return profile->audio_in_endpoint != VDS_USB_NO_ENDPOINT &&
	       endpoint == vds_usb_endpoint_number(profile->audio_in_endpoint);
}

static int vds_usb_copy_to_urb(struct urb *urb, const void *data, u32 length)
{
	u32 copy_len = min_t(u32, length, urb->transfer_buffer_length);

	if (copy_len && !urb->transfer_buffer)
		return -EPIPE;
	if (copy_len)
		memcpy(urb->transfer_buffer, data, copy_len);
	urb->actual_length = copy_len;
	return 0;
}

static int vds_usb_copy_string_descriptor(struct urb *urb, const char *string)
{
	u8 descriptor[255];
	u8 length;
	int i;

	if (!string) {
		descriptor[0] = 4;
		descriptor[1] = USB_DT_STRING;
		descriptor[2] = 0x09;
		descriptor[3] = 0x04;
		return vds_usb_copy_to_urb(urb, descriptor, descriptor[0]);
	}

	length = 2;
	for (i = 0; string[i] && length + 2 <= sizeof(descriptor); i++) {
		descriptor[length++] = string[i];
		descriptor[length++] = 0;
	}

	descriptor[0] = length;
	descriptor[1] = USB_DT_STRING;
	return vds_usb_copy_to_urb(urb, descriptor, length);
}

static int vds_usb_copy_cached_feature_report(struct vds_usb_device *dev,
					      struct urb *urb, u8 report_id)
{
	u8 report[VDS_CONTROLLER_HID_PACKET_SIZE];
	unsigned long flags;
	u16 report_len;

	spin_lock_irqsave(&dev->lock, flags);
	report_len = dev->feature_cache_len[report_id];
	if (report_len)
		memcpy(report, dev->feature_cache[report_id], report_len);
	spin_unlock_irqrestore(&dev->lock, flags);

	if (!report_len)
		return -ENOENT;

	return vds_usb_copy_to_urb(urb, report, report_len);
}

static void vds_usb_set_status(const struct vds_usb_device_ops *ops,
			       void *context, u32 flags, bool enabled)
{
	if (ops && ops->set_status)
		ops->set_status(context, flags, enabled);
}

static int vds_usb_enqueue_frame(const struct vds_usb_device_ops *ops,
				 void *context, u16 type, const void *payload,
				 u32 length, gfp_t gfp)
{
	if (!ops || !ops->enqueue_frame)
		return 0;
	return ops->enqueue_frame(context, type, payload, length, gfp);
}

static void
vds_usb_enqueue_interface_event(const struct vds_usb_device_ops *ops,
				void *context, u8 interface_number,
				u8 altsetting, u8 interface_type, gfp_t gfp)
{
	struct vds_usb_interface_event event = {
		.interface_number = interface_number,
		.altsetting = altsetting,
		.interface_type = interface_type,
	};

	(void)vds_usb_enqueue_frame(ops, context, VDS_FRAME_USB_INTERFACE,
				    &event, sizeof(event), gfp);
}

static int vds_usb_defer_feature_get(const struct vds_usb_device_ops *ops,
				     void *context, struct urb *urb,
				     u8 report_id)
{
	if (!ops || !ops->defer_feature_get || !ops->enqueue_frame)
		return -EPIPE;
	return ops->defer_feature_get(context, urb, report_id);
}

static int vds_usb_audio_control(struct vds_usb_device *dev, struct urb *urb,
				 const struct usb_ctrlrequest *setup,
				 const struct vds_controller_profile *profile)
{
	u16 value = le16_to_cpu(setup->wValue);
	u16 index = le16_to_cpu(setup->wIndex);
	u8 entity_id = index >> 8;
	u8 intf = index & 0xff;
	u8 selector = value >> 8;
	unsigned long flags;
	int feature_index;
	u8 mute;
	__le16 volume;

	if (intf != profile->audio_control_interface)
		return -EPIPE;

	feature_index = vds_controller_audio_feature_index(profile, entity_id);
	if (feature_index < 0)
		return -EPIPE;

	if (setup->bRequestType & USB_DIR_IN) {
		switch (selector) {
		case UAC_FU_CTRL_MUTE:
			if (setup->bRequest != UAC_CS_REQ_GET_CUR)
				return -EPIPE;
			spin_lock_irqsave(&dev->lock, flags);
			mute = dev->audio_mute[feature_index];
			spin_unlock_irqrestore(&dev->lock, flags);
			return vds_usb_copy_to_urb(urb, &mute, sizeof(mute));
		case UAC_FU_CTRL_VOLUME:
			switch (setup->bRequest) {
			case UAC_CS_REQ_GET_CUR:
				spin_lock_irqsave(&dev->lock, flags);
				volume =
					cpu_to_le16(dev->audio_volume[feature_index]);
				spin_unlock_irqrestore(&dev->lock, flags);
				break;
			case UAC_CS_REQ_GET_MIN:
				volume = cpu_to_le16(VDS_USB_SPK_VOLUME_MIN);
				break;
			case UAC_CS_REQ_GET_MAX:
				volume = cpu_to_le16(VDS_USB_SPK_VOLUME_MAX);
				break;
			case UAC_CS_REQ_GET_RES:
				volume = cpu_to_le16(VDS_USB_SPK_VOLUME_RES);
				break;
			default:
				return -EPIPE;
			}
			return vds_usb_copy_to_urb(urb, &volume,
						   sizeof(volume));
		default:
			return -EPIPE;
		}
	}

	if (setup->bRequest != UAC_CS_REQ_SET_CUR || !urb->transfer_buffer)
		return -EPIPE;

	spin_lock_irqsave(&dev->lock, flags);
	if (selector == UAC_FU_CTRL_MUTE && urb->transfer_buffer_length >= 1) {
		dev->audio_mute[feature_index] = *(u8 *)urb->transfer_buffer;
		urb->actual_length = 1;
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	}
	if (selector == UAC_FU_CTRL_VOLUME &&
	    urb->transfer_buffer_length >= sizeof(__le16)) {
		memcpy(&volume, urb->transfer_buffer, sizeof(volume));
		dev->audio_volume[feature_index] = (s16)le16_to_cpu(volume);
		urb->actual_length = sizeof(volume);
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	return -EPIPE;
}

static int vds_usb_hid_control(struct vds_usb_device *dev, struct urb *urb,
			       const struct usb_ctrlrequest *setup,
			       const struct vds_controller_profile *profile,
			       const struct vds_usb_device_ops *ops,
			       void *context, gfp_t gfp)
{
	u16 value = le16_to_cpu(setup->wValue);
	u16 index = le16_to_cpu(setup->wIndex);
	u8 report_type = value >> 8;
	u8 report_id = value & 0xff;
	unsigned long flags;
	u8 zero_report[VDS_CONTROLLER_HID_PACKET_SIZE] = { 0 };
	int ret;

	if ((index & 0xff) != profile->hid_interface)
		return -EPIPE;

	switch (setup->bRequest) {
	case HID_REQ_GET_REPORT:
		if (report_type == HID_REPORT_TYPE_FEATURE) {
			u16 requested_length = le16_to_cpu(setup->wLength);
			u8 feature_get[3] = {
				report_id,
				requested_length & 0xff,
				requested_length >> 8,
			};

			ret = vds_usb_copy_cached_feature_report(dev, urb,
								 report_id);
			if (!ret)
				return 0;
			if (ret != -ENOENT)
				return ret;

			/*
			 * A feature cache miss must be answered by the physical
			 * controller. The HCD queues this URB before userspace
			 * sees the request frame, so a fast reply cannot race the
			 * pending list.
			 */
			ret = vds_usb_defer_feature_get(ops, context, urb,
							report_id);
			if (ret)
				return ret;
			ret = vds_usb_enqueue_frame(ops, context,
						    VDS_FRAME_USB_FEATURE_GET,
						    feature_get,
						    sizeof(feature_get), gfp);
			return ret ? ret : -EINPROGRESS;
		}
		zero_report[0] = report_id;
		return vds_usb_copy_to_urb(urb, zero_report,
					   sizeof(zero_report));
	case HID_REQ_SET_REPORT:
		if (report_type != HID_REPORT_TYPE_FEATURE)
			return -EPIPE;
		if (urb->transfer_buffer_length && !urb->transfer_buffer)
			return -EPIPE;
		if (urb->transfer_buffer_length) {
			u8 feature_set[VDS_CONTROLLER_HID_PACKET_SIZE + 1];

			if (urb->transfer_buffer_length >
			    VDS_CONTROLLER_HID_PACKET_SIZE)
				return -EPIPE;

			feature_set[0] = report_id;
			memcpy(feature_set + 1, urb->transfer_buffer,
			       urb->transfer_buffer_length);
			ret = vds_usb_enqueue_frame(ops, context,
						    VDS_FRAME_USB_FEATURE_SET,
						    feature_set,
						    urb->transfer_buffer_length + 1,
						    gfp);
			if (ret)
				return ret;
		}
		urb->actual_length = urb->transfer_buffer_length;
		return 0;
	case HID_REQ_GET_IDLE:
		spin_lock_irqsave(&dev->lock, flags);
		zero_report[0] = dev->hid_idle;
		spin_unlock_irqrestore(&dev->lock, flags);
		return vds_usb_copy_to_urb(urb, zero_report, 1);
	case HID_REQ_GET_PROTOCOL:
		spin_lock_irqsave(&dev->lock, flags);
		zero_report[0] = dev->hid_protocol;
		spin_unlock_irqrestore(&dev->lock, flags);
		return vds_usb_copy_to_urb(urb, zero_report, 1);
	case HID_REQ_SET_IDLE:
		spin_lock_irqsave(&dev->lock, flags);
		dev->hid_idle = value >> 8;
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	case HID_REQ_SET_PROTOCOL:
		spin_lock_irqsave(&dev->lock, flags);
		dev->hid_protocol = value & 0xff;
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	default:
		return -EPIPE;
	}
}

static int
vds_usb_standard_control(struct vds_usb_device *dev, struct urb *urb,
			 const struct usb_ctrlrequest *setup,
			 const struct vds_controller_profile *profile,
			 const struct vds_usb_device_ops *ops, void *context,
			 gfp_t gfp)
{
	struct usb_device_descriptor device_desc = { 0 };
	u16 value = le16_to_cpu(setup->wValue);
	u16 index = le16_to_cpu(setup->wIndex);
	u8 desc_type = value >> 8;
	u8 desc_index = value & 0xff;
	u8 configuration;
	bool configured;
	bool audio_enabled;
	unsigned long flags;
	__le16 status = cpu_to_le16(0);

	switch (setup->bRequest) {
	case USB_REQ_GET_DESCRIPTOR:
		switch (desc_type) {
		case USB_DT_DEVICE:
			device_desc.bLength = USB_DT_DEVICE_SIZE;
			device_desc.bDescriptorType = USB_DT_DEVICE;
			device_desc.bcdUSB = cpu_to_le16(0x0200);
			device_desc.bDeviceClass = profile->device_class;
			device_desc.bDeviceSubClass = profile->device_subclass;
			device_desc.bDeviceProtocol = profile->device_protocol;
			device_desc.bMaxPacketSize0 = profile->max_packet_size0;
			device_desc.idVendor = cpu_to_le16(VDS_SONY_VENDOR_ID);
			device_desc.idProduct =
				cpu_to_le16(profile->product_id);
			device_desc.bcdDevice =
				cpu_to_le16(profile->device_version);
			device_desc.iManufacturer = 1;
			device_desc.iProduct = 2;
			device_desc.bNumConfigurations =
				profile->num_configurations;
			return vds_usb_copy_to_urb(urb, &device_desc,
						   sizeof(device_desc));
		case USB_DT_CONFIG:
			return vds_usb_copy_to_urb(urb,
						   profile->configuration_descriptor,
						   profile->configuration_descriptor_size);
		case USB_DT_DEVICE_QUALIFIER:
			return vds_usb_copy_to_urb(urb, &vds_qualifier_desc,
						   sizeof(vds_qualifier_desc));
		case USB_DT_STRING:
			switch (desc_index) {
			case 0:
				return vds_usb_copy_string_descriptor(urb,
								      NULL);
			case 1:
				return vds_usb_copy_string_descriptor(urb,
						profile->manufacturer);
			case 2:
				return vds_usb_copy_string_descriptor(urb,
						profile->product);
			default:
				return -EPIPE;
			}
		case HID_DT_REPORT:
			if ((index & 0xff) != profile->hid_interface)
				return -EPIPE;
			return vds_usb_copy_to_urb(urb,
						   profile->hid_report_descriptor,
						   profile->hid_report_descriptor_size);
		case HID_DT_HID:
			if ((index & 0xff) != profile->hid_interface)
				return -EPIPE;
			return vds_usb_copy_to_urb(urb,
						   profile->hid_descriptor,
						   profile->hid_descriptor_size);
		default:
			return -EPIPE;
		}
	case USB_REQ_SET_ADDRESS:
		spin_lock_irqsave(&dev->lock, flags);
		dev->address = value & 0x7f;
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	case USB_REQ_SET_CONFIGURATION:
		if (value && value != profile->configuration_value)
			return -EPIPE;
		spin_lock_irqsave(&dev->lock, flags);
		dev->configuration = value & 0xff;
		dev->audio_out_altsetting = 0;
		dev->audio_in_altsetting = 0;
		configured = dev->configuration == profile->configuration_value;
		spin_unlock_irqrestore(&dev->lock, flags);
		vds_usb_set_status(ops, context,
				   VDS_STATUS_CONFIGURED |
					   VDS_STATUS_HID_ENABLED,
				   configured);
		vds_usb_set_status(ops, context, VDS_STATUS_AUDIO_ENABLED,
				   false);
		return 0;
	case USB_REQ_GET_CONFIGURATION:
		spin_lock_irqsave(&dev->lock, flags);
		configuration = dev->configuration;
		spin_unlock_irqrestore(&dev->lock, flags);
		return vds_usb_copy_to_urb(urb, &configuration,
					   sizeof(configuration));
	case USB_REQ_SET_INTERFACE:
		if ((index & 0xff) == profile->hid_interface) {
			if (value)
				return -EPIPE;
			vds_usb_enqueue_interface_event(ops, context,
							profile->hid_interface,
							0,
							VDS_USB_INTERFACE_HID,
							gfp);
			return 0;
		}
		if (value > 1)
			return -EPIPE;
		if ((index & 0xff) == profile->audio_out_interface) {
			spin_lock_irqsave(&dev->lock, flags);
			dev->audio_out_altsetting = value & 0xff;
			audio_enabled = dev->audio_out_altsetting == 1;
			spin_unlock_irqrestore(&dev->lock, flags);
			vds_usb_set_status(ops, context,
					   VDS_STATUS_AUDIO_ENABLED,
					   audio_enabled);
			vds_usb_enqueue_interface_event(ops, context,
							profile->audio_out_interface,
							value & 0xff,
							VDS_USB_INTERFACE_AUDIO_OUT,
							gfp);
			return 0;
		}
		if (profile->audio_in_interface != VDS_USB_NO_INTERFACE &&
		    (index & 0xff) == profile->audio_in_interface) {
			spin_lock_irqsave(&dev->lock, flags);
			dev->audio_in_altsetting = value & 0xff;
			spin_unlock_irqrestore(&dev->lock, flags);
			vds_usb_enqueue_interface_event(ops, context,
							profile->audio_in_interface,
							value & 0xff,
							VDS_USB_INTERFACE_AUDIO_IN,
							gfp);
			return 0;
		}
		return -EPIPE;
	case USB_REQ_GET_INTERFACE:
		spin_lock_irqsave(&dev->lock, flags);
		if ((index & 0xff) == profile->audio_out_interface) {
			configuration = dev->audio_out_altsetting;
		} else if (profile->audio_in_interface !=
				   VDS_USB_NO_INTERFACE &&
			   (index & 0xff) == profile->audio_in_interface) {
			configuration = dev->audio_in_altsetting;
		} else if ((index & 0xff) == profile->hid_interface) {
			configuration = 0;
		} else {
			spin_unlock_irqrestore(&dev->lock, flags);
			return -EPIPE;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		return vds_usb_copy_to_urb(urb, &configuration,
					   sizeof(configuration));
	case USB_REQ_GET_STATUS:
		return vds_usb_copy_to_urb(urb, &status, sizeof(status));
	case USB_REQ_CLEAR_FEATURE:
	case USB_REQ_SET_FEATURE:
		return 0;
	default:
		return -EPIPE;
	}
}

int vds_usb_device_control_urb(struct vds_usb_device *dev, struct urb *urb,
			       const struct vds_usb_device_ops *ops,
			       void *context, gfp_t gfp)
{
	const struct vds_controller_profile *profile =
		vds_usb_device_profile(dev);
	struct usb_ctrlrequest setup;
	u8 type;

	if (!urb->setup_packet)
		return -EPIPE;

	memcpy(&setup, urb->setup_packet, sizeof(setup));
	urb->actual_length = 0;
	type = setup.bRequestType & USB_TYPE_MASK;

	if (type == USB_TYPE_STANDARD)
		return vds_usb_standard_control(dev, urb, &setup, profile, ops,
						context, gfp);
	if (type == USB_TYPE_CLASS &&
	    (setup.bRequestType & USB_RECIP_MASK) == USB_RECIP_INTERFACE) {
		if ((le16_to_cpu(setup.wIndex) & 0xff) ==
		    profile->hid_interface)
			return vds_usb_hid_control(dev, urb, &setup, profile,
						   ops, context, gfp);
		return vds_usb_audio_control(dev, urb, &setup, profile);
	}

	return -EPIPE;
}
