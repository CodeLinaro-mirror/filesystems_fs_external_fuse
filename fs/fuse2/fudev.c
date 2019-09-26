/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "dev.h"

#include <linux/module.h>
#include <linux/uio.h>
#include <linux/miscdevice.h>
#include <linux/highmem.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/cred.h>
#include <linux/parser.h>

MODULE_AUTHOR("Miklos Szeredi <miklos@szeredi.hu>");
MODULE_DESCRIPTION("Filesystem in Userspace");
MODULE_LICENSE("GPL");

MODULE_ALIAS_MISCDEV(FUSE_MINOR);
MODULE_ALIAS("devname:fuse");

/* Ordinary requests have even IDs, while interrupts IDs are odd */
#define FUSE_INT_REQ_BIT 1
#define FUSE_REQ_ID_STEP (2 * (nr_cpumask_bits + 1))

static DEFINE_MUTEX(fudev_mutex);

struct fuse_iqueue {
	/* Connection established */
	unsigned connected;

	/* Readers of the connection are waiting on this */
	wait_queue_head_t waitq;

	/* The next unique request id */
	u64 reqctr;

	/* The list of pending requests */
	struct list_head pending;

	/* Queue of pending forgets */
	struct list_head forgets;

	/* Batching of FORGET requests (positive indicates FORGET batch) */
	int forget_batch;
};

#define FUSE_PQ_HASH_BITS 8
#define FUSE_PQ_HASH_SIZE (1 << FUSE_PQ_HASH_BITS)

struct fuse_pqueue {
	/* Connection established */
	unsigned connected;

	/* Lock protecting accessess to  members of this structure */
	spinlock_t lock;

	/* Hash table of requests being processed */
	struct list_head *processing;
};

/*
 * Fuse device instance
 */
struct fudev {
	/* refcount */
	refcount_t count;

	/* Processing queue */
	struct fuse_pqueue pq;

	/* Input queue */
	struct fuse_iqueue iq;

	/* Per-cpu mapped buffers */
	struct fudev_aux * __percpu *aux_devs;

	/* Our super block */
	struct super_block *sb;
};

struct fudev_aux {
	/* Fuse device this is bound to or NULL */
	struct fudev *dev;

	/* Request (if exists) */
	struct fuse_req *req;

	/* Memory mapped input buffer */
	void *ibuf;

	/* Size of input buffer */
	size_t ibufsize;

	/* Memory mapped output buffer */
	void *obuf;

	/* Size of output buffer */
	size_t obufsize;

	unsigned int cpu;

	u64 reqctr;

	int reserved;

	int ready;

	int done;

	wait_queue_head_t waitq;

};

static const struct file_operations fudev_operations;
static const struct file_operations fudev_aux_operations;

static struct fudev *fudev(struct file *file)
{
	BUG_ON(file->f_op != &fudev_operations);
	return file->private_data;
}

static struct fudev_aux *fudev_aux(struct file *file)
{
	BUG_ON(file->f_op != &fudev_aux_operations);
	return file->private_data;
}

static u64 fuse_get_unique(struct fuse_iqueue *fiq)
{
	fiq->reqctr += FUSE_REQ_ID_STEP;
	return fiq->reqctr;
}

static unsigned int fuse_req_hash(u64 unique)
{
	return hash_long(unique & ~FUSE_INT_REQ_BIT, FUSE_PQ_HASH_BITS);
}

static int fuse_read_pages(struct iov_iter *to, struct fuse_req *req,
			   unsigned int nbytes)
{
	unsigned int i;

	for (i = 0; i < req->num_pages && nbytes; i++) {
		struct page *page = req->pages[i];
		unsigned int offset = req->page_descs[i].offset;
		unsigned int count = min(nbytes, req->page_descs[i].length);

		if (copy_page_to_iter(page, offset, count, to) != count)
			return -EFAULT;

		nbytes -= count;
	}
	return 0;
}

static int fuse_read_one(struct iov_iter *iter, void *val, unsigned size)
{
	return _copy_to_iter(val, size, iter) == size ? 0 : -EFAULT;
}

static bool forget_pending(struct fuse_iqueue *fiq)
{
	return !list_empty(&fiq->forgets);
}

static bool request_pending(struct fuse_iqueue *fiq)
{
	return !list_empty(&fiq->pending) || forget_pending(fiq);
}

