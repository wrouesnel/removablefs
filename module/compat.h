/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel API compatibility for removablefs.
 *
 * Distribution kernels (RHEL above all) backport VFS changes without
 * changing their version number, so every difference handled here is
 * detected by compiling a probe against the target kernel's headers (see
 * ./configure), never by LINUX_VERSION_CODE.  The probes write
 * removablefs_config.h, which the build force-includes ahead of this file.
 */
#ifndef REMOVABLEFS_COMPAT_H
#define REMOVABLEFS_COMPAT_H

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/mm.h>
#include <linux/splice.h>
#include <linux/uio.h>

/*
 * Inode operations take an idmap (6.3+), a user namespace (5.12+) or
 * nothing.  REMOVABLEFS_IOP_NS_DECL declares that leading argument in our own
 * callbacks and REMOVABLEFS_IOP_NS passes it on to helpers acting on our inodes.
 */
#if defined(HAVE_IOP_IDMAP)
#define REMOVABLEFS_IOP_NS_DECL	struct mnt_idmap *removablefs_ns,
#define REMOVABLEFS_IOP_NS		removablefs_ns,
#elif defined(HAVE_IOP_USERNS)
#define REMOVABLEFS_IOP_NS_DECL	struct user_namespace *removablefs_ns,
#define REMOVABLEFS_IOP_NS		removablefs_ns,
#else
#define REMOVABLEFS_IOP_NS_DECL
#define REMOVABLEFS_IOP_NS
#endif

/* The matching leading argument for vfs_*() calls on the lower fs. */
#if defined(HAVE_VFS_IDMAP)
#define REMOVABLEFS_LOWER_NS		&nop_mnt_idmap,
#elif defined(HAVE_VFS_USERNS)
#define REMOVABLEFS_LOWER_NS		&init_user_ns,
#else
#define REMOVABLEFS_LOWER_NS
#endif

#define REMOVABLEFS_IOP_GETATTR_DECL(name) \
	int name(REMOVABLEFS_IOP_NS_DECL const struct path *path, \
		 struct kstat *stat, u32 request_mask, unsigned int flags)
#define REMOVABLEFS_IOP_SETATTR_DECL(name) \
	int name(REMOVABLEFS_IOP_NS_DECL struct dentry *dentry, struct iattr *attr)

#ifdef HAVE_IOP_MKDIR_DENTRY
typedef struct dentry *removablefs_mkdir_ret_t;
#define REMOVABLEFS_MKDIR_RET(err)	((err) ? ERR_PTR(err) : NULL)
#else
typedef int removablefs_mkdir_ret_t;
#define REMOVABLEFS_MKDIR_RET(err)	(err)
#endif

/*
 * Lower-filesystem lookups.  lookup_noperm*() (6.15+) store the name's hash,
 * salted for @base, into the qstr they are given, so they must never see an
 * removablefs dentry's d_name: rewriting the hash of a hashed dentry corrupts the
 * dcache.  Hand them a copy.
 */
static inline struct dentry *removablefs_lookup_locked(struct dentry *base,
						       const struct qstr *name)
{
#if defined(HAVE_LOOKUP_NOPERM)
	struct qstr q = QSTR_INIT(name->name, name->len);

	return lookup_noperm(&q, base);
#else
	return lookup_one_len(name->name, base, name->len);
#endif
}

/* The same, for when the caller doesn't hold the parent locked. */
static inline struct dentry *removablefs_lookup_unlocked(struct dentry *base,
							 const struct qstr *name)
{
#if defined(HAVE_LOOKUP_NOPERM)
	struct qstr q = QSTR_INIT(name->name, name->len);

	return lookup_noperm_unlocked(&q, base);
#else
	return lookup_one_len_unlocked(name->name, base, name->len);
#endif
}

static inline int removablefs_vfs_create(struct inode *dir, struct dentry *dentry,
					 umode_t mode)
{
#ifdef HAVE_VFS_CREATE_NO_EXCL
	return vfs_create(REMOVABLEFS_LOWER_NS dentry, mode, NULL);
#else
	return vfs_create(REMOVABLEFS_LOWER_NS dir, dentry, mode, true);
#endif
}

