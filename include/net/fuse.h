/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#ifndef _NET_FUSE_H
#define _NET_FUSE_H

#include <linux/fuse.h>
#include <linux/poll.h>
#include <linux/kref.h>

struct fuse_conn;
struct fuse_mount;
struct fuse_inode;
struct fuse_file;
struct fuse_dev;

/** One input argument of a request */
struct fuse_in_arg {
	unsigned size;
	const void *value;
};

/** One output argument of a request */
struct fuse_arg {
	unsigned size;
	void *value;
};

/** FUSE page descriptor */
struct fuse_page_desc {
	unsigned int length;
	unsigned int offset;
};

struct fuse_args {
	uint64_t nodeid;
	uint32_t opcode;
	unsigned short in_numargs;
	unsigned short out_numargs;
	bool force:1;
	bool noreply:1;
	bool nocreds:1;
	bool in_pages:1;
	bool out_pages:1;
	bool out_argvar:1;
	bool page_zeroing:1;
	bool page_replace:1;
	bool may_block:1;
	struct fuse_in_arg in_args[3];
	struct fuse_arg out_args[2];
	void (*end)(struct fuse_mount *fm, struct fuse_args *args, int error);
};

struct fuse_args_pages {
	struct fuse_args args;
	struct page **pages;
	struct fuse_page_desc *descs;
	unsigned int num_pages;
};

#define FUSE_ARGS(args) struct fuse_args args = {}

/** The request IO state (for asynchronous processing) */
struct fuse_io_priv {
	struct kref refcnt;
	int async;
	spinlock_t lock;
	unsigned reqs;
	ssize_t bytes;
	size_t size;
	__u64 offset;
	bool write;
	bool should_dirty;
	int err;
	struct kiocb *iocb;
	struct completion *done;
	bool blocking;
};

#define FUSE_IO_PRIV_SYNC(i) \
{					\
	.refcnt = KREF_INIT(1),		\
	.async = 0,			\
	.iocb = i,			\
}

/** /dev/fuse input queue operations */
extern const struct fuse_iqueue_ops fuse_dev_fiq_ops;

/** Device operations */
extern const struct file_operations fuse_dev_operations;

void fuse_sync_release(struct fuse_inode *fi, struct fuse_file *ff, int flags);

int fuse_simple_background(struct fuse_mount *fm, struct fuse_args *args,
			   gfp_t gfp_flags);

/* Abort all requests */
void fuse_abort_conn(struct fuse_conn *fc);

/**
 * Acquire reference to fuse_conn
 */
struct fuse_conn *fuse_conn_get(struct fuse_conn *fc);

/**
 * Initialize fuse_conn
 */
struct fuse_mount *fuse_conn_new(struct user_namespace *user_ns,
				 const struct fuse_iqueue_ops *fiq_ops,
				 void *fiq_priv, void *private,
				 void (*release)(struct fuse_conn *));

/**
 * Release reference to fuse_conn
 */
void fuse_conn_put(struct fuse_conn *fc);

void *fuse_conn_private(struct fuse_conn *fc);

void fuse_conn_update_param(struct fuse_conn *fc, unsigned int max_read,
			    unsigned int max_write, unsigned int minor);


int fuse_conn_waiting(struct fuse_conn *fc);

void fuse_set_initialized(struct fuse_conn *fc);

struct fuse_conn *fuse_mount_conn(struct fuse_mount *fm);

struct fuse_mount *fuse_file_mount(struct fuse_file *ff);

struct fuse_dev *fuse_dev_alloc_install(struct fuse_conn *fc);
void fuse_dev_free(struct fuse_dev *fud);

/*
 * Remove the mount from the connection
 *
 * Returns whether this was the last mount
 */
bool fuse_mount_remove(struct fuse_mount *fm);

int fuse_do_open(struct fuse_mount *fm, u64 nodeid, struct file *file,
		 bool isdir);

/**
 * fuse_direct_io() flags
 */

/** If set, it is WRITE; otherwise - READ */
#define FUSE_DIO_WRITE (1 << 0)

/** CUSE pass fuse_direct_io() a file which f_mapping->host is not from FUSE */
#define FUSE_DIO_CUSE  (1 << 1)

ssize_t fuse_direct_io(struct fuse_io_priv *io, struct iov_iter *iter,
		       loff_t *ppos, int flags);
long fuse_do_ioctl(struct file *file, unsigned int cmd, unsigned long arg,
		   unsigned int flags);
__poll_t fuse_file_poll(struct file *file, poll_table *wait);

#endif /* _NET_FUSE_H */
