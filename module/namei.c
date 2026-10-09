// SPDX-License-Identifier: GPL-2.0-only
/*
 * removablefs directory operations.
 *
 * Every removablefs dentry that is positive holds a reference to its lower dentry
 * in d_fsdata.  Negative lower dentries are never kept: operations that
 * create a name look it up again under the lower parent's lock, which keeps
 * them correct without a ->d_revalidate.  The lower filesystem is only
 * reachable through removablefs, so nothing else changes it behind our back.
 */
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>

#include "removablefs.h"

static void removablefs_d_release(struct dentry *dentry)
{
	dput(dentry->d_fsdata);
}

const struct dentry_operations removablefs_dops = {
	.d_release	= removablefs_d_release,
};

static struct dentry *removablefs_lookup(struct inode *dir, struct dentry *dentry,
					 unsigned int flags)
{
	struct dentry *lower_parent = removablefs_lower(dentry->d_parent);
	struct inode *inode = NULL;
	struct dentry *lower;
	const struct cred *old;

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	old = removablefs_override_creds(dir->i_sb);
	lower = removablefs_lookup_unlocked(lower_parent, &dentry->d_name);
	removablefs_revert_creds(old);
	if (IS_ERR(lower))
		return ERR_CAST(lower);

	if (d_really_is_positive(lower)) {
		inode = removablefs_iget(dir->i_sb, d_inode(lower));
		if (IS_ERR(inode)) {
			dput(lower);
			return ERR_CAST(inode);
		}
		dentry->d_fsdata = lower;
	} else {
		dput(lower);
	}
	return d_splice_alias(inode, dentry);
}

/*
 * Lock the lower parent of @dentry and look up its name there, which must
 * not exist yet.  On success the caller must removablefs_end_create().
 */
static struct dentry *removablefs_start_create(struct dentry *dentry)
{
	struct dentry *lower_parent = removablefs_lower(dentry->d_parent);
	struct dentry *lower;

	inode_lock_nested(d_inode(lower_parent), I_MUTEX_PARENT);
	lower = removablefs_lookup_locked(lower_parent, &dentry->d_name);
	if (!IS_ERR(lower) && d_really_is_positive(lower)) {
		dput(lower);
		lower = ERR_PTR(-EEXIST);
	}
	if (IS_ERR(lower))
		inode_unlock(d_inode(lower_parent));
	return lower;
}

static void removablefs_end_create(struct dentry *dentry)
{
	struct dentry *lower_parent = removablefs_lower(dentry->d_parent);

	removablefs_copy_attr(d_inode(dentry->d_parent), d_inode(lower_parent));
	inode_unlock(d_inode(lower_parent));
}

/*
 * Attach the freshly created @lower (whose reference we take over) to the
 * negative removablefs @dentry.  Some filesystems leave the dentry passed to
 * ->mkdir() unhashed and splice another; look the name up again then.
 */
static int removablefs_instantiate(struct dentry *dentry, struct dentry *lower)
{
	struct inode *inode;
	struct dentry *stale;

	if (d_unhashed(lower) || d_really_is_negative(lower)) {
		struct dentry *again;

		again = removablefs_lookup_locked(lower->d_parent, &dentry->d_name);
		dput(lower);
		if (IS_ERR(again))
			return PTR_ERR(again);
		if (d_really_is_negative(again)) {
			dput(again);
			return -ENOENT;
		}
		lower = again;
	}

	inode = removablefs_iget(dentry->d_sb, d_inode(lower));
	if (IS_ERR(inode)) {
		dput(lower);
		return PTR_ERR(inode);
	}
	/* A negative dentry may still hold the lower dentry it had before. */
	stale = dentry->d_fsdata;
	dentry->d_fsdata = lower;
	dput(stale);
	d_instantiate(dentry, inode);
	return 0;
}

enum removablefs_create_kind {
	REMOVABLEFS_CREATE,
	REMOVABLEFS_MKDIR,
	REMOVABLEFS_MKNOD,
	REMOVABLEFS_SYMLINK,
};