/*
 * Returns the lower dentry now holding the new directory, which may differ
 * from @dentry.  @dentry's reference is consumed either way, as with the
 * 6.15+ vfs_mkdir().
 */
static inline struct dentry *removablefs_vfs_mkdir(struct inode *dir,
						   struct dentry *dentry,
						   umode_t mode)
{
#if defined(HAVE_VFS_MKDIR_DELEG)
	/*
	 * Since 6.19 (the release that added the delegated_inode argument)
	 * a failing vfs_mkdir() also unlocks @dir via end_creating().  Take
	 * the lock again so that, as with every other removablefs_vfs_*(), the
	 * caller still holds it afterwards.
	 */
	struct dentry *de = vfs_mkdir(REMOVABLEFS_LOWER_NS dir, dentry, mode, NULL);

	if (IS_ERR(de))
		inode_lock_nested(dir, I_MUTEX_PARENT);
	return de;
#elif defined(HAVE_VFS_MKDIR_DENTRY)
	return vfs_mkdir(REMOVABLEFS_LOWER_NS dir, dentry, mode);
#else
	int err = vfs_mkdir(REMOVABLEFS_LOWER_NS dir, dentry, mode);

	if (err) {
		dput(dentry);
		return ERR_PTR(err);
	}
	return dentry;
#endif
}

static inline int removablefs_vfs_mknod(struct inode *dir, struct dentry *dentry,
					umode_t mode, dev_t dev)
{
#ifdef HAVE_VFS_MKNOD_DELEG
	return vfs_mknod(REMOVABLEFS_LOWER_NS dir, dentry, mode, dev, NULL);
#else
	return vfs_mknod(REMOVABLEFS_LOWER_NS dir, dentry, mode, dev);
#endif
}

static inline int removablefs_vfs_symlink(struct inode *dir, struct dentry *dentry,
					  const char *target)
{
#ifdef HAVE_VFS_SYMLINK_DELEG
	return vfs_symlink(REMOVABLEFS_LOWER_NS dir, dentry, target, NULL);
#else
	return vfs_symlink(REMOVABLEFS_LOWER_NS dir, dentry, target);
#endif
}

static inline int removablefs_vfs_link(struct dentry *old, struct inode *dir,
				       struct dentry *new)
{
#if defined(HAVE_VFS_IDMAP) || defined(HAVE_VFS_USERNS)
	return vfs_link(old, REMOVABLEFS_LOWER_NS dir, new, NULL);
#else
	return vfs_link(old, dir, new, NULL);
#endif
}

static inline int removablefs_vfs_unlink(struct inode *dir, struct dentry *dentry)
{
	return vfs_unlink(REMOVABLEFS_LOWER_NS dir, dentry, NULL);
}

static inline int removablefs_vfs_rmdir(struct inode *dir, struct dentry *dentry)
{
#ifdef HAVE_VFS_RMDIR_DELEG
	return vfs_rmdir(REMOVABLEFS_LOWER_NS dir, dentry, NULL);
#else
	return vfs_rmdir(REMOVABLEFS_LOWER_NS dir, dentry);
#endif
}

static inline int removablefs_vfs_rename(struct inode *old_dir, struct dentry *old,
					 struct inode *new_dir, struct dentry *new,
					 unsigned int flags)
{
#if defined(HAVE_RENAMEDATA_PARENT)
	struct renamedata rd = {
		.mnt_idmap	= &nop_mnt_idmap,
		.old_parent	= old->d_parent,
		.old_dentry	= old,
		.new_parent	= new->d_parent,
		.new_dentry	= new,
		.flags		= flags,
	};

	return vfs_rename(&rd);
#elif defined(HAVE_RENAMEDATA)
	struct renamedata rd = {
#if defined(HAVE_VFS_IDMAP)
		.old_mnt_idmap	= &nop_mnt_idmap,
		.new_mnt_idmap	= &nop_mnt_idmap,
#elif defined(HAVE_VFS_USERNS)
		.old_mnt_userns	= &init_user_ns,
		.new_mnt_userns	= &init_user_ns,
#endif
		.old_dir	= old_dir,
		.old_dentry	= old,
		.new_dir	= new_dir,
		.new_dentry	= new,
		.flags		= flags,
	};

	return vfs_rename(&rd);
#else
	return vfs_rename(old_dir, old, new_dir, new, NULL, flags);
#endif
}

