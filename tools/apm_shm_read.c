/*
 * apm_shm_read — read a chunk of another process's address space, even when
 * the VMA is a dma-buf (VM_PFNMAP/VM_MIXEDMAP) where /proc/<pid>/mem returns
 * EIO because __access_remote_vm() cannot get a page reference.
 *
 * Tries, in order:
 *   1. pread() on /proc/<pid>/mem              (normal anon / file mappings)
 *   2. PTRACE_ATTACH + PTRACE_PEEKDATA         (works for pfn-mapped dma-buf)
 *
 * Mode 2 *stops* the target process for the duration of the read (it is
 * SIGSTOP'd and detached afterwards).  On a vendor HAL that is a sub-second
 * stall; nothing is written to the target.
 *
 * usage:
 *   apm_shm_read <pid> <hex-addr> <bytes> <out-file>
 *   apm_shm_read --dmabuf <pid> <out-file>   # first rw-s /dmabuf: mapping
 *
 * build (Android NDK, static so it runs from /data/local/tmp):
 *   $NDK/toolchains/llvm/prebuilt/darwin-x86/bin/clang --target=aarch64-linux-android29 \
 *       -static -O2 -o /tmp/apm_shm_read tools/apm_shm_read.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>

#define PAGE 4096

static int read_memfile(long pid, uintptr_t addr, size_t len, FILE *out)
{
	char path[64];
	uint8_t *buf;
	int fd;
	ssize_t got;

	snprintf(path, sizeof(path), "/proc/%ld/mem", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	buf = malloc(len);
	if (!buf) {
		close(fd);
		return -1;
	}
	got = pread(fd, buf, len, (off_t)addr);
	close(fd);
	if (got != (ssize_t)len) {
		fprintf(stderr, "pread @0x%lx len=%zu: %zd (%s)\n",
			(unsigned long)addr, len, got, strerror(errno));
		free(buf);
		return -1;
	}
	fwrite(buf, len, 1, out);
	free(buf);
	return 0;
}

static int read_ptrace(long pid, uintptr_t addr, size_t len, FILE *out)
{
	uintptr_t a = addr & ~(uintptr_t)(PAGE - 1);	/* start page-aligned */
	size_t head = addr - a;
	size_t total = head + len;
	uint8_t *buf;
	int rc = -1;

	buf = calloc(1, total);
	if (!buf) {
		fprintf(stderr, "oom (%zu)\n", total);
		return -1;
	}

	if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
		fprintf(stderr, "PTRACE_ATTACH %ld: %s\n", pid, strerror(errno));
		free(buf);
		return -1;
	}
	{
		int st = 0;
		if (waitpid(pid, &st, __WALL) < 0)
			fprintf(stderr, "waitpid: %s (continuing anyway)\n",
				strerror(errno));
	}

	for (size_t off = 0; off < total; off += 8) {
		size_t n = total - off;
		if (n > 8)
			n = 8;
		errno = 0;
		long w = ptrace(PTRACE_PEEKDATA, pid, (void *)(a + off), NULL);
		if (w < 0 && errno != 0) {
			fprintf(stderr, "PEEKDATA @0x%lx+%zu: %s\n",
				(unsigned long)a, off, strerror(errno));
			rc = -1;
			goto out_detach;
		}
		memcpy(buf + off, &w, n);
	}
	fwrite(buf + head, len, 1, out);
	fprintf(stderr, "ptrace read: %zu bytes\n", len);
	rc = 0;

out_detach:
	ptrace(PTRACE_DETACH, pid, NULL, NULL);
	free(buf);
	return rc;
}

/* return the first rw-s /dmabuf: mapping of pid */
static int find_dmabuf(long pid, uintptr_t *addr, size_t *len)
{
	char path[64], line[512];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%ld/maps", pid);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned long s, e;
		char perms[8];
		if (sscanf(line, "%lx-%lx %4s", &s, &e, perms) != 3)
			continue;
		if (perms[0] == 'r' && perms[1] == 'w' && perms[3] == 's' &&
		    strstr(line, "/dmabuf:")) {
			*addr = (uintptr_t)s;
			*len = (size_t)(e - s);
			fclose(f);
			return 0;
		}
	}
	fclose(f);
	return -1;
}

int main(int argc, char **argv)
{
	long pid;
	uintptr_t addr;
	size_t len;
	const char *out;
	FILE *o;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <pid> <hex-addr> <bytes> <out>\n", argv[0]);
		fprintf(stderr, "       %s --dmabuf <pid> <out>\n", argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "--dmabuf")) {
		if (argc < 4) {
			fprintf(stderr, "usage: %s --dmabuf <pid> <out>\n", argv[0]);
			return 2;
		}
		pid = atol(argv[2]);
		out = argv[3];
		if (find_dmabuf(pid, &addr, &len) < 0) {
			fprintf(stderr, "no rw-s /dmabuf: mapping in %ld\n", pid);
			return 1;
		}
		fprintf(stderr, "dmabuf map: pid=%ld addr=0x%lx len=0x%zx\n",
			pid, (unsigned long)addr, len);
	} else {
		if (argc < 5) {
			fprintf(stderr, "usage: %s <pid> <hex-addr> <bytes> <out>\n", argv[0]);
			return 2;
		}
		pid = atol(argv[1]);
		addr = (uintptr_t)strtoull(argv[2], NULL, 16);
		out = argv[4];
		len = (size_t)strtoul(argv[3], NULL, 0);
	}

	o = fopen(out, "wb");
	if (!o) {
		fprintf(stderr, "open %s: %s\n", out, strerror(errno));
		return 1;
	}
	if (read_memfile(pid, addr, len, o) == 0) {
		fprintf(stderr, "read via /proc/pid/mem\n");
	} else {
		fprintf(stderr, "/proc/pid/mem failed, falling back to ptrace\n");
		fseek(o, 0, SEEK_SET);
		ftruncate(fileno(o), 0);
		if (read_ptrace(pid, addr, len, o) < 0) {
			fclose(o);
			return 1;
		}
	}
	fclose(o);
	return 0;
}