static void fuse_free_forgets(struct list_head *head)
{
	struct fuse_forget *forget, *next;

	list_for_each_entry_safe(forget, next, head, list) {
		list_del(&forget->list);
		kfree(forget);
	}
}
static ssize_t fuse_read_forget(struct fuse_iqueue *fiq, struct iov_iter *to)
__releases(fiq->waitq.lock)
{
	int err;
	const size_t one_len = sizeof(struct fuse_forget_one);
	unsigned int count, max_forgets;
	struct fuse_forget *forget;
	size_t nbytes = iov_iter_count(to);
	struct list_head head, *last;
	struct fuse_batch_forget_in arg = { .count = 0 };
	struct fuse_in_header ih = {
		.opcode = FUSE_BATCH_FORGET,
		.unique = fuse_get_unique(fiq),
		.len = sizeof(ih) + sizeof(arg),
	};

	if (nbytes < ih.len) {
		spin_unlock(&fiq->waitq.lock);
		return -EINVAL;
	}

	max_forgets = (nbytes - ih.len) / one_len;

	for (count = 0, last = &fiq->forgets;
	     count < max_forgets && last->next != &fiq->forgets;
	     count++, last = last->next);

	list_cut_position(&head, &fiq->forgets, last);
	spin_unlock(&fiq->waitq.lock);

	arg.count = count;
	ih.len += count * one_len;
	err = fuse_read_one(to, &ih, sizeof(ih));
	if (!err)
		err = fuse_read_one(to, &arg, sizeof(arg));

	if (!err) {
		list_for_each_entry(forget, &head, list) {
			err = fuse_read_one(to, &forget->forget_one, one_len);
			if (err)
				break;
		}
	}
	fuse_free_forgets(&head);

	if (err)
		return err;

	return ih.len;
}

/*
 * This function is called when a request is finished.  Either a reply
 * has arrived or it was aborted (and not yet sent) or some error
 * occurred during communication with userspace, or the device file
 * was closed.  The requester thread is woken up (if still waiting),
 * the 'end' callback is called if given, else the reference to the
 * request is released
 */
static void request_end(struct fuse_req *req)
{
	if (!test_and_set_bit(FR_FINISHED, &req->flags)) {
		/* Wake up waiter sleeping in request_wait_answer() */
		wake_up(&req->waitq);
	}
}

static void fuse_io_end(struct fuse_pqueue *fpq, struct fuse_req *req, int err)
{
	if (err) {
		spin_lock(&fpq->lock);
		if (fpq->connected)
			req->outh.error = -EIO;
		spin_unlock(&fpq->lock);
	}
	request_end(req);

	spin_lock(&fpq->lock);
	list_del_init(&req->list);
	spin_unlock(&fpq->lock);

	fuse2_put_request(req);
}


/* Look up request on processing list by unique ID */
static struct fuse_req *request_find(struct fuse_pqueue *fpq, u64 unique)
{
	unsigned int hash = fuse_req_hash(unique);
	struct fuse_req *req;

	list_for_each_entry(req, &fpq->processing[hash], list) {
		if (req->inh.unique == unique)
			return req;
	}
	return NULL;
}

static int copy_out_args(struct iov_iter *from, struct fuse_req *req)
{
	bool zeroing = test_bit(FR_ZEROTAIL, &req->flags);
	unsigned int nbytes = iov_iter_count(from);
	const unsigned int hlen = sizeof(struct fuse_out_header);
	unsigned int thislen = req->inline_outlen - hlen;
	void *arg = req->inlinedata + hlen;
	unsigned int i;
	size_t ret;

	if (req->outh.error)
		return nbytes > 0 ? -EINVAL : 0;

	if (nbytes < req->mand_outlen - hlen || nbytes > req->max_outlen - hlen)
		return -EINVAL;

	thislen = min(thislen, nbytes);

	ret = _copy_from_iter(arg, thislen, from);
	if (ret != thislen)
		return -EFAULT;

	nbytes -= thislen;
	for (i = 0; i < req->num_pages; i++) {
		unsigned offset = req->page_descs[i].offset;
		unsigned count = min(nbytes, req->page_descs[i].length);
		struct page *page = req->pages[i];

		if (zeroing && count < PAGE_SIZE)
			clear_highpage(page);

		if (!count) {
			if (!zeroing)
				break;

			flush_dcache_page(page);
			continue;
		}
		ret = copy_page_from_iter(page, offset, count, from);
		if (ret != count)
			return -EFAULT;

		flush_dcache_page(page);
		nbytes -= count;
	}
	WARN_ON(nbytes);

	return 0;
}

/*
 * Read a single request into the userspace filesystem's buffer.  This
 * function waits until a request is available, then removes it from
 * the pending list and copies request data to userspace buffer.  If
 * no reply is needed (FORGET) or request has been aborted or there
 * was an error during the copying then it's finished by calling
 * request_end().  Otherwise add it to the processing list, and set
 * the 'sent' flag.
 */
