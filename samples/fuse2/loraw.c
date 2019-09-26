#define _GNU_SOURCE
#define LO_NOTHREAD 1

#include <linux/fuse.h>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <inttypes.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <pthread.h>
#include <semaphore.h>

struct lo_inode {
	struct lo_inode *next; /* protected by lo->mutex */
	struct lo_inode *prev; /* protected by lo->mutex */
	int fd;
	dev_t dev;
	uint64_t refcount; /* protected by lo->mutex */
	struct fuse_attr attr;
};

struct lo_file {
	union {
		struct lo_file *next;
		struct {
			int fd;
			int mapfd;
		};
		struct {
			DIR *dp;
			struct dirent *entry;
			off_t offset;
		};
	};
};

struct lo_config {
	int debug;
	int version;
	int single;
	int bind;
	int proc;
	int map;
	uint64_t timeout;
	const char *source;
	int nothread;
};

struct lo_data {
	pthread_mutex_t mutex;
#ifdef LO_NOTHREAD
#define LO_INODE_MAX 65536
	struct lo_inode inodes[LO_INODE_MAX];
	struct lo_inode *free_inodes;
#define LO_FILE_MAX 65536
	struct lo_file files[LO_FILE_MAX];
	struct lo_file *free_files;
	sem_t sem;
#endif
	struct lo_config c;
	struct lo_inode root;
};

struct lo_chan {
	struct lo_data *lo;
	enum { FUDEV_V1, FUDEV_V2, FUDEV_AUX } type;
	int fd;
	int filled;
	int mapped;
	void *inbuf;
	void *outbuf;
	size_t len;
	size_t bufsize;
};


#ifdef LO_NOTHREAD
static inline int lo_nothread(struct lo_data *lo)
{
	return lo->c.nothread;
}

static inline void lo_mutex_init_nt(struct lo_data *lo)
{
	sem_init(&lo->sem, 1, 1);
}

static inline void lo_mutex_lock_nt(struct lo_data *lo)
{
	sem_wait(&lo->sem);
}

static inline void lo_mutex_unlock_nt(struct lo_data *lo)
{
	sem_post(&lo->sem);
}

static inline struct lo_inode *lo_alloc_inode_nt(struct lo_data *lo)
{
	struct lo_inode *inode;

	lo_mutex_lock_nt(lo);
	inode = lo->free_inodes;
	if (inode)
		lo->free_inodes = inode->next;
	lo_mutex_unlock_nt(lo);

	memset(inode, 0, sizeof(*inode));

	return inode;
}

static inline struct lo_file *lo_alloc_file_nt(struct lo_data *lo)
{
	struct lo_file *lf;

	lo_mutex_lock_nt(lo);
	lf = lo->free_files;
	if (lf)
		lo->free_files = lf->next;
	lo_mutex_unlock_nt(lo);

	memset(lf, 0, sizeof(*lf));

	return lf;

}

static inline void lo_free_inode_locked_nt(struct lo_data *lo,
					   struct lo_inode *inode)
{
	inode->next = lo->free_inodes;
	lo->free_inodes = inode;
}

static inline void lo_free_file_locked_nt(struct lo_data *lo,
					  struct lo_file *lf)
{
	lf->next = lo->free_files;
	lo->free_files = lf;
}

static inline void lo_free_inode_nt(struct lo_data *lo, struct lo_inode *inode)
{
	lo_mutex_lock_nt(lo);
	lo_free_inode_locked_nt(lo, inode);
	lo_mutex_unlock_nt(lo);
}

static inline void lo_free_file_nt(struct lo_data *lo, struct lo_file *lf)
{
	lo_mutex_lock_nt(lo);
	lo_free_file_locked_nt(lo, lf);
	lo_mutex_unlock_nt(lo);
}

