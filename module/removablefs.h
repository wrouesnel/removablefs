/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * removablefs - present a POSIX filesystem as owned by one user.
 *
 * removablefs mounts a block device's real filesystem (ext4, xfs, btrfs, ...) on
 * a private, invisible mount and stacks on top of it.  Every file is shown
 * as owned by the uid=/gid= given at mount time, the way vfat and ntfs3
 * behave, and changes are made on the lower filesystem with credentials
 * whose fsuid/fsgid are the drive's own owner, so new files land on disk
 * owned by whoever owns the drive's root directory.
 *
 * Permission checks happen once, at the removablefs layer, against the presented
 * ownership.  Lower operations then run with DAC override, much as
 * overlayfs runs them with the mounter's credentials.
 */
#ifndef REMOVABLEFS_H
#define REMOVABLEFS_H

#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/mount.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/xattr.h>

#include "compat.h"

#define REMOVABLEFS_NAME	"removablefs"
#define REMOVABLEFS_MAGIC	0x726d6673	/* "rmfs" */
#define REMOVABLEFS_TYPE_LEN	32

struct removablefs_mount_opts {
	char *source;		/* block device */
	char *lower_type;	/* lower=; NULL means probe */
	char *lower_data;	/* options passed through to the lower fs */
	size_t lower_data_len;
	kuid_t uid;		/* presented owner */
	kgid_t gid;
	kuid_t disk_uid;	/* on-disk owner of new files */
	kgid_t disk_gid;
	bool uid_set, gid_set, disk_uid_set, disk_gid_set;
	bool quiet;		/* chown to another owner is a silent no-op */
};

struct removablefs_sb_info {
	struct vfsmount *lower_mnt;
	const struct cred *cred;	/* used for every lower operation */
	kuid_t uid;
	kgid_t gid;
	kuid_t disk_uid;
	kgid_t disk_gid;
	bool quiet;
	dev_t anon_dev;			/* restored before kill_anon_super() */
	char lower_type[REMOVABLEFS_TYPE_LEN];
	char *lower_opts;		/* passed through, shown in mountinfo */
};

struct removablefs_inode_info {
	struct inode *lower;
	struct inode vfs_inode;
};

static inline struct removablefs_sb_info *REMOVABLEFS_SB(const struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct removablefs_inode_info *REMOVABLEFS_I(const struct inode *inode)
{
	return container_of(inode, struct removablefs_inode_info, vfs_inode);
}

static inline struct inode *removablefs_lower_inode(const struct inode *inode)
{
	return REMOVABLEFS_I(inode)->lower;
}

/* The lower dentry of a positive removablefs dentry; NULL for negatives. */
static inline struct dentry *removablefs_lower(const struct dentry *dentry)
{
	return dentry->d_fsdata;
}

static inline struct vfsmount *removablefs_lower_mnt(const struct super_block *sb)
{
	return REMOVABLEFS_SB(sb)->lower_mnt;
}

static inline const struct cred *removablefs_override_creds(struct super_block *sb)
{
	return override_creds(REMOVABLEFS_SB(sb)->cred);
}

static inline void removablefs_revert_creds(const struct cred *old)
{
	revert_creds(old);
}

/* inode.c */
extern struct kmem_cache *removablefs_inode_cachep;
struct inode *removablefs_iget(struct super_block *sb, struct inode *lower);
void removablefs_copy_attr(struct inode *dst, const struct inode *src);
void removablefs_copy_attr_dentry(struct dentry *dentry);
extern const struct inode_operations removablefs_file_iops;
extern const struct inode_operations removablefs_symlink_iops;
extern const struct inode_operations removablefs_special_iops;
extern const struct xattr_handler *removablefs_xattr_handlers[];
ssize_t removablefs_listxattr(struct dentry *dentry, char *list, size_t size);
REMOVABLEFS_IOP_GETATTR_DECL(removablefs_getattr);
REMOVABLEFS_IOP_SETATTR_DECL(removablefs_setattr);
umode_t removablefs_sanitize_mode(umode_t mode);

/* namei.c */
extern const struct inode_operations removablefs_dir_iops;
extern const struct dentry_operations removablefs_dops;

/* file.c */
extern const struct file_operations removablefs_file_fops;
extern const struct file_operations removablefs_dir_fops;

#endif /* REMOVABLEFS_H */