static ssize_t fudev_read(struct kiocb *iocb, struct iov_iter *to)
{
	ssize_t err;
	struct file *file = iocb->ki_filp;
	struct fudev *fud = fudev(file);
	struct fuse_iqueue *fiq = &fud->iq;
	struct fuse_pqueue *fpq = &fud->pq;
	struct fuse_req *req;
	unsigned int reqsize, hash;

restart:
	spin_lock(&fiq->waitq.lock);
	err = -EAGAIN;
	if ((file->f_flags & O_NONBLOCK) && fiq->connected &&
	    !request_pending(fiq))
		goto err_unlock;

	err = wait_event_interruptible_exclusive_locked(fiq->waitq,
				!fiq->connected || request_pending(fiq));
	if (err)
		goto err_unlock;

	if (!fiq->connected) {
		err = -ENODEV;
		goto err_unlock;
	}

	if (forget_pending(fiq)) {
		if (list_empty(&fiq->pending) || fiq->forget_batch-- > 0)
			return fuse_read_forget(fiq, to);

		if (fiq->forget_batch <= -8)
			fiq->forget_batch = 16;
	}

	req = list_entry(fiq->pending.next, struct fuse_req, list);
	clear_bit(FR_PENDING, &req->flags);

	hash = fuse_req_hash(req->inh.unique);
	spin_lock(&fpq->lock);
	list_move_tail(&req->list, &fpq->processing[hash]);
	spin_unlock(&fpq->lock);

	spin_unlock(&fiq->waitq.lock);

	reqsize = req->inh.len;

	/* If request is too large, reply with an error and restart the read */
	if (iov_iter_count(to) < reqsize) {
		req->outh.error = -EIO;
		/* SETXATTR is special, since it may contain too large data */
		if (req->inh.opcode == FUSE_SETXATTR)
			req->outh.error = -E2BIG;
		request_end(req);
		fuse2_put_request(req);
		goto restart;
	}


	err = fuse_read_one(to, req->inlinedata, req->inline_inlen);
	if (!err && reqsize > req->inline_inlen)
		err = fuse_read_pages(to, req, reqsize - req->inline_inlen);

	if (!err && test_bit(FR_ISREPLY, &req->flags)) {
		set_bit(FR_SENT, &req->flags);
		return reqsize;
	}

	fuse_io_end(fpq, req, err);
	if (err)
		return err;
	return reqsize;

err_unlock:
	spin_unlock(&fiq->waitq.lock);
	return err;
}

/*
 * Write a single reply to a request.  First the header is copied from
 * the write buffer.  The request is then searched on the processing
 * list by the unique ID found in the header.  If found, then remove
 * it from the list and copy the rest of the buffer to the request.
 * The request is finished by calling request_end()
 */
static ssize_t fudev_write(struct kiocb *iocb, struct iov_iter *from)
{
	int err;
	unsigned int nbytes = iov_iter_count(from);
	struct fudev *fud = fudev(iocb->ki_filp);
	struct fuse_pqueue *fpq = &fud->pq;
	struct fuse_req *req;
	struct fuse_out_header oh;

	if (nbytes < sizeof(struct fuse_out_header))
		return -EINVAL;

	if (_copy_from_iter(&oh, sizeof(oh), from) != sizeof(oh))
		return -EFAULT;

	if (oh.len != nbytes)
		return -EINVAL;

	/*
	 * Zero oh.unique indicates unsolicited notification message
	 * and error contains notification code.
	 */
	if (!oh.unique)
		return -EINVAL;

	if (oh.error <= -1000 || oh.error > 0)
		return -EINVAL;

	spin_lock(&fpq->lock);
	req = request_find(fpq, oh.unique & ~FUSE_INT_REQ_BIT);
	if (!req || !test_bit(FR_SENT, &req->flags)) {
		spin_unlock(&fpq->lock);
		return -ENOENT;
	}
	clear_bit(FR_SENT, &req->flags);
	req->outh = oh;
	spin_unlock(&fpq->lock);

	err = copy_out_args(from, req);

	fuse_io_end(fpq, req, err);
	if (err)
		return err;
	return nbytes;
}

static void fuse_pqueue_init(struct fuse_pqueue *fpq)
{
	unsigned int i;

	spin_lock_init(&fpq->lock);
	for (i = 0; i < FUSE_PQ_HASH_SIZE; i++)
		INIT_LIST_HEAD(&fpq->processing[i]);
	fpq->connected = 1;
}

static void fuse_iqueue_init(struct fuse_iqueue *fiq)
{
	init_waitqueue_head(&fiq->waitq);
	INIT_LIST_HEAD(&fiq->pending);
	INIT_LIST_HEAD(&fiq->forgets);
	fiq->connected = 1;
}

static void fudev_put(void *priv)
{
	struct fudev *fud = priv;

	if (refcount_dec_and_test(&fud->count)) {
		kfree(fud->pq.processing);
		kfree(fud);
	}
}

static int fudev_open(struct inode *inode, struct file *file)
{
	struct fudev *fud;
	struct list_head *pq;

	fud = kzalloc(sizeof(struct fudev), GFP_KERNEL);
	if (!fud)
		goto nomem;

	pq = kcalloc(FUSE_PQ_HASH_SIZE, sizeof(struct list_head), GFP_KERNEL);
	if (!pq)
		goto nomem;

	refcount_set(&fud->count, 1);
	fud->pq.processing = pq;
	fuse_pqueue_init(&fud->pq);
	fuse_iqueue_init(&fud->iq);

	file->private_data = fud;

	return 0;

nomem:
	kfree(fud);
	return -ENOMEM;
}