static inline int removablefs_notify_change(struct dentry *dentry,
					    struct iattr *attr)
{
	return notify_change(REMOVABLEFS_LOWER_NS dentry, attr, NULL);
}

static inline ssize_t removablefs_vfs_getxattr(struct dentry *dentry,
					       const char *name, void *value,
					       size_t size)
{
	return vfs_getxattr(REMOVABLEFS_LOWER_NS dentry, name, value, size);
}

static inline int removablefs_vfs_setxattr(struct dentry *dentry, const char *name,
					   const void *value, size_t size, int flags)
{
	return vfs_setxattr(REMOVABLEFS_LOWER_NS dentry, name, value, size, flags);
}

static inline int removablefs_vfs_removexattr(struct dentry *dentry,
					      const char *name)
{
	return vfs_removexattr(REMOVABLEFS_LOWER_NS dentry, name);
}

static inline int removablefs_setattr_prepare(REMOVABLEFS_IOP_NS_DECL
					      struct dentry *dentry,
					      struct iattr *attr)
{
	return setattr_prepare(REMOVABLEFS_IOP_NS dentry, attr);
}

/* xattr handler ->set() */
#if defined(HAVE_XATTR_SET_IDMAP)
#define REMOVABLEFS_XATTR_SET_NS_DECL	struct mnt_idmap *removablefs_ns,
#elif defined(HAVE_XATTR_SET_USERNS)
#define REMOVABLEFS_XATTR_SET_NS_DECL	struct user_namespace *removablefs_ns,
#else
#define REMOVABLEFS_XATTR_SET_NS_DECL
#endif

/* Inode timestamps moved behind accessors in 6.6 (ctime) and 6.7. */
static inline void removablefs_copy_times(struct inode *dst, const struct inode *src)
{
#ifdef HAVE_INODE_GET_MTIME
	inode_set_atime_to_ts(dst, inode_get_atime(src));
	inode_set_mtime_to_ts(dst, inode_get_mtime(src));
#else
	dst->i_atime = src->i_atime;
	dst->i_mtime = src->i_mtime;
#endif
#ifdef HAVE_INODE_GET_CTIME
	inode_set_ctime_to_ts(dst, inode_get_ctime(src));
#else
	dst->i_ctime = src->i_ctime;
#endif
}

static inline void removablefs_set_default_d_op(struct super_block *sb,
						const struct dentry_operations *op)
{
#ifdef HAVE_SET_DEFAULT_D_OP
	set_default_d_op(sb, op);
#else
	sb->s_d_op = op;
#endif
}

static inline bool removablefs_inode_is_new(struct inode *inode)
{
#ifdef HAVE_INODE_STATE_READ_ONCE
	return inode_state_read_once(inode) & I_NEW;
#else
	return inode->i_state & I_NEW;
#endif
}

static inline void removablefs_vma_set_file(struct vm_area_struct *vma,
					    struct file *file)
{
#ifdef HAVE_VMA_SET_FILE
	vma_set_file(vma, file);
#else
	get_file(file);
	fput(vma->vm_file);
	vma->vm_file = file;
#endif
}

static inline int removablefs_call_mmap(struct file *file, struct vm_area_struct *vma)
{
#if defined(HAVE_VFS_MMAP)
	return vfs_mmap(file, vma);
#else
	if (!file->f_op->mmap)
		return -ENODEV;
	return file->f_op->mmap(file, vma);
#endif
}

#endif /* REMOVABLEFS_COMPAT_H */
