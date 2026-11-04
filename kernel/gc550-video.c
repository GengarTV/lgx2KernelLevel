// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * AVerMedia Live Gamer Extreme (GC550) - V4L2 Video Capture & Stream Parser
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Credits & Attribution:
 * - BT.1120 SAV/EAV framing, header checksum formulas, cross-URB straddle
 *   handling, and 16 KiB FX3 DMA swap detection are based on the protocol
 *   reverse-engineering and reference implementation by Chris Sawczuk (ChrisAJS)
 *   and contributors in `lgx2userspace` (https://github.com/ChrisAJS/lgx2userspace).
 *
 * Stream framing protocol on Bulk IN 0x83:
 * - Video frame start (SAV): [00 FF FF C0] + 4-byte header [b0 b1 b2 b3]
 *   Valid iff: b3 == (b0 + b1 + b2 - 0x40) & 0xFF
 * - Video payload: 1920 * 1080 * 2 = 4,147,200 bytes of YUYV (YUY2)
 * - Video frame end (EAV):   [00 FF FF C1] + 5-byte header [c0 c1 c2 c3 c4]
 *   Valid iff: c4 == (c0 + c1 + c2 + c3 - 0x3F) & 0xFF
 * - Audio packet start:      [00 FF FF 58] + 4-byte BE32 length [L3 L2 L1 L0]
 *   Followed by `length` bytes of 16-bit LE stereo PCM @ 48 kHz.
 *
 * Stream resilience features:
 * 1. Cross-URB marker straddle reassembly (`marker_stage` 0..3):
 *    Holds back partial `[00]`, `[00 FF]`, `[00 FF FF]` prefixes across URB
 *    boundaries so split markers never corrupt pixel buffers or miss frames.
 * 2. 16 KiB FX3 ping-pong DMA swap detector & reorderer:
 *    On warm restarts or HDMI signal drops, the FX3's dual 16 KiB DMA buffers
 *    can flip phase (delivering the second 16 KiB half of each 32 KiB pair
 *    first, resulting in an EAV from frame N-1 appearing 18,432 bytes after
 *    SAV(N)). When detected across two consecutive frames, `swap_mode` flips
 *    each 32 KiB pair back into proper order.
 */

#include <linux/slab.h>
#include <linux/unaligned.h>
#include "gc550.h"

#define GC550_PAIR_BUF_SIZE	(2 * GC550_FX3_DMA_BUF_SIZE) /* 32 KiB */
#define GC550_SWAP_SPAN_LIMIT	(32 * 1024)
#define GC550_MAX_FRAME_SPAN	(GC550_FRAME_SIZE + 0x4000)

static const u8 gc550_yuyv_black[4] = { 16, 128, 16, 128 };

void gc550_parser_reset(struct gc550_stream_parser *parser)
{
	parser->state = GC550_STATE_IDLE;
	parser->sav_b0 = 0;
	parser->hdr_pos = 0;
	parser->audio_rem = 0;
	parser->video_pos = 0;
	parser->marker_stage = 0;
	parser->pair_pos = 0;
}

static bool gc550_sav_hdr_valid(const u8 *h)
{
	u8 expected = (u8)((h[0] + h[1] + h[2] - 0x40) & 0xff);

	return h[3] == expected;
}

static bool gc550_eav_hdr_valid(const u8 *h)
{
	u8 expected = (u8)((h[0] + h[1] + h[2] + h[3] - 0x3f) & 0xff);

	return h[4] == expected;
}

static void gc550_append_video_bytes(struct gc550_dev *dev,
				     const u8 *src, size_t len)
{
	struct gc550_stream_parser *p = &dev->parser;
	unsigned long flags;
	void *vbuf;
	size_t room;

	if (!len)
		return;

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (!dev->cur_buf && !list_empty(&dev->buf_list)) {
		dev->cur_buf = list_first_entry(&dev->buf_list,
						struct gc550_buffer, list);
		list_del(&dev->cur_buf->list);
	}

	if (dev->cur_buf && p->video_pos < GC550_FRAME_SIZE) {
		vbuf = vb2_plane_vaddr(&dev->cur_buf->vb.vb2_buf, 0);
		room = GC550_FRAME_SIZE - p->video_pos;
		if (vbuf)
			memcpy((u8 *)vbuf + p->video_pos, src, min(len, room));
	}
	spin_unlock_irqrestore(&dev->queue_lock, flags);

	/* A missing EAV must not wrap the span counter into a valid frame size. */
	if (p->video_pos <= GC550_MAX_FRAME_SPAN)
		p->video_pos = min_t(size_t, (size_t)p->video_pos + len,
				     GC550_MAX_FRAME_SPAN + 1);
}

static void gc550_complete_video_frame(struct gc550_dev *dev, u8 eav_c0)
{
	struct gc550_stream_parser *p = &dev->parser;
	struct gc550_buffer *done_buf = NULL;
	unsigned long flags;
	size_t i;

	/*
	 * Detect 16 KiB FX3 ping-pong DMA swap:
	 * In swapped state, EAV(N-1) arrives ~18,432 bytes after SAV(N) with
	 * eav_c0 != sav_b0. If two consecutive SAV->EAV pairs exhibit this,
	 * toggle swap_mode to un-swap 16 KiB pairs.
	 */
	if (p->video_pos <= GC550_SWAP_SPAN_LIMIT && eav_c0 != p->sav_b0) {
		p->swap_votes++;
		if (p->swap_votes >= 2) {
			p->swap_mode = !p->swap_mode;
			p->swap_votes = 0;
			p->pair_pos = 0;
			dev_info_ratelimited(&dev->intf->dev,
					     "FX3 16 KiB DMA buffer order switched to %s\n",
					     p->swap_mode ? "SWAPPED" : "NORMAL");
		}
		p->video_pos = 0;
		return;
	}

	p->swap_votes = 0;

	/*
	 * Validate span size is within a 16 KiB FX3 DMA quantum of one
	 * complete 1080p YUYV frame (4,147,200 bytes).
	 */
	if (p->video_pos < GC550_FRAME_SIZE - 0x4000 ||
	    p->video_pos > GC550_MAX_FRAME_SPAN) {
		p->video_pos = 0;
		return;
	}

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (dev->cur_buf) {
		done_buf = dev->cur_buf;
		dev->cur_buf = NULL;
	}
	spin_unlock_irqrestore(&dev->queue_lock, flags);

	if (done_buf) {
		if (p->video_pos < GC550_FRAME_SIZE) {
			u8 *vbuf = vb2_plane_vaddr(&done_buf->vb.vb2_buf, 0);

			if (vbuf)
				for (i = p->video_pos; i < GC550_FRAME_SIZE; i++)
					vbuf[i] = gc550_yuyv_black[i & 3];
		}

		done_buf->vb.vb2_buf.timestamp = ktime_get_ns();
		done_buf->vb.field = V4L2_FIELD_NONE;
		done_buf->vb.sequence = p->frame_sequence++;
		vb2_set_plane_payload(&done_buf->vb.vb2_buf, 0,
				      GC550_FRAME_SIZE);
		vb2_buffer_done(&done_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}

	p->video_pos = 0;
}

static bool gc550_is_marker_type(u8 b)
{
	return b == GC550_MARKER_SAV ||
	       b == GC550_MARKER_EAV ||
	       b == GC550_MARKER_AUDIO;
}

static void gc550_handle_marker(struct gc550_dev *dev, u8 marker_type)
{
	struct gc550_stream_parser *p = &dev->parser;

	p->hdr_pos = 0;

	switch (marker_type) {
	case GC550_MARKER_SAV:
		p->state = GC550_STATE_SAV_HEADER;
		break;
	case GC550_MARKER_EAV:
		if (p->state == GC550_STATE_VIDEO_DATA)
			p->state = GC550_STATE_EAV_HEADER;
		else
			p->state = GC550_STATE_IDLE;
		break;
	case GC550_MARKER_AUDIO:
		p->state = GC550_STATE_AUDIO_HEADER;
		break;
	default:
		p->state = GC550_STATE_IDLE;
		break;
	}
}

/**
 * gc550_parse_ordered_bytes - Core byte-stream parser with cross-URB marker straddle support
 */
static void gc550_parse_ordered_bytes(struct gc550_dev *dev,
				      const u8 *data, size_t len)
{
	static const u8 prefix_bytes[3] = { 0x00, 0xff, 0xff };
	struct gc550_stream_parser *p = &dev->parser;
	size_t i = 0;

	while (i < len) {
		switch (p->state) {
		case GC550_STATE_SAV_HEADER:
			p->hdr_buf[p->hdr_pos++] = data[i++];
			if (p->hdr_pos == 4) {
				if (gc550_sav_hdr_valid(p->hdr_buf)) {
					p->sav_b0 = p->hdr_buf[0];
					p->video_pos = 0;
					p->state = GC550_STATE_VIDEO_DATA;
				} else {
					p->desync_count++;
					p->state = GC550_STATE_IDLE;
				}
			}
			continue;

		case GC550_STATE_EAV_HEADER:
			p->hdr_buf[p->hdr_pos++] = data[i++];
			if (p->hdr_pos == 5) {
				if (gc550_eav_hdr_valid(p->hdr_buf)) {
					if (p->hdr_buf[0] != p->sav_b0)
						p->desync_count++;
					gc550_complete_video_frame(dev, p->hdr_buf[0]);
					p->state = GC550_STATE_POST_EAV;
				} else {
					p->desync_count++;
					p->video_pos = 0;
					p->state = GC550_STATE_IDLE;
				}
			}
			continue;

		case GC550_STATE_AUDIO_HEADER:
			p->hdr_buf[p->hdr_pos++] = data[i++];
			if (p->hdr_pos == 4) {
				p->audio_rem = get_unaligned_be32(p->hdr_buf);
				if (p->audio_rem > 0 && p->audio_rem <= 65536)
					p->state = GC550_STATE_AUDIO_DATA;
				else
					p->state = GC550_STATE_IDLE;
			}
			continue;

		case GC550_STATE_AUDIO_DATA: {
			size_t avail = len - i;
			size_t take = min_t(size_t, avail, p->audio_rem);

			gc550_audio_push_samples(dev, &data[i], take);
			i += take;
			p->audio_rem -= take;
			if (p->audio_rem == 0)
				p->state = GC550_STATE_IDLE;
			continue;
		}

		case GC550_STATE_IDLE:
		case GC550_STATE_POST_EAV:
		case GC550_STATE_VIDEO_DATA:
		default:
			break;
		}

		/*
		 * If we are in the middle of a potential [00 FF FF T] marker
		 * (including one that started at the tail of the previous URB),
		 * advance or flush the prefix state machine one byte at a time.
		 */
		if (p->marker_stage > 0) {
			u8 b = data[i];

			if (p->marker_stage == 1) {
				if (b == 0xff) {
					p->marker_stage = 2;
					i++;
					continue;
				}
			} else if (p->marker_stage == 2) {
				if (b == 0xff) {
					p->marker_stage = 3;
					i++;
					continue;
				}
			} else if (p->marker_stage == 3) {
				if (gc550_is_marker_type(b)) {
					p->marker_stage = 0;
					i++;
					gc550_handle_marker(dev, b);
					continue;
				}
			}

			/*
			 * False alarm: the held-back prefix bytes were actually
			 * regular video data. Flush them to the frame buffer.
			 */
			if (p->state == GC550_STATE_VIDEO_DATA)
				gc550_append_video_bytes(dev, prefix_bytes,
							 p->marker_stage);
			p->marker_stage = 0;
			/* Re-evaluate current byte `data[i]` from stage 0 */
		}

		/* Fast path when marker_stage == 0 */
		if (p->state == GC550_STATE_VIDEO_DATA) {
			const u8 *zero = memchr(&data[i], 0x00, len - i);

			if (!zero) {
				gc550_append_video_bytes(dev, &data[i], len - i);
				break;
			}

			if (zero > &data[i]) {
				size_t span = (size_t)(zero - &data[i]);

				gc550_append_video_bytes(dev, &data[i], span);
				i += span;
			}

			/* Now data[i] == 0x00 */
			p->marker_stage = 1;
			i++;
		} else {
			/* IDLE or POST_EAV: scan for 0x00 */
			const u8 *zero = memchr(&data[i], 0x00, len - i);

			if (!zero)
				break;

			i = (size_t)(zero - data) + 1;
			p->marker_stage = 1;
		}
	}
}

/**
 * gc550_parse_stream_chunk - Top-level entry for Bulk IN 0x83 data with 16 KiB swap support
 */
void gc550_parse_stream_chunk(struct gc550_dev *dev, const u8 *data, size_t len)
{
	struct gc550_stream_parser *p = &dev->parser;

	if (!p->swap_mode || !p->pair_buf) {
		gc550_parse_ordered_bytes(dev, data, len);
		return;
	}

	/*
	 * Swapped FX3 DMA mode: accumulate 32 KiB pairs and emit the second
	 * 16 KiB half before the first 16 KiB half.
	 */
	while (len > 0) {
		size_t space = GC550_PAIR_BUF_SIZE - p->pair_pos;
		size_t copy = min(len, space);

		memcpy(p->pair_buf + p->pair_pos, data, copy);
		p->pair_pos += copy;
		data += copy;
		len -= copy;

		if (p->pair_pos == GC550_PAIR_BUF_SIZE) {
			p->pair_pos = 0;
			gc550_parse_ordered_bytes(dev,
						  p->pair_buf + GC550_FX3_DMA_BUF_SIZE,
						  GC550_FX3_DMA_BUF_SIZE);
			gc550_parse_ordered_bytes(dev,
						  p->pair_buf,
						  GC550_FX3_DMA_BUF_SIZE);
		}
	}
}

/* USB Bulk Stream URB Completion Callback */
static void gc550_urb_complete(struct urb *urb)
{
	struct gc550_dev *dev = urb->context;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->stream_lock, flags);
	if (!dev->streaming || dev->disconnected) {
		spin_unlock_irqrestore(&dev->stream_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&dev->stream_lock, flags);

	switch (urb->status) {
	case 0:
		if (urb->actual_length > 0) {
			dev_dbg_ratelimited(&dev->intf->dev,
					    "URB rx: %d bytes (video_pos=%u, state=%d)\n",
					    urb->actual_length,
					    dev->parser.video_pos,
					    dev->parser.state);
			gc550_parse_stream_chunk(dev, urb->transfer_buffer,
						 urb->actual_length);
		}
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	default:
		dev_warn_ratelimited(&dev->intf->dev,
				     "Stream URB status: %d (actual_len=%d)\n",
				     urb->status, urb->actual_length);
		/* Gap / USB transfer error: reset parser state */
		gc550_parser_reset(&dev->parser);
		break;
	}

	/*
	 * Keep the gate lock across callback resubmission. GFP_ATOMIC is required
	 * here; disconnect/STREAMOFF takes the same lock before killing URBs.
	 */
	spin_lock_irqsave(&dev->stream_lock, flags);
	if (dev->streaming && !dev->disconnected)
		ret = usb_submit_urb(urb, GFP_ATOMIC);
	else
		ret = -ESHUTDOWN;
	spin_unlock_irqrestore(&dev->stream_lock, flags);
	if (ret < 0 && ret != -ENODEV && ret != -ESHUTDOWN)
		dev_err_ratelimited(&dev->intf->dev,
				    "Failed to resubmit stream URB: %d\n", ret);
}

static void gc550_free_stream_urbs(struct gc550_dev *dev)
{
	int i;

	for (i = 0; i < GC550_NUM_URBS; i++) {
		if (dev->urbs[i]) {
			usb_kill_urb(dev->urbs[i]);
			usb_free_urb(dev->urbs[i]);
			dev->urbs[i] = NULL;
		}
		kfree(dev->urb_bufs[i]);
		dev->urb_bufs[i] = NULL;
	}

	kfree(dev->parser.pair_buf);
	dev->parser.pair_buf = NULL;
}

static int gc550_alloc_stream_urbs(struct gc550_dev *dev)
{
	int i;

	dev->parser.pair_buf = kmalloc(GC550_PAIR_BUF_SIZE, GFP_KERNEL);
	if (!dev->parser.pair_buf)
		return -ENOMEM;

	for (i = 0; i < GC550_NUM_URBS; i++) {
		dev->urb_bufs[i] = kmalloc(GC550_URB_BUF_SIZE, GFP_KERNEL);
		if (!dev->urb_bufs[i]) {
			gc550_free_stream_urbs(dev);
			return -ENOMEM;
		}

		dev->urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!dev->urbs[i]) {
			gc550_free_stream_urbs(dev);
			return -ENOMEM;
		}

		usb_fill_bulk_urb(dev->urbs[i], dev->udev,
				  usb_rcvbulkpipe(dev->udev, GC550_EP_STREAM_IN),
				  dev->urb_bufs[i], GC550_URB_BUF_SIZE,
				  gc550_urb_complete, dev);
	}

	return 0;
}

static int gc550_submit_stream_urbs(struct gc550_dev *dev)
{
	int i, ret;

	for (i = 0; i < GC550_NUM_URBS; i++) {
		ret = usb_submit_urb(dev->urbs[i], GFP_KERNEL);
		if (ret < 0) {
			dev_err(&dev->intf->dev,
				"Failed to submit stream URB %d: %d\n", i, ret);
			return ret;
		}
	}

	return 0;
}

/* Videobuf2 Queue Operations */
static int gc550_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			     unsigned int *nplanes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct gc550_dev *dev = vb2_get_drv_priv(vq);

	if (*nbuffers < 2)
		*nbuffers = 2;

	if (*nplanes)
		return sizes[0] < dev->pix_fmt.sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = dev->pix_fmt.sizeimage;
	return 0;
}

static int gc550_buf_prepare(struct vb2_buffer *vb)
{
	struct gc550_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size = dev->pix_fmt.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(&dev->intf->dev,
			"Buffer too small (%lu < %lu)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void gc550_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct gc550_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct gc550_buffer *buf = container_of(vbuf, struct gc550_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->queue_lock, flags);
	list_add_tail(&buf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->queue_lock, flags);
}

static void gc550_return_all_buffers(struct gc550_dev *dev,
				     enum vb2_buffer_state state)
{
	struct gc550_buffer *buf, *node;
	LIST_HEAD(done_list);
	unsigned long flags;

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (dev->cur_buf) {
		list_add_tail(&dev->cur_buf->list, &done_list);
		dev->cur_buf = NULL;
	}
	list_splice_tail_init(&dev->buf_list, &done_list);
	spin_unlock_irqrestore(&dev->queue_lock, flags);

	list_for_each_entry_safe(buf, node, &done_list, list) {
		list_del_init(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static void gc550_stop_stream_resources(struct gc550_dev *dev,
					enum vb2_buffer_state state)
{
	unsigned long flags;

	/* The callback resubmit gate is closed before kill waits for callbacks. */
	mutex_lock(&dev->stream_mutex);
	spin_lock_irqsave(&dev->stream_lock, flags);
	dev->streaming = false;
	spin_unlock_irqrestore(&dev->stream_lock, flags);
	gc550_free_stream_urbs(dev);
	mutex_unlock(&dev->stream_mutex);

	gc550_return_all_buffers(dev, state);
}

void gc550_video_stop_device(struct gc550_dev *dev)
{
	gc550_stop_stream_resources(dev, VB2_BUF_STATE_ERROR);
}

static int gc550_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct gc550_dev *dev = vb2_get_drv_priv(vq);
	unsigned long flags;
	int ret;

	gc550_parser_reset(&dev->parser);
	dev->parser.frame_sequence = 0;
	if (READ_ONCE(dev->disconnected)) {
		gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
		return -ENODEV;
	}

	ret = gc550_bootstrap_hw(dev);
	if (ret < 0) {
		gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
		return ret;
	}
	gc550_log_signal_status(dev);

	mutex_lock(&dev->stream_mutex);
	ret = gc550_alloc_stream_urbs(dev);
	if (ret == 0) {
		spin_lock_irqsave(&dev->stream_lock, flags);
		if (dev->disconnected)
			ret = -ENODEV;
		else
			dev->streaming = true;
		spin_unlock_irqrestore(&dev->stream_lock, flags);
	}
	if (ret == 0)
		ret = gc550_submit_stream_urbs(dev);
	if (ret < 0) {
		spin_lock_irqsave(&dev->stream_lock, flags);
		dev->streaming = false;
		spin_unlock_irqrestore(&dev->stream_lock, flags);
		gc550_free_stream_urbs(dev);
	}
	mutex_unlock(&dev->stream_mutex);

	if (ret < 0)
		gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void gc550_stop_streaming(struct vb2_queue *vq)
{
	struct gc550_dev *dev = vb2_get_drv_priv(vq);

	gc550_stop_stream_resources(dev, VB2_BUF_STATE_ERROR);
	dev_info(&dev->intf->dev,
		 "Stream stopped: frames=%llu, desyncs=%u, swap_mode=%d\n",
		 dev->parser.frame_sequence, dev->parser.desync_count,
		 dev->parser.swap_mode);
}

static const struct vb2_ops gc550_vb2_ops = {
	.queue_setup     = gc550_queue_setup,
	.buf_prepare     = gc550_buf_prepare,
	.buf_queue       = gc550_buf_queue,
	.start_streaming = gc550_start_streaming,
	.stop_streaming  = gc550_stop_streaming,
};

/* V4L2 IOCTL Operations */
static void gc550_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct gc550_dev *dev = container_of(v4l2_dev, struct gc550_dev,
					     v4l2_dev);

	/* The V4L2 kref owns this device reference until its final put. */
	kref_put(&dev->ref, gc550_dev_release);
}

static int gc550_vidioc_querycap(struct file *file, void *priv,
				 struct v4l2_capability *cap)
{
	struct gc550_dev *dev = video_drvdata(file);

	strscpy(cap->driver, GC550_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia Live Gamer Extreme (GC550)",
		sizeof(cap->card));
	usb_make_path(dev->udev, cap->bus_info, sizeof(cap->bus_info));
	return 0;
}

static int gc550_vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
					 struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int gc550_vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	struct gc550_dev *dev = video_drvdata(file);

	f->fmt.pix = dev->pix_fmt;
	return 0;
}

static int gc550_vidioc_try_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	f->fmt.pix.width = GC550_DEFAULT_WIDTH;
	f->fmt.pix.height = GC550_DEFAULT_HEIGHT;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = GC550_DEFAULT_WIDTH * GC550_BYTES_PER_PIXEL;
	f->fmt.pix.sizeimage = GC550_FRAME_SIZE;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
	f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
	f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	f->fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
	return 0;
}

static int gc550_vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (vb2_is_busy(&dev->vb_queue))
		return -EBUSY;

	gc550_vidioc_try_fmt_vid_cap(file, priv, f);
	dev->pix_fmt = f->fmt.pix;
	return 0;
}

