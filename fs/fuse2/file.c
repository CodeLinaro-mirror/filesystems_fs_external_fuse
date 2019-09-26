/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/module.h>
#include <linux/compat.h>
#include <linux/swap.h>
#include <linux/falloc.h>
#include <linux/uio.h>

static int fuse_send_open(struct fuse_conn *fc, u64 nodeid, struct file *file,
			  int opcode, struct fuse_open_out *outargp)
{
	struct fuse_open_in inarg;
	FUSE_ARGS(args);

	memset(&inarg, 0, sizeof(inarg));
	inarg.flags = file->f_flags & ~(O_CREAT | O_EXCL | O_NOCTTY);
	args.in.h.opcode = opcode;
	args.in.h.nodeid = nodeid;
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	args.out.numargs = 1;
	args.out.args[0].size = sizeof(*outargp);
	args.out.args[0].value = outargp;

	return fuse2_simple_request(fc, &args);
}

struct fuse_file *fuse2_file_alloc(struct fuse_conn *fc)
{
	struct fuse_file *ff;

	ff = kzalloc(sizeof(struct fuse_file), GFP_KERNEL);
	if (unlikely(!ff))
		return NULL;

	ff->fc = fc;
	ff->kh = atomic64_inc_return(&fc->khctr);

	return ff;
}

void fuse2_file_free(struct fuse_file *ff)
{
	kfree(ff);
}

static int fuse_do_open(struct fuse_conn *fc, u64 nodeid, struct file *file,
		 bool isdir)
{
	struct fuse_file *ff;
	int opcode = isdir ? FUSE_OPENDIR : FUSE_OPEN;
	struct fuse_open_out outarg;
	int err;

	ff = fuse2_file_alloc(fc);
	if (!ff)
		return -ENOMEM;

	err = fuse_send_open(fc, nodeid, file, opcode, &outarg);
	if (err) {
		fuse2_file_free(ff);
		return err;
	}
	ff->open_flags = outarg.open_flags;
	ff->fh = outarg.fh;
	ff->nodeid = nodeid;
	file->private_data = ff;

	return 0;
}

void fuse2_finish_open(struct inode *inode, struct file *file)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	if (file->f_flags & O_TRUNC) {
		spin_lock(&fi->lock);
		i_size_write(inode, 0);
		spin_unlock(&fi->lock);
	}
}

int fuse2_open_common(struct inode *inode, struct file *file, bool isdir)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	int err;

	err = generic_file_open(inode, file);
	if (err)
		return err;

	err = fuse_do_open(fc, get_node_id(inode), file, isdir);
	if (!err)
		fuse2_finish_open(inode, file);

	return err;
}

void fuse2_release_common(struct fuse_conn *fc, struct fuse_file *ff,
			 int flags, fl_owner_t id, bool isdir)
{
	struct fuse_release_in inarg;
	FUSE_ARGS(args);

	memset(&inarg, 0, sizeof(inarg));
	inarg.fh = ff->fh;
	inarg.flags = flags;
	args.force = true;
	args.in.h.opcode = isdir ? FUSE_RELEASEDIR : FUSE_RELEASE;
	args.in.h.nodeid = ff->nodeid;
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;

	if (ff->flock) {
		inarg.release_flags |= FUSE_RELEASE_FLOCK_UNLOCK;
		inarg.lock_owner = fuse2_lock_owner_id(ff->fc, id);
	}
	fuse2_simple_request(fc, &args);

	kfree(ff);
}

static int fuse_open(struct inode *inode, struct file *file)
{
	return fuse2_open_common(inode, file, false);
}

static int fuse_release(struct inode *inode, struct file *file)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_file *ff = file->private_data;

	fuse2_release_common(fc, ff, file->f_flags, (fl_owner_t) file, false);

	/* return value is ignored by VFS */
	return 0;
}

/*
 * Scramble the ID space with XTEA, so that the value of the files_struct
 * pointer is not exposed to userspace.
 */
