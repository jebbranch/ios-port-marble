/*
 * Boot splash: raw BGRA pixels, 1080x2400, generated from splash.png by
 * build_xnu.sh.  Lives in .rodata so it is PC-relative like the rest of
 * the shim (ABL loads us at a fixed, non-zero address).
 */
	/* Its own section, placed after the stacks: a 10 MiB blob in .rodata
	 * would push _stack_top/_bss_* out of start.o's ADR +/-1 MiB range. */
	.section .splash,"a"
	.balign 16
	.global splash_img
splash_img:
	.incbin "splash.bgra"
	.global splash_img_end
splash_img_end:
