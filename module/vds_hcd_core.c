// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
/*
 * Virtual DualSense host controller frontend.
 *
 * This module exposes one or more independent virtual high-speed USB
 * DualSense-compatible devices directly below the Linux USB core. It is
 * intentionally device-specific: only the endpoints and control requests needed
 * by a wired DualSense-compatible path are implemented.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/usb/ch11.h>
#include <linux/usb/hcd.h>

#include "uapi/vds.h"
#include "vds_usb.h"

#ifndef kmalloc_obj
#define kmalloc_obj(VAR_OR_TYPE, ...) kmalloc(sizeof(VAR_OR_TYPE), __VA_ARGS__)
#endif

#define VDS_HCD_DRIVER_NAME "vds_hcd"
#define VDS_AUDIO_IN_BYTES_PER_MS	192
#define VDS_AUDIO_OUT_BYTES_PER_MS	384
#define VDS_AUDIO_IN_QUEUE_MS		100
#define VDS_AUDIO_IN_QUEUE_MAX_BYTES \
	(VDS_AUDIO_IN_BYTES_PER_MS * VDS_AUDIO_IN_QUEUE_MS)

static unsigned int max_port = VDS_MAX_PORT_COUNT;
module_param(max_port, uint, 0444);
MODULE_PARM_DESC(max_port, "Number of virtual DualSense ports to create (1-4)");

/* Root-hub port change bits live in the high 16 bits of port_status. */
#define PORT_C_MASK                                               \
	((USB_PORT_STAT_C_CONNECTION | USB_PORT_STAT_C_ENABLE |   \
	  USB_PORT_STAT_C_SUSPEND | USB_PORT_STAT_C_OVERCURRENT | \
	  USB_PORT_STAT_C_RESET)                                  \
	 << 16)

struct vds_event {
	struct list_head list;
	struct vds_frame_header header;
	u8 payload[];
};

struct vds_input_packet {
	struct list_head list;
	u32 length;
	u8 payload[VDS_CONTROLLER_HID_PACKET_SIZE];
};

struct vds_audio_packet {
	struct list_head list;
	u32 offset;
	u32 length;
	u8 payload[VDS_FRAME_MAX_PAYLOAD];
};

enum vds_urb_context_state {
	VDS_URB_ACTIVE = 0,
	VDS_URB_PENDING_HID_IN,
	VDS_URB_PENDING_FEATURE_GET,
	VDS_URB_PENDING_GIVEBACK,
	VDS_URB_GIVING_BACK,
};

struct vds_urb_context {
	struct list_head list;
	struct urb *urb;
	ktime_t ready_time;
	int status;
	u8 feature_report_id;
	enum vds_urb_context_state state;
	bool deliver_iso_out;
	bool deliver_iso_in;
};

struct vds_hcd_dev {
	struct usb_hcd *hcd;
	struct miscdevice misc;
	char misc_name[16];
	/* Protects frame queues, URB queues, status flags, and port state. */
	spinlock_t lock;
	wait_queue_head_t read_wq;
	wait_queue_head_t giveback_wq;
	struct list_head events;
	struct list_head pending_hid_in;
	struct list_head pending_feature_get;
	struct list_head completed_urbs;
	struct list_head input_packets;
	struct list_head audio_in_packets;
	struct task_struct *giveback_thread;
	u64 frames_to_user;
	u64 frames_from_user;
	u64 next_sequence;
	ktime_t next_iso_out_ready;
	ktime_t next_iso_in_ready;
	ktime_t next_hid_in_ready;
	u32 event_count;
	u32 status_flags;
	u32 port_status;
	bool stopping;
	bool opened;
	unsigned int port_index;
	u32 input_packet_count;
	u32 audio_in_packet_count;
	u32 audio_in_queued_bytes;
	struct vds_usb_device usb;
};

static struct platform_device **vds_hcd_platform_devices;
static unsigned int vds_hcd_port_count;

static struct vds_hcd_dev *vds_hcd_priv(struct usb_hcd *hcd)
{
	return (struct vds_hcd_dev *)hcd->hcd_priv;
}

static struct vds_hcd_dev *vds_from_file(struct file *file)
{
	return container_of(file->private_data, struct vds_hcd_dev, misc);
}

static void vds_status_set_locked(struct vds_hcd_dev *dev, u32 flags,
				  bool enabled)
{
	if (enabled)
		dev->status_flags |= flags;
	else
		dev->status_flags &= ~flags;
}

static bool vds_is_stopping(struct vds_hcd_dev *dev)
{
	unsigned long flags;
	bool stopping;

	spin_lock_irqsave(&dev->lock, flags);
	stopping = dev->stopping;
	spin_unlock_irqrestore(&dev->lock, flags);
	return stopping;
}

static bool vds_read_ready(struct vds_hcd_dev *dev)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&dev->lock, flags);
	ready = dev->stopping || !list_empty(&dev->events);
	spin_unlock_irqrestore(&dev->lock, flags);
	return ready;
}

static bool vds_completed_urbs_ready(struct vds_hcd_dev *dev)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&dev->lock, flags);
	ready = !list_empty(&dev->completed_urbs);
	spin_unlock_irqrestore(&dev->lock, flags);
	return ready;
}

static int vds_enqueue_frame(struct vds_hcd_dev *dev, u16 type,
			     const void *payload, u32 length, gfp_t gfp)
{
	struct vds_event *event;
	unsigned long flags;

	if (length > VDS_FRAME_MAX_PAYLOAD)
		return -EMSGSIZE;
	if (length && !payload)
		return -EINVAL;

	event = kmalloc(sizeof(*event) + length, gfp);
	if (!event)
		return -ENOMEM;

	event->header.type = type;
	event->header.flags = 0;
	event->header.length = length;
	if (payload && length)
		memcpy(event->payload, payload, length);

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->stopping) {
		spin_unlock_irqrestore(&dev->lock, flags);
		kfree(event);
		return -ESHUTDOWN;
	}
	/* Bound queued USB-to-userspace frames per virtual controller. */
	if (dev->event_count >= 16384) {
		spin_unlock_irqrestore(&dev->lock, flags);
		kfree(event);
		return -ENOSPC;
	}
	event->header.sequence = dev->next_sequence++;
	list_add_tail(&event->list, &dev->events);
	dev->event_count++;
	dev->frames_to_user++;
	spin_unlock_irqrestore(&dev->lock, flags);

	wake_up_interruptible(&dev->read_wq);
	return 0;
}

