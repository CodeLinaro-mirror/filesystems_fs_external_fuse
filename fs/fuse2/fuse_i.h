/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#ifndef _FS_FUSE_I_H
#define _FS_FUSE_I_H

#include "dev.h"

/** Default max number of pages that can be used in a single read request */
#define FUSE_DEFAULT_MAX_PAGES_PER_REQ 32

/** Maximum of max_pages received in init_out */
#define FUSE_MAX_MAX_PAGES 256

/** Bias for fi->writectr, meaning new writepages must not be sent */
#define FUSE_NOWRITE INT_MIN

/** It could be as large as PATH_MAX, but would that have any uses? */
#define FUSE_NAME_MAX 1024

/** Number of dentries for each connection in the control filesystem */
#define FUSE_CTL_NUM_DENTRIES 5

/** FUSE inode */
struct fuse_inode {
	/** Inode data */
	struct inode inode;

	/** Unique ID, which identifies the inode between userspace
	 * and kernel */
	u64 nodeid;

	/** Number of lookups on this inode */
	u64 nlookup;

	/** The request used for sending the FORGET message */
	struct fuse_forget *forget;

	/** Miscellaneous bits describing inode state */
	unsigned long state;

	/** Lock to protect write related fields */
	spinlock_t lock;
};

struct fuse_conn;

/** FUSE specific file data */
struct fuse_file {
	/** Fuse connection for this file */
	struct fuse_conn *fc;

	/** Kernel file handle guaranteed to be unique */
	u64 kh;

	/** File handle used by userspace */
	u64 fh;

	/** Node id of this file */
	u64 nodeid;

	/** FOPEN_* flags returned by open */
	u32 open_flags;

	/** Has flock been performed on this file? */
	bool flock:1;
};

#define FUSE_ARGS(args) struct fuse_args args = {}

/**
 * A Fuse connection.
 *
 * This structure is created, when the filesystem is mounted, and is
 * destroyed, when the client device is closed and the filesystem is
 * unmounted.
 */
struct fuse_conn {
	/** Refcount */
	refcount_t count;

	struct rcu_head rcu;

	/** The pid namespace for this mount */
	struct pid_namespace *pid_ns;

	/** The user namespace for this mount */
	struct user_namespace *user_ns;

	/** Maxmum number of pages that can be used in a single request */
	unsigned int max_pages;

	/** The next unique kernel file handle */
	atomic64_t khctr;

	/*
	 * The following bitfields are only for optimization purposes
	 * and hence races in setting them will not cause malfunction
	 */

	/** Is fsync not implemented by fs? */
	unsigned no_fsync:1;

	/** Is fsyncdir not implemented by fs? */
	unsigned no_fsyncdir:1;

	/** Is flush not implemented by fs? */
	unsigned no_flush:1;

	/** Is setxattr not implemented by fs? */
	unsigned no_setxattr:1;

	/** Is getxattr not implemented by fs? */
	unsigned no_getxattr:1;

	/** Is listxattr not implemented by fs? */
	unsigned no_listxattr:1;

	/** Is removexattr not implemented by fs? */
	unsigned no_removexattr:1;

	/** Are posix file locking primitives not implemented by fs? */
	unsigned no_lock:1;

	/** Is create not implemented by fs? */
	unsigned no_create:1;

	/** Are BSD file locking primitives not implemented by fs? */
	unsigned no_flock:1;

	/** Is fallocate not implemented by fs? */
	unsigned no_fallocate:1;

	/** Is rename with flags implemented by fs? */
	unsigned no_rename2:1;

	/** Is lseek not implemented by fs? */
	unsigned no_lseek:1;

	/** Does the filesystem support posix acls? */
	unsigned posix_acl:1;

	/** Negotiated minor version */
	unsigned minor;

	/** Key for lock owner ID scrambling */
	u32 scramble_key[4];

	/* fuse device operations */
	const struct fuse_dev_operations *dev_ops;

	/* device data */
	void *dev_priv;

	/* Map file */
	struct file *mapfile;
};

