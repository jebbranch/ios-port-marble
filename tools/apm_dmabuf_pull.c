/*
 * apm_dmabuf_pull — read a dma-buf that belongs to ANOTHER process.
 *
 * Why this exists: AGM's shared-memory region is mapped VM_PFNMAP, so
 *   pread(/proc/<pid>/mem)   -> EIO
 *   PTRACE_PEEKDATA          -> EIO
 *   open("/proc/<pid>/fd/N") -> ENXIO (anon inode cannot be reopened)
 * all fail, while those very pages are perfectly readable through a mapping the
 * *reading* process owns.  pidfd_open() + pidfd_getfd() give us a real dma-buf
 * fd for the same buffer, and our own mmap() of it is CPU-readable.
 *
 * usage:  apm_dmabuf_pull <pid> <out-prefix>
 *   auto-selects every "rw-s ... /dmabuf:" mapping of <pid>.
 *
 * build:
 *   clang --target=aarch64-linux-android29 -static -O2 -o /tmp/apm_dmabuf_pull \
 *       tools/apm_dmabuf_pull.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 425
#endif

struct map {
	uintptr_t start;
	size_t len;
	unsigned long ino;
};

static int scan_maps(long pid, struct map *out, int max)
{
	char path[64], line[512];
	FILE *f;
	int n = 0;

	snprintf(path, sizeof(path), "/proc/%ld/maps", pid);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f) && n < max) {
		unsigned long s = 0, e = 0, ino = 0;
		char perms[8] = {0}, file[256] = {0};
		if (sscanf(line, "%lx-%lx %7s %*s %*s %lu %255[^\n]",
			   &s, &e, perms, &ino, file) < 5)
			continue;
		if (perms[0] == 'r' && perms[1] == 'w' && perms[3] == 's' &&
		    strstr(file, "/dmabuf:")) {
			out[n].start = (uintptr_t)s;
			out[n].len = (size_t)(e - s);
			out[n].ino = ino;
			n++;
		}
	}
	fclose(f);
	return n;
}

/* which fd of <pid> has fdinfo ino == `ino`? */
static int fd_for_ino(long pid, unsigned long ino)
{
	char dir[64], path[128], line[256];
	DIR *d;
	struct dirent *de;

	snprintf(dir, sizeof(dir), "/proc/%ld/fd", pid);
	d = opendir(dir);
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		FILE *f;
		unsigned long v;
		int fd = atoi(de->d_name);

		if (fd <= 0)
			continue;
		snprintf(path, sizeof(path), "/proc/%ld/fdinfo/%d", pid, fd);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "ino: %lu", &v) == 1 && v == ino) {
				fclose(f);
				closedir(d);
				return fd;
			}
		}
		fclose(f);
	}
	closedir(d);
	return -1;
}

int main(int argc, char **argv)
{
	long pid;
	const char *prefix;
	struct map maps[8];
	int n, i, rc = 1;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <pid> <out-prefix>\n", argv[0]);
		return 2;
	}
	pid = atol(argv[1]);
	prefix = argv[2];

	n = scan_maps(pid, maps, 8);
	if (n <= 0) {
		fprintf(stderr, "no rw-s /dmabuf: mapping in pid %ld\n", pid);
		return 1;
	}
	for (i = 0; i < n; i++) {
		char out[256];
		int pidfd, tfd, fd2, m, w, fd;
		void *p;
		FILE *o;

		tfd = fd_for_ino(pid, maps[i].ino);
		printf("map[%d] %#lx+%zu ino=%lu target_fd=%d\n", i,
		       (unsigned long)maps[i].start, maps[i].len, maps[i].ino, tfd);
		if (tfd < 0)
			continue;
		pidfd = (int)syscall(SYS_pidfd_open, (pid_t)pid, 0);
		if (pidfd < 0) {
			fprintf(stderr, "  pidfd_open(%ld): %s\n", pid, strerror(errno));
			continue;
		}
		fd2 = (int)syscall(SYS_pidfd_getfd, pidfd, tfd, 0);
		close(pidfd);
		if (fd2 < 0) {
			fprintf(stderr, "  pidfd_getfd(fd=%d): %s\n", tfd, strerror(errno));
			continue;
		}
		m = (int)maps[i].len;
		/* dma_buf mmap needs the buffer length; use the mapping length we saw */
		p = mmap(NULL, (size_t)m, PROT_READ, MAP_SHARED, fd2, 0);
		if (p == MAP_FAILED) {
			fprintf(stderr, "  mmap(%d): %s\n", m, strerror(errno));
			close(fd2);
			continue;
		}
		snprintf(out, sizeof(out), "%s-%d-%lx-%x.bin", prefix, tfd,
			 (unsigned long)maps[i].start, (unsigned)m);
		fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			fprintf(stderr, "  create %s: %s\n", out, strerror(errno));
			munmap(p, (size_t)m);
			close(fd2);
			continue;
		}
		w = (int)write(fd, p, (size_t)m);
		close(fd);
		printf("  -> %s wrote=%d%s\n", out, w, w == m ? " (complete)" : " (SHORT)");
		munmap(p, (size_t)m);
		close(fd2);
		rc = 0;
	}
	return rc;
}