static inline struct lo_data *lo_alloc_lo_nt(void)
{
	struct lo_data *lo;
	unsigned int i;

	lo = mmap(NULL, sizeof(struct lo_data), PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (lo == MAP_FAILED) {
		warn("mmap(NULL, %zu, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)", sizeof(struct lo_data));
		return NULL;
	}

	for (i = 0; i < LO_INODE_MAX; i++)
		lo_free_inode_locked_nt(lo, &lo->inodes[i]);
	for (i = 0; i < LO_FILE_MAX; i++)
		lo_free_file_locked_nt(lo, &lo->files[i]);

	return lo;
}
#else

#define lo_nothread(lo) ((void) lo, 0)
#define lo_alloc_inode_nt(lo) NULL
#define lo_alloc_file_nt(lo) NULL
#define lo_free_inode_nt(lo, inode) abort()
#define lo_free_file_nt(lo, lf) abort()
#define lo_alloc_lo_nt() NULL
#define lo_mutex_init_nt(lo) abort()
#define lo_mutex_lock_nt(lo) abort()
#define lo_mutex_unlock_nt(lo) abort()

#endif

static void lo_mutex_init(struct lo_data *lo)
{
	if (!lo_nothread(lo))
		pthread_mutex_init(&lo->mutex, NULL);
	else
		lo_mutex_init_nt(lo);
}

static void lo_mutex_lock(struct lo_data *lo)
{
	if (!lo_nothread(lo))
		pthread_mutex_lock(&lo->mutex);
	else
		lo_mutex_lock_nt(lo);
}

static void lo_mutex_unlock(struct lo_data *lo)
{
	if (!lo_nothread(lo))
		pthread_mutex_unlock(&lo->mutex);
	else
		lo_mutex_unlock_nt(lo);
}

static struct lo_inode *lo_alloc_inode(struct lo_data *lo)
{
	if (!lo_nothread(lo))
		return calloc(1, sizeof(struct lo_inode));
	else
		return lo_alloc_inode_nt(lo);
}

static struct lo_file *lo_alloc_file(struct lo_data *lo)
{
	if (!lo_nothread(lo))
		return calloc(1, sizeof(struct lo_file));
	else
		return lo_alloc_file_nt(lo);
}

static void lo_free_inode(struct lo_data *lo, struct lo_inode *inode)
{
	if (!lo_nothread(lo))
		free(inode);
	else
		lo_free_inode_nt(lo, inode);
}

static void lo_free_file(struct lo_data *lo, struct lo_file *lf)
{
	if (!lo_nothread(lo))
		free(lf);
	else
		lo_free_file_nt(lo, lf);
}

static struct lo_inode *lo_inode(struct lo_data *lo, uint64_t ino)
{
	if (ino == FUSE_ROOT_ID)
		return &lo->root;
	else
		return (struct lo_inode *) (uintptr_t) ino;
}

static int lo_debug(struct lo_chan *lc)
{
	return lc->lo->c.debug;
}

static void lo_reply(struct lo_chan *lc, int error, size_t argsize)
{

	struct fuse_in_header *inh = lc->inbuf;
	struct fuse_out_header *outh = lc->outbuf;
	int res;

	outh->len = sizeof(struct fuse_out_header) + argsize;
	outh->error = -error;
	outh->unique = inh->unique;

	if (lo_debug(lc)) {
		fprintf(stderr,
			"unique: %"PRIu64", opcode: %i, nodeid: %"PRIu64", insize: %zu\n",
			inh->unique, inh->opcode, inh->nodeid, lc->len);

		fprintf(stderr, "   error: %i, outsize: %u\n",
			error, outh->len);
	}

	if (lc->mapped) {
		res = ioctl(lc->fd, FUSE2_DEV_IOC_PROC, 0);
		if (res == -1)
			err(1, "writing/reading fuse device");

		if (res > 0) {
			lc->len = res;
			lc->filled = 1;
		}
	} else {
		res = write(lc->fd, lc->outbuf, outh->len);
		if (res == -1)
			err(1, "writing fuse device");
	}
}

static void *lo_out_arg(struct lo_chan *lc)
{
	return ((struct fuse_out_header *) lc->outbuf) + 1;
}

static void lo_convert_stat(const struct stat *stat, struct fuse_attr *attr)
{
	memset(attr, 0, sizeof(*attr));

	attr->ino	= stat->st_ino;
	attr->mode	= stat->st_mode;
	attr->nlink	= stat->st_nlink;
	attr->uid	= stat->st_uid;
	attr->gid	= stat->st_gid;
	attr->rdev	= stat->st_rdev;
	attr->size	= stat->st_size;
	attr->blksize	= stat->st_blksize;
	attr->blocks	= stat->st_blocks;
	attr->atime	= stat->st_atime;
	attr->mtime	= stat->st_mtime;
	attr->ctime	= stat->st_ctime;
	attr->atimensec	= stat->st_atim.tv_nsec;
	attr->mtimensec	= stat->st_mtim.tv_nsec;
	attr->ctimensec	= stat->st_ctim.tv_nsec;
}

static void lo_getattr(struct lo_chan *lc, struct fuse_in_header *inh,
		       struct fuse_getattr_in *inarg)
{
	struct lo_data *lo = lc->lo;
	struct fuse_attr_out *outarg = lo_out_arg(lc);

	(void) inarg;

	if (lo_debug(lc))
		fprintf(stderr, "lo_getattr(ino=%"PRIu64")\n", inh->nodeid);

	outarg->attr_valid = lo->c.timeout;
	outarg->attr_valid_nsec = 0;
	outarg->dummy = 0;
	outarg->attr = lo_inode(lo, inh->nodeid)->attr;
	lo_reply(lc, 0, sizeof(*outarg));
}

static struct lo_inode *lo_find(struct lo_data *lo, struct stat *st)
{
	struct lo_inode *p;
	struct lo_inode *ret = NULL;

	lo_mutex_lock(lo);
	for (p = lo->root.next; p != &lo->root; p = p->next) {
		if (p->attr.ino == st->st_ino && p->dev == st->st_dev) {
			assert(p->refcount > 0);
			ret = p;
			ret->refcount++;
			break;
		}
	}
	lo_mutex_unlock(lo);
	return ret;
}

static void lo_lookup(struct lo_chan *lc, struct fuse_in_header *inh,
		      char *name)
{
	struct lo_data *lo = lc->lo;
	struct lo_inode *inode, *parent = lo_inode(lo, inh->nodeid);
	struct fuse_entry_out *outarg = lo_out_arg(lc);
	struct stat stat;
	int newfd;
	int res;
	int saverr;

	if (lo_debug(lc)) {
		fprintf(stderr, "lo_lookup(parent=%"PRIu64", name=%s)\n",
			inh->nodeid, name);
	}

	newfd = openat(parent->fd, name, O_PATH | O_NOFOLLOW);
	if (newfd == -1)
		goto out_err;

	res = fstatat(newfd, "", &stat, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		goto out_err;


	inode = lo_find(lo, &stat);
	if (inode) {
		close(newfd);
		newfd = -1;
	} else {
		struct lo_inode *prev, *next;

		saverr = ENOMEM;
		inode = lo_alloc_inode(lo);
		if (!inode)
			goto out_err;

		inode->refcount = 1;
		inode->fd = newfd;
		inode->dev = stat.st_dev;
		lo_convert_stat(&stat, &inode->attr);

		lo_mutex_lock(lo);
		prev = &lo->root;
		next = prev->next;
		next->prev = inode;
		inode->next = next;
		inode->prev = prev;
		prev->next = inode;
		lo_mutex_unlock(lo);
	}
	memset(outarg, 0, sizeof(*outarg));
	outarg->nodeid = (uintptr_t) inode;
	outarg->entry_valid = lo->c.timeout;
	outarg->attr_valid = lo->c.timeout;
	outarg->attr = inode->attr;

	if (lo_debug(lc)) {
		fprintf(stderr, "  %"PRIu64"/%s -> %"PRIu64"\n",
			inh->nodeid, name, outarg->nodeid);
	}

	lo_reply(lc, 0, sizeof(*outarg));
	return;

out_err:
	saverr = errno;
	if (newfd != -1)
		close(newfd);
	lo_reply(lc, saverr, 0);
}

static void lo_open(struct lo_chan *lc, struct fuse_in_header *inh,
		    struct fuse_open_in *inarg)
{
	struct lo_data *lo = lc->lo;
	struct lo_inode *inode = lo_inode(lo, inh->nodeid);
	struct fuse_open_out *outarg = lo_out_arg(lc);
	struct lo_file *lf;
	char buf[64];
	int fd;

	sprintf(buf, "/proc/self/fd/%i", inode->fd);
	fd = open(buf, inarg->flags & ~O_NOFOLLOW);
	if (fd == -1) {
		lo_reply(lc, errno, 0);
		return;
	}

	lf = lo_alloc_file(lo);
	if (lf == NULL)
		errx(1, "malloc failed");

	memset(outarg, 0, sizeof(*outarg));
	outarg->fh = (uintptr_t) lf;
	outarg->open_flags = FOPEN_DIRECT_IO;

	lf->fd = fd;
	if (lo->c.map) {
		lf->mapfd = ioctl(lc->fd, FUSE2_DEV_IOC_MAP_OPEN, fd);
		if (lf->mapfd == -1)
			warn("FUSE2_DEV_IOC_MAP_OPEN");
		else
			outarg->open_flags |= FOPEN_MAP;
	}

	lo_reply(lc, 0, sizeof(*outarg));
}

static struct lo_file *lo_file(uint64_t fh)
{
	return (void *) (uintptr_t) fh;
}

static void lo_release(struct lo_chan *lc, struct fuse_in_header *inh,
		       struct fuse_release_in *inarg)
{
	struct lo_file *lf = lo_file(inarg->fh);

	(void) inh;

	close(lf->fd);
	if (lf->mapfd != -1)
		ioctl(lc->fd, FUSE2_DEV_IOC_MAP_CLOSE, lf->mapfd);

	lo_free_file(lc->lo, lf);
	lo_reply(lc, 0, 0);
}

static void lo_read(struct lo_chan *lc, struct fuse_in_header *inh,
		    struct fuse_read_in *inarg)
{
	char *outarg = lo_out_arg(lc);
	struct lo_file *lf = lo_file(inarg->fh);
	ssize_t res;

	(void) inh;

	if (inarg->size > lc->bufsize - (outarg - (char *) lc->outbuf)) {
		lo_reply(lc, EOVERFLOW, 0);
		return;
	}

	res = pread(lf->fd, outarg, inarg->size, inarg->offset);
	if (res == -1) {
		lo_reply(lc, errno, 0);
		return;
	}

	lo_reply(lc, 0, res);
}

static void lo_map(struct lo_chan *lc, struct fuse_in_header *inh,
		    struct fuse_read_in *inarg)
{
	struct lo_file *lf = lo_file(inarg->fh);
	struct fuse_map_out *outarg = lo_out_arg(lc);

	(void) inh;

	if (lo_debug(lc))
		fprintf(stderr, "lo_map(offset=%"PRIu64", size=%u)\n",
			inarg->offset, inarg->size);

	outarg->mapfd = lf->mapfd;
	outarg->offset = inarg->offset;
	outarg->size = inarg->size;

	lo_reply(lc, 0, sizeof(*outarg));
}


static void lo_opendir(struct lo_chan *lc, struct fuse_in_header *inh,
		       struct fuse_open_in *inarg)
{
	struct lo_data *lo = lc->lo;
	struct lo_inode *inode = lo_inode(lo, inh->nodeid);
	struct fuse_open_out *outarg = lo_out_arg(lc);
	struct lo_file *lf;
	int fd;
	DIR *dp;

	(void) inarg;

	fd = openat(inode->fd, ".", O_RDONLY);
	if (fd == -1) {
		lo_reply(lc, errno, 0);
		return;
	}

	dp = fdopendir(fd);
	if (dp == NULL) {
		int saverr = errno;

		close(fd);
		lo_reply(lc, saverr, 0);
		return;
	}

	lf = lo_alloc_file(lo);
	if (lf == NULL)
		errx(1, "malloc failed");

	memset(outarg, 0, sizeof(*outarg));
	outarg->fh = (uintptr_t) lf;
	outarg->open_flags = 0;

	lf->dp = dp;

	lo_reply(lc, 0, sizeof(*outarg));
}

static void lo_releasedir(struct lo_chan *lc, struct fuse_in_header *inh,
		       struct fuse_release_in *inarg)
{
	struct lo_file *lf = lo_file(inarg->fh);

	(void) inh;

	closedir(lf->dp);

	lo_free_file(lc->lo, lf);
	lo_reply(lc, 0, 0);
}

static void lo_readdir(struct lo_chan *lc, struct fuse_in_header *inh,
		       struct fuse_read_in *inarg)
{
	void *p = lo_out_arg(lc);
	struct lo_file *lf = lo_file(inarg->fh);
	size_t rem = inarg->size;
	int err = 0;
	const char *name;
	size_t namelen, entlen, entlen_padded;
	struct fuse_dirent *dirent;
	off_t nextoff;

	(void) inh;

	if (inarg->size > lc->bufsize - (p - lc->outbuf)) {
		lo_reply(lc, EOVERFLOW, 0);
		return;
	}

	if ((off_t) inarg->offset != lf->offset) {
		seekdir(lf->dp, inarg->offset);
		lf->entry = NULL;
		lf->offset = inarg->offset;
	}
	while (1) {
		if (!lf->entry) {
			errno = 0;
			lf->entry = readdir(lf->dp);
			if (!lf->entry) {
				if (errno) {
					err = errno;
					break;
				} else {
					break; 
				}
			}
		}
		nextoff = lf->entry->d_off;
		name = lf->entry->d_name;
		namelen = strlen(name);
		entlen = FUSE_NAME_OFFSET + namelen;
		entlen_padded = FUSE_DIRENT_ALIGN(entlen);
		if (entlen_padded > rem)
			break;

		dirent = (struct fuse_dirent *) p;

		dirent->ino = lf->entry->d_ino;
		dirent->off = nextoff;
		dirent->namelen = namelen;
		dirent->type = lf->entry->d_type;
		memcpy(dirent->name, name, namelen);
		memset(dirent->name + namelen, 0, entlen_padded - entlen);

		p += entlen_padded;
		rem -= entlen_padded;

		lf->entry = NULL;
		lf->offset = nextoff;
	}

	if (err && rem == inarg->size)
		lo_reply(lc, err, 0);
	else
		lo_reply(lc, 0, inarg->size - rem);
}

static void unref_inode(struct lo_data *lo, struct lo_inode *inode, uint64_t n)
{
	if (!inode)
		return;

	lo_mutex_lock(lo);
	assert(inode->refcount >= n);
	inode->refcount -= n;
	if (!inode->refcount) {
		struct lo_inode *prev, *next;

		prev = inode->prev;
		next = inode->next;
		next->prev = prev;
		prev->next = next;
		lo_mutex_unlock(lo);

		close(inode->fd);
		lo_free_inode(lo, inode);
	} else {
		lo_mutex_unlock(lo);
	}
}

static void lo_forget_one(struct lo_data *lo, uint64_t nodeid,
			  uint64_t nlookup)
{
	struct lo_inode *inode = lo_inode(lo, nodeid);

	if (lo->c.debug) {
		fprintf(stderr, "  forget %"PRIu64" %"PRIu64" -%"PRIu64"\n",
			nodeid, inode->refcount, nlookup);
	}

	unref_inode(lo, inode, nlookup);

}

static void lo_forget(struct lo_data *lo, struct fuse_in_header *inh,
		      struct fuse_forget_in *inarg)
{
	lo_forget_one(lo, inh->nodeid, inarg->nlookup);
}

static void lo_batch_forget(struct lo_data *lo, struct fuse_in_header *inh,
			    struct fuse_batch_forget_in *inarg)
{
	struct fuse_forget_one *param = (void *) (inarg + 1);
	unsigned int i;

	(void) inh;

	for (i = 0; i < inarg->count; i++)
		lo_forget_one(lo, param[i].nodeid, param[i].nlookup);
}

static void lo_init(struct lo_chan *lc, struct fuse_in_header *inh,
		    struct fuse_init_in *inarg)
{
	struct fuse_init_out *outarg = lo_out_arg(lc);

	(void) inh;

	memset(outarg, 0, sizeof(*outarg));
	outarg->flags = inarg->flags &
		(FUSE_ATOMIC_O_TRUNC | FUSE_BIG_WRITES | FUSE_PARALLEL_DIROPS |
		 FUSE_HANDLE_KILLPRIV);
	outarg->major = FUSE_KERNEL_VERSION;
	outarg->minor = FUSE_KERNEL_MINOR_VERSION;

	lo_reply(lc, 0, sizeof(*outarg));
}

static ssize_t lo_getreq(struct lo_chan *lc)
{
	ssize_t res;

	if (lc->mapped)
		res = ioctl(lc->fd, FUSE2_DEV_IOC_READ, 0);
	else
		res = read(lc->fd, lc->inbuf, lc->bufsize);
	if (res != -1)
		lc->len = res;

	return res;
}

static void lo_process(struct lo_chan *lc)
{
	int res;
	struct fuse_in_header *inh = lc->inbuf;
	void *arg = inh + 1;

	if (!lc->filled) {
		res = lo_getreq(lc);
		if (res == -1)
			err(1, "reading from device");
	}

	lc->filled = 0;

	if (lc->len < sizeof(*inh))
		errx(1, "short read from fuse device");

	switch (inh->opcode) {
	case FUSE_INIT:
		lo_init(lc, inh, arg);
		break;

	case FUSE_LOOKUP:
		lo_lookup(lc, inh, arg);
		break;

	case FUSE_GETATTR:
		lo_getattr(lc, inh, arg);
		break;

	case FUSE_OPEN:
		lo_open(lc, inh, arg);
		break;

	case FUSE_RELEASE:
		lo_release(lc, inh, arg);
		break;

	case FUSE_READ:
		lo_read(lc, inh, arg);
		break;

	case FUSE_MAP:
		lo_map(lc, inh, arg);
		break;

	case FUSE_OPENDIR:
		lo_opendir(lc, inh, arg);
		break;

	case FUSE_RELEASEDIR:
		lo_releasedir(lc, inh, arg);
		break;

	case FUSE_READDIR:
		lo_readdir(lc, inh, arg);
		break;

	case FUSE_FORGET:
		lo_forget(lc->lo, inh, arg);
		break;

	case FUSE_BATCH_FORGET:
		lo_batch_forget(lc->lo, inh, arg);
		break;

	default:
		lo_reply(lc, ENOSYS, 0);
	}

#if 0
	{
		static int slow_ctr, fast_ctr;
		int *cp;

		cp = lc->type == FUDEV_AUX ? &fast_ctr : &slow_ctr;
		if (__atomic_add_fetch(cp, 1, __ATOMIC_SEQ_CST) % 1000000 == 0)
			fprintf(stderr, "slow: %9i fast: %9i total: %9i\r",
				slow_ctr, fast_ctr, slow_ctr + fast_ctr);
	}
#endif
}

static void lo_alloc_bufs(struct lo_chan *lc)
{
	int res;

	res = posix_memalign(&lc->inbuf, 0x1000, lc->bufsize);
	if (res)
		errx(1, "allocating aligned buffer: %s", strerror(res));

	res = posix_memalign(&lc->outbuf, 0x1000, lc->bufsize);
	if (res)
		errx(1, "allocating aligned buffer: %s", strerror(res));
}

static void lo_map_bufs(struct lo_chan *lc)
{
	lc->inbuf = mmap(NULL, lc->bufsize, PROT_READ, MAP_SHARED, lc->fd,
			 FUSE2_MMAP_INBUF_OFFSET);
	if (lc->inbuf == MAP_FAILED)
		err(1, "mmap of inbuf failed");

	lc->outbuf = mmap(NULL, lc->bufsize, PROT_WRITE, MAP_SHARED, lc->fd,
			  FUSE2_MMAP_OUTBUF_OFFSET);
	if (lc->outbuf == MAP_FAILED)
		err(1, "mmap of outbuf failed");

	lc->mapped = 1;
}

static void lo_open_aux(struct lo_chan *lc, int devfd)
{
	int res;

	lc->type = FUDEV_AUX;
	lc->fd = open("/dev/fuse2-aux", O_RDWR);
	if (lc->fd == -1)
		err(1, "failed to open /dev/fuse2-aux");

	res = ioctl(lc->fd, FUSE2_DEV_IOC_BIND, &devfd);
	if (res == -1)
		err(1, "failed to bind aux device");

	if (lc->lo->c.proc)
		lo_map_bufs(lc);
	else
		lo_alloc_bufs(lc);
}

struct lo_thread_data {
	struct lo_chan *def_chan;
	int cpu;
};

static void lo_start_aux(struct lo_thread_data *ltd)
{
	struct lo_chan lc = {
		.lo = ltd->def_chan->lo,
		.bufsize = lc.lo->c.proc ? 0x2000 : ltd->def_chan->bufsize,
	};
	cpu_set_t set;
	int res;

	CPU_ZERO(&set);
	CPU_SET(ltd->cpu, &set);

	res = sched_setaffinity(0, sizeof(set), &set);
	if (res == -1)
		err(1, "sched_getaffinity() to cpu %i", ltd->cpu);

	lo_open_aux(&lc, ltd->def_chan->fd);

	while (1)
		lo_process(&lc);
}

static void lo_start_v1(struct lo_thread_data *ltd)
{
	int res;
	int devfd = ltd->def_chan->fd;
	struct lo_chan lc = {
		.type = FUDEV_V1,
		.lo = ltd->def_chan->lo,
		.bufsize = ltd->def_chan->bufsize,
	};

	if (lc.lo->c.bind) {
		lc.fd = open("/dev/fuse", O_RDWR);
		if (lc.fd == -1)
			err(1, "/dev/fuse");

		res = ioctl(lc.fd, FUSE_DEV_IOC_CLONE, &devfd);
		if (res == -1)
			err(1, "FUSE_DEV_IOC_CLONE");

	} else {
		lc.fd = devfd;
	}

	lo_alloc_bufs(&lc);


	while (1)
		lo_process(&lc);
}
static void *lo_start_one(void *data)
{
	struct lo_thread_data *ltd = data;

	if (ltd->def_chan->type == FUDEV_V2)
		lo_start_aux(ltd);
	else
		lo_start_v1(ltd);

	return NULL;
}

static int lo_start_one_nt(void *data)
{
	lo_start_one(data);
	return 0;
}

static void lo_start_threads(struct lo_chan *def_chan)
{
	int i, n, res;
	cpu_set_t set;
	struct lo_thread_data *ltd;

	res = sched_getaffinity(0, sizeof(set), &set);
	if (res == -1)
		err(1, "sched_getaffinity()");

	n = CPU_COUNT(&set);
	for (i = 0; n && i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &set)) {
			ltd = malloc(sizeof(*ltd));
			if (ltd == NULL)
				errx(1, "malloc failed");

			ltd->def_chan = def_chan;
			ltd->cpu = i;
			if (!lo_nothread(def_chan->lo)) {
				pthread_t id;

				res = pthread_create(&id, NULL, lo_start_one, ltd);
				if (res != 0) {
					errno = res;
					err(1, "pthread_create");
				}
			} else {
				void *stack, *top;
				size_t stack_size = 1048576;

				stack = malloc(stack_size);
				if (stack == NULL)
					errx(1, "failed to allocate child stack");
				top = stack + stack_size;

				res = clone(lo_start_one_nt, top, CLONE_FILES, ltd);
				if (res == -1)
					err(1, "clone");
			}

			n--;
		}
	}
}

