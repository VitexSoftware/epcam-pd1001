#ifndef __LINUX_epcam_H
#define __LINUX_epcam_H

#include <asm/uaccess.h>
#include <linux/videodev.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
#include <media/v4l2-ioctl.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
#include <media/v4l2-common.h>
#endif

#include <linux/smp_lock.h>

#ifdef epcam_DEBUG
#  define PDEBUG(level, fmt, args...) \
if (debug >= level) info("[%s:%d] " fmt, __PRETTY_FUNCTION__, __LINE__ , ## args)
#else
#  define PDEBUG(level, fmt, args...) do {} while(0)
#endif

#define debugprintk(level,x...) { if(epcam_debug) \
                                printk(level "EPCAM:" x);}

/* An almost drop-in replacement for sleep_on_interruptible */
#define wait_interruptible(test, noblock, queue, wait) \
{ \
	add_wait_queue(queue, wait); \
	set_current_state(TASK_INTERRUPTIBLE); \
	if (test) \
		schedule(); \
	remove_wait_queue(queue, wait); \
	set_current_state(TASK_RUNNING); \
	if (signal_pending(current)) \
		break; \
}


/* Helpers for reading/writing integers to the QT engine */
#define INT2QT(val, buf) { (buf)[0]=(val)&255; (buf)[1]=(val)/256; }
#define QT2INT(buf) ((buf)[0]+(buf)[1]*256)


/* EPCAM controls: */

#define VENDOR_REQ_CAMERA_INFO		0x00
#define VENDOR_REQ_CAPTURE_INFO		0x01
#define	VENDOR_REQ_COMPRESSION		0x02
#define VENDOR_REQ_CONT_CAPTURE		0x03
#define VENDOR_REQ_CAPTURE_FRAME	0x04
#define VENDOR_REQ_IMAGE_INFO		0x05
#define VENDOR_REQ_EXT_FEATURE		0x06
#define	VENDOR_REQ_CAM_POWER		0x07
#define VENDOR_REQ_LED_CONTROL		0x08
#define VENDOR_DEAD_PIXEL		0x09
#define VENDOR_REQ_AUTO_CONTROL		0x0a
#define VENDOR_REQ_BIOS			0xff

#define VENDOR_CMD_BIOS_READ		0x07

#define EPCAM_FORMAT_BAYER		1

/* Hyundai hv7131b registers
   7121 and 7141 should be the same (haven't really checked...) */
/* Mode registers: */
#define HV7131_REG_MODE_A	0x00
#define HV7131_REG_MODE_B	0x01
#define HV7131_REG_MODE_C	0x02
/* Frame registers: */
#define HV7131_REG_FRSU		0x10
#define HV7131_REG_FRSL		0x11
#define HV7131_REG_FCSU		0x12
#define HV7131_REG_FCSL		0x13
#define HV7131_REG_FWHU		0x14
#define HV7131_REG_FWHL		0x15
#define HV7131_REG_FWWU		0x16
#define HV7131_REG_FWWL		0x17
/* Timing registers: */
#define HV7131_REG_THBU		0x20
#define HV7131_REG_THBL		0x21
#define HV7131_REG_TVBU		0x22
#define HV7131_REG_TVBL		0x23
#define HV7131_REG_TITU		0x25
#define HV7131_REG_TITM		0x26
#define HV7131_REG_TITL		0x27
#define HV7131_REG_TMCD		0x28
/* Adjust Registers: */
#define HV7131_REG_ARLV		0x30
#define HV7131_REG_ARCG		0x31
#define HV7131_REG_AGCG		0x32
#define HV7131_REG_ABCG		0x33
#define HV7131_REG_APBV		0x34
#define HV7131_REG_ASLP		0x54
/* Offset Registers: */
#define HV7131_REG_OFSR		0x50
#define HV7131_REG_OFSG		0x51
#define HV7131_REG_OFSB		0x52
/* Reset level statistics registers: */
#define HV7131_REG_LOREFNOH	0x57
#define HV7131_REG_LOREFNOL	0x58
#define HV7131_REG_HIREFNOH	0x59
#define HV7131_REG_HIREFNOL	0x5a

