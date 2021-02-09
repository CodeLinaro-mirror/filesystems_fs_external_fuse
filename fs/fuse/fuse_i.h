/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#ifndef _FS_FUSE_I_H
#define _FS_FUSE_I_H

#ifndef pr_fmt
# define pr_fmt(fmt) "fuse: " fmt
#endif

#include <net/fuse.h>

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/backing-dev.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/rbtree.h>
#include <linux/workqueue.h>
#include <linux/xattr.h>
#include <linux/pid_namespace.h>
#include <linux/refcount.h>
#include <linux/user_namespace.h>

/** Default max number of pages that can be used in a single read request */
#define FUSE_DEFAULT_MAX_PAGES_PER_REQ 32

/** Maximum of max_pages received in init_out */
#define FUSE_MAX_MAX_PAGES 256

/** Bias for fi->writectr, meaning new writepages must not be sent */
#define FUSE_NOWRITE INT_MIN

/** It could be as large as PATH_MAX, but would that have any uses? */
#define FUSE_NAME_MAX 1024

/** List of active connections */
extern struct list_head fuse_conn_list;

/** Global mutex protecting fuse_conn_list and the control filesystem */
extern struct mutex fuse_mutex;

/** Module parameters */
extern unsigned max_user_bgreq;
extern unsigned max_user_congthresh;

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
	struct fuse_forget_link *forget;

	/** Time in jiffies until the file attributes are valid */
	u64 i_time;

	/* Which attributes are invalid */
	u32 inval_mask;

	/** The sticky bit in inode->i_mode may have been removed, so
	    preserve the original mode */
	umode_t orig_i_mode;

	/** 64 bit inode number */
	u64 orig_ino;

	/** Version of last attribute change */
	u64 attr_version;

	union {
		/* Write related fields (regular file only) */
		struct {
			/* Files usable in writepage.  Protected by fi->lock */
			struct list_head write_files;

			/* Writepages pending on truncate or fsync */
			struct list_head queued_writes;

			/* Number of sent writes, a negative bias
			 * (FUSE_NOWRITE) means more writes are blocked */
			int writectr;

			/* Waitq for writepage completion */
			wait_queue_head_t page_waitq;

			/* List of writepage requestst (pending or sent) */
			struct rb_root writepages;
		};

		/* readdir cache (directory only) */
		struct {
			/* true if fully cached */
			bool cached;

			/* size of cache */
			loff_t size;

			/* position at end of cache (position of next entry) */
			loff_t pos;

			/* version of the cache */
			u64 version;

			/* modification time of directory when cache was
			 * started */
			struct timespec64 mtime;

			/* iversion of directory when cache was started */
			u64 iversion;

			/* protects above fields */
			spinlock_t lock;
		} rdc;
	};

	/** Miscellaneous bits describing inode state */
	unsigned long state;

	/** Lock for serializing lookup and readdir for back compatibility*/
	struct mutex mutex;

	/** Lock to protect write related fields */
	spinlock_t lock;

	/**
	 * Can't take inode lock in fault path (leads to circular dependency).
	 * Introduce another semaphore which can be taken in fault path and
	 * then other filesystem paths can take this to block faults.
	 */
	struct rw_semaphore i_mmap_sem;

#ifdef CONFIG_FUSE_DAX
	/*
	 * Dax specific inode data
	 */
	struct fuse_inode_dax *dax;
#endif
};

/** FUSE inode state bits */
enum {
	/** Advise readdirplus  */
	FUSE_I_ADVISE_RDPLUS,
	/** Initialized with readdirplus */
	FUSE_I_INIT_RDPLUS,
	/** An operation changing file size is in progress  */
	FUSE_I_SIZE_UNSTABLE,
	/* Bad inode */
	FUSE_I_BAD,
};

/**
 * Request flags
 *
 * FR_ISREPLY:		set if the request has reply
 * FR_FORCE:		force sending of the request even if interrupted
 * FR_BACKGROUND:	request is sent in the background
 * FR_WAITING:		request is counted as "waiting"
 * FR_ABORTED:		the request was aborted
 * FR_INTERRUPTED:	the request has been interrupted
 * FR_LOCKED:		data is being copied to/from the request
 * FR_PENDING:		request is not yet in userspace
 * FR_SENT:		request is in userspace, waiting for an answer
 * FR_FINISHED:		request is finished
 * FR_PRIVATE:		request is on private list
 * FR_ASYNC:		request is asynchronous
 */
