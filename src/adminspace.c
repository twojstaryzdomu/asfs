/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta7
 *
 * This file contains some parts of the original amiga version of 
 * SmartFilesystem source code.
 *
 * SmartFilesystem is copyrighted (C) 2003 by: John Hendrikx, 
 * Ralph Schmidt, Emmanuel Lesueur, David Gerber, and Marcin Kurek
 * 
 * Adapted and modified by Marek 'March' Szyprowski <marek@amiga.pl>
 *
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include "asfs_fs.h"
#include "bitfuncs.h"

#include <asm/byteorder.h>

#ifdef CONFIG_ASFS_RW

static int setfreeblocks(struct super_block *sb, u32 freeblocks)
{
	struct buffer_head *bh;
	if ((bh = asfs_breadcheck(sb, ASFS_SB(sb)->rootobjectcontainer, ASFS_OBJECTCONTAINER_ID))) {
		struct fsRootInfo *ri = (struct fsRootInfo *) ((u8 *) bh->b_data + sb->s_blocksize - sizeof(struct fsRootInfo));
		ASFS_SB(sb)->freeblocks = freeblocks;
		ri->freeblocks = cpu_to_be32(freeblocks);
		asfs_bstore(sb, bh);
		asfs_brelse(bh);
		return 0;
	}
	return -EIO;
}

static inline int enoughspace(struct super_block *sb, u32 blocks)
{
	if (ASFS_SB(sb)->freeblocks - ASFS_ALWAYSFREE < blocks)
		return FALSE;

	return TRUE;
}

	/* Determines the amount of free blocks starting from block /block/.
	   If there are no blocks found or if there was an error -1 is returned,
	   otherwise this function will count the number of free blocks until
	   an allocated block is encountered or until maxneeded has been
	   exceeded. */

static int availablespace(struct super_block *sb, u32 block, u32 maxneeded)
{
	struct buffer_head *bh = NULL;
	struct fsBitmap *b;
	u32 longs = ASFS_SB(sb)->blocks_inbitmap >> 5;
	u32 maxbitmapblock = ASFS_SB(sb)->bitmapbase + ASFS_SB(sb)->blocks_bitmap;
	int blocksfound = 0;
	u32 bitstart;
	int bitend;
	u32 nextblock = ASFS_SB(sb)->bitmapbase + block / ASFS_SB(sb)->blocks_inbitmap;

	bitstart = block % ASFS_SB(sb)->blocks_inbitmap;

	while (nextblock < maxbitmapblock && (bh = asfs_breadcheck(sb, nextblock++, ASFS_BITMAP_ID))) {
		b = (void *) bh->b_data;

		if ((bitend = bmffz(b->bitmap, longs, bitstart)) >= 0) {
			blocksfound += bitend - bitstart;
			asfs_brelse(bh);
			return blocksfound;
		}
		blocksfound += ASFS_SB(sb)->blocks_inbitmap - bitstart;
		if (blocksfound >= maxneeded) {
			asfs_brelse(bh);
			return blocksfound;
		}
		bitstart = 0;
		asfs_brelse(bh);
	}

	if (bh == NULL)
		return (-1);

	return (blocksfound);
}