static inline struct fuse_conn *get_fuse_conn_super(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct fuse_conn *get_fuse_conn(struct inode *inode)
{
	return get_fuse_conn_super(inode->i_sb);
}

static inline struct fuse_inode *get_fuse_inode(struct inode *inode)
{
	return container_of(inode, struct fuse_inode, inode);
}

static inline u64 get_node_id(struct inode *inode)
{
	return get_fuse_inode(inode)->nodeid;
}

static inline int invalid_nodeid(u64 nodeid)
{
	return !nodeid || nodeid == FUSE_ROOT_ID;
}

extern const struct dentry_operations fuse2_dentry_operations;

/**
 * Inode to nodeid comparison.
 */
int fuse2_inode_eq(struct inode *inode, void *_nodeidp);

/**
 * Get a filled in inode
 */
struct inode *fuse2_iget(struct super_block *sb, u64 nodeid,
			int generation, struct fuse_attr *attr);

/**
 * Initialize READ or READDIR request
 */
void fuse2_read_fill(struct fuse_req *req, struct fuse_read_in **inarg,
		    struct file *file, loff_t pos, size_t count, int opcode);

/**
 * Send OPEN or OPENDIR request
 */
int fuse2_open_common(struct inode *inode, struct file *file, bool isdir);

struct fuse_file *fuse2_file_alloc(struct fuse_conn *fc);
void fuse2_file_free(struct fuse_file *ff);
void fuse2_finish_open(struct inode *inode, struct file *file);

/**
 * Send RELEASE or RELEASEDIR request
 */
void fuse2_release_common(struct fuse_conn *fc, struct fuse_file *ff,
			 int flags, fl_owner_t id, bool isdir);

/**
 * Send FSYNC or FSYNCDIR request
 */
int fuse2_fsync_common(struct file *file, loff_t start, loff_t end,
		      int datasync, int opcode);

/**
 * Notify poll wakeup
 */
int fuse_notify_poll_wakeup(struct fuse_conn *fc,
			    struct fuse_notify_poll_wakeup_out *outarg);

/**
 * Initialize file operations on a regular file
 */
void fuse2_init_file_inode(struct inode *inode);

/**
 * Initialize inode operations on regular files and special files
 */
void fuse2_init_common(struct inode *inode);

/**
 * Initialize inode and file operations on a directory
 */
void fuse2_init_dir(struct inode *inode);

/**
 * Initialize inode operations on a symlink
 */
void fuse2_init_symlink(struct inode *inode);

/**
 * Change attributes of an inode
 */
void fuse2_change_attributes(struct inode *inode, struct fuse_attr *attr);

void fuse2_change_attributes_common(struct inode *inode, struct fuse_attr *attr);

int  fuse_req_cache_init(void);
void fuse_req_cache_cleanup(void);

/**
 * Get a request, may fail with -ENOMEM,
 * caller should specify # elements in req->pages[] explicitly
 */
struct fuse_req *fuse2_get_req(struct fuse_conn *fc, unsigned npages);

struct fuse_forget *fuse2_alloc_forget(void);

/*
 * Send a request (synchronous)
 */
int fuse2_request_send(struct fuse_conn *fc, struct fuse_req *req);

/**
 * Simple request sending that does request allocation and freeing
 */
ssize_t fuse2_simple_request(struct fuse_conn *fc, struct fuse_args *args);

/*
 * Send FORGET command
 */
void fuse2_queue_forget(struct fuse_conn *fc, struct fuse_forget *forget,
		       u64 nodeid, u64 nlookup);

void fuse2_force_forget(struct fuse_conn *fc, u64 nodeid);

/**
 * Is current process allowed to perform filesystem operation?
 */
int fuse2_allow_current_process(struct fuse_conn *fc);

u64 fuse2_lock_owner_id(struct fuse_conn *fc, fl_owner_t id);

int fuse2_update_attributes(struct inode *inode, struct file *file);

int fuse2_setxattr(struct inode *inode, const char *name, const void *value,
		  size_t size, int flags);
ssize_t fuse2_getxattr(struct inode *inode, const char *name, void *value,
		      size_t size);
ssize_t fuse2_listxattr(struct dentry *entry, char *list, size_t size);
int fuse2_removexattr(struct inode *inode, const char *name);
extern const struct xattr_handler *fuse2_xattr_handlers[];
extern const struct xattr_handler *fuse2_acl_xattr_handlers[];
extern const struct xattr_handler *fuse2_no_acl_xattr_handlers[];

struct posix_acl;
struct posix_acl *fuse2_get_acl(struct inode *inode, int type);
int fuse2_set_acl(struct inode *inode, struct posix_acl *acl, int type);


/* readdir.c */
int fuse2_readdir(struct file *file, struct dir_context *ctx);

/* map.c */
struct file *fuse2_map_get(struct fuse_conn *fc, u64 mapfd);

#endif /* _FS_FUSE_I_H */
