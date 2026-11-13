
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
static uint16_t regs[256];
static uint8_t pointer_reg, last_tx[3];
static unsigned calls, last_nt, last_nr;
static int last_stretch, force_error;
static void delay(int us) { (void)us; }
void sweet_log_sync(void) {}
static int se15_msg(uint8_t addr, const uint8_t *tx, unsigned nt,
    uint8_t *rx, unsigned nr, bool stretch) {
    (void)addr; calls++; last_nt=nt; last_nr=nr; last_stretch=stretch;
    memset(last_tx,0,sizeof(last_tx));
    if(nt>3 || (nt && nr)) abort();
    if(nt) memcpy(last_tx,tx,nt);
    if(force_error) return force_error;
    if(nt) {
        pointer_reg=tx[0];
        if(nt==3) regs[tx[0]]=(uint16_t)((tx[1]<<8)|tx[2]);
    } else if(nr==2) {
        rx[0]=(uint8_t)(regs[pointer_reg]>>8); rx[1]=(uint8_t)regs[pointer_reg];
    }
    return 0;
}
static void reset_model(void) {
    memset(regs,0,sizeof(regs)); regs[0]=0x2032; regs[4]=0x4003; regs[5]=0x38;
    pointer_reg=0; calls=last_nt=last_nr=0; force_error=0;
}
static const struct {
	uint8_t reg;
	uint16_t val;
} aw882xx_music_a[44] = {
	{ 0x03, 0xffff }, { 0x04, 0xb440 }, { 0x05, 0x05a9 }, { 0x06, 0x94e9 },
	{ 0x07, 0x007d }, { 0x08, 0x2168 }, { 0x09, 0x3940 }, { 0x0a, 0x0030 },
	{ 0x0b, 0x01e0 }, { 0x0c, 0x0064 }, { 0x0d, 0xa05b }, { 0x0e, 0x2392 },
	{ 0x0f, 0xee80 }, { 0x15, 0x9838 }, { 0x20, 0x2001 }, { 0x30, 0x460c },
	{ 0x31, 0xc989 }, { 0x32, 0x3541 }, { 0x33, 0x4db8 }, { 0x34, 0xd6f9 },
	{ 0x35, 0x7ace }, { 0x36, 0xec7c }, { 0x37, 0x000c }, { 0x38, 0x0030 },
	{ 0x50, 0x0080 }, { 0x52, 0x20c4 }, { 0x53, 0x0010 }, { 0x54, 0x0000 },
	{ 0x55, 0x00d8 }, { 0x59, 0x000c }, { 0x5a, 0xc103 }, { 0x60, 0x0c0b },
	{ 0x61, 0x6979 }, { 0x62, 0x04c1 }, { 0x63, 0x260f }, { 0x64, 0x15c3 },
	{ 0x65, 0x8d05 }, { 0x66, 0x7f07 }, { 0x67, 0x0002 }, { 0x68, 0x3002 },
	{ 0x69, 0x04a0 }, { 0x6a, 0xa068 }, { 0x6b, 0x8884 }, { 0x6c, 0x1000 },
};
static int
se15_aw_read(uint8_t addr, uint8_t reg, uint16_t *out)
{
	uint8_t b[2] = { 0, 0 };
	int r;

	r = se15_msg(addr, &reg, 1, NULL, 0, true);
	if (r != 0) {
		return r;
	}
	r = se15_msg(addr, NULL, 0, b, 2, false);
	if (r != 0) {
		return r;
	}
	*out = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
	return 0;
}
static int
se15_aw_write(uint8_t addr, uint8_t reg, uint16_t val)
{
	uint8_t b[2] = { (uint8_t)(val >> 8), (uint8_t)val };

	if (se15_msg(addr, &reg, 1, NULL, 0, true) != 0) {
		return -1;
	}
	if (se15_msg(addr, b, 2, NULL, 0, false) != 0) {
		return -1;
	}
	return 0;
}
static int
se15_aw_init(uint8_t addr)
{
	unsigned n = sizeof(aw882xx_music_a) / sizeof(aw882xx_music_a[0]);
	int bad = 0;

	if (se15_aw_write(addr, 0x00, 0x55aa) != 0) {
		printf("SWEET I2C: geni amp %#x soft reset failed\n", addr);
		return -1;
	}
	delay(2);
	for (unsigned i = 0; i < n; i++) {
		if (se15_aw_write(addr, aw882xx_music_a[i].reg,
		    aw882xx_music_a[i].val) != 0) {
			bad++;
		}
	}
	printf("SWEET I2C: geni amp %#x profile %u/%u\n", addr, n - bad, n);
	return bad ? -1 : 0;
}

int main(void) {
    reset_model();
    int rc=se15_aw_write(0x34,4,0xb440);
    printf("ROW {\"kind\":\"write\",\"rc\":%d,\"calls\":%u,\"nt\":%u,"
        "\"nr\":%u,\"stretch\":%d,\"tx\":[%u,%u,%u],\"readback\":%u}\n",
        rc,calls,last_nt,last_nr,last_stretch,last_tx[0],last_tx[1],last_tx[2],regs[4]);
    reset_model(); uint16_t out=0; rc=se15_aw_read(0x35,0,&out);
    printf("ROW {\"kind\":\"read\",\"rc\":%d,\"calls\":%u,\"value\":%u}\n",rc,calls,out);
    reset_model(); rc=se15_aw_init(0x34);
    printf("ROW {\"kind\":\"profile\",\"rc\":%d,\"calls\":%u,"
        "\"sysctrl\":%u,\"sysctrl2\":%u}\n",rc,calls,regs[4],regs[5]);
    reset_model(); force_error=-2; rc=se15_aw_write(0x34,4,0xb440);
    printf("ROW {\"kind\":\"error\",\"rc\":%d,\"calls\":%u,\"readback\":%u}\n",rc,calls,regs[4]);
    return 0;
}
