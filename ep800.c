// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Modern V4L2 driver for Endpoints EP800 based cameras
 * (Creative Webcam PD1001 and relatives).
 *
 * Protocol and EPLite/Bayer decoding based on the historical epcam
 * driver by Jeroen B. Vreeken and later contributors (GPLv2+).
 *
 * Rewritten for modern kernels: V4L2, videobuf2, current USB APIs.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "ep800.h"

MODULE_AUTHOR("Port based on epcam by Jeroen Vreeken et al.");
MODULE_DESCRIPTION("Endpoints EP800 / Creative PD1001 USB camera");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.7");

static int video_nr = -1;
module_param(video_nr, int, 0644);
MODULE_PARM_DESC(video_nr, "videoX minor number (-1 = auto)");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "enable verbose debug messages");

static bool force_bayer;
module_param(force_bayer, bool, 0644);
MODULE_PARM_DESC(force_bayer, "disable EPLite compression (raw Bayer)");

#define ep_dbg(dev, fmt, ...)						\
	do {								\
		if (debug)						\
			dev_info(&(dev)->intf->dev, fmt, ##__VA_ARGS__);\
	} while (0)

enum {
	FMT_BAYER = 0,
	FMT_EPLITE,
};

struct ep800_scratch {
	u8 *data;
	int length;
	int offset;
	bool ready;
};

struct ep800 {
	struct v4l2_device v4l2_dev;
	struct video_device *vdev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct vb2_queue vb_queue;
	struct usb_device *udev;
	struct usb_interface *intf;

	struct mutex lock;
	spinlock_t qlock;
	struct list_head buf_list;
	struct work_struct decode_work;

	u16 camid;
	u16 maxwidth;
	u16 maxheight;
	u16 width;
	u16 height;
	int format;

	bool streaming;
	int iface;

	/* DMA-capable buffer for usb_control_msg (must not be on stack) */
	u8 *ctrl_buf;
	size_t ctrl_buf_size;

	/* isochronous streaming */
	int packetsize;
	struct urb *urb[EP800_NUMSBUF];
	u8 *sbuf[EP800_NUMSBUF];
	struct ep800_scratch scratch[EP800_NUMSCRATCH];
	int scratch_next;
	int scratch_use;
	int scratch_offset;
	int nullpackets;

	/* decode state */
	u8 *rgb;		/* current RGB24 frame being assembled */
	size_t rgb_size;
	int curpix;
	u8 *curline;
	int curlinepix;
	int eplite_curpix;
	int eplite_curline;
	u8 eplite_data[1024];
	int vlc_size;
	int vlc_cod;
	int vlc_data;
	int datacorrupt;
	u32 sequence;

	/* picture / exposure (legacy scale 0..65535) */
	u16 brightness;
	u16 contrast;
	u16 saturation;
	u16 hue;
	u16 rgain;
	u16 ggain;
	u16 bgain;

	/* snapshot button via interrupt endpoint (vendor, not HID) */
	struct input_dev *input;
	struct urb *button_urb;
	u8 *button_buf;
	size_t button_buf_len;
	char input_phys[64];
};

struct ep800_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

static inline struct ep800_buffer *to_ep800_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct ep800_buffer, vb);
}

/* ------------------------------------------------------------------ */
/* USB vendor helpers                                                   */
/* ------------------------------------------------------------------ */

static int ep800_ctrl(struct ep800 *dev, bool set, u8 req, u16 value,
		      void *data, int size)
{
	int pipe, ret;
	u8 type;
	void *buf = NULL;

	if (!dev->udev)
		return -ENODEV;

	if (size < 0)
		return -EINVAL;

	pipe = set ? usb_sndctrlpipe(dev->udev, 0)
		   : usb_rcvctrlpipe(dev->udev, 0);
	type = (set ? USB_DIR_OUT : USB_DIR_IN) |
	       USB_TYPE_VENDOR | USB_RECIP_DEVICE;

	if (size > 0) {
		if (!dev->ctrl_buf || size > (int)dev->ctrl_buf_size)
			return -ENOMEM;
		buf = dev->ctrl_buf;
		if (set && data)
			memcpy(buf, data, size);
		else if (!set)
			memset(buf, 0, size);
	}

	ret = usb_control_msg(dev->udev, pipe, req, type, value, 0,
			      buf, size, 1000);
	if (ret < 0) {
		ep_dbg(dev, "ctrl %s req=%02x val=%04x size=%d -> %d\n",
		       set ? "OUT" : "IN", req, value, size, ret);
		return ret;
	}
	if (!set && data && ret > 0)
		memcpy(data, buf, ret);
	return ret;
}

static int ep800_set_feature(struct ep800 *dev, u16 reg, u16 value)
{
	u8 cp[2];

	ep800_int2qt(value, cp);
	return ep800_ctrl(dev, true, EP800_VENDOR_REQ_EXT_FEATURE, reg, cp, 2);
}

static void ep800_sensor_init(struct ep800 *dev)
{
	ep800_set_feature(dev, H1A424M167_OP_MODE, 0x12);
	ep800_set_feature(dev, H1A424M167_BASE_ENB, 0x0d);
	ep800_set_feature(dev, H1A424M167_AUTO_ENB, 0xf3);
	ep800_set_feature(dev, H1A424M167_RESET_LEVEL, 0x2d);

	ep800_set_feature(dev, HV7131_REG_MODE_B, 0x5);
	ep800_set_feature(dev, HV7131_REG_MODE_C, 0xa);
	/* Higher default exposure than historic epcam defaults */
	ep800_set_feature(dev, HV7131_REG_TITU, 0x20);
	ep800_set_feature(dev, HV7131_REG_TITM, 0x00);
	ep800_set_feature(dev, HV7131_REG_TITL, 0x00);
	ep800_set_feature(dev, HV7131_REG_ARCG, 0x3f);
	ep800_set_feature(dev, HV7131_REG_AGCG, 0x3f);
	ep800_set_feature(dev, HV7131_REG_ABCG, 0x3f);
}

static void ep800_hsv2rgb(u16 hue, u16 sat, u16 val, u16 *r, u16 *g, u16 *b)
{
	unsigned int segment, valsat;
	signed int h = (signed int)hue;
	unsigned int s = (sat > 32768) ? (sat - 32768) * 2 : 0;
	unsigned int v = val;
	unsigned int p;

	if (sat < 32768) {
		*r = *g = *b = val;
		return;
	}
	if (val <= (0xffff / 8)) {
		*r = *g = *b = 0;
		return;
	}

	segment = (h + 10923) & 0xffff;
	segment = segment * 3 >> 16;
	hue -= segment * 21845;
	h = hue;
	h *= 3;
	valsat = v * s >> 16;
	p = v - valsat;
	if (h >= 0) {
		unsigned int t = v - (valsat * (32769 - h) >> 15);

		switch (segment) {
		case 0: *r = v; *g = t; *b = p; break;
		case 1: *r = p; *g = v; *b = t; break;
		default: *r = t; *g = p; *b = v; break;
		}
	} else {
		unsigned int q = v - (valsat * (32769 + h) >> 15);

		switch (segment) {
		case 0: *r = v; *g = p; *b = q; break;
		case 1: *r = q; *g = v; *b = p; break;
		default: *r = p; *g = q; *b = v; break;
		}
	}
}

static void ep800_adjust_pict(struct ep800 *dev)
{
	unsigned int exposure, percentage;
	u16 r = 0, g = 0, b = 0;

	ep800_hsv2rgb(dev->hue, dev->saturation, dev->contrast, &r, &g, &b);
	r = 0x40 - (r >> 10);
	g = 0x40 - (g >> 10);
	b = 0x40 - (b >> 10);

	percentage = (dev->brightness * 100) >> 16;
	exposure = EP800_MAX_EXPOSURE * percentage / 100;
	exposure = clamp(exposure, (unsigned int)EP800_MIN_EXPOSURE,
			 (unsigned int)EP800_MAX_EXPOSURE);

	ep800_set_feature(dev, HV7131_REG_MODE_B, 0x5);
	ep800_set_feature(dev, HV7131_REG_TITU, exposure >> 16);
	ep800_set_feature(dev, HV7131_REG_TITM, (exposure >> 8) & 0xff);
	ep800_set_feature(dev, HV7131_REG_TITL, exposure & 0xff);
	ep800_set_feature(dev, HV7131_REG_MODE_C, 0xa);
	ep800_set_feature(dev, HV7131_REG_ARCG, dev->rgain);
	ep800_set_feature(dev, HV7131_REG_AGCG, dev->ggain);
	ep800_set_feature(dev, HV7131_REG_ABCG, dev->bgain);
	ep800_set_feature(dev, HV7131_REG_OFSR, r);
	ep800_set_feature(dev, HV7131_REG_OFSG, g);
	ep800_set_feature(dev, HV7131_REG_OFSB, b);
}

static void ep800_send_size(struct ep800 *dev, int width, int height)
{
	u8 cp[40];
	int compress = 0;
	int size;

	dev->format = FMT_BAYER;
	if (dev->camid == EP800_CAMID_EP800) {
		/* EPLite on by default; module param can force raw Bayer. */
		if (!force_bayer) {
			compress = 0x02;
			dev->format = FMT_EPLITE;
		}
	}

	memset(cp, 0, sizeof(cp));
	size = ep800_ctrl(dev, false, EP800_VENDOR_REQ_CAPTURE_INFO, 0,
			  cp, sizeof(cp));
	if (size < 0)
		size = 12;
	if (size > (int)sizeof(cp))
		size = sizeof(cp);

	ep800_int2qt(1, cp + 2);
	ep800_int2qt((dev->maxwidth - width) / 2, cp + 4);
	ep800_int2qt((dev->maxheight - height) / 2, cp + 6);
	ep800_int2qt(width, cp + 8);
	ep800_int2qt(height, cp + 10);
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_CAPTURE_INFO, 0, cp, size);

	memset(cp, 0, sizeof(cp));
	ep800_ctrl(dev, false, EP800_VENDOR_REQ_COMPRESSION, 0, cp, sizeof(cp));
	ep800_int2qt(compress, cp);
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_COMPRESSION, 0, cp, 2);

	ep_dbg(dev, "capture %dx%d format=%s compress=%d\n",
	       width, height,
	       dev->format == FMT_EPLITE ? "eplite" : "bayer",
	       compress);
}

/* ------------------------------------------------------------------ */
/* Frame decoding (from epcam)                                          */
/* ------------------------------------------------------------------ */

static void ep800_frame_done(struct ep800 *dev);