static __poll_t fudev_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;
	struct fudev *fud = fudev(file);
	struct fuse_iqueue *fiq = &fud->iq;

	poll_wait(file, &fiq->waitq, wait);

	spin_lock(&fiq->waitq.lock);
	if (!fiq->connected)
		mask = EPOLLERR;
	else if (request_pending(fiq))
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock(&fiq->waitq.lock);

	return mask;
}

/* Abort all requests on the given list (pending or processing) */
static void end_requests(struct list_head *head, spinlock_t *lock)
{
	struct fuse_req *req;

	while (!list_empty(head)) {
		req = list_entry(head->next, struct fuse_req, list);
		list_del_init(&req->list);
		req->outh.error = -ECONNABORTED;
		clear_bit(FR_PENDING, &req->flags);
		fuse2_get_request(req);
		spin_unlock(lock);
		request_end(req);
		fuse2_put_request(req);
		spin_lock(lock);
	}
}

static void fudev_abort(void *priv)
{
	struct fudev *fud = priv;
	struct fuse_iqueue *fiq;
	struct fuse_pqueue *fpq;
	unsigned int i;

	fpq = &fud->pq;
	spin_lock(&fpq->lock);
	fpq->connected = 0;
	for (i = 0; i < FUSE_PQ_HASH_SIZE; i++)
		end_requests(&fpq->processing[i], &fpq->lock);
	spin_unlock(&fpq->lock);

	fiq = &fud->iq;
	spin_lock(&fiq->waitq.lock);
	fiq->connected = 0;

	end_requests(&fiq->pending, &fiq->waitq.lock);
	fuse_free_forgets(&fiq->forgets);
	wake_up_all_locked(&fiq->waitq);
	spin_unlock(&fiq->waitq.lock);
}

static int fudev_release(struct inode *inode, struct file *file)
{
	struct fudev *fud = fudev(file);

	fudev_abort(fud);
	fudev_put(fud);

	return 0;
}

static long fudev_map_ioctl(struct fudev *fud, unsigned int cmd,
			    unsigned long arg)
{
	struct file *file;

	switch (cmd) {
	case FUSE2_DEV_IOC_MAP_OPEN:
		file = fget(arg);
		if (!file)
			return -EBADF;
		return fuse2_map_open(fud->sb, file);

	case FUSE2_DEV_IOC_MAP_CLOSE:
		return fuse2_map_close(fud->sb, arg);

	default:
		return -ENOTTY;
	}

}

static long fudev_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	return fudev_map_ioctl(fudev(file), cmd, arg);
}

static const struct file_operations fudev_operations = {
	.owner		= THIS_MODULE,
	.open		= fudev_open,
	.llseek		= no_llseek,
	.read_iter	= fudev_read,
	.write_iter	= fudev_write,
	.poll		= fudev_poll,
	.release	= fudev_release,
	.unlocked_ioctl	= fudev_ioctl,
};


static int fudev_aux_open(struct inode *inode, struct file *file)
{
	struct fudev_aux *fux;

	fux = kzalloc(sizeof(struct fudev_aux), GFP_KERNEL);
	if (!fux)
		return -ENOMEM;

	init_waitqueue_head(&fux->waitq);
	file->private_data = fux;

	return 0;
}

static long fudev_aux_bind(struct file *file, unsigned long arg)
{
	int err;
	int oldfd;
	struct file *old;
	struct fudev *fud;
	struct fudev_aux *fux = fudev_aux(file);
	const struct cpumask *cpumask = current->cpus_ptr;

	if (get_user(oldfd, (__u32 __user *) arg) != 0)
		return -EFAULT;

	old = fget(oldfd);
	if (!old)
		return -EINVAL;

	err = -EINVAL;
	if (old->f_op != &fudev_operations ||
	    old->f_cred->user_ns != file->f_cred->user_ns)
		goto out_put_old;

	fud = fudev(old);

	mutex_lock(&fudev_mutex);
	err = -EINVAL;
	if (fux->dev)
		goto out_unlock;

	if (cpumask_weight(cpumask) == 1) {
		unsigned int cpu;

		if (!fud->aux_devs) {
			fud->aux_devs = alloc_percpu(struct fudev_aux *);
			err = -ENOMEM;
			if (!fud->aux_devs)
				goto out_unlock;
		}
		cpu = get_cpu();
		fux->cpu = cpu;
		fux->reqctr = cpu + 1;
		pr_info("...binding to cpu %u\n", cpu);
		WARN_ON(cpu != cpumask_first(cpumask));
		*per_cpu_ptr(fud->aux_devs, cpu) = fux;
		put_cpu();
	}

	refcount_inc(&fud->count);
	fux->dev = fud;
	err = 0;

out_unlock:
	mutex_unlock(&fudev_mutex);
out_put_old:
	fput(old);

	return err;
}

static inline void fudev_barrier(void)
{
	/* FIXME: ??? */
	barrier();
}

static void fudev_req_done(struct fudev_aux *fux)
{
	fux->ready = 0;
	fudev_barrier();
	fux->done = 1;
	wake_up(&fux->waitq);
}

