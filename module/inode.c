// SPDX-License-Identifier: GPL-2.0-only
/*
 * removablefs inodes: lifetime, attribute presentation and xattrs.
 */
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/capability.h>
#include <linux/fcntl.h>

#include "removablefs.h"

struct kmem_cache *removablefs_inode_cachep;

/* Lower inode flags worth reflecting; the rest describe lower internals. */
#define REMOVABLEFS_COPIED_IFLAGS (S_IMMUTABLE | S_APPEND | S_NOATIME)

/*
 * Copy the lower inode's attributes, except that ownership is always the
 * mount's presented owner.
 */
void removablefs_copy_attr(struct inode *dst, const struct inode *src)
{
	struct removablefs_sb_info *sbi = REMOVABLEFS_SB(dst->i_sb);

	dst->i_mode = src->i_mode;
	dst->i_uid = sbi->uid;
	dst->i_gid = sbi->gid;
	dst->i_rdev = src->i_rdev;
	removablefs_copy_times(dst, src);
	dst->i_blkbits = src->i_blkbits;
	dst->i_flags = (dst->i_flags & ~REMOVABLEFS_COPIED_IFLAGS) |
		       (src->i_flags & REMOVABLEFS_COPIED_IFLAGS);
	set_nlink(dst, src->i_nlink);
	i_size_write(dst, i_size_read(src));
	dst->i_blocks = src->i_blocks;
}

void removablefs_copy_attr_dentry(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	if (inode)
		removablefs_copy_attr(inode, removablefs_lower_inode(inode));
}

/*
 * Only the owner of a drive should ever hand out setuid programs from it.
 * Files created through removablefs land on disk owned by the drive's owner,
 * which may be root, so a user must not be able to plant setuid or setgid
 * executables there: they would take effect wherever the drive is later
 * mounted without nosuid.  Setgid on directories only steers group
 * inheritance and is harmless.
 */
umode_t removablefs_sanitize_mode(umode_t mode)
{
	if (capable(CAP_FSETID))
		return mode;
	if (S_ISDIR(mode))
		return mode & ~S_ISUID;
	return mode & ~(S_ISUID | S_ISGID);
}

static int removablefs_inode_test(struct inode *inode, void *lower)
{
	return REMOVABLEFS_I(inode)->lower == lower;
}

static int removablefs_inode_set(struct inode *inode, void *lower)
{
	REMOVABLEFS_I(inode)->lower = lower;
	return 0;
}

/*
 * removablefs inodes cache no pages.  Kernels that decide whether O_DIRECT is
 * allowed by looking for ->direct_IO need a stub; the I/O itself goes to
 * the lower file, opened with O_DIRECT too.
 */
static const struct address_space_operations removablefs_aops = {
#if !defined(HAVE_FMODE_CAN_ODIRECT) && defined(HAVE_NOOP_DIRECT_IO)
	.direct_IO	= noop_direct_IO,
#endif
};

/*
 * Find or create the removablefs inode stacked on @lower.  Hard links share a
 * lower inode, so they share a removablefs inode too.
 */
struct inode *removablefs_iget(struct super_block *sb, struct inode *lower)
{
	struct inode *inode;

	if (!igrab(lower))
		return ERR_PTR(-ESTALE);

	inode = iget5_locked(sb, (unsigned long)lower, removablefs_inode_test,
			     removablefs_inode_set, lower);
	if (!inode) {
		iput(lower);
		return ERR_PTR(-ENOMEM);
	}
	if (!removablefs_inode_is_new(inode)) {
		/* The existing inode already holds a reference. */
		iput(lower);
		return inode;
	}

	inode->i_ino = lower->i_ino;
	inode->i_mapping->a_ops = &removablefs_aops;
	removablefs_copy_attr(inode, lower);