static void decode_bayer(struct ep800 *dev, u8 *data, int len)
{
	int datasize = dev->width * dev->height;
	u8 *framedata = dev->rgb;
	u8 *frame_end;
	u8 *curline, *nextline;
	int width = dev->width;
	int blineoffset = 0, bline;
	int linelength = width * 3;
	int i;

	/* Guard against stop_streaming freeing rgb while work runs. */
	if (!framedata || !dev->streaming)
		return;

	frame_end = framedata + datasize * 3;

	if (dev->curpix + len > datasize)
		len = datasize - dev->curpix;
	if (len <= 0)
		return;

	if (dev->curpix == 0) {
		memset(framedata + datasize * 3 - linelength, 0, linelength);
		dev->curline = framedata + linelength * 2;
		dev->curlinepix = 0;
	}

	if (dev->height % 4)
		blineoffset = 1;
	bline = dev->curpix / width + blineoffset;

	curline = dev->curline;
	if (!curline || curline < framedata || curline >= frame_end)
		curline = framedata + linelength * 2;
	nextline = curline + linelength;
	if (nextline >= frame_end)
		nextline = curline;

	while (len) {
		if (dev->curlinepix >= width) {
			dev->curlinepix -= width;
			bline = dev->curpix / width + blineoffset;
			curline += linelength * 2;
			nextline += linelength * 2;
			if (curline >= frame_end) {
				dev->curlinepix++;
				curline -= 3;
				nextline -= 3;
				len--;
				data++;
				dev->curpix++;
			}
			if (nextline >= frame_end)
				nextline = curline;
		}
		/* Edge writes use curline-1/-2/-3; skip if outside buffer. */
		if (curline >= framedata + 3 && curline + 2 < frame_end &&
		    nextline >= framedata + 3 && nextline + 2 < frame_end) {
			if (bline & 1) {
				if (dev->curlinepix & 1) {
					*(curline + 2) = *data;
					*(curline - 1) = *data;
					*(nextline + 2) = *data;
					*(nextline - 1) = *data;
				} else {
					*(curline + 1) =
						(*(curline + 1) + *data) / 2;
					*(curline - 2) =
						(*(curline - 2) + *data) / 2;
					*(nextline + 1) = *data;
					*(nextline - 2) = *data;
				}
			} else {
				if (dev->curlinepix & 1) {
					*(curline + 1) =
						(*(curline + 1) + *data) / 2;
					*(curline - 2) =
						(*(curline - 2) + *data) / 2;
					*(nextline + 1) = *data;
					*(nextline - 2) = *data;
				} else {
					*curline = *data;
					*(curline - 3) = *data;
					*nextline = *data;
					*(nextline - 3) = *data;
				}
			}
		}
		dev->curlinepix++;
		curline -= 3;
		nextline -= 3;
		len--;
		data++;
		dev->curpix++;
	}
	dev->curline = curline;

	if (dev->curpix < datasize)
		return;

	if (!dev->rgb || !dev->streaming)
		return;

	/* border fixups from epcam */
	framedata = dev->rgb + linelength * 2;
	for (i = 0; i < linelength * 2; i++) {
		framedata--;
		if (framedata >= dev->rgb &&
		    framedata + linelength < frame_end)
			*framedata = *(framedata + linelength);
	}
	for (i = 0; i < dev->height; i++) {
		if (framedata + 5 < frame_end) {
			*framedata = *(framedata + 3);
			*(framedata + 1) = *(framedata + 4);
			*(framedata + 2) = *(framedata + 5);
		}
		framedata += linelength;
		if (framedata >= frame_end)
			break;
	}
	framedata -= linelength * 2;
	for (i = 0; i < linelength * 2; i++) {
		framedata++;
		if (framedata < frame_end && framedata >= dev->rgb + linelength)
			*framedata = *(framedata - linelength);
	}

	/*
	 * Demosaic writes component 0/1/2 in sensor order. On PD1001 that is
	 * already B,G,R in memory (legacy epcam BGRon=0), matching BGR24.
	 * Do not R↔B swap here — that turns skin blue in ffplay/VLC.
	 */
	ep800_frame_done(dev);
	dev->curpix = 0;
}

static void decode_eplite_integrate(struct ep800 *dev, int data)
{
	int linelength = dev->width;
	int i;

	if (dev->eplite_curpix < 2) {
		dev->eplite_data[dev->eplite_curpix] = 1 + data * 4;
	} else {
		i = dev->eplite_data[dev->eplite_curpix - 2] + data * 4;
		dev->eplite_data[dev->eplite_curpix] = i;
		if (i > 255 || i < 0) {
			dev->datacorrupt++;
			return;
		}
	}

	dev->eplite_curpix++;
	if (dev->eplite_curpix >= linelength) {
		decode_bayer(dev, dev->eplite_data, linelength);
		dev->eplite_curpix = 0;
		dev->eplite_curline += linelength;
		if (dev->eplite_curline >= dev->height * linelength)
			dev->eplite_curline = 0;
	}
}

static void decode_eplite(struct ep800 *dev, struct ep800_scratch *buffer)
{
	u8 *data = buffer->data;
	int len = buffer->length;
	int pos = 0;
	int vlc_cod = dev->vlc_cod;
	int vlc_size = dev->vlc_size;
	int vlc_data = dev->vlc_data;

	if (!buffer->offset) {
		dev->curpix = 0;
		dev->eplite_curline = 0;
		dev->eplite_curpix = 0;
		vlc_cod = vlc_size = vlc_data = 0;
	}

	while (pos < len) {
		int bit_cur = 8;

		while (bit_cur) {
			int bit = ((*data) >> (bit_cur - 1)) & 1;

			if (!vlc_cod) {
				if (bit) {
					vlc_size++;
				} else if (!vlc_size) {
					decode_eplite_integrate(dev, 0);
				} else {
					vlc_cod = 2;
					vlc_data = 0;
				}
			} else {
				if (vlc_size > 7) {
					dev->datacorrupt++;
					dev->vlc_cod = 0;
					dev->vlc_size = 0;
					dev->vlc_data = 0;
					return;
				}
				if (vlc_cod == 2) {
					if (!bit)
						vlc_data = -(1 << vlc_size) + 1;
					vlc_cod--;
				}
				vlc_size--;
				vlc_data += bit << vlc_size;
				if (!vlc_size) {
					decode_eplite_integrate(dev, vlc_data);
					vlc_cod = 0;
				}
			}
			bit_cur--;
		}
		pos++;
		data++;
	}

	dev->vlc_size = vlc_size;
	dev->vlc_cod = vlc_cod;
	dev->vlc_data = vlc_data;
}

static void ep800_process_scratch(struct ep800 *dev)
{
	while (dev->streaming && dev->scratch[dev->scratch_use].ready) {
		struct ep800_scratch *sc = &dev->scratch[dev->scratch_use];

		if (dev->format == FMT_EPLITE)
			decode_eplite(dev, sc);
		else
			decode_bayer(dev, sc->data, sc->length);

		sc->ready = false;
		dev->scratch_use = (dev->scratch_use + 1) % EP800_NUMSCRATCH;
	}
}

