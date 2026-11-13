// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) - Passive Diagnostic Telemetry (Stage 1)
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Design Specification: E1.9 Passive Diagnostic Instrumentation (Revision 4)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include "gc550.h"
#include "gc550-diag.h"

static struct dentry *gc550_diag_root;
static DEFINE_MUTEX(gc550_diag_root_lock);
static unsigned int gc550_diag_device_count;

struct gc550_diag_reader {
	struct gc550_dev *dev;
	struct gc550_diag_record current_rec;
	u64 current_seq;
	u64 reader_generation;
	u64 record_generation;
	u64 overrun_lost;
	bool is_synthetic_reset;
};

int gc550_diag_init(struct gc550_dev *dev)
{
	struct gc550_diag_ring *ring = &dev->diag_ring;
	int i;

	raw_spin_lock_init(&ring->lock);
	ring->capacity = GC550_DIAG_RING_CAPACITY;
	ring->head = 0;
	ring->ring_overwritten_slots = 0;
	ring->reset_generation = 0;
	ring->enabled = false;
	ring->records = NULL;

	ring->records = vzalloc(array_size(ring->capacity,
					   sizeof(struct gc550_diag_record)));
	if (!ring->records)
		return -ENOMEM;

	atomic_set(&dev->diag_concurrency.active_callbacks, 0);
	atomic_set(&dev->diag_concurrency.max_simultaneous, 0);
	atomic64_set(&dev->diag_concurrency.callback_entry_seq, 0);
	atomic64_set(&dev->diag_concurrency.callback_exit_seq, 0);
	atomic64_set(&dev->diag_submission_counter, 0);
	dev->diag_session_epoch = 0;

	for (i = 0; i < GC550_NUM_URBS; i++) {
		dev->urb_ctx[i].dev = dev;
		dev->urb_ctx[i].slot_index = (u16)i;
		dev->urb_ctx[i].submission_attempt_id = 0;
	}

	return 0;
}

void gc550_diag_free(struct gc550_dev *dev)
{
	struct gc550_diag_ring *ring = &dev->diag_ring;
	struct gc550_diag_record *recs;
	unsigned long flags;

	raw_spin_lock_irqsave(&ring->lock, flags);
	WRITE_ONCE(ring->enabled, false);
	recs = ring->records;
	ring->records = NULL;
	raw_spin_unlock_irqrestore(&ring->lock, flags);

	vfree(recs);
}

void gc550_diag_emit(struct gc550_dev *dev, const struct gc550_diag_record *rec)
{
	struct gc550_diag_ring *ring = &dev->diag_ring;
	unsigned long flags;
	u32 idx;

	/* Fast-path check: avoid spinlock acquisition when telemetry is disabled */
	if (!READ_ONCE(ring->enabled))
		return;

	raw_spin_lock_irqsave(&ring->lock, flags);
	if (unlikely(!ring->enabled || !ring->records)) {
		raw_spin_unlock_irqrestore(&ring->lock, flags);
		return;
	}

	idx = (u32)(ring->head & (ring->capacity - 1));
	ring->records[idx] = *rec;
	ring->head++;

	if (ring->head > ring->capacity)
		ring->ring_overwritten_slots = ring->head - ring->capacity;
	raw_spin_unlock_irqrestore(&ring->lock, flags);
}

void gc550_diag_reset(struct gc550_dev *dev)
{
	struct gc550_diag_ring *ring = &dev->diag_ring;
	unsigned long flags;

	raw_spin_lock_irqsave(&ring->lock, flags);
	ring->head = 0;
	ring->ring_overwritten_slots = 0;
	ring->reset_generation++;
	raw_spin_unlock_irqrestore(&ring->lock, flags);

	atomic_set(&dev->diag_concurrency.max_simultaneous, 0);
}

/* Stage 2A Passive Transport Telemetry Helpers */

