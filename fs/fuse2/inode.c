/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/module.h>
#include <linux/statfs.h>
#include <linux/random.h>
#include <linux/backing-dev.h>
#include <linux/pid_namespace.h>

MODULE_AUTHOR("Miklos Szeredi <miklos@szeredi.hu>");
MODULE_DESCRIPTION("Filesystem in Userspace");
MODULE_LICENSE("GPL");

static struct kmem_cache *fuse_inode_cachep;

#define FUSE_SUPER_MAGIC 0x65735546

#define FUSE_DEFAULT_BLKSIZE 512

static struct inode *fuse_alloc_inode(struct super_block *sb)
{
	struct fuse_inode *fi;

	fi = kmem_cache_alloc(fuse_inode_cachep, GFP_KERNEL);
	if (!fi)
		return NULL;

	fi->nodeid = 0;
	fi->nlookup = 0;
	fi->state = 0;
	spin_lock_init(&fi->lock);
	fi->forget = fuse2_alloc_forget();
	if (!fi->forget) {
		kmem_cache_free(fuse_inode_cachep, fi);
		return NULL;
	}

	return &fi->inode;
}

static void fuse_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(fuse_inode_cachep, inode);
}

static void fuse_destroy_inode(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	kfree(fi->forget);
	call_rcu(&inode->i_rcu, fuse_i_callback);
}

static void fuse_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (inode->i_sb->s_flags & SB_ACTIVE) {
		struct fuse_conn *fc = get_fuse_conn(inode);
		struct fuse_inode *fi = get_fuse_inode(inode);
		fuse2_queue_forget(fc, fi->forget, fi->nodeid, fi->nlookup);
		fi->forget = NULL;
	}
}

static int fuse_remount_fs(struct super_block *sb, int *flags, char *data)
{
	sync_filesystem(sb);
	if (*flags & SB_MANDLOCK)
		return -EINVAL;

	return 0;
}

/*
 * ino_t is 32-bits on 32-bit arch. We have to squash the 64-bit value down
 * so that it will fit.
 */
static ino_t fuse_squash_ino(u64 ino64)
{
	ino_t ino = (ino_t) ino64;
	if (sizeof(ino_t) < sizeof(u64))
		ino ^= ino64 >> (sizeof(u64) - sizeof(ino_t)) * 8;
	return ino;
}

void fuse2_change_attributes_common(struct inode *inode, struct fuse_attr *attr)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);

	lockdep_assert_held(&fi->lock);

	inode->i_ino     = fuse_squash_ino(attr->ino);
	inode->i_mode    = (inode->i_mode & S_IFMT) | (attr->mode & 07777);
	set_nlink(inode, attr->nlink);
	inode->i_uid     = make_kuid(fc->user_ns, attr->uid);
	inode->i_gid     = make_kgid(fc->user_ns, attr->gid);
	inode->i_blocks  = attr->blocks;
	inode->i_atime.tv_sec   = attr->atime;
	inode->i_atime.tv_nsec  = attr->atimensec;
	inode->i_mtime.tv_sec   = attr->mtime;
	inode->i_mtime.tv_nsec  = attr->mtimensec;
	inode->i_ctime.tv_sec   = attr->ctime;
	inode->i_ctime.tv_nsec  = attr->ctimensec;

	if (attr->blksize != 0)
		inode->i_blkbits = ilog2(attr->blksize);
	else
		inode->i_blkbits = inode->i_sb->s_blocksize_bits;
}

