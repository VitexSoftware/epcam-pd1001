/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Endpoints EP800 / Creative PD1001 register and protocol definitions.
 * Derived from the historical epcam driver (Jeroen Vreeken et al.).
 */
#ifndef EP800_H
#define EP800_H

#define EP800_VENDOR_REQ_CAMERA_INFO	0x00
#define EP800_VENDOR_REQ_CAPTURE_INFO	0x01
#define EP800_VENDOR_REQ_COMPRESSION	0x02
#define EP800_VENDOR_REQ_CONT_CAPTURE	0x03
#define EP800_VENDOR_REQ_CAPTURE_FRAME	0x04
#define EP800_VENDOR_REQ_IMAGE_INFO	0x05
#define EP800_VENDOR_REQ_EXT_FEATURE	0x06
#define EP800_VENDOR_REQ_CAM_POWER	0x07
#define EP800_VENDOR_REQ_LED_CONTROL	0x08
#define EP800_VENDOR_DEAD_PIXEL		0x09
#define EP800_VENDOR_REQ_AUTO_CONTROL	0x0a
#define EP800_VENDOR_REQ_BIOS		0xff
#define EP800_VENDOR_CMD_BIOS_READ	0x07

#define EP800_FORMAT_BAYER		1

/* Hyundai HV7131B */
#define HV7131_REG_MODE_A		0x00
#define HV7131_REG_MODE_B		0x01
#define HV7131_REG_MODE_C		0x02
#define HV7131_REG_TITU			0x25
#define HV7131_REG_TITM			0x26
#define HV7131_REG_TITL			0x27
#define HV7131_REG_ARCG			0x31
#define HV7131_REG_AGCG			0x32
#define HV7131_REG_ABCG			0x33
#define HV7131_REG_OFSR			0x50
#define HV7131_REG_OFSG			0x51
#define HV7131_REG_OFSB			0x52

/* Hyundai H1A424M167 ISP */
#define H1A424M167_OP_MODE		0x80
#define H1A424M167_BASE_ENB		0x81
#define H1A424M167_AUTO_ENB		0xa0
#define H1A424M167_RESET_LEVEL		0xae

#define EP800_PACKETBUFS		4
#define EP800_NUMSBUF			2
#define EP800_VIDEO_ENDPOINT		1
#define EP800_BUTTON_ENDPOINT		2
#define EP800_NUMSCRATCH		4
#define EP800_MAX_NULLPACKETS		200
#define EP800_ISO_ALTSETTING		4

#define EP800_MAX_EXPOSURE		0x259f00
#define EP800_MIN_EXPOSURE		0x01f400

#define EP800_CAMID_EP800		0x800
#define EP800_CAMID_SE402		0x402
#define EP800_CAMID_SE401		0x401

static inline void ep800_int2qt(u16 val, u8 *buf)
{
	buf[0] = val & 0xff;
	buf[1] = val >> 8;
}

static inline u16 ep800_qt2int(const u8 *buf)
{
	return buf[0] | (buf[1] << 8);
}

#endif /* EP800_H */