static int vds_usb_enqueue_frame(void *context, u16 type, const void *payload,
				 u32 length, gfp_t gfp)
{
	return vds_enqueue_frame(context, type, payload, length, gfp);
}

static int vds_usb_defer_feature_get(void *context, struct urb *urb,
				     u8 report_id)
{
	struct vds_hcd_dev *dev = context;
	struct vds_urb_context *urb_context = urb->hcpriv;
	unsigned long flags;
	int ret = 0;

	if (!urb_context)
		return -EINVAL;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->stopping) {
		ret = -ESHUTDOWN;
	} else {
		urb_context->feature_report_id = report_id;
		urb_context->state = VDS_URB_PENDING_FEATURE_GET;
		list_add_tail(&urb_context->list, &dev->pending_feature_get);
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	return ret;
}

static void vds_usb_set_status(void *context, u32 flags, bool enabled)
{
	struct vds_hcd_dev *dev = context;
	unsigned long irq_flags;

	spin_lock_irqsave(&dev->lock, irq_flags);
	vds_status_set_locked(dev, flags, enabled);
	spin_unlock_irqrestore(&dev->lock, irq_flags);
}

static const struct vds_usb_device_ops vds_usb_ops = {
	.enqueue_frame = vds_usb_enqueue_frame,
	.defer_feature_get = vds_usb_defer_feature_get,
	.set_status = vds_usb_set_status,
};

static ssize_t vds_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct vds_hcd_dev *dev = vds_from_file(file);
	struct vds_event *event;
	unsigned long flags;
	size_t frame_size;
	int ret;

	(void)ppos;

	if (file->f_flags & O_NONBLOCK) {
		spin_lock_irqsave(&dev->lock, flags);
		ret = list_empty(&dev->events) && !dev->stopping ? -EAGAIN : 0;
		spin_unlock_irqrestore(&dev->lock, flags);
		if (ret)
			return ret;
	}

	ret = wait_event_interruptible(dev->read_wq, vds_read_ready(dev));
	if (ret)
		return ret;

	spin_lock_irqsave(&dev->lock, flags);
	if (list_empty(&dev->events)) {
		ret = dev->stopping ? -ESHUTDOWN : -EAGAIN;
		spin_unlock_irqrestore(&dev->lock, flags);
		return ret;
	}
	event = list_first_entry(&dev->events, struct vds_event, list);
	frame_size = sizeof(event->header) + event->header.length;
	if (count < frame_size) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return -EMSGSIZE;
	}
	list_del(&event->list);
	dev->event_count--;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (copy_to_user(buf, &event->header, sizeof(event->header)) ||
	    copy_to_user(buf + sizeof(event->header), event->payload,
			 event->header.length)) {
		kfree(event);
		return -EFAULT;
	}

	kfree(event);
	return frame_size;
}

static __poll_t vds_poll(struct file *file, poll_table *wait)
{
	struct vds_hcd_dev *dev = vds_from_file(file);
	unsigned long flags;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &dev->read_wq, wait);

	spin_lock_irqsave(&dev->lock, flags);
	if (!list_empty(&dev->events))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (dev->stopping)
		mask |= EPOLLHUP;
	spin_unlock_irqrestore(&dev->lock, flags);

	return mask;
}

static struct vds_input_packet *
vds_pop_input_packet_locked(struct vds_hcd_dev *dev)
{
	struct vds_input_packet *packet;

	if (list_empty(&dev->input_packets))
		return NULL;

	packet = list_first_entry(&dev->input_packets, struct vds_input_packet,
				  list);
	list_del(&packet->list);
	dev->input_packet_count--;
	return packet;
}

static bool vds_queue_urb_giveback_locked(struct vds_hcd_dev *dev,
					  struct vds_urb_context *context,
					  int status)
{
	if (context->state == VDS_URB_PENDING_GIVEBACK ||
	    context->state == VDS_URB_GIVING_BACK)
		return false;

	context->status = status;
	context->state = VDS_URB_PENDING_GIVEBACK;
	if (!list_empty(&context->list))
		list_del_init(&context->list);
	context->urb->hcpriv = context;

	if (!context->ready_time) {
		struct vds_urb_context *pos;

		/*
		 * Control and interrupt URBs must not sit behind future ISO
		 * audio completions. Otherwise requests such as SET_INTERFACE
		 * can time out while an audio stream keeps the timed queue
		 * full.
		 */
		list_for_each_entry(pos, &dev->completed_urbs, list) {
			if (pos->ready_time) {
				list_add_tail(&context->list, &pos->list);
				return true;
			}
		}
	} else {
		struct vds_urb_context *pos;

		list_for_each_entry(pos, &dev->completed_urbs, list) {
			if (!pos->ready_time)
				continue;
			if (ktime_after(pos->ready_time, context->ready_time)) {
				list_add_tail(&context->list, &pos->list);
				return true;
			}
		}
	}

	list_add_tail(&context->list, &dev->completed_urbs);
	return true;
}

static bool vds_cancel_pending_urbs_locked(struct vds_hcd_dev *dev, int status)
{
	struct vds_urb_context *context, *tmp;
	bool wake_giveback = false;

	list_for_each_entry_safe(context, tmp, &dev->pending_hid_in, list)
		wake_giveback |=
			vds_queue_urb_giveback_locked(dev, context, status);
	list_for_each_entry_safe(context, tmp, &dev->pending_feature_get, list)
		wake_giveback |=
			vds_queue_urb_giveback_locked(dev, context, status);
	return wake_giveback;
}

static void vds_flush_buffered_frames_locked(struct vds_hcd_dev *dev)
{
	struct vds_event *event, *event_tmp;
	struct vds_input_packet *packet, *packet_tmp;
	struct vds_audio_packet *audio, *audio_tmp;

	list_for_each_entry_safe(packet, packet_tmp, &dev->input_packets,
				 list) {
		list_del(&packet->list);
		kfree(packet);
	}
	dev->input_packet_count = 0;

	list_for_each_entry_safe(audio, audio_tmp, &dev->audio_in_packets,
				 list) {
		list_del(&audio->list);
		kfree(audio);
	}
	dev->audio_in_packet_count = 0;
	dev->audio_in_queued_bytes = 0;

	list_for_each_entry_safe(event, event_tmp, &dev->events, list) {
		list_del(&event->list);
		kfree(event);
	}
	dev->event_count = 0;
}

