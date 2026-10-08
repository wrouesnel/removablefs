// SPDX-License-Identifier: GPL-2.0-only
/*
 * ownfs superblock, mount options and module setup.
 *
 * Mount options:
 *   uid=N, gid=N         owner presented for every file (default: mounter)
 *   lower=TYPE           the device's real filesystem (default: probe)
 *   diskuid=N, diskgid=N owner written to disk for new files
 *                        (default: owner of the lower root directory)
 *   quiet                chown/chgrp to another owner succeeds silently
 * Anything else is handed to the lower filesystem.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/parser.h>
#include <linux/string.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#ifdef HAVE_FS_CONTEXT
#include <linux/fs_context.h>
#endif

#include "ownfs.h"

#define OWNFS_VERSION "0.1.0"

static const struct super_operations ownfs_sops;

/* Tried in order when lower= is not given. */
static const char *const ownfs_probe_types[] = {
	"ext4", "xfs", "btrfs", "f2fs", NULL
};

static void ownfs_free_opts(struct ownfs_mount_opts *o)
{
	kfree(o->source);
	kfree(o->lower_type);
	kfree(o->lower_data);
	memset(o, 0, sizeof(*o));
}

static int ownfs_append_lower(struct ownfs_mount_opts *o, const char *key,
			      const char *val)
{
	size_t need = strlen(key) + (val ? strlen(val) + 1 : 0) + 2;
	size_t used = o->lower_data ? strlen(o->lower_data) : 0;
	char *buf;

	if (used + need > o->lower_data_len) {
		size_t len = max_t(size_t, 2 * (used + need), 64);

		buf = krealloc(o->lower_data, len, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		if (!o->lower_data)
			buf[0] = '\0';
		o->lower_data = buf;
		o->lower_data_len = len;
	}
	buf = o->lower_data + used;
	if (used)
		*buf++ = ',';
	if (val)
		sprintf(buf, "%s=%s", key, val);
	else
		strcpy(buf, key);
	return 0;
}

static int ownfs_parse_id(const char *val, u32 *out)
{
	if (!val || !*val)
		return -EINVAL;
	return kstrtou32(val, 10, out);
}

/* One option, already split at '='.  @val is NULL for flags. */
static int ownfs_parse_opt(struct ownfs_mount_opts *o, const char *key,
			   const char *val)
{
	struct user_namespace *ns = current_user_ns();
	u32 id;
	int err;

	if (!strcmp(key, "uid") || !strcmp(key, "diskuid")) {
		kuid_t uid;

		err = ownfs_parse_id(val, &id);
		if (err)
			return err;
		uid = make_kuid(ns, id);
		if (!uid_valid(uid))
			return -EINVAL;
		if (key[0] == 'u') {
			o->uid = uid;
			o->uid_set = true;
		} else {
			o->disk_uid = uid;
			o->disk_uid_set = true;
		}
		return 0;
	}
	if (!strcmp(key, "gid") || !strcmp(key, "diskgid")) {
		kgid_t gid;

		err = ownfs_parse_id(val, &id);
		if (err)
			return err;
		gid = make_kgid(ns, id);
		if (!gid_valid(gid))
			return -EINVAL;
		if (key[0] == 'g') {
			o->gid = gid;
			o->gid_set = true;
		} else {
			o->disk_gid = gid;
			o->disk_gid_set = true;
		}
		return 0;
	}
	if (!strcmp(key, "lower")) {
		if (!val || !*val || strlen(val) >= OWNFS_TYPE_LEN)
			return -EINVAL;
		kfree(o->lower_type);
		o->lower_type = kstrdup(val, GFP_KERNEL);
		return o->lower_type ? 0 : -ENOMEM;
	}
	if (!strcmp(key, "quiet")) {
		o->quiet = true;
		return 0;
	}
	return ownfs_append_lower(o, key, val);
}

#ifndef HAVE_FS_CONTEXT
/* Parse a legacy comma-separated option string. */
static int ownfs_parse_string(struct ownfs_mount_opts *o, char *data)
{
	char *p;
	int err;

	while ((p = strsep(&data, ",")) != NULL) {
		char *val;

		if (!*p)
			continue;
		val = strchr(p, '=');
		if (val)
			*val++ = '\0';
		err = ownfs_parse_opt(o, p, val);
		if (err) {
			pr_err("ownfs: bad mount option '%s'\n", p);
			return err;
		}
	}
	return 0;
}
#endif

/* put_filesystem() is not exported; it is just this. */
static void ownfs_put_filesystem(struct file_system_type *fstype)
{
	module_put(fstype->owner);
}

static struct vfsmount *ownfs_mount_lower_type(const char *type, int flags,
					       struct ownfs_mount_opts *o)
{
	struct file_system_type *fstype;
	struct vfsmount *mnt;

	if (!strcmp(type, OWNFS_NAME))
		return ERR_PTR(-EINVAL);
	fstype = get_fs_type(type);
	if (!fstype)
		return ERR_PTR(-ENODEV);
	if (!(fstype->fs_flags & FS_REQUIRES_DEV)) {
		/* Only block-device filesystems make sense underneath. */
		ownfs_put_filesystem(fstype);
		return ERR_PTR(-EINVAL);
	}
	mnt = vfs_kern_mount(fstype, flags, o->source, o->lower_data);
	ownfs_put_filesystem(fstype);
	return mnt;
}

static struct vfsmount *ownfs_mount_lower(struct ownfs_mount_opts *o,
					  int flags, char *type_out,
					  size_t type_len)
{
	struct vfsmount *mnt = ERR_PTR(-EINVAL);
	int i;

	if (o->lower_type) {
		mnt = ownfs_mount_lower_type(o->lower_type, flags, o);
		if (!IS_ERR(mnt))
			strscpy(type_out, o->lower_type, type_len);
		return mnt;
	}
	for (i = 0; ownfs_probe_types[i]; i++) {
		mnt = ownfs_mount_lower_type(ownfs_probe_types[i], flags, o);
		if (!IS_ERR(mnt)) {
			strscpy(type_out, ownfs_probe_types[i], type_len);
			return mnt;
		}
		/* Lack of memory or a busy device won't improve on retry. */
		if (PTR_ERR(mnt) == -ENOMEM || PTR_ERR(mnt) == -EBUSY)
			break;
	}
	return mnt;
}

/* The dev_t of the block device at @path, if it is one; else unchanged. */
static void ownfs_source_dev(const char *path, dev_t *dev)
{
#if defined(HAVE_LOOKUP_BDEV_DEV)
	dev_t d;

	if (!lookup_bdev(path, &d))
		*dev = d;
#elif defined(HAVE_LOOKUP_BDEV_PTR)
	struct block_device *bdev = lookup_bdev(path);

	if (!IS_ERR(bdev)) {
		*dev = bdev->bd_dev;
		bdput(bdev);
	}
#endif
}

/*
 * The credentials every lower operation runs with: the mounter's (udisksd,
 * normally), with fsuid/fsgid set to the drive's owner so new files are
 * stored as that owner, no supplementary groups, and only the capabilities
 * needed to bypass the lower fs's own DAC checks.  Permission was already
 * checked at the ownfs layer.  Deliberately absent: CAP_SYS_RESOURCE (no
 * dipping into reserved blocks), CAP_SYS_ADMIN (no trusted.* xattrs),
 * CAP_FSETID and CAP_CHOWN.
 */
static const struct cred *ownfs_make_cred(kuid_t uid, kgid_t gid)
{
	struct group_info *groups;
	struct cred *cred;
	kernel_cap_t caps = CAP_EMPTY_SET;

	cred = prepare_creds();
	if (!cred)
		return NULL;

	groups = groups_alloc(0);
	if (!groups) {
		abort_creds(cred);
		return NULL;
	}
	set_groups(cred, groups);
	put_group_info(groups);

	cred->fsuid = uid;
	cred->fsgid = gid;
	cap_raise(caps, CAP_DAC_OVERRIDE);
	cap_raise(caps, CAP_DAC_READ_SEARCH);
	cap_raise(caps, CAP_FOWNER);
	cred->cap_effective = caps;
	return cred;
}

static int ownfs_fill_super(struct super_block *sb,
			    struct ownfs_mount_opts *o, int silent)
{
	struct ownfs_sb_info *sbi;
	struct super_block *lower_sb;
	struct dentry *lower_root;
	struct inode *root;
	int err;

	if (!o->source || !*o->source) {
		if (!silent)
			pr_err("ownfs: no block device given\n");
		return -EINVAL;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	sbi->lower_mnt = ownfs_mount_lower(o, sb->s_flags & SB_RDONLY,
					   sbi->lower_type,
					   sizeof(sbi->lower_type));
	if (IS_ERR(sbi->lower_mnt)) {
		err = PTR_ERR(sbi->lower_mnt);
		sbi->lower_mnt = NULL;
		if (!silent)
			pr_err("ownfs: cannot mount %s: %d\n", o->source, err);
		return err;
	}
	lower_sb = sbi->lower_mnt->mnt_sb;
	lower_root = sbi->lower_mnt->mnt_root;

	err = -EINVAL;
	if (lower_sb->s_stack_depth >= FILESYSTEM_MAX_STACK_DEPTH) {
		pr_err("ownfs: maximum fs stacking depth exceeded\n");
		return err;
	}

	sbi->uid = o->uid_set ? o->uid : current_fsuid();
	sbi->gid = o->gid_set ? o->gid : current_fsgid();
	sbi->disk_uid = o->disk_uid_set ? o->disk_uid : d_inode(lower_root)->i_uid;
	sbi->disk_gid = o->disk_gid_set ? o->disk_gid : d_inode(lower_root)->i_gid;
	sbi->quiet = o->quiet;

	err = -ENOMEM;
	sbi->cred = ownfs_make_cred(sbi->disk_uid, sbi->disk_gid);
	if (!sbi->cred)
		return err;

	sb->s_magic = OWNFS_MAGIC;
	sb->s_op = &ownfs_sops;
	sb->s_xattr = ownfs_xattr_handlers;
	ownfs_set_default_d_op(sb, &ownfs_dops);
	sb->s_stack_depth = lower_sb->s_stack_depth + 1;
	sb->s_maxbytes = lower_sb->s_maxbytes;
	sb->s_blocksize = lower_sb->s_blocksize;
	sb->s_blocksize_bits = lower_sb->s_blocksize_bits;
	sb->s_time_gran = lower_sb->s_time_gran;
#ifdef HAVE_SB_TIME_MINMAX
	sb->s_time_min = lower_sb->s_time_min;
	sb->s_time_max = lower_sb->s_time_max;
#endif
	if (sb_rdonly(lower_sb))
		sb->s_flags |= SB_RDONLY;

	/*
	 * Report the lower device as ours, so /proc/self/mountinfo ties this
	 * mount to the block device just as a plain mount would.  udisks and
	 * the desktop then show the drive as mounted (and offer to eject it),
	 * and st_dev from stat() agrees with mountinfo.  The anonymous dev_t
	 * goes back before kill_anon_super() frees it.
	 *
	 * btrfs has an anonymous s_dev of its own; udisks2 only resolves those
	 * through the mount source for fstype "btrfs", so use the block device
	 * itself.  st_dev then stays btrfs's per-subvolume value, as it must:
	 * inode numbers repeat across subvolumes.
	 */
	sbi->anon_dev = sb->s_dev;
	sb->s_dev = lower_sb->s_dev;
	if (MAJOR(sb->s_dev) == 0)
		ownfs_source_dev(o->source, &sb->s_dev);

	root = ownfs_iget(sb, d_inode(lower_root));
	if (IS_ERR(root))
		return PTR_ERR(root);
	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;
	sb->s_root->d_fsdata = dget(lower_root);
	return 0;
}

static void ownfs_kill_sb(struct super_block *sb)
{
	struct ownfs_sb_info *sbi = OWNFS_SB(sb);

	if (sbi && sbi->anon_dev)
		sb->s_dev = sbi->anon_dev;
	kill_anon_super(sb);
	if (sbi) {
		/* Unmounts the lower filesystem; the device is then free. */
		if (sbi->lower_mnt)
			mntput(sbi->lower_mnt);
		if (sbi->cred)
			put_cred(sbi->cred);
		kfree(sbi);
	}
}

static struct inode *ownfs_alloc_inode(struct super_block *sb)
{
	struct ownfs_inode_info *oi;

#ifdef HAVE_ALLOC_INODE_SB
	oi = alloc_inode_sb(sb, ownfs_inode_cachep, GFP_KERNEL);
#else
	oi = kmem_cache_alloc(ownfs_inode_cachep, GFP_KERNEL);
#endif
	if (!oi)
		return NULL;
	oi->lower = NULL;
	return &oi->vfs_inode;
}

static void ownfs_evict_inode(struct inode *inode)
{
	struct inode *lower = ownfs_lower_inode(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	OWNFS_I(inode)->lower = NULL;
	iput(lower);
}

#ifdef HAVE_FREE_INODE
static void ownfs_free_inode(struct inode *inode)
{
	kmem_cache_free(ownfs_inode_cachep, OWNFS_I(inode));
}
#else
static void ownfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(ownfs_inode_cachep, OWNFS_I(inode));
}

static void ownfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, ownfs_i_callback);
}
#endif

