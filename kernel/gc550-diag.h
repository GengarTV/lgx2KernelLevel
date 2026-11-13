/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * AVerMedia Live Gamer Extreme (GC550) - Passive Diagnostic Telemetry (Stage 2B)
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Design Specification: E1.9 Stage 2B Passive Parser Telemetry (Revision 3)
 */

#ifndef _GC550_DIAG_H_
#define _GC550_DIAG_H_

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>
#include <media/videobuf2-core.h>

struct gc550_dev;
struct gc550_buffer;
struct urb;

/* Ring capacity: 262,144 records = 20.00 MiB (minimal power-of-two for 5-min retention) */
#define GC550_DIAG_RING_CAPACITY	262144U

/* Diagnostic Event Types */
enum gc550_diag_event {
	GC550_EVT_NONE			= 0x00,
	GC550_EVT_URB_COMPLETE		= 0x01,
	GC550_EVT_SAV_DETECTED		= 0x02,
	GC550_EVT_EAV_DETECTED		= 0x03,
	GC550_EVT_AUDIO_HEADER		= 0x04,
	GC550_EVT_SWAP_VOTE		= 0x05,
	GC550_EVT_SWAP_TOGGLE		= 0x06,
	GC550_EVT_PARSER_DESYNC		= 0x07,
	GC550_EVT_STREAM_START_BEGIN	= 0x08,
	GC550_EVT_STREAM_START_COMPLETE	= 0x09,
	GC550_EVT_STREAM_STOP_BEGIN	= 0x0A,
	GC550_EVT_STREAM_STOP_COMPLETE	= 0x0B,
	GC550_EVT_PAIR_DISPATCH		= 0x0C,
	GC550_EVT_PAIR_DISCARDED	= 0x0D,
	GC550_EVT_STRADDLE_PROVENANCE	= 0x0E,
	GC550_EVT_PARSER_RESET		= 0x0F,
	GC550_EVT_SUBMISSION_FAILURE	= 0x10,
	GC550_EVT_FRAME_COMPLETE	= 0x11,
};

/* Transport Diagnostic Flags (16-bit bitfield at offset 0x46) */
#define GC550_FLAG_SHORT_TRANSFER		BIT(0)
#define GC550_FLAG_ZERO_LENGTH			BIT(1)
#define GC550_FLAG_TRANSFER_ERROR		BIT(2)
#define GC550_FLAG_PARTIAL_PAYLOAD_DISCARDED	BIT(3)
#define GC550_FLAG_URB_CANCELLED		BIT(4)
#define GC550_FLAG_SUBMISSION_FAILURE		BIT(5)
#define GC550_FLAG_STREAM_DISCONTINUITY		BIT(6)
#define GC550_FLAG_CALLBACK_OVERLAP		BIT(7)
#define GC550_FLAG_DIST_OVERFLOW		BIT(8)
#define GC550_FLAG_ABNORMAL_ABORT		BIT(9)
#define GC550_FLAG_DEVICE_DISCONNECTED		BIT(10)
#define GC550_FLAG_LIFECYCLE_STOP		BIT(11)
#define GC550_FLAG_PROVENANCE_OVERFLOW		BIT(12)
#define GC550_FLAG_MULTI_URB_SPAN		BIT(13)

/* Dedicated Parser Diagnostic Flags (16-bit bitfield at offset 0x48) */
#define GC550_PFLAG_SAV_CHECKSUM_VALID		BIT(0)
#define GC550_PFLAG_SAV_CHECKSUM_INVALID	BIT(1)
#define GC550_PFLAG_EAV_CHECKSUM_VALID		BIT(2)
#define GC550_PFLAG_EAV_CHECKSUM_INVALID	BIT(3)
#define GC550_PFLAG_SEQ_MATCH			BIT(4)
#define GC550_PFLAG_SEQ_MISMATCH		BIT(5)
#define GC550_PFLAG_SPAN_VALID			BIT(6)
#define GC550_PFLAG_SPAN_INVALID		BIT(7)
#define GC550_PFLAG_FRAME_DONE			BIT(8)
#define GC550_PFLAG_FRAME_ERROR			BIT(9)
#define GC550_PFLAG_UNCLOSED_FRAME_ABORT	BIT(10)
#define GC550_PFLAG_PROV_DIRECT_VERIFIED	BIT(11)
#define GC550_PFLAG_PROV_COMPOSITE_CONFIRMED	BIT(12)
#define GC550_PFLAG_PATH_PAIR_BUF		BIT(13)
#define GC550_PFLAG_PATH_DIRECT_URB		BIT(14)
#define GC550_PFLAG_PROV_UNKNOWN		BIT(15)