static void vds_queue_urb_giveback(struct vds_hcd_dev *dev,
				   struct vds_urb_context *context, int status)
{
	unsigned long flags;
	bool queued;

	spin_lock_irqsave(&dev->lock, flags);
	queued = vds_queue_urb_giveback_locked(dev, context, status);
	spin_unlock_irqrestore(&dev->lock, flags);

	if (queued)
		wake_up_interruptible(&dev->giveback_wq);
}

static int vds_iso_out_urb(struct vds_hcd_dev *dev, struct urb *urb)
{
	u8 *chunk = NULL;
	u32 chunk_len = 0;
	u32 next_offset = 0;
	u32 total = 0;
	int i;

	if (!urb->transfer_buffer)
		return -EPIPE;

	urb->error_count = 0;
	for (i = 0; i < urb->number_of_packets; i++) {
		struct usb_iso_packet_descriptor *desc =
			&urb->iso_frame_desc[i];
		u8 *data;

		if (desc->offset > urb->transfer_buffer_length ||
		    desc->length > urb->transfer_buffer_length - desc->offset) {
			desc->status = -EOVERFLOW;
			desc->actual_length = 0;
			urb->error_count++;
			continue;
		}

		data = (u8 *)urb->transfer_buffer + desc->offset;
		desc->status = 0;
		desc->actual_length = desc->length;
		total += desc->actual_length;
		if (!desc->actual_length)
			continue;

		if (!chunk) {
			chunk = data;
			chunk_len = desc->actual_length;
			next_offset = desc->offset + desc->actual_length;
			continue;
		}

		if (desc->offset == next_offset &&
		    chunk_len + desc->actual_length <= VDS_FRAME_MAX_PAYLOAD) {
			chunk_len += desc->actual_length;
			next_offset += desc->actual_length;
			continue;
		}

		/*
		 * USB audio is realtime. If userspace cannot keep up,
		 * drop the PCM chunk instead of stalling the isochronous pipe.
		 */
		(void)vds_enqueue_frame(dev, VDS_FRAME_USB_AUDIO_OUT, chunk,
					chunk_len, GFP_ATOMIC);
		chunk = data;
		chunk_len = desc->actual_length;
		next_offset = desc->offset + desc->actual_length;
	}

	if (chunk)
		(void)vds_enqueue_frame(dev, VDS_FRAME_USB_AUDIO_OUT, chunk,
					chunk_len, GFP_ATOMIC);

	urb->actual_length = total;
	return 0;
}

static void vds_fill_audio_in_locked(struct vds_hcd_dev *dev, u8 *data,
				     u32 length)
{
	u32 copied = 0;

	while (copied < length && !list_empty(&dev->audio_in_packets)) {
		struct vds_audio_packet *packet;
		u32 available;
		u32 copy_len;

		packet = list_first_entry(&dev->audio_in_packets,
					  struct vds_audio_packet, list);
		available = packet->length - packet->offset;
		copy_len = min(length - copied, available);
		memcpy(data + copied, packet->payload + packet->offset,
		       copy_len);
		copied += copy_len;
		packet->offset += copy_len;
		if (dev->audio_in_queued_bytes > copy_len)
			dev->audio_in_queued_bytes -= copy_len;
		else
			dev->audio_in_queued_bytes = 0;
		if (packet->offset == packet->length) {
			list_del(&packet->list);
			dev->audio_in_packet_count--;
			kfree(packet);
		}
	}

	if (copied < length)
		memset(data + copied, 0, length - copied);
}