static void lo_usage(char *argv[])
{
	errx(1, "usage: %s [-d] [-s] [-b] [-m] [-1] [-2] mountpoint", argv[0]);
}

int main(int argc, char *argv[])
{
	struct lo_data *lo;
	struct lo_config c = {};
	struct lo_chan def_chan = {
		.bufsize = 0x21000,
	};
	char *devname;
	int res, pid;
	int status;
	struct stat stat;
	int ctr;
	const char *mnt = NULL;

	if (argc < 2)
		lo_usage(argv);

	c.source = "/";
	for (ctr = 1; ctr < argc; ctr++) {
		char *arg = argv[ctr];

		if (arg[0] == '-') {
			switch (arg[1]) {
			case 'd':
				c.debug = 1;
				break;
			case 's':
				c.single = 1;
				break;
			case 'b':
				c.bind = 1;
				break;
			case 'p':
				c.proc = 1;
				break;
			case 'm':
				c.map = 1;
				break;
			case '1':
				c.version = 1;
				break;
			case '2':
				c.version = 2;
				break;
#ifdef LO_NOTHREAD
			case 't':
				c.nothread = 1;
				break;
#endif
			default:
				lo_usage(argv);
			}

		} else if (!mnt) {
			mnt = arg;
		} else {
			lo_usage(argv);
		}
	}

	if (c.nothread)
		lo = lo_alloc_lo_nt();
	else
		lo = calloc(1, sizeof(struct lo_data));
	if (lo == NULL)
		errx(1, "failed to allocate memory");

	lo->c = c;
	lo_mutex_init(lo);

	/* Don't mask creation mode, kernel already did that */
	umask(0);

	lo->root.next = lo->root.prev = &lo->root;
	lo->root.refcount = 2;

	lo->root.fd = open(lo->c.source, O_PATH);
	if (lo->root.fd == -1)
		err(1, "open(\"%s\", O_PATH)", lo->c.source);

	res = fstatat(lo->root.fd, "", &stat,
		      AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		err(1, "statting root");

	lo_convert_stat(&stat, &lo->root.attr);


	def_chan.lo = lo;
	def_chan.fd = -1;
	if (lo->c.version != 1) {
		def_chan.type = FUDEV_V2;
		devname = "/dev/fuse2";
		def_chan.fd = open(devname, O_RDWR);
	}
	if (lo->c.version != 2 && def_chan.fd == -1) {
		def_chan.type = FUDEV_V1;
		devname = "/dev/fuse";
		def_chan.fd = open(devname, O_RDWR);
	}
	if (def_chan.fd == -1)
		err(1, "opening %s", devname);

	lo_alloc_bufs(&def_chan);

	pid = fork();
	if (pid == -1)
		err(1, "fork");

	if (pid == 0) {
		char opts[128];

		if (def_chan.type == FUDEV_V2) {
			snprintf(opts, sizeof(opts), "fd=%i", def_chan.fd);
			res = mount("loraw", mnt, "fuse2.loraw", 0, opts);
		} else {
			snprintf(opts, sizeof(opts),
				 "fd=%i,rootmode=40000,user_id=0,group_id=0",
				 def_chan.fd);

			res = mount("loraw", mnt, "fuse.loraw", 0, opts);
		}
		if (res == -1)
			exit(1);

		exit(0);
	}
	res = lo_getreq(&def_chan);
	if (res > 0) {
		struct fuse_in_header *inh = def_chan.inbuf;

		if (def_chan.len < sizeof(*inh))
			errx(1, "short read from fuse device");
		if (inh->opcode != FUSE_INIT)
			errx(1, "FUSE_INIT expected");
		lo_init(&def_chan, inh, (void *) (inh + 1));
	}
	res = waitpid(pid, &status, 0);
	if (res == -1)
		err(1, "waitpid failed for mount process");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		errx(1, "mount failed");

	if (!lo->c.single)
		lo_start_threads(&def_chan);

	while (1)
		lo_process(&def_chan);

	return 0;
}