u64 fuse2_lock_owner_id(struct fuse_conn *fc, fl_owner_t id)
{
	u32 *k = fc->scramble_key;
	u64 v = (unsigned long) id;
	u32 v0 = v;
	u32 v1 = v >> 32;
	u32 sum = 0;
	int i;

	for (i = 0; i < 32; i++) {
		v0 += ((v1 << 4 ^ v1 >> 5) + v1) ^ (sum + k[sum & 3]);
		sum += 0x9E3779B9;
		v1 += ((v0 << 4 ^ v0 >> 5) + v0) ^ (sum + k[sum>>11 & 3]);
	}

	return (u64) v0 + ((u64) v1 << 32);
}

static int fuse_flush(struct file *file, fl_owner_t id)
{
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_file *ff = file->private_data;
	struct fuse_flush_in inarg;
	FUSE_ARGS(args);
	int err;

	if (is_bad_inode(inode))
		return -EIO;

	if (fc->no_flush)
		return 0;

	err = filemap_check_errors(file->f_mapping);
	if (err)
		return err;

	memset(&inarg, 0, sizeof(inarg));
	inarg.fh = ff->fh;
	inarg.lock_owner = fuse2_lock_owner_id(fc, id);
	args.force = true;
	args.in.h.opcode = FUSE_FLUSH;
	args.in.h.nodeid = get_node_id(inode);
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	err = fuse2_simple_request(fc, &args);
	if (err == -ENOSYS) {
		fc->no_flush = 1;
		err = 0;
	}
	return err;
}

int fuse2_fsync_common(struct file *file, loff_t start, loff_t end,
		      int datasync, int opcode)
{
	struct inode *inode = file->f_mapping->host;
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_file *ff = file->private_data;
	FUSE_ARGS(args);
	struct fuse_fsync_in inarg;

	memset(&inarg, 0, sizeof(inarg));
	inarg.fh = ff->fh;
	inarg.fsync_flags = datasync ? 1 : 0;
	args.in.h.opcode = opcode;
	args.in.h.nodeid = get_node_id(inode);
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	return fuse2_simple_request(fc, &args);
}

static int fuse_fsync(struct file *file, loff_t start, loff_t end,
		      int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct fuse_conn *fc = get_fuse_conn(inode);
	int err;

	if (is_bad_inode(inode))
		return -EIO;

	if (fc->no_fsync)
		return 0;

	inode_lock(inode);
	err = fuse2_fsync_common(file, start, end, datasync, FUSE_FSYNC);
	if (err == -ENOSYS) {
		fc->no_fsync = 1;
		err = 0;
	}
	inode_unlock(inode);

	return err;
}

void fuse2_read_fill(struct fuse_req *req, struct fuse_read_in **p,
		    struct file *file, loff_t pos, size_t count, int opcode)
{
	struct fuse_file *ff = file->private_data;
	struct fuse_read_in *inarg;

	req->inline_inlen = sizeof(struct fuse_in_header) + sizeof(*inarg);
	BUILD_BUG_ON(req->inline_inlen > FUSE_REQ_INLINE_DATA);

	req->mand_outlen = req->inline_outlen = sizeof(struct fuse_out_header);
	req->max_outlen = req->mand_outlen + count;

	inarg = (void *) req->inlinedata + sizeof(struct fuse_in_header);
	inarg->fh = ff->fh;
	inarg->offset = pos;
	inarg->size = count;
	inarg->flags = file->f_flags;
	if (p)
		*p = inarg;

	req->inh.opcode = opcode;
	req->inh.nodeid = ff->nodeid;
	req->inh.len = req->inline_inlen;
}

static ssize_t fuse_send_read(struct fuse_req *req, struct kiocb *iocb,
			      loff_t pos, size_t count)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;
	struct fuse_conn *fc = ff->fc;
	ssize_t res;

	fuse2_read_fill(req, NULL, iocb->ki_filp, pos, count, FUSE_READ);
	res = fuse2_request_send(fc, req);

	return res ? res : req->outh.len - req->mand_outlen;
}