static int vds_iso_in_urb(struct vds_hcd_dev *dev, struct urb *urb)
{
	unsigned long flags;
	u32 total = 0;
	int i;

	if (!urb->transfer_buffer)
		return -EPIPE;

	spin_lock_irqsave(&dev->lock, flags);
	urb->error_count = 0;
	for (i = 0; i < urb->number_of_packets; i++) {
		struct usb_iso_packet_descriptor *desc =
			&urb->iso_frame_desc[i];
		u8 *data;

		if (desc->offset > urb->transfer_buffer_length ||
		    desc->length > urb->transfer_buffer_length - desc->offset) {
			desc->status = -EOVERFLOW;
			desc->actual_length = 0;
			urb->error_count++;
			continue;
		}

		data = (u8 *)urb->transfer_buffer + desc->offset;
		desc->actual_length =
			min_t(u32, desc->length, VDS_AUDIO_IN_BYTES_PER_MS);
		vds_fill_audio_in_locked(dev, data, desc->actual_length);
		desc->status = 0;
		total += desc->actual_length;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	urb->actual_length = total;
	return 0;
}

static int vds_giveback_thread(void *data)
{
	struct vds_hcd_dev *dev = data;

	for (;;) {
		struct vds_urb_context *context;
		struct urb *urb;
		int status;
		unsigned long flags;

		wait_event_interruptible(dev->giveback_wq,
					 kthread_should_stop() ||
						 vds_completed_urbs_ready(dev));

		spin_lock_irqsave(&dev->lock, flags);
		if (list_empty(&dev->completed_urbs)) {
			spin_unlock_irqrestore(&dev->lock, flags);
			if (kthread_should_stop())
				break;
			continue;
		}

		context = list_first_entry(&dev->completed_urbs,
					   struct vds_urb_context, list);
		if (context->ready_time) {
			ktime_t now = ktime_get();
			s64 delay_us = ktime_us_delta(context->ready_time, now);

			if (delay_us > 0) {
				ktime_t expires = context->ready_time;

				spin_unlock_irqrestore(&dev->lock, flags);
				set_current_state(TASK_INTERRUPTIBLE);
				if (!kthread_should_stop())
					schedule_hrtimeout_range_clock(&expires,
								       50 * NSEC_PER_USEC,
								       HRTIMER_MODE_ABS,
								       CLOCK_MONOTONIC);
				__set_current_state(TASK_RUNNING);
				continue;
			}
		}
		list_del_init(&context->list);
		urb = context->urb;
		status = context->status;
		context->state = VDS_URB_GIVING_BACK;
		usb_hcd_unlink_urb_from_ep(dev->hcd, urb);
		urb->hcpriv = NULL;
		spin_unlock_irqrestore(&dev->lock, flags);

		if (!status && context->deliver_iso_out)
			status = vds_iso_out_urb(dev, urb);
		if (!status && context->deliver_iso_in)
			status = vds_iso_in_urb(dev, urb);
		usb_hcd_giveback_urb(dev->hcd, urb, status);
		kfree(context);
	}

	return 0;
}

static void vds_fill_hid_in_urb(struct urb *urb, const u8 *payload, u32 length)
{
	u32 copy_len = min_t(u32, length, urb->transfer_buffer_length);

	if (copy_len && urb->transfer_buffer)
		memcpy(urb->transfer_buffer, payload, copy_len);
	urb->actual_length = copy_len;
}

static void vds_schedule_hid_in_giveback_locked(struct vds_hcd_dev *dev,
						struct vds_urb_context *context)
{
	const struct vds_controller_profile *profile =
		vds_usb_device_profile(&dev->usb);
	ktime_t now = ktime_get();
	ktime_t base = ktime_after(dev->next_hid_in_ready, now) ?
			       dev->next_hid_in_ready :
			       now;

	context->ready_time = ktime_add_us(base, profile->hid_in_interval_us);
	dev->next_hid_in_ready = context->ready_time;
}

static bool vds_complete_pending_input_locked(struct vds_hcd_dev *dev,
					      const u8 *payload, u32 length,
					      bool *wake_giveback)
{
	struct vds_urb_context *pending;

	if (list_empty(&dev->pending_hid_in))
		return false;

	pending = list_first_entry(&dev->pending_hid_in, struct vds_urb_context,
				   list);
	vds_fill_hid_in_urb(pending->urb, payload, length);
	vds_schedule_hid_in_giveback_locked(dev, pending);
	*wake_giveback |= vds_queue_urb_giveback_locked(dev, pending, 0);
	return true;
}

static int vds_queue_input_packet_locked(struct vds_hcd_dev *dev,
					 const u8 *payload, u32 length)
{
	struct vds_input_packet *packet;

	if (!list_empty(&dev->input_packets)) {
		/*
		 * HID IN reports are controller state snapshots. If the host
		 * is temporarily not polling, keep the newest state instead of
		 * replaying stale inputs later.
		 */
		packet = list_last_entry(&dev->input_packets,
					 struct vds_input_packet, list);
		packet->length = length;
		memcpy(packet->payload, payload, length);
		return 0;
	}

	packet = kmalloc_obj(*packet, GFP_ATOMIC);
	if (!packet)
		return -ENOMEM;

	packet->length = length;
	memcpy(packet->payload, payload, length);
	list_add_tail(&packet->list, &dev->input_packets);
	dev->input_packet_count++;
	return 0;
}

static int vds_write_input_frame(struct vds_hcd_dev *dev, const u8 *payload,
				 u32 length)
{
	unsigned long flags;
	bool wake_giveback = false;
	int ret = 0;

	if (!length || length > VDS_CONTROLLER_HID_PACKET_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->stopping) {
		ret = -ESHUTDOWN;
	} else if (!vds_complete_pending_input_locked(dev, payload, length,
						      &wake_giveback)) {
		ret = vds_queue_input_packet_locked(dev, payload, length);
	}
	if (!ret)
		dev->frames_from_user++;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (wake_giveback)
		wake_up_interruptible(&dev->giveback_wq);

	return ret;
}

static int vds_fill_feature_urb(struct urb *urb, const u8 *payload, u32 length)
{
	u32 copy_len = min_t(u32, length, urb->transfer_buffer_length);

	if (copy_len && !urb->transfer_buffer)
		return -EPIPE;
	if (copy_len)
		memcpy(urb->transfer_buffer, payload, copy_len);
	urb->actual_length = copy_len;
	return 0;
}

static void vds_complete_pending_feature_replies_locked(struct vds_hcd_dev *dev,
							const u8 *payload,
							u32 length,
							bool *wake_giveback)
{
	struct vds_urb_context *context, *tmp;
	u8 report_id;

	report_id = payload[0];
	list_for_each_entry_safe(context, tmp, &dev->pending_feature_get,
				 list) {
		int status;

		if (context->feature_report_id != report_id)
			continue;

		status = vds_fill_feature_urb(context->urb, payload, length);
		*wake_giveback |=
			vds_queue_urb_giveback_locked(dev, context, status);
	}
}

static int vds_write_feature_reply(struct vds_hcd_dev *dev, const u8 *payload,
				   u32 length)
{
	unsigned long flags;
	bool wake_giveback = false;
	int ret;

	ret = vds_usb_device_update_feature_reply(&dev->usb, payload, length);
	if (ret)
		return ret;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->stopping) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return -ESHUTDOWN;
	}
	vds_complete_pending_feature_replies_locked(dev, payload, length,
						    &wake_giveback);
	dev->frames_from_user++;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (wake_giveback)
		wake_up_interruptible(&dev->giveback_wq);
	return 0;
}