static int ownfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct path lower_path = {
		.mnt	= ownfs_lower_mnt(dentry->d_sb),
		.dentry	= ownfs_lower(dentry),
	};
	const struct cred *old;
	int err;

	old = ownfs_override_creds(dentry->d_sb);
	err = vfs_statfs(&lower_path, buf);
	ownfs_revert_creds(old);
	if (!err)
		buf->f_type = OWNFS_MAGIC;
	return err;
}

static int ownfs_sync_fs(struct super_block *sb, int wait)
{
	struct super_block *lower_sb = ownfs_lower_mnt(sb)->mnt_sb;
	int err;

	if (!wait || sb_rdonly(lower_sb))
		return 0;
	down_read(&lower_sb->s_umount);
	err = sync_filesystem(lower_sb);
	up_read(&lower_sb->s_umount);
	return err;
}

static int ownfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct ownfs_sb_info *sbi = OWNFS_SB(root->d_sb);

	seq_printf(m, ",uid=%u", from_kuid_munged(&init_user_ns, sbi->uid));
	seq_printf(m, ",gid=%u", from_kgid_munged(&init_user_ns, sbi->gid));
	seq_printf(m, ",lower=%s", sbi->lower_type);
	seq_printf(m, ",diskuid=%u",
		   from_kuid_munged(&init_user_ns, sbi->disk_uid));
	seq_printf(m, ",diskgid=%u",
		   from_kgid_munged(&init_user_ns, sbi->disk_gid));
	if (sbi->quiet)
		seq_puts(m, ",quiet");
	return 0;
}