static void ep800_decode_work(struct work_struct *work)
{
	struct ep800 *dev = container_of(work, struct ep800, decode_work);

	if (!dev->streaming)
		return;
	ep800_process_scratch(dev);
}

static void ep800_frame_done(struct ep800 *dev)
{
	struct ep800_buffer *buf = NULL;
	unsigned long flags;
	void *vaddr;
	size_t size = (size_t)dev->width * dev->height * 3;

	spin_lock_irqsave(&dev->qlock, flags);
	if (!list_empty(&dev->buf_list)) {
		buf = list_first_entry(&dev->buf_list, struct ep800_buffer, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&dev->qlock, flags);

	if (!buf)
		return;

	vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	if (vaddr && size <= vb2_plane_size(&buf->vb.vb2_buf, 0))
		memcpy(vaddr, dev->rgb, size);

	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = dev->sequence++;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, size);
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/* ------------------------------------------------------------------ */
/* Isochronous URBs                                                     */
/* ------------------------------------------------------------------ */

static void ep800_video_irq(struct urb *urb)
{
	struct ep800 *dev = urb->context;
	int length = 0, i, ret;

	if (!dev->streaming)
		return;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
	    urb->status == -ESHUTDOWN)
		return;

	if (!dev->scratch[dev->scratch_next].ready) {
		for (i = 0; i < EP800_PACKETBUFS; i++) {
			if (urb->iso_frame_desc[i].status ||
			    !urb->iso_frame_desc[i].actual_length)
				continue;
			dev->nullpackets = 0;
			memcpy(dev->scratch[dev->scratch_next].data + length,
			       urb->transfer_buffer +
			       urb->iso_frame_desc[i].offset,
			       urb->iso_frame_desc[i].actual_length);
			length += urb->iso_frame_desc[i].actual_length;
		}
		if (length) {
			dev->scratch[dev->scratch_next].offset =
				dev->scratch_offset;
			dev->scratch[dev->scratch_next].length = length;
			dev->scratch[dev->scratch_next].ready = true;
			dev->scratch_next =
				(dev->scratch_next + 1) % EP800_NUMSCRATCH;
			/* Never decode in softirq — that soft-locked the host. */
			schedule_work(&dev->decode_work);
		} else {
			dev->nullpackets++;
		}
	}

	if (length)
		dev->scratch_offset++;
	else
		dev->scratch_offset = 0;

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		ep_dbg(dev, "resubmit urb failed: %d\n", ret);
}

static void ep800_free_urbs(struct ep800 *dev)
{
	int i;

	for (i = 0; i < EP800_NUMSBUF; i++) {
		if (dev->urb[i]) {
			usb_kill_urb(dev->urb[i]);
			usb_free_urb(dev->urb[i]);
			dev->urb[i] = NULL;
		}
		kfree(dev->sbuf[i]);
		dev->sbuf[i] = NULL;
	}
	for (i = 0; i < EP800_NUMSCRATCH; i++) {
		kfree(dev->scratch[i].data);
		dev->scratch[i].data = NULL;
		dev->scratch[i].ready = false;
	}
}

static int ep800_alloc_urbs(struct ep800 *dev)
{
	struct urb *urb;
	int i, fx, err;

	for (i = 0; i < EP800_NUMSCRATCH; i++) {
		dev->scratch[i].data =
			kmalloc(dev->packetsize * EP800_PACKETBUFS, GFP_KERNEL);
		if (!dev->scratch[i].data)
			goto fail;
		dev->scratch[i].ready = false;
	}

	for (i = 0; i < EP800_NUMSBUF; i++) {
		dev->sbuf[i] =
			kmalloc(dev->packetsize * EP800_PACKETBUFS, GFP_KERNEL);
		if (!dev->sbuf[i])
			goto fail;

		urb = usb_alloc_urb(EP800_PACKETBUFS, GFP_KERNEL);
		if (!urb)
			goto fail;

		urb->dev = dev->udev;
		urb->context = dev;
		urb->pipe = usb_rcvisocpipe(dev->udev, EP800_VIDEO_ENDPOINT);
		urb->transfer_flags = URB_ISO_ASAP;
		urb->transfer_buffer = dev->sbuf[i];
		urb->complete = ep800_video_irq;
		urb->number_of_packets = EP800_PACKETBUFS;
		urb->transfer_buffer_length =
			dev->packetsize * EP800_PACKETBUFS;
		urb->interval = 1;
		for (fx = 0; fx < EP800_PACKETBUFS; fx++) {
			urb->iso_frame_desc[fx].offset = dev->packetsize * fx;
			urb->iso_frame_desc[fx].length = dev->packetsize;
		}
		dev->urb[i] = urb;
	}

	for (i = 0; i < EP800_NUMSBUF; i++) {
		err = usb_submit_urb(dev->urb[i], GFP_KERNEL);
		if (err) {
			ep_dbg(dev, "submit urb %d failed: %d\n", i, err);
			goto fail;
		}
	}
	return 0;
fail:
	ep800_free_urbs(dev);
	return -ENOMEM;
}

/* ------------------------------------------------------------------ */
/* Snapshot button (interrupt EP 0x82 → KEY_CAMERA)                     */
/* ------------------------------------------------------------------ */

static int ep800_button_interval(struct ep800 *dev)
{
	struct usb_host_interface *alt = dev->intf->cur_altsetting;
	int i;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (usb_endpoint_is_int_in(ep) &&
		    usb_endpoint_num(ep) == EP800_BUTTON_ENDPOINT)
			return max_t(int, 1, ep->bInterval);
	}
	return 8;
}

