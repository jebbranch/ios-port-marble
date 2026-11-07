
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define __asm__(...)
#define G_PCM_OFF_A                 0x0000u
#define G_PCM_BYTES                 1152u
#define G_SHM_SIZE                          0x5000u  /* vendor MAP size */
#define G_VP_WR_IID                         0x40bdu
#define G_VP_TDM_IID                        0x428fu
#define G_VP_AW_IID                         0x42d3u
#define G_VP_MFC_IID                        0x48bau
#define SWEET_CC_MAX 256
#define GPR_DOMAIN_ADSP         2u
#define GPR_DOMAIN_APPS         3u
#define APM_CMD_GRAPH_OPEN                  0x01001000u
static const uint8_t g_graph_vp_wr_dec_cnv[856] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08,
	0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0xe0, 0x0a, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x07, 0x12, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0x07, 0x40, 0x00, 0x00, 0x11, 0x10, 0x00, 0x07, 0x8e, 0x41, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x07, 0x8f, 0x42, 0x00, 0x00, 0x98, 0x10, 0x00, 0x07,
	0x06, 0x40, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07, 0xba, 0x48, 0x00, 0x00,
	0x01, 0x3d, 0x01, 0x10, 0xd3, 0x42, 0x00, 0x00, 0x00, 0x10, 0x00, 0x07,
	0xbd, 0x40, 0x00, 0x00, 0x05, 0x10, 0x00, 0x07, 0xbf, 0x40, 0x00, 0x00,
	0x03, 0x10, 0x00, 0x07, 0xc0, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x03, 0x10, 0x00, 0x08, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0a, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x1c, 0x00, 0x00, 0x00, 0x8f, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x04, 0x10, 0x00, 0x08, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x11, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x8f, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0xe2, 0x41, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00,
	0x7e, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x25, 0x00, 0x00, 0x00, 0x36, 0x44, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x99, 0x41, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00,
	0x8d, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xd3, 0x42, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xd3, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x2f, 0x00, 0x00, 0x00, 0x2c, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x48, 0x44, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
	0x33, 0x43, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xbd, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xc0, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};
static const uint8_t g_graph_aform_no_aw[808] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xb0,
	0x03, 0x00, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08,
	0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x11, 0x10, 0x00, 0x08,
	0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x0b,
	0x12, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x13, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x14, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x74, 0x11, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xcb, 0x10, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x4d, 0x12, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x08,
	0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0xb0, 0x02, 0x00, 0x00, 0xe0, 0x09, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x00, 0x07, 0x12, 0x40, 0x00, 0x00, 0x1a, 0x10, 0x00, 0x07,
	0x07, 0x40, 0x00, 0x00, 0x11, 0x10, 0x00, 0x07, 0x8e, 0x41, 0x00, 0x00,
	0x0e, 0x10, 0x00, 0x07, 0x8f, 0x42, 0x00, 0x00, 0x98, 0x10, 0x00, 0x07,
	0x06, 0x40, 0x00, 0x00, 0x15, 0x10, 0x00, 0x07, 0xba, 0x48, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x07, 0xbd, 0x40, 0x00, 0x00, 0x05, 0x10, 0x00, 0x07,
	0xbf, 0x40, 0x00, 0x00, 0x03, 0x10, 0x00, 0x07, 0xc0, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x08, 0xdc, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x12, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x17, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x8f, 0x42, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x15, 0x10, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x04, 0x10, 0x00, 0x08, 0x04, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x07, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x8f, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x12, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00,
	0xe2, 0x41, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x1d, 0x00, 0x00, 0x00, 0x7e, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x36, 0x44, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00,
	0x99, 0x41, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x2d, 0x00, 0x00, 0x00, 0x8d, 0x46, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00,
	0x2c, 0x42, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00,
	0x33, 0x00, 0x00, 0x00, 0x48, 0x44, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x8e, 0x41, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0x33, 0x43, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x07, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xbd, 0x40, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xbf, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xbf, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x12, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xba, 0x48, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x8e, 0x41, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};