static long fudev_aux_proc(struct file *file, unsigned int cmd)
{
	struct fudev_aux *fux = fudev_aux(file);
	int ret;

	if (cpumask_weight(current->cpus_ptr) != 1 ||
	    fux->cpu != smp_processor_id())
		return -EINVAL;

	if (cmd == FUSE2_DEV_IOC_PROC) {
		fudev_req_done(fux);
	}
	ret = wait_event_interruptible(fux->waitq, fux->ready);
	if (ret) {
		/* Must report success of write! */
		if (cmd == FUSE2_DEV_IOC_PROC)
			return 0;
		return ret;
	}

	return ((struct fuse_in_header *) fux->ibuf)->len;
}

static long fudev_aux_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	switch (cmd) {
	case FUSE2_DEV_IOC_BIND:
		return fudev_aux_bind(file, arg);

	case FUSE2_DEV_IOC_PROC:
	case FUSE2_DEV_IOC_READ:
		return fudev_aux_proc(file, cmd);

	case FUSE2_DEV_IOC_MAP_OPEN:
	case FUSE2_DEV_IOC_MAP_CLOSE:
		return fudev_map_ioctl(fudev_aux(file)->dev, cmd, arg);

	default:
		return -ENOTTY;
	}
}

static int fudev_aux_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct fudev_aux *fux = fudev_aux(file);
	unsigned long length = vma->vm_end - vma->vm_start;
	void **bufp;
	size_t *bufsizep;
 	unsigned int off;
	int err;

	if (length != 0x2000 ||
	    (vma->vm_pgoff != (FUSE2_MMAP_INBUF_OFFSET >> PAGE_SHIFT) &&
	     vma->vm_pgoff != (FUSE2_MMAP_OUTBUF_OFFSET >> PAGE_SHIFT)))
		return -EINVAL;

	if (vma->vm_pgoff == (FUSE2_MMAP_INBUF_OFFSET >> PAGE_SHIFT)) {
		bufp = &fux->ibuf;
		bufsizep = &fux->ibufsize;
	} else {
		bufp = &fux->obuf;
		bufsizep = &fux->obufsize;
	}

	if (!*bufp) {
		*bufp = (void *)
			__get_free_pages(GFP_KERNEL | __GFP_COMP |__GFP_ZERO,
					 get_order(length));
		if (!*bufp)
			return -ENOMEM;
		*bufsizep = length;
	}

	for (off = 0; off < length; off += PAGE_SIZE) {
		struct page *page = virt_to_page(*bufp + off);

		err = vm_insert_page(vma, vma->vm_start + off, page);
		if (err)
			return err;
	}

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

	return 0;
}

static void fudev_aux_remove(void *info)
{
	struct fudev_aux *fux = info;
	struct fudev *fud = fux->dev;

	*per_cpu_ptr(fud->aux_devs, fux->cpu) = NULL;
}

static int fudev_aux_release(struct inode *inode, struct file *file)
{
	struct fudev_aux *fux = fudev_aux(file);
	struct fudev *fud = fux->dev;
	int err;

	if (fud) {
		/* FIXME: synchronize with fudev_simple_send() */
		err = smp_call_function_single(fux->cpu, fudev_aux_remove,
					       fux, true);
		if (err)
			fudev_aux_remove(fux);

		fudev_put(fud);
	}

	free_pages((unsigned long) fux->ibuf, get_order(fux->ibufsize));
	free_pages((unsigned long) fux->obuf, get_order(fux->obufsize));

	kfree(fux);

	return 0;
}

static ssize_t fudev_aux_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct fudev_aux *fux = fudev_aux(file);
	struct fuse_req *req;
	unsigned int reqsize;
	int err;

	if (cpumask_weight(current->cpus_ptr) != 1 ||
	    fux->cpu != smp_processor_id())
		return -EINVAL;

restart:
	err = wait_event_interruptible(fux->waitq, fux->ready);
	if (err)
		return err;

	req = READ_ONCE(fux->req);
	if (!req)
		return -EIO;

	reqsize = req->inh.len;

	/* If request is too large, reply with an error and restart the read */
	if (iov_iter_count(to) < reqsize) {
		req->outh.error = -EIO;
		/* SETXATTR is special, since it may contain too large data */
		if (req->inh.opcode == FUSE_SETXATTR)
			req->outh.error = -E2BIG;
		fudev_req_done(fux);
		goto restart;
	}

	err = fuse_read_one(to, req->inlinedata, req->inline_inlen);
	if (!err && reqsize > req->inline_inlen)
		err = fuse_read_pages(to, req, reqsize - req->inline_inlen);

	if (err) {
		req->outh.error = -EIO;
		fudev_req_done(fux);
		return err;
	}

	return reqsize;
}