static int fuse_do_readpage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_req *req;
	loff_t pos = page_offset(page);
	size_t count = PAGE_SIZE;
	int err;

	req = fuse2_get_req(fc, 1);
	if (IS_ERR(req))
		return PTR_ERR(req);

	req->num_pages = 1;
	get_page(req->pages[0] = page);
	req->page_descs[0].length = count;
	__set_bit(FR_ZEROTAIL, &req->flags);
	fuse2_read_fill(req, NULL, file, pos, count, FUSE_READ);
	err = fuse2_request_send(fc, req);

	if (!err)
		SetPageUptodate(page);

	fuse2_put_request(req);

	return err;
}

static int fuse_readpage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	int err;

	err = -EIO;
	if (is_bad_inode(inode))
		goto out;

	err = fuse_do_readpage(file, page);
 out:
	unlock_page(page);
	return err;
}

static ssize_t fuse_send_write(struct fuse_req *req, struct kiocb *iocb,
			       loff_t pos, size_t count)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;
	struct fuse_conn *fc = ff->fc;
	struct fuse_write_in *inarg;
	struct fuse_write_out *outarg;
	ssize_t res;

	req->inline_inlen = sizeof(struct fuse_in_header) + sizeof(*inarg);
	BUILD_BUG_ON(req->inline_inlen > FUSE_REQ_INLINE_DATA);

	req->inline_outlen = sizeof(struct fuse_out_header) + sizeof(*outarg);
	BUILD_BUG_ON(req->inline_outlen > FUSE_REQ_INLINE_DATA);
	req->max_outlen = req->mand_outlen = req->inline_outlen;

	inarg = (void *) req->inlinedata + sizeof(struct fuse_in_header); 
	inarg->fh = ff->fh;
	inarg->offset = pos;
	inarg->size = count;
	inarg->flags = file->f_flags;
	if (iocb->ki_flags & IOCB_DSYNC)
		inarg->flags |= O_DSYNC;
	if (iocb->ki_flags & IOCB_SYNC)
		inarg->flags |= O_SYNC;
	if (!capable(CAP_FSETID))
		inarg->write_flags |= FUSE_WRITE_KILL_PRIV;
	req->inh.opcode = FUSE_WRITE;
	req->inh.nodeid = ff->nodeid;
	req->inh.len = req->inline_inlen + count;
	res = fuse2_request_send(fc, req);
	outarg = (void *) req->inlinedata + sizeof(struct fuse_out_header);

	return res ? res : outarg->size;
}

static void fuse_write_update_size(struct inode *inode, loff_t pos)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	spin_lock(&fi->lock);
	if (pos > inode->i_size)
		i_size_write(inode, pos);
	spin_unlock(&fi->lock);
}

static inline void fuse_page_descs_length_init(struct fuse_req *req,
		unsigned index, unsigned nr_pages)
{
	int i;

	for (i = index; i < index + nr_pages; i++)
		req->page_descs[i].length = PAGE_SIZE -
			req->page_descs[i].offset;
}

static int fuse_get_user_pages(struct fuse_req *req, struct iov_iter *ii,
			       size_t *nbytesp, int write)
{
	size_t nbytes = 0;  /* # bytes already packed in req */
	ssize_t ret = 0;

	while (nbytes < *nbytesp && req->num_pages < req->max_pages) {
		unsigned npages;
		size_t start;
		ret = iov_iter_get_pages(ii, &req->pages[req->num_pages],
					*nbytesp - nbytes,
					req->max_pages - req->num_pages,
					&start);
		if (ret < 0)
			break;

		iov_iter_advance(ii, ret);
		nbytes += ret;

		ret += start;
		npages = (ret + PAGE_SIZE - 1) / PAGE_SIZE;

		req->page_descs[req->num_pages].offset = start;
		fuse_page_descs_length_init(req, req->num_pages, npages);

		req->num_pages += npages;
		req->page_descs[req->num_pages - 1].length -=
			(PAGE_SIZE - ret) & (PAGE_SIZE - 1);
	}

	*nbytesp = nbytes;

	return ret < 0 ? ret : 0;
}

