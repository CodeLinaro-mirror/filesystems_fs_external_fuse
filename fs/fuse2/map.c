/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/file.h>
#include <linux/idr.h>

static DEFINE_SPINLOCK(fuse2_map_lock);
static DEFINE_IDR(fuse2_map);

int fuse2_map_open(struct super_block *sb, struct file *file)
{
	int res;

	idr_preload(GFP_KERNEL);
	spin_lock(&fuse2_map_lock);
	res = idr_alloc(&fuse2_map, file, 0, 0, GFP_ATOMIC);
	spin_unlock(&fuse2_map_lock);
	idr_preload_end();
	if (res)
		fput(file);

	return res;
}
EXPORT_SYMBOL(fuse2_map_open);

int fuse2_map_close(struct super_block *sb, unsigned long mapfd)
{
	struct file *file;

	spin_lock(&fuse2_map_lock);
	file = idr_remove(&fuse2_map, mapfd);
	spin_unlock(&fuse2_map_lock);

	if (!file)
		return -EBADF;

	fput(file);
	return 0;
}
EXPORT_SYMBOL(fuse2_map_close);

struct file *fuse2_map_get(struct fuse_conn *fc, u64 mapfd)
{
	struct file *file;

	rcu_read_lock();
	file = idr_find(&fuse2_map, mapfd);
	if (file)
		get_file(file);
	rcu_read_unlock();

	return file;
}