static int vds_write_audio_in_frame(struct vds_hcd_dev *dev, const u8 *payload,
				    u32 length)
{
	struct vds_audio_packet *packet, *oldest;
	unsigned long flags;
	int ret = 0;

	if (!length)
		return -EINVAL;

	packet = kmalloc_obj(*packet, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;

	packet->offset = 0;
	packet->length = length;
	memcpy(packet->payload, payload, length);

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->stopping) {
		ret = -ESHUTDOWN;
	} else {
		while (!list_empty(&dev->audio_in_packets) &&
		       dev->audio_in_queued_bytes + length >
			       VDS_AUDIO_IN_QUEUE_MAX_BYTES) {
			u32 available;

			oldest = list_first_entry(&dev->audio_in_packets,
						  struct vds_audio_packet,
						  list);
			available = oldest->length - oldest->offset;
			list_del(&oldest->list);
			dev->audio_in_packet_count--;
			if (dev->audio_in_queued_bytes > available)
				dev->audio_in_queued_bytes -= available;
			else
				dev->audio_in_queued_bytes = 0;
			kfree(oldest);
		}
		list_add_tail(&packet->list, &dev->audio_in_packets);
		dev->audio_in_packet_count++;
		dev->audio_in_queued_bytes += length;
		dev->frames_from_user++;
		packet = NULL;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	kfree(packet);
	return ret;
}

static ssize_t vds_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct vds_hcd_dev *dev = vds_from_file(file);
	struct vds_frame_header header;
	u8 *payload = NULL;
	int ret = 0;

	(void)ppos;

	if (count < sizeof(header))
		return -EINVAL;
	if (vds_is_stopping(dev))
		return -ESHUTDOWN;
	if (copy_from_user(&header, buf, sizeof(header)))
		return -EFAULT;
	if (header.length > VDS_FRAME_MAX_PAYLOAD ||
	    count != sizeof(header) + header.length)
		return -EINVAL;
	if (header.flags)
		return -EINVAL;
	if (header.type != VDS_FRAME_USB_HID_IN &&
	    header.type != VDS_FRAME_USB_FEATURE_REPLY &&
	    header.type != VDS_FRAME_USB_AUDIO_IN)
		return -EINVAL;

	if (header.length) {
		payload = memdup_user(buf + sizeof(header), header.length);
		if (IS_ERR(payload))
			return PTR_ERR(payload);
	}

	if (header.type == VDS_FRAME_USB_HID_IN)
		ret = vds_write_input_frame(dev, payload, header.length);
	else if (header.type == VDS_FRAME_USB_FEATURE_REPLY)
		ret = vds_write_feature_reply(dev, payload, header.length);
	else if (header.type == VDS_FRAME_USB_AUDIO_IN)
		ret = vds_write_audio_in_frame(dev, payload, header.length);

	kfree(payload);
	return ret ? ret : count;
}

static void vds_set_connection(struct vds_hcd_dev *dev, bool connected)
{
	unsigned long flags;
	bool wake_giveback = false;
	bool reset_usb = false;
	bool was_connected;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->stopping) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}
	was_connected = dev->port_status & USB_PORT_STAT_CONNECTION;
	if (connected) {
		dev->port_status |= USB_PORT_STAT_POWER |
				    USB_PORT_STAT_CONNECTION;
		if (!was_connected)
			dev->port_status |= USB_PORT_STAT_C_CONNECTION << 16;
		vds_status_set_locked(dev, VDS_STATUS_CONNECTED, true);
	} else {
		dev->port_status &=
			~(USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE |
			  USB_PORT_STAT_HIGH_SPEED);
		if (was_connected)
			dev->port_status |= USB_PORT_STAT_C_CONNECTION << 16;
		reset_usb = true;
		wake_giveback = vds_cancel_pending_urbs_locked(dev, -ESHUTDOWN);
		vds_flush_buffered_frames_locked(dev);
		dev->next_iso_out_ready = 0;
		dev->next_iso_in_ready = 0;
		dev->next_hid_in_ready = 0;
		vds_status_set_locked(dev,
				      VDS_STATUS_CONNECTED |
					      VDS_STATUS_CONFIGURED |
					      VDS_STATUS_HID_ENABLED |
					      VDS_STATUS_AUDIO_ENABLED,
				      false);
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (wake_giveback)
		wake_up_interruptible(&dev->giveback_wq);
	if (reset_usb)
		vds_usb_device_reset_state(&dev->usb);

	usb_hcd_resume_root_hub(dev->hcd);
	usb_hcd_poll_rh_status(dev->hcd);
}

static long vds_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vds_hcd_dev *dev = vds_from_file(file);
	struct vds_driver_info driver_info;
	struct vds_profile_config profile;
	struct vds_status status;
	unsigned long flags;
	int ret;

	if (cmd != VDS_IOC_GET_STATUS && cmd != VDS_IOC_GET_DRIVER_INFO &&
	    vds_is_stopping(dev))
		return -ESHUTDOWN;

	switch (cmd) {
	case VDS_IOC_GET_STATUS:
		memset(&status, 0, sizeof(status));
		spin_lock_irqsave(&dev->lock, flags);
		status.status_flags = dev->status_flags;
		status.profile = vds_usb_device_profile_id(&dev->usb);
		status.frames_to_user = dev->frames_to_user;
		status.frames_from_user = dev->frames_from_user;
		spin_unlock_irqrestore(&dev->lock, flags);
		if (copy_to_user((void __user *)arg, &status, sizeof(status)))
			return -EFAULT;
		return 0;
	case VDS_IOC_GET_DRIVER_INFO:
		memset(&driver_info, 0, sizeof(driver_info));
		driver_info.version = VDS_DRIVER_INFO_VERSION;
		driver_info.size = sizeof(driver_info);
		strscpy(driver_info.driver_version, VDS_VERSION,
			sizeof(driver_info.driver_version));
		if (copy_to_user((void __user *)arg, &driver_info,
				 sizeof(driver_info)))
			return -EFAULT;
		return 0;
	case VDS_IOC_SET_PROFILE:
		if (copy_from_user(&profile, (void __user *)arg,
				   sizeof(profile)))
			return -EFAULT;
		if (profile.polling_rate_mode)
			return -EOPNOTSUPP;
		spin_lock_irqsave(&dev->lock, flags);
		if (dev->status_flags & VDS_STATUS_CONNECTED) {
			spin_unlock_irqrestore(&dev->lock, flags);
			return -EBUSY;
		}
		ret = vds_usb_device_set_profile(&dev->usb, profile.profile);
		if (ret) {
			spin_unlock_irqrestore(&dev->lock, flags);
			return ret;
		}
		vds_status_set_locked(dev,
				      VDS_STATUS_CONFIGURED |
					      VDS_STATUS_HID_ENABLED |
					      VDS_STATUS_AUDIO_ENABLED,
				      false);
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	case VDS_IOC_CONNECT:
		vds_set_connection(dev, true);
		return 0;
	case VDS_IOC_DISCONNECT:
		vds_set_connection(dev, false);
		return 0;
	default:
		return -ENOTTY;
	}
}

static int vds_open(struct inode *inode, struct file *file)
{
	struct vds_hcd_dev *dev = vds_from_file(file);
	unsigned long flags;

	(void)inode;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->opened) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return -EBUSY;
	}
	dev->opened = true;
	spin_unlock_irqrestore(&dev->lock, flags);
	return 0;
}