/* If set, it is WRITE; otherwise - READ */
#define FUSE_DIO_WRITE (1 << 0)

static ssize_t fuse_direct_io(struct kiocb *iocb, struct iov_iter *iter,
			      int flags)
{
	int write = flags & FUSE_DIO_WRITE;
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;
	struct fuse_conn *fc = ff->fc;
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(iter);
	ssize_t res = 0;
	struct fuse_req *req;
	bool should_dirty = !write && iter_is_iovec(iter);
	unsigned int i;
	int err = 0;

	req = fuse2_get_req(fc, iov_iter_npages(iter, fc->max_pages));
	if (IS_ERR(req))
		return PTR_ERR(req);

	while (count) {
		ssize_t nres;
		size_t nbytes = count;
		err = fuse_get_user_pages(req, iter, &nbytes, write);
		if (err && !nbytes)
			break;

		if (write)
			nres = fuse_send_write(req, iocb, pos, nbytes);
		else
			nres = fuse_send_read(req, iocb, pos, nbytes);

		if (should_dirty) {
			for (i = 0; i < req->num_pages; i++)
				set_page_dirty_lock(req->pages[i]);
		}
		if (nres < 0) {
			err = nres;
			break;
		} else if (nres > nbytes) {
			res = 0;
			err = -EIO;
			break;
		}
		count -= nres;
		res += nres;
		pos += nres;
		if (nres != nbytes)
			break;
		if (count) {
			fuse2_put_request(req);
			req = fuse2_get_req(fc, iov_iter_npages(iter, fc->max_pages));
			if (IS_ERR(req))
				break;
		}
	}
	if (!IS_ERR(req))
		fuse2_put_request(req);

	if (res > 0) {
		iocb->ki_pos = pos;
		return res;
	}

	return err;
}

static ssize_t fuse_read_buf(struct file *file, loff_t pos,
			     void *buf, size_t len)
{
	struct fuse_conn *fc = get_fuse_conn(file_inode(file));
	struct fuse_file *ff = file->private_data;
	struct fuse_read_in inarg = {
		.fh = ff->fh,
		.offset = pos,
		.size = len,
		.flags = file->f_flags,
	};
	FUSE_ARGS(args);

	args.in.h.opcode = FUSE_READ;
	args.in.h.nodeid = ff->nodeid;
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	args.out.argvar = 1;
	args.out.numargs = 1;
	args.out.args[0].size = len;
	args.out.args[0].value = buf;

	return fuse2_simple_request(fc, &args);
}

struct fuse_kvec_ctx {
	struct file *file;
	size_t res;
	loff_t pos;
};

static int fuse_read_kvec(struct kvec *vec, void *_ctx)
{
	struct fuse_kvec_ctx *ctx = _ctx;
	ssize_t res;

	while (vec->iov_len) {
		size_t len = min(vec->iov_len, PAGE_SIZE);

		res = fuse_read_buf(ctx->file, ctx->pos, vec->iov_base, len);
		if (res <= 0)
			return res;

		ctx->res += res;
		ctx->pos += res;

		if (res < len)
			break;

		vec->iov_len -= res;
		vec->iov_base += res;
	}

	return 0;
}

static int fuse_send_map(struct kiocb *iocb, size_t count,
			 struct fuse_map_out *outarg)
{
	struct file *file = iocb->ki_filp;
	struct fuse_conn *fc = get_fuse_conn(file_inode(file));
	struct fuse_file *ff = file->private_data;
	struct fuse_read_in inarg = {
		.fh = ff->fh,
		.offset = iocb->ki_pos,
		.size = count,
		.flags = file->f_flags,
	};
	FUSE_ARGS(args);

