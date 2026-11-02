// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * AVerMedia Live Gamer Extreme (GC550) - ALSA PCM Capture Subdriver
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Credits & Attribution:
 * - Multiplexed audio packet format (`00 FF FF 58` + BE32 length + S16_LE 48kHz
 *   stereo PCM) based on protocol reverse-engineering by Chris Sawczuk (ChrisAJS)
 *   and contributors in `lgx2userspace` (https://github.com/ChrisAJS/lgx2userspace).
 *
 * Audio packets arrive multiplexed in the USB 3.0 bulk stream (EP 0x83)
 * immediately following each video frame's EAV marker:
 *   [00 FF FF 58] [BE32 length] [LPCM 16-bit LE stereo @ 48 kHz]
 */

#include <linux/vmalloc.h>
#include <sound/initval.h>
#include "gc550.h"

static const struct snd_pcm_hardware gc550_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 64 * 1024,
	.period_bytes_min = 1600,
	.period_bytes_max = 16 * 1024,
	.periods_min = 2,
	.periods_max = 32,
};

static int gc550_pcm_open(struct snd_pcm_substream *substream)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	runtime->hw = gc550_pcm_hw;
	substream->private_data = dev;

	return 0;
}

static int gc550_pcm_close(struct snd_pcm_substream *substream)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&dev->alsa_lock, flags);
	dev->alsa_substream = NULL;
	spin_unlock_irqrestore(&dev->alsa_lock, flags);

	return 0;
}

static int gc550_pcm_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int gc550_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int gc550_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&dev->alsa_lock, flags);
	dev->alsa_hw_ptr = 0;
	dev->alsa_period_pos = 0;
	spin_unlock_irqrestore(&dev->alsa_lock, flags);

	return 0;
}

static int gc550_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dev->alsa_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		dev->alsa_substream = substream;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		dev->alsa_substream = NULL;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&dev->alsa_lock, flags);

	return ret;
}

static snd_pcm_uframes_t gc550_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct gc550_dev *dev = snd_pcm_substream_chip(substream);
	unsigned long flags;
	size_t hw_ptr;

	spin_lock_irqsave(&dev->alsa_lock, flags);
	hw_ptr = dev->alsa_hw_ptr;
	spin_unlock_irqrestore(&dev->alsa_lock, flags);

	return bytes_to_frames(substream->runtime, hw_ptr);
}

static const struct snd_pcm_ops gc550_pcm_ops = {
	.open = gc550_pcm_open,
	.close = gc550_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = gc550_pcm_hw_params,
	.hw_free = gc550_pcm_hw_free,
	.prepare = gc550_pcm_prepare,
	.trigger = gc550_pcm_trigger,
	.pointer = gc550_pcm_pointer,
};

/**
 * gc550_audio_push_samples - Feed demuxed PCM bytes from EP 0x83 into ALSA ring buffer
 */
void gc550_audio_push_samples(struct gc550_dev *dev, const u8 *data, size_t len)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	size_t buf_bytes, period_bytes;
	bool period_elapsed = false;

	if (!len)
		return;

	spin_lock_irqsave(&dev->alsa_lock, flags);
	substream = dev->alsa_substream;
	if (!substream || !substream->runtime || !substream->runtime->dma_area) {
		spin_unlock_irqrestore(&dev->alsa_lock, flags);
		return;
	}

	runtime = substream->runtime;
	buf_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	period_bytes = frames_to_bytes(runtime, runtime->period_size);

	while (len > 0) {
		size_t chunk = min(len, buf_bytes - dev->alsa_hw_ptr);

		memcpy(runtime->dma_area + dev->alsa_hw_ptr, data, chunk);
		dev->alsa_hw_ptr = (dev->alsa_hw_ptr + chunk) % buf_bytes;
		dev->alsa_period_pos += chunk;
		data += chunk;
		len -= chunk;

		if (dev->alsa_period_pos >= period_bytes) {
			dev->alsa_period_pos %= period_bytes;
			period_elapsed = true;
		}
	}
	spin_unlock_irqrestore(&dev->alsa_lock, flags);

	if (period_elapsed)
		snd_pcm_period_elapsed(substream);
}

int gc550_audio_register(struct gc550_dev *dev)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	spin_lock_init(&dev->alsa_lock);

	ret = snd_card_new(&dev->intf->dev, SNDRV_DEFAULT_IDX1, "GC550Audio",
			   THIS_MODULE, 0, &card);
	if (ret < 0)
		return ret;

	strscpy(card->driver, GC550_DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, "AVerMedia GC550", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "AVerMedia Live Gamer Extreme GC550 at usb-%s-%s",
		 dev->udev->bus->bus_name, dev->udev->devpath);

	ret = snd_pcm_new(card, "GC550 HDMI Audio", 0, 0, 1, &pcm);
	if (ret < 0) {
		snd_card_free(card);
		return ret;
	}

	pcm->private_data = dev;
	strscpy(pcm->name, "AVerMedia GC550 Capture", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &gc550_pcm_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      NULL, 64 * 1024, 64 * 1024);

	ret = snd_card_register(card);
	if (ret < 0) {
		snd_card_free(card);
		return ret;
	}

	dev->snd_card = card;
	dev->snd_pcm = pcm;
	return 0;
}

void gc550_audio_unregister(struct gc550_dev *dev)
{
	if (dev->snd_card) {
		snd_card_disconnect(dev->snd_card);
		snd_card_free_when_closed(dev->snd_card);
		dev->snd_card = NULL;
	}
}