static int vds_release(struct inode *inode, struct file *file)
{
	struct vds_hcd_dev *dev = vds_from_file(file);
	unsigned long flags;
	bool disconnect;

	(void)inode;

	spin_lock_irqsave(&dev->lock, flags);
	disconnect = dev->status_flags & VDS_STATUS_CONNECTED;
	dev->opened = false;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (disconnect)
		vds_set_connection(dev, false);
	return 0;
}

static const struct file_operations vds_fops = {
	.owner = THIS_MODULE,
	.open = vds_open,
	.release = vds_release,
	.read = vds_read,
	.write = vds_write,
	.poll = vds_poll,
	.unlocked_ioctl = vds_ioctl,
	.llseek = noop_llseek,
};

static void vds_schedule_iso_giveback(struct vds_hcd_dev *dev,
				      struct vds_urb_context *context,
				      struct urb *urb, bool input)
{
	unsigned int packets = max_t(unsigned int, urb->number_of_packets, 1);
	unsigned int duration_us = packets * 1000U;
	ktime_t *next_ready;
	ktime_t base;
	ktime_t now;
	u32 total = 0;
	unsigned long flags;
	int i;

	for (i = 0; i < urb->number_of_packets; i++) {
		u32 length = urb->iso_frame_desc[i].length;

		if (input)
			length = min_t(u32, length, VDS_AUDIO_IN_BYTES_PER_MS);
		total += length;
	}
	if (total) {
		/*
		 * ALSA may submit a single descriptor containing multiple
		 * milliseconds of PCM. Pace completion by audio duration, not
		 * only by descriptor count, otherwise userspace receives audio
		 * faster than realtime and the Bluetooth haptics queue underruns
		 * or drops. Both virtual UAC1 streams are 48 kHz S16_LE:
		 * OUT is 4ch (384 bytes/ms), IN is 2ch (192 bytes/ms).
		 */
		u32 bytes_per_ms = input ? VDS_AUDIO_IN_BYTES_PER_MS :
					   VDS_AUDIO_OUT_BYTES_PER_MS;
		u64 duration_by_bytes;

		duration_by_bytes =
			DIV_ROUND_UP_ULL((u64)total * 1000ULL, bytes_per_ms);
		duration_us = max_t(unsigned int,
				    (unsigned int)duration_by_bytes, 1000U);
	}

	now = ktime_get();
	spin_lock_irqsave(&dev->lock, flags);
	next_ready = input ? &dev->next_iso_in_ready : &dev->next_iso_out_ready;
	base = ktime_after(*next_ready, now) ? *next_ready : now;
	urb->start_frame = (int)ktime_to_ms(base);
	/*
	 * Serialize ISO completion by the amount of PCM carried by each URB so
	 * the host audio stack observes a realtime USB sink/source.
	 */
	context->ready_time = ktime_add_us(base, duration_us);
	*next_ready = context->ready_time;
	spin_unlock_irqrestore(&dev->lock, flags);
}

static int vds_interrupt_out_urb(struct vds_hcd_dev *dev, struct urb *urb)
{
	if (!urb->transfer_buffer_length) {
		urb->actual_length = 0;
		return 0;
	}
	if (!urb->transfer_buffer ||
	    urb->transfer_buffer_length > VDS_CONTROLLER_HID_PACKET_SIZE)
		return -EPIPE;

	/*
	 * HID output reports are best-effort here. Userspace keeps the newest
	 * controller state, so backpressure should not stall the interrupt
	 * pipe.
	 */
	(void)vds_enqueue_frame(dev, VDS_FRAME_USB_HID_OUT,
				urb->transfer_buffer,
				urb->transfer_buffer_length, GFP_ATOMIC);
	urb->actual_length = urb->transfer_buffer_length;
	return 0;
}

static int vds_interrupt_in_urb(struct vds_hcd_dev *dev, struct urb *urb,
				struct vds_urb_context *context)
{
	struct vds_input_packet *packet;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	packet = vds_pop_input_packet_locked(dev);
	if (packet) {
		vds_fill_hid_in_urb(urb, packet->payload, packet->length);
		vds_schedule_hid_in_giveback_locked(dev, context);
		kfree(packet);
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	}

	context->state = VDS_URB_PENDING_HID_IN;
	list_add_tail(&context->list, &dev->pending_hid_in);
	spin_unlock_irqrestore(&dev->lock, flags);
	return -EINPROGRESS;
}

static int vds_urb_enqueue(struct usb_hcd *hcd, struct urb *urb,
			   gfp_t mem_flags)
{
	struct vds_hcd_dev *dev = vds_hcd_priv(hcd);
	struct vds_urb_context *context;
	int pipe_type = usb_pipetype(urb->pipe);
	int endpoint = usb_pipeendpoint(urb->pipe);
	unsigned long flags;
	bool timed_iso = false;
	bool iso_input = false;
	int ret;

	context = kmalloc_obj(struct vds_urb_context, mem_flags);
	if (!context)
		return -ENOMEM;
	context->urb = urb;
	context->ready_time = 0;
	context->feature_report_id = 0;
	context->state = VDS_URB_ACTIVE;
	context->deliver_iso_out = false;
	context->deliver_iso_in = false;
	INIT_LIST_HEAD(&context->list);

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->stopping) {
		spin_unlock_irqrestore(&dev->lock, flags);
		kfree(context);
		return -ESHUTDOWN;
	}
	ret = usb_hcd_link_urb_to_ep(hcd, urb);
	if (ret) {
		spin_unlock_irqrestore(&dev->lock, flags);
		kfree(context);
		return ret;
	}
	urb->hcpriv = context;
	spin_unlock_irqrestore(&dev->lock, flags);

	switch (pipe_type) {
	case PIPE_CONTROL:
		ret = vds_usb_device_control_urb(&dev->usb, urb, &vds_usb_ops,
						 dev, GFP_ATOMIC);
		if (ret == -EINPROGRESS)
			return 0;
		break;
	case PIPE_INTERRUPT:
		if (usb_urb_dir_in(urb) &&
		    vds_usb_device_is_hid_in(&dev->usb, endpoint)) {
			ret = vds_interrupt_in_urb(dev, urb, context);
			if (ret == -EINPROGRESS)
				return 0;
		} else if (!usb_urb_dir_in(urb) &&
			   vds_usb_device_is_hid_out(&dev->usb, endpoint)) {
			ret = vds_interrupt_out_urb(dev, urb);
		} else {
			ret = -EPIPE;
		}
		break;
	case PIPE_ISOCHRONOUS:
		if (!usb_urb_dir_in(urb) &&
		    vds_usb_device_is_audio_out(&dev->usb, endpoint)) {
			if (!urb->transfer_buffer) {
				ret = -EPIPE;
			} else {
				ret = 0;
				timed_iso = true;
				context->deliver_iso_out = true;
			}
		} else if (usb_urb_dir_in(urb) &&
			   vds_usb_device_is_audio_in(&dev->usb, endpoint)) {
			if (!urb->transfer_buffer) {
				ret = -EPIPE;
			} else {
				ret = 0;
				timed_iso = true;
				context->deliver_iso_in = true;
			}
			iso_input = true;
		} else {
			ret = -EPIPE;
		}
		break;
	default:
		ret = -EPIPE;
		break;
	}

	if (timed_iso)
		vds_schedule_iso_giveback(dev, context, urb, iso_input);
	vds_queue_urb_giveback(dev, context, ret);
	return 0;
}