void fuse2_change_attributes(struct inode *inode, struct fuse_attr *attr)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_conn *fc = get_fuse_conn(inode);
	loff_t oldsize;
	struct timespec64 old_mtime;

	/* before taking spinlock, check if anything has changed */
	if (i_size_read(inode) == attr->size &&
	    inode->i_ino == fuse_squash_ino(attr->ino) &&
	    (inode->i_mode & 07777) == (attr->mode & 07777) &&
	    inode->i_nlink == attr->nlink &&
	    uid_eq(inode->i_uid, make_kuid(fc->user_ns, attr->uid)) &&
	    gid_eq(inode->i_gid, make_kgid(fc->user_ns, attr->gid)) &&
	    inode->i_blocks == attr->blocks &&
	    inode->i_atime.tv_sec == attr->atime &&
	    inode->i_atime.tv_nsec == attr->atimensec &&
	    inode->i_mtime.tv_sec == attr->mtime &&
	    inode->i_mtime.tv_nsec == attr->mtimensec &&
	    inode->i_ctime.tv_sec == attr->ctime &&
	    inode->i_ctime.tv_nsec == attr->ctimensec &&
	    ((attr->blksize != 0 && inode->i_blkbits == ilog2(attr->blksize)) ||
	     (attr->blksize == 0 && inode->i_blkbits == inode->i_sb->s_blocksize_bits)))
		return;

	spin_lock(&fi->lock);
	old_mtime = inode->i_mtime;
	fuse2_change_attributes_common(inode, attr);

	oldsize = inode->i_size;
	i_size_write(inode, attr->size);
	spin_unlock(&fi->lock);

	if (S_ISREG(inode->i_mode)) {
		if (oldsize != attr->size) {
			truncate_pagecache(inode, attr->size);
			invalidate_inode_pages2(inode->i_mapping);
		}
	}
}

static void fuse_init_inode(struct inode *inode, struct fuse_attr *attr)
{
	inode->i_mode = attr->mode & S_IFMT;
	inode->i_size = attr->size;
	inode->i_mtime.tv_sec  = attr->mtime;
	inode->i_mtime.tv_nsec = attr->mtimensec;
	inode->i_ctime.tv_sec  = attr->ctime;
	inode->i_ctime.tv_nsec = attr->ctimensec;
	if (S_ISREG(inode->i_mode)) {
		fuse2_init_common(inode);
		fuse2_init_file_inode(inode);
	} else if (S_ISDIR(inode->i_mode))
		fuse2_init_dir(inode);
	else if (S_ISLNK(inode->i_mode))
		fuse2_init_symlink(inode);
	else if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) ||
		 S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
		fuse2_init_common(inode);
		init_special_inode(inode, inode->i_mode,
				   new_decode_dev(attr->rdev));
	} else
		BUG();
}

int fuse2_inode_eq(struct inode *inode, void *_nodeidp)
{
	u64 nodeid = *(u64 *) _nodeidp;
	if (get_node_id(inode) == nodeid)
		return 1;
	else
		return 0;
}

static int fuse_inode_set(struct inode *inode, void *_nodeidp)
{
	u64 nodeid = *(u64 *) _nodeidp;
	get_fuse_inode(inode)->nodeid = nodeid;
	return 0;
}

struct inode *fuse2_iget(struct super_block *sb, u64 nodeid,
			int generation, struct fuse_attr *attr)
{
	struct inode *inode;
	struct fuse_inode *fi;

 retry:
	inode = iget5_locked(sb, nodeid, fuse2_inode_eq, fuse_inode_set, &nodeid);
	if (!inode)
		return NULL;

	if ((inode->i_state & I_NEW)) {
		inode->i_flags |= S_NOATIME;
		inode->i_flags |= S_NOCMTIME;
		inode->i_generation = generation;
		fuse_init_inode(inode, attr);
		unlock_new_inode(inode);
	} else if ((inode->i_mode ^ attr->mode) & S_IFMT) {
		/* Inode has changed type, any I/O on the old should fail */
		make_bad_inode(inode);
		iput(inode);
		goto retry;
	}

	fi = get_fuse_inode(inode);
	spin_lock(&fi->lock);
	fi->nlookup++;
	spin_unlock(&fi->lock);
	fuse2_change_attributes(inode, attr);

	return inode;
}

static void fuse_umount_begin(struct super_block *sb)
{
	struct fuse_conn *fc = get_fuse_conn_super(sb);

	fc->dev_ops->abort(fc->dev_priv);
}

