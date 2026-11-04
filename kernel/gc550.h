/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * AVerMedia Live Gamer Extreme (GC550) USB 3.0 Capture Driver
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Credits & Acknowledgments:
 * - USB bulk protocol, BT.1120 SAV/EAV framing, 16 KiB FX3 DMA swap analysis,
 *   AT88SC CryptoMemory safety analysis, and GC550/GC551 I2C bootstrap tables
 *   are based on the reverse-engineering work and userspace driver by
 *   Chris Sawczuk (ChrisAJS) and contributors at:
 *   https://github.com/ChrisAJS/lgx2userspace (licensed under GPL-3.0).
 * - ADV7604 and ADV7511 register definitions cross-referenced with the Linux
 *   kernel media drivers (`drivers/media/i2c/adv7604.c` & `adv7511-v4l2.c`).
 */

#ifndef _GC550_H_
#define _GC550_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include <sound/core.h>
#include <sound/pcm.h>

#define GC550_DRIVER_NAME	"gc550"
#define GC550_DRIVER_VERSION	"0.1.1"

/* USB Vendor & Product IDs */
#define USB_VID_AVERMEDIA	0x07ca
#define USB_PID_GC550		0x4710 /* Live Gamer Extreme (GC550) primary USB ID */
#define USB_PID_GC550_ALT	0x0550 /* Alternate GC550 USB ID */

/* USB Bulk Endpoints */
#define GC550_EP_CMD_OUT	0x01
#define GC550_EP_CMD_IN		0x81
#define GC550_EP_STREAM_IN	0x83

/* Command Bridge Constants */
#define GC550_CMD_MAX_PKT_SIZE	1024
#define GC550_CMD_TIMEOUT_MS	5000

/* Bridge Opcodes (Bulk OUT 0x01 / Bulk IN 0x81) */
#define GC550_OP_I2C_WRITE	0x01
#define GC550_OP_I2C_READ	0x02
#define GC550_OP_CRYPTO_HDR	0x05 /* AT88SC CryptoMemory: DO NOT USE FOR AUTH REPLAY */
#define GC550_OP_CRYPTO_READ	0x06
#define GC550_OP_CRYPTO_DATA	0x07
#define GC550_OP_TRIG_08	0x08
#define GC550_OP_EEPROM_WRITE	0x0d
#define GC550_OP_EEPROM_READ	0x0e
#define GC550_OP_STREAM_ARM	0x14

/* Known 7-bit I2C Slave Addresses on GC550 */
#define GC550_I2C_FPGA		0x10 /* Lattice ECP3 FPGA */
#define GC550_I2C_TLV320AIC3X	0x18 /* Analog audio codec */
#define GC550_I2C_ADV7604_IO	0x20 /* ADV7604 HDMI RX IO Map */
#define GC550_I2C_ADV7604_CP	0x22 /* ADV7604 CP Map */
#define GC550_I2C_ADV7604_AFE	0x26 /* ADV7604 AFE Map */
#define GC550_I2C_ADV7604_REP	0x32 /* ADV7604 Repeater / KSV Map */
#define GC550_I2C_ADV7604_HDMI	0x34 /* ADV7604 HDMI Map */
#define GC550_I2C_ADV7604_EDID	0x36 /* ADV7604 EDID RAM Map */
#define GC550_I2C_ADV7511_TX	0x3d /* ADV7511 HDMI TX (Passthrough) */
#define GC550_I2C_ADV7604_INFO	0x46 /* ADV7604 InfoFrame Map (GC550 uses 0x46) */
#define GC550_I2C_PCA9539A	0x74 /* PCA9539A 16-bit GPIO expander */

/* Video Frame / Stream Constants (1920x1080 YUYV) */
#define GC550_DEFAULT_WIDTH	1920
#define GC550_DEFAULT_HEIGHT	1080
#define GC550_BYTES_PER_PIXEL	2
#define GC550_FRAME_SIZE	(GC550_DEFAULT_WIDTH * GC550_DEFAULT_HEIGHT * GC550_BYTES_PER_PIXEL)

/* USB Stream Transfer Parameters */
#define GC550_NUM_URBS		8
#define GC550_URB_BUF_SIZE	(512 * 1024) /* 512 KiB per URB chunk */
#define GC550_FX3_DMA_BUF_SIZE	(16 * 1024)  /* 16 KiB internal FX3 DMA unit */

/* BT.1120 / FX3 Stream Marker Type Bytes (preceded by 00 FF FF) */
#define GC550_MARKER_SAV	0xC0 /* Start of Active Video: 00 FF FF C0 */
#define GC550_MARKER_EAV	0xC1 /* End of Active Video:   00 FF FF C1 */
#define GC550_MARKER_AUDIO	0x58 /* Audio Packet Start:    00 FF FF 58 */