enum fuse_req_flag {
	FR_ISREPLY,
	FR_FORCE,
	FR_BACKGROUND,
	FR_WAITING,
	FR_ABORTED,
	FR_INTERRUPTED,
	FR_LOCKED,
	FR_PENDING,
	FR_SENT,
	FR_FINISHED,
	FR_PRIVATE,
	FR_ASYNC,
};

/**
 * A request to the client
 *
 * .waitq.lock protects the following fields:
 *   - FR_ABORTED
 *   - FR_LOCKED (may also be modified under fc->lock, tested under both)
 */
struct fuse_req {
	/** This can be on either pending processing or io lists in
	    fuse_conn */
	struct list_head list;

	/** Entry on the interrupts list  */
	struct list_head intr_entry;

	/* Input/output arguments */
	struct fuse_args *args;

	/** refcount */
	refcount_t count;

	/* Request flags, updated with test/set/clear_bit() */
	unsigned long flags;

	/* The request input header */
	struct {
		struct fuse_in_header h;
	} in;

	/* The request output header */
	struct {
		struct fuse_out_header h;
	} out;

	/** Used to wake up the task waiting for completion of request*/
	wait_queue_head_t waitq;

#if IS_ENABLED(CONFIG_VIRTIO_FS)
	/** virtio-fs's physically contiguous buffer for in and out args */
	void *argbuf;
#endif

	/** fuse_mount this request belongs to */
	struct fuse_mount *fm;
};

struct fuse_iqueue;

/**
 * Input queue callbacks
 *
 * Input queue signalling is device-specific.  For example, the /dev/fuse file
 * uses fiq->waitq and fasync to wake processes that are waiting on queue
 * readiness.  These callbacks allow other device types to respond to input
 * queue activity.
 */
struct fuse_iqueue_ops {
	/**
	 * Signal that a forget has been queued
	 */
	void (*wake_forget_and_unlock)(struct fuse_iqueue *fiq)
		__releases(fiq->lock);

	/**
	 * Signal that an INTERRUPT request has been queued
	 */
	void (*wake_interrupt_and_unlock)(struct fuse_iqueue *fiq)
		__releases(fiq->lock);

	/**
	 * Signal that a request has been queued
	 */
	void (*wake_pending_and_unlock)(struct fuse_iqueue *fiq)
		__releases(fiq->lock);

	/**
	 * Clean up when fuse_iqueue is destroyed
	 */
	void (*release)(struct fuse_iqueue *fiq);
};

#define FUSE_PQ_HASH_BITS 8
#define FUSE_PQ_HASH_SIZE (1 << FUSE_PQ_HASH_BITS)

struct fuse_fs_context {
	int fd;
	unsigned int rootmode;
	kuid_t user_id;
	kgid_t group_id;
	bool is_bdev:1;
	bool fd_present:1;
	bool rootmode_present:1;
	bool user_id_present:1;
	bool group_id_present:1;
	bool default_permissions:1;
	bool allow_other:1;
	bool destroy:1;
	bool no_control:1;
	bool no_force_umount:1;
	bool legacy_opts_show:1;
	bool dax:1;
	unsigned int max_read;
	unsigned int blksize;
	const char *subtype;

	/* DAX device, may be NULL */
	struct dax_device *dax_dev;

	/* fuse_dev pointer to fill in, should contain NULL on entry */
	void **fudptr;
};


