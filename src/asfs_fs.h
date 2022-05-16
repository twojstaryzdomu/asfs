#ifndef __LINUX_ASFS_FS_H
#define __LINUX_ASFS_FS_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <asm/byteorder.h>
#include "amigasfs.h"

#define asfs_debug(fmt,arg...) /* no debug at all */
//#define asfs_debug(fmt,arg...) printk(fmt,##arg)  /* general debug infos */

#if !defined (__BIG_ENDIAN) && !defined (__LITTLE_ENDIAN)
#error Endianes must be known for ASFS to work. Sorry.
#endif

#define ASFS_MAXFN_BUF (ASFS_MAXFN + 4)
#define ASFS_DEFAULT_UID 0
#define ASFS_DEFAULT_GID 0
#define ASFS_DEFAULT_MODE 0644	/* default permission bits for files, dirs have same permission, but with "x" set */

/* Extent structure located in RAM (e.g. inside inode structure), 
   currently used to store last used extent */

struct inramExtent {
	u32 startblock;	/* Block from begginig of the file */
	u32 key;
	u32 next;
	u16 blocks;
};

/* inode in-kernel data */

struct asfs_inode_info {
	atomic_t i_opencnt;
	u32 firstblock;
	u32 hashtable;
	int modified;
	loff_t mmu_private;
	struct inramExtent ext_cache;
	struct inode vfs_inode;
};

/* short cut to get to the asfs specific inode data */
static inline struct asfs_inode_info *ASFS_I(struct inode *inode)
{
   return list_entry(inode, struct asfs_inode_info, vfs_inode);
}

/* Amiga SFS superblock in-core data */

struct asfs_sb_info {
	u32 totalblocks;
	u32 rootobjectcontainer;
	u32 extentbnoderoot;
	u32 objectnoderoot;

	u32 adminspacecontainer;
	u32 bitmapbase;
	u32 freeblocks;
	u32 blocks_inbitmap;
	u32 blocks_bitmap;
	u32 block_rovingblockptr;

	uid_t uid;
	gid_t gid;
	umode_t mode;
	u16 flags;
	char *prefix;
	char *root_volume;		/* Volume prefix for absolute symlinks. */
	char *iocharset;
	char *codepage;
	struct nls_table *nls_io;
	struct nls_table *nls_disk;
};

/* short cut to get to the asfs specific sb data */
static inline struct asfs_sb_info *ASFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}
 
/* io inline code */

u32 asfs_calcchecksum(void *block, u32 blocksize);

static inline int
asfs_check_block(struct fsBlockHeader *block, u32 blocksize, u32 n, u32 id)
{
	if (asfs_calcchecksum(block, blocksize) ==
	    be32_to_cpu(((struct fsBlockHeader *) block)->checksum) &&
	    n == be32_to_cpu(((struct fsBlockHeader *) block)->ownblock) &&
	    id == be32_to_cpu(((struct fsBlockHeader *) block)->id))
		return TRUE;
	return FALSE;
}

/* get fs structure from block and do some checks... */
static inline struct buffer_head *
asfs_breadcheck(struct super_block *sb, u32 n, u32 type)
{
	struct buffer_head *bh;
	if ((bh = sb_bread(sb, n))) {
		if (asfs_check_block ((void *)bh->b_data, sb->s_blocksize, n, type)) {
			return bh;	/* all okay */
		}
		brelse(bh);
	}
	return NULL;		/* error */
}

static inline struct buffer_head *
asfs_getzeroblk(struct super_block *sb, int block)
{
	struct buffer_head *bh;
	bh = sb_getblk(sb, block);
	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	return bh;
}

static inline void 
asfs_bstore(struct super_block *sb, struct buffer_head *bh)
{
	((struct fsBlockHeader *) (bh->b_data))->checksum =
	    cpu_to_be32(asfs_calcchecksum(bh->b_data, sb->s_blocksize));
	mark_buffer_dirty(bh);
}

static inline void asfs_brelse(struct buffer_head *bh)
{
	brelse(bh);
}