static ssize_t fudev_aux_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct fudev_aux *fux = fudev_aux(file);
	struct fuse_req *req;
	unsigned int nbytes = iov_iter_count(from);
	struct fuse_out_header oh;
	int err;

	if (cpumask_weight(current->cpus_ptr) != 1 ||
	    fux->cpu != smp_processor_id())
		return -EINVAL;

	if (nbytes < sizeof(struct fuse_out_header))
		return -EINVAL;

	if (_copy_from_iter(&oh, sizeof(oh), from) != sizeof(oh))
		return -EFAULT;

	if (oh.len != nbytes)
		return -EINVAL;

	/*
	 * Zero oh.unique indicates unsolicited notification message
	 * and error contains notification code.
	 */
	if (!oh.unique)
		return -EINVAL;

	if (oh.error <= -1000 || oh.error > 0)
		return -EINVAL;


	req = READ_ONCE(fux->req);
	if (!req)
		return -EIO;

	if (req->inh.unique != oh.unique)
		return -EIO;

	req->outh = oh;

	err = copy_out_args(from, req);
	if (err) {
		req->outh.error = -EIO;
		fudev_req_done(fux);
		return err;
	}

	fudev_req_done(fux);

	return nbytes;

}

static const struct file_operations fudev_aux_operations = {
	.owner		= THIS_MODULE,
	.open		= fudev_aux_open,
	.llseek		= no_llseek,
	.read_iter	= fudev_aux_read,
	.write_iter	= fudev_aux_write,
	.release	= fudev_aux_release,
	.unlocked_ioctl	= fudev_aux_ioctl,
	.mmap		= fudev_aux_mmap,
};

static struct miscdevice fuse_miscdevice = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "fuse2",
	.fops = &fudev_operations,
};

static struct miscdevice fuse_aux_miscdevice = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "fuse2-aux",
	.fops = &fudev_aux_operations,
};

static int fudev_queue_req(struct fuse_iqueue *fiq, struct fuse_req *req)
{
	int err;

	spin_lock(&fiq->waitq.lock);

	err = -ENOTCONN;
	if (!fiq->connected)
		goto unlock;

	req->inh.unique = fuse_get_unique(fiq);
	list_add_tail(&req->list, &fiq->pending);
	wake_up_locked(&fiq->waitq);
	/*
	 * acquire extra reference, since request is still needed after
	 * request_end()
	 */
	refcount_inc(&req->count);
	err = 0;

unlock:
	spin_unlock(&fiq->waitq.lock);

	return err;
}

static int fudev_wait_req(struct fuse_iqueue *fiq, struct fuse_req *req)
{
	int err;

	if (!test_bit(FR_FORCE, &req->flags)) {
		/* Only fatal signals may interrupt this */
		err = wait_event_killable(req->waitq,
					test_bit(FR_FINISHED, &req->flags));
		if (!err)
			return req->outh.error;

		spin_lock(&fiq->waitq.lock);
		/* Request is not yet in userspace, bail out */
		if (test_bit(FR_PENDING, &req->flags)) {
			list_del(&req->list);
			spin_unlock(&fiq->waitq.lock);
			fuse2_put_request(req);
			return -EINTR;
		}
		spin_unlock(&fiq->waitq.lock);
		if (test_bit(FR_KILLABLE, &req->flags))
			return -EINTR;
	}

	/*
	 * Either request is already in userspace, or it was forced.
	 * Wait it out.
	 */
	wait_event(req->waitq, test_bit(FR_FINISHED, &req->flags));

	return req->outh.error;
}

static void fudev_simple_copy_to_user(struct fudev_aux *fux,
				      struct fuse_args *args,
				      unsigned int inlen, u64 unique, uid_t uid,
				      gid_t gid, pid_t pid)
{
	struct fuse_in_header *inh = fux->ibuf;
	void *ptr = inh + 1;
	struct fuse_in_arg *a;
	unsigned int i;

	inh->len = inlen;
	inh->opcode = args->in.h.opcode;
	inh->unique = unique;
	inh->nodeid = args->in.h.nodeid;
	inh->uid = uid;
	inh->gid = gid;
	inh->pid = pid;
	inh->padding = 0;

	ptr = inh + 1;

	for (i = 0; i < args->in.numargs; i++) {
		a = &args->in.args[i];
		memcpy(ptr, a->value, a->size);
		ptr += a->size;
	}
}

static ssize_t fudev_simple_copy_from_user(struct fudev_aux *fux,
					   struct fuse_args *args,
					   unsigned int outlen, u64 unique)
{
	struct fuse_out_header *outh = fux->obuf;
	void *ptr = outh + 1;
	struct fuse_arg *a;
	unsigned int i;
	size_t rem;

	if (outh->unique != unique)
		return -EIO;

	rem = outh->len;
	if (rem > outlen)
		return -EIO;

	rem -= sizeof(*outh);

	if (outh->error)
		return outh->error;

	for (i = 0; i < args->out.numargs; i++) {
		a = &args->out.args[i];
		/* FIXME: truncate to rem? */
		if (a->value)
			memcpy(a->value, ptr, a->size);
		ptr += a->size;
		rem -= a->size;
	}
	if (args->out.argvar)
		return outh->len - sizeof(*outh);

	return 0;
}

static int fudev_simple_wake_and_wait(struct fudev_aux *fux,
				      struct fuse_args *args)
{
	wake_up(&fux->waitq);

