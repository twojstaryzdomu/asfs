/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta12
 *  
 * Copyright (C) 2003,2004,2005,2006  Marek 'March' Szyprowski <marek@amiga.pl>
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/smp_lock.h>
#include <linux/time.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/dirent.h>
#include "asfs_fs.h"

#include <asm/byteorder.h>

#ifdef CONFIG_ASFS_RW
static int asfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd);
static int asfs_mkdir(struct inode *dir, struct dentry *dentry, int mode);
static int asfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname);
static int asfs_rmdir(struct inode *dir, struct dentry *dentry);
static int asfs_unlink(struct inode *dir, struct dentry *dentry);
static int asfs_rename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry);
/*static int asfs_notify_change(struct dentry *dentry, struct iattr *attr);*/
#endif

/* Mapping from our types to the kernel */

static struct address_space_operations asfs_aops = {
	.readpage	= asfs_readpage,
	.sync_page	= block_sync_page,
	.bmap		= asfs_bmap,
#ifdef CONFIG_ASFS_RW
	.writepage	= asfs_writepage,
	.write_begin = asfs_write_begin,
	.write_end = generic_write_end,
#endif
};

static struct file_operations asfs_file_operations = {
	.llseek		= generic_file_llseek,
	.aio_read	= generic_file_aio_read,
	.mmap		= generic_file_mmap,
	.splice_read = generic_file_splice_read,
#ifdef CONFIG_ASFS_RW
	.aio_write	= generic_file_aio_write,
	.open		= asfs_file_open,
	.release	= asfs_file_release,
	.fsync		= file_fsync,
#endif
};

static struct file_operations asfs_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= asfs_readdir,
	.llseek		= generic_file_llseek,
};

static struct inode_operations asfs_dir_inode_operations = {
	.lookup		= asfs_lookup,
#ifdef CONFIG_ASFS_RW
	.create		= asfs_create,
	.unlink		= asfs_unlink,
	.symlink	= asfs_symlink,
	.mkdir		= asfs_mkdir,
	.rmdir		= asfs_rmdir,
	.rename		= asfs_rename,
/*	.setattr	= asfs_notify_change,*/
#endif
};

static struct inode_operations asfs_file_inode_operations = {
#ifdef CONFIG_ASFS_RW
	.truncate	= asfs_truncate,
/*	.setattr		= asfs_notify_change,*/
#endif
};

static struct address_space_operations asfs_symlink_aops = {
	.readpage	= asfs_symlink_readpage,
};

static struct inode_operations asfs_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= page_follow_link_light,
	.put_link	= page_put_link,
#ifdef CONFIG_ASFS_RW
/*	.setattr	= asfs_notify_change,*/
#endif
};

void asfs_read_locked_inode(struct inode *inode, void *arg)
{
	struct super_block *sb = inode->i_sb;
	struct fsObject *obj = arg;

	inode->i_mode = ASFS_SB(sb)->mode;
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec = inode->i_ctime.tv_sec = be32_to_cpu(obj->datemodified) + (365*8+2)*24*60*60;  
	/* Linux: seconds since 01-01-1970, AmigaSFS: seconds since 01-01-1978 */
	inode->i_mtime.tv_nsec = inode->i_ctime.tv_nsec = inode->i_atime.tv_nsec = 0;
	inode->i_uid = ASFS_SB(sb)->uid;
	inode->i_gid = ASFS_SB(sb)->gid;
	atomic_set(&ASFS_I(inode)->i_opencnt, 0);

	asfs_debug("asfs_read_inode2: Setting-up node %lu... ", inode->i_ino);

	if (obj->bits & OTYPE_DIR) {
		asfs_debug("dir (FirstdirBlock: %u, HashTable %u)\n", \
		           be32_to_cpu(obj->object.dir.firstdirblock), be32_to_cpu(obj->object.dir.hashtable));

		inode->i_size = 0;
		inode->i_op = &asfs_dir_inode_operations;
		inode->i_fop = &asfs_dir_operations;
		inode->i_mode |= S_IFDIR | ((inode->i_mode & 0400) ? 0100 : 0) | 
		              ((inode->i_mode & 0040) ? 0010 : 0) | ((inode->i_mode & 0004) ? 0001 : 0);
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.dir.firstdirblock);
		ASFS_I(inode)->hashtable = be32_to_cpu(obj->object.dir.hashtable);
		ASFS_I(inode)->modified = 0;
	} else if (obj->bits & OTYPE_LINK && !(obj->bits & OTYPE_HARDLINK)) {
		asfs_debug("symlink\n");
		inode->i_size = 0;
		inode->i_op = &asfs_symlink_inode_operations;
		inode->i_data.a_ops = &asfs_symlink_aops;
		inode->i_mode |= S_IFLNK | S_IRWXUGO;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.file.data);
	} else {
		asfs_debug("file (Size: %u, FirstBlock: %u)\n", be32_to_cpu(obj->object.file.size), be32_to_cpu(obj->object.file.data));
		inode->i_size = be32_to_cpu(obj->object.file.size);
		inode->i_blocks = (be32_to_cpu(obj->object.file.size) + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
		inode->i_op = &asfs_file_inode_operations;
		inode->i_fop = &asfs_file_operations;
		inode->i_mapping->a_ops = &asfs_aops;
		inode->i_mode |= S_IFREG;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.file.data);
		ASFS_I(inode)->ext_cache.startblock = 0;
		ASFS_I(inode)->ext_cache.key = 0;
		ASFS_I(inode)->mmu_private = inode->i_size;
	}
	return;	
}

