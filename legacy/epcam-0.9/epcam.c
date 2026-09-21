/*
 * Endpoints EPCAM USB Camera Driver
 *
 * Copyright (c) 2003, 2004 Jeroen B. Vreeken (pe1rxq@amsat.org)
 *
 * Based on the se401 driver, which in turn is based on the ov511 driver.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Thanks to Endpoints Inc. (www.endpoints.com) for making documentation on
 * their chips available and sending me two reference boards.
 * 	- Jeroen Vreeken
 *
 * Modified by Fabio Mauro 2006 and Antoine DEBOURG
 * works fine with Ubuntu
 *
 * Modified by Djordje Stanarevic 2008 	- Version 0.08 vastly improved
 *                                      - tested with Creative PD1001 webcam
 *                                      - some code based on other linux drivers 
 *                                        (stv680,quickcam_messanger,usbvideo)
 *                                      - Version 0.08.2 added sysfs support
 *					  for kernel 2.6.23 and older 
 *					- Version 0.08.3 re-enabled reading of
 *					  data frames with epcam_read
 * Modified by Djordje Stanarevic 12/2008 - made changes introduced by kernel 2.6.27 
 * 					    to v4l 
 * Modified by Djordje Staarevic  11/2009 - implemented changes introduced after 2.6.27
 *                                          kernel.Added V4L2 support.
 */

static const char version[] = "0.09.0";
#define MAJOR_VERSION	0
#define MINOR_VERSION	9	
#define RELEASE_VERSION	0	
#define __OLD_VIDIOC_	1
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/usb.h>


#include "epcam.h"

static int video_nr=-1;
static int epcam_debug=0;
static int BGRon=0;

static void epcam_remove_disconnected(struct usb_epcam *epcam);
static inline int remap_page_range(struct vm_area_struct *vma,
unsigned long uvaddr,
unsigned long paddr,
unsigned long size, pgprot_t prot)
{
return remap_pfn_range(vma, uvaddr, paddr >> PAGE_SHIFT, size, prot);
}



static struct usb_device_id device_table [] = {
	{ USB_DEVICE(0x03e8, 0x1005), driver_info: (unsigned long)"Endpoints EP800"	},/* Reference model */
	{ USB_DEVICE(0x03e8, 0x1003), driver_info: (unsigned long)"Endpoints SE402"	},/* Reference model */
	{ USB_DEVICE(0x03e8, 0x1000), driver_info: (unsigned long)"Endpoints SE401"	},/* Reference model */
	{ USB_DEVICE(0x03e8, 0x2112), driver_info: (unsigned long)"SpyPen Actor" 	},
	{ USB_DEVICE(0x03e8, 0x2040), driver_info: (unsigned long)"Rimax Slim Multicam"	},
	{ USB_DEVICE(0x03e8, 0x1010), driver_info: (unsigned long)"Concord Eye-Q Easy"	},
	{ USB_DEVICE(0x041e, 0x400d), driver_info: (unsigned long)"Creative PD1001"	},
	{ USB_DEVICE(0x04f2, 0xa001), driver_info: (unsigned long)"Chicony DC-100"	},
	{ USB_DEVICE(0x08ca, 0x0102), driver_info: (unsigned long)"Aiptek Pencam 400"	},
	{ USB_DEVICE(0x03e8, 0x2182), driver_info: (unsigned long)"Concord EyeQ Mini"	},
	{ USB_DEVICE(0x03e8, 0x2123), driver_info: (unsigned long)"Sipix StyleCam"	},
	{}
};

MODULE_DEVICE_TABLE(usb, device_table);

MODULE_AUTHOR("Jeroen Vreeken <pe1rxq@amsat.org>.Patched by Djordje Stanarevic.");
MODULE_DESCRIPTION("EPcam USB Camera Driver");
MODULE_LICENSE("GPL");

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0)
module_param(video_nr, int, 0644);
module_param(epcam_debug, int, 0644);
module_param(BGRon, int, 0644);
#else
MODULE_PARM(video_nr, "i");
MODULE_PARM(epcam_debug, "i");
MODULE_PARM(BGRon, "i");
#endif

MODULE_DEVICE_TABLE(usb, device_table);
MODULE_PARM_DESC(video_nr,"Video device number /dev/video<nr> , default value is -1 (picks first available)");
MODULE_PARM_DESC(epcam_debug,"Debug messages, default value is 0 (no messages)");
MODULE_PARM_DESC(BGRon,"Convert RGB24 to BGR24, default value is 0 (turned off)");

static struct usb_driver epcam_driver;


/**********************************************************************
 *
 * Memory management
 *
 **********************************************************************/

/* Here we want the physical address of the memory.
 * This is used when initializing the contents of the area.
 */
static inline unsigned long kvirt_to_pa(unsigned long adr)
{
	unsigned long kva, ret;

	kva = (unsigned long) page_address(vmalloc_to_page((void *)adr));
	kva |= adr & (PAGE_SIZE-1); /* restore the offset */
	ret = __pa(kva);
	return ret;
}