/* Hyundai H1A424M167 Image Signal Processor registers */
/* Base registers: */
#define H1A424M167_OP_MODE	0x80
#define H1A424M167_BASE_ENB	0x81
#define H1A424M167_SCALE_UPPER	0x82
#define H1A424M167_SCALE_LOWER	0x83
#define H1A424M167_CMA11	0x8A
#define H1A424M167_CMA12	0x8B
#define H1A424M167_CMA13	0x8C
#define H1A424M167_CMA21	0x8D
#define H1A424M167_CMA22	0x8E
#define H1A424M167_CMA23	0x8F
#define H1A424M167_CMA31	0x90
#define H1A424M167_CMA32	0x91
#define H1A424M167_CMA33	0x92
#define H1A424M167_OFSR		0x93	
#define H1A424M167_OFSG		0x94
#define H1A424M167_OFSB		0x95
/* Auto registers: */
#define H1A424M167_AUTO_ENB	0xA0
#define H1A424M167_WIN_H_START	0xA1
#define H1A424M167_WIN_H_SIDE	0xA2
#define H1A424M167_WIN_H_CENTER	0xA3
#define H1A424M167_WIN_V_START	0xA4
#define H1A424M167_WIN_V_SIDE	0xA5
#define H1A424M167_WIN_V_CENTER	0xA6
#define H1A424M167_GAIN_TOP	0xA7
#define H1A424M167_GAIN_BOTTOM	0xA8
#define H1A424M167_AWB_CONTROL	0xA9
#define H1A424M167_AWB_LOCK	0xAA
#define H1A424M167_AE_CONTROL	0xAB
#define H1A424M167_AE_LOCK	0xAC
#define H1A424M167_Y_TARGET	0xAD
#define H1A424M167_RESET_LEVEL	0xAE
#define H1A424M167_EXP_LMT_UPPER	0xB0
#define H1A424M167_EXP_LMT_MIDDLE	0xB1
#define H1A424M167_EXP_LMT_LOWER	0xB2
#define H1A424M167_AWB_CR_TARGET	0xB3
#define H1A424M167_AWB_CB_TARGET	0xB4
#define H1A424M167_AF_UT_UPPER		0xB5
#define H1A424M167_AF_UT_MIDDLE		0xB6
#define H1A424M167_AF_UT_LOWER		0xB7
#define H1A424M167_STATUS_FLAGS		0xB8
/* OUT registers */
#define H1A424M167_EDGE_CONTROL		0xC0
#define H1A424M167_OUT_FORM		0xC1
#define H1A424M167_HSYNC_COUNT		0xC2
#define H1A424M167_HISTO_MODE		0xC3
#define H1A424M167_FIXED_FACTOR		0xC4
#define H1A424M167_GMA_START0		0xE0
#define H1A424M167_GMA_START1		0xE1
#define H1A424M167_GMA_START2		0xE2
#define H1A424M167_GMA_START3		0xE3
#define H1A424M167_GMA_START4		0xE4
#define H1A424M167_GMA_START5		0xE5
#define H1A424M167_GMA_START6		0xE6
#define H1A424M167_GMA_START7		0xE7
#define H1A424M167_GMA_START8		0xE8
#define H1A424M167_GMA_SLOPE0		0xE9
#define H1A424M167_GMA_SLOPE1		0xEA
#define H1A424M167_GMA_SLOPE2		0xEB
#define H1A424M167_GMA_SLOPE3		0xEC
#define H1A424M167_GMA_SLOPE4		0xED
#define H1A424M167_GMA_SLOPE5		0xEE
#define H1A424M167_GMA_SLOPE6		0xEF
#define H1A424M167_GMA_SLOPE7		0xF0
#define H1A424M167_GMA_SLOPE8		0xF1

///* se401 registers */
//#define SE401_OPERATINGMODE	0x2000