static void ep800_button_irq(struct urb *urb)
{
	struct ep800 *dev = urb->context;
	int status = urb->status;
	int i;
	bool pressed = false;

	switch (status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* unlinked — stream alt change or disconnect */
		return;
	default:
		goto resubmit;
	}

	/* Legacy epcam: length >= 2 and non-zero payload → click. */
	if (urb->actual_length >= 2 && dev->input && dev->button_buf) {
		for (i = 0; i < urb->actual_length; i++) {
			if (dev->button_buf[i]) {
				pressed = true;
				break;
			}
		}
		if (pressed) {
			input_report_key(dev->input, KEY_CAMERA, 1);
			input_sync(dev->input);
			input_report_key(dev->input, KEY_CAMERA, 0);
			input_sync(dev->input);
			ep_dbg(dev, "snapshot button\n");
		}
	}

resubmit:
	if (!dev->udev)
		return;
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status)
		ep_dbg(dev, "button urb resubmit failed: %d\n", status);
}

static void ep800_button_stop(struct ep800 *dev)
{
	if (dev->button_urb)
		usb_kill_urb(dev->button_urb);
}

static int ep800_button_start(struct ep800 *dev)
{
	int interval;
	int ret;

	if (!dev->udev || !dev->button_urb || !dev->button_buf)
		return -ENODEV;

	ep800_button_stop(dev);
	interval = ep800_button_interval(dev);
	usb_fill_int_urb(dev->button_urb, dev->udev,
			 usb_rcvintpipe(dev->udev, EP800_BUTTON_ENDPOINT),
			 dev->button_buf, dev->button_buf_len,
			 ep800_button_irq, dev, interval);
	ret = usb_submit_urb(dev->button_urb, GFP_KERNEL);
	if (ret)
		dev_warn(&dev->intf->dev,
			 "button urb submit failed: %d\n", ret);
	return ret;
}

static void ep800_button_free(struct ep800 *dev)
{
	ep800_button_stop(dev);
	if (dev->input) {
		input_unregister_device(dev->input);
		dev->input = NULL;
	}
	if (dev->button_urb) {
		usb_free_urb(dev->button_urb);
		dev->button_urb = NULL;
	}
	kfree(dev->button_buf);
	dev->button_buf = NULL;
}

static int ep800_button_init(struct ep800 *dev)
{
	struct input_dev *input;
	int ret;

	dev->button_buf_len = 64;
	dev->button_buf = kmalloc(dev->button_buf_len, GFP_KERNEL);
	if (!dev->button_buf)
		return -ENOMEM;

	dev->button_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->button_urb) {
		ret = -ENOMEM;
		goto err_buf;
	}

	input = input_allocate_device();
	if (!input) {
		ret = -ENOMEM;
		goto err_urb;
	}

	usb_make_path(dev->udev, dev->input_phys, sizeof(dev->input_phys));
	strlcat(dev->input_phys, "/button", sizeof(dev->input_phys));

	input->name = "Creative PD1001 Button";
	input->phys = dev->input_phys;
	usb_to_input_id(dev->udev, &input->id);
	input->dev.parent = &dev->intf->dev;
	input->evbit[0] = BIT_MASK(EV_KEY);
	set_bit(KEY_CAMERA, input->keybit);

	ret = input_register_device(input);
	if (ret)
		goto err_input;

	dev->input = input;
	ret = ep800_button_start(dev);
	if (ret)
		dev_warn(&dev->intf->dev,
			 "snapshot button not available (%d)\n", ret);
	else
		dev_info(&dev->intf->dev,
			 "snapshot button → KEY_CAMERA on %s\n",
			 dev->input_phys);
	return 0;

err_input:
	input_free_device(input);
err_urb:
	usb_free_urb(dev->button_urb);
	dev->button_urb = NULL;
err_buf:
	kfree(dev->button_buf);
	dev->button_buf = NULL;
	return ret;
}

static int ep800_start_stream(struct ep800 *dev)
{
	struct usb_host_interface *alt;
	int ret;

	if (!dev->udev)
		return -ENODEV;

	ret = usb_set_interface(dev->udev, dev->iface, EP800_ISO_ALTSETTING);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "set alt %d failed: %d\n",
			EP800_ISO_ALTSETTING, ret);
		return ret;
	}
	/* set_interface unlinks the button URB — restart on new alt. */
	ep800_button_start(dev);

	alt = &dev->intf->altsetting[EP800_ISO_ALTSETTING];
	dev->packetsize =
		le16_to_cpu(alt->endpoint[0].desc.wMaxPacketSize) & 0x7ff;
	if (!dev->packetsize)
		dev->packetsize = 1016;

	vfree(dev->rgb);
	dev->rgb_size = (size_t)dev->width * dev->height * 3;
	dev->rgb = vzalloc(dev->rgb_size);
	if (!dev->rgb)
		return -ENOMEM;

	dev->scratch_next = 0;
	dev->scratch_use = 0;
	dev->scratch_offset = 0;
	dev->nullpackets = 0;
	dev->curpix = 0;
	dev->eplite_curpix = 0;
	dev->eplite_curline = 0;
	dev->vlc_cod = 0;
	dev->vlc_size = 0;
	dev->vlc_data = 0;
	dev->datacorrupt = 0;

	ep800_ctrl(dev, true, EP800_VENDOR_REQ_CAM_POWER, 1, NULL, 0);
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_LED_CONTROL, 1, NULL, 0);
	ep800_adjust_pict(dev);
	ep800_send_size(dev, dev->width, dev->height);
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_CONT_CAPTURE, 1, NULL, 0);

	dev->streaming = true;
	ret = ep800_alloc_urbs(dev);
	if (ret) {
		dev->streaming = false;
		ep800_ctrl(dev, true, EP800_VENDOR_REQ_CONT_CAPTURE, 0, NULL, 0);
		ep800_ctrl(dev, true, EP800_VENDOR_REQ_LED_CONTROL, 0, NULL, 0);
		ep800_ctrl(dev, true, EP800_VENDOR_REQ_CAM_POWER, 0, NULL, 0);
		usb_set_interface(dev->udev, dev->iface, 0);
		ep800_button_start(dev);
		return ret;
	}

	ep_dbg(dev, "streaming started (%dx%d, pkt=%d)\n",
	       dev->width, dev->height, dev->packetsize);
	return 0;
}