struct inode *asfs_get_root_inode(struct super_block *sb)
{
	struct inode *result = NULL;
	struct fsObject *obj;
	struct buffer_head *bh;

	asfs_debug("asfs_get_root_inode\n");

	if ((bh = asfs_breadcheck(sb, ASFS_SB(sb)->rootobjectcontainer, ASFS_OBJECTCONTAINER_ID))) {
		obj = &(((struct fsObjectContainer *)bh->b_data)->object[0]);
		if (be32_to_cpu(obj->objectnode) > 0)
			result = iget_locked(sb, be32_to_cpu(obj->objectnode));

		if (result != NULL && result->i_state & I_NEW) {
			asfs_read_locked_inode(result, obj);
			unlock_new_inode(result);
		}
		asfs_brelse(bh);
	}
	return result;
}

#ifdef CONFIG_ASFS_RW

static void asfs_sync_dir_inode(struct inode *dir, struct fsObject *obj)
{
	ASFS_I(dir)->firstblock = be32_to_cpu(obj->object.dir.firstdirblock);
	ASFS_I(dir)->modified = 1;
	dir->i_mtime = dir->i_atime = dir->i_ctime = CURRENT_TIME;
	obj->datemodified = cpu_to_be32(dir->i_mtime.tv_sec - (365*8+2)*24*60*60);
}

enum { it_file, it_dir, it_link };

static int asfs_create_object(struct inode *dir, struct dentry *dentry, int mode, int type, const char *symname)
{
	int error;
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct buffer_head *bh, *dir_bh;
	struct fsObject obj_data, *dir_obj, *obj;
	u8 *name = (u8 *) dentry->d_name.name;
	u8 bufname[ASFS_MAXFN_BUF];

	asfs_debug("asfs_create_obj %s in dir node %d\n", name, (int)dir->i_ino);

	asfs_translate(bufname, name, ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io, ASFS_MAXFN_BUF);
	if ((error = asfs_check_name(bufname, strlen(bufname))) != 0)
		return error;

	sb = dir->i_sb;
	inode = new_inode(sb);
	if (!inode)
		return -ENOMEM;

	memset(&obj_data, 0, sizeof(struct fsObject));

	obj_data.protection = cpu_to_be32(FIBF_READ|FIBF_WRITE|FIBF_EXECUTE|FIBF_DELETE);
	obj_data.datemodified = cpu_to_be32(inode->i_mtime.tv_sec - (365*8+2)*24*60*60);
	switch (type) {
	case it_dir:
		obj_data.bits = OTYPE_DIR;
		break;
	case it_link:
		obj_data.bits = OTYPE_LINK;
		break;
	default:
		break;
	}

	lock_super(sb);

	if ((error = asfs_readobject(sb, dir->i_ino, &dir_bh, &dir_obj)) != 0) {
		dec_count(inode);
		unlock_super(sb);
		return error;
	}

	bh = dir_bh;
	obj = dir_obj;

	if ((error = asfs_createobject(sb, &bh, &obj, &obj_data, bufname, FALSE)) != 0) {
		asfs_brelse(dir_bh);
		dec_count(inode);
		unlock_super(sb);
		return error;
	}

	inode->i_ino = be32_to_cpu(obj->objectnode);
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_size = inode->i_blocks = 0;
	inode->i_uid = dir->i_uid;
	inode->i_gid = dir->i_gid;
	inode->i_mode = mode | ASFS_SB(sb)->mode;

	switch (type) {
	case it_dir:
		inode->i_mode |= S_IFDIR;
		inode->i_op = &asfs_dir_inode_operations;
		inode->i_fop = &asfs_dir_operations;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.dir.firstdirblock);
		ASFS_I(inode)->hashtable = be32_to_cpu(obj->object.dir.hashtable);
		ASFS_I(inode)->modified = 0;
		break;
	case it_file:
		inode->i_mode |= S_IFREG;
		inode->i_op = &asfs_file_inode_operations;
		inode->i_fop = &asfs_file_operations;
		inode->i_mapping->a_ops = &asfs_aops;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.file.data);
		ASFS_I(inode)->ext_cache.startblock = 0;
		ASFS_I(inode)->ext_cache.key = 0;
		ASFS_I(inode)->mmu_private = inode->i_size;
		break;
	case it_link:
		inode->i_mode = S_IFLNK | S_IRWXUGO;
		inode->i_op = &page_symlink_inode_operations;
		inode->i_mapping->a_ops = &asfs_symlink_aops;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.file.data);
		error = asfs_write_symlink(inode, symname);
		break;
	default:
		break;
	}

	asfs_bstore(sb, bh);
	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
	asfs_sync_dir_inode(dir, dir_obj);
	asfs_bstore(sb, dir_bh); 

	unlock_super(sb);
	asfs_brelse(bh);
	asfs_brelse(dir_bh);
	
	return error;
}