	args.in.h.opcode = FUSE_MAP;
	args.in.h.nodeid = ff->nodeid;
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	args.out.numargs = 1;
	args.out.args[0].size = sizeof(*outarg);
	args.out.args[0].value = outarg;

	return fuse2_simple_request(fc, &args);
}

static ssize_t fuse_file_map_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct fuse_conn *fc = get_fuse_conn(file_inode(iocb->ki_filp));
	struct fuse_map_out outarg;
	struct file *mapfile;
	ssize_t res, total = 0;
	size_t count;
	loff_t pos;

	while ((count = iov_iter_count(to))) {
		res = fuse_send_map(iocb, count, &outarg);
		if (res || !outarg.size)
			break;

		res = -EBADF;
		mapfile = fuse2_map_get(fc, outarg.mapfd);
		if (!mapfile)
			break;

		iov_iter_truncate(to, outarg.size);
		pos = outarg.offset;
		res = vfs_iter_read(mapfile, to, &pos, /* FIXME */ 0);
		fput(mapfile);
		if (res < 0)
			break;
		iov_iter_reexpand(to, count - res);
		if (res == 0)
			break;

		total += res;
		iocb->ki_pos += res;
	}

	return total ?: res;
}

static ssize_t fuse_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;
	struct inode *inode = file_inode(file);

	if (is_bad_inode(inode))
		return -EIO;

	if (ff->open_flags & FOPEN_MAP)
		return fuse_file_map_iter(iocb, to);

	if (iov_iter_is_kvec(to)) {
		struct fuse_kvec_ctx ctx = {
			.file = file,
			.pos = iocb->ki_pos
		};
		size_t len = iov_iter_count(to);
		int err;

		err = iov_iter_for_each_range(to, len, fuse_read_kvec, &ctx);
		iocb->ki_pos = ctx.pos;

		if (ctx.res > 0)
			return ctx.res;

		return err;
	}

	return fuse_direct_io(iocb, to, 0);
}

static ssize_t fuse_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	ssize_t res;

	if (is_bad_inode(inode))
		return -EIO;

	/* Don't allow parallel writes to the same file */
	inode_lock(inode);
	res = generic_write_checks(iocb, from);
	if (res > 0)
		res = fuse_direct_io(iocb, from, FUSE_DIO_WRITE);
	if (res > 0)
		fuse_write_update_size(inode, iocb->ki_pos);
	inode_unlock(inode);

	return res;
}

static int fuse_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* Can't provide the coherency needed for MAP_SHARED */
	if (vma->vm_flags & VM_MAYSHARE)
		return -ENODEV;

	invalidate_inode_pages2(file->f_mapping);

	return generic_file_mmap(file, vma);
}

static int convert_fuse_file_lock(struct fuse_conn *fc,
				  const struct fuse_file_lock *ffl,
				  struct file_lock *fl)
{
	switch (ffl->type) {
	case F_UNLCK:
		break;

	case F_RDLCK:
	case F_WRLCK:
		if (ffl->start > OFFSET_MAX || ffl->end > OFFSET_MAX ||
		    ffl->end < ffl->start)
			return -EIO;

		fl->fl_start = ffl->start;
		fl->fl_end = ffl->end;

		/*
		 * Convert pid into init's pid namespace.  The locks API will
		 * translate it into the caller's pid namespace.
		 */
		rcu_read_lock();
		fl->fl_pid = pid_nr_ns(find_pid_ns(ffl->pid, fc->pid_ns), &init_pid_ns);
		rcu_read_unlock();
		break;

	default:
		return -EIO;
	}
	fl->fl_type = ffl->type;
	return 0;
}