	/* FIXME: make aux dev requests killable */
#if 0
	if (!args->force && args->killable)
		return wait_event_killable(fux->waitq, fux->done);
#endif

	wait_event(fux->waitq, fux->done);
	return 0;
}

static unsigned len_args(unsigned numargs, struct fuse_arg *args)
{
	unsigned nbytes = 0;
	unsigned i;

	for (i = 0; i < numargs; i++)
		nbytes += args[i].size;

	return nbytes;
}

static ssize_t fudev_simple_send(void *priv, struct fuse_args *args, uid_t uid,
				 gid_t gid, pid_t pid)
{
	unsigned int cpu;
	struct fudev *fud = priv;
	struct fudev_aux *fux;
	unsigned int inlen = sizeof(struct fuse_in_header) +
		len_args(args->in.numargs, (struct fuse_arg *) args->in.args);
	unsigned int outlen = sizeof(struct fuse_out_header) +
		len_args(args->out.numargs, args->out.args);
	ssize_t ret;
	u64 unique;
	cpumask_t orig_mask;

	cpu = get_cpu();
	fux = *per_cpu_ptr(fud->aux_devs, cpu);
	if (!fux || fux->reserved ||
	    inlen > fux->ibufsize || outlen > fux->obufsize) {
		put_cpu();
		return -EPROBE_DEFER;
	}
	fux->reserved = 1;
	fudev_barrier();
	cpumask_copy(&orig_mask, current->cpus_ptr);
	cpumask_copy(&current->cpus_mask, cpumask_of(cpu));
	put_cpu();

	unique = fux->reqctr;
	fux->reqctr += FUSE_REQ_ID_STEP;

	fudev_simple_copy_to_user(fux, args, inlen, unique, uid, gid, pid);
	fux->done = 0;
	fudev_barrier();
	fux->ready = 1;

	ret = fudev_simple_wake_and_wait(fux, args);
	if (!ret)
		ret = fudev_simple_copy_from_user(fux, args, outlen, unique);

	fudev_barrier();
	fux->reserved = 0;
	cpumask_copy(&current->cpus_mask, &orig_mask);

	return ret;
}

static int fudev_send_slow(struct fudev *fud, struct fuse_req *req)
{
	int err;

	err = fudev_queue_req(&fud->iq, req);
	if (!err)
		err = fudev_wait_req(&fud->iq, req);

	return err;
}

static int fudev_send_fast(struct fudev *fud, struct fuse_req *req)
{
 	unsigned int cpu;
	struct fudev_aux *fux;
	ssize_t ret;
	u64 unique;
	cpumask_t orig_mask;
	void *ptr, *addr;
	struct fuse_out_header *outh;
	struct fuse_page_desc *pd;
	unsigned int i, thislen, rem;

	cpu = get_cpu();
	fux = *per_cpu_ptr(fud->aux_devs, cpu);
	if (!fux || fux->reserved) {
		put_cpu();
		return -EPROBE_DEFER;
	}

	if (!fux->ibuf) {
		fux->reserved = 1;
		fudev_barrier();
		req->inh.unique = unique = fux->reqctr;
		fux->reqctr += FUSE_REQ_ID_STEP;
		fux->req = req;
		fux->done = 0;
		fudev_barrier();
		fux->ready = 1;

		put_cpu();
		ret = fudev_simple_wake_and_wait(fux, NULL);
		fudev_barrier();
		fux->reserved = 0;

		return ret;
	}
	if (req->inh.len > fux->ibufsize || req->max_outlen > fux->obufsize) {
		put_cpu();
		return -EPROBE_DEFER;
	}

	fux->reserved = 1;
	fudev_barrier();
	cpumask_copy(&orig_mask, current->cpus_ptr);
	cpumask_copy(&current->cpus_mask, cpumask_of(cpu));
	put_cpu();

	req->inh.unique = unique = fux->reqctr;
	fux->reqctr += FUSE_REQ_ID_STEP;

	ptr = fux->ibuf;
	memcpy(ptr, req->inlinedata, req->inline_inlen);
	ptr += req->inline_inlen;
	for (i = 0; i < req->num_pages; i++) {
		pd = &req->page_descs[i];
		addr = kmap_atomic(req->pages[i]);
		memcpy(ptr, addr + pd->offset, pd->length);
		kunmap_atomic(addr);
		ptr += pd->length;
	}

	fux->done = 0;
	fudev_barrier();
	fux->ready = 1;

	ret = fudev_simple_wake_and_wait(fux, NULL);
	if (!ret) {
		outh = ptr = fux->obuf;

		if (outh->unique != unique || outh->len < sizeof(*outh)) {
			ret = -EIO;
			goto out;
		}
		if (outh->error) {
			ret = outh->error;
			goto out;
		}
		rem = outh->len;
		if (rem < req->mand_outlen || rem > req->max_outlen) {
			ret = -EIO;
			goto out;
		}

		memcpy(req->inlinedata, ptr, req->inline_outlen);
		ptr += req->inline_outlen;
		rem -= req->inline_outlen;

		for (i = 0; rem && i < req->num_pages; i++) {
			pd = &req->page_descs[i];
			thislen = min(pd->length, rem);
			addr = kmap_atomic(req->pages[i]);
			memcpy(addr + pd->offset, ptr, thislen);
			kunmap_atomic(addr);
			ptr += thislen;
			rem -= thislen;
		}
	}

out:
	fudev_barrier();
	fux->reserved = 0;
	cpumask_copy(&current->cpus_mask, &orig_mask);

	return ret;
}