int asfs_findspace(struct super_block *sb, u32 maxneeded, u32 start, u32 end, u32 * returned_block, u32 * returned_blocks)
{
	struct buffer_head *bh;
	u32 longs = ASFS_SB(sb)->blocks_inbitmap >> 5;
	u32 space = 0;
	u32 block;
	u32 bitmapblock = ASFS_SB(sb)->bitmapbase + start / ASFS_SB(sb)->blocks_inbitmap;
	u32 breakpoint;
	int bitstart, bitend;
	int reads;

	if (enoughspace(sb, maxneeded) == FALSE) {
		*returned_block = 0;
		*returned_blocks = 0;
		return -ENOSPC;
	}

	if (start >= ASFS_SB(sb)->totalblocks)
		start -= ASFS_SB(sb)->totalblocks;

	if (end == 0)
		end = ASFS_SB(sb)->totalblocks;

	reads = ((end - 1) / ASFS_SB(sb)->blocks_inbitmap) + 1 - start / ASFS_SB(sb)->blocks_inbitmap;

	if (start >= end)
		reads += (ASFS_SB(sb)->totalblocks - 1) / ASFS_SB(sb)->blocks_inbitmap + 1;

	breakpoint = (start < end ? end : ASFS_SB(sb)->totalblocks);

	*returned_block = 0;
	*returned_blocks = 0;

	bitend = start % ASFS_SB(sb)->blocks_inbitmap;
	block = start - bitend;

	while ((bh = asfs_breadcheck(sb, bitmapblock++, ASFS_BITMAP_ID))) {
		struct fsBitmap *b = (void *) bh->b_data;
		u32 localbreakpoint = breakpoint - block;

		if (localbreakpoint > ASFS_SB(sb)->blocks_inbitmap)
			localbreakpoint = ASFS_SB(sb)->blocks_inbitmap;

		/* At this point space contains the amount of free blocks at
		   the end of the previous bitmap block.  If there are no
		   free blocks at the start of this bitmap block, space will
		   be set to zero, since in that case the space isn't adjacent. */

		while ((bitstart = bmffo(b->bitmap, longs, bitend)) < ASFS_SB(sb)->blocks_inbitmap) {
			/* found the start of an empty space, now find out how large it is */

			if (bitstart >= localbreakpoint)
				break;

			if (bitstart != 0)
				space = 0;

			bitend = bmffz(b->bitmap, longs, bitstart);

			if (bitend > localbreakpoint)
				bitend = localbreakpoint;

			space += bitend - bitstart;

			if (*returned_blocks < space) {
				*returned_block = block + bitend - space;
				if (space >= maxneeded) {
					*returned_blocks = maxneeded;
					asfs_brelse(bh);
					return 0;
				}
				*returned_blocks = space;
			}

			if (bitend >= localbreakpoint)
				break;
		}

		if (--reads == 0)
			break;

		/* no (more) empty spaces found in this block */

		if (bitend != ASFS_SB(sb)->blocks_inbitmap)
			space = 0;

		bitend = 0;
		block += ASFS_SB(sb)->blocks_inbitmap;

		if (block >= ASFS_SB(sb)->totalblocks) {
			block = 0;
			space = 0;
			breakpoint = end;
			bitmapblock = ASFS_SB(sb)->bitmapbase;
		}
		asfs_brelse(bh);
	}

	if (bh == NULL)
		return -EIO;

	asfs_brelse(bh);

	if (*returned_blocks == 0)
		return -ENOSPC;
	else
		return 0;
}

int asfs_markspace(struct super_block *sb, u32 block, u32 blocks)
{
	int errorcode;

	asfs_debug("markspace: Marking %d blocks from block %d\n", blocks, block);

	if ((availablespace(sb, block, blocks)) < blocks) {
		printk("ASFS: Attempted to mark %d blocks from block %d, but some of them were already full!\n", blocks, block);
		return -EIO;
	}

	if ((errorcode = setfreeblocks(sb, ASFS_SB(sb)->freeblocks - blocks)) == 0) {
		struct buffer_head *bh;
		u32 skipblocks = block / ASFS_SB(sb)->blocks_inbitmap;
		u32 longs = (sb->s_blocksize - sizeof(struct fsBitmap)) >> 2;
		u32 bitmapblock;

		block -= skipblocks * ASFS_SB(sb)->blocks_inbitmap;
		bitmapblock = ASFS_SB(sb)->bitmapbase + skipblocks;

		while (blocks > 0) {
			if ((bh = asfs_breadcheck(sb, bitmapblock++, ASFS_BITMAP_ID))) {
				struct fsBitmap *b = (void *) bh->b_data;

				blocks -= bmclr(b->bitmap, longs, block, blocks);
				block = 0;

				asfs_bstore(sb, bh);
				asfs_brelse(bh);
			} else
				return -EIO;
		}
	}

	return (errorcode);
}

	/* This function checks the bitmap and tries to locate at least /blocksneeded/
	   adjacent unused blocks.  If found it sets returned_block to the start block
	   and returns no error.  If not found, ERROR_DISK_IS_FULL is returned and
	   returned_block is set to zero.  Any other errors are returned as well. */

