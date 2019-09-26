/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2019  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/mm.h>
#include <linux/uio.h>
#include <linux/cred.h>

static struct kmem_cache *fuse_req_cachep;

int fuse2_request_send(struct fuse_conn *fc, struct fuse_req *req)
{
	return fc->dev_ops->send(fc->dev_priv, req);
}

static void fuse_request_init(struct fuse_req *req, struct page **pages,
			      struct fuse_page_desc *page_descs,
			      unsigned npages)
{
	INIT_LIST_HEAD(&req->list);
	init_waitqueue_head(&req->waitq);
	refcount_set(&req->count, 1);
	req->pages = pages;
	req->page_descs = page_descs;
	req->max_pages = npages;
	__set_bit(FR_PENDING, &req->flags);
	__set_bit(FR_ISREPLY, &req->flags);
}

static struct page **fuse_req_pages_alloc(unsigned int npages, gfp_t flags,
					  struct fuse_page_desc **desc)
{
	struct page **pages;

	pages = kzalloc(npages * (sizeof(struct page *) +
				  sizeof(struct fuse_page_desc)), flags);
	*desc = (void *) pages + npages * sizeof(struct page *);

	return pages;
}

static struct fuse_req *fuse_request_alloc(unsigned npages, gfp_t flags)
{
	struct fuse_req *req = kmem_cache_zalloc(fuse_req_cachep, flags);
	if (req) {
		struct page **pages = NULL;
		struct fuse_page_desc *page_descs = NULL;

		WARN_ON(npages > FUSE_MAX_MAX_PAGES);
		if (npages > FUSE_REQ_INLINE_PAGES) {
			pages = fuse_req_pages_alloc(npages, flags,
						     &page_descs);
			if (!pages) {
				kmem_cache_free(fuse_req_cachep, req);
				return NULL;
			}
		} else if (npages) {
			pages = req->inline_pages;
			page_descs = req->inline_page_descs;
		}

		fuse_request_init(req, pages, page_descs, npages);
	}
	return req;
}

static void fuse_request_free(struct fuse_req *req)
{
	unsigned int i;

	for (i = 0; i < req->num_pages; i++)
		put_page(req->pages[i]);

	if (req->pages != req->inline_pages)
		kfree(req->pages);
	kmem_cache_free(fuse_req_cachep, req);
}

static struct fuse_req *fuse2_get_req_gfp(struct fuse_conn *fc, unsigned npages,
					 gfp_t flags)
{
	struct fuse_req *req;
	int err;

	req = fuse_request_alloc(npages, flags);
	err = -ENOMEM;
	if (!req)
		goto out;

	req->inh.uid = from_kuid(fc->user_ns, current_fsuid());
	req->inh.gid = from_kgid(fc->user_ns, current_fsgid());
	req->inh.pid = pid_nr_ns(task_pid(current), fc->pid_ns);

	if (unlikely(req->inh.uid == ((uid_t)-1) ||
		     req->inh.gid == ((gid_t)-1))) {
		fuse2_put_request(req);
		return ERR_PTR(-EOVERFLOW);
	}
	return req;

 out:
	return ERR_PTR(err);
}

struct fuse_req *fuse2_get_req(struct fuse_conn *fc, unsigned npages)
{
	return fuse2_get_req_gfp(fc, npages, GFP_KERNEL);
}

struct fuse_forget *fuse2_alloc_forget(void)
{
	return kzalloc(sizeof(struct fuse_forget), GFP_KERNEL);
}

void fuse2_get_request(struct fuse_req *req)
{
	refcount_inc(&req->count);
}
EXPORT_SYMBOL(fuse2_get_request);

void fuse2_put_request(struct fuse_req *req)
{
	if (refcount_dec_and_test(&req->count))
		fuse_request_free(req);
}
EXPORT_SYMBOL(fuse2_put_request);

static unsigned len_args(unsigned numargs, struct fuse_arg *args)
{
	unsigned nbytes = 0;
	unsigned i;

	for (i = 0; i < numargs; i++)
		nbytes += args[i].size;

	return nbytes;
}

static int fuse_req_pages_fill(struct fuse_req *req, gfp_t flags)
{
	unsigned int i;

	for (i = 0; i < req->max_pages; i++) {
		struct page *page = alloc_page(flags);
		if (!page)
			return -ENOMEM;

		req->pages[i] = page;
		req->page_descs[i].length = PAGE_SIZE;
		req->num_pages++;
	}
	return 0;
}

#define FUSE_SIMPLE_MAX_PAGES 17

