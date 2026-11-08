
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#define __asm__(...)
typedef uintptr_t vm_offset_t;
static vm_offset_t sw_i2c_gpio;
static uint32_t pins[1026], spksw_pad[2];
static int se15_probe_calls, sw_i2c_ok;
static int rst_a, rst_b, boot_nobb, disabled, map_fail, init_fail, bad_id, profile_fail;
static int scans, drive_low_after_profile, profile_attempts, init_calls, snapshots;
static int scan_success;
#define SW_I2C_TLMM_PA  0x0f138000u     /* pinctrl@f000000 + 56*0x1000 */
#define SW_I2C_TLMM_SZ  0x2000u
#define SW_I2C_SCL      0u
#define SW_I2C_SDA      1u
#define SW_AMP_RST_A_PA 0x0f101000u     /* amp@34 reset: GPIO1 */
#define SW_AMP_RST_B_PA 0x0f178000u     /* amp@35 reset: GPIO120 */
#define SW_AMP_SPKSW_PA 0x0f134000u     /* amp@34 spksw: GPIO52 */
#define SW_GPIO_CFG(n)   ((uint32_t)(n) * 0x1000u)
#define SW_GPIO_IO(n)    (SW_GPIO_CFG(n) + 4u)
int PE_parse_boot_argn(const char *name, void *out, unsigned long len) {
    int value;
    if(!strcmp(name,"sweetnobb") && boot_nobb >= 0) value=boot_nobb;
    else if(!strcmp(name,"sweeti2c") && disabled) value=0;
    else return 0;
    if(len!=sizeof(value)) abort(); memcpy(out,&value,len); return 1;
}
vm_offset_t ml_io_map(vm_offset_t address, unsigned long size) {
    (void)address; (void)size; return map_fail ? 0 : (vm_offset_t)pins;
}
vm_offset_t sw_pad_map(uint32_t address) {
    return address == SW_AMP_SPKSW_PA ? (vm_offset_t)spksw_pad : 0;
}
uint32_t sw_rd(uint32_t offset) {
    if(offset/4 >= 1026) abort(); return pins[offset/4];
}
void sw_wr(uint32_t offset, uint32_t value) {
    if(offset/4 >= 1026) abort(); pins[offset/4]=value;
}
void sw_gpio_drive(uint32_t address, bool high) {
    if(address==SW_AMP_RST_A_PA || address==SW_AMP_RST_B_PA) {
        if(!high && profile_attempts) drive_low_after_profile++;
        if(address==SW_AMP_RST_A_PA) rst_a=high; else rst_b=high;
    } else if(address==SW_AMP_SPKSW_PA) {
        spksw_pad[0]=0x200; spksw_pad[1]=high ? 3 : 0;
    }
}
int sw_gpio_read_level(uint32_t address) {
    if(address==SW_AMP_RST_A_PA) return rst_a;
    if(address==SW_AMP_RST_B_PA) return rst_b;
    if(address==SW_AMP_SPKSW_PA) return spksw_pad[1]&1u;
    return 1;
}
void sw_gpio_cfg(unsigned pin, unsigned pull, bool output) {
    pins[SW_GPIO_CFG(pin)/4]=(output ? 0x200u : 0u)+pull;
}
void delay(int usecs) { (void)usecs; }
void sweet_log_sync(void) {}
void sw_scl(int high) { pins[1]=high ? 3 : 0; }
void sw_i2c_delay(void) {}
void sw_i2c_stop(void) {}
void sw_i2c_pins_up(void) { pins[0]=pins[1024]=0x200; }
void sw_i2c_pins_down(void) { pins[0]=pins[1024]=0x100; }
int sw_sda_read(void) { return 1; }
int sw_i2c_scan(const char *tag) { (void)tag; scans++; return scan_success; }
int sw_aw_read(uint8_t addr, uint8_t reg, uint16_t *out) {
    (void)addr; (void)reg; (void)out; return -1;
}
int sw_aw_init(uint8_t addr) { (void)addr; return 0; }
int se15_init(void) {
    init_calls++; if(init_fail) return 0;
    sw_gpio_drive(SW_AMP_RST_A_PA,false); sw_gpio_drive(SW_AMP_RST_A_PA,true);
    sw_gpio_drive(SW_AMP_RST_B_PA,false); sw_gpio_drive(SW_AMP_RST_B_PA,true);
    pins[0]=pins[1024]=0x87; pins[1]=3; pins[1025]=1; return 1;
}
void se15_tdm_pins(void) {}
int se15_aw_read(uint8_t addr, uint8_t reg, uint16_t *out) {
    (void)reg; *out=(bad_id && addr==0x35) ? 0xffff : 0x2032; return 0;
}
int se15_aw_init(uint8_t addr) {
    profile_attempts++; return profile_fail && addr==0x35 ? -1 : 0;
}
void sweet_i2c_amp_readback(const char *tag) {
    if(!strcmp(tag,"post-init")) snapshots++;
}
void
sweet_i2c_probe_amps(void)
{
	static const uint8_t addrs[2] = { 0x34, 0x35 };
	int on = 1, amp_on = 1, nobb = 1;
	int init_ok = 0, profiles = 0;
	vm_offset_t spksw = 0;
	uint32_t cfg_scl, cfg_sda, io_scl, io_sda;
	uint32_t spksw_cfg = 0, spksw_io = 0;

	(void)PE_parse_boot_argn("sweeti2c", &on, sizeof(on));
	(void)PE_parse_boot_argn("sweetamp", &amp_on, sizeof(amp_on));
	(void)PE_parse_boot_argn("sweetnobb", &nobb, sizeof(nobb));
	if (!on) {
		printf("SWEET I2C: disabled (sweeti2c=0)\n");
		return;
	}

	sw_i2c_gpio = ml_io_map(SW_I2C_TLMM_PA, SW_I2C_TLMM_SZ);
	if (!sw_i2c_gpio) {
		printf("SWEET I2C: no TLMM mapping\n");
		return;
	}
	cfg_scl = sw_rd(SW_GPIO_CFG(SW_I2C_SCL));
	cfg_sda = sw_rd(SW_GPIO_CFG(SW_I2C_SDA));
	printf("SWEET I2C: TLMM @%#x scl cfg=%#x sda cfg=%#x\n",
	    SW_I2C_TLMM_PA, cfg_scl, cfg_sda);

	/* The first probe runs before UFS is mounted, so a fault there would
	 * take the log with it.  Only touch the GENI controller from the later
	 * audio pass, after the disk and GLINK are up. */
	if (se15_probe_calls++ > 0) {
		if (se15_init()) {
			init_ok = 1;
			se15_tdm_pins();
			for (int i = 0; i < 2; i++) {
				uint16_t id = 0;
				int r = se15_aw_read(addrs[i], 0x00, &id);

				printf("SWEET I2C: geni amp %#x r=%d id=%#x\n",
				    addrs[i], r, id);
				if (r == 0 && id == 0x2032) {
					if (se15_aw_init(addrs[i]) == 0) {
						profiles++;
					}
				}
			}
		/*
		 * 0240: read the PA back at the last moment GENI is still usable.
		 * The 0237 boot proved the ordering: this same se15_aw_read()
		 * returned id=0x2032 a few lines above (ring-rxsnap.txt:2313), and
		 * the identical call forty log lines later timed out with irq=0
		 * (ibid.:2358).  Between them sits only our own bit-bang phase,
		 * which switches exactly these two pads to GPIO
		 * (sw_gpio_cfg(SW_I2C_SCL/SDA, 0, false) below), i.e. off the QUP.
		 * So "post-stream read-back fails" was never about the stream.
		 * This call is placed before that phase to keep one window where
		 * the instrument is known-good; gated by sweetampverify so no boot
		 * changes unless the flag is given.
		 */
		{
			int ampverify = 0;

			(void)PE_parse_boot_argn("sweetampverify", &ampverify,
			    sizeof(ampverify));
			if (ampverify) {
				sweet_i2c_amp_readback("pre-bitbang");
			}
		}

		}
	}

	/* Normal playback must not change control pins after the PA setup. */
	printf("SWEET MARK: ampguard v1 pass=%u skip=%d init=%d profiles=%d rstA=%d rstB=%d\n",
	    (unsigned)se15_probe_calls, nobb, init_ok, profiles,
	    sw_gpio_read_level(SW_AMP_RST_A_PA),
	    sw_gpio_read_level(SW_AMP_RST_B_PA));
	sweet_log_sync();
	if (nobb) {
		if (init_ok) {
			sweet_i2c_amp_readback("post-init");
		}
		printf("SWEET MARK: bitbang skipped\n");
		sweet_log_sync();
		return;
	}

	/* The opt-in diagnostic must restore the bus and speaker-switch pads. */
	cfg_scl = sw_rd(SW_GPIO_CFG(SW_I2C_SCL));
	cfg_sda = sw_rd(SW_GPIO_CFG(SW_I2C_SDA));
	io_scl = sw_rd(SW_GPIO_IO(SW_I2C_SCL));
	io_sda = sw_rd(SW_GPIO_IO(SW_I2C_SDA));
	spksw = sw_pad_map(SW_AMP_SPKSW_PA);
	if (spksw) {
		spksw_cfg = *(volatile uint32_t *)spksw;
		spksw_io = *(volatile uint32_t *)(spksw + 4);
	}

	/* Leave the bus alone and look at what the bootloader handed us.  The
	 * external I2C pull-ups are a good proxy for the audio I/O rail: if
	 * they are unpowered the lines sit low even with both pins released. */
	printf("SWEET I2C: levels before (rstA=%d spksw=%d scl=%d sda=%d "
	    "rstB=%d irqA=%d irqB=%d)\n",
	    sw_gpio_read_level(SW_AMP_RST_A_PA),
	    sw_gpio_read_level(SW_AMP_SPKSW_PA),
	    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SCL)) & 1u),
	    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SDA)) & 1u),
	    sw_gpio_read_level(SW_AMP_RST_B_PA),
	    sw_gpio_read_level(0x0f158000u),    /* GPIO88, amp A irq */
	    sw_gpio_read_level(0x0f13f000u));   /* GPIO63, amp B irq */
	sw_gpio_cfg(SW_I2C_SCL, 0, false);
	sw_gpio_cfg(SW_I2C_SDA, 0, false);
	delay(200);
	printf("SWEET I2C: floating (no pull) scl=%d sda=%d\n",
	    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SCL)) & 1u),
	    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SDA)) & 1u));

	int found;

	/*
	 * The vendor's aw882xx_hw_reset() in this build is a no-op: its GPIO
	 * code is commented out ("reset is not available").  Android therefore
	 * never pulses these lines, and the chip answers I2C in whatever state
	 * the boot chain left it.  Never pulse a running chip here either: only
	 * release a line that is actually held low, then leave it high.
	 */
	if (sw_gpio_read_level(SW_AMP_RST_A_PA) == 0) {
		sw_gpio_drive(SW_AMP_RST_A_PA, true);
	}
	if (sw_gpio_read_level(SW_AMP_RST_B_PA) == 0) {
		sw_gpio_drive(SW_AMP_RST_B_PA, true);
	}
	delay(20000);  /* let the chips settle before the first address */
	printf("SWEET I2C: resets released (A=%d B=%d)\n",
	    sw_gpio_read_level(SW_AMP_RST_A_PA),
	    sw_gpio_read_level(SW_AMP_RST_B_PA));

	/* Level check: with the pin driven, the IO register should follow
	 * the output.  A stubborn 1 while driving low means the line is
	 * held up externally (or the pin is not ours). */
	if (sw_i2c_gpio) {
		sw_gpio_cfg(SW_I2C_SDA, 0, true);
		sw_wr(SW_GPIO_IO(SW_I2C_SDA), sw_rd(SW_GPIO_IO(SW_I2C_SDA)) & ~2u);
		delay(100);
		printf("SWEET I2C: sda driven low reads %d\n",
		    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SDA)) & 1u));
		sw_wr(SW_GPIO_IO(SW_I2C_SDA), sw_rd(SW_GPIO_IO(SW_I2C_SDA)) | 2u);
		delay(100);
		printf("SWEET I2C: sda driven high reads %d\n",
		    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SDA)) & 1u));
	}

	/* Prove the clock line actually moves: drive it low and high and read
	 * the pad back.  A stuck 1 while driven low means the pin is not ours
	 * (or the QUP still owns it). */
	sw_gpio_cfg(SW_I2C_SCL, 0, true);
	sw_scl(0);
	delay(200);
	printf("SWEET I2C: scl driven low reads %d\n",
	    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SCL)) & 1u));
	sw_scl(1);
	delay(200);
	printf("SWEET I2C: scl high reads %d\n",
	    (int)(sw_rd(SW_GPIO_IO(SW_I2C_SCL)) & 1u));

	/* Recover a bus left mid-transfer by the previous OS: nine clocks. */
	for (int i = 0; i < 9; i++) {
		sw_scl(0);
		sw_i2c_delay();
		sw_scl(1);
		sw_i2c_delay();
	}
	sw_i2c_stop();

	sw_i2c_pins_up();
	printf("SWEET I2C: bus idle, sda=%d\n", sw_sda_read());
	found = sw_i2c_scan("released");

	if (found == 0) {
		/* Android's amp driver leaves spksw low when it reads the chip
		 * ID; earlier runs drove it high before scanning.  Try both. */
		sw_gpio_drive(SW_AMP_SPKSW_PA, true);
		delay(5);
		printf("SWEET I2C: spksw high\n");
		found = sw_i2c_scan("spksw-high");
	}

	/* A failed address scan is not a reason to assert amplifier reset. */

	for (int i = 0; i < 2; i++) {
		uint16_t id = 0, sysst = 0, sysctrl = 0;

		if (sw_aw_read(addrs[i], 0x00, &id) != 0) {
			printf("SWEET I2C: amp %#x no ack\n", addrs[i]);
			continue;
		}
		(void)sw_aw_read(addrs[i], 0x01, &sysst);
		(void)sw_aw_read(addrs[i], 0x04, &sysctrl);
		printf("SWEET I2C: amp %#x id=%#x sysst=%#x sysctrl=%#x\n",
		    addrs[i], id, sysst, sysctrl);
		if (id == 0x2032) {
			sw_i2c_ok++;
			if (amp_on) {
				int rc = sw_aw_init(addrs[i]);

				printf("SWEET I2C: amp %#x init %s\n",
				    addrs[i], rc == 0 ? "ok" : "FAILED");
			}
		}
	}

	sw_i2c_pins_down();
	sw_wr(SW_GPIO_IO(SW_I2C_SCL), io_scl);
	sw_wr(SW_GPIO_IO(SW_I2C_SDA), io_sda);
	sw_wr(SW_GPIO_CFG(SW_I2C_SCL), cfg_scl);
	sw_wr(SW_GPIO_CFG(SW_I2C_SDA), cfg_sda);
	if (spksw) {
		*(volatile uint32_t *)(spksw + 4) = spksw_io;
		*(volatile uint32_t *)spksw = spksw_cfg;
	}
	__asm__ volatile("dsb osh" ::: "memory");
	printf("SWEET I2C: %d amplifier(s) answered 0x2032\n", sw_i2c_ok);
}