static inline int internalfindspace(struct super_block *sb, u32 blocksneeded, u32 startblock, u32 endblock, u32 * returned_block)
{
	u32 blocks;
	int errorcode;

	if ((errorcode = asfs_findspace(sb, blocksneeded, startblock, endblock, returned_block, &blocks)) == 0)
		if (blocks != blocksneeded)
			return -ENOSPC;

	return errorcode;
}

static int findandmarkspace(struct super_block *sb, u32 blocksneeded, u32 * returned_block)
{
	int errorcode;

	if (enoughspace(sb, blocksneeded) != FALSE) {
		if ((errorcode = internalfindspace(sb, blocksneeded, 0, ASFS_SB(sb)->totalblocks, returned_block)) == 0)
			errorcode = asfs_markspace(sb, *returned_block, blocksneeded);
	} else
		errorcode = -ENOSPC;

	return (errorcode);
}

/* ************************** */

int asfs_freespace(struct super_block *sb, u32 block, u32 blocks)
{
	int errorcode;

	asfs_debug("freespace: Freeing %d blocks from block %d\n", blocks, block);

	if ((errorcode = setfreeblocks(sb, ASFS_SB(sb)->freeblocks + blocks)) == 0) {
		struct buffer_head *bh;
		u32 skipblocks = block / ASFS_SB(sb)->blocks_inbitmap;
		u32 longs = (sb->s_blocksize - sizeof(struct fsBitmap)) >> 2;
		u32 bitmapblock;

		block -= skipblocks * ASFS_SB(sb)->blocks_inbitmap;
		bitmapblock = ASFS_SB(sb)->bitmapbase + skipblocks;

		while (blocks > 0) {
			if ((bh = asfs_breadcheck(sb, bitmapblock++, ASFS_BITMAP_ID))) {
				struct fsBitmap *b = (void *) bh->b_data;

				blocks -= bmset(b->bitmap, longs, block, blocks);
				block = 0;

				asfs_bstore(sb, bh);
				asfs_brelse(bh);
			} else
				return -EIO;
		}
	}

	return (errorcode);
}

/*************** admin space containers ****************/