void gc550_diag_emit_urb_complete(struct gc550_dev *dev,
				  const struct gc550_urb_ctx *ctx,
				  const struct urb *urb,
				  u64 entry_seq, int active,
				  u64 timestamp_ns, u16 cpu_id)
{
	struct gc550_diag_record rec;
	u16 flags = 0;
	int status;
	u32 actual_length;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	status = urb ? urb->status : 0;
	actual_length = urb ? urb->actual_length : 0;

	switch (status) {
	case 0:
		if (actual_length == 0)
			flags |= GC550_FLAG_ZERO_LENGTH;
		else if (urb && actual_length < urb->transfer_buffer_length)
			flags |= GC550_FLAG_SHORT_TRANSFER;
		break;
	case -ENOENT:
	case -ECONNRESET:
		flags |= GC550_FLAG_URB_CANCELLED;
		if (dev->streaming)
			flags |= GC550_FLAG_ABNORMAL_ABORT;
		else
			flags |= GC550_FLAG_LIFECYCLE_STOP;
		if (actual_length > 0)
			flags |= GC550_FLAG_PARTIAL_PAYLOAD_DISCARDED;
		else
			flags |= GC550_FLAG_ZERO_LENGTH;
		break;
	case -ESHUTDOWN:
		flags |= GC550_FLAG_DEVICE_DISCONNECTED;
		if (actual_length > 0)
			flags |= GC550_FLAG_PARTIAL_PAYLOAD_DISCARDED;
		else
			flags |= GC550_FLAG_ZERO_LENGTH;
		break;
	default:
		flags |= GC550_FLAG_TRANSFER_ERROR;
		if (actual_length > 0)
			flags |= GC550_FLAG_PARTIAL_PAYLOAD_DISCARDED;
		else
			flags |= GC550_FLAG_ZERO_LENGTH;
		break;
	}

	rec.timestamp_ns = timestamp_ns;
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = 0;
	rec.submission_generation = ctx ? ctx->submission_attempt_id : 0;
	rec.completion_seq = entry_seq;
	rec.urb_rel_offset = 0;
	rec.distance_from_prev_sav = U32_MAX;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = actual_length;
	rec.pair_pos = 0;
	rec.cpu_id = cpu_id;
	rec.urb_status = (s16)status;
	rec.event_type = GC550_EVT_URB_COMPLETE;
	rec.slot_index = ctx ? (u8)ctx->slot_index : 0;
	rec.marker_type = 0;
	rec.marker_seq = 0;
	rec.parser_state = 0;
	rec.swap_mode = 0;
	rec.swap_votes = 0;
	rec.active_callbacks = (u8)active;
	rec.flags = flags;
	rec.parser_flags = 0;
	memset(rec.reserved, 0, sizeof(rec.reserved));

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_submission_failure(struct gc550_dev *dev,
				       u16 slot_index, u64 sub_id,
				       int err)
{
	struct gc550_diag_record rec;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = 0;
	rec.submission_generation = sub_id;
	rec.completion_seq = 0;
	rec.urb_rel_offset = 0;
	rec.distance_from_prev_sav = U32_MAX;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = 0;
	rec.pair_pos = 0;
	rec.cpu_id = (u16)raw_smp_processor_id();
	rec.urb_status = (s16)err;
	rec.event_type = GC550_EVT_SUBMISSION_FAILURE;
	rec.slot_index = (u8)slot_index;
	rec.marker_type = 0;
	rec.marker_seq = 0;
	rec.parser_state = 0;
	rec.swap_mode = 0;
	rec.swap_votes = 0;
	rec.active_callbacks = (u8)atomic_read(&dev->diag_concurrency.active_callbacks);
	rec.flags = GC550_FLAG_SUBMISSION_FAILURE;
	rec.parser_flags = 0;
	memset(rec.reserved, 0, sizeof(rec.reserved));

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_lifecycle(struct gc550_dev *dev, enum gc550_diag_event evt)
{
	struct gc550_diag_record rec;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = 0;
	rec.submission_generation = 0;
	rec.completion_seq = 0;
	rec.urb_rel_offset = 0;
	rec.distance_from_prev_sav = U32_MAX;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = 0;
	rec.pair_pos = 0;
	rec.cpu_id = (u16)raw_smp_processor_id();
	rec.urb_status = 0;
	rec.event_type = (u8)evt;
	rec.slot_index = 0;
	rec.marker_type = 0;
	rec.marker_seq = 0;
	rec.parser_state = 0;
	rec.swap_mode = 0;
	rec.swap_votes = 0;
	rec.active_callbacks = (u8)atomic_read(&dev->diag_concurrency.active_callbacks);
	rec.flags = (evt == GC550_EVT_STREAM_STOP_BEGIN || evt == GC550_EVT_STREAM_STOP_COMPLETE) ?
		    GC550_FLAG_LIFECYCLE_STOP : 0;
	rec.parser_flags = 0;
	memset(rec.reserved, 0, sizeof(rec.reserved));

	gc550_diag_emit(dev, &rec);
}

/* Stage 2B Parser Telemetry Helpers */

void gc550_diag_emit_marker_sav(struct gc550_dev *dev,
				const struct gc550_dispatch_ctx *dctx,
				const u8 *hdr, bool valid, bool is_pair_buf)
{
	struct gc550_diag_record rec;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = dev->parser.current_frame_sequence;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = 0;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = (u32)dev->pix_fmt.sizeimage;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = 0;
	rec.event_type = GC550_EVT_SAV_DETECTED;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = GC550_MARKER_SAV;
	rec.marker_seq = hdr ? hdr[0] : 0;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = dev->parser.swap_mode ? 1 : 0;
	rec.swap_votes = dev->parser.swap_votes;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;
	rec.parser_flags = (valid ? GC550_PFLAG_SAV_CHECKSUM_VALID : GC550_PFLAG_SAV_CHECKSUM_INVALID) |
			   (is_pair_buf ? GC550_PFLAG_PATH_PAIR_BUF : GC550_PFLAG_PATH_DIRECT_URB) |
			   GC550_PFLAG_PROV_UNKNOWN;
	memset(rec.reserved, 0, sizeof(rec.reserved));
	if (hdr) {
		rec.reserved[0] = hdr[3];
		rec.reserved[1] = (u8)((hdr[0] + hdr[1] + hdr[2] - 0x40) & 0xff);
	}

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_marker_eav(struct gc550_dev *dev,
				const struct gc550_dispatch_ctx *dctx,
				const u8 *hdr, bool valid, bool is_pair_buf)
{
	struct gc550_diag_record rec;
	u16 pflags;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = dev->parser.current_frame_sequence;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = (u32)dev->parser.video_pos;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = (u32)dev->pix_fmt.sizeimage;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = 0;
	rec.event_type = GC550_EVT_EAV_DETECTED;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = GC550_MARKER_EAV;
	rec.marker_seq = hdr ? hdr[0] : 0;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = dev->parser.swap_mode ? 1 : 0;
	rec.swap_votes = dev->parser.swap_votes;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;

	pflags = (valid ? GC550_PFLAG_EAV_CHECKSUM_VALID : GC550_PFLAG_EAV_CHECKSUM_INVALID);
	if (hdr && hdr[0] == dev->parser.sav_b0)
		pflags |= GC550_PFLAG_SEQ_MATCH;
	else
		pflags |= GC550_PFLAG_SEQ_MISMATCH;
	pflags |= (is_pair_buf ? GC550_PFLAG_PATH_PAIR_BUF : GC550_PFLAG_PATH_DIRECT_URB) |
		  GC550_PFLAG_PROV_UNKNOWN;
	rec.parser_flags = pflags;

	memset(rec.reserved, 0, sizeof(rec.reserved));
	rec.reserved[0] = dev->parser.sav_b0;
	if (hdr) {
		rec.reserved[1] = hdr[4];
		rec.reserved[2] = (u8)((hdr[0] + hdr[1] + hdr[2] + hdr[3] - 0x3f) & 0xff);
	}

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_audio_header(struct gc550_dev *dev,
				  const struct gc550_dispatch_ctx *dctx,
				  const u8 *hdr, u32 audio_rem, bool is_pair_buf)
{
	struct gc550_diag_record rec;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = 0;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = (u32)dev->parser.video_pos;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = audio_rem;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = 0;
	rec.event_type = GC550_EVT_AUDIO_HEADER;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = GC550_MARKER_AUDIO;
	rec.marker_seq = 0;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = dev->parser.swap_mode ? 1 : 0;
	rec.swap_votes = dev->parser.swap_votes;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;
	rec.parser_flags = (is_pair_buf ? GC550_PFLAG_PATH_PAIR_BUF : GC550_PFLAG_PATH_DIRECT_URB) |
			   GC550_PFLAG_PROV_UNKNOWN;
	memset(rec.reserved, 0, sizeof(rec.reserved));
	rec.reserved[0] = (u8)((audio_rem >> 16) & 0xff);

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_swap_vote(struct gc550_dev *dev,
			       const struct gc550_dispatch_ctx *dctx,
			       u8 sav_b0, u8 eav_c0, u8 votes, bool is_pair_buf)
{
	struct gc550_diag_record rec;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = 0;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = (u32)dev->parser.video_pos;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = GC550_SWAP_SPAN_LIMIT;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = (s16)(dev->parser.swap_mode ? 1 : 0);
	rec.event_type = GC550_EVT_SWAP_VOTE;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = GC550_MARKER_EAV;
	rec.marker_seq = eav_c0;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = dev->parser.swap_mode ? 1 : 0;
	rec.swap_votes = votes;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;
	rec.parser_flags = (is_pair_buf ? GC550_PFLAG_PATH_PAIR_BUF : GC550_PFLAG_PATH_DIRECT_URB) |
			   GC550_PFLAG_PROV_UNKNOWN;
	memset(rec.reserved, 0, sizeof(rec.reserved));
	rec.reserved[0] = sav_b0;

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_swap_toggle(struct gc550_dev *dev,
				 const struct gc550_dispatch_ctx *dctx,
				 bool old_mode, bool new_mode, bool is_pair_buf)
{
	struct gc550_diag_record rec;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = 0;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = (u32)dev->parser.video_pos;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = GC550_SWAP_SPAN_LIMIT;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = (s16)(old_mode ? 1 : 0);
	rec.event_type = GC550_EVT_SWAP_TOGGLE;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = GC550_MARKER_EAV;
	rec.marker_seq = 0;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = new_mode ? 1 : 0;
	rec.swap_votes = 0;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;
	rec.parser_flags = (is_pair_buf ? GC550_PFLAG_PATH_PAIR_BUF : GC550_PFLAG_PATH_DIRECT_URB) |
			   GC550_PFLAG_PROV_UNKNOWN;
	memset(rec.reserved, 0, sizeof(rec.reserved));
	rec.reserved[0] = dev->parser.sav_b0;

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_desync(struct gc550_dev *dev,
			    const struct gc550_dispatch_ctx *dctx,
			    u16 reason, u8 marker_type, u8 marker_seq,
			    bool is_pair_buf)
{
	struct gc550_diag_record rec;
	u16 pflags;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = dev->parser.desync_count;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = (u32)dev->parser.video_pos;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = (u32)dev->pix_fmt.sizeimage;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = (s16)reason;
	rec.event_type = GC550_EVT_PARSER_DESYNC;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = marker_type;
	rec.marker_seq = marker_seq;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = dev->parser.swap_mode ? 1 : 0;
	rec.swap_votes = dev->parser.swap_votes;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;

	pflags = (is_pair_buf ? GC550_PFLAG_PATH_PAIR_BUF : GC550_PFLAG_PATH_DIRECT_URB) |
		 GC550_PFLAG_PROV_UNKNOWN;
	if (reason == GC550_DESYNC_SAV_CHECKSUM)
		pflags |= GC550_PFLAG_SAV_CHECKSUM_INVALID;
	else if (reason == GC550_DESYNC_SEQ_MISMATCH)
		pflags |= GC550_PFLAG_SEQ_MISMATCH;
	else if (reason == GC550_DESYNC_EAV_CHECKSUM)
		pflags |= GC550_PFLAG_EAV_CHECKSUM_INVALID;
	rec.parser_flags = pflags;

	memset(rec.reserved, 0, sizeof(rec.reserved));
	rec.reserved[0] = dev->parser.sav_b0;
	if (reason == GC550_DESYNC_SEQ_MISMATCH)
		rec.reserved[1] = dev->parser.sav_b0;

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_frame_complete(struct gc550_dev *dev,
				    const struct gc550_dispatch_ctx *dctx,
				    struct gc550_buffer *buf,
				    enum vb2_buffer_state state,
				    u8 term_reason, bool is_pair_buf)
{
	struct gc550_diag_record rec;
	size_t frame_size;
	bool valid_span;
	u16 pflags;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	frame_size = dev->pix_fmt.sizeimage;
	valid_span = (dev->parser.video_pos >= frame_size - GC550_FRAME_SPAN_TOLERANCE &&
		      dev->parser.video_pos <= frame_size + GC550_FRAME_SPAN_TOLERANCE);

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = dev->parser.current_frame_sequence;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = (u32)dev->parser.video_pos;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = (u32)frame_size;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = (s16)state;
	rec.event_type = GC550_EVT_FRAME_COMPLETE;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = 0;
	rec.marker_seq = dev->parser.sav_b0;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = dev->parser.swap_mode ? 1 : 0;
	rec.swap_votes = dev->parser.swap_votes;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;

	pflags = (state == VB2_BUF_STATE_DONE) ? GC550_PFLAG_FRAME_DONE : GC550_PFLAG_FRAME_ERROR;
	if (valid_span)
		pflags |= GC550_PFLAG_SPAN_VALID;
	else
		pflags |= GC550_PFLAG_SPAN_INVALID;
	if (term_reason == GC550_TERM_UNCLOSED_SAV_ABORT)
		pflags |= GC550_PFLAG_UNCLOSED_FRAME_ABORT;
	pflags |= (is_pair_buf ? GC550_PFLAG_PATH_PAIR_BUF : GC550_PFLAG_PATH_DIRECT_URB) |
		  GC550_PFLAG_PROV_UNKNOWN;
	rec.parser_flags = pflags;

	memset(rec.reserved, 0, sizeof(rec.reserved));
	rec.reserved[0] = term_reason;

	gc550_diag_emit(dev, &rec);
}

void gc550_diag_emit_parser_reset(struct gc550_dev *dev,
				  const struct gc550_dispatch_ctx *dctx,
				  u16 reason)
{
	struct gc550_diag_record rec;

	if (!READ_ONCE(dev->diag_ring.enabled))
		return;

	rec.timestamp_ns = ktime_get_raw_ns();
	rec.endpoint_payload_offset = 0;
	rec.secondary_payload_offset = dev->parser.frame_sequence;
	rec.submission_generation = dctx ? dctx->submission_generation : 0;
	rec.completion_seq = dctx ? dctx->completion_seq : 0;
	rec.urb_rel_offset = U32_MAX;
	rec.distance_from_prev_sav = (u32)dev->parser.video_pos;
	rec.segment_id = dev->diag_session_epoch;
	rec.payload_len = 0;
	rec.pair_pos = dev->parser.pair_pos;
	rec.cpu_id = dctx ? dctx->cpu_id : (u16)raw_smp_processor_id();
	rec.urb_status = (s16)reason;
	rec.event_type = GC550_EVT_PARSER_RESET;
	rec.slot_index = dctx ? (u8)dctx->slot_index : 0xFF;
	rec.marker_type = 0;
	rec.marker_seq = 0;
	rec.parser_state = dev->parser.state;
	rec.swap_mode = dev->parser.swap_mode ? 1 : 0;
	rec.swap_votes = dev->parser.swap_votes;
	rec.active_callbacks = dctx ? dctx->active_callbacks : 0;
	rec.flags = 0;
	rec.parser_flags = GC550_PFLAG_PROV_UNKNOWN;
	memset(rec.reserved, 0, sizeof(rec.reserved));
	rec.reserved[0] = (u8)reason;

	gc550_diag_emit(dev, &rec);
}

/* Debugfs seq_file iterator implementation */

static void *gc550_diag_seq_start(struct seq_file *m, loff_t *pos)
{
	struct gc550_diag_reader *reader = m->private;
	struct gc550_diag_ring *ring = &reader->dev->diag_ring;
	unsigned long flags;
	u64 oldest_available, cur_seq, cur_gen;

	raw_spin_lock_irqsave(&ring->lock, flags);
	if (!ring->records) {
		raw_spin_unlock_irqrestore(&ring->lock, flags);
		return NULL;
	}

	cur_gen = ring->reset_generation;

	if (reader->reader_generation != cur_gen) {
		/*
		 * Generation transition detected.
		 * Emit a standalone reset notification token.
		 * Do NOT advance reader_generation or mutate *pos yet so that
		 * any seq_file buffer-reallocation retry remains 100% idempotent.
		 */
		reader->is_synthetic_reset = true;
		raw_spin_unlock_irqrestore(&ring->lock, flags);
		return reader;
	}

	/* Same generation: normal sequence position checking */
	reader->is_synthetic_reset = false;
	oldest_available = (ring->head > ring->capacity)
			   ? (ring->head - ring->capacity)
			   : 0;

	cur_seq = (u64)*pos;
	if (cur_seq < oldest_available) {
		reader->overrun_lost = oldest_available - cur_seq;
		cur_seq = oldest_available;
	} else {
		reader->overrun_lost = 0;
	}

	if (cur_seq >= ring->head) {
		raw_spin_unlock_irqrestore(&ring->lock, flags);
		return NULL;
	}

	reader->current_seq = cur_seq;
	reader->record_generation = cur_gen;
	reader->current_rec = ring->records[cur_seq & (ring->capacity - 1)];
	raw_spin_unlock_irqrestore(&ring->lock, flags);

	return &reader->current_rec;
}

static void *gc550_diag_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct gc550_diag_reader *reader = m->private;
	struct gc550_diag_ring *ring = &reader->dev->diag_ring;
	unsigned long flags;
	u64 oldest_available, cur_seq, cur_gen;

	raw_spin_lock_irqsave(&ring->lock, flags);
	if (!ring->records) {
		raw_spin_unlock_irqrestore(&ring->lock, flags);
		return NULL;
	}

	cur_gen = ring->reset_generation;

	if (v == reader || reader->is_synthetic_reset) {
		/*
		 * The reset notification was successfully emitted by show().
		 * Consume the reset: adopt the new generation.
		 */
		reader->reader_generation = cur_gen;
		reader->is_synthetic_reset = false;

		/* If the new generation has no records yet, terminate iteration cleanly */
		if (ring->head == 0) {
			(*pos)++;
			raw_spin_unlock_irqrestore(&ring->lock, flags);
			return NULL;
		}

		oldest_available = (ring->head > ring->capacity)
				   ? (ring->head - ring->capacity)
				   : 0;

		cur_seq = oldest_available;
		reader->overrun_lost = (ring->head > ring->capacity) ? oldest_available : 0;
		*pos = (loff_t)cur_seq;
	} else {
		/*
		 * Normal record progression.
		 * First, check if a reset occurred between show() and next().
		 */
		if (reader->record_generation != cur_gen) {
			/*
			 * Generation changed while active!
			 * Return NULL to finish current seq_file buffer at the
			 * generation boundary. The next read syscall will enter
			 * start(), detect reader_generation != cur_gen, and emit
			 * the reset notification.
			 * Never rewind *pos within this active iteration.
			 */
			raw_spin_unlock_irqrestore(&ring->lock, flags);
			return NULL;
		}

		/* Same generation: advance *pos monotonically */
		*pos = (loff_t)(reader->current_seq + 1);

		oldest_available = (ring->head > ring->capacity)
				   ? (ring->head - ring->capacity)
				   : 0;

		cur_seq = (u64)*pos;
		if (cur_seq < oldest_available) {
			reader->overrun_lost = oldest_available - cur_seq;
			cur_seq = oldest_available;
		} else {
			reader->overrun_lost = 0;
		}
	}

	if (cur_seq >= ring->head) {
		raw_spin_unlock_irqrestore(&ring->lock, flags);
		return NULL;
	}

	reader->current_seq = cur_seq;
	reader->record_generation = cur_gen;
	reader->current_rec = ring->records[cur_seq & (ring->capacity - 1)];
	raw_spin_unlock_irqrestore(&ring->lock, flags);

	return &reader->current_rec;
}

static void gc550_diag_seq_stop(struct seq_file *m, void *v)
{
	/* Idempotent; no persistent lock held across start/next and show */
}

static int gc550_diag_seq_show(struct seq_file *m, void *v)
{
	struct gc550_diag_reader *reader = m->private;
	struct gc550_diag_record *r = v;

	if (v == reader || reader->is_synthetic_reset) {
		seq_puts(m, "[RING RESET DETECTED]\n");
		return 0;
	}

	if (reader->overrun_lost > 0)
		seq_printf(m, "[OVERRUN: %llu records overwritten before read]\n",
			   reader->overrun_lost);

	seq_printf(m, "ts=%llu seq=%llu sub=%llu off=%llu sec=%llu "
		      "slot=%u stat=%d evt=%u len=%u req=%u pair=%u cpu=%u "
		      "cb=%u flg=0x%04x mkr=0x%02x mseq=0x%02x "
		      "st=%u swp=%u vte=%u seg=%u dst=%u pflg=0x%04x r0=0x%02x r1=0x%02x\n",
		   r->timestamp_ns,
		   r->completion_seq,
		   r->submission_generation,
		   r->endpoint_payload_offset,
		   r->secondary_payload_offset,
		   r->slot_index,
		   r->urb_status,
		   r->event_type,
		   r->payload_len,
		   r->urb_rel_offset,
		   r->pair_pos,
		   r->cpu_id,
		   r->active_callbacks,
		   r->flags,
		   r->marker_type,
		   r->marker_seq,
		   r->parser_state,
		   r->swap_mode,
		   r->swap_votes,
		   r->segment_id,
		   r->distance_from_prev_sav,
		   r->parser_flags,
		   r->reserved[0],
		   r->reserved[1]);

	return 0;
}

static const struct seq_operations gc550_diag_seq_ops = {
	.start = gc550_diag_seq_start,
	.next  = gc550_diag_seq_next,
	.stop  = gc550_diag_seq_stop,
	.show  = gc550_diag_seq_show,
};

static int gc550_diag_records_open(struct inode *inode, struct file *file)
{
	struct gc550_dev *dev = inode->i_private;
	struct gc550_diag_reader *reader;
	struct seq_file *m;
	unsigned long flags;
	int ret;

	if (!kref_get_unless_zero(&dev->ref))
		return -ENODEV;

	reader = kzalloc(sizeof(*reader), GFP_KERNEL);
	if (!reader) {
		kref_put(&dev->ref, gc550_dev_release);
		return -ENOMEM;
	}
	reader->dev = dev;

	raw_spin_lock_irqsave(&dev->diag_ring.lock, flags);
	reader->reader_generation = dev->diag_ring.reset_generation;
	reader->record_generation = dev->diag_ring.reset_generation;
	raw_spin_unlock_irqrestore(&dev->diag_ring.lock, flags);

	ret = seq_open(file, &gc550_diag_seq_ops);
	if (ret) {
		kfree(reader);
		kref_put(&dev->ref, gc550_dev_release);
		return ret;
	}

	m = file->private_data;
	m->private = reader;
	return 0;
}

static int gc550_diag_records_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct gc550_diag_reader *reader = m ? m->private : NULL;
	struct gc550_dev *dev = reader ? reader->dev : inode->i_private;
	int ret;

	kfree(reader);
	ret = seq_release(inode, file);
	if (dev)
		kref_put(&dev->ref, gc550_dev_release);
	return ret;
}

static const struct file_operations gc550_diag_records_fops = {
	.owner   = THIS_MODULE,
	.open    = gc550_diag_records_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = gc550_diag_records_release,
};

/* diag_status debugfs node */

static int gc550_diag_status_show(struct seq_file *m, void *v)
{
	struct gc550_dev *dev = m->private;
	struct gc550_diag_ring *ring = &dev->diag_ring;
	unsigned long flags;
	u64 head, overwritten, reset_gen;
	bool enabled;

	raw_spin_lock_irqsave(&ring->lock, flags);
	head = ring->head;
	overwritten = ring->ring_overwritten_slots;
	reset_gen = ring->reset_generation;
	enabled = ring->enabled;
	raw_spin_unlock_irqrestore(&ring->lock, flags);

	seq_printf(m, "enabled: %d\n", enabled ? 1 : 0);
	seq_printf(m, "capacity: %u\n", ring->capacity);
	seq_printf(m, "head_records: %llu\n", head);
	seq_printf(m, "overwritten_slots: %llu\n", overwritten);
	seq_printf(m, "reset_generation: %llu\n", reset_gen);
	seq_printf(m, "active_callbacks: %d\n",
		   atomic_read(&dev->diag_concurrency.active_callbacks));
	seq_printf(m, "max_simultaneous: %d\n",
		   atomic_read(&dev->diag_concurrency.max_simultaneous));
	seq_printf(m, "callback_entry_seq: %lld\n",
		   (long long)atomic64_read(&dev->diag_concurrency.callback_entry_seq));
	seq_printf(m, "callback_exit_seq: %lld\n",
		   (long long)atomic64_read(&dev->diag_concurrency.callback_exit_seq));
	seq_printf(m, "submission_counter: %lld\n",
		   (long long)atomic64_read(&dev->diag_submission_counter));
	seq_printf(m, "session_epoch: %u\n", dev->diag_session_epoch);
	return 0;
}

static int gc550_diag_status_open(struct inode *inode, struct file *file)
{
	struct gc550_dev *dev = inode->i_private;

	if (!kref_get_unless_zero(&dev->ref))
		return -ENODEV;
	return single_open(file, gc550_diag_status_show, dev);
}

static int gc550_diag_status_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct gc550_dev *dev = m ? m->private : inode->i_private;
	int ret = single_release(inode, file);

	if (dev)
		kref_put(&dev->ref, gc550_dev_release);
	return ret;
}

static const struct file_operations gc550_diag_status_fops = {
	.owner   = THIS_MODULE,
	.open    = gc550_diag_status_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = gc550_diag_status_release,
};

/* diag_enable debugfs node */

static ssize_t gc550_diag_enable_read(struct file *file, char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct gc550_dev *dev = file->private_data;
	struct gc550_diag_ring *ring = &dev->diag_ring;
	unsigned long flags;
	char buf[8];
	size_t len;
	bool enabled;

	raw_spin_lock_irqsave(&ring->lock, flags);
	enabled = ring->enabled;
	raw_spin_unlock_irqrestore(&ring->lock, flags);

	len = scnprintf(buf, sizeof(buf), "%d\n", enabled ? 1 : 0);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t gc550_diag_enable_write(struct file *file, const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct gc550_dev *dev = file->private_data;
	struct gc550_diag_ring *ring = &dev->diag_ring;
	unsigned long flags;
	bool val;
	int ret;

	ret = kstrtobool_from_user(ubuf, count, &val);
	if (ret)
		return ret;

	raw_spin_lock_irqsave(&ring->lock, flags);
	if (ring->records)
		WRITE_ONCE(ring->enabled, val);
	else
		ret = -ENODEV;
	raw_spin_unlock_irqrestore(&ring->lock, flags);

	return ret ? ret : count;
}

static int gc550_diag_enable_open(struct inode *inode, struct file *file)
{
	struct gc550_dev *dev = inode->i_private;

	if (!kref_get_unless_zero(&dev->ref))
		return -ENODEV;
	file->private_data = dev;
	return nonseekable_open(inode, file);
}

static int gc550_diag_enable_release(struct inode *inode, struct file *file)
{
	struct gc550_dev *dev = file->private_data;

	if (dev)
		kref_put(&dev->ref, gc550_dev_release);
	return 0;
}

static const struct file_operations gc550_diag_enable_fops = {
	.owner   = THIS_MODULE,
	.open    = gc550_diag_enable_open,
	.read    = gc550_diag_enable_read,
	.write   = gc550_diag_enable_write,
	.llseek  = noop_llseek,
	.release = gc550_diag_enable_release,
};

/* diag_reset debugfs node */

static ssize_t gc550_diag_reset_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct gc550_dev *dev = file->private_data;

	gc550_diag_reset(dev);
	return count;
}

static int gc550_diag_reset_open(struct inode *inode, struct file *file)
{
	struct gc550_dev *dev = inode->i_private;

	if (!kref_get_unless_zero(&dev->ref))
		return -ENODEV;
	file->private_data = dev;
	return 0;
}

static int gc550_diag_reset_release(struct inode *inode, struct file *file)
{
	struct gc550_dev *dev = file->private_data;

	if (dev)
		kref_put(&dev->ref, gc550_dev_release);
	return 0;
}

static const struct file_operations gc550_diag_reset_fops = {
	.owner   = THIS_MODULE,
	.open    = gc550_diag_reset_open,
	.write   = gc550_diag_reset_write,
	.llseek  = noop_llseek,
	.release = gc550_diag_reset_release,
};

void gc550_diag_debugfs_init(struct gc550_dev *dev)
{
	const char *name;

	mutex_lock(&gc550_diag_root_lock);
	if (!gc550_diag_root)
		gc550_diag_root = debugfs_create_dir(GC550_DRIVER_NAME, NULL);
	gc550_diag_device_count++;
	mutex_unlock(&gc550_diag_root_lock);

	name = video_device_node_name(&dev->vdev);
	if (!name || !name[0])
		name = dev_name(&dev->intf->dev);

	dev->debugfs_dir = debugfs_create_dir(name, gc550_diag_root);

	debugfs_create_file("diag_enable", 0644, dev->debugfs_dir, dev,
			    &gc550_diag_enable_fops);
	debugfs_create_file("diag_status", 0444, dev->debugfs_dir, dev,
			    &gc550_diag_status_fops);
	debugfs_create_file("diag_records", 0444, dev->debugfs_dir, dev,
			    &gc550_diag_records_fops);
	debugfs_create_file("diag_reset", 0200, dev->debugfs_dir, dev,
			    &gc550_diag_reset_fops);
}

void gc550_diag_debugfs_remove(struct gc550_dev *dev)
{
	if (!dev->debugfs_dir)
		return;

	debugfs_remove_recursive(dev->debugfs_dir);
	dev->debugfs_dir = NULL;

	mutex_lock(&gc550_diag_root_lock);
	if (gc550_diag_device_count > 0) {
		gc550_diag_device_count--;
		if (gc550_diag_device_count == 0) {
			debugfs_remove(gc550_diag_root);
			gc550_diag_root = NULL;
		}
	}
	mutex_unlock(&gc550_diag_root_lock);
}