static inline void dec_count(struct inode *inode)
{
	inode->i_nlink--;
	mark_inode_dirty(inode);
}

/* all prototypes */

/* adminspace.c */
int asfs_allocadminspace(struct super_block *sb, u32 * block);
int asfs_freeadminspace(struct super_block *sb, u32 block);
int asfs_markspace(struct super_block *sb, u32 block, u32 blocks);
int asfs_freespace(struct super_block *sb, u32 block, u32 blocks);
int asfs_findspace(struct super_block *sb, u32 maxneeded, u32 start, u32 end,
	      u32 * returned_block, u32 * returned_blocks);

/* dir.c */
int asfs_readdir(struct file *filp, void *dirent, filldir_t filldir);
struct dentry *asfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd);

/* extents.c */
int asfs_getextent(struct super_block *sb, u32 key, struct buffer_head **ret_bh,
	      struct fsExtentBNode **ret_ebn);
int asfs_deletebnode(struct super_block *sb, struct buffer_head *cb, u32 key);
int asfs_deleteextents(struct super_block *sb, u32 key);
int asfs_addblocks(struct super_block *sb, u16 blocks, u32 newspace,
	      u32 objectnode, u32 * io_lastextentbnode);

/* file.c */
int asfs_readpage(struct file *file, struct page *page);
sector_t asfs_bmap(struct address_space *mapping, sector_t block);
int asfs_writepage(struct page *page, struct writeback_control *wbc);
int asfs_write_begin(struct file *file, struct address_space *mapping, loff_t pos, unsigned len, unsigned flags, struct page **pagep, void **fsdata);
void asfs_truncate(struct inode *inode);
int asfs_file_open(struct inode *inode, struct file *filp);
int asfs_file_release(struct inode *inode, struct file *filp);

/* inode.c */
struct inode *asfs_get_root_inode(struct super_block *sb);
void asfs_read_locked_inode(struct inode *inode, void *arg);

/* namei */
u8 asfs_lowerchar(u8 c);
int asfs_check_name(const u8 *name, int len);
int asfs_namecmp(u8 *s, u8 *ct, int casesensitive, struct nls_table *t);
u16 asfs_hash(u8 *name, int casesensitive);
void asfs_translate(u8 *to, u8 *from, struct nls_table *nls_to, struct nls_table *nls_from, int limit);

/* nodes */
int asfs_getnode(struct super_block *sb, u32 nodeno,
	    struct buffer_head **ret_bh, struct fsObjectNode **ret_node);
int asfs_createnode(struct super_block *sb, struct buffer_head **returned_cb,
	       struct fsNode **returned_node, u32 * returned_nodeno);
int asfs_deletenode(struct super_block *sb, u32 objectnode);

/* objects */
struct fsObject *asfs_nextobject(struct fsObject *obj);
struct fsObject *asfs_find_obj_by_name(struct super_block *sb,
		struct fsObjectContainer *objcont, u8 * name);
int asfs_readobject(struct super_block *sb, u32 objectnode,
	       struct buffer_head **cb, struct fsObject **returned_object);
int asfs_createobject(struct super_block *sb, struct buffer_head **io_cb,
		 struct fsObject **io_o, struct fsObject *src_o,
		 u8 * objname, int force);
int asfs_deleteobject(struct super_block *sb, struct buffer_head *cb,
		 struct fsObject *o);
int asfs_renameobject(struct super_block *sb, struct buffer_head *cb1,
		 struct fsObject *o1, struct buffer_head *cbparent,
		 struct fsObject *oparent, u8 * newname);

int asfs_addblockstofile(struct super_block *sb, struct buffer_head *objcb,
		    struct fsObject *o, u32 blocks, u32 * newspace,
		    u32 * addedblocks);
int asfs_truncateblocksinfile(struct super_block *sb, struct buffer_head *bh,
			 struct fsObject *o, u32 newsize);

/* symlink.c */
int asfs_symlink_readpage(struct file *file, struct page *page);
int asfs_write_symlink(struct inode *symfile, const char *symname);

#endif
