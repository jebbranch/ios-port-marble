/*
 * uprobe_selftest — positive control for the tools/apm_blob_dev.sh uprobes.
 *
 * Why: "no uprobe hits" only means "the function was not called" if uprobe
 * events work at all on this vendor kernel.  This binary has a function whose
 * address we know, we put a uprobe on it, call it, and check the event lands.
 *
 * build (static + no-pie so the symbol value is the file offset):
 *   clang --target=aarch64-linux-android29 -static -no-pie -O0 -o /tmp/uprobe_selftest tools/uprobe_selftest.c
 */
#include <stdio.h>
#include <unistd.h>

__attribute__((noinline)) int probe_me(int n)
{
	usleep(1000);
	return n * 3 + 1;
}

int main(void)
{
	int i, acc = 0;
	for (i = 0; i < 5; i++)
		acc += probe_me(i);
	printf("UPROBE SELFTEST acc=%d\n", acc);
	return 0;
}