static void ep800_stop_stream(struct ep800 *dev)
{
	if (!dev->streaming)
		return;

	dev->streaming = false;
	/* Kill URBs only while caller may hold the vb2 lock. */
	ep800_free_urbs(dev);

	if (dev->udev) {
		ep800_ctrl(dev, true, EP800_VENDOR_REQ_CONT_CAPTURE, 0, NULL, 0);
		ep800_ctrl(dev, true, EP800_VENDOR_REQ_LED_CONTROL, 0, NULL, 0);
		ep800_ctrl(dev, true, EP800_VENDOR_REQ_CAM_POWER, 0, NULL, 0);
		usb_set_interface(dev->udev, dev->iface, 0);
		ep800_button_start(dev);
	}
	ep_dbg(dev, "streaming stopped\n");
}

/* ------------------------------------------------------------------ */
/* videobuf2                                                            */
/* ------------------------------------------------------------------ */

static int ep800_queue_setup(struct vb2_queue *vq,
			     unsigned int *nbuffers, unsigned int *nplanes,
			     unsigned int sizes[], struct device *alloc_devs[])
{
	struct ep800 *dev = vb2_get_drv_priv(vq);
	unsigned int size = (unsigned int)dev->width * dev->height * 3;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;
	if (*nbuffers < 2)
		*nbuffers = 2;
	return 0;
}

static int ep800_buffer_prepare(struct vb2_buffer *vb)
{
	struct ep800 *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int size = (unsigned int)dev->width * dev->height * 3;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void ep800_buffer_queue(struct vb2_buffer *vb)
{
	struct ep800 *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct ep800_buffer *buf = to_ep800_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	list_add_tail(&buf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->qlock, flags);
}

static int ep800_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct ep800 *dev = vb2_get_drv_priv(vq);
	return ep800_start_stream(dev);
}

static void ep800_return_buffers(struct ep800 *dev, enum vb2_buffer_state state)
{
	struct ep800_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&dev->qlock, flags);
}

static void ep800_stop_streaming(struct vb2_queue *vq)
{
	struct ep800 *dev = vb2_get_drv_priv(vq);

	ep800_stop_stream(dev);
	ep800_return_buffers(dev, VB2_BUF_STATE_ERROR);

	/* Drop queue lock so cancel_work_sync cannot deadlock with disconnect. */
	vb2_ops_wait_prepare(vq);
	cancel_work_sync(&dev->decode_work);
	vfree(dev->rgb);
	dev->rgb = NULL;
	dev->curline = NULL;
	vb2_ops_wait_finish(vq);
}

static const struct vb2_ops ep800_vb2_ops = {
	.queue_setup	 = ep800_queue_setup,
	.buf_prepare	 = ep800_buffer_prepare,
	.buf_queue	 = ep800_buffer_queue,
	.start_streaming = ep800_start_streaming,
	.stop_streaming	 = ep800_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

/* ------------------------------------------------------------------ */
/* V4L2 ioctls                                                          */
/* ------------------------------------------------------------------ */

struct ep800_fmt {
	u32 width;
	u32 height;
};

static const struct ep800_fmt ep800_modes[] = {
	{ 176, 144 },
	{ 320, 240 },
	{ 352, 288 },
	{ 400, 300 },
};

static int ep800_querycap(struct file *file, void *priv,
			  struct v4l2_capability *cap)
{
	struct ep800 *dev = video_drvdata(file);

	strscpy(cap->driver, "ep800", sizeof(cap->driver));
	strscpy(cap->card, "Creative PD1001 (EP800)", sizeof(cap->card));
	usb_make_path(dev->udev, cap->bus_info, sizeof(cap->bus_info));
	return 0;
}

static int ep800_enum_fmt(struct file *file, void *priv,
			  struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_BGR24;
	strscpy(f->description, "24-bit BGR", sizeof(f->description));
	return 0;
}

static int ep800_enum_framesizes(struct file *file, void *priv,
				 struct v4l2_frmsizeenum *fsize)
{
	struct ep800 *dev = video_drvdata(file);

	if (fsize->pixel_format != V4L2_PIX_FMT_BGR24)
		return -EINVAL;
	if (fsize->index >= ARRAY_SIZE(ep800_modes))
		return -EINVAL;
	if (ep800_modes[fsize->index].width > dev->maxwidth ||
	    ep800_modes[fsize->index].height > dev->maxheight)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = ep800_modes[fsize->index].width;
	fsize->discrete.height = ep800_modes[fsize->index].height;
	return 0;
}

static void ep800_fill_fmt(struct ep800 *dev, struct v4l2_format *f)
{
	f->fmt.pix.width = dev->width;
	f->fmt.pix.height = dev->height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = dev->width * 3;
	f->fmt.pix.sizeimage = dev->width * dev->height * 3;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
}

static int ep800_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	ep800_fill_fmt(video_drvdata(file), f);
	return 0;
}

static int ep800_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ep800 *dev = video_drvdata(file);
	const struct ep800_fmt *best = &ep800_modes[0];
	unsigned int i, bestdiff = ~0U;

	for (i = 0; i < ARRAY_SIZE(ep800_modes); i++) {
		unsigned int diff;

		if (ep800_modes[i].width > dev->maxwidth ||
		    ep800_modes[i].height > dev->maxheight)
			continue;
		diff = abs((int)ep800_modes[i].width - (int)f->fmt.pix.width) +
		       abs((int)ep800_modes[i].height - (int)f->fmt.pix.height);
		if (diff < bestdiff) {
			bestdiff = diff;
			best = &ep800_modes[i];
		}
	}

	f->fmt.pix.width = best->width;
	f->fmt.pix.height = best->height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = best->width * 3;
	f->fmt.pix.sizeimage = best->width * best->height * 3;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int ep800_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ep800 *dev = video_drvdata(file);

	if (vb2_is_busy(&dev->vb_queue))
		return -EBUSY;

	ep800_try_fmt(file, priv, f);
	dev->width = f->fmt.pix.width;
	dev->height = f->fmt.pix.height;
	return 0;
}

static int ep800_enum_input(struct file *file, void *priv,
			    struct v4l2_input *inp)
{
	if (inp->index != 0)
		return -EINVAL;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->std = 0;
	inp->status = 0;
	inp->capabilities = 0;
	strscpy(inp->name, "Camera", sizeof(inp->name));
	return 0;
}

static int ep800_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int ep800_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int ep800_enum_frameintervals(struct file *file, void *priv,
				     struct v4l2_frmivalenum *fival)
{
	struct ep800 *dev = video_drvdata(file);
	unsigned int i;

	if (fival->pixel_format != V4L2_PIX_FMT_BGR24 || fival->index)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ep800_modes); i++) {
		if (ep800_modes[i].width == fival->width &&
		    ep800_modes[i].height == fival->height)
			break;
	}
	if (i == ARRAY_SIZE(ep800_modes) ||
	    fival->width > dev->maxwidth || fival->height > dev->maxheight)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	/* Full-Speed USB + EPLite typically yields ~2–3 fps */
	fival->discrete.numerator = 1;
	fival->discrete.denominator = 3;
	return 0;
}

static int ep800_g_parm(struct file *file, void *priv,
			struct v4l2_streamparm *parm)
{
	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.readbuffers = 2;
	parm->parm.capture.timeperframe.numerator = 1;
	parm->parm.capture.timeperframe.denominator = 3;
	return 0;
}

static int ep800_s_parm(struct file *file, void *priv,
			struct v4l2_streamparm *parm)
{
	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* Fixed rate; report what we actually use. */
	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.readbuffers = 2;
	parm->parm.capture.timeperframe.numerator = 1;
	parm->parm.capture.timeperframe.denominator = 3;
	return 0;
}

static const struct v4l2_ioctl_ops ep800_ioctl_ops = {
	.vidioc_querycap		= ep800_querycap,
	.vidioc_enum_fmt_vid_cap	= ep800_enum_fmt,
	.vidioc_g_fmt_vid_cap		= ep800_g_fmt,
	.vidioc_try_fmt_vid_cap		= ep800_try_fmt,
	.vidioc_s_fmt_vid_cap		= ep800_s_fmt,
	.vidioc_enum_framesizes		= ep800_enum_framesizes,
	.vidioc_enum_frameintervals	= ep800_enum_frameintervals,
	.vidioc_enum_input		= ep800_enum_input,
	.vidioc_g_input			= ep800_g_input,
	.vidioc_s_input			= ep800_s_input,
	.vidioc_g_parm			= ep800_g_parm,
	.vidioc_s_parm			= ep800_s_parm,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations ep800_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

static int ep800_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ep800 *dev =
		container_of(ctrl->handler, struct ep800, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		dev->brightness = ctrl->val;
		break;
	case V4L2_CID_CONTRAST:
		dev->contrast = ctrl->val;
		break;
	case V4L2_CID_SATURATION:
		dev->saturation = ctrl->val;
		break;
	case V4L2_CID_HUE:
		dev->hue = ctrl->val;
		break;
	default:
		return -EINVAL;
	}
	if (dev->streaming)
		ep800_adjust_pict(dev);
	return 0;
}

static const struct v4l2_ctrl_ops ep800_ctrl_ops = {
	.s_ctrl = ep800_s_ctrl,
};

/* ------------------------------------------------------------------ */
/* Probe / disconnect                                                   */
/* ------------------------------------------------------------------ */

static int ep800_init_device(struct ep800 *dev)
{
	u8 cp[0x80];
	int rc, i;

	ep800_ctrl(dev, true, EP800_VENDOR_REQ_LED_CONTROL, 1, NULL, 0);
	memset(cp, 0, sizeof(cp));
	rc = ep800_ctrl(dev, false, EP800_VENDOR_REQ_CAMERA_INFO, 0,
			cp, sizeof(cp));
	if (rc < 0) {
		dev_err(&dev->intf->dev, "CAMERA_INFO failed: %d\n", rc);
		return rc;
	}

	dev->camid = ep800_qt2int(cp + 2);
	dev->maxwidth = ep800_qt2int(cp + 6);
	dev->maxheight = ep800_qt2int(cp + 8);

	dev_info(&dev->intf->dev,
		 "camid=0x%x max=%dx%d formats=%u\n",
		 dev->camid, dev->maxwidth, dev->maxheight,
		 ep800_qt2int(cp + 14));

	if (dev->camid != EP800_CAMID_EP800 &&
	    dev->camid != EP800_CAMID_SE402 &&
	    dev->camid != EP800_CAMID_SE401) {
		dev_err(&dev->intf->dev, "unsupported camid 0x%x\n",
			dev->camid);
		return -ENODEV;
	}

	/* Prefer CIF unless sensor reports less. */
	dev->width = 176;
	dev->height = 144;
	if (dev->maxwidth < 176 || dev->maxheight < 144) {
		dev->width = dev->maxwidth;
		dev->height = dev->maxheight;
	}

	for (i = 0; i < ep800_qt2int(cp + 14); i++) {
		if (ep800_qt2int(cp + 16 + i * 2) == EP800_FORMAT_BAYER)
			dev_info(&dev->intf->dev, "Bayer format reported\n");
	}

	ep800_sensor_init(dev);

	dev->brightness = 32767;
	dev->contrast = 32767;
	dev->saturation = 32767;
	dev->hue = 32767;
	dev->rgain = 0x3f;
	dev->ggain = 0x3f;
	dev->bgain = 0x3f;

	/* LED blink to confirm talk */
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_CAM_POWER, 1, NULL, 0);
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_LED_CONTROL, 1, NULL, 0);
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_CAM_POWER, 0, NULL, 0);
	ep800_ctrl(dev, true, EP800_VENDOR_REQ_LED_CONTROL, 0, NULL, 0);
	return 0;
}

