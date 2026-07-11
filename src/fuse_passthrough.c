// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal FUSE server that serves a fixed set of regular files through FUSE
 * passthrough (FOPEN_PASSTHROUGH), used to exercise copy_file_range() between
 * FUSE passthrough files and files on the filesystem backing them.
 *
 * It speaks the /dev/fuse protocol directly so it does not depend on libfuse.
 * It mounts itself on the given mountpoint and serves five fixed nodes, each
 * backed by one of the given backing files.
 * FUSE_COPY_FILE_RANGE is answered with EOPNOTSUPP so the kernel exercises its
 * passthrough and fallback paths rather than a server-side copy.
 *
 * usage: fuse_passthrough MOUNTPOINT SRC DST1 DST2 OVLSRC OVLDST
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

struct node {
	uint64_t ino;
	const char *name;
	int fd;
	int backing_id;
	int read_only;
};

static struct node nodes[] = {
	{ .ino = 2, .name = "src", .read_only = 1 },
	{ .ino = 3, .name = "dst1" },
	{ .ino = 4, .name = "dst2" },
	{ .ino = 5, .name = "ovlsrc", .read_only = 1 },
	{ .ino = 6, .name = "ovldst" },
};

static struct node *find_node(uint64_t ino)
{
	size_t i;

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
		if (nodes[i].ino == ino)
			return &nodes[i];
	return NULL;
}

static struct node *find_name(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
		if (!strcmp(nodes[i].name, name))
			return &nodes[i];
	return NULL;
}

static void fill_attr(struct fuse_attr *attr, struct node *node)
{
	struct stat st;

	memset(attr, 0, sizeof(*attr));
	if (!node) {
		attr->ino = FUSE_ROOT_ID;
		attr->mode = S_IFDIR | 0755;
		attr->nlink = 2;
		attr->blksize = 4096;
		return;
	}

	if (fstat(node->fd, &st) < 0) {
		perror("fstat");
		exit(1);
	}
	attr->ino = node->ino;
	attr->size = st.st_size;
	attr->mode = st.st_mode;
	attr->nlink = st.st_nlink;
	attr->uid = st.st_uid;
	attr->gid = st.st_gid;
}

