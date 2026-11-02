// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * AVerMedia Live Gamer Extreme (GC550) - USB 3.0 Capture Driver Core
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Based on USB protocol reverse-engineering and hardware analysis by
 * Chris Sawczuk (ChrisAJS) and contributors (https://github.com/ChrisAJS/lgx2userspace).
 */

#include <linux/slab.h>
#include "gc550.h"

static const struct usb_device_id gc550_usb_table[] = {
	{ USB_DEVICE(USB_VID_AVERMEDIA, USB_PID_GC550) },
	{ USB_DEVICE(USB_VID_AVERMEDIA, USB_PID_GC550_ALT) },
	{ }
};
MODULE_DEVICE_TABLE(usb, gc550_usb_table);

static int gc550_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct gc550_dev *dev;
	int ret;

	/* Only bind to interface 0 */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	dev_info(&intf->dev,
		 "AVerMedia Live Gamer Extreme (GC550) [%04x:%04x] detected (USB speed: %s)\n",
		 le16_to_cpu(udev->descriptor.idVendor),
		 le16_to_cpu(udev->descriptor.idProduct),
		 usb_speed_string(udev->speed));

	if (udev->speed < USB_SPEED_SUPER) {
		dev_warn(&intf->dev,
			 "Device is not connected to a USB 3.0 SuperSpeed port! Streaming 1080p60 requires USB 3.0.\n");
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = usb_get_dev(udev);
	dev->intf = intf;
	mutex_init(&dev->v4l2_lock);
	mutex_init(&dev->cmd_lock);

	usb_set_intfdata(intf, dev);

	ret = gc550_cmd_init(dev);
	if (ret < 0)
		goto err_put_usb;

	ret = gc550_video_register(dev);
	if (ret < 0) {
		dev_err(&intf->dev, "Failed to register V4L2 video device: %d\n",
			ret);
		goto err_free_cmd;
	}

	ret = gc550_audio_register(dev);
	if (ret < 0) {
		dev_err(&intf->dev, "Failed to register ALSA audio device: %d\n",
			ret);
		goto err_unreg_video;
	}

	dev_info(&intf->dev,
		 "Registered %s as /dev/%s and ALSA capture card\n",
		 GC550_DRIVER_NAME, video_device_node_name(&dev->vdev));

	return 0;

err_unreg_video:
	gc550_video_unregister(dev);
err_free_cmd:
	gc550_cmd_free(dev);
err_put_usb:
	usb_set_intfdata(intf, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);
	return ret;
}

static void gc550_disconnect(struct usb_interface *intf)
{
	struct gc550_dev *dev = usb_get_intfdata(intf);

	if (!dev)
		return;

	dev_info(&intf->dev, "AVerMedia GC550 disconnecting\n");

	mutex_lock(&dev->v4l2_lock);
	dev->disconnected = true;
	vb2_queue_error(&dev->vb_queue);
	gc550_audio_unregister(dev);
	gc550_video_unregister(dev);
	mutex_unlock(&dev->v4l2_lock);

	gc550_cmd_free(dev);
	usb_set_intfdata(intf, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);
}

static struct usb_driver gc550_usb_driver = {
	.name       = GC550_DRIVER_NAME,
	.probe      = gc550_probe,
	.disconnect = gc550_disconnect,
	.id_table   = gc550_usb_table,
};

module_usb_driver(gc550_usb_driver);

MODULE_AUTHOR("Øystein Lie <oeystein@gengartv.com>");
MODULE_AUTHOR("Chris Sawczuk (protocol reverse-engineering & lgx2userspace)");
MODULE_DESCRIPTION("AVerMedia Live Gamer Extreme (GC550) V4L2 + ALSA USB 3.0 Capture Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(GC550_DRIVER_VERSION);