static void ep800_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct ep800 *dev = container_of(v4l2_dev, struct ep800, v4l2_dev);

	cancel_work_sync(&dev->decode_work);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	kfree(dev->ctrl_buf);
	vfree(dev->rgb);
	kfree(dev);
}

static int ep800_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct ep800 *dev;
	struct video_device *vdev;
	int ret;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = usb_get_dev(udev);
	dev->intf = intf;
	dev->iface = intf->cur_altsetting->desc.bInterfaceNumber;
	mutex_init(&dev->lock);
	spin_lock_init(&dev->qlock);
	INIT_LIST_HEAD(&dev->buf_list);
	INIT_WORK(&dev->decode_work, ep800_decode_work);
	dev->v4l2_dev.release = ep800_v4l2_release;

	dev->ctrl_buf_size = 128;
	dev->ctrl_buf = kmalloc(dev->ctrl_buf_size, GFP_KERNEL);
	if (!dev->ctrl_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	ret = v4l2_device_register(&intf->dev, &dev->v4l2_dev);
	if (ret)
		goto err_ctrlbuf;

	ret = ep800_init_device(dev);
	if (ret)
		goto err_v4l2;

	v4l2_ctrl_handler_init(&dev->ctrl_handler, 4);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &ep800_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, 0, 65535, 1, 32767);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &ep800_ctrl_ops,
			  V4L2_CID_CONTRAST, 0, 65535, 1, 32767);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &ep800_ctrl_ops,
			  V4L2_CID_SATURATION, 0, 65535, 1, 32767);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &ep800_ctrl_ops,
			  V4L2_CID_HUE, 0, 65535, 1, 32767);
	if (dev->ctrl_handler.error) {
		ret = dev->ctrl_handler.error;
		goto err_ctrl;
	}
	dev->v4l2_dev.ctrl_handler = &dev->ctrl_handler;

	dev->vb_queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dev->vb_queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	dev->vb_queue.drv_priv = dev;
	dev->vb_queue.buf_struct_size = sizeof(struct ep800_buffer);
	dev->vb_queue.ops = &ep800_vb2_ops;
	dev->vb_queue.mem_ops = &vb2_vmalloc_memops;
	dev->vb_queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	dev->vb_queue.lock = &dev->lock;
	dev->vb_queue.min_queued_buffers = 2;
	ret = vb2_queue_init(&dev->vb_queue);
	if (ret)
		goto err_ctrl;

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_ctrl;
	}
	*vdev = (struct video_device){
		.fops = &ep800_fops,
		.ioctl_ops = &ep800_ioctl_ops,
		.release = video_device_release,
		.vfl_dir = VFL_DIR_RX,
		.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			       V4L2_CAP_READWRITE,
		.v4l2_dev = &dev->v4l2_dev,
		.queue = &dev->vb_queue,
		.lock = &dev->lock,
	};
	strscpy(vdev->name, "Creative PD1001", sizeof(vdev->name));
	video_set_drvdata(vdev, dev);
	dev->vdev = vdev;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, video_nr);
	if (ret)
		goto err_vdev;

	ret = ep800_button_init(dev);
	if (ret)
		goto err_unreg;

	usb_set_intfdata(intf, dev);
	/* Extra ref: dropped in disconnect after unregister. */
	v4l2_device_get(&dev->v4l2_dev);
	dev_info(&intf->dev, "registered as %s\n",
		 video_device_node_name(vdev));
	return 0;

err_unreg:
	video_unregister_device(vdev);
	vdev = NULL;
err_vdev:
	if (vdev)
		video_device_release(vdev);
err_ctrl:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_ctrlbuf:
	kfree(dev->ctrl_buf);
err_put:
	usb_put_dev(dev->udev);
	kfree(dev);
	return ret;
}

static void ep800_disconnect(struct usb_interface *intf)
{
	struct ep800 *dev = usb_get_intfdata(intf);
	struct usb_device *udev;

	usb_set_intfdata(intf, NULL);
	if (!dev)
		return;

	/*
	 * Do not call ep800_stop_stream() while holding dev->lock here.
	 * Release paths take the same lock and may be in usb_kill_urb();
	 * waiting on the lock from disconnect deadlocks USB unbind (D state).
	 */
	mutex_lock(&dev->lock);
	udev = dev->udev;
	dev->udev = NULL;
	dev->streaming = false;
	video_unregister_device(dev->vdev);
	v4l2_device_disconnect(&dev->v4l2_dev);
	mutex_unlock(&dev->lock);

	ep800_button_free(dev);
	ep800_free_urbs(dev);
	cancel_work_sync(&dev->decode_work);

	if (udev)
		usb_put_dev(udev);

	v4l2_device_put(&dev->v4l2_dev);
}

static const struct usb_device_id ep800_id_table[] = {
	{ USB_DEVICE(0x041e, 0x400d) },	/* Creative PD1001 */
	{ USB_DEVICE(0x03e8, 0x1005) },	/* Endpoints EP800 reference */
	{ }
};
MODULE_DEVICE_TABLE(usb, ep800_id_table);

static struct usb_driver ep800_driver = {
	.name		= "ep800",
	.id_table	= ep800_id_table,
	.probe		= ep800_probe,
	.disconnect	= ep800_disconnect,
};

module_usb_driver(ep800_driver);