/* Going read-write is only possible if the lower fs already is. */
static int ownfs_check_remount(struct super_block *sb, unsigned int sb_flags)
{
	struct super_block *lower_sb = ownfs_lower_mnt(sb)->mnt_sb;

	if (!(sb_flags & SB_RDONLY) && sb_rdonly(lower_sb))
		return -EROFS;
	return 0;
}

#ifndef HAVE_FS_CONTEXT
static int ownfs_remount_fs(struct super_block *sb, int *flags, char *data)
{
	sync_filesystem(sb);
	return ownfs_check_remount(sb, *flags);
}
#endif

static const struct super_operations ownfs_sops = {
	.alloc_inode	= ownfs_alloc_inode,
#ifdef HAVE_FREE_INODE
	.free_inode	= ownfs_free_inode,
#else
	.destroy_inode	= ownfs_destroy_inode,
#endif
	.evict_inode	= ownfs_evict_inode,
#ifdef HAVE_INODE_JUST_DROP
	.drop_inode	= inode_just_drop,
#else
	.drop_inode	= generic_delete_inode,
#endif
	.statfs		= ownfs_statfs,
	.sync_fs	= ownfs_sync_fs,
	.show_options	= ownfs_show_options,
#ifndef HAVE_FS_CONTEXT
	.remount_fs	= ownfs_remount_fs,
#endif
};