static int asfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
	return asfs_create_object(dir, dentry, mode, it_file, NULL);
}

static int asfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	return asfs_create_object(dir, dentry, mode, it_dir, NULL);
}

static int asfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	return asfs_create_object(dir, dentry, 0, it_link, symname);
}

static int asfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	asfs_debug("ASFS: %s\n", __FUNCTION__);

	if (ASFS_I(dentry->d_inode)->firstblock != 0)
		return -ENOTEMPTY;
	
	return asfs_unlink(dir, dentry);
}

static int asfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	int error;
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh, *dir_bh;
	struct fsObject *dir_obj, *obj;

	asfs_debug("ASFS: %s\n", __FUNCTION__);

	lock_super(sb);

	if ((error = asfs_readobject(sb, inode->i_ino, &bh, &obj)) != 0) {
		unlock_super(sb);
		return error;
	}
	if ((error = asfs_deleteobject(sb, bh, obj)) != 0) {
		asfs_brelse(bh);
		unlock_super(sb);
		return error;
	}
	asfs_brelse(bh);

	/* directory data could change after removing the object */
	if ((error = asfs_readobject(sb, dir->i_ino, &dir_bh, &dir_obj)) != 0) {
		unlock_super(sb);
		return error;
	}

	asfs_sync_dir_inode(dir, dir_obj);
	asfs_bstore(sb, dir_bh); 

	dec_count(inode);
	unlock_super(sb);
	asfs_brelse(dir_bh);

	return 0;
}

static int asfs_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry)
{
	struct super_block *sb = old_dir->i_sb;
	struct buffer_head *src_bh, *old_bh, *new_bh;
	int error;
	struct fsObject *src_obj, *old_obj, *new_obj;
	u8 bufname[ASFS_MAXFN_BUF];

	asfs_debug("ASFS: rename (old=%u,\"%*s\" to new=%u,\"%*s\")\n",
		 (u32)old_dir->i_ino, (int)old_dentry->d_name.len, old_dentry->d_name.name,
		 (u32)new_dir->i_ino, (int)new_dentry->d_name.len, new_dentry->d_name.name);

	asfs_translate(bufname, (u8 *) new_dentry->d_name.name, ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io, ASFS_MAXFN_BUF);
	if ((error = asfs_check_name(bufname, strlen(bufname))) != 0)
		return error;


	/* Unlink destination if it already exists */
	if (new_dentry->d_inode) 
		if ((error = asfs_unlink(new_dir, new_dentry)) != 0)
			return error;

	lock_super(sb);

	if ((error = asfs_readobject(sb, old_dentry->d_inode->i_ino, &src_bh, &src_obj)) != 0) {
		unlock_super(sb);
		return error;
	}
	if ((error = asfs_readobject(sb, new_dir->i_ino, &new_bh, &new_obj)) != 0) {
		asfs_brelse(src_bh);
		unlock_super(sb);
		return error;
	}

	if ((error = asfs_renameobject(sb, src_bh, src_obj, new_bh, new_obj, bufname)) != 0) {
		asfs_brelse(src_bh);
		asfs_brelse(new_bh);
		unlock_super(sb);
		return error;
	}
	asfs_brelse(src_bh);
	asfs_brelse(new_bh);

	if ((error = asfs_readobject(sb, old_dir->i_ino, &old_bh, &old_obj)) != 0) {
		unlock_super(sb);
		return error;
	}
	if ((error = asfs_readobject(sb, new_dir->i_ino, &new_bh, &new_obj)) != 0) {
		asfs_brelse(old_bh);
		unlock_super(sb);
		return error;
	}

	asfs_sync_dir_inode(old_dir, old_obj);
	asfs_sync_dir_inode(new_dir, new_obj);

	asfs_bstore(sb, new_bh);	
	asfs_bstore(sb, old_bh);

	unlock_super(sb);
	asfs_brelse(old_bh);
	asfs_brelse(new_bh);

	mark_inode_dirty(old_dir);
	mark_inode_dirty(new_dir);

	return 0;
}

/*
int asfs_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	int error = 0;

	asfs_debug("ASFS: notify_change(%lu,0x%x)\n",inode->i_ino,attr->ia_valid);

	error = inode_change_ok(inode,attr);

	return error;
}
*/
#endif
