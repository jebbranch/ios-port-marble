/* GMU firmware blob, embedded for the shim's GMU bring-up. */
	.section .rodata
	.balign 4096
	.global gmu_fw_start
	.global gmu_fw_end
gmu_fw_start:
	.incbin "gmu_gen70000.bin"
gmu_fw_end:
	.balign 16
