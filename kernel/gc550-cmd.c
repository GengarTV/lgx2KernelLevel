// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * AVerMedia Live Gamer Extreme (GC550) - USB Command & I2C Bridge
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Credits & Attribution:
 * - Command bridge protocol, board-state detection (ADV7604 IO 0xFA / PCA9539A),
 *   automatic input color space fix (ADV7604 IO 0x02), and AT88SC CryptoMemory
 *   safety research by Chris Sawczuk (ChrisAJS) and contributors to
 *   `lgx2userspace` (https://github.com/ChrisAJS/lgx2userspace).
 *
 * Protocol notes:
 * - Commands are sent over Bulk OUT 0x01 and replies received on Bulk IN 0x81.
 * - SAFETY WARNING: Opcode 0x05/0x06/0x07 communicate with an Atmel AT88SC
 *   CryptoMemory chip. Never replay recorded authentication challenges (0xB8/0xBA),
 *   as failed challenge attempts permanently decrement the hardware counter and
 *   lock the chip for official Windows drivers. The hardware streams video and
 *   audio normally without touching the CryptoMemory authentication zones.
 */

#include <linux/unaligned.h>
#include "gc550.h"
#include "gc550-regtables.h"

int gc550_cmd_init(struct gc550_dev *dev)
{
	dev->cmd_out_buf = kmalloc(GC550_CMD_MAX_PKT_SIZE, GFP_KERNEL);
	if (!dev->cmd_out_buf)
		return -ENOMEM;

	dev->cmd_in_buf = kmalloc(GC550_CMD_MAX_PKT_SIZE, GFP_KERNEL);
	if (!dev->cmd_in_buf) {
		kfree(dev->cmd_out_buf);
		dev->cmd_out_buf = NULL;
		return -ENOMEM;
	}

	return 0;
}

void gc550_cmd_free(struct gc550_dev *dev)
{
	kfree(dev->cmd_in_buf);
	kfree(dev->cmd_out_buf);
	dev->cmd_in_buf = NULL;
	dev->cmd_out_buf = NULL;
}

/**
 * gc550_cmd_drain_responses - Drain any stale replies sitting in Bulk IN 0x81
 */
static void gc550_cmd_drain_responses(struct gc550_dev *dev)
{
	int actual_len = 0;
	int i;

	for (i = 0; i < 4; i++) {
		int ret = usb_bulk_msg(dev->udev,
				       usb_rcvbulkpipe(dev->udev, GC550_EP_CMD_IN),
				       dev->cmd_in_buf, GC550_CMD_MAX_PKT_SIZE,
				       &actual_len, 20);
		if (ret < 0)
			break;
	}
}

static int gc550_cmd_send(struct gc550_dev *dev, const u8 *tx_buf, int tx_len)
{
	int actual_len = 0;
	int ret;

	if (tx_len > GC550_CMD_MAX_PKT_SIZE)
		return -EINVAL;

	/* Hard safety gate: block any AT88SC CryptoMemory write/auth opcodes */
	if (tx_len > 0 && (tx_buf[0] == GC550_OP_CRYPTO_HDR ||
			   tx_buf[0] == GC550_OP_CRYPTO_READ ||
			   tx_buf[0] == GC550_OP_CRYPTO_DATA ||
			   tx_buf[0] == 0x09)) {
		dev_err(&dev->intf->dev,
			"Blocked unsafe CryptoMemory opcode 0x%02x!\n",
			tx_buf[0]);
		return -EPERM;
	}

	memcpy(dev->cmd_out_buf, tx_buf, tx_len);

	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, GC550_EP_CMD_OUT),
			   dev->cmd_out_buf, tx_len, &actual_len,
			   GC550_CMD_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Bulk OUT 0x01 failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int gc550_cmd_xfer(struct gc550_dev *dev, const u8 *tx_buf, int tx_len,
			  u8 *rx_buf, int rx_expected)
{
	int actual_len = 0;
	int ret;

	if (rx_expected <= 0 || rx_expected > GC550_CMD_MAX_PKT_SIZE)
		return -EINVAL;

	ret = gc550_cmd_send(dev, tx_buf, tx_len);
	if (ret < 0)
		return ret;

	/*
	 * Always read up to GC550_CMD_MAX_PKT_SIZE (wMaxPacketSize = 1024)
	 * to prevent USB babble / overflow if a previous reply was left queued.
	 */
	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, GC550_EP_CMD_IN),
			   dev->cmd_in_buf, GC550_CMD_MAX_PKT_SIZE,
			   &actual_len, GC550_CMD_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Bulk IN 0x81 failed: %d\n", ret);
		return ret;
	}

	if (actual_len != rx_expected) {
		ret = actual_len < rx_expected ? -EREMOTEIO : -EPROTO;
		dev_err(&dev->intf->dev,
			"Bulk IN 0x81 length mismatch: got %d, expected %d (%d)\n",
			actual_len, rx_expected, ret);
		return ret;
	}

	if (rx_buf)
		memcpy(rx_buf, dev->cmd_in_buf, rx_expected);

	return actual_len;
}

/**
 * gc550_i2c_write - Write up to 32 bytes to a 7-bit I2C slave over USB bridge
 * Wire format: [0x01] [slave_addr] [0x01 (reg addr len)] [reg] [data...]
 */
int gc550_i2c_write(struct gc550_dev *dev, u8 slave_addr, u8 reg,
		    const u8 *data, u8 len)
{
	u8 pkt[4 + 32];
	int ret;

	if (len > 32 || (len && !data))
		return -EINVAL;

	pkt[0] = GC550_OP_I2C_WRITE;
	pkt[1] = slave_addr;
	pkt[2] = 0x01; /* 1-byte register address */
	pkt[3] = reg;
	if (len > 0 && data)
		memcpy(&pkt[4], data, len);

	mutex_lock(&dev->cmd_lock);
	ret = gc550_cmd_send(dev, pkt, 4 + len);
	mutex_unlock(&dev->cmd_lock);

	return ret;
}

int gc550_i2c_write_byte(struct gc550_dev *dev, u8 slave_addr, u8 reg, u8 val)
{
	return gc550_i2c_write(dev, slave_addr, reg, &val, 1);
}

/**
 * gc550_i2c_read - Read bytes from a 7-bit I2C slave over USB bridge
 * Wire format: [0x02] [slave_addr] [0x01 (reg addr len)] [len] [reg]
 */
int gc550_i2c_read(struct gc550_dev *dev, u8 slave_addr, u8 reg,
		   u8 *data, u8 len)
{
	u8 pkt[5];
	int ret;

	if (!data || len == 0 || len > 64)
		return -EINVAL;

	pkt[0] = GC550_OP_I2C_READ;
	pkt[1] = slave_addr;
	pkt[2] = 0x01; /* 1-byte register address */
	pkt[3] = len;
	pkt[4] = reg;

	mutex_lock(&dev->cmd_lock);
	ret = gc550_cmd_xfer(dev, pkt, sizeof(pkt), data, len);
	mutex_unlock(&dev->cmd_lock);

	return (ret < 0) ? ret : 0;
}

int gc550_i2c_read_byte(struct gc550_dev *dev, u8 slave_addr, u8 reg, u8 *val)
{
	return gc550_i2c_read(dev, slave_addr, reg, val, 1);
}

/**
 * gc550_detect_board_state - Check if the GC550 was already bootstrapped
 *
 * The ADV7604 and PCA9539A retain their state across warm restarts as long as
 * USB power is maintained:
 * - ADV7604 IO 0xFA (EDID map address) is 0x00 at cold reset, 0x6C when configured.
 * - PCA9539A (0x74) config regs 0x06/0x07 are 00 89 at cold power-up, 00 09 when configured.
 */
int gc550_detect_board_state(struct gc550_dev *dev, bool *already_configured)
{
	u8 edid_map = 0;
	u8 gpio_cfg[2] = { 0, 0 };
	int ret;

	*already_configured = false;

	mutex_lock(&dev->cmd_lock);
	gc550_cmd_drain_responses(dev);
	mutex_unlock(&dev->cmd_lock);

	ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0xfa, &edid_map);
	if (ret < 0)
		return ret;

	ret = gc550_i2c_read(dev, GC550_I2C_PCA9539A, 0x06, gpio_cfg, 2);
	if (ret < 0)
		return ret;

	dev_info(&dev->intf->dev,
		 "Board state probe: ADV7604 IO[0xFA]=0x%02x, PCA9539A CFG=0x%02x 0x%02x\n",
		 edid_map, gpio_cfg[0], gpio_cfg[1]);

	if (edid_map == 0x6c && gpio_cfg[0] == 0x00 && gpio_cfg[1] == 0x09)
		*already_configured = true;

	return 0;
}

/**
 * gc550_set_auto_input_colorspace - Set ADV7604 input color space to automatic
 *
 * Prevents green/orange tint on YCbCr/RGB HDMI sources by letting the ADV7604
 * choose input color space from the HDMI AVI InfoFrame (IO reg 0x02 bits 7:4 = 0xF)
 * and resetting CP picture adjustments (0x3A..0x3D) to neutral.
 */
static int gc550_set_auto_input_colorspace(struct gc550_dev *dev)
{
	static const u8 neutral_cp[4] = { 0x80, 0x80, 0x00, 0x00 };
	u8 io_02 = 0;
	int ret;

	ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0x02, &io_02);
	if (ret == 0) {
		u8 new_io_02 = (io_02 & 0x0f) | 0xf0;

		if (new_io_02 != io_02) {
			ret = gc550_i2c_write_byte(dev, GC550_I2C_ADV7604_IO,
						   0x02, new_io_02);
			if (ret < 0)
				return ret;
		}
	}

	/* Reset CP contrast/saturation/brightness/hue to neutral */
	return gc550_i2c_write(dev, GC550_I2C_ADV7604_CP, 0x3a,
			       neutral_cp, sizeof(neutral_cp));
}