/* Terminal Outcome Reason Codes (reserved[0] on GC550_EVT_FRAME_COMPLETE) */
#define GC550_TERM_NORMAL_DONE			1
#define GC550_TERM_INVALID_SPAN_ERROR		2
#define GC550_TERM_SWAP_CANDIDATE_ERROR		3
#define GC550_TERM_UNCLOSED_SAV_ABORT		4
#define GC550_TERM_PARSER_ABORT_ERROR		5
#define GC550_TERM_STREAMOFF_TEARDOWN		6

/* Desync Reason Codes (urb_status on GC550_EVT_PARSER_DESYNC) */
#define GC550_DESYNC_SAV_CHECKSUM		1
#define GC550_DESYNC_SEQ_MISMATCH		2
#define GC550_DESYNC_EAV_CHECKSUM		3

/* Reset Reason Codes (urb_status on GC550_EVT_PARSER_RESET) */
#define GC550_RESET_STREAMON			1
#define GC550_RESET_USB_TRANSFER_ERROR		2

/**
 * struct gc550_diag_record - Fixed 80-byte diagnostic record ABI
 *
 * All offsets represent host-delivered endpoint payload coordinates in bytes.
 * For parser events where individual physical byte provenance cannot be proven,
 * urb_rel_offset is strictly set to U32_MAX.
 */
struct gc550_diag_record {
	/* [0x00 - 0x07] Kernel monotonic clock (ktime_get_raw_ns()) */
	u64 timestamp_ns;

	/* [0x08 - 0x0F] Cumulative observed endpoint payload offset */
	u64 endpoint_payload_offset;

	/* [0x10 - 0x17] Secondary coordinate / frame sequence / desync count */
	u64 secondary_payload_offset;

	/* [0x18 - 0x1F] Unique successful submission generation ID */
	u64 submission_generation;

	/* [0x20 - 0x27] Monotonic callback observation index */
	u64 completion_seq;

	/* [0x28 - 0x2B] Relative offset within URB buffer (or U32_MAX) */
	u32 urb_rel_offset;

	/* [0x2C - 0x2F] Distance in payload bytes from preceding SAV (or span) */
	u32 distance_from_prev_sav;

	/* [0x30 - 0x33] Continuous stream segment epoch */
	u32 segment_id;

	/* [0x34 - 0x37] Event payload length / frame size / audio_rem */
	u32 payload_len;

	/* [0x38 - 0x39] Parser pair accumulation pos (0 .. 32,767) */
	u16 pair_pos;

	/* [0x3A - 0x3B] CPU core ID executing event (supports NR_CPUS > 255) */
	u16 cpu_id;

	/* [0x3C - 0x3D] URB status / desync reason / reset reason / vb2 state */
	s16 urb_status;

	/* [0x3E] Event identifier (enum gc550_diag_event) */
	u8 event_type;

	/* [0x3F] URB buffer slot index (0 .. 7 or 0xFF sentinel) */
	u8 slot_index;

	/* [0x40] Protocol marker type byte (0xC0, 0xC1, 0x58, or 0x00) */
	u8 marker_type;

	/* [0x41] Marker sequence byte (SAV b0 or EAV c0) */
	u8 marker_seq;

	/* [0x42] Parser state machine (enum gc550_parse_state) */
	u8 parser_state;

	/* [0x43] Active swap mode (0 = NORMAL, 1 = SWAPPED) */
	u8 swap_mode;