int asfs_allocadminspace(struct super_block *sb, u32 *returned_block)
{
	struct buffer_head *bh;
	u32 adminspaceblock = ASFS_SB(sb)->adminspacecontainer;
	int errorcode = -EIO;

	asfs_debug("allocadminspace: allocating new block\n");

	while ((bh = asfs_breadcheck(sb, adminspaceblock, ASFS_ADMINSPACECONTAINER_ID))) {
		struct fsAdminSpaceContainer *asc1 = (void *) bh->b_data;
		struct fsAdminSpace *as1 = asc1->adminspace;
		int adminspaces1 = (sb->s_blocksize - sizeof(struct fsAdminSpaceContainer)) / sizeof(struct fsAdminSpace);

		while (adminspaces1-- > 0) {
			s16 bitoffset;

			if (as1->space != 0 && (bitoffset = bfffz(be32_to_cpu(as1->bits), 0)) >= 0) {
				u32 emptyadminblock = be32_to_cpu(as1->space) + bitoffset;
				as1->bits |= cpu_to_be32(1 << (31 - bitoffset));
				asfs_bstore(sb, bh);
				*returned_block = emptyadminblock;
				asfs_brelse(bh);
				asfs_debug("allocadminspace: found block %d\n", *returned_block);
				return 0;
			}
			as1++;
		}

		adminspaceblock = be32_to_cpu(asc1->next);
		asfs_brelse(bh);

		if (adminspaceblock == 0) {
			u32 startblock;

			asfs_debug("allocadminspace: allocating new adminspace area\n");

			/* If we get here it means current adminspace areas are all filled.
			   We would now need to find a new area and create a fsAdminSpace
			   structure in one of the AdminSpaceContainer blocks.  If these
			   don't have any room left for new adminspace areas a new
			   AdminSpaceContainer would have to be created first which is
			   placed as the first block in the newly found admin area. */

			adminspaceblock = ASFS_SB(sb)->adminspacecontainer;

			if ((errorcode = findandmarkspace(sb, 32, &startblock)))
				return errorcode;

			while ((bh = asfs_breadcheck(sb, adminspaceblock, ASFS_ADMINSPACECONTAINER_ID))) {
				struct fsAdminSpaceContainer *asc2 = (void *) bh->b_data;
				struct fsAdminSpace *as2 = asc2->adminspace;
				int adminspaces2 = (sb->s_blocksize - sizeof(struct fsAdminSpaceContainer)) / sizeof(struct fsAdminSpace);

				while (adminspaces2-- > 0 && as2->space != 0)
					as2++;

				if (adminspaces2 >= 0) {	/* Found a unused AdminSpace in this AdminSpaceContainer! */
					as2->space = cpu_to_be32(startblock);
					as2->bits = 0;
					asfs_bstore(sb, bh);
					asfs_brelse(bh);
					break;
				}

				if (asc2->next == 0) {
					/* Oh-oh... we marked our new adminspace area in use, but we couldn't
					   find space to store a fsAdminSpace structure in the existing
					   fsAdminSpaceContainer blocks.  This means we need to create and
					   link a new fsAdminSpaceContainer as the first block in our newly
					   marked adminspace. */

					asc2->next = cpu_to_be32(startblock);
					asfs_bstore(sb, bh);
					asfs_brelse(bh);

					/* Now preparing new AdminSpaceContainer */

					if ((bh = asfs_getzeroblk(sb, startblock)) == NULL)
						return -EIO;

					asc2 = (void *) bh->b_data;
					asc2->bheader.id = cpu_to_be32(ASFS_ADMINSPACECONTAINER_ID);
					asc2->bheader.ownblock = cpu_to_be32(startblock);
					asc2->previous = cpu_to_be32(adminspaceblock);
					asc2->adminspace[0].space = cpu_to_be32(startblock);
					asc2->adminspace[0].bits = cpu_to_be32(0x80000000);
					asc2->bits = 32;

					asfs_bstore(sb, bh);
					asfs_brelse(bh);

					adminspaceblock = startblock;
					break;	/* Breaks through to outer loop! */
				}
				adminspaceblock = be32_to_cpu(asc2->next);
				asfs_brelse(bh);
			}
		}
	}
	return errorcode;
}

int asfs_freeadminspace(struct super_block *sb, u32 block)
{
	struct buffer_head *bh;
	u32 adminspaceblock = ASFS_SB(sb)->adminspacecontainer;

	asfs_debug("freeadminspace: Entry -- freeing block %d\n", block);

	while ((bh = asfs_breadcheck(sb, adminspaceblock, ASFS_ADMINSPACECONTAINER_ID))) {
		struct fsAdminSpaceContainer *asc = (void *) bh->b_data;
		struct fsAdminSpace *as = asc->adminspace;
		int adminspaces = (sb->s_blocksize - sizeof(struct fsAdminSpaceContainer)) / sizeof(struct fsAdminSpace);

		while (adminspaces-- > 0) {
			if (block >= be32_to_cpu(as->space) && block < be32_to_cpu(as->space) + 32) {
				s16 bitoffset = block - be32_to_cpu(as->space);
				asfs_debug("freeadminspace: Block to be freed is located in AdminSpaceContainer block at %d\n", adminspaceblock);
				as->bits &= cpu_to_be32(~(1 << (31 - bitoffset)));
				asfs_bstore(sb, bh);
				asfs_brelse(bh);
				return 0;
			}
			as++;
		}

		if ((adminspaceblock = be32_to_cpu(asc->next)) == 0)
			break;

		asfs_brelse(bh);
	}

	if (bh != NULL) {
		asfs_brelse(bh);
		printk("ASFS: Unable to free an administration block. The block cannot be found.");
		return -ENOENT;
	}

	return -EIO;
}

#endif