static void fuse_lk_fill(struct fuse_args *args, struct file *file,
			 const struct file_lock *fl, int opcode, pid_t pid,
			 int flock, struct fuse_lk_in *inarg)
{
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_file *ff = file->private_data;

	memset(inarg, 0, sizeof(*inarg));
	inarg->fh = ff->fh;
	inarg->owner = fuse2_lock_owner_id(fc, fl->fl_owner);
	inarg->lk.start = fl->fl_start;
	inarg->lk.end = fl->fl_end;
	inarg->lk.type = fl->fl_type;
	inarg->lk.pid = pid;
	if (flock)
		inarg->lk_flags |= FUSE_LK_FLOCK;
	args->in.h.opcode = opcode;
	args->in.h.nodeid = get_node_id(inode);
	args->in.numargs = 1;
	args->in.args[0].size = sizeof(*inarg);
	args->in.args[0].value = inarg;
}

static int fuse_getlk(struct file *file, struct file_lock *fl)
{
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = get_fuse_conn(inode);
	FUSE_ARGS(args);
	struct fuse_lk_in inarg;
	struct fuse_lk_out outarg;
	int err;

	fuse_lk_fill(&args, file, fl, FUSE_GETLK, 0, 0, &inarg);
	args.out.numargs = 1;
	args.out.args[0].size = sizeof(outarg);
	args.out.args[0].value = &outarg;
	err = fuse2_simple_request(fc, &args);
	if (!err)
		err = convert_fuse_file_lock(fc, &outarg.lk, fl);

	return err;
}

static int fuse_setlk(struct file *file, struct file_lock *fl, int flock)
{
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = get_fuse_conn(inode);
	FUSE_ARGS(args);
	struct fuse_lk_in inarg;
	int opcode = (fl->fl_flags & FL_SLEEP) ? FUSE_SETLKW : FUSE_SETLK;
	struct pid *pid = fl->fl_type != F_UNLCK ? task_tgid(current) : NULL;
	pid_t pid_nr = pid_nr_ns(pid, fc->pid_ns);
	int err;

	if (fl->fl_lmops && fl->fl_lmops->lm_grant) {
		/* NLM needs asynchronous locks, which we don't support yet */
		return -ENOLCK;
	}

	/* Unlock on close is handled by the flush method */
	if ((fl->fl_flags & FL_CLOSE_POSIX) == FL_CLOSE_POSIX)
		return 0;

	fuse_lk_fill(&args, file, fl, opcode, pid_nr, flock, &inarg);
	err = fuse2_simple_request(fc, &args);

	/* locking is restartable */
	if (err == -EINTR)
		err = -ERESTARTSYS;

	return err;
}

static int fuse_file_lock(struct file *file, int cmd, struct file_lock *fl)
{
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = get_fuse_conn(inode);
	int err;

	if (cmd == F_CANCELLK) {
		err = 0;
	} else if (cmd == F_GETLK) {
		if (fc->no_lock) {
			posix_test_lock(file, fl);
			err = 0;
		} else
			err = fuse_getlk(file, fl);
	} else {
		if (fc->no_lock)
			err = posix_lock_file(file, fl, NULL);
		else
			err = fuse_setlk(file, fl, 0);
	}
	return err;
}

static int fuse_file_flock(struct file *file, int cmd, struct file_lock *fl)
{
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = get_fuse_conn(inode);
	int err;

	if (fc->no_flock) {
		err = locks_lock_file_wait(file, fl);
	} else {
		struct fuse_file *ff = file->private_data;

		/* emulate flock with POSIX locks */
		ff->flock = true;
		err = fuse_setlk(file, fl, 1);
	}

	return err;
}