	/* [0x44] Swap vote counter (0 .. 2) */
	u8 swap_votes;

	/* [0x45] Active concurrent callbacks at event entry */
	u8 active_callbacks;

	/* [0x46 - 0x47] Transport diagnostic flags bitfield */
	u16 flags;

	/* [0x48 - 0x49] Dedicated Parser diagnostic flags bitfield */
	u16 parser_flags;

	/* [0x4A - 0x4F] Reserved padding / auxiliary bytes to guarantee exact 80-byte ABI */
	u8 reserved[6];
} __packed __aligned(8);

static_assert(sizeof(struct gc550_diag_record) == 80,
	      "struct gc550_diag_record ABI size must be exactly 80 bytes");
static_assert(offsetof(struct gc550_diag_record, parser_flags) == 0x48,
	      "parser_flags must begin at byte offset 0x48");
static_assert(offsetof(struct gc550_diag_record, reserved) == 0x4A,
	      "reserved must begin at byte offset 0x4A");

/**
 * struct gc550_diag_ring - Diagnostic circular buffer container
 */
struct gc550_diag_ring {
	raw_spinlock_t lock;
	struct gc550_diag_record *records;
	u64 head;
	u64 ring_overwritten_slots;
	u64 reset_generation;
	u32 capacity;
	bool enabled;
};

/**
 * struct gc550_diag_concurrency - Passive concurrency tracking telemetry
 */
struct gc550_diag_concurrency {
	atomic_t active_callbacks;
	atomic_t max_simultaneous;
	atomic64_t callback_entry_seq;
	atomic64_t callback_exit_seq;
};

/**
 * struct gc550_urb_ctx - Per-URB tracking wrapper
 */
struct gc550_urb_ctx {
	struct gc550_dev *dev;
	u64 submission_attempt_id;
	u16 slot_index;
};

/**
 * struct gc550_dispatch_ctx - Stack-local execution context for active callback
 *
 * Propagated synchronously down the parser call chain. Never stored in heap,
 * persistent parser structures, or pair buffers. Identifies the executing
 * callback dispatch; does NOT represent physical byte provenance.
 */
struct gc550_dispatch_ctx {
	u64 submission_generation;
	u64 completion_seq;
	u16 slot_index;
	u16 cpu_id;
	u8  active_callbacks;
};

/* Diagnostics Infrastructure API */
#ifdef CONFIG_GC550_DIAG

int gc550_diag_init(struct gc550_dev *dev);
void gc550_diag_free(struct gc550_dev *dev);
void gc550_diag_debugfs_init(struct gc550_dev *dev);
void gc550_diag_debugfs_remove(struct gc550_dev *dev);
void gc550_diag_emit(struct gc550_dev *dev, const struct gc550_diag_record *rec);
void gc550_diag_reset(struct gc550_dev *dev);

/* Transport Telemetry Helpers */
void gc550_diag_emit_urb_complete(struct gc550_dev *dev,
				  const struct gc550_urb_ctx *ctx,
				  const struct urb *urb,
				  u64 entry_seq, int active,
				  u64 timestamp_ns, u16 cpu_id);
void gc550_diag_emit_submission_failure(struct gc550_dev *dev,
				       u16 slot_index, u64 sub_id,
				       int err);
void gc550_diag_emit_lifecycle(struct gc550_dev *dev, enum gc550_diag_event evt);

/* Stage 2B Parser Telemetry Helpers */
void gc550_diag_emit_marker_sav(struct gc550_dev *dev,
				const struct gc550_dispatch_ctx *dctx,
				const u8 *hdr, bool valid, bool is_pair_buf);
void gc550_diag_emit_marker_eav(struct gc550_dev *dev,
				const struct gc550_dispatch_ctx *dctx,
				const u8 *hdr, bool valid, bool is_pair_buf);
void gc550_diag_emit_audio_header(struct gc550_dev *dev,
				  const struct gc550_dispatch_ctx *dctx,
				  const u8 *hdr, u32 audio_rem, bool is_pair_buf);