static int removablefs_create_object(struct inode *dir, struct dentry *dentry,
				     enum removablefs_create_kind kind, umode_t mode,
				     dev_t rdev, const char *target)
{
	struct vfsmount *mnt = removablefs_lower_mnt(dir->i_sb);
	struct dentry *lower_parent;
	struct dentry *lower;
	struct inode *ldir;
	const struct cred *old;
	int err;

	mode = removablefs_sanitize_mode(mode);

	err = mnt_want_write(mnt);
	if (err)
		return err;
	old = removablefs_override_creds(dir->i_sb);

	lower = removablefs_start_create(dentry);
	if (IS_ERR(lower)) {
		err = PTR_ERR(lower);
		goto out;
	}
	lower_parent = removablefs_lower(dentry->d_parent);
	ldir = d_inode(lower_parent);

	switch (kind) {
	case REMOVABLEFS_CREATE:
		err = removablefs_vfs_create(ldir, lower, mode);
		break;
	case REMOVABLEFS_MKDIR:
		lower = removablefs_vfs_mkdir(ldir, lower, mode);
		err = PTR_ERR_OR_ZERO(lower);
		if (err)
			lower = NULL;
		break;
	case REMOVABLEFS_MKNOD:
		err = removablefs_vfs_mknod(ldir, lower, mode, rdev);
		break;
	case REMOVABLEFS_SYMLINK:
		err = removablefs_vfs_symlink(ldir, lower, target);
		break;
	}

	if (!err)
		err = removablefs_instantiate(dentry, lower);
	else
		dput(lower);
	removablefs_end_create(dentry);
out:
	removablefs_revert_creds(old);
	mnt_drop_write(mnt);
	return err;
}

static int removablefs_create(REMOVABLEFS_IOP_NS_DECL struct inode *dir,
			      struct dentry *dentry, umode_t mode, bool excl)
{
	return removablefs_create_object(dir, dentry, REMOVABLEFS_CREATE,
					 (mode & 07777) | S_IFREG, 0, NULL);
}

static removablefs_mkdir_ret_t removablefs_mkdir(REMOVABLEFS_IOP_NS_DECL struct inode *dir,
						 struct dentry *dentry, umode_t mode)
{
	int err = removablefs_create_object(dir, dentry, REMOVABLEFS_MKDIR,
					    (mode & 07777) | S_IFDIR, 0, NULL);

	return REMOVABLEFS_MKDIR_RET(err);
}

static int removablefs_mknod(REMOVABLEFS_IOP_NS_DECL struct inode *dir,
			     struct dentry *dentry, umode_t mode, dev_t rdev)
{
	/* The VFS has already required CAP_MKNOD for device nodes. */
	return removablefs_create_object(dir, dentry, REMOVABLEFS_MKNOD, mode, rdev, NULL);
}

static int removablefs_symlink(REMOVABLEFS_IOP_NS_DECL struct inode *dir,
			       struct dentry *dentry, const char *target)
{
	return removablefs_create_object(dir, dentry, REMOVABLEFS_SYMLINK, S_IFLNK | 0777,
					 0, target);
}

static int removablefs_link(struct dentry *old_dentry, struct inode *dir,
			    struct dentry *new_dentry)
{
	struct vfsmount *mnt = removablefs_lower_mnt(dir->i_sb);
	struct dentry *lower_old = removablefs_lower(old_dentry);
	struct dentry *lower;
	const struct cred *old;
	int err;

	err = mnt_want_write(mnt);
	if (err)
		return err;
	old = removablefs_override_creds(dir->i_sb);

	lower = removablefs_start_create(new_dentry);
	if (IS_ERR(lower)) {
		err = PTR_ERR(lower);
		goto out;
	}
	err = removablefs_vfs_link(lower_old, d_inode(removablefs_lower(new_dentry->d_parent)),
				   lower);
	if (!err)
		err = removablefs_instantiate(new_dentry, lower);
	else
		dput(lower);
	removablefs_end_create(new_dentry);
	removablefs_copy_attr_dentry(old_dentry);
out:
	removablefs_revert_creds(old);
	mnt_drop_write(mnt);
	return err;
}

