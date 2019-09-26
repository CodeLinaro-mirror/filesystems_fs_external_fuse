/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2018  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/


#include "fuse_i.h"
#include <linux/iversion.h>
#include <linux/posix_acl.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>

static bool fuse_emit(struct file *file, struct dir_context *ctx,
		      struct fuse_dirent *dirent)
{
	return dir_emit(ctx, dirent->name, dirent->namelen, dirent->ino,
			dirent->type);
}

static int parse_dirfile(char *buf, size_t nbytes, struct file *file,
			 struct dir_context *ctx)
{
	while (nbytes >= FUSE_NAME_OFFSET) {
		struct fuse_dirent *dirent = (struct fuse_dirent *) buf;
		size_t reclen = FUSE_DIRENT_SIZE(dirent);
		if (!dirent->namelen || dirent->namelen > FUSE_NAME_MAX)
			return -EIO;
		if (reclen > nbytes)
			break;
		if (memchr(dirent->name, '/', dirent->namelen) != NULL)
			return -EIO;

		if (!fuse_emit(file, ctx, dirent))
			break;

		buf += reclen;
		nbytes -= reclen;
		ctx->pos = dirent->off;
	}

	return 0;
}

static int fuse2_readdir_uncached(struct file *file, struct dir_context *ctx)
{
	int err;
	size_t nbytes;
	struct page *page;
	struct inode *inode = file_inode(file);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_req *req;
	struct fuse_read_in *inarg;

	req = fuse2_get_req(fc, 1);
	if (IS_ERR(req))
		return PTR_ERR(req);

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		fuse2_put_request(req);
		return -ENOMEM;
	}

	req->num_pages = 1;
	req->pages[0] = page;
	req->page_descs[0].length = PAGE_SIZE;
	fuse2_read_fill(req, &inarg, file, ctx->pos, PAGE_SIZE, FUSE_READDIR);
	err = fuse2_request_send(fc, req);
	nbytes = req->outh.len - req->mand_outlen;
	if (!err && nbytes)
		err = parse_dirfile(page_address(page), nbytes, file, ctx);

	fuse2_put_request(req);

	return err;
}

int fuse2_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);

	if (is_bad_inode(inode))
		return -EIO;

	return fuse2_readdir_uncached(file, ctx);
}