void gc550_diag_emit_swap_vote(struct gc550_dev *dev,
			       const struct gc550_dispatch_ctx *dctx,
			       u8 sav_b0, u8 eav_c0, u8 votes, bool is_pair_buf);
void gc550_diag_emit_swap_toggle(struct gc550_dev *dev,
				 const struct gc550_dispatch_ctx *dctx,
				 bool old_mode, bool new_mode, bool is_pair_buf);
void gc550_diag_emit_desync(struct gc550_dev *dev,
			    const struct gc550_dispatch_ctx *dctx,
			    u16 reason, u8 marker_type, u8 marker_seq,
			    bool is_pair_buf);
void gc550_diag_emit_frame_complete(struct gc550_dev *dev,
				    const struct gc550_dispatch_ctx *dctx,
				    struct gc550_buffer *buf,
				    enum vb2_buffer_state state,
				    u8 term_reason, bool is_pair_buf);
void gc550_diag_emit_parser_reset(struct gc550_dev *dev,
				  const struct gc550_dispatch_ctx *dctx,
				  u16 reason);

#else /* !CONFIG_GC550_DIAG */

static inline int gc550_diag_init(struct gc550_dev *dev) { return 0; }
static inline void gc550_diag_free(struct gc550_dev *dev) {}
static inline void gc550_diag_debugfs_init(struct gc550_dev *dev) {}
static inline void gc550_diag_debugfs_remove(struct gc550_dev *dev) {}
static inline void gc550_diag_emit(struct gc550_dev *dev,
				   const struct gc550_diag_record *rec) {}
static inline void gc550_diag_reset(struct gc550_dev *dev) {}

static inline void gc550_diag_emit_urb_complete(struct gc550_dev *dev,
						const struct gc550_urb_ctx *ctx,
						const struct urb *urb,
						u64 entry_seq, int active,
						u64 timestamp_ns, u16 cpu_id) {}
static inline void gc550_diag_emit_submission_failure(struct gc550_dev *dev,
						     u16 slot_index, u64 sub_id,
						     int err) {}
static inline void gc550_diag_emit_lifecycle(struct gc550_dev *dev,
					     enum gc550_diag_event evt) {}

static inline void gc550_diag_emit_marker_sav(struct gc550_dev *dev,
					      const struct gc550_dispatch_ctx *dctx,
					      const u8 *hdr, bool valid,
					      bool is_pair_buf) {}
static inline void gc550_diag_emit_marker_eav(struct gc550_dev *dev,
					      const struct gc550_dispatch_ctx *dctx,
					      const u8 *hdr, bool valid,
					      bool is_pair_buf) {}
static inline void gc550_diag_emit_audio_header(struct gc550_dev *dev,
						const struct gc550_dispatch_ctx *dctx,
						const u8 *hdr, u32 audio_rem,
						bool is_pair_buf) {}
static inline void gc550_diag_emit_swap_vote(struct gc550_dev *dev,
					     const struct gc550_dispatch_ctx *dctx,
					     u8 sav_b0, u8 eav_c0, u8 votes,
					     bool is_pair_buf) {}
static inline void gc550_diag_emit_swap_toggle(struct gc550_dev *dev,
					       const struct gc550_dispatch_ctx *dctx,
					       bool old_mode, bool new_mode,
					       bool is_pair_buf) {}
static inline void gc550_diag_emit_desync(struct gc550_dev *dev,
					  const struct gc550_dispatch_ctx *dctx,
					  u16 reason, u8 marker_type,
					  u8 marker_seq, bool is_pair_buf) {}
static inline void gc550_diag_emit_frame_complete(struct gc550_dev *dev,
						  const struct gc550_dispatch_ctx *dctx,
						  struct gc550_buffer *buf,
						  enum vb2_buffer_state state,
						  u8 term_reason,
						  bool is_pair_buf) {}
static inline void gc550_diag_emit_parser_reset(struct gc550_dev *dev,
						const struct gc550_dispatch_ctx *dctx,
						u16 reason) {}

#endif /* CONFIG_GC550_DIAG */

#endif /* _GC550_DIAG_H_ */