static int gc550_vidioc_enum_framesizes(struct file *file, void *priv,
					struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index > 0 || fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = GC550_DEFAULT_WIDTH;
	fsize->discrete.height = GC550_DEFAULT_HEIGHT;
	return 0;
}

static int gc550_vidioc_enum_frameintervals(struct file *file, void *priv,
					    struct v4l2_frmivalenum *fival)
{
	if (fival->index > 0 ||
	    fival->pixel_format != V4L2_PIX_FMT_YUYV ||
	    fival->width != GC550_DEFAULT_WIDTH ||
	    fival->height != GC550_DEFAULT_HEIGHT)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = 1;
	fival->discrete.denominator = 60;
	return 0;
}

static int gc550_vidioc_enum_input(struct file *file, void *priv,
				   struct v4l2_input *inp)
{
	if (inp->index > 0)
		return -EINVAL;

	strscpy(inp->name, "HDMI", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int gc550_vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int gc550_vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	return (i == 0) ? 0 : -EINVAL;
}

static int gc550_vidioc_g_parm(struct file *file, void *priv,
			       struct v4l2_streamparm *sp)
{
	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	sp->parm.capture.readbuffers = 2;
	sp->parm.capture.timeperframe.numerator = 1;
	sp->parm.capture.timeperframe.denominator = 60;
	return 0;
}

static int gc550_vidioc_s_parm(struct file *file, void *priv,
			       struct v4l2_streamparm *sp)
{
	return gc550_vidioc_g_parm(file, priv, sp);
}

static const struct v4l2_ioctl_ops gc550_ioctl_ops = {
	.vidioc_querycap            = gc550_vidioc_querycap,
	.vidioc_enum_fmt_vid_cap    = gc550_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap       = gc550_vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap     = gc550_vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap       = gc550_vidioc_s_fmt_vid_cap,
	.vidioc_enum_framesizes     = gc550_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = gc550_vidioc_enum_frameintervals,
	.vidioc_enum_input          = gc550_vidioc_enum_input,
	.vidioc_g_input             = gc550_vidioc_g_input,
	.vidioc_s_input             = gc550_vidioc_s_input,
	.vidioc_g_parm              = gc550_vidioc_g_parm,
	.vidioc_s_parm              = gc550_vidioc_s_parm,

	.vidioc_reqbufs             = vb2_ioctl_reqbufs,
	.vidioc_create_bufs         = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf         = vb2_ioctl_prepare_buf,
	.vidioc_querybuf            = vb2_ioctl_querybuf,
	.vidioc_qbuf                = vb2_ioctl_qbuf,
	.vidioc_dqbuf               = vb2_ioctl_dqbuf,
	.vidioc_expbuf              = vb2_ioctl_expbuf,
	.vidioc_streamon            = vb2_ioctl_streamon,
	.vidioc_streamoff           = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations gc550_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap,
};

int gc550_video_register(struct gc550_dev *dev)
{
	struct vb2_queue *vq = &dev->vb_queue;
	struct video_device *vdev = &dev->vdev;
	int ret;

	INIT_LIST_HEAD(&dev->buf_list);
	spin_lock_init(&dev->queue_lock);

	/* Initialize default 1080p60 YUYV format */
	dev->pix_fmt.width = GC550_DEFAULT_WIDTH;
	dev->pix_fmt.height = GC550_DEFAULT_HEIGHT;
	dev->pix_fmt.pixelformat = V4L2_PIX_FMT_YUYV;
	dev->pix_fmt.field = V4L2_FIELD_NONE;
	dev->pix_fmt.bytesperline = GC550_DEFAULT_WIDTH * GC550_BYTES_PER_PIXEL;
	dev->pix_fmt.sizeimage = GC550_FRAME_SIZE;
	dev->pix_fmt.colorspace = V4L2_COLORSPACE_REC709;
	dev->pix_fmt.ycbcr_enc = V4L2_YCBCR_ENC_709;
	dev->pix_fmt.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	dev->pix_fmt.xfer_func = V4L2_XFER_FUNC_709;

	dev->v4l2_dev.release = gc550_v4l2_release;
	ret = v4l2_device_register(&dev->intf->dev, &dev->v4l2_dev);
	if (ret < 0)
		return ret;
	kref_get(&dev->ref);
	dev->v4l2_registered = true;
	dev->v4l2_ref_held = true;

	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	vq->drv_priv = dev;
	vq->buf_struct_size = sizeof(struct gc550_buffer);
	vq->ops = &gc550_vb2_ops;
	vq->mem_ops = &vb2_vmalloc_memops;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vq->min_queued_buffers = 2;
	vq->lock = &dev->v4l2_lock;

	ret = vb2_queue_init(vq);
	if (ret < 0) {
		gc550_video_unregister(dev);
		return ret;
	}

	strscpy(vdev->name, "AVerMedia GC550", sizeof(vdev->name));
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->fops = &gc550_fops;
	vdev->ioctl_ops = &gc550_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->queue = vq;
	vdev->lock = &dev->v4l2_lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE |
			    V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		gc550_video_unregister(dev);
		return ret;
	}

	return 0;
}

void gc550_video_unregister(struct gc550_dev *dev)
{
	if (video_is_registered(&dev->vdev))
		video_unregister_device(&dev->vdev);
	if (dev->v4l2_registered) {
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->v4l2_registered = false;
	}
	if (dev->v4l2_ref_held) {
		dev->v4l2_ref_held = false;
		v4l2_device_put(&dev->v4l2_dev);
	}
}