static void fuse_conn_put(struct fuse_conn *fc)
{
	if (refcount_dec_and_test(&fc->count)) {
		put_pid_ns(fc->pid_ns);
		put_user_ns(fc->user_ns);
		fc->dev_ops->put(fc->dev_priv);
		kfree_rcu(fc, rcu);
	}
}

static void fuse_abort_and_put(struct fuse_conn *fc)
{
	fc->dev_ops->abort(fc->dev_priv);
	fuse_conn_put(fc);
}

static void fuse_put_super(struct super_block *sb)
{
	struct fuse_conn *fc = get_fuse_conn_super(sb);
	FUSE_ARGS(args);

	args.force = true;
	args.in.h.opcode = FUSE_DESTROY;
	fuse2_simple_request(fc, &args);

	fuse_abort_and_put(fc);
}

static void convert_fuse_statfs(struct kstatfs *stbuf, struct fuse_kstatfs *attr)
{
	stbuf->f_type    = FUSE_SUPER_MAGIC;
	stbuf->f_bsize   = attr->bsize;
	stbuf->f_frsize  = attr->frsize;
	stbuf->f_blocks  = attr->blocks;
	stbuf->f_bfree   = attr->bfree;
	stbuf->f_bavail  = attr->bavail;
	stbuf->f_files   = attr->files;
	stbuf->f_ffree   = attr->ffree;
	stbuf->f_namelen = attr->namelen;
	/* fsid is left zero */
}

static int fuse_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct fuse_conn *fc = get_fuse_conn_super(sb);
	FUSE_ARGS(args);
	struct fuse_statfs_out outarg;
	int err;

	if (!fuse2_allow_current_process(fc)) {
		buf->f_type = FUSE_SUPER_MAGIC;
		return 0;
	}

	memset(&outarg, 0, sizeof(outarg));
	args.in.h.opcode = FUSE_STATFS;
	args.in.h.nodeid = get_node_id(d_inode(dentry));
	args.out.numargs = 1;
	args.out.args[0].size = sizeof(outarg);
	args.out.args[0].value = &outarg;
	err = fuse2_simple_request(fc, &args);
	if (!err)
		convert_fuse_statfs(buf, &outarg.st);
	return err;
}

static int fuse_show_options(struct seq_file *m, struct dentry *root)
{
	return 0;
}

static struct fuse_conn *fuse_conn_alloc(void)
{
	struct fuse_conn *fc;

	fc = kzalloc(sizeof(struct fuse_conn), GFP_KERNEL);
	if (!fc)
		goto out;

	refcount_set(&fc->count, 1);
	atomic64_set(&fc->khctr, 0);
	get_random_bytes(&fc->scramble_key, sizeof(fc->scramble_key));
	fc->pid_ns = get_pid_ns(task_active_pid_ns(current));
	fc->user_ns = get_user_ns(current_user_ns());
	fc->max_pages = FUSE_DEFAULT_MAX_PAGES_PER_REQ;

out:
	return fc;
}

static struct fuse_conn *fuse_conn_get(struct fuse_conn *fc)
{
	refcount_inc(&fc->count);
	return fc;
}

static struct inode *fuse_get_root_inode(struct super_block *sb, unsigned mode)
{
	struct fuse_attr attr;
	memset(&attr, 0, sizeof(attr));

	attr.mode = mode;
	attr.ino = FUSE_ROOT_ID;
	attr.nlink = 1;
	return fuse2_iget(sb, 1, 0, &attr);
}

static const struct super_operations fuse_super_operations = {
	.alloc_inode    = fuse_alloc_inode,
	.destroy_inode  = fuse_destroy_inode,
	.evict_inode	= fuse_evict_inode,
	.drop_inode	= generic_delete_inode,
	.remount_fs	= fuse_remount_fs,
	.put_super	= fuse_put_super,
	.umount_begin	= fuse_umount_begin,
	.statfs		= fuse_statfs,
	.show_options	= fuse_show_options,
};

