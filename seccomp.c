#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>

#include "seccomp.h"

#define MAX_BPF_SIZE 32*1024

void die(const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	vfprintf(stderr, msg, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

FILE* sc_must_read_and_validate_header_from_file(const char *profile_path, struct sc_seccomp_file_header *hdr)
{
	FILE *file = fopen(profile_path, "rb");
	if (file == NULL) {
		die("cannot open seccomp filter %s", profile_path);
	}
	size_t num_read = fread(hdr, 1, sizeof(struct sc_seccomp_file_header), file);
	if (ferror(file) != 0) {
		fclose(file);
		die("cannot read seccomp profile %s", profile_path);
	}
	if (num_read < sizeof(struct sc_seccomp_file_header)) {
		fclose(file);
		die("short read on seccomp header: %zu", num_read);
	}
	// check everything
	if (hdr->header[0] != 'S' || hdr->header[1] != 'C') {
		fclose(file);
		die("unexpected seccomp header: %x%x", hdr->header[0], hdr->header[1]);
	}
	if (hdr->unrestricted != 0 && hdr->unrestricted != 1) {
		fclose(file);
		die("unsupported seccomp unrestricted: %u", hdr->unrestricted);
	}
	if (hdr->len_filter > MAX_BPF_SIZE) {
		fclose(file);
		die("allow filter size too big %u", hdr->len_filter);
	}
	if (hdr->len_filter == 0) {
		fclose(file);
		die("allow filter size is 0");
	}
	return file;
}

void sc_must_read_filter_from_file(FILE *file, uint32_t len_bytes, struct sock_fprog *prog)
{
	// Check if the bytes are divisible by sizeof(struct sock_filter)
	if (len_bytes % sizeof(struct sock_filter) != 0) {
		fclose(file);
		die("allow filter size is not divisible by sizeof(struct sock_filter)");
	}

	prog->len = len_bytes / sizeof(struct sock_filter);
	prog->filter = (struct sock_filter *)malloc(len_bytes);
	if (prog->filter == NULL) {
		die("cannot allocate %u bytes of memory for seccomp filter ", len_bytes);
	}
	size_t num_read = fread(prog->filter, 1, len_bytes, file);
	if (ferror(file)) {
		free(prog->filter);
		fclose(file);
		die("cannot read filter");
	}
	if (num_read != len_bytes) {
		free(prog->filter);
		fclose(file);
		die("short read for filter %zu != %i", num_read, len_bytes);
	}
}

int seccomp(unsigned int operation, unsigned int flags, void *args) {
	errno = 0;
	return syscall(__NR_seccomp, operation, flags, args);
}

void sc_apply_seccomp_filter(struct sock_fprog *prog) {
	int err = seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_LOG, prog);
	if (err != 0) {
		die("cannot apply seccomp profile");
	}
}