static ssize_t fuse2_simple_send(struct fuse_conn *fc, struct fuse_args *args)
{
	struct fuse_req *req;
	ssize_t ret;
	gfp_t flags = GFP_KERNEL;
	unsigned int inlen = sizeof(struct fuse_in_header) +
		len_args(args->in.numargs, (struct fuse_arg *) args->in.args);
	unsigned int outlen = sizeof(struct fuse_out_header) +
		len_args(args->out.numargs, args->out.args);
	unsigned int maxlen = maxlen = max(inlen, outlen);
	unsigned int npages = 0;
	struct kvec vec[FUSE_SIMPLE_MAX_PAGES + 1];
	struct iov_iter iter;
	unsigned int i;

	if (maxlen > FUSE_REQ_INLINE_DATA) {
		npages = DIV_ROUND_UP(maxlen - FUSE_REQ_INLINE_DATA, PAGE_SIZE);
		if (WARN_ON(npages > FUSE_SIMPLE_MAX_PAGES))
			return -EIO;
	}

	if (args->force)
		flags |= __GFP_NOFAIL;

	req = fuse2_get_req_gfp(fc, npages, flags);
	if (IS_ERR(req))
		return PTR_ERR(req);

	vec[0].iov_base = req->inlinedata;
	vec[0].iov_len = FUSE_REQ_INLINE_DATA;
	if (npages) {
		ret = fuse_req_pages_fill(req, flags);
		if (ret)
			goto out_put;

		for (i = 0; i < npages; i++) {
			vec[i + 1].iov_base = page_address(req->pages[i]);
			vec[i + 1].iov_len = req->page_descs[i].length;
		}
	}

	iov_iter_kvec(&iter, READ, vec, npages + 1, inlen);
	iov_iter_advance(&iter, sizeof(struct fuse_in_header));
	for (i = 0; i < args->in.numargs; i++) {
		struct fuse_in_arg *a = &args->in.args[i];

		ret = _copy_to_iter(a->value, a->size, &iter);
		WARN_ON(ret != a->size);
	}
	req->inline_inlen = min_t(unsigned int, inlen, FUSE_REQ_INLINE_DATA);
	req->inh.opcode = args->in.h.opcode;
	req->inh.nodeid = args->in.h.nodeid;

	req->inline_outlen = min_t(unsigned int, outlen, FUSE_REQ_INLINE_DATA);
	req->max_outlen = req->mand_outlen = outlen;
	if (args->out.argvar)
		req->mand_outlen = sizeof(struct fuse_out_header);

	if (args->force)
		__set_bit(FR_FORCE, &req->flags);
	if (args->killable)
		__set_bit(FR_KILLABLE, &req->flags);
	req->inh.len = inlen;
	ret = fuse2_request_send(fc, req);
	if (ret)
		goto out_put;

	iov_iter_kvec(&iter, WRITE, vec, npages + 1, req->outh.len);
	iov_iter_advance(&iter, sizeof(struct fuse_out_header));
	for (i = 0; i < args->out.numargs; i++) {
		struct fuse_arg *a = &args->out.args[i];

		if (a->value)
			_copy_from_iter(a->value, a->size, &iter);
		else
			iov_iter_advance(&iter, a->size);
	}
	if (args->out.argvar)
		ret = req->outh.len - req->mand_outlen;

out_put:
	fuse2_put_request(req);

	return ret;
}

ssize_t fuse2_simple_request(struct fuse_conn *fc, struct fuse_args *args)
{
	ssize_t res;
	uid_t uid = from_kuid(fc->user_ns, current_fsuid());
	gid_t gid = from_kgid(fc->user_ns, current_fsgid());
	pid_t pid = pid_nr_ns(task_pid(current), fc->pid_ns);

	if (unlikely(uid == ((uid_t)-1) || gid == ((gid_t)-1)))
		return -EOVERFLOW;

	if (WARN_ON(args->out.argvar && args->out.numargs != 1))
		return -EIO;

	if (fc->dev_ops->simple_send) {
			res = fc->dev_ops->simple_send(fc->dev_priv, args,
					       uid, gid, pid);
		if (res != -EPROBE_DEFER)
			return res;
	}

	return fuse2_simple_send(fc, args);
}

void fuse2_queue_forget(struct fuse_conn *fc, struct fuse_forget *forget,
		       u64 nodeid, u64 nlookup)
{
	forget->forget_one.nodeid = nodeid;
	forget->forget_one.nlookup = nlookup;

	fc->dev_ops->forget(fc->dev_priv, forget);
}

void fuse2_force_forget(struct fuse_conn *fc, u64 nodeid)
{
	struct fuse_forget *forget;

	forget = kzalloc(sizeof(struct fuse_forget), GFP_KERNEL | __GFP_NOFAIL);
	fuse2_queue_forget(fc, forget, nodeid, 1);
}

int __init fuse_req_cache_init(void)
{
	fuse_req_cachep = kmem_cache_create("fuse2_request",
					    sizeof(struct fuse_req),
					    0, 0, NULL);
	if (!fuse_req_cachep)
		return -ENOMEM;

	return 0;
}

void fuse_req_cache_cleanup(void)
{
	kmem_cache_destroy(fuse_req_cachep);
}