static int vds_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct vds_hcd_dev *dev = vds_hcd_priv(hcd);
	struct vds_urb_context *context;
	unsigned long flags;
	bool wake_giveback = false;
	int ret;

	spin_lock_irqsave(&dev->lock, flags);
	ret = usb_hcd_check_unlink_urb(hcd, urb, status);
	if (!ret) {
		context = urb->hcpriv;
		if (context) {
			if (context->state == VDS_URB_PENDING_GIVEBACK) {
				context->status = status;
				context->ready_time = 0;
				wake_giveback = true;
			} else {
				wake_giveback =
					vds_queue_urb_giveback_locked(dev,
								      context,
								      status);
			}
		}
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (ret)
		return ret;
	if (wake_giveback)
		wake_up_interruptible(&dev->giveback_wq);
	return 0;
}

static int vds_get_frame_number(struct usb_hcd *hcd)
{
	return (int)(ktime_get_ns() / NSEC_PER_MSEC);
}

static void vds_hub_descriptor(struct usb_hub_descriptor *desc)
{
	memset(desc, 0, sizeof(*desc));
	desc->bDescriptorType = USB_DT_HUB;
	desc->bDescLength = 9;
	desc->wHubCharacteristics =
		cpu_to_le16(HUB_CHAR_INDV_PORT_LPSM | HUB_CHAR_COMMON_OCPM);
	desc->bNbrPorts = 1;
	desc->u.hs.DeviceRemovable[0] = 0;
	desc->u.hs.DeviceRemovable[1] = 0xff;
}

static int vds_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct vds_hcd_dev *dev = vds_hcd_priv(hcd);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->port_status & PORT_C_MASK) {
		buf[0] = 1 << 1;
		ret = 1;
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	return ret;
}

