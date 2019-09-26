/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2019  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include <linux/fuse.h>

#include <linux/fs.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/refcount.h>
#include <linux/mm_types.h>

/*
 * Request flags
 *
 * FR_ISREPLY:		set if the request has reply
 * FR_FORCE:		force sending of the request
 * FR_PENDING:		request is not yet in userspace
 * FR_SENT:		request sent to userspace
 * FR_FINISHED:		request is finished
 * FR_ZEROTAIL:		zero tail of returned data
 */
enum fuse_req_flag {
	FR_ISREPLY,
	FR_FORCE,
	FR_PENDING,
	FR_SENT,
	FR_FINISHED,
	FR_ZEROTAIL,
	FR_KILLABLE,
};

/* FUSE page descriptor */
struct fuse_page_desc {
	unsigned int length;
	unsigned int offset;
};

/* Number of page pointers embedded in fuse_req */
#define FUSE_REQ_INLINE_PAGES 1

#define FUSE_REQ_INLINE_DATA 196

struct fuse_req {
	/* This can be on either pending or processing lists */
	struct list_head list;

	/* refcount */
	refcount_t count;

	/* Request flags, updated with test/set/clear_bit() */
	unsigned long flags;

	union {
		/* The request header */
		struct fuse_in_header inh;

		/* The reply header */
		struct fuse_out_header outh;

		/* Inline data */
		char inlinedata[FUSE_REQ_INLINE_DATA];
	};

	/* length of inline in data */
	unsigned short inline_inlen;

	/* length of inline out data */
	unsigned short inline_outlen;

	/* mandatory out len */
	unsigned int mand_outlen;

	/* max out len */
	unsigned int max_outlen;

	/* size of the 'pages' array */
	unsigned short max_pages;

	/* number of pages in vector */
	unsigned short num_pages;

	/* Used to wake up the task waiting for completion of request*/
	wait_queue_head_t waitq;

	/* page vector */
	struct page **pages;

	/* page-descriptor vector */
	struct fuse_page_desc *page_descs;

	/* inline page vector */
	struct page *inline_pages[FUSE_REQ_INLINE_PAGES];

	/* inline page-descriptor vector */
	struct fuse_page_desc inline_page_descs[FUSE_REQ_INLINE_PAGES];
};

/* One forget request */
struct fuse_forget {
	struct fuse_forget_one forget_one;
	struct list_head list;
};

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

struct fuse_args {
	bool force:1;
	bool killable:1;
	struct {
		struct {
			uint32_t opcode;
			uint64_t nodeid;
		} h;
		unsigned numargs;
		struct fuse_in_arg args[3];

	} in;
	struct {
		unsigned argvar:1;
		unsigned numargs;
		struct fuse_arg args[2];
	} out;
};

struct fuse_dev_operations {
	void (*put)(void *dev);
	void (*abort)(void *dev);
	int (*send)(void *dev, struct fuse_req *req);
	ssize_t (*simple_send)(void *dev, struct fuse_args *args, uid_t uid,
			       gid_t gid, pid_t pid);
	void (*forget)(void *dev, struct fuse_forget *forget);
};

struct dentry *fuse_mount_common(struct file_system_type *fs_type,
				 int flags, void *opts,
				 const struct fuse_dev_operations *dev_ops,
				 void *dev_priv);
void fuse_kill_sb(struct super_block *sb);
void fuse2_get_request(struct fuse_req *req);
void fuse2_put_request(struct fuse_req *req);
int fuse2_map_open(struct super_block *sb, struct file *file);
int fuse2_map_close(struct super_block *sb, unsigned long mapfd);