/**
 * gc550_run_init_sequence - Replay the 6,576 hardware initialization steps
 */
static int gc550_run_init_sequence(struct gc550_dev *dev)
{
	size_t offset = 0;
	unsigned int step = 0;
	int ret = 0;

	mutex_lock(&dev->cmd_lock);
	while (offset + 3 <= GC550_INIT_BLOB_SIZE) {
		u8 tx_len = gc550_init_blob[offset];
		u16 rx_len = get_unaligned_le16(&gc550_init_blob[offset + 1]);
		const u8 *tx_data = &gc550_init_blob[offset + 3];

		if (offset + 3 + tx_len > GC550_INIT_BLOB_SIZE) {
			ret = -EINVAL;
			break;
		}

		if (rx_len > 0)
			ret = gc550_cmd_xfer(dev, tx_data, tx_len, NULL, rx_len);
		else
			ret = gc550_cmd_send(dev, tx_data, tx_len);

		if (ret < 0) {
			dev_err(&dev->intf->dev,
				"Init sequence failed at step %u/%u (offset %zu): %d\n",
				step, GC550_INIT_NUM_STEPS, offset, ret);
			break;
		}

		offset += 3 + tx_len;
		step++;
	}

	gc550_cmd_drain_responses(dev);
	mutex_unlock(&dev->cmd_lock);

	return (ret < 0) ? ret : 0;
}

/**
 * gc550_bootstrap_hw - Initialize GC550 I2C peripherals (ADV7604, ADV7511, PCA9539A)
 */
int gc550_bootstrap_hw(struct gc550_dev *dev)
{
	bool already_configured = false;
	int ret;

	if (dev->is_bootstrapped)
		return 0;

	ret = gc550_detect_board_state(dev, &already_configured);
	if (ret < 0) {
		dev_warn(&dev->intf->dev,
			 "Could not query board state (%d), proceeding with cold bootstrap\n",
			 ret);
	}

	if (already_configured) {
		dev_info(&dev->intf->dev,
			 "GC550 already configured; reusing existing hardware state\n");
		gc550_set_auto_input_colorspace(dev);
		dev->is_bootstrapped = true;
		return 0;
	}

	dev_info(&dev->intf->dev,
		 "Bootstrapping GC550 (%u commands, CryptoMemory auth stripped)...\n",
		 GC550_INIT_NUM_STEPS);

	ret = gc550_run_init_sequence(dev);
	if (ret < 0)
		return ret;

	gc550_set_auto_input_colorspace(dev);

	dev_info(&dev->intf->dev, "GC550 hardware bootstrap complete\n");
	dev->is_bootstrapped = true;
	gc550_log_signal_status(dev);
	return 0;
}

/**
 * gc550_log_signal_status - Query ADV7604 HDMI cable, TMDS clock, and measured timings
 */
void gc550_log_signal_status(struct gc550_dev *dev)
{
	u8 io_0c = 0, io_12 = 0, io_6a = 0, io_6f = 0, cp_b1 = 0;
	u8 hdmi_05_0c[8] = { 0 };
	bool cable_5v, tmds_clk, tmds_lock, stdi_valid, pwr_down, hdmi_mode, hdcp_on;
	u16 width, height;

	if (gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0x6f, &io_6f) < 0)
		return;
	gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0x0c, &io_0c);
	gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0x12, &io_12);
	gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0x6a, &io_6a);
	gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_CP, 0xb1, &cp_b1);
	gc550_i2c_read(dev, GC550_I2C_ADV7604_HDMI, 0x05,
		       hdmi_05_0c, sizeof(hdmi_05_0c));

	cable_5v   = (io_6f & 0x10) != 0;
	tmds_clk   = (io_6a & 0x10) != 0;
	tmds_lock  = (io_6a & 0xe0) == 0xe0;
	stdi_valid = (cp_b1 & 0x80) != 0;
	pwr_down   = (io_0c & 0x24) != 0;
	hdmi_mode  = (hdmi_05_0c[0] & 0x80) != 0;
	hdcp_on    = hdmi_mode && ((hdmi_05_0c[0] & 0x40) != 0);

	width  = ((hdmi_05_0c[2] << 8) | hdmi_05_0c[3]) & 0x0fff;
	height = ((hdmi_05_0c[4] << 8) | hdmi_05_0c[5]) & 0x0fff;
	if (io_12 & 0x10)
		height += ((hdmi_05_0c[6] << 8) | hdmi_05_0c[7]) & 0x0fff;

	dev_info(&dev->intf->dev,
		 "HDMI Status: cable=%s, signal=%s (%ux%u%c, %s, HDCP=%s, TMDS=%uMHz)\n",
		 cable_5v ? "YES" : "NO",
		 (!pwr_down && stdi_valid && tmds_lock && tmds_clk) ? "LOCKED" : "NONE",
		 width, height, (io_12 & 0x10) ? 'i' : 'p',
		 hdmi_mode ? "HDMI" : "DVI",
		 hdcp_on ? "YES" : "NO",
		 hdmi_05_0c[1]);
}