static loff_t fuse_lseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_file *ff = file->private_data;
	FUSE_ARGS(args);
	struct fuse_lseek_in inarg = {
		.fh = ff->fh,
		.offset = offset,
		.whence = whence
	};
	struct fuse_lseek_out outarg;
	int err;

	if (fc->no_lseek)
		goto fallback;

	args.in.h.opcode = FUSE_LSEEK;
	args.in.h.nodeid = ff->nodeid;
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	args.out.numargs = 1;
	args.out.args[0].size = sizeof(outarg);
	args.out.args[0].value = &outarg;
	err = fuse2_simple_request(fc, &args);
	if (err) {
		if (err == -ENOSYS) {
			fc->no_lseek = 1;
			goto fallback;
		}
		return err;
	}

	return vfs_setpos(file, outarg.offset, inode->i_sb->s_maxbytes);

fallback:
	err = fuse2_update_attributes(inode, file);
	if (!err)
		return generic_file_llseek(file, offset, whence);
	else
		return err;
}

static loff_t fuse_file_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t retval;
	struct inode *inode = file_inode(file);

	switch (whence) {
	case SEEK_SET:
	case SEEK_CUR:
		 /* No i_mutex protection necessary for SEEK_CUR and SEEK_SET */
		retval = generic_file_llseek(file, offset, whence);
		break;
	case SEEK_END:
		inode_lock(inode);
		retval = fuse2_update_attributes(inode, file);
		if (!retval)
			retval = generic_file_llseek(file, offset, whence);
		inode_unlock(inode);
		break;
	case SEEK_HOLE:
	case SEEK_DATA:
		inode_lock(inode);
		retval = fuse_lseek(file, offset, whence);
		inode_unlock(inode);
		break;
	default:
		retval = -EINVAL;
	}

	return retval;
}

static inline loff_t fuse_round_up(struct fuse_conn *fc, loff_t off)
{
	return round_up(off, fc->max_pages << PAGE_SHIFT);
}

static long fuse_file_fallocate(struct file *file, int mode, loff_t offset,
				loff_t length)
{
	struct fuse_file *ff = file->private_data;
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = ff->fc;
	FUSE_ARGS(args);
	struct fuse_fallocate_in inarg = {
		.fh = ff->fh,
		.offset = offset,
		.length = length,
		.mode = mode
	};
	int err;
	bool lock_inode = !(mode & FALLOC_FL_KEEP_SIZE) ||
			   (mode & FALLOC_FL_PUNCH_HOLE);

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	if (fc->no_fallocate)
		return -EOPNOTSUPP;

	if (lock_inode) {
		inode_lock(inode);
		if (mode & FALLOC_FL_PUNCH_HOLE) {
			loff_t endbyte = offset + length - 1;
			err = filemap_write_and_wait_range(inode->i_mapping,
							   offset, endbyte);
			if (err)
				goto out;
		}
	}

	args.in.h.opcode = FUSE_FALLOCATE;
	args.in.h.nodeid = ff->nodeid;
	args.in.numargs = 1;
	args.in.args[0].size = sizeof(inarg);
	args.in.args[0].value = &inarg;
	err = fuse2_simple_request(fc, &args);
	if (err == -ENOSYS) {
		fc->no_fallocate = 1;
		err = -EOPNOTSUPP;
	}
	if (err)
		goto out;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		truncate_pagecache_range(inode, offset, offset + length - 1);
out:
	if (lock_inode)
		inode_unlock(inode);

	return err;
}

static const struct file_operations fuse_file_operations = {
	.llseek		= fuse_file_llseek,
	.read_iter	= fuse_file_read_iter,
	.write_iter	= fuse_file_write_iter,
	.mmap		= fuse_file_mmap,
	.open		= fuse_open,
	.flush		= fuse_flush,
	.release	= fuse_release,
	.fsync		= fuse_fsync,
	.lock		= fuse_file_lock,
	.flock		= fuse_file_flock,
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= fuse_file_fallocate,
};

static const struct address_space_operations fuse_file_aops  = {
	.readpage	= fuse_readpage,
};

void fuse2_init_file_inode(struct inode *inode)
{
	inode->i_fop = &fuse_file_operations;
	inode->i_data.a_ops = &fuse_file_aops;
}