static int process_init_reply(struct fuse_conn *fc, struct super_block *sb,
			      struct fuse_init_out *arg)
{
	unsigned long ra_pages;

	if (arg->major != FUSE_KERNEL_VERSION || arg->minor < 23)
		return -EINVAL;

	ra_pages = arg->max_readahead / PAGE_SIZE;
	if (!(arg->flags & FUSE_POSIX_LOCKS))
		fc->no_lock = 1;
	if (!(arg->flags & FUSE_FLOCK_LOCKS))
		fc->no_flock = 1;
	if (!(arg->flags & FUSE_ATOMIC_O_TRUNC)) {
		pr_info("fuse: must support FUSE_ATOMIC_O_TRUNC\n");
		return -EINVAL;
	}
	if (!(arg->flags & FUSE_BIG_WRITES)) {
		pr_info("fuse: must support FUSE_BIG_WRITES\n");
		return -EINVAL;
	}
	if (!(arg->flags & FUSE_PARALLEL_DIROPS)) {
		pr_info("fuse: must support FUSE_PARALLEL_DIROPS\n");
		return -EINVAL;
	}
	if (!(arg->flags & FUSE_HANDLE_KILLPRIV)) {
		pr_info("fuse: must support FUSE_HANDLE_KILLPRIV\n");
		return -EINVAL;
	}
	if (arg->time_gran && arg->time_gran <= 1000000000)
		sb->s_time_gran = arg->time_gran;
	if ((arg->flags & FUSE_POSIX_ACL)) {
		fc->posix_acl = 1;
		sb->s_xattr = fuse2_acl_xattr_handlers;
	}
	if (arg->flags & FUSE_MAX_PAGES) {
		fc->max_pages =
			min_t(unsigned int, FUSE_MAX_MAX_PAGES,
			      max_t(unsigned int, arg->max_pages, 1));
	}
	return 0;
}

static int fuse_send_init(struct fuse_conn *fc, struct super_block *sb,
			  char *opts)
{
	struct fuse_init_in inarg;
	struct fuse_init_out outarg;
	FUSE_ARGS(args);
	int err;

	memset(&inarg, 0, sizeof(inarg));
	inarg.major = FUSE_KERNEL_VERSION;
	inarg.minor = FUSE_KERNEL_MINOR_VERSION;
	inarg.max_readahead = sb->s_bdi->ra_pages * PAGE_SIZE;
	inarg.flags = FUSE_POSIX_LOCKS | FUSE_ATOMIC_O_TRUNC |
		FUSE_BIG_WRITES | FUSE_PARALLEL_DIROPS | FUSE_FLOCK_LOCKS |
		FUSE_HANDLE_KILLPRIV | FUSE_POSIX_ACL |
		FUSE_MAX_PAGES;

	args.killable = true;
	args.in.h.opcode = FUSE_INIT;
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	if (opts) {
		args.in.numargs++;
		args.in.args[1].size = strlen(opts) + 1;
		args.in.args[1].value = opts;
	}
	args.out.numargs = 1;
	args.out.args[0].size = sizeof(outarg);
	args.out.args[0].value = &outarg;
	err = fuse2_simple_request(fc, &args);
	if (!err)
		err = process_init_reply(fc, sb, &outarg);

	return err;
}

static int fuse_fill_super(struct super_block *sb, char *opts)
{
	struct fuse_conn *fc = get_fuse_conn_super(sb);
	struct inode *root;
	int err;

	if (sb->s_flags & SB_MANDLOCK)
		return -EINVAL;

	sb->s_flags &= ~(SB_NOSEC | SB_I_VERSION);
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = FUSE_SUPER_MAGIC;
	sb->s_op = &fuse_super_operations;
	sb->s_xattr = fuse2_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;
	sb->s_iflags |= SB_I_IMA_UNVERIFIABLE_SIGNATURE;
	if (sb->s_user_ns != &init_user_ns)
		sb->s_iflags |= SB_I_UNTRUSTED_MOUNTER;

	/*
	 * If we are not in the initial user namespace posix
	 * acls must be translated.
	 */
	if (sb->s_user_ns != &init_user_ns)
		sb->s_xattr = fuse2_no_acl_xattr_handlers;

	sb->s_flags |= SB_POSIXACL;

	err = fuse_send_init(fc, sb, opts);
	if (err)
		return err;

	root = fuse_get_root_inode(sb, S_IFDIR | 0);
	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;

	/* Root dentry doesn't have .d_revalidate */
	sb->s_d_op = &fuse2_dentry_operations;

	return 0;
}