static const int16_t g_sine_48[48] = {
	0, 1069, 2120, 3135, 4096, 4987, 5793, 6499,
	7094, 7568, 7913, 8122, 8192, 8122, 7913, 7568,
	7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069,
	0, -1069, -2120, -3135, -4096, -4987, -5793, -6499,
	-7094, -7568, -7913, -8122, -8192, -8122, -7913, -7568,
	-7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069,
};
typedef uintptr_t vm_offset_t;
static uint8_t region[0x5000], g_rsp[64];
static vm_offset_t g_shm_va;
static uint64_t g_shm_pa = 1;
static int g_shm_mapped = 1, g_tone_filled, g_vol_printed;
static uint32_t g_shm_handle = 0x12345678, g_rsp_len, g_stream_port;
static uint32_t g_pcm_bytes = G_PCM_BYTES;
static int g_rsp_ready, boot_v8, boot_vcfg2, reject_open;
static int boot_descsep, boot_nowait, boot_dbb;
static uint32_t last_graph_offset;
static int cfg_aw, cfg_bf, cfg_c0, cfg_absent, fallback_count;
static int sent_buffers, descriptor_buffers;
static const uint8_t *descriptor;
static size_t descriptor_size;
static int PE_parse_boot_argn(const char *n, void *p, unsigned long size) {
    int v;
    if (!strcmp(n, "sweetvol")) v = 16;
    else if (!strcmp(n, "sweetbufs")) v = 120;
    else if (!strcmp(n, "sweetpcm")) v = 1;
    else if (!strcmp(n, "sweettdmvendor")) v = 7;
    else if (!strcmp(n, "sweetvgraph8")) v = boot_v8;
    else if (!strcmp(n, "sweetvcfg2")) v = boot_vcfg2;
    else if (!strcmp(n, "sweetdescsep")) v = boot_descsep;
    else if (!strcmp(n, "sweetnowait")) v = boot_nowait;
    else if (!strcmp(n, "sweetdbb")) v = boot_dbb;
    else return 0;
    if (size != sizeof(v)) abort();
    memcpy(p, &v, size); return 1;
}
static void sweet_log_sync(void) {}
static void g_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t g_le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t g_shm_region_size(void) { return sizeof(region); }
static int g_apm_shm_map(uint32_t port) { (void)port; return 0; }
static void g_send_gpr(const uint8_t *p, uint32_t n) {
    (void)n; g_rsp_ready = 1; g_rsp_len = 32;
    last_graph_offset = g_le32(p + 24);
    g_put32(g_rsp + 24, APM_CMD_GRAPH_OPEN);
    g_put32(g_rsp + 28, reject_open ? 2 : 0);
}
static void g_drain(int ms) { (void)ms; }
static void g_wait(int ms) { (void)ms; }
static int g_apm_xfer(uint8_t *packet, uint32_t size, const char *what) {
    (void)what;
    if (size < 44) abort();
    printf("AUDIT {\"kind\":\"eos\",\"packet_bytes\":%u,\"opcode\":%u,"
        "\"payload_words\":[%u,%u,%u,%u,%u]}\n", size, g_le32(packet + 20),
        g_le32(packet + 24), g_le32(packet + 28), g_le32(packet + 32),
        g_le32(packet + 36), g_le32(packet + 40));
    return 0;
}
static int g_apm_set_cfg(uint32_t port, uint32_t iid, uint32_t pid,
    const void *data, uint32_t len, const char *what) {
    (void)port; (void)pid; (void)data; (void)len; (void)what;
    if (iid == G_VP_AW_IID) cfg_aw++;
    if (iid == 0x40bf) cfg_bf++;
    if (iid == 0x40c0) cfg_c0++;
    if ((boot_v8 && iid == G_VP_AW_IID) || iid == 0x40c8 || iid == 0x4165) {
        cfg_absent++; return -1;
    }
    return 0;
}
static int g_apm_set_cfg_batch(uint32_t port, const uint8_t *data, uint32_t len,
    uint32_t count, const char *what) {
    (void)port; (void)data; (void)len; (void)count; (void)what; return 0;
}
static int g_apm_set_cfg_oob(uint32_t port, const char *what) {
    (void)port; (void)what; return 0;
}
static void g_apm_get_cfg_dump(uint32_t port, uint32_t iid, uint32_t pid,
    uint32_t len, const char *what) {
    (void)port; (void)iid; (void)pid; (void)len; (void)what;
}
static int g_apm_graph_start(uint32_t port, uint32_t sg) { (void)port; (void)sg; return 0; }
static void g_apm_state_query_sync(const char *s) { (void)s; }
static void g_db_wait_report(const char *s) { (void)s; }
static void g_ibat_burst(const char *s) { (void)s; }
static void sweet_i2c_amp_readback(const char *s) { (void)s; }
static void delay(int ms) { (void)ms; }
static int g_apm_data_buffer(uint32_t port, uint32_t iid, uint32_t off,
    uint32_t len, uint32_t token) {
    (void)port; (void)iid; (void)token;
    if (off + len > sizeof(region)) abort();
    sent_buffers++;
    if (off == 0 && descriptor_size <= len && !memcmp(region, descriptor, descriptor_size))
        descriptor_buffers++;
    return 0;
}
static void
g_pcm_bytes_init(void)
{
	static int done = 0;
	uint32_t bb = 0;

	if (done) {
		return;
	}
	done = 1;
	(void)PE_parse_boot_argn("sweetdbb", &bb, sizeof(bb));
	if (bb >= G_PCM_BYTES && bb <= 0x4000u && (bb % 64u) == 0 &&
	    bb > g_pcm_bytes) {
		g_pcm_bytes = bb;
	}
	printf("SWEET MARK: dbb v1 bytes=%u ms=%u\n", g_pcm_bytes,
	    (unsigned)(g_pcm_bytes / 192u));
	sweet_log_sync();
}
static void
g_fill_tone(uint32_t off)
{
	int16_t *p = (int16_t *)(g_shm_va + off);

	g_pcm_bytes_init();

	/* The WR endpoint is 48 kHz / 16-bit / stereo (vendor SET_CFG), so
	 * 1152 bytes is 288 frames (6 ms).  The 48-entry table is one
	 * period: a 1 kHz tone.  Amplitude is deliberately low - the tone
	 * only has to prove the pipe, not be loud. */
	int g = 1;

	(void)PE_parse_boot_argn("sweetvol", &g, sizeof(g));
	if (g < 1 || g > 31) {
		g = 1;		/* the shipped 1/8 of 1/8, i.e. peak 1024 */
	}
	if (!g_vol_printed) {
		g_vol_printed = 1;
		printf("SWEET MARK: tone gain g=%d peak=%d\n", g, 1024 * g);
		sweet_log_sync();
	}
	for (uint32_t i = 0; i < g_pcm_bytes / 4; i++) {
		int16_t v = (int16_t)((g_sine_48[i % 48] * g) >> 3);

		p[2 * i] = v;
		p[2 * i + 1] = v;
	}
}
static int
g_pcm_prepare(void)
{
	if (g_apm_shm_map(0x2002u) != 0 || g_shm_pa == 0) {
		printf("SWEET GLINK: pcm: no shared memory\n");
		sweet_log_sync();
		return -1;
	}
	if (g_shm_va && !g_tone_filled) {
		g_fill_tone(G_PCM_OFF_A);
		g_fill_tone(g_pcm_bytes);
		__asm__ volatile("dsb sy" ::: "memory");
		g_tone_filled = 1;
		printf("SWEET GLINK: pcm: tone filled before start\n");
	} else if (!g_shm_va) {
		printf("SWEET GLINK: pcm: using SHIM-prefilled low DRAM\n");
	}
	sweet_log_sync();
	return 0;
}
static int
g_apm_graph_open_shm(uint32_t src_port, const uint8_t *payload, uint32_t len,
    uint32_t off, const char *what)
{
	uint8_t pkt[24 + 16];
	uint32_t pkt_size = sizeof(pkt);
	int descsep = 0;

	(void)PE_parse_boot_argn("sweetdescsep", &descsep, sizeof(descsep));
	if (descsep) {
		int nowait = 0;
		uint32_t dbb = 0;

		/* The separated descriptor is safe only with the two default PCM
		 * buffers; fire-and-forget walks the entire mapped region. */
		(void)PE_parse_boot_argn("sweetnowait", &nowait, sizeof(nowait));
		(void)PE_parse_boot_argn("sweetdbb", &dbb, sizeof(dbb));
		if (nowait || (dbb != 0 && dbb != 1152u)) {
			printf("SWEET MARK: descsep v1 refused nw=%d dbb=%u\n",
			    nowait, dbb);
			sweet_log_sync();
			return -1;
		}
		off = 0x4000u;
		printf("SWEET MARK: descsep v1 on=%d off=%#x len=%u\n",
		    descsep, off, len);
		sweet_log_sync();
	}

	if (!g_shm_mapped || !g_shm_va || off + len > g_shm_region_size()) {
		return -1;
	}
	memcpy((void *)(g_shm_va + off), payload, len);
	/* Never flush_dcache64() a low physical address: phystokv_range()
	 * only knows the ADT banks (0x840000000+) and panics otherwise.
	 * The window is Device-mapped, so the stores are already in DRAM. */
	__asm__ volatile("dsb sy" ::: "memory");

	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	g_put32(pkt + 8, src_port);
	g_put32(pkt + 12, 1);
	g_put32(pkt + 16, 0x400u);
	g_put32(pkt + 20, APM_CMD_GRAPH_OPEN);
	g_put32(pkt + 24 + 0, off);   /* payload offset inside the region */
	g_put32(pkt + 24 + 4, 0);
	g_put32(pkt + 24 + 8, g_shm_handle);
	g_put32(pkt + 24 + 12, len);
	printf("SWEET GLINK: APM %s src=%#x (shm handle=%#x len=%u)\n",
	    what, src_port, g_shm_handle, len);
	sweet_log_sync();
	g_rsp_ready = 0;
	g_rsp_len = 0;
	g_send_gpr(pkt, pkt_size);
	for (unsigned i = 0; i < 5000; i += 5) {
		g_drain(0);
		if (g_rsp_ready) {
			break;
		}
		g_wait(5);
		/* Beat once a second.  Three vendor4 boots left the ring at the graph_open
		 * line with neither a response nor the timeout print, and those two readings
		 * cannot be told apart without something that keeps writing while we wait. */
		if ((i % 200) == 195) {
			printf("SWEET GLINK: poll beat %u rsp=%d\n", i, g_rsp_ready);
			sweet_log_sync();
		}
	}
	if (!g_rsp_ready) {
		printf("SWEET GLINK: %s: no response\n", what);
		return -1;
	}
	if (g_rsp_len >= 32) {
		uint32_t status = g_le32(g_rsp + 28);

		printf("SWEET GLINK: %s rsp op=%#x status=%#x\n", what,
		    g_le32(g_rsp + 24), status);
		sweet_log_sync();
		return status == 0 ? 0 : -1;
	}
	printf("SWEET GLINK: %s rsp %u bytes\n", what, g_rsp_len);
	return -1;
}
static void
g_audio_tone(uint32_t src_port, uint32_t dst_port)
{
	int pcm = 0;

	g_pcm_bytes_init();

	/* The non-secure world cannot set S2CR.BYPASS (run71 read back
	 * TYPE=FAULT), so the DSP's first buffer access needs a real stage-2
	 * context.  g_smmu_probe() builds one under the same sweetpcm flag.
	 * Keep this probe opt-in: if the translation is wrong the DSP stalls
	 * the interconnect and the phone freezes. */
	(void)PE_parse_boot_argn("sweetpcm", &pcm, sizeof(pcm));
	if (!pcm) {
		printf("SWEET GLINK: pcm probe disabled (sweetpcm=1 to enable)\n");
		sweet_log_sync();
		return;
	}
	/* The vendor maps the PCM region from 0x2002 and feeds buffers from
	 * the stream port 0x2011; graph commands stay on 0x2010. */
	if (g_pcm_prepare() != 0) {
		printf("SWEET GLINK: pcm: tone skipped\n");
		sweet_log_sync();
		return;
	}

	{
		int nodata = 0;

		(void)PE_parse_boot_argn("sweetnodata", &nodata,
		    sizeof(nodata));
		if (nodata) {
			printf("SWEET GLINK: sweetnodata: START only, no "
			    "DATA_BUFFER\n");
			sweet_log_sync();
			return;
		}
	}
	/* Only the WR module accepts buffers (vendor trace: every
	 * DATA_BUFFER_V2 goes to the WR instance; SAL never answers).  Send
	 * exactly one probe there and stop guessing.
	 *
	 * sweetnowait=1 does not require a reply.  With the WR SET_CFG gone
	 * (sweetwrmf=0) nothing dies any more, but the DSP answers nothing
	 * either, so waiting for DATA_BUFFER_DONE means the tone loop never
	 * runs and we can never find out whether it is actually playing.
	 * Fire-and-forget is the only way to hear it. */
	int nw = 0;
	{
		static int nw_cached = -1;

		if (nw_cached < 0) {
			nw_cached = 0;
			(void)PE_parse_boot_argn("sweetnowait", &nw_cached,
			    sizeof(nw_cached));
		}
		nw = nw_cached;
	}
	/*
	 * sweetdspcheck=1 answers the question pacing cannot: is the DSP still
	 * ALIVE after a buffer?  l2/l3 stopped getting intents back after ~96
	 * sends, i.e. the DSP stopped reading GLINK at all - which would mean it
	 * died on the first buffer and every "completed" run since was talking
	 * to a corpse.  Ask APM for its state before and after one buffer.
	 */
	int dspcheck = 0;
	{
		(void)PE_parse_boot_argn("sweetdspcheck", &dspcheck,
		    sizeof(dspcheck));
	}
	if (dspcheck && dst_port == G_VP_WR_IID) {
		g_apm_state_query_sync("before-buf");
		{
			int r = g_apm_data_buffer(g_stream_port, G_VP_WR_IID,
			    G_PCM_OFF_A, G_PCM_BYTES, 0);

			printf("SWEET GLINK: dspcheck buffer -> %d\n", r);
			sweet_log_sync();
		}
		g_apm_state_query_sync("after-buf");
		return;
	}
	if (dst_port == G_VP_WR_IID) {
		int r = g_apm_data_buffer(g_stream_port, G_VP_WR_IID,
		    G_PCM_OFF_A, G_PCM_BYTES, 0);

		printf("SWEET GLINK: buffer probe dst=%#x -> %d%s\n",
		    G_VP_WR_IID, r, (r != 0 && nw) ? " (ignored)" : "");
		sweet_log_sync();
		if (r != 0 && !nw) {
			return;
		}
	}
	printf("SWEET GLINK: pcm tone start (dst=%#x)%s\n", dst_port,
	    nw ? " fire-and-forget" : "");
	g_ibat_burst("pre");

	/* 2000 buffers x 100 ms of waiting is 200 s; that only makes sense when
	 * the DSP actually replies.  Fire-and-forget needs a few seconds of
	 * audio, not five minutes. */
	/*
	 * The shim fills the whole shared-memory region with raw PCM
	 * (48 kHz/24-bit/stereo), not just the two 1152-byte buffers the vendor
	 * ping-pongs.  Streaming across the region plays that clip instead of a
	 * synthetic tone.  Stay one buffer short of the end so no offset runs
	 * past what SHM_MAP declared - the DSP validates the range.
	 */
	const uint32_t region = (uint32_t)g_shm_region_size();
	const uint32_t blocks = (region > g_pcm_bytes) ?
	    ((region - g_pcm_bytes) / g_pcm_bytes + 1u) : 1u;
	/* Fire-and-forget walks the whole region once, i.e. plays the entire
	 * clip; the reply-waiting path keeps the vendor's 2000 ping-pongs. */
	int tone_buffers = nw ? (int)blocks : 2000;
	{
		/* sweetbufs=N caps the loop so region size and flood length can
		 * be tested separately instead of changing together. */
		int cap = 0;

		(void)PE_parse_boot_argn("sweetbufs", &cap, sizeof(cap));
		if (cap > 0 && cap < tone_buffers) {
			tone_buffers = cap;
		}
	}

	if (tone_buffers > 1200) {
		tone_buffers = 1200;
	}

	/* 0276: clear the census here so the tone's numbers are the tone's; the
	 * single-buffer probes above have their own samples and are reported too,
	 * separately, instead of being averaged into the loop we care about. */
	g_db_wait_report("pre");
	for (int n = 0; n < tone_buffers; n++) {
		/* 0243: sampled once, at the middle of the feed loop, so the burst
		 * overlaps the tone; this is not a per-buffer probe. */
		if (n == tone_buffers / 2) {
			/*
			 * 0278: the amplifier register state DURING the stream has never
			 * been read.  The existing snapshots are 'pre-bitbang',
			 * 'pre-stream' and 'post-stream' - the last of which only runs
			 * after `pcm tone done` - so every conclusion about the PA being
			 * mute, unpowered or never told to play has been drawn from a
			 * moment that is strictly outside the thing being measured.
			 * This sits at the same midpoint as the IBAT burst, so the read
			 * overlaps live audio.
			 *
			 * Two honest costs.  (1) It is an I2C transaction in the middle
			 * of the feed loop, so it can perturb the very reply-latency
			 * census K10/K11 established; the census is still printed after
			 * the loop and 'dblat ... avg' moving in this round is a note
			 * about the instrument, not about the DSP.  (2) It reuses
			 * sweetampverify rather than inventing an eighth knob, because
			 * the boot-args field is at 578/590 and a new name would not fit;
			 * the witness line prints the state it actually read.
			 */
			static int amptone = -1;

			g_ibat_burst("tone");
			if (amptone < 0) {
				amptone = 0;
				(void)PE_parse_boot_argn("sweetampverify",
				    &amptone, sizeof(amptone));
				printf("SWEET MARK: amptone v1 on=%d at=%d/%d\n",
				    amptone, n, tone_buffers);
				sweet_log_sync();
			}
			if (amptone) {
				sweet_i2c_amp_readback("mid-tone");
			}
		}
		uint32_t off = nw ?
		    ((uint32_t)((unsigned)n % blocks) * g_pcm_bytes) :
		    ((n & 1) ? g_pcm_bytes : G_PCM_OFF_A);

		{
			/* sweetbufgap=<ms> paces the fire-and-forget stream.  Sending
			 * 909 buffers back to back killed the DSP (j1); 30 did not.
			 * One buffer is 1152 bytes = 192 frames = 4 ms at 48 kHz,
			 * so 4 ms is roughly real time and anything larger just
			 * underruns. */
			static int gap = -1;

			if (gap < 0) {
				gap = 0;
				(void)PE_parse_boot_argn("sweetbufgap", &gap,
				    sizeof(gap));
				if (gap > 0 && gap < 500) {
					printf("SWEET GLINK: tone pace %d ms/buffer\n",
					    gap);
					sweet_log_sync();
				}
			}
			if (gap > 0) {
				delay(gap);
			}
		}
		if (nw && (n & 31) == 0) {
			/* So a death inside the loop still tells us how far it got. */
			printf("SWEET GLINK: tone buf %d/%d off=%#x\n", n,
			    tone_buffers, off);
			sweet_log_sync();
		}
		if (g_apm_data_buffer(g_stream_port, dst_port, off, g_pcm_bytes,
		    (uint32_t)(n & 1)) != 0 && !nw) {
			printf("SWEET GLINK: pcm buffer %d not consumed\n", n);
			g_db_wait_report("aborted");
			return;
		}
	}
	printf("SWEET GLINK: pcm tone done (%d buffers)\n", tone_buffers);
	/* 0276: the number this whole round exists to get - how long each
	 * DATA_BUFFER_DONE actually took.  min=avg=max=0 with n=120 would mean
	 * the module answers the queue instantly (flood model); max around 20
	 * with n=120 would mean it pulls at real time like the vendor capture;
	 * timeout>0 says the window was too short to be evidence at all. */
	g_db_wait_report("tone");
	g_ibat_burst("post");

	(void)src_port;
}
static void
g_audio_graph_cfg_vp(uint32_t src_port)
{
	static const uint8_t tdm_intf[28] = {
		0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
	};
	/* Vendor AW parameter 0x10013D2A, captured from the speaker graph. */
	/*
	 * 0248: the vendor sends this 28-byte payload twice for the AW module -
	 * {0, 0x10013d1b, 1, 0, 0, 0, 0x632b} and {0, 0x10013d1c, 1, 0, 0, 0,
	 * 0x6f60}.  Our previous copy put the value in word [5] instead of [6]
	 * and never sent the second one, which fits ep9: the tone ran and not
	 * one PA register moved.
	 */
	static const uint8_t aw[28] = {
		0x00, 0x00, 0x00, 0x00, 0x1b, 0x3d, 0x01, 0x10,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x2b, 0x63, 0x00, 0x00,
	};
	static const uint8_t aw2[28] = {
		0x00, 0x00, 0x00, 0x00, 0x1c, 0x3d, 0x01, 0x10,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x60, 0x6f, 0x00, 0x00,
	};
	uint8_t mf[32];
	uint32_t v;
	int tdm_sup = 0;
	int rc;

	/* The AP endpoint is the WR_SHARED_MEM_EP, not SAL: in the vendor
	 * kprobe trace the module that receives media-format 0x0800100C and
	 * every DATA_BUFFER_V2 is the WR instance (0x40bd), while SAL only
	 * gets module configs.  96 kHz / 32 bit / stereo first, then the
	 * vendor's 48 kHz / 24 bit form if the module refuses it. */
	g_put32(mf + 0, 1);
	g_put32(mf + 4, 0x09001000);
	g_put32(mf + 8, 18);
	/* Vendor kprobe 2026-10-27, SET_CFG(0x40bd, 0x0800100C) byte for
	 * byte: 48000 Hz, 16-bit, q15, LSB-aligned, LE, stereo {1,2}.
	 * We had been declaring 24-bit here while the vendor says 16. */
	g_put32(mf + 12, 48000);
	mf[16] = 16; mf[17] = 0; mf[18] = 1; mf[19] = 0;
	mf[20] = 16; mf[21] = 0; mf[22] = 15; mf[23] = 0;
	mf[24] = 1;  mf[25] = 0; mf[26] = 2;  mf[27] = 0;
	mf[28] = 1;  mf[29] = 2;
	{
		/* The vendor trace never configures the WR by SET_CFG; the
		 * module's format comes from the graph.  sweetwrmf=0 drops our
		 * extra call so "harmless but wrong" can be ruled out without
		 * touching the graph. */
		int wrmf = 1;

		(void)PE_parse_boot_argn("sweetwrmf", &wrmf, sizeof(wrmf));
		rc = wrmf ? g_apm_set_cfg(src_port, G_VP_WR_IID, 0x0800100c,
		    mf, 30, "vp/wr_mf") : 0;
	}
	if (rc != 0) {
		g_put32(mf + 12, 48000);
		mf[16] = 16; mf[17] = 0;
		mf[20] = 16; mf[21] = 0; mf[22] = 15; mf[23] = 0;
		(void)g_apm_set_cfg(src_port, G_VP_WR_IID, 0x0800100c, mf, 30,
		    "vp/wr_mf48");
	}

	/* TDM sink input: 96 kHz / 32 bit / 3 channels. */
	g_put32(mf + 0, 1);
	g_put32(mf + 4, 0x09001000);
	g_put32(mf + 8, 19);
	g_put32(mf + 12, 96000);
	mf[16] = 32; mf[17] = 0; mf[18] = 1; mf[19] = 0;
	mf[20] = 32; mf[21] = 0; mf[22] = 31; mf[23] = 0;
	mf[24] = 1;  mf[25] = 0; mf[26] = 3;  mf[27] = 0;
	mf[28] = 1;  mf[29] = 2; mf[30] = 3;
	/* The vendor never gives the TDM endpoint a generic media format
	 * (0x0800100C); it sends the TDM-specific pair below, which is why
	 * 0x0800100C came back EUNSUPPORTED (0x3) every round.  Values are
	 * copied byte-for-byte from the vendor GPR trace. */
	{
		uint8_t tdm_rate[12] = { 0 };
		/*
		 * 0273: the vendor's OWN accepted playback (walked whole, all 112
		 * rows, in tools/vendor_tx_timeline.py) says
		 *   SET_CFG(0x428f, 0x08001017) psize=12 :: 0x00017700 0x00030018 0x1
		 * while we send 0x00030020 in the second word.  Read as halfwords
		 * that is bit width 24 vs our 32 (the channel half, 3, agrees).  The
		 * same 0x00030018 also appears in the vendor's MFC output record, so
		 * it is the vendor's consistent container width for this path, not a
		 * one-off.  sweettdmvendor bit3 switches to the vendor's word; 7 (the
		 * H1/J1/K1/K2 baseline) keeps 0x00030020 exactly as before, and this
		 * is a separate read of the same knob because the parse below (which
		 * masks the three invented params) happens after this send.
		 */
		int tdm_w24 = 0;
		uint32_t tdm_mf_w1 = 0x00030020u;

		(void)PE_parse_boot_argn("sweettdmvendor", &tdm_w24,
		    sizeof(tdm_w24));
		if (tdm_w24 & 8) {
			tdm_mf_w1 = 0x00030018u;
		}
		printf("SWEET MARK: tdmmf v1 tv=%d w1=%#x\n", tdm_w24, tdm_mf_w1);
		sweet_log_sync();

		g_put32(tdm_rate + 0, 96000);
		g_put32(tdm_rate + 4, tdm_mf_w1);
		g_put32(tdm_rate + 8, 1);
		(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001017,
		    tdm_rate, 12, "vp/tdm_rate");
	}

	{
		uint8_t tdm_cfg[28] = { 0 };

		g_put32(tdm_cfg + 0, 0);
		g_put32(tdm_cfg + 4, 2);
		g_put32(tdm_cfg + 8, 0x10001);
		g_put32(tdm_cfg + 12, 7);
		g_put32(tdm_cfg + 16, 0x200004);
		g_put32(tdm_cfg + 20, 0);
		g_put32(tdm_cfg + 24, 1);
		(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x0800101b,
		    tdm_cfg, 28, "vp/tdm_cfg");
	}

	/*
	 * sweetcccfg=<n> only.  The flag is now a selector so that a follow-up
	 * round is a local repack (boot args only) instead of a CI build: one
	 * kernel, four containers, still exactly one send per boot through
	 * g_apm_set_cfg_batch with dst=1 in-band and the same handle.
	 *
	 * The bodies are not invented - they are byte-exact from the vendor
	 * capture whose accepted responses are tabulated in
	 * docs/data/vendor-accepted-setcfg-2026-11-05.md (every SET_CFG there was
	 * answered status=0, and every accepted record names a parameter that
	 * belongs to the module its iid names).
	 *
	 * 1  C verbatim (0252): iid 0x428f with 0x08001017 size 12 and
	 *    0x0800101b size 28.  The control for this build; it answered 0 and
	 *    its 80-byte body is the vendor's own two records for this module.
	 * 2  The (2) case: iid = 1 (APM itself) with both params = 0x08001004,
	 *    same sizes and data as C.  H and J sent that pair **out** of band and
	 *    got 0x3; in-band it has never been sent, so this isolates the one
	 *    axis N1 closed off (an OOB body is never written back, so the out-of-
	 *    band readings and an in-band one were never compared for this pair).
	 * 3  Right module, wrong family: iid 0x428f carrying the vendor's *MFC*
	 *    records - param 0x08001024 size 12 twice, data {0xbb80,0x20020,
	 *    0x20001} and {0x17700,0x20020,0x20001}.  Body 64 B, packet 104 B -
	 *    the vendor's own multi-record geometry, replayed with our TDM iid.
	 * 4  Right family, wrong module: the AW module 0x42d3 (present in this
	 *    graph, and it answers vp/aw_cfg with 0) carrying C's two TDM records
	 *    unchanged.
	 *
	 * 1/3/4 are the pairing matrix (right-right, right-module-wrong-param,
	 * wrong-module-right-param) and every iid named is instantiated in the
	 * graph we actually open (g_graph_vp_wr: 0x4012, 0x4007, 0x418e, 0x428f,
	 * 0x4006, 0x48ba, 0x42d3, 0x40bd) - the vendor's MFC iids 0x40ca/0x40c8
	 * are deliberately **not** used, because a refusal there could not be told
	 * apart from "no such module".  Two refused readings with 1 accepted
	 * supports the pairing rule; any accepted reading kills it and leaves the
	 * per-record size and the count.  Any other value behaves like 1.
	 */
	{
		int sel = 0;

		(void)PE_parse_boot_argn("sweetcccfg", &sel, sizeof(sel));
		if (sel != 0) {
			static const uint32_t cc_rate[3] = { 96000, 0x30020, 1 };
			static const uint32_t cc_cfg[7] = {
				0, 2, 0x10001, 7, 0x200004, 0, 1
			};
			static const uint32_t mfc_a[3] = { 0xbb80u, 0x20020u, 0x20001u };
			static const uint32_t mfc_b[3] = { 0x17700u, 0x20020u, 0x20001u };
			uint32_t iid0, iid1, par0, par1, sz0, sz1;
			const uint32_t *d0, *d1;
			uint32_t n0, n1;
			uint8_t body[SWEET_CC_MAX];
			uint32_t off = 0;
			uint32_t i;

			switch (sel) {
			case 2:	/* APM itself + the graph-level param, in band */
				iid0 = iid1 = 1u;
				par0 = par1 = 0x08001004u;
				sz0 = sizeof(cc_rate); d0 = cc_rate; n0 = 3;
				sz1 = sizeof(cc_cfg); d1 = cc_cfg; n1 = 7;
				break;
			case 3:	/* TDM iid carrying the vendor's MFC records */
				iid0 = iid1 = G_VP_TDM_IID;
				par0 = par1 = 0x08001024u;
				sz0 = sizeof(mfc_a); d0 = mfc_a; n0 = 3;
				sz1 = sizeof(mfc_b); d1 = mfc_b; n1 = 3;
				break;
			case 4:	/* AW iid carrying C's TDM records */
				iid0 = iid1 = G_VP_AW_IID;
				par0 = 0x08001017u; par1 = 0x0800101bu;
				sz0 = sizeof(cc_rate); d0 = cc_rate; n0 = 3;
				sz1 = sizeof(cc_cfg); d1 = cc_cfg; n1 = 7;
				break;
			default:	/* 1 and anything else: C verbatim */
				iid0 = iid1 = G_VP_TDM_IID;
				par0 = 0x08001017u; par1 = 0x0800101bu;
				sz0 = sizeof(cc_rate); d0 = cc_rate; n0 = 3;
				sz1 = sizeof(cc_cfg); d1 = cc_cfg; n1 = 7;
				break;
			}

			memset(body, 0, sizeof(body));
			g_put32(body + off + 0, iid0);
			g_put32(body + off + 4, par0);
			g_put32(body + off + 8, sz0);
			for (i = 0; i < n0; i++) {
				g_put32(body + off + 16 + i * 4, d0[i]);
			}
			off += 16 + ((sz0 + 7u) & ~7u);
			g_put32(body + off + 0, iid1);
			g_put32(body + off + 4, par1);
			g_put32(body + off + 8, sz1);
			for (i = 0; i < n1; i++) {
				g_put32(body + off + 16 + i * 4, d1[i]);
			}
			off += 16 + ((sz1 + 7u) & ~7u);
			printf("SWEET MARK: cccfg v3 sel=%d iids=%#x/%#x params=%#x/%#x sizes=%u/%u\n",
			    sel, iid0, iid1, par0, par1, sz0, sz1);
			sweet_log_sync();
			(void)g_apm_set_cfg_batch(src_port, body, off, 2,
			    "cc_batch");
		}
	}
	/*
	 * sweetoobcfg=1 only, and not together with sweetcccfg: one container per
	 * round.  9 records x 32 bytes = 288 > 256, so this is the form that has to
	 * go through shared memory; 8 records would sit exactly on the in-band cap.
	 */
	{
		int on = 0;
		int cc_on = 0;

		(void)PE_parse_boot_argn("sweetoobcfg", &on, sizeof(on));
		/*
		 * One container per boot, now enforced in code rather than by the
		 * comment: with both flags set the two probes land in the same ring
		 * and a refusal cannot be attributed.  The in-band cc_batch has
		 * already gone out by here, so it is the out-of-band send - the one
		 * that has wedged the plane before - that gets dropped, and the log
		 * says so.
		 */
		(void)PE_parse_boot_argn("sweetcccfg", &cc_on, sizeof(cc_on));
		if (on != 0 && cc_on != 0) {
			printf("SWEET MARK: cccfg v3 both flags set, oob_batch suppressed\n");
			sweet_log_sync();
		} else if (on != 0) {
			(void)g_apm_set_cfg_oob(src_port, "oob_batch");
		}
	}

	g_put32(mf + 0, 96000);
	g_put32(mf + 4, 32u | (3u << 16));
	g_put32(mf + 8, 1);
	(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001017, mf, 12,
	    "vp/tdm_ep_mf");

	/*
	 * 0267: three of the five parameters we push to the TDM module (0x428f)
	 * appear nowhere in the vendor's accepted traffic - 0x08001018 = 1,
	 * 0x08001176 = 0 and 0x0800113c = {0, 12288000, 0, 0} come from the
	 * CDMA-era list in this driver.  All three answer status=0, but
	 * "command-level accepted" is not "harmless": F1 proved the graph plays
	 * a full-scale tone that nobody hears, and the AW-side payloads (0x42d3,
	 * param 0x10013d2a) are already byte-exact against the capture, so the
	 * TDM module's own parameter list is the last place our traffic differs
	 * from the vendor's.  sweettdmvendor is a bitmask so one build can bisect
	 * it with boot args only: bit0 drops 0x08001018, bit1 drops 0x08001176,
	 * bit2 drops 0x0800113c, 7 = exactly the vendor's two records.
	 */
	{
		int tdmsup = 0;
		int tp_shown = 0;

		(void)PE_parse_boot_argn("sweettdmvendor", &tdmsup,
		    sizeof(tdmsup));
		(void)PE_parse_boot_argn("sweettdmpwr", &tp_shown,
		    sizeof(tp_shown));
		printf("SWEET MARK: tdmcfg v1 sup=%d tdpwr=%d\n", tdmsup,
		    tp_shown);
		sweet_log_sync();
		tdm_sup = tdmsup;
	}

	if (!(tdm_sup & 1)) {
		v = 1;
		(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001018, &v, 4,
		    "vp/tdm_frame");
	} else {
		printf("SWEET MARK: tdmcfg v1 1018 not sent\n");
		sweet_log_sync();
	}
	/*
	 * 0248: the vendor trace never sends 0x08001176 at all - it comes from the
	 * CDMA-era list.  sweettdmpwr keeps the shipped behaviour (0) addressable
	 * without another build: 1 sends the value 1, 2 does not send the packet.
	 * 0267 adds the value to the log: before this, tp=0 and tp=1 printed
	 * exactly the same lines, so a round could not prove which value went out.
	 */
	{
		int tp = 0;

		(void)PE_parse_boot_argn("sweettdmpwr", &tp, sizeof(tp));
		if (tp != 2 && !(tdm_sup & 2)) {
			v = tp == 1 ? 1 : 0;
			printf("SWEET MARK: tdmcfg v1 1176 val=%u\n", v);
			sweet_log_sync();
			(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x08001176,
			    &v, 4, "vp/tdm_power");
		} else {
			printf("SWEET MARK: tdmcfg v1 1176 not sent (tp=%d sup=%d)\n",
			    tp, tdm_sup);
			sweet_log_sync();
		}
	}
	if (!(tdm_sup & 4)) {
		uint32_t clk[4] = { 0, 12288000, 0, 0 };

		(void)g_apm_set_cfg(src_port, G_VP_TDM_IID, 0x0800113c, clk,
		    16, "vp/tdm_clk");
	} else {
		printf("SWEET MARK: tdmcfg v1 113c not sent\n");
		sweet_log_sync();
	}

	(void)g_apm_set_cfg(src_port, G_VP_AW_IID, 0x10013d2a, aw,
	    sizeof(aw), "vp/aw_cfg");
	(void)g_apm_set_cfg(src_port, G_VP_AW_IID, 0x10013d2a, aw2,
	    sizeof(aw2), "vp/aw_cfg2");

	/*
	 * 0268: the one module in OUR graph that sits on the rate boundary has
	 * never been told what its output is.  The vendor's own accepted traffic
	 * (logs/gpr-both-20261105-223253.txt, dst=1, payload 64, packet 104)
	 * sends PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT 0x08001024 as a PAIR, one MFC
	 * instance per rate:
	 *   {iid 0x40ca, 0x08001024, 12, {48000, 0x00020020, 0x00020001}}
	 *   {iid 0x40c8, 0x08001024, 12, {96000, 0x00020020, 0x00020001}}
	 * and a second batch {0x401d -> 48000, 0x40b6 -> 96000}.  Our graph has a
	 * single MFC instance - MODULE_ID_MFC 0x07001015 at iid 0x48ba, byte-
	 * verified in g_graph_vp_wr's module table - and grep says 0x48ba appears
	 * in this file only inside comments.  sweetcccfg=3 aimed those two records
	 * at 0x428f (the TDM module): right params, wrong module, refused 0x1.
	 *
	 * sweetmfc is a selector so one build covers the three shapes:
	 *   1 = one record to OUR MFC at 96000 (the rate the TDM iface is set to)
	 *   2 = one record to OUR MFC at 48000 (the rate the WR endpoint feeds)
	 *   3 = the vendor's two-record batch, both records naming 0x48ba
	 *   4 = 0273: the vendor's OWN 14-byte output record, byte-for-byte from
	 *       the accepted playback timeline, to OUR 0x48ba
	 * Anything else sends nothing, so the shipped sequence is unchanged.
	 */
	{
		int mfc_sel = 0;
		int mfc_get = 0;

		(void)PE_parse_boot_argn("sweetmfc", &mfc_sel, sizeof(mfc_sel));
		(void)PE_parse_boot_argn("sweetmfcget", &mfc_get, sizeof(mfc_get));
		if (mfc_get != 0) {
			g_apm_get_cfg_dump(src_port, G_VP_MFC_IID, 0x08001024u,
			    72, "pre-set");
		}
		if (mfc_sel != 0) {
			static const uint32_t mfc_out[3] = {
				0x00017700u, 0x00020020u, 0x00020001u
			};
			static const uint32_t mfc_in[3] = {
				0x0000bb80u, 0x00020020u, 0x00020001u
			};
			uint8_t body[64];
			int mfc_rc = -1;
			int mfc_sent = 0;

			memset(body, 0, sizeof(body));
			if (mfc_sel == 1) {
				mfc_rc = g_apm_set_cfg(src_port, G_VP_MFC_IID,
				    0x08001024u, mfc_out, sizeof(mfc_out),
				    "vp/mfc_out96");
				mfc_sent = 1;
			} else if (mfc_sel == 2) {
				mfc_rc = g_apm_set_cfg(src_port, G_VP_MFC_IID,
				    0x08001024u, mfc_in, sizeof(mfc_in),
				    "vp/mfc_out48");
				mfc_sent = 1;
			} else if (mfc_sel == 3) {
				/* Two records, so dst = APM = 1, exactly how the
				 * vendor frames the pair.  Each record is
				 * 16 + align8(12) = 32 bytes, which is what makes
				 * the vendor's payload_size 64 and its packet 104 -
				 * striding by the raw 12 gives 56 and puts record
				 * two eight bytes early. */
				uint32_t off = 0;

				g_put32(body + off + 0, G_VP_MFC_IID);
				g_put32(body + off + 4, 0x08001024u);
				g_put32(body + off + 8, 12);
				g_put32(body + off + 16, 0x0000bb80u);
				g_put32(body + off + 20, 0x00020020u);
				g_put32(body + off + 24, 0x00020001u);
				off += 16 + ((12u + 7u) & ~7u);
				g_put32(body + off + 0, G_VP_MFC_IID);
				g_put32(body + off + 4, 0x08001024u);
				g_put32(body + off + 8, 12);
				g_put32(body + off + 16, 0x00017700u);
				g_put32(body + off + 20, 0x00020020u);
				g_put32(body + off + 24, 0x00020001u);
				off += 16 + ((12u + 7u) & ~7u);
				mfc_rc = g_apm_set_cfg_batch(src_port, body, off,
				    2, "vp/mfc_pair");
				mfc_sent = 1;
			} else if (mfc_sel == 4) {
				/* 0273: the vendor's 14-byte MFC output record,
				 * byte-for-byte from the accepted timeline walked by
				 * tools/vendor_tx_timeline.py:
				 *   SET_CFG(0x40c8, 0x08001024) psize=14 ::
				 *   {0x00017700, 0x00030018, 0x00020001, 0x00000003}
				 * (payload_size 32 = 16 + align8(14), which is what
				 * the capture shows and what our builder produces).
				 * Against our own sel=1 the deltas are the second
				 * word 0x00020020 -> 0x00030018 (32-bit -> the
				 * vendor's 24-bit container), psize 12 -> 14, and the
				 * fourth word 3 we never sent.  It goes to OUR MFC
				 * instance 0x48ba, and 0271/0272's GET reads it back,
				 * so unlike every earlier write this one is scored by
				 * what the module reports, not by our own rc. */
				static const uint32_t mfc_vnd[4] = {
					0x00017700u, 0x00030018u, 0x00020001u,
					0x00000003u
				};

				/* 14, NOT sizeof(): the vendor's declared size is
				 * 0x08001024 = 14 bytes, so the last word is only
				 * half inside the record (the kprobe prints the
				 * whole word, which is how 14 and four words sit
				 * together in one row).  Sending 16 would make our
				 * param_size disagree with the packet we are copying
				 * even though payload_size still rounds to 32; the
				 * host stub caught this. */
				mfc_rc = g_apm_set_cfg(src_port, G_VP_MFC_IID,
				    0x08001024u, mfc_vnd, 14,
				    "vp/mfc_vendor14");
				mfc_sent = 1;
			} else {
				printf("SWEET MARK: mfc v1 sel=%d out of range, "
				    "nothing sent\n", mfc_sel);
				sweet_log_sync();
			}
			if (mfc_sent) {
				printf("SWEET MARK: mfc v1 sel=%d iid=%#x param=%#x rc=%d\n",
				    mfc_sel, G_VP_MFC_IID, 0x08001024u, mfc_rc);
				sweet_log_sync();
			}
			if (mfc_get != 0) {
				g_apm_get_cfg_dump(src_port, G_VP_MFC_IID,
				    0x08001024u, 72, "post-set");
			}
		}
	}

	{
		/*
		 * sweetep4308=1 also configures the second endpoint instance the
		 * vendor sets these parameters on (iid 0x4308, slot_mask 3 instead
		 * of 7).  Default off: it is the next single variable, not this one.
		 */
		int ep4308 = 0;

		(void)PE_parse_boot_argn("sweetep4308", &ep4308, sizeof(ep4308));
		if (ep4308) {
			uint8_t mf2[12];
			uint8_t cf2[28] = { 0 };

			g_put32(mf2 + 0, 96000);
			g_put32(mf2 + 4, 32u | (2u << 16));
			g_put32(mf2 + 8, 1);
			(void)g_apm_set_cfg(src_port, 0x4308u, 0x08001017, mf2,
			    sizeof(mf2), "ep4308_mf");
			g_put32(cf2 + 0, 0);
			g_put32(cf2 + 4, 2);
			g_put32(cf2 + 8, 0x10001);
			g_put32(cf2 + 12, 3);
			g_put32(cf2 + 16, 0x200004);
			g_put32(cf2 + 20, 0);
			g_put32(cf2 + 24, 1);
			(void)g_apm_set_cfg(src_port, 0x4308u, 0x0800101b, cf2,
			    sizeof(cf2), "ep4308_cfg");
		}
	}

	{
		/*
		 * sweetvcfg2=1 replays the four module configs the vendor sends and
		 * we never do, byte for byte, from the two kprobe captures
		 * (docs/audio-closeout.md section 3): param 0x08001008 length 30 to
		 * 0x40bf and 0x40c0, param 0x08001024 length 14 to 0x40c8, and the
		 * port map 0x080010C4 = {0x428f} to 0x4165.
		 *
		 * Those four instance ids are NOT inside the graph we open - a byte
		 * search for bf 40 / c0 40 / c8 40 / 65 41 in g_graph_vp_wr returns
		 * zero hits - so the honest expectation is an error or silence.  The
		 * point of the boot is precisely that: if any of the four answers
		 * status 0 then the graph does contain them after all and the fix is
		 * "send more config"; if all four fail then the gap is structural and
		 * the only real options are the vendor blob (already judged dead in
		 * this same permission state) or extending the graph by hand.
		 *
		 * No param here is given a name: 0x08001008/0x08001024/0x080010C4 are
		 * identified only by position and length in the captured packets.
		 */
		int vcfg2 = 0;
		int rc_bf = -1, rc_c0 = -1, rc_c8 = -1, rc_65 = -1;
		uint8_t conv[30];
		uint8_t scl[14];
		uint8_t pmap[4];

		(void)PE_parse_boot_argn("sweetvcfg2", &vcfg2, sizeof(vcfg2));
		if (vcfg2) {
			memset(conv, 0, sizeof(conv));
			g_put32(conv + 0, 1);
			g_put32(conv + 4, 0x09001000);
			g_put32(conv + 8, 18);
			g_put32(conv + 12, 0x00010010);
			g_put32(conv + 16, 0x000f0010);
			g_put32(conv + 20, 0x00030001);
			g_put32(conv + 24, 0x00020000);
			conv[28] = 0x01;
			conv[29] = 0x02;

			memset(scl, 0, sizeof(scl));
			g_put32(scl + 0, 96000);
			g_put32(scl + 4, 0x00030018);
			g_put32(scl + 8, 0x00020001);
			scl[12] = 0x03;

			g_put32(pmap + 0, G_VP_TDM_IID);

			rc_bf = g_apm_set_cfg(src_port, 0x40bfu, 0x08001008u,
			    conv, sizeof(conv), "vp/cfg_40bf");
			rc_c0 = g_apm_set_cfg(src_port, 0x40c0u, 0x08001008u,
			    conv, sizeof(conv), "vp/cfg_40c0");
			rc_c8 = g_apm_set_cfg(src_port, 0x40c8u, 0x08001024u,
			    scl, sizeof(scl), "vp/cfg_40c8");
			rc_65 = g_apm_set_cfg(src_port, 0x4165u, 0x080010c4u,
			    pmap, sizeof(pmap), "vp/cfg_4165");
			printf("SWEET MARK: vcfg2 rc bf=%d c0=%d c8=%d "
			    "p65=%d\n", rc_bf, rc_c0, rc_c8, rc_65);
			sweet_log_sync();
		}
	}

	(void)sizeof(tdm_intf);
}
static int
g_apm_cmd(uint32_t src_port, uint32_t dst_port, const uint8_t *payload,
    uint32_t plen, uint32_t opcode, const char *what)
{
	uint8_t pkt[24 + 16 + 1024];
	uint32_t pkt_size = 24 + 16 + plen;

	if (plen > sizeof(pkt) - 40) {
		return -1;
	}
	memset(pkt, 0, sizeof(pkt));
	g_put32(pkt + 0, (6u << 4) | (pkt_size << 8));
	g_put32(pkt + 4, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	{
		/* sweetdbsrc=<port>: push the buffer from another client so the
		 * sink graph and the buffers can be aligned either way. */
		static uint32_t dbsrc;

		if (dbsrc == 0) {
			int v = 0;

			(void)PE_parse_boot_argn("sweetdbsrc", &v, sizeof(v));
			dbsrc = (v > 0) ? (uint32_t)v : 0xffffffffu;
		}
		g_put32(pkt + 8, (dbsrc != 0xffffffffu) ? dbsrc : src_port);
	}     /* GPR client port */
	g_put32(pkt + 12, dst_port);    /* 1 = APM, else module iid */
	g_put32(pkt + 16, 0x100u + opcode);
	g_put32(pkt + 20, opcode);
	g_put32(pkt + 24 + 0, 0);       /* payload_address_lsw  (inline) */
	g_put32(pkt + 24 + 4, 0);       /* payload_address_msw */
	g_put32(pkt + 24 + 8, 0);       /* mem_map_handle */
	g_put32(pkt + 24 + 12, plen);   /* payload_size */
	memcpy(pkt + 40, payload, plen);

	printf("SWEET GLINK: APM %s src=%#x dst=%#x (%u bytes)\n", what,
	    src_port, dst_port, pkt_size);
	sweet_log_sync();
	return g_apm_xfer(pkt, pkt_size, what);
}
static void run_b1(void) { uint32_t port = 0x2010u; g_stream_port = port;

	{
		int v8 = 0;

		(void)PE_parse_boot_argn("sweetvgraph8", &v8, sizeof(v8));
		printf("SWEET MARK: noaw v1 on=%d bytes=%u start=%#x\n", v8,
		    (unsigned)sizeof(g_graph_aform_no_aw), 0xb0000002u);
		sweet_log_sync();
		if (v8) {
			if (g_pcm_prepare() == 0 &&
			    g_apm_graph_open_shm(port, g_graph_aform_no_aw,
			    (uint32_t)sizeof(g_graph_aform_no_aw), 0,
			    "graph_open/no-aw") == 0) {
				g_audio_graph_cfg_vp(port);
				if (g_apm_graph_start(port, 0xb0000002u) == 0) {
					printf("SWEET GLINK: no-aw started\n");
					sweet_log_sync();
					g_audio_tone(port, G_VP_WR_IID);
				} else {
					printf("SWEET GLINK: no-aw did not start\n");
					sweet_log_sync();
				}
				return;
			}
			printf("SWEET GLINK: no-aw open refused\n");
			sweet_log_sync();
		}
	}
 fallback_count++;
}

static void reset(void) {
    memset(region, 0, sizeof(region)); g_shm_va = (vm_offset_t)region;
    g_tone_filled = 0; cfg_aw = cfg_bf = cfg_c0 = cfg_absent = 0;
    fallback_count = sent_buffers = descriptor_buffers = 0;
    last_graph_offset = 0;
}
static void separation_case(int nowait, int dbb, uint32_t size) {
    uint8_t before[G_PCM_BYTES];
    reset(); boot_descsep = 1; boot_nowait = nowait; boot_dbb = dbb;
    reject_open = 0;
    if (g_pcm_prepare()) abort();
    memcpy(before, region, sizeof(before));
    int rc = g_apm_graph_open_shm(0x2010, g_graph_vp_wr_dec_cnv, size, 0, "host/separated");
    printf("AUDIT {\"kind\":\"separation\",\"nowait\":%d,\"dbb\":%d,\"size\":%u,"
        "\"rc\":%d,\"packet_offset\":%u,\"pcm_a_changed\":%d}\n",
        nowait, dbb, size, rc, last_graph_offset, memcmp(before, region, sizeof(before)) != 0);
    boot_descsep = boot_nowait = boot_dbb = 0;
}
static void memory_case(const uint8_t *body, size_t size, uint32_t off) {
    uint8_t before[G_PCM_BYTES];
    reset(); boot_v8 = 0; reject_open = 0;
    descriptor = body; descriptor_size = size;
    if (g_pcm_prepare()) abort();
    memcpy(before, region, sizeof(before));
    if (g_apm_graph_open_shm(0x2010, body, (uint32_t)size, off, "host/open")) abort();
    if (g_pcm_prepare()) abort();
    int changed = memcmp(before, region, sizeof(before)) != 0;
    g_audio_tone(0x2010, G_VP_WR_IID);
    printf("AUDIT {\"kind\":\"memory\",\"body_bytes\":%zu,\"offset\":%u,"
        "\"pcm_a_changed\":%d,\"descriptor_buffers\":%d,\"total_buffers\":%d}\n",
        size, off, changed, descriptor_buffers, sent_buffers);
}
int main(void) {
    memory_case(g_graph_vp_wr_dec_cnv, sizeof(g_graph_vp_wr_dec_cnv), 0);
    memory_case(g_graph_aform_no_aw, sizeof(g_graph_aform_no_aw), 0);
    memory_case(g_graph_vp_wr_dec_cnv, sizeof(g_graph_vp_wr_dec_cnv), 0x4000);
    for (int vc = 0; vc <= 1; vc++) {
        reset(); boot_v8 = 0; boot_vcfg2 = vc;
        g_audio_graph_cfg_vp(0x2010);
        printf("AUDIT {\"kind\":\"cfg\",\"vcfg2\":%d,\"aw\":%d,\"dec\":%d,"
            "\"cnv\":%d,\"absent\":%d}\n", vc, cfg_aw, cfg_bf, cfg_c0, cfg_absent);
    }
    for (int refuse = 0; refuse <= 1; refuse++) {
        reset(); boot_v8 = 1; boot_vcfg2 = 0; reject_open = refuse;
        descriptor = g_graph_aform_no_aw; descriptor_size = sizeof(g_graph_aform_no_aw);
        run_b1();
        printf("AUDIT {\"kind\":\"b1\",\"refused\":%d,\"aw\":%d,"
            "\"absent\":%d,\"fallback\":%d,\"total_buffers\":%d}\n",
            refuse, cfg_aw, cfg_absent, fallback_count, sent_buffers);
    }
    uint8_t eos[4];
    g_put32(eos, 1u);
    if (g_apm_cmd(0x2010, G_VP_WR_IID, eos, sizeof(eos), 0x04001002u, "host/eos")) abort();
    separation_case(0, 0, 856);
    separation_case(1, 0, 856);
    separation_case(0, 4096, 856);
    separation_case(0, 0, 4097);
    return 0;
}