	switch (lower->i_mode & S_IFMT) {
	case S_IFDIR:
		inode->i_op = &removablefs_dir_iops;
		inode->i_fop = &removablefs_dir_fops;
		break;
	case S_IFREG:
		inode->i_op = &removablefs_file_iops;
		inode->i_fop = &removablefs_file_fops;
		break;
	case S_IFLNK:
		inode->i_op = &removablefs_symlink_iops;
		break;
	default:
		/* Devices, fifos and sockets: the VFS handles them itself. */
		inode->i_op = &removablefs_special_iops;
		init_special_inode(inode, lower->i_mode, lower->i_rdev);
		break;
	}

	unlock_new_inode(inode);
	return inode;
}

REMOVABLEFS_IOP_GETATTR_DECL(removablefs_getattr)
{
	struct dentry *dentry = path->dentry;
	struct removablefs_sb_info *sbi = REMOVABLEFS_SB(dentry->d_sb);
	struct path lower_path = {
		.mnt	= sbi->lower_mnt,
		.dentry	= removablefs_lower(dentry),
	};
	const struct cred *old;
	int err;

	old = removablefs_override_creds(dentry->d_sb);
#ifdef AT_GETATTR_NOSEC
	/* Security was checked at our layer; the VFS says not to repeat it. */
	if (flags & AT_GETATTR_NOSEC)
		err = vfs_getattr_nosec(&lower_path, stat, request_mask, flags);
	else
#endif
		err = vfs_getattr(&lower_path, stat, request_mask, flags);
	removablefs_revert_creds(old);
	if (err)
		return err;

	/*
	 * stat->dev is the lower device, which matches the removablefs superblock's
	 * s_dev (see removablefs_fill_super), so st_dev agrees with mountinfo.
	 */
	stat->uid = sbi->uid;
	stat->gid = sbi->gid;
	removablefs_copy_attr(d_inode(dentry), d_inode(lower_path.dentry));
	return 0;
}

REMOVABLEFS_IOP_SETATTR_DECL(removablefs_setattr)
{
	struct inode *inode = d_inode(dentry);
	struct removablefs_sb_info *sbi = REMOVABLEFS_SB(inode->i_sb);
	struct dentry *lower = removablefs_lower(dentry);
	struct inode *lower_inode = d_inode(lower);
	const struct cred *old;
	int err;

	/*
	 * Ownership is not stored per file.  Changing it to what it already
	 * is succeeds, so "cp -a" and friends keep working; anything else
	 * fails like vfat, or is ignored with "quiet".
	 */
	if (attr->ia_valid & ATTR_UID) {
		if (!uid_eq(attr->ia_uid, sbi->uid) && !sbi->quiet)
			return -EPERM;
		attr->ia_valid &= ~ATTR_UID;
	}
	if (attr->ia_valid & ATTR_GID) {
		if (!gid_eq(attr->ia_gid, sbi->gid) && !sbi->quiet)
			return -EPERM;
		attr->ia_valid &= ~ATTR_GID;
	}

	err = removablefs_setattr_prepare(REMOVABLEFS_IOP_NS dentry, attr);
	if (err)
		return err;

	/* The lower fs must not see our struct file, nor O_TRUNC's flag. */
	attr->ia_valid &= ~(ATTR_FILE | ATTR_OPEN);
	/* notify_change() BUGs on ATTR_MODE together with ATTR_KILL_S*ID. */
	if (attr->ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		attr->ia_valid &= ~ATTR_MODE;
	if (attr->ia_valid & ATTR_MODE)
		attr->ia_mode = removablefs_sanitize_mode(attr->ia_mode);
	if (!(attr->ia_valid & ~ATTR_FORCE))
		return 0;

	err = mnt_want_write(sbi->lower_mnt);
	if (err)
		return err;
	old = removablefs_override_creds(inode->i_sb);
	inode_lock(lower_inode);
	err = removablefs_notify_change(lower, attr);
	inode_unlock(lower_inode);
	removablefs_revert_creds(old);
	mnt_drop_write(sbi->lower_mnt);

	removablefs_copy_attr(inode, lower_inode);
	return err;
}

static const char *removablefs_get_link(struct dentry *dentry, struct inode *inode,
					struct delayed_call *done)
{
	const struct cred *old;
	const char *link;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	old = removablefs_override_creds(inode->i_sb);
	link = vfs_get_link(removablefs_lower(dentry), done);
	removablefs_revert_creds(old);
	return link;
}

/*
 * Extended attributes: only the user.* namespace is passed through.  POSIX
 * ACLs and security.* labels describe on-disk owners and policies that
 * removablefs deliberately ignores, and trusted.* needs CAP_SYS_ADMIN, which the
 * lower credentials do not carry.
 */
static int removablefs_xattr_get(const struct xattr_handler *handler,
				 struct dentry *dentry, struct inode *inode,
				 const char *name, void *buffer, size_t size)
{
	const char *full = xattr_full_name(handler, name);
	const struct cred *old;
	ssize_t ret;

	old = removablefs_override_creds(inode->i_sb);
	ret = removablefs_vfs_getxattr(removablefs_lower(dentry), full, buffer, size);
	removablefs_revert_creds(old);
	return ret;
}

static int removablefs_xattr_set(const struct xattr_handler *handler,
				 REMOVABLEFS_XATTR_SET_NS_DECL
				 struct dentry *dentry, struct inode *inode,
				 const char *name, const void *value, size_t size,
				 int flags)
{
	const char *full = xattr_full_name(handler, name);
	struct removablefs_sb_info *sbi = REMOVABLEFS_SB(inode->i_sb);
	struct dentry *lower = removablefs_lower(dentry);
	const struct cred *old;
	int err;

	err = mnt_want_write(sbi->lower_mnt);
	if (err)
		return err;
	old = removablefs_override_creds(inode->i_sb);
	if (value)
		err = removablefs_vfs_setxattr(lower, full, value, size, flags);
	else
		err = removablefs_vfs_removexattr(lower, full);
	removablefs_revert_creds(old);
	mnt_drop_write(sbi->lower_mnt);

	removablefs_copy_attr(inode, d_inode(lower));
	return err;
}

static const struct xattr_handler removablefs_user_xattr_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= removablefs_xattr_get,
	.set	= removablefs_xattr_set,
};