static int removablefs_remove(struct inode *dir, struct dentry *dentry, bool is_dir)
{
	struct vfsmount *mnt = removablefs_lower_mnt(dir->i_sb);
	struct dentry *lower_parent = removablefs_lower(dentry->d_parent);
	struct dentry *lower = removablefs_lower(dentry);
	struct inode *ldir = d_inode(lower_parent);
	const struct cred *old;
	int err;

	err = mnt_want_write(mnt);
	if (err)
		return err;
	old = removablefs_override_creds(dir->i_sb);

	inode_lock_nested(ldir, I_MUTEX_PARENT);
	dget(lower);
	if (lower->d_parent != lower_parent || d_unhashed(lower))
		err = -ESTALE;
	else if (is_dir)
		err = removablefs_vfs_rmdir(ldir, lower);
	else
		err = removablefs_vfs_unlink(ldir, lower);
	removablefs_copy_attr(dir, ldir);
	inode_unlock(ldir);

	if (!err) {
		/* nlink of what remains (other hard links, an open file). */
		struct inode *inode = d_inode(dentry);

		set_nlink(inode, removablefs_lower_inode(inode)->i_nlink);
		if (is_dir)
			clear_nlink(inode);
	}
	dput(lower);

	removablefs_revert_creds(old);
	mnt_drop_write(mnt);
	return err;
}

static int removablefs_unlink(struct inode *dir, struct dentry *dentry)
{
	return removablefs_remove(dir, dentry, false);
}

static int removablefs_rmdir(struct inode *dir, struct dentry *dentry)
{
	return removablefs_remove(dir, dentry, true);
}

static int removablefs_rename(REMOVABLEFS_IOP_NS_DECL struct inode *old_dir,
			      struct dentry *old_dentry, struct inode *new_dir,
			      struct dentry *new_dentry, unsigned int flags)
{
	struct vfsmount *mnt = removablefs_lower_mnt(old_dir->i_sb);
	struct dentry *lower_old_parent = removablefs_lower(old_dentry->d_parent);
	struct dentry *lower_new_parent = removablefs_lower(new_dentry->d_parent);
	struct dentry *lower_old = removablefs_lower(old_dentry);
	struct dentry *lower_new = NULL;
	struct dentry *trap;
	const struct cred *old;
	int err;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;

	err = mnt_want_write(mnt);
	if (err)
		return err;
	old = removablefs_override_creds(old_dir->i_sb);

	trap = lock_rename(lower_new_parent, lower_old_parent);
	if (IS_ERR(trap)) {
		err = PTR_ERR(trap);
		goto out;
	}

	err = -ESTALE;
	if (lower_old->d_parent != lower_old_parent || d_unhashed(lower_old))
		goto unlock;
	err = -EINVAL;
	if (lower_old == trap)
		goto unlock;

	lower_new = removablefs_lookup_locked(lower_new_parent, &new_dentry->d_name);
	if (IS_ERR(lower_new)) {
		err = PTR_ERR(lower_new);
		lower_new = NULL;
		goto unlock;
	}
	err = -ENOTEMPTY;
	if (lower_new == trap)
		goto unlock;
	/* What we think is at the target must be what is really there. */
	err = -ESTALE;
	if (d_really_is_positive(new_dentry) ?
	    lower_new != removablefs_lower(new_dentry) :
	    d_really_is_positive(lower_new))
		goto unlock;

	err = removablefs_vfs_rename(d_inode(lower_old_parent), lower_old,
				     d_inode(lower_new_parent), lower_new, flags);
unlock:
	removablefs_copy_attr(old_dir, d_inode(lower_old_parent));
	if (new_dir != old_dir)
		removablefs_copy_attr(new_dir, d_inode(lower_new_parent));
	unlock_rename(lower_new_parent, lower_old_parent);
	dput(lower_new);
	if (!err) {
		removablefs_copy_attr_dentry(old_dentry);
		removablefs_copy_attr_dentry(new_dentry);
	}
out:
	removablefs_revert_creds(old);
	mnt_drop_write(mnt);
	return err;
}

const struct inode_operations removablefs_dir_iops = {
	.lookup		= removablefs_lookup,
	.create		= removablefs_create,
	.mkdir		= removablefs_mkdir,
	.mknod		= removablefs_mknod,
	.symlink	= removablefs_symlink,
	.link		= removablefs_link,
	.unlink		= removablefs_unlink,
	.rmdir		= removablefs_rmdir,
	.rename		= removablefs_rename,
	.getattr	= removablefs_getattr,
	.setattr	= removablefs_setattr,
	.listxattr	= removablefs_listxattr,
};