static void *rvmalloc(unsigned long size)
{
	void *mem;
	unsigned long adr;

	size = PAGE_ALIGN(size);
	mem = vmalloc_32(size);
	if (!mem)
		return NULL;

	memset(mem, 0, size); /* Clear the ram out, no junk to the user */
	adr = (unsigned long) mem;
	while (size > 0) {
		SetPageReserved(vmalloc_to_page((void *)adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	return mem;
}

static void rvfree(void *mem, unsigned long size)
{
	unsigned long adr;

	if (!mem)
		return;

	adr = (unsigned long) mem;
	while ((long) size > 0) {
		ClearPageReserved(vmalloc_to_page((void *)adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}
	vfree(mem);
}



/****************************************************************************
 *
 * epcam register read/write functions
 *
 ***************************************************************************/

static int epcam_sndctrl(int set, struct usb_epcam *epcam, unsigned short req,
			 unsigned short value, unsigned char *cp, int size)
{
	return usb_control_msg (
                epcam->dev,
                set ? usb_sndctrlpipe(epcam->dev, 0) : usb_rcvctrlpipe(epcam->dev, 0),
                req,
                (set ? USB_DIR_OUT : USB_DIR_IN) | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                value,
                0,
                cp,
                size,
                HZ
        );
}

static int epcam_set_feature(struct usb_epcam *epcam, unsigned short reg,
			     unsigned short value)
{
	unsigned char cp[2];
	INT2QT(value, cp);
	return usb_control_msg (
	epcam->dev,
		usb_sndctrlpipe(epcam->dev, 0),
		VENDOR_REQ_EXT_FEATURE,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		reg,
		0,
		cp,
		2,
		HZ
	);
}

/*static unsigned short epcam_get_feature(struct usb_epcam *epcam, 
				        unsigned short reg)
{
	unsigned char cp[2];
	usb_control_msg (
		epcam->dev,
		usb_rcvctrlpipe(epcam->dev, 0),
		VENDOR_REQ_EXT_FEATURE,
		USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		reg,
		0,
		cp,
		2,
		HZ
		);
	return QT2INT(cp);
}*/

static unsigned short epcam_read_bios(struct usb_epcam *epcam,
					unsigned short address)
{
	unsigned char cp[2];
	usb_control_msg (
		epcam->dev,
		usb_rcvctrlpipe(epcam->dev, 0),
		VENDOR_REQ_BIOS,
		USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		VENDOR_CMD_BIOS_READ,
		address,
		cp,
		2,
		HZ
	);
	return QT2INT(cp);
}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
/****************************************************************************
 *  sysfs
 ****************************************************************************/
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
#define epcam_file(name, variable, field)                              \
static ssize_t show_##name(struct device *class_dev,                    \
                           struct device_attribute *attr, char *buf)    \
{                                                                       \
        struct video_device *vdev = to_video_device(class_dev);         \
        struct usb_epcam *epcam = video_get_drvdata(vdev);              \
        return sprintf(buf, field, epcam->variable);                    \
}                                                                       \
static DEVICE_ATTR(name, S_IRUGO, show_##name, NULL);
#else
#define epcam_file(name, variable, field)                              \
static ssize_t show_##name(struct class_device *class_dev, char *buf)	\
{                                                                       \
        struct video_device *vdev = to_video_device(class_dev);         \
        struct usb_epcam *epcam = video_get_drvdata(vdev);              \
        return sprintf(buf, field, epcam->variable);                    \
}                                                                       \
static CLASS_DEVICE_ATTR(name, S_IRUGO, show_##name, NULL);
#endif
epcam_file(model, camera_name, "%s\n");
epcam_file(in_use, user, "%d\n");
epcam_file(streaming, streaming, "%d\n");
epcam_file(palette, palette, "%i\n");
epcam_file(frames_total, readcount, "%d\n");
epcam_file(frames_read, framecount, "%d\n");
epcam_file(packets_dropped, dropped, "%d\n");

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
static int epcam_create_sysfs_files(struct video_device *vdev)
{
        int rc;

        rc = device_create_file(&vdev->dev, &dev_attr_model);
        if (rc) goto err;
        rc = device_create_file(&vdev->dev, &dev_attr_in_use);
        if (rc) goto err_model;
        rc = device_create_file(&vdev->dev, &dev_attr_streaming);
        if (rc) goto err_inuse;
        rc = device_create_file(&vdev->dev, &dev_attr_palette);
        if (rc) goto err_stream;
        rc = device_create_file(&vdev->dev, &dev_attr_frames_total);
        if (rc) goto err_pal;
        rc = device_create_file(&vdev->dev, &dev_attr_frames_read);
        if (rc) goto err_framtot;
        rc = device_create_file(&vdev->dev, &dev_attr_packets_dropped);
        if (rc) goto err_framread;

        return 0;
	
        device_remove_file(&vdev->dev, &dev_attr_packets_dropped);
err_framread:
        device_remove_file(&vdev->dev, &dev_attr_frames_read);
err_framtot:
        device_remove_file(&vdev->dev, &dev_attr_frames_total);
err_pal:
        device_remove_file(&vdev->dev, &dev_attr_palette);
err_stream:
        device_remove_file(&vdev->dev, &dev_attr_streaming);
err_inuse:
        device_remove_file(&vdev->dev, &dev_attr_in_use);
err_model:
        device_remove_file(&vdev->dev, &dev_attr_model);
err:
        return rc;
}

static void epcam_remove_sysfs_files(struct video_device *vdev)
{
        device_remove_file(&vdev->dev, &dev_attr_model);
        device_remove_file(&vdev->dev, &dev_attr_in_use);
        device_remove_file(&vdev->dev, &dev_attr_streaming);
        device_remove_file(&vdev->dev, &dev_attr_palette);
        device_remove_file(&vdev->dev, &dev_attr_frames_total);
        device_remove_file(&vdev->dev, &dev_attr_frames_read);
        device_remove_file(&vdev->dev, &dev_attr_packets_dropped);
}
#else
static int epcam_create_sysfs_files(struct video_device *vdev)
{
        int rc;

        rc = video_device_create_file(vdev, &dev_attr_model);
        if (rc) goto err;
        rc = video_device_create_file(vdev, &dev_attr_in_use);
        if (rc) goto err_model;
        rc = video_device_create_file(vdev, &dev_attr_streaming);
        if (rc) goto err_inuse;
        rc = video_device_create_file(vdev, &dev_attr_palette);
        if (rc) goto err_stream;
        rc = video_device_create_file(vdev, &dev_attr_frames_total);
        if (rc) goto err_pal;
        rc = video_device_create_file(vdev, &dev_attr_frames_read);
        if (rc) goto err_framtot;
        rc = video_device_create_file(vdev, &dev_attr_packets_dropped);
        if (rc) goto err_framread;

        return 0;
	
        video_device_remove_file(vdev, &dev_attr_packets_dropped);
err_framread:
        video_device_remove_file(vdev, &dev_attr_frames_read);
err_framtot:
        video_device_remove_file(vdev, &dev_attr_frames_total);
err_pal:
        video_device_remove_file(vdev, &dev_attr_palette);
err_stream:
        video_device_remove_file(vdev, &dev_attr_streaming);
err_inuse:
        video_device_remove_file(vdev, &dev_attr_in_use);
err_model:
        video_device_remove_file(vdev, &dev_attr_model);
err:
        return rc;
}

static void epcam_remove_sysfs_files(struct video_device *vdev)
{
        video_device_remove_file(vdev, &dev_attr_model);
        video_device_remove_file(vdev, &dev_attr_in_use);
        video_device_remove_file(vdev, &dev_attr_streaming);
        video_device_remove_file(vdev, &dev_attr_palette);
        video_device_remove_file(vdev, &dev_attr_frames_total);
        video_device_remove_file(vdev, &dev_attr_frames_read);
        video_device_remove_file(vdev, &dev_attr_packets_dropped);
}
#endif
#else
static int epcam_create_sysfs_files(struct video_device *vdev)
{
        int rc;

        rc = video_device_create_file(vdev, &class_device_attr_model);
        if (rc) goto err;
        rc = video_device_create_file(vdev, &class_device_attr_in_use);
        if (rc) goto err_model;
        rc = video_device_create_file(vdev, &class_device_attr_streaming);
        if (rc) goto err_inuse;
        rc = video_device_create_file(vdev, &class_device_attr_palette);
        if (rc) goto err_stream;
        rc = video_device_create_file(vdev, &class_device_attr_frames_total);
        if (rc) goto err_pal;
        rc = video_device_create_file(vdev, &class_device_attr_frames_read);
        if (rc) goto err_framtot;
        rc = video_device_create_file(vdev, &class_device_attr_packets_dropped);
        if (rc) goto err_framread;

        return 0;
	
        video_device_remove_file(vdev, &class_device_attr_packets_dropped);
err_framread:
        video_device_remove_file(vdev, &class_device_attr_frames_read);
err_framtot:
        video_device_remove_file(vdev, &class_device_attr_frames_total);
err_pal:
        video_device_remove_file(vdev, &class_device_attr_palette);
err_stream:
        video_device_remove_file(vdev, &class_device_attr_streaming);
err_inuse:
        video_device_remove_file(vdev, &class_device_attr_in_use);
err_model:
        video_device_remove_file(vdev, &class_device_attr_model);
err:
        return rc;
}

static void epcam_remove_sysfs_files(struct video_device *vdev)
{
        video_device_remove_file(vdev, &class_device_attr_model);
        video_device_remove_file(vdev, &class_device_attr_in_use);
        video_device_remove_file(vdev, &class_device_attr_streaming);
        video_device_remove_file(vdev, &class_device_attr_palette);
        video_device_remove_file(vdev, &class_device_attr_frames_total);
        video_device_remove_file(vdev, &class_device_attr_frames_read);
        video_device_remove_file(vdev, &class_device_attr_packets_dropped);
}
#endif
#endif
/****************************************************************************
 *
 * Camera control
 *
 ***************************************************************************/

/* taken from usbvideo driver */
void epcam_hexdump(const unsigned char *data, int len)
{
        const int bytes_per_line = 32;
        char tmp[128]; /* 32*3 + 5 */
        int i, k;

        for (i=k=0; len > 0; i++, len--) {
                if (i > 0 && ((i % bytes_per_line) == 0)) {
                        debugprintk(KERN_INFO,"%s\n", tmp);
                        k=0;
                }
                if ((i % bytes_per_line) == 0)
                        k += sprintf(&tmp[k], "%04x: ", i);
                k += sprintf(&tmp[k], "%02x ", data[i]);
        }
        if (k > 0)
                debugprintk(KERN_INFO,"%s\n", tmp);
}

/* taken from quickcam_messanger driver */
static void epcam_hsv2rgb(u16 hue, u16 sat, u16 val, u16 *r, u16 *g, u16 *b)
{
	unsigned int segment, valsat;
	signed int   h = (signed int) hue;
	unsigned int s = (sat - 32768) * 2;	/* rescale */
	unsigned int v = val;
	unsigned int p;

	/*
	the registers controling gain are 8 bit of which
	we affect only the last 4 bits with our gain.
	we know that if saturation is 0, (unsaturated) then
	we're grayscale (center axis of the colour cone) so
	we set rgb=value. we use a formula obtained from
	wikipedia to map the cone to the RGB plane. it's
	as follows for the human value case of h=0..360,
	s=0..1, v=0..1
	h_i = h/60 % 6 , f = h/60 - h_i , p = v(1-s)
	q = v(1 - f*s) , t = v(1 - (1-f)s)
	h_i==0 => r=v , g=t, b=p
	h_i==1 => r=q , g=v, b=p
	h_i==2 => r=p , g=v, b=t
	h_i==3 => r=p , g=q, b=v
	h_i==4 => r=t , g=p, b=v
	h_i==5 => r=v , g=p, b=q
	the bottom side (the point) and the stuff just up
	of that is black so we simplify those two cases.
	*/
	if (sat < 32768) {
		/* anything less than this is unsaturated */
		*r = val;
		*g = val;
		*b = val;
		return;
	}
	if (val <= (0xFFFF/8)) {
		/* anything less than this is black */
		*r = 0;
		*g = 0;
		*b = 0;
		return;
	}

	/* the rest of this code is copying tukkat's
	implementation of the hsv2rgb conversion as taken
	from qc-usb-messenger code. the 10923 is 0xFFFF/6
	to divide the cone into 6 sectors.  */

	segment = (h + 10923) & 0xFFFF;
	segment = segment*3 >> 16;		/* 0..2: 0=R, 1=G, 2=B */
	hue -= segment * 21845;			/* -10923..10923 */
	h = hue;
	h *= 3;
	valsat = v*s >> 16;			/* 0..65534 */
	p = v - valsat;
	if (h >= 0) {
		unsigned int t = v - (valsat * (32769 - h) >> 15);
		switch (segment) {
		case 0:	/* R-> */
			*r = v;
			*g = t;
			*b = p;
			break;
		case 1:	/* G-> */
			*r = p;
			*g = v;
			*b = t;
			break;
		case 2:	/* B-> */
			*r = t;
			*g = p;
			*b = v;
			break;
		}
	} else {
		unsigned int q = v - (valsat * (32769 + h) >> 15);
		switch (segment) {
		case 0:	/* ->R */
			*r = v;
			*g = p;
			*b = q;
			break;
		case 1:	/* ->G */
			*r = q;
			*g = v;
			*b = p;
			break;
		case 2:	/* ->B */
			*r = p;
			*g = q;
			*b = v;
			break;
		}
	}
}

/*static int epcam_send_pict(struct usb_epcam *epcam)
{
	unsigned char cp[40];
	int size;

	if (epcam->brightness<4096)
		epcam->brightness=4096;
*/	
//	epcam_set_feature(epcam, HV7131_REG_TITU, epcam->brightness>>8);
//	epcam_set_feature(epcam, HV7131_REG_TITM, epcam->brightness&0xff);
//	epcam_set_feature(epcam, HV7131_REG_TITL, 0);

	/* the SE402 does this itself, don't bother it... */
/*	if (epcam->camid!=0x402) {
		epcam_sndctrl(0, epcam, VENDOR_REQ_IMAGE_INFO, 0, cp, sizeof(cp));
		INT2QT(epcam->brightness>>5, cp+2);
		size=QT2INT(cp);
		if (size>40)
			size=40;
		epcam_sndctrl(1, epcam, VENDOR_REQ_IMAGE_INFO, 0, cp, size);
	}

//	info("TIT: %d", 256*256*epcam_get_feature(epcam, HV7131_REG_TITU)+
//	    256*epcam_get_feature(epcam, HV7131_REG_TITM)+
//	    epcam_get_feature(epcam, HV7131_REG_TITL));

	return 0;
}*/

static void adjust_pict(struct usb_epcam *epcam)
{
	unsigned int exposure;
	unsigned int percentage;	
	u16 r=0,g=0,b=0;

	epcam_hsv2rgb(epcam->hue, epcam->colour, epcam->contrast, &r, &g, &b);
	r>>=10;
	b>>=10;
	g>>=10;
	r=0x40-r;
	b=0x40-b;
	g=0x40-g;

	percentage=(epcam->brightness*100)>>16;
	exposure = (EPCAM_MAX_EXPOSURE)*percentage/100;
	exposure=min(exposure,(unsigned int)EPCAM_MAX_EXPOSURE);
	exposure=max(exposure,(unsigned int)EPCAM_MIN_EXPOSURE);
        epcam_set_feature(epcam, HV7131_REG_MODE_B,0x5);
	epcam_set_feature(epcam, HV7131_REG_TITU, exposure>>16);
	epcam_set_feature(epcam, HV7131_REG_TITM, (exposure>>8) & 0xff);
	epcam_set_feature(epcam, HV7131_REG_TITL, exposure & 0xff);
        epcam_set_feature(epcam, HV7131_REG_MODE_C,0xa);
        epcam_set_feature(epcam, HV7131_REG_ARCG, epcam->rgain);
        epcam_set_feature(epcam, HV7131_REG_AGCG, epcam->ggain);
        epcam_set_feature(epcam, HV7131_REG_ABCG, epcam->bgain);
        epcam_set_feature(epcam, HV7131_REG_OFSR, r);
        epcam_set_feature(epcam, HV7131_REG_OFSG, g);
        epcam_set_feature(epcam, HV7131_REG_OFSB, b);
	epcam->chgsettings=0;
}

/*static int epcam_recv_pict(struct usb_epcam *epcam)
{
	unsigned char cp[40];
	    	
	if (epcam->camid!=0x402) {
		epcam_sndctrl(0, epcam, VENDOR_REQ_IMAGE_INFO, 0, cp, sizeof(cp));
		epcam->brightness=QT2INT(cp+2)<<5;
	}

//	epcam->brightness=
//	    epcam_get_feature(epcam, HV7131_REG_TITU)*256+
//	    epcam_get_feature(epcam, HV7131_REG_TITM);
//	epcam_get_feature(epcam, HV7131_REG_TITL);

	return 0;
}*/

static int epcam_get_pict(struct usb_epcam *epcam, struct video_picture *p)
{
	/*epcam_recv_pict(epcam);*/
	p->brightness=epcam->brightness;
	p->whiteness=epcam->whiteness;
	p->colour=epcam->colour;
	p->contrast=epcam->contrast;
	p->hue=epcam->hue;
	p->palette=epcam->palette;
	p->depth=24; /* rgb24 */

	return 0;
}


static int epcam_set_pict(struct usb_epcam *epcam, struct video_picture *p)
{
	if (p->palette != VIDEO_PALETTE_RGB24 && p->palette != VIDEO_PALETTE_RGB565) {
		/*epcam->palette=p->palette;*/
		debugprintk(KERN_INFO,"Palette: %d not supported\n", p->palette);
		return 1;
	}
	else
	{
		if(p->palette == VIDEO_PALETTE_RGB565)
			BGRon=1;
		else
			BGRon=0;
	}

	epcam->whiteness=p->whiteness;
	epcam->colour = p->colour;
	epcam->contrast = p->contrast;
	epcam->hue=p->hue;
        epcam->brightness=p->brightness;
	epcam->palette=p->palette;
	epcam->chgsettings=1;
	/*epcam_send_pict(epcam);*/
	return 0;
}

/*
	Hyundai have some really nice docs about this and other sensor related
	stuff on their homepage: www.hei.co.kr
*/
/*static void epcam_auto_resetlevel(struct usb_epcam *epcam)
{
	unsigned int ahrc, alrc;
	int oldreset=epcam->resetlevel;*/

	/* For some reason these normally read-only registers don't get reset
	   to zero after reading them just once...
	 */
/*	epcam_get_feature(epcam, HV7131_REG_HIREFNOH); 
	epcam_get_feature(epcam, HV7131_REG_HIREFNOL);
	epcam_get_feature(epcam, HV7131_REG_LOREFNOH);
	epcam_get_feature(epcam, HV7131_REG_LOREFNOL);
	ahrc=256*epcam_get_feature(epcam, HV7131_REG_HIREFNOH) + 
	    epcam_get_feature(epcam, HV7131_REG_HIREFNOL);
	alrc=256*epcam_get_feature(epcam, HV7131_REG_LOREFNOH) +
	    epcam_get_feature(epcam, HV7131_REG_LOREFNOL);*/

	/* Not an exact science, but it seems to work pretty well... */
/*	if (alrc > 10) {
		while (alrc>=10 && epcam->resetlevel < 63) {
			epcam->resetlevel++;
			alrc /=2;
		}
	} else if (ahrc > 20) {
		while (ahrc>=20 && epcam->resetlevel > 0) {
			epcam->resetlevel--;
			ahrc /=2;
		}
	}
	if (epcam->resetlevel!=oldreset)
		epcam_set_feature(epcam, HV7131_REG_ARLV, epcam->resetlevel);

	return;
}*/

/* irq handler for snapshot button */
static void epcam_button_irq(struct urb *urb)
{
        struct usb_epcam *epcam = urb->context;
        int status;

        if (!epcam->dev) {
                debugprintk(KERN_INFO,"ohoh: device vapourished\n");
                return;
        }

        switch (urb->status) {
        case 0:
                /* success */
                break;
        case -ECONNRESET:
        case -ENOENT:
        case -ESHUTDOWN:
                /* this urb is terminated, clean up */
                dbg("%s - urb shutting down with status: %d", __FUNCTION__, urb->status);
                return;
        default:
                dbg("%s - nonzero urb status received: %d", __FUNCTION__, urb->status);
                goto exit;
        }

        if (urb->actual_length >=2) {
                if (epcam->button)
                        epcam->buttonpressed=1;
        }
exit:
        status = usb_submit_urb (urb, GFP_ATOMIC);
        if (status)
                err ("%s - usb_submit_urb failed with result %d",
                     __FUNCTION__, status);
}

static void epcam_video_irq(struct urb *urb, struct pt_regs *regs)
{
	struct usb_epcam *epcam=urb->context;
	int length=0, i;

	/* ohoh... */
	if (!epcam->streaming) {
		return;
	}
	if (!urb) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
		info ("ohoh: null urb");
#else
		dev_info(&urb->dev->dev,"ohoh: null urb");
#endif
		return;
	}
	if (!epcam->dev) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
		info ("ohoh: device vapourished");
#else
		dev_info (&urb->dev->dev,"ohoh: device vapourished");
#endif
		return;
	}

	switch(epcam->scratch[epcam->scratch_next].state) {
		case BUFFER_READY:
		case BUFFER_BUSY: {
			epcam->dropped++;
			break;
		}
		case BUFFER_UNUSED: {
			for (i=0; i<EPCAM_PACKETBUFS; i++) {
				if (urb->iso_frame_desc[i].actual_length) {
					epcam->nullpackets=0;
					memcpy(
					 epcam->scratch[epcam->scratch_next].data+length,
					 (unsigned char *)urb->transfer_buffer+urb->iso_frame_desc[i].offset,
					 urb->iso_frame_desc[i].actual_length);
					length+=urb->iso_frame_desc[i].actual_length;

				}
			}
			if (length) {
				epcam->scratch[epcam->scratch_next].state=BUFFER_READY;
				epcam->scratch[epcam->scratch_next].offset=epcam->scratch_offset;
				epcam->scratch[epcam->scratch_next].length=length;
				if (waitqueue_active(&epcam->wq))
					wake_up_interruptible(&epcam->wq);
				epcam->scratch_overflow=0;
				epcam->scratch_next++;
				if (epcam->scratch_next>=EPCAM_NUMSCRATCH)
					epcam->scratch_next=0;
			}
		}
	}

	if (length)
		epcam->scratch_offset++;
	else
		epcam->scratch_offset=0;
	urb->dev = epcam->dev;
	if ((i = usb_submit_urb(urb, GFP_ATOMIC)) != 0)
	{
		debugprintk(KERN_INFO,"usb_submit_urb() returnd %d\n", i);
		epcam_remove_disconnected(epcam);
	}

	return;
}

static void epcam_send_size(struct usb_epcam *epcam, int width, int height)
{
	unsigned char cp[40];
	int compress=0;
	int size;
	
	/* Only set auto controls on cameras that support it... */
	if (epcam->camid==0x402)
		epcam_sndctrl(0, epcam, VENDOR_REQ_AUTO_CONTROL, 1, NULL, 0);
	
	epcam->format=FMT_BAYER;
	if (epcam->camid==0x800) {
		/* Don't use compression on small images: camera goes crazy */
		/*if (width*height>=38400) {*/
			compress=0x02;
			epcam->format=FMT_EPLITE;
			/*if (width <= epcam->maxwidth/2 &&
			    height <= epcam->maxheight/2) {
				width*=2;
				height*=2;
				compress|=0x80;
			}*/
		/*}*/
	}
	memset(cp,0,sizeof(cp));
	size = epcam_sndctrl(0, epcam, VENDOR_REQ_CAPTURE_INFO, 0, cp, sizeof(cp));
	debugprintk(KERN_INFO,"vendor_req_capture_info: %d\n", size);
	epcam_hexdump(cp,sizeof(cp));
	INT2QT(1, cp+2);
	/*INT2QT(0, cp+4);
	INT2QT(0, cp+6);*/
	INT2QT((epcam->maxwidth-width)/2, cp+4);
	INT2QT((epcam->maxheight-height)/2, cp+6);
	INT2QT(width, cp+8);
	INT2QT(height, cp+10);
	/*size=QT2INT(cp);
	if (size>40)
		size=40;*/
	epcam_sndctrl(1, epcam, VENDOR_REQ_CAPTURE_INFO, 0, cp, size);
	size = epcam_sndctrl(0, epcam, VENDOR_REQ_CAPTURE_INFO, 0, cp, sizeof(cp));
	debugprintk(KERN_INFO,"vendor_req_capture_info: %d\n", size);
	epcam_hexdump(cp,sizeof(cp));
	debugprintk(KERN_INFO,"capture info size: %d\n", QT2INT(cp));
	debugprintk(KERN_INFO,"mode     : %d\n", QT2INT(cp+2));
	debugprintk(KERN_INFO,"xstart   : %d\n", QT2INT(cp+4));
	debugprintk(KERN_INFO,"ystart   : %d\n", QT2INT(cp+6));
	debugprintk(KERN_INFO,"width    : %d\n", QT2INT(cp+8));
	debugprintk(KERN_INFO,"height   : %d\n", QT2INT(cp+10));
	debugprintk(KERN_INFO,"framerate: %d\n", QT2INT(cp+12));
	debugprintk(KERN_INFO,"zoom     : %d\n", QT2INT(cp+14));

	memset(cp,0,sizeof(cp));
	debugprintk(KERN_INFO,"vendor_req_compression: %d\n", epcam_sndctrl(0, epcam, VENDOR_REQ_COMPRESSION, 0, cp, sizeof(cp)));
	INT2QT(compress, cp);
	epcam_sndctrl(1, epcam, VENDOR_REQ_COMPRESSION, 0, cp, 2);
	debugprintk(KERN_INFO,"compression: %d\n", compress);

	return;
}

/*
	In this function epcam_send_pict is called several times,
	for some reason (depending on the state of the sensor and the phase of
	the moon :) doing this only in either place doesn't always work...
*/
static int epcam_start_stream(struct usb_epcam *epcam)
{
	struct urb *urb;
	int err=0, i, fx;
	epcam->streaming=1;

debugprintk(KERN_INFO,"starting stream\n");

	/* Set the camera to Iso transfers */
	if (usb_set_interface(epcam->dev, epcam->iface, 4)< 0) {
		debugprintk(KERN_INFO,"interface set failed\n");
		return -1;
	}


debugprintk(KERN_INFO,"interface set\n");
	epcam->packetsize=epcam->dev->config[0].interface[0]->altsetting[4].endpoint[0].desc.wMaxPacketSize;
debugprintk(KERN_INFO,"packetsize: %d\n", epcam->packetsize);

	epcam_sndctrl(1, epcam, VENDOR_REQ_CAM_POWER, 1, NULL, 0);
        epcam_sndctrl(1, epcam, VENDOR_REQ_LED_CONTROL, 1, NULL, 0);
debugprintk(KERN_INFO,"led and power on\n");
	/* Set picture settings */

        adjust_pict(epcam);
        /*epcam_send_pict(epcam);*/

	epcam_send_size(epcam, epcam->cwidth, epcam->cheight);
	

	epcam_sndctrl(1, epcam, VENDOR_REQ_CONT_CAPTURE, 1, NULL, 0);
debugprintk(KERN_INFO,"capture on\n");
	/* Do some memory allocation */
	for (i=0; i<EPCAM_NUMFRAMES; i++) {
		epcam->frame[i].data=epcam->fbuf + i * epcam->maxframesize;
		epcam->frame[i].curpix=0;
	}
	for (i=0; i<EPCAM_NUMSBUF; i++) {
		epcam->sbuf[i].data=kmalloc(epcam->packetsize*EPCAM_PACKETBUFS, GFP_KERNEL);
		if(!epcam->sbuf[i].data) {
			for(i=i-1;i >= 0;i--) {
			kfree(epcam->sbuf[i].data);
			epcam->sbuf[i].data = NULL;
		}
		return -ENOMEM;
		}
	}

	epcam->scratch_offset=0;
	epcam->lastoffset=-1;
	epcam->scratch_next=0;
	epcam->scratch_use=0;
	epcam->scratch_overflow=0;
	for (i=0; i<EPCAM_NUMSCRATCH; i++) {
		epcam->scratch[i].data=kmalloc(epcam->packetsize*EPCAM_PACKETBUFS, GFP_KERNEL);
		if(!epcam->scratch[i].data) {
			for(i = i - 1; i >= 0; i--) {
				kfree(epcam->scratch[i].data);
				epcam->scratch[i].data = NULL;
			}
			goto nomem_sbuf;
		epcam->scratch[i].state=BUFFER_UNUSED;
	}
	}

	for (i=0; i<EPCAM_NUMSBUF; i++) {
		urb=usb_alloc_urb(EPCAM_PACKETBUFS, GFP_KERNEL);
		if(!urb) {
			for(i = i - 1;i >=0 ; i --) {
				usb_kill_urb(epcam->urb[i]);
				usb_free_urb(epcam->urb[i]);
				epcam->urb[i] = NULL;
			}
			goto nomem_scratch;
		}

		urb->dev=epcam->dev;
		urb->context=epcam;
		urb->pipe=usb_rcvisocpipe(epcam->dev, EPCAM_VIDEO_ENDPOINT);
		urb->transfer_flags=URB_ISO_ASAP;
		urb->transfer_buffer=epcam->sbuf[i].data;
		urb->complete=epcam_video_irq;
		urb->number_of_packets=EPCAM_PACKETBUFS;
		urb->transfer_buffer_length=epcam->packetsize*EPCAM_PACKETBUFS;
		urb->interval = 1;
		for (fx=0; fx<EPCAM_PACKETBUFS; fx++) {
			urb->iso_frame_desc[fx].offset=epcam->packetsize*fx;
			urb->iso_frame_desc[fx].length=epcam->packetsize;
		}
		epcam->urb[i]=urb;
	}
	for (i=0; i<EPCAM_NUMSBUF; i++) {
		err=usb_submit_urb(epcam->urb[i], GFP_KERNEL);
		if(err)
			err("usb_submit_urbed  error code %d",err);
	}
	epcam->framecount=0;
debugprintk(KERN_INFO,"urbs flying!\n");
	/*epcam_set_feature(epcam, HV7131_REG_ARLV, epcam->resetlevel);*/

	return 0;

nomem_scratch:
        for (i=0; i<EPCAM_NUMSCRATCH; i++) {
                kfree(epcam->scratch[i].data);
                epcam->scratch[i].data = NULL;
        }
 nomem_sbuf:
        for (i=0; i<EPCAM_NUMSBUF; i++) {
                kfree(epcam->sbuf[i].data);
                epcam->sbuf[i].data = NULL;
        }
        return -ENOMEM;

}

static int epcam_stop_stream(struct usb_epcam *epcam)
{
	int i;

	if (!epcam->streaming || !epcam->dev)
		return 1;

	epcam->streaming=0;

	for (i=0; i<EPCAM_NUMSBUF; i++) if (epcam->urb[i]) {
		usb_unlink_urb(epcam->urb[i]);
		usb_free_urb(epcam->urb[i]);
		epcam->urb[i]=NULL;
		kfree(epcam->sbuf[i].data);
	}
	for (i=0; i<EPCAM_NUMSCRATCH; i++) {
		kfree(epcam->scratch[i].data);
		epcam->scratch[i].data=NULL;
	}
	epcam_sndctrl(1, epcam, VENDOR_REQ_CONT_CAPTURE, 0, NULL, 0);
        epcam_sndctrl(1, epcam, VENDOR_REQ_LED_CONTROL, 0, NULL, 0);
	epcam_sndctrl(1, epcam, VENDOR_REQ_CAM_POWER, 0, NULL, 0);

	return 0;
}

static int epcam_set_size(struct usb_epcam *epcam, int width, int height)
{
	int wasstreaming=epcam->streaming;

	/* Check to see if we need to change */
	if (epcam->cwidth==width && epcam->cheight==height)
		return 0;

	/* Check for a valid mode */
	if (width<=0 || height<=0)
		return 1;
	if (width>epcam->maxwidth)
		return 1;
	if (height>epcam->maxheight)
		return 1;

	/* Stop a current stream and start it again at the new size */
	if (wasstreaming)
		epcam_stop_stream(epcam);
	epcam->cwidth=width;
	epcam->cheight=height;
	if (wasstreaming)
		epcam_start_stream(epcam);

	return 0;
}


/****************************************************************************
 *
 * Video Decoding
 *
 ***************************************************************************/

static inline void decode_bayer (struct usb_epcam *epcam, unsigned char *data, int len)
{
	int datasize=epcam->cwidth*epcam->cheight;
	struct epcam_frame *frame=&epcam->frame[epcam->curframe];

	unsigned char *framedata=frame->data,*temp=frame->data, *curline, *nextline;
	int width=epcam->cwidth;
	int blineoffset=0, bline;
	int linelength=width*3, i;
	unsigned char p=0;
	int x,y;
	
	
	/* Check if we have to much data */
	if (frame->curpix+len > datasize) {
		//debugprintk(KERN_INFO,"to much! %d %d %d %d\n", datasize, frame->curpix, len, frame->curpix+len-datasize);
//		frame->curpix=0;
//		return;
		len=datasize-frame->curpix;
	}

	if (frame->curpix==0) {
		if (frame->grabstate==FRAME_READY) {
			frame->grabstate=FRAME_GRABBING;
		}
		/* Clear stuff the user might have put on this line */
		memset(framedata+datasize*3-linelength, 0, linelength);
		frame->curline=framedata+linelength*2;
		frame->curlinepix=0;
	}

	if (epcam->cheight%4)
		blineoffset=1;
	bline=frame->curpix/epcam->cwidth+blineoffset;

	curline=frame->curline;
	nextline=curline+linelength;
	if (nextline >= framedata+datasize*3)
		nextline=curline;
	while (len) {
		if (frame->curlinepix>=width) {
			frame->curlinepix-=width;
			bline=frame->curpix/width+blineoffset;
			curline+=linelength*2;
			nextline+=linelength*2;
			if (curline >= framedata+datasize*3) {
				frame->curlinepix++;
				curline-=3;
				nextline-=3;
				len--;
				data++;
				frame->curpix++;
			}
			if (nextline >= framedata+datasize*3)
				nextline=curline;
		}
		if ((bline&1)) {
			if ((frame->curlinepix&1)) {
				*(curline+2)=*data;
				*(curline-1)=*data;
				*(nextline+2)=*data;
				*(nextline-1)=*data;
			} else {
				*(curline+1)=
					(*(curline+1)+*data)/2;
				*(curline-2)=
					(*(curline-2)+*data)/2;
				*(nextline+1)=*data;
				*(nextline-2)=*data;
			}
		} else {
			if ((frame->curlinepix&1)) {
				*(curline+1)=
					(*(curline+1)+*data)/2;
				*(curline-2)=
					(*(curline-2)+*data)/2;
				*(nextline+1)=*data;
				*(nextline-2)=*data;
			} else {
				*curline=*data;
				*(curline-3)=*data;
				*nextline=*data;
				*(nextline-3)=*data;
			}
		}
		frame->curlinepix++;
		curline-=3;
		nextline-=3;
		len--;
		data++;
		frame->curpix++;
	}
	frame->curline=curline;

	/*if (frame->curpix>=datasize-epcam->packetsize) {*/
	if (frame->curpix>=datasize) {
		/* Fix the top line */
		framedata+=linelength*2;
		for (i=0; i<linelength*2; i++) {
			framedata--;
			*(framedata)=*(framedata+linelength);
		}
		/* Fix the left side (green is already present) */
		for (i=0; i<epcam->cheight; i++) {
			*framedata=*(framedata+3);
			*(framedata+1)=*(framedata+4);
			*(framedata+2)=*(framedata+5);
			framedata+=linelength;
		}
		framedata-=linelength*2;
		/* Bottom */
		for (i=0; i<linelength*2; i++) {
			framedata++;
			*(framedata)=*(framedata-linelength);
		}

	        if (BGRon) {
        	        for (y = 0; y < epcam->cheight; y++) {
                	        for (x = 0; x < epcam->cwidth; x++) {
                        	        i = (y * epcam->cwidth + x) * 3;
                                	*(temp) = *(frame->data + i);
	                                *(frame->data + i) = *(frame->data + i + 2);
	                                *(frame->data + i + 2) = *(temp);
	                        }
	                }
	        }

        	/* whiteness correction taken from stv680 driver */
                if (epcam->whiteness >= 32767) {
                        p = (epcam->whiteness - 32767) / 256;
                        for (x = 0; x < (epcam->cwidth * epcam->cheight * 3); x++) {
                                if ((*(frame->data + x) + (unsigned char) p) > 255)
                                        *(frame->data + x) = 255;
                                else
                                        *(frame->data + x) += (unsigned char) p;
                        }       /* for */
                } else {
                        p = (32767 - epcam->whiteness) / 256;
                        for (x = 0; x < (epcam->cwidth * epcam->cheight * 3); x++) {
                                if ((unsigned char) p > *(frame->data + x))
                                        *(frame->data + x) = 0;
                                else
                                        *(frame->data + x) -= (unsigned char) p;
                        }       /* for */
                }               /* else */
	
		if(epcam->chgsettings)
			adjust_pict(epcam);
		frame->curpix=0;
		frame->grabstate=FRAME_DONE;
		epcam->framecount++;
		epcam->readcount++;
		if (epcam->frame[(epcam->curframe+1)&(EPCAM_NUMFRAMES-1)].grabstate==FRAME_READY) {
			epcam->curframe=(epcam->curframe+1) & (EPCAM_NUMFRAMES-1);
		}
	}
}

static inline void decode_eplite_integrate(struct usb_epcam *epcam, int data)
{
	int linelength=epcam->cwidth;
	int i;

	/* First two are absolute, all others relative.
	 */
	if (epcam->eplite_curpix < 2) {
		*(epcam->eplite_data+epcam->eplite_curpix)=1+data*4;
	} else {
		*(epcam->eplite_data+epcam->eplite_curpix)=i=
		    *(epcam->eplite_data+epcam->eplite_curpix-2)+data*4;
		if (i>255 || i<0) {
			epcam->lastoffset=-1;
			epcam->datacorrupt++;
		}
	}

	epcam->eplite_curpix++;

	if (epcam->eplite_curpix>=linelength) {
		decode_bayer(epcam, epcam->eplite_data, linelength);

		epcam->eplite_curpix=0;
		epcam->eplite_curline+=linelength;
		if (epcam->eplite_curline>=epcam->cheight*linelength)
			epcam->eplite_curline=0;
	}
}

static inline void decode_eplite (struct usb_epcam *epcam, struct epcam_scratch *buffer)
{
	int i;
	
	unsigned char *data=buffer->data;
	int len=buffer->length;
	struct epcam_frame *frame=&epcam->frame[epcam->curframe];

	int pos=0;
	int vlc_cod;
	int vlc_size;
	int vlc_data;
	int bit_cur;

	int bit;

	/* Check for cancelled frames: */
	/*for (i=0; i<len-3; i++) {
		if (data[i]==0xff && data[i+1]==0xff && data[i+2]==0xff && data[i+3]==0xff) {
			epcam->cancel++;
			frame->curpix=0;
			return;
		}
	}*/

	/* New image? */
	if (!buffer->offset) {
		frame->curpix=0;
	}
	if (!frame->curpix) {
		epcam->eplite_curline=0;
		epcam->eplite_curpix=0;
		epcam->vlc_cod=0;
		epcam->vlc_data=0;
		epcam->vlc_size=0;
		if (frame->grabstate==FRAME_READY)
			frame->grabstate=FRAME_GRABBING;
	}

	vlc_cod=epcam->vlc_cod;
	vlc_size=epcam->vlc_size;
	vlc_data=epcam->vlc_data;
	
	while (pos < len && frame->grabstate==FRAME_GRABBING) {
		bit_cur=8;
		while (bit_cur) {
			bit=((*data)>>(bit_cur-1))&1;
			if (!vlc_cod) {
				if (bit) {
					vlc_size++;
				} else {
					if (!vlc_size) {
						decode_eplite_integrate(epcam, 0);
					} else {
						vlc_cod=2;
						vlc_data=0;
					}
				}
			} else {
				if (vlc_size > 7) {
					epcam->datacorrupt++;
					epcam->lastoffset=-1;
					return;
				}
				if (vlc_cod==2) {
					if (!bit) vlc_data=-(1<<vlc_size)+1;
					vlc_cod--;
				}
				vlc_size--;
				vlc_data+=bit<<vlc_size;
				if (!vlc_size) {
					decode_eplite_integrate(epcam, vlc_data);
					vlc_cod=0;
				}
			}
			bit_cur--;
		}
		pos++;
		data++;
		
	}
	
	/* Store variable-lengt-decoder state if in middle of frame */
	if (frame->grabstate==FRAME_GRABBING) {
		epcam->vlc_size=vlc_size;
		epcam->vlc_cod=vlc_cod;
		epcam->vlc_data=vlc_data;
	} else {
		/* If there is data left regard image as corrupt */
		if (len-pos > epcam->packetsize) {
			debugprintk(KERN_INFO,"len-pos: %d\n", len-pos);
			frame->grabstate=FRAME_GRABBING;
			epcam->datacorrupt++;
		}
		epcam->lastoffset=-1;
	}

}

static int epcam_newframe(struct usb_epcam *epcam, int framenr)
{
	DECLARE_WAITQUEUE(wait, current);
	int errors=0;
	int process;

	/* this is now done by image signal processor */
	/*if (epcam->framecount%64==8)
		epcam_auto_resetlevel(epcam);*/

	while (epcam->streaming &&
	    (epcam->frame[framenr].grabstate==FRAME_READY ||
	     epcam->frame[framenr].grabstate==FRAME_GRABBING) ) {
		if(!epcam->frame[framenr].curpix) {
			errors++;
		}
		wait_interruptible(
		    epcam->scratch[epcam->scratch_use].state!=BUFFER_READY,
		    0,
		    &epcam->wq,
		    &wait
		);
		if (epcam->nullpackets > EPCAM_MAX_NULLPACKETS) {
			epcam->nullpackets=0;
			debugprintk(KERN_INFO,"to many null length packets, restarting capture\n");
			epcam_stop_stream(epcam);
			epcam_start_stream(epcam);			
		} else {
			struct epcam_scratch *buffer=&epcam->scratch[epcam->scratch_use];
			unsigned char *data=epcam->scratch[epcam->scratch_use].data;
			int len=epcam->scratch[epcam->scratch_use].length;
			struct epcam_frame *frame=&epcam->frame[epcam->curframe];

			epcam->scratch[epcam->scratch_use].state=BUFFER_BUSY;

			process=1;

			/* New image? */
			if (!buffer->offset) {
				frame->curpix=0;
			}

			if (process) {
				if (epcam->format==FMT_EPLITE) {
					decode_eplite(epcam, &epcam->scratch[epcam->scratch_use]);
				} else {
					decode_bayer(epcam, data, len);
				}
			}
			
			epcam->scratch[epcam->scratch_use].state=BUFFER_UNUSED;
			epcam->scratch_use++;
			if (epcam->scratch_use>=EPCAM_NUMSCRATCH)
				epcam->scratch_use=0;
			if (errors > EPCAM_MAX_ERRORS) {
				errors=0;
				debugprintk(KERN_INFO,"to much errors, restarting capture\n");
				epcam_stop_stream(epcam);
				epcam_start_stream(epcam);
			}
		}
	}
	if (epcam->frame[framenr].grabstate==FRAME_GRABBING)
		return -EAGAIN;
	
	return 0;
}

static void epcam_remove_disconnected(struct usb_epcam *epcam)
{
	int i;

        epcam->dev = NULL;

	for (i=0; i<EPCAM_NUMSBUF; i++) if (epcam->urb[i]) {
		usb_unlink_urb(epcam->urb[i]);
		usb_free_urb(epcam->urb[i]);
		epcam->urb[i] = NULL;
		kfree(epcam->sbuf[i].data);
	}
	for (i=0; i<EPCAM_NUMSCRATCH; i++) if (epcam->scratch[i].data) {
		kfree(epcam->scratch[i].data);
	}

	debugprintk(KERN_INFO,"%s disconnected\n", epcam->camera_name);

        /* Free the memory */
        if (!epcam->user) {
                kfree(epcam);
                epcam = NULL;
        }
}



/****************************************************************************
 *
 * Video4Linux
 *
 ***************************************************************************/

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
static int epcam_open(struct inode *inode, struct file *file)
#else
static int epcam_open(struct file *file)
#endif
{
	struct video_device *dev = video_devdata(file);
	struct usb_epcam *epcam = (struct usb_epcam *)dev;

	debugprintk(KERN_INFO,"epcam_open()\n");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,28)
	lock_kernel();
	if (epcam->user)
	{
		unlock_kernel();
		return -EBUSY;
	}
#else
	if (epcam->user)
		return -EBUSY;
#endif	
	epcam->fbuf=rvmalloc(epcam->maxframesize * EPCAM_NUMFRAMES);
	if(epcam->fbuf) 
		file->private_data = dev;
	else
		return -ENOMEM;

	epcam->user=1;
	debugprintk(KERN_INFO,"epcam_open() done\n");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,28)
	unlock_kernel();
#endif

	return 0;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
static int epcam_close(struct inode *inode, struct file *file)
#else
static int epcam_close(struct file *file)
#endif
{
	struct video_device *dev = video_devdata(file);
        struct usb_epcam *epcam = (struct usb_epcam *)dev;
	int i;

	debugprintk(KERN_INFO,"epcam_close()\n");
	if(dev == NULL)
		return -ENODEV;
	rvfree(epcam->fbuf, epcam->maxframesize * EPCAM_NUMFRAMES);
	if (epcam->removed) {
		epcam_remove_disconnected(epcam);
		debugprintk(KERN_INFO,"device unregistered\n");
	} else {
		for (i=0; i<EPCAM_NUMFRAMES; i++)
			epcam->frame[i].grabstate=FRAME_UNUSED;
		if (epcam->streaming)
			epcam_stop_stream(epcam);
	}
	epcam->user=0;
	file->private_data = NULL;
	debugprintk(KERN_INFO,"epcam_close() done\n");
	return 0;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
static int epcam_do_ioctl(struct inode *inode, struct file *file,
	unsigned int cmd, void *arg)
#else
static int epcam_do_ioctl(struct file *file,
	unsigned int cmd, void *arg)
#endif
{
	struct video_device *vdev = file->private_data;
        struct usb_epcam *epcam = (struct usb_epcam *)vdev;

        if(epcam_debug > 1)
		v4l_print_ioctl(epcam->vdev.name,cmd);
	if(epcam->removed)
		return -ENODEV;
	if (!epcam->dev)
                return -EIO;
        switch (cmd) {
	case VIDIOCGCAP:
	{
		struct video_capability *b=arg;
		strcpy(b->name, epcam->camera_name);
		b->type = VID_TYPE_CAPTURE;
		b->channels = 1;
		b->audios = 0;
		b->maxwidth = epcam->maxwidth;
		b->maxheight = epcam->maxheight;
		b->minwidth = 160;
		b->minheight = 120;

		return 0;
	}
	case VIDIOCGCHAN:
	{
		struct video_channel *v=arg;

		if (v->channel != 0)
			return -EINVAL;

		v->flags = 0;
		v->tuners = 0;
		v->type = VIDEO_TYPE_CAMERA;
		strcpy(v->name, "Camera");

		return 0;
	}
	case VIDIOCSCHAN:
	{
		struct video_channel *v=arg;

		if (v->channel != 0)
			return -EINVAL;

		return 0;
	}
        case VIDIOCGPICT:
        {
		struct video_picture *p=arg;

		epcam_get_pict(epcam, p);

		return 0;
	}
	case VIDIOCSPICT:
	{
		struct video_picture *p=arg;

		if (epcam_set_pict(epcam, p))
			return -EINVAL;
		return 0;
	}
	case VIDIOCSWIN:
	{
		struct video_window *vw=arg;

		if (vw->flags)
			return -EINVAL;
		if (vw->clipcount)
			return -EINVAL;
		if (epcam_set_size(epcam, vw->width, vw->height))
			return -EINVAL;
		
                adjust_pict(epcam);
		return 0;
        }
	case VIDIOCGWIN:
	{
		struct video_window *vw=arg;

		vw->x = 0;
		vw->y = 0;
		vw->chromakey = 0;
		vw->flags = 0;
		vw->clipcount = 0;
		vw->width = epcam->cwidth;
		vw->height = epcam->cheight;

		return 0;
	}
	case VIDIOCGMBUF:
	{
		struct video_mbuf *vm=arg;
		int i;

		memset(vm, 0, sizeof(struct video_mbuf));
		vm->size = EPCAM_NUMFRAMES * epcam->maxframesize;
		vm->frames = EPCAM_NUMFRAMES;
		for (i=0; i<EPCAM_NUMFRAMES; i++)
			vm->offsets[i] = epcam->maxframesize * i;

		return 0;
	}
	case VIDIOCMCAPTURE:
	{
		struct video_mmap *vm=arg;

		if (vm->format != VIDEO_PALETTE_RGB24 && vm->format != VIDEO_PALETTE_RGB565){
			debugprintk(KERN_INFO,"Unsupported palette %d\n",vm->format);
			return -EINVAL;
		}
		{
			if(vm->format == VIDEO_PALETTE_RGB565)
				BGRon=1;
			else
				BGRon=0;
		}
		if (vm->frame < 0 || vm->frame >= EPCAM_NUMFRAMES){
			debugprintk(KERN_INFO,"Frame out of range %d\n",vm->format);
			return -EINVAL;
		}
		if (epcam->frame[vm->frame].grabstate != FRAME_UNUSED)
			return -EBUSY;

		if (epcam_set_size(epcam, vm->width, vm->height)){
			debugprintk(KERN_INFO,"epcam_set_size failed %dx%d\n",vm->width,vm->height);
			return -EINVAL;
		}
		epcam->frame[vm->frame].grabstate=FRAME_READY;

                /* Set the picture properties */
                if (epcam->framecount==0)
                        adjust_pict(epcam);

		if (!epcam->streaming)
			epcam_start_stream(epcam);

		return 0;
	}
	case VIDIOCSYNC:
	{
		int *frame=arg, ret=0;

		if (*frame < 0 || *frame >= EPCAM_NUMFRAMES)
			return -EINVAL;

		ret=epcam_newframe(epcam, *frame);
		epcam->frame[*frame].grabstate=FRAME_UNUSED;
		return ret;
	}
	case VIDIOCGFBUF:
	{
		struct video_buffer *vb=arg;

		memset(vb, 0, sizeof(*vb));
		vb->base = NULL; /* frame buffer not supported, not used */

		return 0;
	}
	case VIDIOCKEY:
		return 0;
	case VIDIOCCAPTURE:
		return -EINVAL;
	case VIDIOCSFBUF:
		return -EINVAL;
	case VIDIOCGTUNER:
	case VIDIOCSTUNER:
		return -EINVAL;
	case VIDIOCGFREQ:
	case VIDIOCSFREQ:
		return -EINVAL;
	case VIDIOCGAUDIO:
	case VIDIOCSAUDIO:
		return -EINVAL;        
	case VIDIOC_QUERYCAP:
	{
		struct v4l2_capability *cap = arg;
		memset(cap,0,sizeof(*cap));
		strncpy(cap->driver,"epcam",sizeof (cap->driver));
		strncpy(cap->card,vdev->name, 32);
		strncpy(cap->bus_info,epcam->usb_path,sizeof(cap->bus_info));
		cap->version = KERNEL_VERSION(MAJOR_VERSION,MINOR_VERSION,RELEASE_VERSION);
		cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE;
		return 0;        
	}
	case VIDIOC_QUERYCTRL:
	{
		struct v4l2_queryctrl *ctrl = arg;
               if (ctrl->id < V4L2_CID_BRIGHTNESS ||
                   ctrl->id > V4L2_CID_HUE)
			return -EINVAL;
		else {
                        int id = ctrl->id;
                        memset(ctrl, 0, sizeof(*ctrl));
                        ctrl->id = id;
                }

                switch (ctrl->id) {
                case V4L2_CID_BRIGHTNESS:
                        strncpy(ctrl->name, "Brightness", sizeof(ctrl->name)-1);
                        break;
                case V4L2_CID_CONTRAST:
                        strncpy(ctrl->name, "Contrast", sizeof(ctrl->name)-1);
                        break;
                case V4L2_CID_SATURATION:
                        strncpy(ctrl->name, "Saturation", sizeof(ctrl->name)-1);
                        break;
                case V4L2_CID_HUE:
                        strncpy(ctrl->name, "Hue", sizeof(ctrl->name)-1);
                        break;
                }

                ctrl->minimum = 0;
                ctrl->maximum = 65535;
                ctrl->step = 1;
                ctrl->default_value = 32768;
                ctrl->type = V4L2_CTRL_TYPE_INTEGER;
		debugprintk(KERN_INFO,"VIDIOC_QUERYCTRL ioctl command %u\n",cmd);
                return 0;

	}
	case VIDIOC_G_CTRL:
	{
		struct v4l2_control *ctrl = arg;
		/* we only support hue/saturation/contrast/brightness */
                if (ctrl->id < V4L2_CID_BRIGHTNESS ||
                    ctrl->id > V4L2_CID_HUE)
                        return -EINVAL;

                switch (ctrl->id) {
                case V4L2_CID_BRIGHTNESS:
                        ctrl->value = epcam->brightness;
                        break;
                case V4L2_CID_CONTRAST:
                        ctrl->value = epcam->contrast;
                        break;
                case V4L2_CID_SATURATION:
                        ctrl->value = epcam->colour;
                        break;
                case V4L2_CID_HUE:
                        ctrl->value = epcam->hue;
                        break;
                }

		debugprintk(KERN_INFO,"VIDIOC_G_CTRL ioctl command %u\n",cmd);
		return 0;
	}
	case VIDIOC_S_CTRL:
	{
		struct v4l2_control *ctrl = arg;
		/* we only support hue/saturation/contrast/brightness */
                if (ctrl->id < V4L2_CID_BRIGHTNESS ||
                    ctrl->id > V4L2_CID_HUE)
                        return -EINVAL;

                if (ctrl->value < 0 || ctrl->value > 65535) {
                        return -ERANGE;
                }
		
                switch (ctrl->id) {
                case V4L2_CID_BRIGHTNESS:
                        epcam->brightness = ctrl->value;
                        break;
                case V4L2_CID_CONTRAST:
                        epcam->contrast = ctrl->value;
                        break;
                case V4L2_CID_SATURATION:
                        epcam->colour = ctrl->value;
                        break;
                case V4L2_CID_HUE:
                        epcam->hue = ctrl->value;
                        break;
                }
		epcam->chgsettings = 1;
		debugprintk(KERN_INFO,"VIDIOC_S_CTRL ioctl command %u\n",cmd);
		return 0;
	}
	case VIDIOC_ENUMINPUT:
	{
		debugprintk(KERN_INFO,"VIDIOC_ENUMINPUT ioctl command %u\n",cmd);
		struct v4l2_input *input = arg;
		unsigned int index;
		if ( input->index > 0) {
                                return -EINVAL;
                }
		index = input->index;
		memset(input,0,sizeof(*input));
		input->index = index;
		input->type = V4L2_INPUT_TYPE_CAMERA;
		strcpy(input->name ,"Composite");
		return 0;
	}
	case VIDIOC_G_INPUT:
	{
		int *input = arg;
		*input = 0;
		debugprintk(KERN_INFO,"VIDIOC_G_INPUT ioctl command %u\n",cmd);
		return 0;
	}
	case VIDIOC_S_INPUT:
	{
		int input = *(int *)arg;
		if(input < 0 || input > 0)
			return -EINVAL;
		debugprintk(KERN_INFO,"VIDIOC_S_INPUT ioctl command %u\n",cmd);
		return 0;
	}
	case VIDIOC_ENUM_FMT:
	{
		struct v4l2_fmtdesc *f = arg;
		unsigned int index;

		if(f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || f->index > 0)
			return -EINVAL;
		index = f->index;
		memset(f, 0, sizeof(*f));
		f->index = index;
	        f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                strlcpy(f->description, "24 bpp RGB, le",sizeof(f->description));
                f->pixelformat = V4L2_PIX_FMT_BGR24;
        	return 0;
	}
	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT:
	{
		struct v4l2_format *fmt = arg;
		if(fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || fmt->fmt.pix.pixelformat != V4L2_PIX_FMT_BGR24)
			return -EINVAL;
	
		debugprintk(KERN_INFO,"VIDIOC_S_FMT colorspace 0x%x\n",fmt->fmt.pix.colorspace);
		debugprintk(KERN_INFO,"VIDIOC_S_FMT bytesperline %u\n",fmt->fmt.pix.bytesperline);
		debugprintk(KERN_INFO,"VIDIOC_S_FMT field %u\n",fmt->fmt.pix.field);
		debugprintk(KERN_INFO,"VIDIOC_S_FMT width %u\n",fmt->fmt.pix.width);
		debugprintk(KERN_INFO,"VIDIOC_S_FMT height %u\n",fmt->fmt.pix.height);


		if(fmt->fmt.pix.width <= 176)
			fmt->fmt.pix.width = 176;
		if(fmt->fmt.pix.height <= 144)
			fmt->fmt.pix.height = 144;
		if(fmt->fmt.pix.width >= 352)
			fmt->fmt.pix.width = 352;
		if(fmt->fmt.pix.height >= 288)
			fmt->fmt.pix.height = 288;
		if(epcam_set_size(epcam,fmt->fmt.pix.width,fmt->fmt.pix.height))
		{
			return -EINVAL;
		}
		else
		{
			memset(fmt,0,sizeof(*fmt));
			fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
			fmt->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
			fmt->fmt.pix.field = V4L2_FIELD_NONE;
			fmt->fmt.pix.width = epcam->cwidth;
			fmt->fmt.pix.height = epcam->cheight;
			fmt->fmt.pix.bytesperline = fmt->fmt.pix.width * 3;
			fmt->fmt.pix.sizeimage = fmt->fmt.pix.height *  fmt->fmt.pix.bytesperline;
			fmt->fmt.pix.priv=0;
			return 0;
		}
		
	}
	case VIDIOC_G_FMT:
	{
		struct v4l2_format *fmt = arg;
		if(fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		memset(fmt,0,sizeof(*fmt));
		fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		fmt->fmt.pix.width = epcam->cwidth;
		fmt->fmt.pix.height = epcam->cheight;
		fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
		fmt->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
		fmt->fmt.pix.bytesperline = fmt->fmt.pix.width * 3;
		fmt->fmt.pix.sizeimage = fmt->fmt.pix.height *  fmt->fmt.pix.bytesperline;
		fmt->fmt.pix.field = V4L2_FIELD_NONE;
		debugprintk(KERN_INFO,"VIDIOC_G_FMT ioctl command %u\n",cmd);

		return 0;
	}
	default:
		debugprintk(KERN_INFO,"Unsupported ioctl command %u\n",cmd);
		return -EINVAL;
        } /* end switch */

        return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
static int epcam_ioctl(struct inode *inode, struct file *file,
	unsigned int cmd, unsigned long arg)
{
	return video_usercopy(inode, file, cmd, arg, epcam_do_ioctl);
}
#else
static int epcam_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	return video_usercopy(file, cmd, arg, epcam_do_ioctl);
}
#endif

static ssize_t epcam_read(struct file *file, char *buf,
	size_t count, loff_t *ppos)
{
	int realcount=count, ret=0;
	struct video_device *dev = file->private_data;
	struct usb_epcam *epcam = (struct usb_epcam *)dev;

	if (epcam->removed)
		return -ENODEV;
	if(!epcam->streaming)
		debugprintk(KERN_INFO,"In epcam_read\n");
	if (epcam->dev == NULL)
		return -ENODEV;
	if (realcount > epcam->cwidth*epcam->cheight*3)
		realcount=epcam->cwidth*epcam->cheight*3;

	if (!(epcam->frame[0].grabstate==FRAME_GRABBING)) {
		epcam->frame[0].grabstate=FRAME_READY;
		epcam->frame[1].grabstate=FRAME_UNUSED;
		epcam->curframe=0;
	}
	else
		return -EBUSY;

	if (!epcam->streaming)
		epcam_start_stream(epcam);

        if (epcam->framecount==0)
                adjust_pict(epcam);

	ret=epcam_newframe(epcam, 0);

	if (ret == -EAGAIN)
		return ret;
	if (!ret) {
		copy_to_user(buf, epcam->frame[0].data, realcount);
	} else {
		realcount=ret;
	}

	epcam->frame[0].grabstate=FRAME_UNUSED;
	return realcount;
}

static int epcam_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct video_device *dev = file->private_data;
	struct usb_epcam *epcam = (struct usb_epcam *)dev;
	unsigned long start = vma->vm_start;
	unsigned long size  = vma->vm_end-vma->vm_start;
	unsigned long page, pos;

	mutex_lock(&epcam->lock);

	if (epcam->dev == NULL) {
		mutex_unlock(&epcam->lock);
		return -EIO;
	}
	if (size > (((EPCAM_NUMFRAMES * epcam->maxframesize) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))) {
		mutex_unlock(&epcam->lock);
		return -EINVAL;
	}
	pos = (unsigned long)epcam->fbuf;
	while (size > 0) {
		page = kvirt_to_pa(pos);
		if (remap_page_range(vma, start, page, PAGE_SIZE, PAGE_SHARED)) {
			mutex_unlock(&epcam->lock);
			return -EAGAIN;
		}
		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}
	mutex_unlock(&epcam->lock);

        return 0;
}

/* release callback too suppress kernel warning message */
void epcam_vdev_release(struct video_device *vdev)
{
	debugprintk(KERN_INFO,"Video device release callback\n");
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
static struct file_operations epcam_fops = {
#else
static struct v4l2_file_operations epcam_fops = {
#endif
	.owner =	THIS_MODULE,
	.open =		epcam_open,
	.release =	epcam_close,
	.read =		epcam_read,
	.mmap =		epcam_mmap,
	.ioctl =	epcam_ioctl,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
#ifdef CONFIG_COMPAT
	.compat_ioctl = v4l_compat_ioctl32,
#endif
	.llseek = 	no_llseek,
#endif
};

static struct video_device epcam_template = {
        .name =		"EPCAM USB camera",
	.fops =		&epcam_fops,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
	.type = 	VID_TYPE_CAPTURE | VID_TYPE_SCALES,
	.release=	epcam_vdev_release,
	.minor=		-1
#else
	.release=	video_device_release_empty
#endif
};



/***************************/
static void hv7121_reset_registers(struct usb_epcam *epcam)
{
        epcam_set_feature(epcam,HV7131_REG_MODE_B,0x5);
        epcam_set_feature(epcam,HV7131_REG_MODE_C,0xa);
        epcam_set_feature(epcam,HV7131_REG_TITU,0x3);
        epcam_set_feature(epcam,HV7131_REG_TITM,0x0d);
        epcam_set_feature(epcam,HV7131_REG_TITL,0x40);
        epcam_set_feature(epcam,HV7131_REG_ARCG,0x2d);
        epcam_set_feature(epcam,HV7131_REG_AGCG,0x32);
        epcam_set_feature(epcam,HV7131_REG_ABCG,0x2d);
}

static void H1A424M167_set_registers(struct usb_epcam *epcam)
{
        epcam_set_feature(epcam,H1A424M167_OP_MODE,0x12 );
        epcam_set_feature(epcam,H1A424M167_BASE_ENB,0xd );
        epcam_set_feature(epcam,H1A424M167_AUTO_ENB,0xf3 );
        epcam_set_feature(epcam,H1A424M167_RESET_LEVEL,0x2d );
}

static int epcam_init(struct usb_epcam *epcam)
{
        int i=0, rc;
        unsigned char cp[0x80];

        epcam_sndctrl(1, epcam, VENDOR_REQ_LED_CONTROL, 1, NULL, 0);

	/* get camera descriptor */
	memset(cp, 0, 0x80);
	rc=epcam_sndctrl(0, epcam, VENDOR_REQ_CAMERA_INFO, 0, cp, sizeof(cp));
	debugprintk(KERN_INFO,"vendor_req_camera_info: %d\n", rc);
	if (rc<0) {
		printk(KERN_ERR "EPCAM:Error reading camera descriptor\n");
		return 1;
	}
	debugprintk(KERN_INFO,"size     :%d  %x %x\n", QT2INT(cp   ), cp[0], cp[1]);
	debugprintk(KERN_INFO,"rev      :%d  %x %x\n", QT2INT(cp+4 ), cp[4], cp[5]);
	debugprintk(KERN_INFO,"maxwidth :%d  %x %x\n", QT2INT(cp+6 ), cp[6], cp[7]);
	debugprintk(KERN_INFO,"maxheight:%d  %x %x\n", QT2INT(cp+8 ), cp[8], cp[9]);
	debugprintk(KERN_INFO,"zoomcaps :%d  %x %x\n", QT2INT(cp+10), cp[10], cp[11]);
	debugprintk(KERN_INFO,"ISPCaps  :%d  %x %x\n", QT2INT(cp+12), cp[12], cp[13]);
	debugprintk(KERN_INFO,"Formats  :%d  %x %x\n", QT2INT(cp+14), cp[14], cp[15]);
	for (i=0; i<QT2INT(cp+14); i++) {
		if (QT2INT(cp+16+i*2)==EPCAM_FORMAT_BAYER)
			printk(KERN_INFO "EPCAM:Bayer format supported\n");
		else
			printk(KERN_INFO "EPCAM:Unknown format %d\n", QT2INT(cp+16+i*2));
	}
	debugprintk(KERN_INFO,"bios version: %d\n", epcam_read_bios(epcam, 0xfffc));

	/*epcam->maxwidth=QT2INT(cp+6);
	epcam->maxheight=QT2INT(cp+8);*/
	epcam->maxwidth=352;
	epcam->maxheight=288;
	epcam->camid=QT2INT(cp+2);

	debugprintk(KERN_INFO,"camid: %x\n", epcam->camid);
	if (epcam->camid!=0x800 &&
	    epcam->camid!=0x402 &&
	    epcam->camid!=0x401 ) {
		err("Not a supported camid: %d!", epcam->camid);
		return 1;
	}
	if (epcam->camid==0x401) {
		debugprintk(KERN_INFO,"Your camera might work with this driver...\n");
		debugprintk(KERN_INFO,"Please send your results to: pe1rxq@amsat.org\n");
	}

	epcam->cwidth=176;
	epcam->cheight=144;
	epcam->maxframesize=(epcam->maxwidth+1)*(epcam->maxheight+1)*3;

	H1A424M167_set_registers(epcam);
        hv7121_reset_registers(epcam);

	/* some default values */
	epcam->palette=VIDEO_PALETTE_RGB24;
	epcam->dropped=0;
	epcam->framecount=0;
	epcam->error=0;
	epcam->readcount=0;
	epcam->streaming =0;
        epcam->brightness = 32767;
        epcam->whiteness = 32767;  /* only for greyscale */
	epcam->rgain = 0x2d;
	epcam->ggain = 0x32;
	epcam->bgain = 0x2d;
        epcam->colour = 32767;
        epcam->contrast = 32767;
        epcam->hue = 32767;
	/*epcam_recv_pict(epcam);*/
	
        /* Start interrupt transfers for snapshot button */
        epcam->inturb=usb_alloc_urb(0, GFP_KERNEL);
        if (!epcam->inturb) {
               debugprintk(KERN_INFO,"Allocation of inturb failed\n");
               return 1;
        }
        usb_fill_int_urb(epcam->inturb, epcam->dev,
               usb_rcvintpipe(epcam->dev, EPCAM_BUTTON_ENDPOINT),
               &epcam->button, sizeof(epcam->button),
               epcam_button_irq,
               epcam,
               8
        );
        if (usb_submit_urb(epcam->inturb, GFP_KERNEL)) {
               debugprintk(KERN_INFO,"int urb burned down\n");
               return 1;
        }

	/* Flash the led */
        epcam_sndctrl(1, epcam, VENDOR_REQ_CAM_POWER, 1, NULL, 0);
	epcam_sndctrl(1, epcam, VENDOR_REQ_LED_CONTROL, 1, NULL, 0);
        epcam_sndctrl(1, epcam, VENDOR_REQ_CAM_POWER, 0, NULL, 0);
	epcam_sndctrl(1, epcam, VENDOR_REQ_LED_CONTROL, 0, NULL, 0);


        return 0;
}

static int epcam_probe(struct usb_interface *intf,
	const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
        struct usb_interface_descriptor *interface;
        struct usb_epcam *epcam;
        char *camera_name=NULL;
	int ret=0;


	if (!id)
		return -ENODEV;
        if (dev->descriptor.bNumConfigurations != 1)
                return -ENODEV;

	interface = &intf->altsetting[0].desc;
	
	if (interface->bInterfaceNumber!=0)
		return -ENODEV;

	camera_name=(char *)id->driver_info;

        /* We found one */
        printk(KERN_INFO "EPCAM:camera found -> %s\n", camera_name);

        if ((epcam = kmalloc(sizeof(*epcam), GFP_KERNEL)) == NULL) {
                err("couldn't kmalloc epcam struct");
                return -ENODEV;
        }

        memset(epcam, 0, sizeof(*epcam));


        epcam->dev = dev;
        epcam->iface = interface->bInterfaceNumber;
        epcam->camera_name = camera_name;
	debugprintk(KERN_INFO,"firmware version: %02x\n", dev->descriptor.bcdDevice & 255);

        if (epcam_init(epcam)) {
		kfree(epcam);
		return -ENODEV;
	}


	memcpy(&epcam->vdev, &epcam_template, sizeof(epcam_template));
	memcpy(epcam->vdev.name, epcam->camera_name, strlen(epcam->camera_name));
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
	epcam->vdev.dev = &dev->dev;
#else
	epcam->vdev.parent = &dev->dev;
#endif
	init_waitqueue_head(&epcam->wq);
	mutex_init(&epcam->lock);
	wmb();

        if (usb_make_path(dev, epcam->usb_path, EPCAM_USB_PATH_LEN) < 0) {
                err("usb_make_path error");
		kfree(epcam);
		return -ENODEV;
        }
	
	video_set_drvdata(&epcam->vdev,epcam);

	if (video_register_device(&epcam->vdev, VFL_TYPE_GRABBER, video_nr) == -1) {
		err("video_register_device failed");
		kfree(epcam);
		return -ENODEV;
	}
        debugprintk(KERN_INFO,"Device at %s registered to minor %d\n", epcam->usb_path,
             epcam->vdev.minor);
	debugprintk(KERN_INFO,"registered new video device: video%d\n", epcam->vdev.minor);
	usb_set_intfdata(intf, epcam);

	ret = epcam_create_sysfs_files(&epcam->vdev);
	if(ret) {
		video_unregister_device(&epcam->vdev);
		video_device_release(&epcam->vdev);	
		kfree(epcam);
		return ret;
	}	
        return 0;
}

static void epcam_disconnect(struct usb_interface *intf)
{
	struct usb_epcam *epcam = usb_get_intfdata(intf);

	debugprintk(KERN_INFO,"camera disconnected\n");
	usb_set_intfdata(intf, NULL);
	if (epcam) {
		debugprintk(KERN_INFO,"unregistering video device\n");
        	epcam_remove_sysfs_files(&epcam->vdev);
		video_unregister_device(&epcam->vdev);
		if (!epcam->user)
			epcam_remove_disconnected(epcam);
		else {
		        epcam->frame[0].grabstate = FRAME_ERROR;
		        epcam->frame[1].grabstate = FRAME_ERROR;

			epcam->streaming = 0;

	                wake_up_interruptible(&epcam->wq);
			epcam->removed = 1;
		}
	}
}


static struct usb_driver epcam_driver = {
	.name		= "epcam",
	.id_table	= device_table,
	.probe		= epcam_probe,
	.disconnect	= epcam_disconnect,
};



/****************************************************************************
 *
 *  Module routines
 *
 ***************************************************************************/

static int __init usb_epcam_init(void)
{
	debugprintk(KERN_INFO,"usb camera driver version %s registering\n", version);
	return usb_register(&epcam_driver);
}

static void __exit usb_epcam_exit(void)
{
	usb_deregister(&epcam_driver);
	debugprintk(KERN_INFO,"driver deregistered\n");
}

module_init(usb_epcam_init);
module_exit(usb_epcam_exit);