/* size of usb transfers */
#define EPCAM_PACKETBUFS	4	
/* number of iso urbs to use */
#define EPCAM_NUMSBUF		2	
/* read the usb specs for this one :) */
#define EPCAM_VIDEO_ENDPOINT	1
#define EPCAM_BUTTON_ENDPOINT	2
/* number of frames supported by the v4l part */
#define EPCAM_NUMFRAMES		2
/* scratch buffers for passing data to the decoders */
#define EPCAM_NUMSCRATCH	2
/* maximum amount of data in a JangGu packet */
#define EPCAM_VLCDATALEN	1024
/* number of nul sized packets to receive before kicking the camera */
#define EPCAM_MAX_NULLPACKETS	200	
/* number of decoding errors before kicking the camera */
#define EPCAM_MAX_ERRORS	100
/* size of usb_make_path() buffer */
#define EPCAM_USB_PATH_LEN	64

#define EPCAM_MAX_EXPOSURE	0x259f00
#define	EPCAM_MIN_EXPOSURE	0x01f400

struct usb_device;

struct epcam_sbuf {
	unsigned char *data;
};

enum {
	FRAME_UNUSED,		/* Unused (no MCAPTURE) */
	FRAME_READY,		/* Ready to start grabbing */
	FRAME_GRABBING,		/* In the process of being grabbed into */
	FRAME_DONE,		/* Finished grabbing, but not been synced yet */
	FRAME_ERROR,		/* Something bad happened while processing */
};

enum {
	FMT_BAYER,
	FMT_JANGGU,
	FMT_EPLITE,
};

enum {
	BUFFER_UNUSED,
	BUFFER_READY,
	BUFFER_BUSY,
	BUFFER_DONE,
};

struct epcam_scratch {
	unsigned char *data;
	volatile int state;
	int offset;
	int length;
};

struct epcam_frame {
	unsigned char *data;		/* Frame buffer */

	volatile int grabstate;		/* State of grabbing */

	unsigned char *curline;
	int curlinepix;
	int curpix;
};

struct usb_epcam {
	struct video_device vdev;

	/* Device structure */
	struct usb_device *dev;

	unsigned char iface;
	char usb_path[EPCAM_USB_PATH_LEN];

	char *camera_name;
	unsigned int camid;

	unsigned int brightness;
	unsigned int resetlevel;
	unsigned int chgsettings;
	unsigned int colour;
	unsigned int contrast;
	unsigned int whiteness;
        unsigned int rgain;
        unsigned int ggain;
        unsigned int bgain;
	unsigned int hue;
	

	int format;
	int maxwidth;		/* max width */
	int maxheight;		/* max height */
	int cwidth;		/* current width */
	int cheight;		/* current height */
	int palette;
	int maxframesize;

	int removed;
	int user;		/* user count for exclusive use */

	int streaming;		/* Are we streaming video? */

	char *fbuf;		/* Videodev buffer area */

	int packetsize;
	struct urb *urb[EPCAM_NUMSBUF];
	struct urb *inturb;

	int button;
	int buttonpressed;

	int curframe;		/* Current receiving frame */
	struct epcam_frame frame[EPCAM_NUMFRAMES];
	int readcount;
	int framecount;

	int cancel;
	int dropped;
	int error;
	int underrun;
	int datacorrupt;

	int scratch_next;
	int scratch_use;
	int scratch_overflow;
	struct epcam_scratch scratch[EPCAM_NUMSCRATCH];
	int scratch_offset;

	/* Decoder specific data: */
	int lastoffset;
	int eplite_curpix;
	int eplite_curline;
	unsigned char eplite_data[1024];
	int vlc_size;
	int vlc_cod;
	int vlc_data;

	struct epcam_sbuf sbuf[EPCAM_NUMSBUF];

	wait_queue_head_t wq;	/* Processes waiting */
	struct mutex lock;
	struct mutex res_lock;

	/* proc interface */
	struct proc_dir_entry *proc_entry;	/* /proc/epcam/videoX */

	int nullpackets;
};


#endif