static int reply_data(int fd, const struct fuse_in_header *in, int error,
		      const void *data, size_t len)
{
	struct fuse_out_header out = {
		.len = sizeof(out) + (error ? 0 : len),
		.error = error ? -error : 0,
		.unique = in->unique,
	};
	struct iovec iov[2] = {
		{ .iov_base = &out, .iov_len = sizeof(out) },
		{ .iov_base = (void *)data, .iov_len = error ? 0 : len },
	};
	ssize_t ret;

	do {
		ret = writev(fd, iov, error || !len ? 1 : 2);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		return -errno;
	return ret == (ssize_t)out.len ? 0 : -EIO;
}

static int reply_attr(int fd, const struct fuse_in_header *in,
		      struct node *node)
{
	struct fuse_attr_out out = {};

	fill_attr(&out.attr, node);
	return reply_data(fd, in, 0, &out, sizeof(out));
}

static int register_backing(int fuse_fd, struct node *node)
{
	struct fuse_backing_map map = {
		.fd = node->fd,
	};
	int id;

	if (node->backing_id > 0)
		return node->backing_id;
	id = ioctl(fuse_fd, FUSE_DEV_IOC_BACKING_OPEN, &map);
	if (id < 0)
		return -errno;
	node->backing_id = id;
	return id;
}

static int handle_request(int fd, void *buf, ssize_t size)
{
	struct fuse_in_header *in = buf;
	struct node *node;
	void *payload;

	if (size < (ssize_t)sizeof(*in) || in->len < sizeof(*in) ||
	    in->len > (uint32_t)size)
		return -EINVAL;
	node = find_node(in->nodeid);
	payload = (char *)buf + sizeof(*in);

	switch (in->opcode) {
	case FUSE_INIT: {
		const struct fuse_init_in *arg = payload;
		struct fuse_init_out out = {
			.major = FUSE_KERNEL_VERSION,
			.minor = arg->minor < FUSE_KERNEL_MINOR_VERSION ?
				 arg->minor : FUSE_KERNEL_MINOR_VERSION,
			.max_readahead = arg->max_readahead,
			.max_write = 1 << 20,
			.time_gran = 1,
			.max_pages = 256,
			.max_stack_depth = 2,
		};
		uint64_t flags = FUSE_INIT_EXT | FUSE_PASSTHROUGH;

		out.flags = flags;
		out.flags2 = flags >> 32;
		return reply_data(fd, in, 0, &out, sizeof(out));
	}
	case FUSE_LOOKUP: {
		struct fuse_entry_out out = {};

		if (in->nodeid != FUSE_ROOT_ID)
			return reply_data(fd, in, ENOENT, NULL, 0);
		node = find_name(payload);
		if (!node)
			return reply_data(fd, in, ENOENT, NULL, 0);
		out.nodeid = node->ino;
		out.generation = 1;
		fill_attr(&out.attr, node);
		return reply_data(fd, in, 0, &out, sizeof(out));
	}
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
		return 0;
	case FUSE_GETATTR:
		if (in->nodeid == FUSE_ROOT_ID)
			return reply_attr(fd, in, NULL);
		return node ? reply_attr(fd, in, node) :
			reply_data(fd, in, ENOENT, NULL, 0);
	case FUSE_OPEN: {
		struct fuse_open_out out = {};
		int id;

		if (!node)
			return reply_data(fd, in, ENOENT, NULL, 0);
		id = register_backing(fd, node);
		if (id < 0)
			return reply_data(fd, in, -id, NULL, 0);
		out.fh = node->ino;
		out.open_flags = FOPEN_PASSTHROUGH;
		out.backing_id = id;
		return reply_data(fd, in, 0, &out, sizeof(out));
	}
	case FUSE_FLUSH:
	case FUSE_RELEASE:
		return reply_data(fd, in, 0, NULL, 0);
	case FUSE_COPY_FILE_RANGE:
#if FUSE_KERNEL_MINOR_VERSION >= 45
	case FUSE_COPY_FILE_RANGE_64:
#endif
		return reply_data(fd, in, EOPNOTSUPP, NULL, 0);
	case FUSE_DESTROY:
		return 1;
	default:
		fprintf(stderr, "unsupported FUSE opcode %u\n", in->opcode);
		return reply_data(fd, in, ENOSYS, NULL, 0);
	}
}

int main(int argc, char **argv)
{
	char options[256];
	char buf[FUSE_MIN_READ_BUFFER];
	int fuse_fd;
	size_t i;

	if (argc != 7) {
		fprintf(stderr,
			"usage: %s MOUNTPOINT SRC DST1 DST2 OVLSRC OVLDST\n",
			argv[0]);
		return 2;
	}
	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		int flags = nodes[i].read_only ? O_RDONLY : O_RDWR;

		nodes[i].fd = open(argv[i + 2], flags | O_CLOEXEC);
		if (nodes[i].fd < 0) {
			perror(argv[i + 2]);
			return 1;
		}
	}

	fuse_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (fuse_fd < 0) {
		perror("/dev/fuse");
		return 1;
	}
	snprintf(options, sizeof(options),
		 "fd=%d,rootmode=40000,user_id=0,group_id=0,default_permissions",
		 fuse_fd);
	if (mount("fuse-passthrough-test", argv[1], "fuse",
		  MS_NOSUID | MS_NODEV, options) < 0) {
		perror("mount fuse");
		return 1;
	}

	for (;;) {
		ssize_t size = read(fuse_fd, buf, sizeof(buf));
		int ret;

		if (size < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ENODEV)
				break;
			perror("read /dev/fuse");
			return 1;
		}
		ret = handle_request(fuse_fd, buf, size);
		if (ret == 1)
			break;
		if (ret < 0) {
			errno = -ret;
			perror("handle FUSE request");
			return 1;
		}
	}
	return 0;
}