#ifdef HAVE_FS_CONTEXT
static int ownfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct ownfs_mount_opts *o = fc->fs_private;
	const char *val = NULL;
	int err;

	/* Let the VFS take "source" itself. */
	if (!strcmp(param->key, "source"))
		return -ENOPARAM;

	switch (param->type) {
	case fs_value_is_flag:
		break;
	case fs_value_is_string:
		val = param->string;
		break;
	default:
		return invalf(fc, "ownfs: %s: unsupported value type",
			      param->key);
	}
	err = ownfs_parse_opt(o, param->key, val);
	if (err)
		return invalf(fc, "ownfs: bad mount option '%s'", param->key);
	return 0;
}

static int ownfs_fill_super_fc(struct super_block *sb, struct fs_context *fc)
{
	struct ownfs_mount_opts *o = fc->fs_private;

	kfree(o->source);
	o->source = kstrdup(fc->source, GFP_KERNEL);
	if (fc->source && !o->source)
		return -ENOMEM;
	return ownfs_fill_super(sb, o, fc->sb_flags & SB_SILENT);
}

static int ownfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, ownfs_fill_super_fc);
}

static int ownfs_reconfigure(struct fs_context *fc)
{
	sync_filesystem(fc->root->d_sb);
	return ownfs_check_remount(fc->root->d_sb, fc->sb_flags);
}