static int vds_hub_control(struct usb_hcd *hcd, u16 type_req, u16 value,
			   u16 index, char *buf, u16 length)
{
	struct vds_hcd_dev *dev = vds_hcd_priv(hcd);
	unsigned long flags;
	int ret = 0;

	(void)length;

	spin_lock_irqsave(&dev->lock, flags);
	switch (type_req) {
	case ClearHubFeature:
		break;
	case GetHubDescriptor:
		vds_hub_descriptor((struct usb_hub_descriptor *)buf);
		break;
	case GetHubStatus:
		*(__le32 *)buf = cpu_to_le32(0);
		break;
	case GetPortStatus:
		if (index != 1) {
			ret = -EPIPE;
			break;
		}
		((__le16 *)buf)[0] = cpu_to_le16(dev->port_status);
		((__le16 *)buf)[1] = cpu_to_le16(dev->port_status >> 16);
		break;
	case ClearPortFeature:
		if (index != 1) {
			ret = -EPIPE;
			break;
		}
		switch (value) {
		case USB_PORT_FEAT_ENABLE:
			dev->port_status &= ~USB_PORT_STAT_ENABLE;
			break;
		case USB_PORT_FEAT_POWER:
			dev->port_status &= ~USB_PORT_STAT_POWER;
			break;
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:
			dev->port_status &= ~(1 << value);
			break;
		default:
			ret = -EPIPE;
			break;
		}
		break;
	case SetPortFeature:
		if (index != 1) {
			ret = -EPIPE;
			break;
		}
		switch (value) {
		case USB_PORT_FEAT_POWER:
			dev->port_status |= USB_PORT_STAT_POWER;
			break;
		case USB_PORT_FEAT_RESET:
			if (dev->port_status & USB_PORT_STAT_CONNECTION) {
				dev->port_status &= ~USB_PORT_STAT_RESET;
				dev->port_status |=
					USB_PORT_STAT_ENABLE |
					USB_PORT_STAT_HIGH_SPEED |
					(USB_PORT_STAT_C_RESET << 16);
			}
			break;
		default:
			ret = -EPIPE;
			break;
		}
		break;
	default:
		ret = -EPIPE;
		break;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (!ret)
		usb_hcd_poll_rh_status(hcd);
	return ret;
}

static int vds_bus_suspend(struct usb_hcd *hcd)
{
	hcd->state = HC_STATE_SUSPENDED;
	return 0;
}

static int vds_bus_resume(struct usb_hcd *hcd)
{
	hcd->state = HC_STATE_RUNNING;
	return 0;
}

static int vds_hcd_reset(struct usb_hcd *hcd)
{
	hcd->speed = HCD_USB2;
	hcd->self.root_hub->speed = USB_SPEED_HIGH;
	hcd->self.sg_tablesize = 0;
	return 0;
}

static int vds_hcd_start(struct usb_hcd *hcd)
{
	struct vds_hcd_dev *dev = vds_hcd_priv(hcd);
	unsigned long flags;

	hcd->power_budget = 500;
	hcd->state = HC_STATE_RUNNING;
	hcd->uses_new_polling = 1;
	hcd->has_tt = 1;

	spin_lock_irqsave(&dev->lock, flags);
	dev->stopping = false;
	dev->next_iso_out_ready = 0;
	dev->next_iso_in_ready = 0;
	dev->next_hid_in_ready = 0;
	dev->port_status = USB_PORT_STAT_POWER;
	vds_status_set_locked(dev, VDS_STATUS_CONNECTED, false);
	spin_unlock_irqrestore(&dev->lock, flags);
	return 0;
}

static void vds_hcd_stop(struct usb_hcd *hcd)
{
	struct vds_hcd_dev *dev = vds_hcd_priv(hcd);
	unsigned long flags;
	bool wake_giveback = false;

	spin_lock_irqsave(&dev->lock, flags);
	dev->stopping = true;
	wake_giveback = vds_cancel_pending_urbs_locked(dev, -ESHUTDOWN);
	vds_flush_buffered_frames_locked(dev);
	dev->status_flags = 0;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (wake_giveback)
		wake_up_interruptible(&dev->giveback_wq);
	wake_up_interruptible(&dev->read_wq);
}

static const struct hc_driver vds_hc_driver = {
	.description = VDS_HCD_DRIVER_NAME,
	/* Shown as this virtual HCD's product description in sysfs. */
	.product_desc = "Virtual DualSense HCD",
	.hcd_priv_size = sizeof(struct vds_hcd_dev),
	.flags = HCD_USB2,
	.reset = vds_hcd_reset,
	.start = vds_hcd_start,
	.stop = vds_hcd_stop,
	.urb_enqueue = vds_urb_enqueue,
	.urb_dequeue = vds_urb_dequeue,
	.get_frame_number = vds_get_frame_number,
	.hub_status_data = vds_hub_status_data,
	.hub_control = vds_hub_control,
	.bus_suspend = vds_bus_suspend,
	.bus_resume = vds_bus_resume,
};

static int vds_hcd_probe(struct platform_device *pdev)
{
	struct usb_hcd *hcd;
	struct vds_hcd_dev *dev;
	int ret;

	hcd = usb_create_hcd(&vds_hc_driver, &pdev->dev, dev_name(&pdev->dev));
	if (!hcd)
		return -ENOMEM;

	dev = vds_hcd_priv(hcd);
	memset(dev, 0, sizeof(*dev));
	dev->hcd = hcd;
	dev->port_index = pdev->id;
	vds_usb_device_init(&dev->usb, VDS_PROFILE_DS5);
	spin_lock_init(&dev->lock);
	init_waitqueue_head(&dev->read_wq);
	init_waitqueue_head(&dev->giveback_wq);
	INIT_LIST_HEAD(&dev->events);
	INIT_LIST_HEAD(&dev->pending_hid_in);
	INIT_LIST_HEAD(&dev->pending_feature_get);
	INIT_LIST_HEAD(&dev->completed_urbs);
	INIT_LIST_HEAD(&dev->input_packets);
	INIT_LIST_HEAD(&dev->audio_in_packets);

	dev->misc.minor = MISC_DYNAMIC_MINOR;
	snprintf(dev->misc_name, sizeof(dev->misc_name), "vds%u",
		 dev->port_index);
	dev->misc.name = dev->misc_name;
	dev->misc.fops = &vds_fops;
	dev->misc.parent = &pdev->dev;

	platform_set_drvdata(pdev, hcd);

	dev->giveback_thread = kthread_run(vds_giveback_thread, dev,
					   "vds_hcd_gb%u", dev->port_index);
	if (IS_ERR(dev->giveback_thread)) {
		ret = PTR_ERR(dev->giveback_thread);
		dev->giveback_thread = NULL;
		goto put_hcd;
	}

	ret = usb_add_hcd(hcd, 0, 0);
	if (ret)
		goto stop_giveback;

	ret = misc_register(&dev->misc);
	if (ret)
		goto remove_hcd;

	return 0;

remove_hcd:
	usb_remove_hcd(hcd);
stop_giveback:
	kthread_stop(dev->giveback_thread);
put_hcd:
	usb_put_hcd(hcd);
	return ret;
}

static void vds_hcd_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);
	struct vds_hcd_dev *dev = vds_hcd_priv(hcd);

	misc_deregister(&dev->misc);
	usb_remove_hcd(hcd);
	if (dev->giveback_thread)
		kthread_stop(dev->giveback_thread);
	usb_put_hcd(hcd);
}

static struct platform_driver vds_hcd_platform_driver = {
	.probe = vds_hcd_probe,
	.remove = vds_hcd_remove,
	.driver = {
		.name = VDS_HCD_DRIVER_NAME,
	},
};

static int __init vds_hcd_init(void)
{
	struct platform_device *pdev;
	unsigned int i;
	int ret;

	/* Bound per-module virtual controllers to a small, predictable set. */
	if (max_port < VDS_MIN_PORT_COUNT || max_port > VDS_MAX_PORT_COUNT)
		return -EINVAL;

	ret = platform_driver_register(&vds_hcd_platform_driver);
	if (ret)
		return ret;

	vds_hcd_platform_devices =
		kcalloc(max_port, sizeof(*vds_hcd_platform_devices), GFP_KERNEL);
	if (!vds_hcd_platform_devices) {
		ret = -ENOMEM;
		goto unregister_driver;
	}

	for (i = 0; i < max_port; i++) {
		pdev = platform_device_register_simple(VDS_HCD_DRIVER_NAME, i,
						       NULL, 0);
		if (IS_ERR(pdev)) {
			ret = PTR_ERR(pdev);
			goto unregister_devices;
		}
		vds_hcd_platform_devices[i] = pdev;
		vds_hcd_port_count++;
	}

	return 0;

unregister_devices:
	while (vds_hcd_port_count) {
		vds_hcd_port_count--;
		pdev = vds_hcd_platform_devices[vds_hcd_port_count];
		platform_device_unregister(pdev);
	}
	kfree(vds_hcd_platform_devices);
	vds_hcd_platform_devices = NULL;
unregister_driver:
	platform_driver_unregister(&vds_hcd_platform_driver);
	return ret;
}

static void __exit vds_hcd_exit(void)
{
	struct platform_device *pdev;

	while (vds_hcd_port_count) {
		vds_hcd_port_count--;
		pdev = vds_hcd_platform_devices[vds_hcd_port_count];
		platform_device_unregister(pdev);
	}
	kfree(vds_hcd_platform_devices);
	vds_hcd_platform_devices = NULL;
	platform_driver_unregister(&vds_hcd_platform_driver);
}

module_init(vds_hcd_init);
module_exit(vds_hcd_exit);

MODULE_DESCRIPTION("Virtual DualSense host controller");
MODULE_AUTHOR("Jihong Min <hurryman2212@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION(VDS_VERSION);