static int fuse_set_super(struct super_block *s, void *data)
{
	int err;

	err = get_anon_bdev(&s->s_dev);
	if (!err)
		s->s_fs_info = fuse_conn_get(data);

	return err;
}

static int fuse_test_super(struct super_block *s, void *data)
{
	struct fuse_conn *fc = data;

	return fc->dev_priv == get_fuse_conn_super(s)->dev_priv;
}

struct dentry *fuse_mount_common(struct file_system_type *fs_type,
				 int flags, void *opts,
				 const struct fuse_dev_operations *dev_ops,
				 void *dev_priv)
{
	struct super_block *s;
	struct fuse_conn *fc;
	int err;

	fc = fuse_conn_alloc();
	if (!fc) {
		dev_ops->abort(dev_priv);
		dev_ops->put(dev_priv);
		return ERR_PTR(-ENOMEM);
	}
	fc->dev_ops = dev_ops;
	fc->dev_priv = dev_priv;

	s = sget(fs_type, fuse_test_super, fuse_set_super, flags, fc);
	err = PTR_ERR(s);
	if (IS_ERR(s))
		goto abort;

	err = -EIO;
	if (WARN_ON(fc->user_ns != s->s_user_ns))
		goto deactivate;

	if (s->s_root) {
		err = -EBUSY;
		if ((flags ^ s->s_flags) & SB_RDONLY)
			goto deactivate;
	} else {
		err = fuse_fill_super(s, opts);
		if (err)
			goto deactivate;

		s->s_flags |= SB_ACTIVE;
	}
	fuse_conn_put(fc);

	return dget(s->s_root);

deactivate:
	deactivate_locked_super(s);
abort:
	fuse_abort_and_put(fc);

	return ERR_PTR(err);
}
EXPORT_SYMBOL(fuse_mount_common);

void fuse_kill_sb(struct super_block *sb)
{
	struct fuse_conn *fc = get_fuse_conn_super(sb);

	if (!sb->s_root) {
		/*
		 * Setup of the sb didn't complete, ->put_super() won't be
		 * called.
		 */
		sb->s_fs_info = NULL;
		fuse_abort_and_put(fc);
	}
	kill_anon_super(sb);
}
EXPORT_SYMBOL(fuse_kill_sb);


static void fuse_inode_init_once(void *foo)
{
	struct inode *inode = foo;

	inode_init_once(inode);
}

static int __init fuse_fs_init(void)
{
	fuse_inode_cachep = kmem_cache_create("fuse2_inode",
			sizeof(struct fuse_inode), 0,
			SLAB_HWCACHE_ALIGN|SLAB_ACCOUNT|SLAB_RECLAIM_ACCOUNT,
			fuse_inode_init_once);

	if (!fuse_inode_cachep)
		return -ENOMEM;

	return 0;
}

static void fuse_fs_cleanup(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(fuse_inode_cachep);
}

static int __init fuse_init(void)
{
	int res;

	res = fuse_fs_init();
	if (res)
		goto err;

	res = fuse_req_cache_init();
	if (res)
		goto err_fs_cleanup;

	pr_info("fuse2 core initialized (API version %i.%i)\n",
		FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION);

	return 0;

err_fs_cleanup:
	fuse_fs_cleanup();
err:
	return res;
}

static void __exit fuse_exit(void)
{
	printk(KERN_DEBUG "fuse exit\n");

	fuse_req_cache_cleanup();
	fuse_fs_cleanup();
}

module_init(fuse_init);
module_exit(fuse_exit);