static inline struct fuse_mount *get_fuse_mount_super(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct fuse_conn *get_fuse_conn_super(struct super_block *sb)
{
	return get_fuse_mount_super(sb)->fc;
}

static inline struct fuse_mount *get_fuse_mount(struct inode *inode)
{
	return get_fuse_mount_super(inode->i_sb);
}

static inline struct fuse_conn *get_fuse_conn(struct inode *inode)
{
	return get_fuse_mount_super(inode->i_sb)->fc;
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

static inline u64 fuse_get_attr_version(struct fuse_conn *fc)
{
	return atomic64_read(&fc->attr_version);
}

static inline void fuse_make_bad(struct inode *inode)
{
	set_bit(FUSE_I_BAD, &get_fuse_inode(inode)->state);
}

static inline bool fuse_is_bad(struct inode *inode)
{
	return unlikely(test_bit(FUSE_I_BAD, &get_fuse_inode(inode)->state));
}

extern const struct dentry_operations fuse_dentry_operations;
extern const struct dentry_operations fuse_root_dentry_operations;

/**
 * Get a filled in inode
 */
struct inode *fuse_iget(struct super_block *sb, u64 nodeid,
			int generation, struct fuse_attr *attr,
			u64 attr_valid, u64 attr_version);

int fuse_lookup_name(struct super_block *sb, u64 nodeid, const struct qstr *name,
		     struct fuse_entry_out *outarg, struct inode **inode);

/**
 * Send FORGET command
 */
void fuse_queue_forget(struct fuse_conn *fc, struct fuse_forget_link *forget,
		       u64 nodeid, u64 nlookup);

struct fuse_forget_link *fuse_alloc_forget(void);

struct fuse_forget_link *fuse_dequeue_forget(struct fuse_iqueue *fiq,
					     unsigned int max,
					     unsigned int *countp);

/*
 * Initialize READ or READDIR request
 */
struct fuse_io_args {
	union {
		struct {
			struct fuse_read_in in;
			u64 attr_ver;
		} read;
		struct {
			struct fuse_write_in in;
			struct fuse_write_out out;
		} write;
	};
	struct fuse_args_pages ap;
	struct fuse_io_priv *io;
	struct fuse_file *ff;
};

void fuse_read_args_fill(struct fuse_io_args *ia, struct file *file, loff_t pos,
			 size_t count, int opcode);


/**
 * Send OPEN or OPENDIR request
 */
int fuse_open_common(struct inode *inode, struct file *file, bool isdir);

struct fuse_file *fuse_file_alloc(struct fuse_mount *fm);
void fuse_file_free(struct fuse_file *ff);
void fuse_finish_open(struct inode *inode, struct file *file);

/**
 * Send RELEASE or RELEASEDIR request
 */
void fuse_release_common(struct file *file, bool isdir);

/**
 * Send FSYNC or FSYNCDIR request
 */
int fuse_fsync_common(struct file *file, loff_t start, loff_t end,
		      int datasync, int opcode);

/**
 * Notify poll wakeup
 */
int fuse_notify_poll_wakeup(struct fuse_conn *fc,
			    struct fuse_notify_poll_wakeup_out *outarg);

/**
 * Initialize file operations on a regular file
 */
void fuse_init_file_inode(struct inode *inode);

/**
 * Initialize inode operations on regular files and special files
 */
void fuse_init_common(struct inode *inode);

/**
 * Initialize inode and file operations on a directory
 */
void fuse_init_dir(struct inode *inode);

/**
 * Initialize inode operations on a symlink
 */
void fuse_init_symlink(struct inode *inode);

/**
 * Change attributes of an inode
 */
void fuse_change_attributes(struct inode *inode, struct fuse_attr *attr,
			    u64 attr_valid, u64 attr_version);

void fuse_change_attributes_common(struct inode *inode, struct fuse_attr *attr,
				   u64 attr_valid);

/**
 * Initialize the client device
 */
int fuse_dev_init(void);

/**
 * Cleanup the client device
 */
void fuse_dev_cleanup(void);

int fuse_ctl_init(void);
void __exit fuse_ctl_cleanup(void);

/**
 * Simple request sending that does request allocation and freeing
 */
ssize_t fuse_simple_request(struct fuse_mount *fm, struct fuse_args *args);

/**
 * End a finished request
 */
void fuse_request_end(struct fuse_req *req);

void fuse_wait_aborted(struct fuse_conn *fc);

/**
 * Invalidate inode attributes
 */
void fuse_invalidate_attr(struct inode *inode);

void fuse_invalidate_entry_cache(struct dentry *entry);

void fuse_invalidate_atime(struct inode *inode);

u64 entry_attr_timeout(struct fuse_entry_out *o);
void fuse_change_entry_timeout(struct dentry *entry, struct fuse_entry_out *o);

struct fuse_dev *fuse_dev_alloc(void);
void fuse_dev_install(struct fuse_dev *fud, struct fuse_conn *fc);
void fuse_send_init(struct fuse_mount *fm);

/**
 * Fill in superblock and initialize fuse connection
 * @sb: partially-initialized superblock to fill in
 * @ctx: mount context
 */
int fuse_fill_super_common(struct super_block *sb, struct fuse_fs_context *ctx);

/*
 * Fill in superblock for submounts
 * @sb: partially-initialized superblock to fill in
 * @parent_fi: The fuse_inode of the parent filesystem where this submount is
 * 	       mounted
 */
int fuse_fill_super_submount(struct super_block *sb,
			     struct fuse_inode *parent_fi);

/*
 * Remove the mount from the connection
 *
 * Returns whether this was the last mount
 */
bool fuse_mount_remove(struct fuse_mount *fm);

/*
 * Shut down the connection (possibly sending DESTROY request).
 */
void fuse_conn_destroy(struct fuse_mount *fm);

/**
 * Add connection to control filesystem
 */
int fuse_ctl_add_conn(struct fuse_conn *fc);

/**
 * Remove connection from control filesystem
 */
void fuse_ctl_remove_conn(struct fuse_conn *fc);

/**
 * Is file type valid?
 */
int fuse_valid_type(int m);

bool fuse_invalid_attr(struct fuse_attr *attr);

/**
 * Is current process allowed to perform filesystem operation?
 */
int fuse_allow_current_process(struct fuse_conn *fc);

u64 fuse_lock_owner_id(struct fuse_conn *fc, fl_owner_t id);

void fuse_update_ctime(struct inode *inode);

int fuse_update_attributes(struct inode *inode, struct file *file);

void fuse_flush_writepages(struct inode *inode);

void fuse_set_nowrite(struct inode *inode);
void fuse_release_nowrite(struct inode *inode);

/**
 * Scan all fuse_mounts belonging to fc to find the first where
 * ilookup5() returns a result.  Return that result and the
 * respective fuse_mount in *fm (unless fm is NULL).
 *
 * The caller must hold fc->killsb.
 */
struct inode *fuse_ilookup(struct fuse_conn *fc, u64 nodeid,
			   struct fuse_mount **fm);

/**
 * File-system tells the kernel to invalidate cache for the given node id.
 */
int fuse_reverse_inval_inode(struct fuse_conn *fc, u64 nodeid,
			     loff_t offset, loff_t len);

/**
 * File-system tells the kernel to invalidate parent attributes and
 * the dentry matching parent/name.
 *
 * If the child_nodeid is non-zero and:
 *    - matches the inode number for the dentry matching parent/name,
 *    - is not a mount point
 *    - is a file or oan empty directory
 * then the dentry is unhashed (d_delete()).
 */
int fuse_reverse_inval_entry(struct fuse_conn *fc, u64 parent_nodeid,
			     u64 child_nodeid, struct qstr *name);

long fuse_ioctl_common(struct file *file, unsigned int cmd,
		       unsigned long arg, unsigned int flags);

bool fuse_write_update_size(struct inode *inode, loff_t pos);

int fuse_flush_times(struct inode *inode, struct fuse_file *ff);
int fuse_write_inode(struct inode *inode, struct writeback_control *wbc);

int fuse_do_setattr(struct dentry *dentry, struct iattr *attr,
		    struct file *file);

void fuse_set_initialized(struct fuse_conn *fc);

void fuse_unlock_inode(struct inode *inode, bool locked);
bool fuse_lock_inode(struct inode *inode);

int fuse_setxattr(struct inode *inode, const char *name, const void *value,
		  size_t size, int flags);
ssize_t fuse_getxattr(struct inode *inode, const char *name, void *value,
		      size_t size);
ssize_t fuse_listxattr(struct dentry *entry, char *list, size_t size);
int fuse_removexattr(struct inode *inode, const char *name);
extern const struct xattr_handler *fuse_xattr_handlers[];
extern const struct xattr_handler *fuse_acl_xattr_handlers[];
extern const struct xattr_handler *fuse_no_acl_xattr_handlers[];

struct posix_acl;
struct posix_acl *fuse_get_acl(struct inode *inode, int type);
int fuse_set_acl(struct inode *inode, struct posix_acl *acl, int type);


/* readdir.c */
int fuse_readdir(struct file *file, struct dir_context *ctx);

/**
 * Return the number of bytes in an arguments list
 */
unsigned int fuse_len_args(unsigned int numargs, struct fuse_arg *args);

/**
 * Get the next unique ID for a request
 */
u64 fuse_get_unique(struct fuse_iqueue *fiq);
void fuse_free_conn(struct fuse_conn *fc);

/* dax.c */

#define FUSE_IS_DAX(inode) (IS_ENABLED(CONFIG_FUSE_DAX) && IS_DAX(inode))

ssize_t fuse_dax_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t fuse_dax_write_iter(struct kiocb *iocb, struct iov_iter *from);
int fuse_dax_mmap(struct file *file, struct vm_area_struct *vma);
int fuse_dax_break_layouts(struct inode *inode, u64 dmap_start, u64 dmap_end);
int fuse_dax_conn_alloc(struct fuse_conn *fc, struct dax_device *dax_dev);
void fuse_dax_conn_free(struct fuse_conn *fc);
bool fuse_dax_inode_alloc(struct super_block *sb, struct fuse_inode *fi);
void fuse_dax_inode_init(struct inode *inode);
void fuse_dax_inode_cleanup(struct inode *inode);
bool fuse_dax_check_alignment(struct fuse_conn *fc, unsigned int map_alignment);
void fuse_dax_cancel_work(struct fuse_conn *fc);

#endif /* _FS_FUSE_I_H */