void reset_case(void) {
    memset(pins,0,sizeof(pins)); pins[0]=pins[1024]=1; pins[1]=pins[1025]=1;
    spksw_pad[0]=0x21; spksw_pad[1]=0;
    rst_a=rst_b=0; boot_nobb=-1; disabled=map_fail=init_fail=bad_id=profile_fail=0;
    scans=drive_low_after_profile=profile_attempts=init_calls=snapshots=0;
    se15_probe_calls=1; sw_i2c_ok=0; scan_success=0;
}
void report(const char *label) {
    printf("CASE {\"label\":\"%s\",\"scans\":%d,\"post_profile_reset_low\":%d,"
        "\"rst_a\":%d,\"rst_b\":%d,\"profile_attempts\":%d,\"init_calls\":%d,"
        "\"snapshots\":%d,\"scl_cfg\":%u,\"sda_cfg\":%u,\"scl_io\":%u,"
        "\"sda_io\":%u,\"spksw_cfg\":%u,\"spksw_io\":%u}\n",label,
        scans,drive_low_after_profile,rst_a,rst_b,profile_attempts,init_calls,
        snapshots,pins[0],pins[1024],pins[1],pins[1025],spksw_pad[0],spksw_pad[1]);
}
int main(void) {
    reset_case(); sweet_i2c_probe_amps(); report("default_late");
    reset_case(); se15_probe_calls=0; sweet_i2c_probe_amps(); report("default_early");
    reset_case(); boot_nobb=1; sweet_i2c_probe_amps(); report("explicit_skip");
    reset_case(); init_fail=1; sweet_i2c_probe_amps(); report("init_failure");
    reset_case(); bad_id=1; sweet_i2c_probe_amps(); report("wrong_chip_id");
    reset_case(); profile_fail=1; sweet_i2c_probe_amps(); report("profile_failure");
    reset_case(); boot_nobb=0; sweet_i2c_probe_amps(); report("diagnostic_no_devices");
    reset_case(); boot_nobb=0; scan_success=1; sweet_i2c_probe_amps(); report("diagnostic_device_found");
    reset_case(); boot_nobb=0; se15_probe_calls=0; sweet_i2c_probe_amps(); report("diagnostic_early");
    reset_case(); disabled=1; sweet_i2c_probe_amps(); report("disabled");
    reset_case(); map_fail=1; sweet_i2c_probe_amps(); report("mapping_failure");
    return 0;
}