static int fudev_send(void *priv, struct fuse_req *req)
{
	int ret;

	ret = fudev_send_fast(priv, req);
	if (ret == -EPROBE_DEFER)
		ret = fudev_send_slow(priv, req);

	return ret;
}

static void fudev_forget(void *priv, struct fuse_forget *forget)
{
	struct fudev *fud = priv;
	struct fuse_iqueue *fiq = &fud->iq;

	spin_lock(&fiq->waitq.lock);
	if (fiq->connected) {
		list_add_tail(&forget->list, &fiq->forgets);
		wake_up_locked(&fiq->waitq);
	} else {
		kfree(forget);
	}
	spin_unlock(&fiq->waitq.lock);
}

static const struct fuse_dev_operations fudev_ops = {
	.put = fudev_put,
	.abort = fudev_abort,
	.send = fudev_send,
	.simple_send = fudev_simple_send,
	.forget = fudev_forget,
};

static struct fudev *fudev_get(int fd)
{
	struct fudev *fud;
	struct file *file;
	int err = -EINVAL;

	file = fget(fd);
	if (!file)
		goto out_err;

	if (file->f_op != &fudev_operations)
		goto fput;

	fud = fudev(file);
	/*
	 * Require mount to happen from the same user namespace which
	 * opened /dev/fuse to prevent potential attacks.
	 */
	if (file->f_cred->user_ns != current_user_ns())
		goto abort;

	refcount_inc(&fud->count);
	fput(file);

	return fud;

abort:
	fudev_abort(fud);
fput:
	fput(file);
out_err:
	return ERR_PTR(err);
}

struct fuse_mount_data {
	int fd;
};

enum {
	OPT_FD,
	OPT_ROOTMODE,
	OPT_ERR
};

static const match_table_t tokens = {
	{OPT_FD,			"fd=%u"},
	{OPT_ROOTMODE,			"rootmode=%o"},
	{OPT_ERR,			NULL}
};

static int parse_fuse_opt(char *opt, struct fuse_mount_data *d)
{
	char *p, *t = opt, *e = opt;
	bool fd_present = false;

	memset(d, 0, sizeof(struct fuse_mount_data));

	while ((p = strsep(&opt, ",")) != NULL) {
		int token;
		int value;
		size_t len;
		substring_t args[MAX_OPT_ARGS];
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case OPT_FD:
			if (match_int(&args[0], &value))
				return 0;
			d->fd = value;
			fd_present = true;
			break;

		case OPT_ROOTMODE:
			pr_info("fuse: legacy mount mode not supported\n");
			return 0;

		default:
			len = strlen(p);
			memmove(t, p, len);
			*(e = t + len) = ',';
			t = e + 1;
		}
	}

	if (!fd_present)
		return 0;

	*e = '\0';

	return 1;
}

static struct dentry *fudev_mount(struct file_system_type *fs_type,
		       int flags, const char *dev_name, void *opts)
{
	struct fuse_mount_data d;
	struct fudev *fud;
	struct dentry *root;

	if (!parse_fuse_opt(opts, &d))
		return ERR_PTR(-EINVAL);

	fud = fudev_get(d.fd);
	if (IS_ERR(fud))
		return ERR_CAST(fud);

	root = fuse_mount_common(fs_type, flags, opts, &fudev_ops, fud);
	if (!IS_ERR(root))
		fud->sb = root->d_sb;

	return root;
}

static struct file_system_type fuse_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "fuse2",
	.fs_flags	= FS_HAS_SUBTYPE | FS_USERNS_MOUNT,
	.mount		= fudev_mount,
	.kill_sb	= fuse_kill_sb,
};
MODULE_ALIAS_FS("fuse");

static int __init fudev_init(void)
{
	int err;

	err = register_filesystem(&fuse_fs_type);
	if (err)
		goto out;

	err = misc_register(&fuse_miscdevice);
	if (err)
		goto unreg_fs;

	err = misc_register(&fuse_aux_miscdevice);
	if (err)
		goto unreg_misc;

	pr_info("fuse2 device initialized\n");

	return 0;

unreg_misc:
	misc_deregister(&fuse_miscdevice);
unreg_fs:
	unregister_filesystem(&fuse_fs_type);
out:
	return err;

}
module_init(fudev_init);

static void fudev_cleanup(void)
{
	unregister_filesystem(&fuse_fs_type);
	misc_deregister(&fuse_miscdevice);
	misc_deregister(&fuse_aux_miscdevice);
}
module_exit(fudev_cleanup);