/* Stream Parser State Machine */
enum gc550_parse_state {
	GC550_STATE_IDLE = 0,
	GC550_STATE_SAV_HEADER,
	GC550_STATE_VIDEO_DATA,
	GC550_STATE_EAV_HEADER,
	GC550_STATE_POST_EAV,
	GC550_STATE_AUDIO_HEADER,
	GC550_STATE_AUDIO_DATA,
};

/* V4L2 Frame Buffer Wrapper */
struct gc550_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

/* Stream Parser Context */
struct gc550_stream_parser {
	enum gc550_parse_state state;
	u8 sav_b0;
	u8 hdr_buf[8];
	u8 hdr_pos;
	u32 audio_rem;
	u32 video_pos;
	u64 frame_sequence;
	u32 desync_count;

	/* Byte-level marker prefix state (tracks partial [00 FF FF] across USB URBs) */
	u8 marker_stage; /* 0 = none, 1 = saw 00, 2 = saw 00 FF, 3 = saw 00 FF FF */

	/* 16 KiB FX3 ping-pong DMA swap reorderer state */
	bool swap_mode;
	u8 swap_votes;
	u8 *pair_buf;      /* 32 KiB staging buffer when swap_mode is active */
	u32 pair_pos;
};

/* Main Device Structure */
struct gc550_dev {
	struct kref ref;
	struct usb_device *udev;
	struct usb_interface *intf;

	/* V4L2 Subsystem */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vb_queue;
	struct v4l2_pix_format pix_fmt;

	/* Locks */
	struct mutex v4l2_lock;	  /* Protects V4L2 ioctls and vb2 queue */
	struct mutex cmd_lock;	  /* Protects EP 0x01 / 0x81 command bridge */
	struct mutex stream_mutex; /* Serializes process-context stream setup/stop */
	spinlock_t queue_lock;	  /* Protects active buffer list */
	spinlock_t stream_lock;	  /* Gates URB submission against stop/disconnect */

	/* Command Bridge Buffers (DMA-safe kmalloc) */
	u8 *cmd_out_buf;
	u8 *cmd_in_buf;

	/* Video Streaming State */
	struct list_head buf_list;
	struct gc550_buffer *cur_buf;
	struct gc550_stream_parser parser;
	struct urb *urbs[GC550_NUM_URBS];
	u8 *urb_bufs[GC550_NUM_URBS];
	bool streaming;
	bool disconnected;
	bool is_bootstrapped;
	bool v4l2_registered;
	bool v4l2_ref_held;

	/* ALSA Audio Subsystem */
	struct snd_card *snd_card;
	struct snd_pcm *snd_pcm;
	struct snd_pcm_substream *alsa_substream;
	spinlock_t alsa_lock;
	wait_queue_head_t alsa_wait;
	unsigned int alsa_inflight;
	bool alsa_disconnected;
	size_t alsa_hw_ptr;
	size_t alsa_period_pos;
};

/* Function Prototypes - gc550-cmd.c */
int gc550_cmd_init(struct gc550_dev *dev);
void gc550_cmd_free(struct gc550_dev *dev);
int gc550_i2c_write(struct gc550_dev *dev, u8 slave_addr, u8 reg,
		    const u8 *data, u8 len);
int gc550_i2c_write_byte(struct gc550_dev *dev, u8 slave_addr, u8 reg, u8 val);
int gc550_i2c_read(struct gc550_dev *dev, u8 slave_addr, u8 reg,
		   u8 *data, u8 len);
int gc550_i2c_read_byte(struct gc550_dev *dev, u8 slave_addr, u8 reg, u8 *val);
int gc550_detect_board_state(struct gc550_dev *dev, bool *already_configured);
int gc550_bootstrap_hw(struct gc550_dev *dev);
void gc550_log_signal_status(struct gc550_dev *dev);

/* Function Prototypes - gc550-video.c */
int gc550_video_register(struct gc550_dev *dev);
void gc550_video_unregister(struct gc550_dev *dev);
void gc550_video_stop_device(struct gc550_dev *dev);
void gc550_parser_reset(struct gc550_stream_parser *parser);
void gc550_parse_stream_chunk(struct gc550_dev *dev, const u8 *data, size_t len);

/* Function Prototypes - gc550-audio.c */
int gc550_audio_register(struct gc550_dev *dev);
void gc550_audio_unregister(struct gc550_dev *dev);
void gc550_audio_push_samples(struct gc550_dev *dev, const u8 *data, size_t len);
void gc550_dev_release(struct kref *ref);

#endif /* _GC550_H_ */
