// SPDX-License-Identifier: GPL-2.0-only
/*
 * ownfs file operations: every open ownfs file holds an open file on the
 * lower filesystem in private_data and passes I/O straight through to it.
 * ownfs inodes have no page cache of their own; mmap hands the VMA to the
 * lower file.
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uio.h>
#include <linux/splice.h>
#include <linux/falloc.h>
#include <linux/file.h>

#include "ownfs.h"

static inline struct file *ownfs_lower_file(const struct file *file)
{
	return file->private_data;
}

static int ownfs_open(struct inode *inode, struct file *file)
{
	struct path lower_path = {
		.mnt	= ownfs_lower_mnt(inode->i_sb),
		.dentry	= ownfs_lower(file->f_path.dentry),
	};
	struct file *lower;
	/* Creation and truncation have already happened at the ownfs layer. */
	int flags = file->f_flags & ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);

	lower = dentry_open(&lower_path, flags, OWNFS_SB(inode->i_sb)->cred);
	if (IS_ERR(lower))
		return PTR_ERR(lower);

	file->private_data = lower;
#ifdef HAVE_FMODE_CAN_ODIRECT
	file->f_mode |= lower->f_mode & FMODE_CAN_ODIRECT;
#endif
	return 0;
}

static int ownfs_release(struct inode *inode, struct file *file)
{
	fput(ownfs_lower_file(file));
	return 0;
}

/* Per-call flags (preadv2/pwritev2) that matter to the lower file. */
#define OWNFS_IOCB_PASS (IOCB_DSYNC | IOCB_SYNC | IOCB_APPEND)

static ssize_t ownfs_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct file *lower = ownfs_lower_file(file);
	struct kiocb kiocb;
	const struct cred *old;
	ssize_t ret;

	if (!iov_iter_count(iter))
		return 0;
	if (!lower->f_op->read_iter)
		return -EINVAL;

	init_sync_kiocb(&kiocb, lower);
	kiocb.ki_pos = iocb->ki_pos;
	kiocb.ki_flags |= iocb->ki_flags & OWNFS_IOCB_PASS;

	old = ownfs_override_creds(file_inode(file)->i_sb);
	ret = lower->f_op->read_iter(&kiocb, iter);
	ownfs_revert_creds(old);
	if (ret > 0)
		iocb->ki_pos = kiocb.ki_pos;

	ownfs_copy_times(file_inode(file), file_inode(lower));
	return ret;
}

static ssize_t ownfs_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct file *lower = ownfs_lower_file(file);
	struct kiocb kiocb;
	const struct cred *old;
	ssize_t ret;

	if (!iov_iter_count(iter))
		return 0;
	if (!lower->f_op->write_iter)
		return -EINVAL;

	inode_lock(inode);
	init_sync_kiocb(&kiocb, lower);
	kiocb.ki_pos = iocb->ki_pos;
	kiocb.ki_flags |= iocb->ki_flags & OWNFS_IOCB_PASS;

	old = ownfs_override_creds(inode->i_sb);
	/* Freeze protection for the lower superblock. */
	file_start_write(lower);
	ret = lower->f_op->write_iter(&kiocb, iter);
	file_end_write(lower);
	ownfs_revert_creds(old);
	if (ret > 0)
		iocb->ki_pos = kiocb.ki_pos;

	ownfs_copy_attr(inode, file_inode(lower));
	inode_unlock(inode);
	return ret;
}

static int ownfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *lower = ownfs_lower_file(file);
	const struct cred *old;
	int ret;

	/* From here on the VMA belongs to the lower file. */
	ownfs_vma_set_file(vma, lower);

	old = ownfs_override_creds(file_inode(file)->i_sb);
	ret = ownfs_call_mmap(lower, vma);
	ownfs_revert_creds(old);

	ownfs_copy_times(file_inode(file), file_inode(lower));
	return ret;
}

static int ownfs_fsync(struct file *file, loff_t start, loff_t end,
		       int datasync)
{
	struct file *lower = ownfs_lower_file(file);
	const struct cred *old;
	int ret;

	old = ownfs_override_creds(file_inode(file)->i_sb);
	ret = vfs_fsync_range(lower, start, end, datasync);
	ownfs_revert_creds(old);
	return ret;
}

static loff_t ownfs_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file_inode(file);
	struct file *lower = ownfs_lower_file(file);
	const struct cred *old;
	loff_t ret;

	/* SEEK_DATA/HOLE and directory cookies are the lower fs's business. */
	inode_lock(inode);
	lower->f_pos = file->f_pos;
	old = ownfs_override_creds(inode->i_sb);
	ret = vfs_llseek(lower, offset, whence);
	ownfs_revert_creds(old);
	file->f_pos = lower->f_pos;
	inode_unlock(inode);
	return ret;
}

static long ownfs_fallocate(struct file *file, int mode, loff_t offset,
			    loff_t len)
{
	struct inode *inode = file_inode(file);
	struct file *lower = ownfs_lower_file(file);
	const struct cred *old;
	long ret;

	inode_lock(inode);
	old = ownfs_override_creds(inode->i_sb);
	ret = vfs_fallocate(lower, mode, offset, len);
	ownfs_revert_creds(old);
	ownfs_copy_attr(inode, file_inode(lower));
	inode_unlock(inode);
	return ret;
}

static int ownfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct file *lower = ownfs_lower_file(file);
	const struct cred *old;
	int ret;

	old = ownfs_override_creds(file_inode(file)->i_sb);
	ret = iterate_dir(lower, ctx);
	ownfs_revert_creds(old);

	ownfs_copy_times(file_inode(file), file_inode(lower));
	return ret;
}

const struct file_operations ownfs_file_fops = {
	.open		= ownfs_open,
	.release	= ownfs_release,
	.llseek		= ownfs_llseek,
	.read_iter	= ownfs_read_iter,
	.write_iter	= ownfs_write_iter,
	.mmap		= ownfs_mmap,
	.fsync		= ownfs_fsync,
	.fallocate	= ownfs_fallocate,
#ifdef HAVE_COPY_SPLICE_READ
	.splice_read	= copy_splice_read,
#else
	.splice_read	= generic_file_splice_read,
#endif
	.splice_write	= iter_file_splice_write,
};

const struct file_operations ownfs_dir_fops = {
	.open		= ownfs_open,
	.release	= ownfs_release,
	.llseek		= ownfs_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= ownfs_iterate,
	.fsync		= ownfs_fsync,
};
