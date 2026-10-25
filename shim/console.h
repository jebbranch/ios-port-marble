#pragma once
#include <stdint.h>

#define FB_BASE		0xb8000000UL	/* marble cont_splash_region */
#define FB_W		1080
#define FB_H		2400
#ifndef FB_STRIDE
#define FB_STRIDE	4352		/* 1080 aligned to 32 px; try 4320 if skewed */
#endif

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define BLACK	0xff000000U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
/* Fill the screen with a w*h BGRA image (stride is FB_STRIDE). */
void con_image(const uint32_t *img, int w, int h);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