static void ownfs_free_fc(struct fs_context *fc)
{
	struct ownfs_mount_opts *o = fc->fs_private;

	if (o) {
		ownfs_free_opts(o);
		kfree(o);
	}
}

static const struct fs_context_operations ownfs_context_ops = {
	.parse_param	= ownfs_parse_param,
	.get_tree	= ownfs_get_tree,
	.reconfigure	= ownfs_reconfigure,
	.free		= ownfs_free_fc,
};

static int ownfs_init_fs_context(struct fs_context *fc)
{
	struct ownfs_mount_opts *o;

	o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (!o)
		return -ENOMEM;
	fc->fs_private = o;
	fc->ops = &ownfs_context_ops;
	return 0;
}
#else /* !HAVE_FS_CONTEXT */
static int ownfs_fill_super_legacy(struct super_block *sb, void *data,
				   int silent)
{
	return ownfs_fill_super(sb, data, silent);
}

static struct dentry *ownfs_mount(struct file_system_type *fs_type, int flags,
				  const char *dev_name, void *data)
{
	struct ownfs_mount_opts o = {};
	struct dentry *root;
	int err;

	o.source = kstrdup(dev_name, GFP_KERNEL);
	if (dev_name && !o.source)
		return ERR_PTR(-ENOMEM);
	if (data) {
		char *copy = kstrdup(data, GFP_KERNEL);

		if (!copy) {
			ownfs_free_opts(&o);
			return ERR_PTR(-ENOMEM);
		}
		err = ownfs_parse_string(&o, copy);
		kfree(copy);
		if (err) {
			ownfs_free_opts(&o);
			return ERR_PTR(err);
		}
	}
	root = mount_nodev(fs_type, flags, &o, ownfs_fill_super_legacy);
	ownfs_free_opts(&o);
	return root;
}
#endif /* HAVE_FS_CONTEXT */

static struct file_system_type ownfs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= OWNFS_NAME,
#ifdef HAVE_FS_CONTEXT
	.init_fs_context	= ownfs_init_fs_context,
#else
	.mount			= ownfs_mount,
#endif
	.kill_sb		= ownfs_kill_sb,
	/*
	 * No FS_REQUIRES_DEV: ownfs has no block device of its own, and
	 * libmount/udisks only need the source string.  No FS_USERNS_MOUNT:
	 * only the initial namespace's root (udisksd) may mount it.
	 */
};
MODULE_ALIAS_FS(OWNFS_NAME);

static void ownfs_inode_init_once(void *p)
{
	struct ownfs_inode_info *oi = p;

	inode_init_once(&oi->vfs_inode);
}

static int __init ownfs_init(void)
{
	int err;

	ownfs_inode_cachep = kmem_cache_create("ownfs_inode",
					       sizeof(struct ownfs_inode_info),
					       0, SLAB_RECLAIM_ACCOUNT |
					       SLAB_ACCOUNT,
					       ownfs_inode_init_once);
	if (!ownfs_inode_cachep)
		return -ENOMEM;

	err = register_filesystem(&ownfs_fs_type);
	if (err) {
		kmem_cache_destroy(ownfs_inode_cachep);
		return err;
	}
	pr_info("ownfs " OWNFS_VERSION " loaded\n");
	return 0;
}

static void __exit ownfs_exit(void)
{
	unregister_filesystem(&ownfs_fs_type);
	/* Make sure RCU-freed inodes are gone before the cache goes. */
	rcu_barrier();
	kmem_cache_destroy(ownfs_inode_cachep);
}

module_init(ownfs_init);
module_exit(ownfs_exit);

MODULE_DESCRIPTION("Present a POSIX filesystem as owned by the mounting user");
MODULE_AUTHOR("Will Rouesnel <wrouesnel@wrouesnel.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION(OWNFS_VERSION);