const struct xattr_handler *removablefs_xattr_handlers[] = {
	&removablefs_user_xattr_handler,
	NULL
};

ssize_t removablefs_listxattr(struct dentry *dentry, char *list, size_t size)
{
	struct dentry *lower = removablefs_lower(dentry);
	const struct cred *old;
	ssize_t len, out = 0;
	char *buf, *p;

	old = removablefs_override_creds(dentry->d_sb);
	len = vfs_listxattr(lower, NULL, 0);
	if (len <= 0)
		goto out;

	buf = kvmalloc(len, GFP_KERNEL);
	if (!buf) {
		len = -ENOMEM;
		goto out;
	}
	len = vfs_listxattr(lower, buf, len);
	if (len <= 0)
		goto free;

	for (p = buf; p < buf + len; p += strlen(p) + 1) {
		size_t n = strlen(p) + 1;

		if (strncmp(p, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
			continue;
		if (list) {
			if (out + n > size) {
				out = -ERANGE;
				break;
			}
			memcpy(list + out, p, n);
		}
		out += n;
	}
	len = out;
free:
	kvfree(buf);
out:
	removablefs_revert_creds(old);
	return len;
}

const struct inode_operations removablefs_file_iops = {
	.getattr	= removablefs_getattr,
	.setattr	= removablefs_setattr,
	.listxattr	= removablefs_listxattr,
};

const struct inode_operations removablefs_symlink_iops = {
	.get_link	= removablefs_get_link,
	.getattr	= removablefs_getattr,
	.setattr	= removablefs_setattr,
	.listxattr	= removablefs_listxattr,
};

const struct inode_operations removablefs_special_iops = {
	.getattr	= removablefs_getattr,
	.setattr	= removablefs_setattr,
	.listxattr	= removablefs_listxattr,
};
