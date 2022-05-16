/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta7
 *
 * This file contains some parts of the original amiga version of 
 * SmartFilesystem source code.
 *
 * SmartFilesystem is copyrighted (C) 2003 by: John Hendrikx, 
 * Ralph Schmidt, Emmanuel Lesueur, David Gerber and Marcin Kurek
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

#include <asm/byteorder.h>

/* Finds a specific node by number. */
int asfs_getnode(struct super_block *sb, u32 nodeno, struct buffer_head **ret_bh, struct fsObjectNode **ret_node)
{
	struct buffer_head *bh;
	struct fsNodeContainer *nodecont;
	u32 nodeindex = ASFS_SB(sb)->objectnoderoot;

	while ((bh = asfs_breadcheck(sb, nodeindex, ASFS_NODECONTAINER_ID))) {
		nodecont = (struct fsNodeContainer *) bh->b_data;

		if (be32_to_cpu(nodecont->nodes) == 1) {
			*ret_node = (struct fsObjectNode *) ((u8 *) nodecont->node + NODE_STRUCT_SIZE * (nodeno - be32_to_cpu(nodecont->nodenumber)));
			*ret_bh = bh;
			return 0;
		} else {
			u16 containerentry = (nodeno - be32_to_cpu(nodecont->nodenumber)) / be32_to_cpu(nodecont->nodes);
			nodeindex = be32_to_cpu(nodecont->node[containerentry]) >> (sb->s_blocksize_bits - ASFS_BLCKFACCURACY);
		}
		asfs_brelse(bh);
	}
	if (bh == NULL)
		return -EIO;
	return -ENOENT;
}

#ifdef CONFIG_ASFS_RW

	/* Looks for the parent of the passed-in buffer_head (fsNodeContainer)
	   starting from the root.  It returns an error if any error occured.
	   If error is 0 and io_bh is NULL as well, then there was no parent (ie,
	   you asked parent of the root).  Otherwise io_bh should contain the
	   parent of the passed-in NodeContainer. */

static int parentnodecontainer(struct super_block *sb, struct buffer_head **io_bh)
{
	u32 noderoot = ASFS_SB(sb)->objectnoderoot;
	u32 childblock = be32_to_cpu(((struct fsBlockHeader *) (*io_bh)->b_data)->ownblock);
	u32 nodenumber = be32_to_cpu(((struct fsNodeContainer *) (*io_bh)->b_data)->nodenumber);
	int errorcode = 0;

	if (noderoot == childblock) {
		*io_bh = NULL;
		return 0;
	}

	while ((*io_bh = asfs_breadcheck(sb, noderoot, ASFS_NODECONTAINER_ID))) {
		struct fsNodeContainer *nc = (void *) (*io_bh)->b_data;

		if (be32_to_cpu(nc->nodes) == 1) {
			/* We've descended the tree to a leaf NodeContainer, something
			   which should never happen if the passed-in io_bh had
			   contained a valid fsNodeContainer. */
			printk("ASFS: Failed to locate the parent NodeContainer - node tree is corrupted!\n");
			*io_bh = NULL;
			return -EIO;
		} else {
			u16 containerentry = (nodenumber - be32_to_cpu(nc->nodenumber)) / be32_to_cpu(nc->nodes);
			noderoot = be32_to_cpu(nc->node[containerentry]) >> (sb->s_blocksize_bits - ASFS_BLCKFACCURACY);
		}

		if (noderoot == childblock)
			break;

		asfs_brelse(*io_bh);
	}

	if (*io_bh == NULL)
		return -EIO;

	return errorcode;
}


static int isfull(struct super_block *sb, struct fsNodeContainer *nc)
{
	u32 *p = nc->node;
	s16 n = NODECONT_BLOCK_COUNT;

	while (--n >= 0) {
		if (*p == 0 || (be32_to_cpu(*p) & 0x00000001) == 0) {
			break;
		}
		p++;
	}

	return n < 0;
}

static int markparentfull(struct super_block *sb, struct buffer_head *bh)
{
	u32 nodenumber = be32_to_cpu(((struct fsNodeContainer *) (bh->b_data))->nodenumber);
	int errorcode;

	if ((errorcode = parentnodecontainer(sb, &bh)) == 0 && bh != 0) {
		struct fsNodeContainer *nc = (void *) bh->b_data;
		u16 containerentry = (nodenumber - be32_to_cpu(nc->nodenumber)) / be32_to_cpu(nc->nodes);

		nc->node[containerentry] = cpu_to_be32(be32_to_cpu(nc->node[containerentry]) | 0x00000001);

		asfs_bstore(sb, bh);

		if (isfull(sb, nc)) {	/* This container now is full as well!  Mark the next higher up container too then! */
			return markparentfull(sb, bh);
		}
		asfs_brelse(bh);
	}

	return errorcode;
}

static int addnewnodelevel(struct super_block *sb, u16 nodesize)
{
	struct buffer_head *bh;
	u32 noderoot = ASFS_SB(sb)->objectnoderoot;
	int errorcode;

	/* Adds a new level to the Node tree. */

	asfs_debug("addnewnodelevel: Entry\n");

	if ((bh = asfs_breadcheck(sb, noderoot, ASFS_NODECONTAINER_ID))) {
		struct buffer_head *newbh;
		u32 newblock;

		if ((errorcode = asfs_allocadminspace(sb, &newblock)) == 0 && (newbh = asfs_getzeroblk(sb, newblock))) {
			struct fsNodeContainer *nc = (void *) bh->b_data;
			struct fsNodeContainer *newnc = (void *) newbh->b_data;

			/* The newly allocated block will become a copy of the current root. */

			newnc->bheader.id = cpu_to_be32(ASFS_NODECONTAINER_ID);
			newnc->bheader.ownblock = cpu_to_be32(newblock);
			newnc->nodenumber = nc->nodenumber;
			newnc->nodes = nc->nodes;
			memcpy(newnc->node, nc->node, sb->s_blocksize - sizeof(struct fsNodeContainer));

			asfs_bstore(sb, newbh);
			asfs_brelse(newbh);

			/* The current root will now be transformed into a new root. */

			if (be32_to_cpu(nc->nodes) == 1)
				nc->nodes = cpu_to_be32((sb->s_blocksize - sizeof(struct fsNodeContainer)) / nodesize);
			else
				nc->nodes = cpu_to_be32(be32_to_cpu(nc->nodes) * NODECONT_BLOCK_COUNT);

			nc->node[0] = cpu_to_be32((newblock << (sb->s_blocksize_bits - ASFS_BLCKFACCURACY)) + 1);	/* Tree is full from that point! */
			memset(&nc->node[1], 0, sb->s_blocksize - sizeof(struct fsNodeContainer) - 4);

			asfs_bstore(sb, bh);
		}
		asfs_brelse(bh);
	} else
		errorcode = -EIO;

	return errorcode;
}

static int createnodecontainer(struct super_block *sb, u32 nodenumber, u32 nodes, u32 * returned_block)
{
	struct buffer_head *bh;
	int errorcode;
	u32 newblock;

	asfs_debug("createnodecontainer: nodenumber = %u, nodes = %u\n", nodenumber, nodes);

	if ((errorcode = asfs_allocadminspace(sb, &newblock)) == 0 && (bh = asfs_getzeroblk(sb, newblock))) {
		struct fsNodeContainer *nc = (void *) bh->b_data;

		nc->bheader.id = cpu_to_be32(ASFS_NODECONTAINER_ID);
		nc->bheader.ownblock = cpu_to_be32(newblock);

		nc->nodenumber = cpu_to_be32(nodenumber);
		nc->nodes = cpu_to_be32(nodes);

		asfs_bstore(sb, bh);
		asfs_brelse(bh);
		*returned_block = newblock;
	}

	return errorcode;
}

	/* This function creates a new fsNode structure in a fsNodeContainer.  If needed
	   it will create a new fsNodeContainers and a new fsNodeIndexContainer. */

int asfs_createnode(struct super_block *sb, struct buffer_head **returned_bh, struct fsNode **returned_node, u32 * returned_nodeno)
{
	u16 nodecount = (sb->s_blocksize - sizeof(struct fsNodeContainer)) / NODE_STRUCT_SIZE;
	u32 noderoot = ASFS_SB(sb)->objectnoderoot;
	u32 nodeindex = noderoot;
	int errorcode = 0;

	while ((*returned_bh = asfs_breadcheck(sb, nodeindex, ASFS_NODECONTAINER_ID))) {
		struct fsNodeContainer *nc = (void *) (*returned_bh)->b_data;

		if (be32_to_cpu(nc->nodes) == 1) {	/* Is it a leaf-container? */
			struct fsNode *n;
			s16 i = nodecount;

			n = (struct fsNode *) nc->node;

			while (i-- > 0) {
				if (n->data == 0)
					break;

				n = (struct fsNode *) ((u8 *) n + NODE_STRUCT_SIZE);
			}

			if (i >= 0) {
				/* Found an empty fsNode structure! */
				*returned_node = n;
				*returned_nodeno = be32_to_cpu(nc->nodenumber) + ((u8 *) n - (u8 *) nc->node) / NODE_STRUCT_SIZE;

				asfs_debug("createnode: Created Node %d\n", *returned_nodeno);

				/* Below we continue to look through the NodeContainer block.  We skip the entry
				   we found to be unused, and see if there are any more unused entries.  If we
				   do not find any more unused entries then this container is now full. */

				n = (struct fsNode *) ((u8 *) n + NODE_STRUCT_SIZE);

				while (i-- > 0) {
					if (n->data == 0)
						break;

					n = (struct fsNode *) ((u8 *) n + NODE_STRUCT_SIZE);
				}

				if (i < 0) {
					/* No more empty fsNode structures in this block.  Mark parent full. */
					errorcode = markparentfull(sb, *returned_bh);
				}

				return errorcode;
			} else {
				/* What happened now is that we found a leaf-container which was
				   completely filled.  In practice this should only happen when there
				   is only a single NodeContainer (only this container), or when there
				   was an error in one of the full-bits in a higher level container. */

				if (noderoot != nodeindex) {
					/*** Hmmm... it looks like there was a damaged full-bit or something.
					     In this case we'd probably better call markcontainerfull. */

					printk("ASFS: Couldn't find empty Node in NodeContainer while NodeIndexContainer indicated there should be one!\n");

					errorcode = -ENOSPC;
					break;
				} else {
					/* Container is completely filled. */

					if ((errorcode = addnewnodelevel(sb, NODE_STRUCT_SIZE)) != 0)
						return errorcode;

					nodeindex = noderoot;
				}
			}
		} else {	/* This isn't a leaf container */
			u32 *p = nc->node;
			s16 i = NODECONT_BLOCK_COUNT;

			/* We've read a normal container */

			while (i-- > 0) {
				if (*p != 0 && (be32_to_cpu(*p) & 0x00000001) == 0)
					break;

				p++;
			}

			if (i >= 0) {
				/* Found a not completely filled Container */

				nodeindex = be32_to_cpu(*p) >> (sb->s_blocksize_bits - ASFS_BLCKFACCURACY);
			} else {
				/* Everything in the NodeIndexContainer was completely filled.  There possibly
				   are some unused pointers in this block however.  */

				asfs_debug("createnode: NodeContainer at block has no empty Nodes.\n");

				p = nc->node;
				i = NODECONT_BLOCK_COUNT;

				while (i-- > 0) {
					if (*p == 0)
						break;

					p++;
				}

				if (i >= 0) {
					u32 newblock;
					u32 nodes;

					/* Found an unused Container pointer */

					if (be32_to_cpu(nc->nodes) == (sb->s_blocksize - sizeof(struct fsNodeContainer)) / NODE_STRUCT_SIZE) {
						nodes = 1;
					} else {
						nodes = be32_to_cpu(nc->nodes) / NODECONT_BLOCK_COUNT;
					}

					if ((errorcode = createnodecontainer(sb, be32_to_cpu(nc->nodenumber) + (p - nc->node) * be32_to_cpu(nc->nodes), nodes, &newblock)) != 0) {
						break;
					}

					*p = cpu_to_be32(newblock << (sb->s_blocksize_bits - ASFS_BLCKFACCURACY));

					asfs_bstore(sb, *returned_bh);
				} else {
					/* Container is completely filled.  This must be the top-level NodeIndex container
					   as otherwise the full-bit would have been wrong! */

					if ((errorcode = addnewnodelevel(sb, NODE_STRUCT_SIZE)) != 0)
						break;

					nodeindex = noderoot;
				}
			}
		}
		asfs_brelse(*returned_bh);
	}

	if (*returned_bh == NULL)
		return -EIO;

	return (errorcode);
}

static int markparentempty(struct super_block *sb, struct buffer_head *bh)
{
	u32 nodenumber = be32_to_cpu(((struct fsNodeContainer *) bh->b_data)->nodenumber);
	int errorcode;

	if ((errorcode = parentnodecontainer(sb, &bh)) == 0 && bh != 0) {
		struct fsNodeContainer *nc = (void *) bh->b_data;
		int wasfull;
		u16 containerentry = (nodenumber - be32_to_cpu(nc->nodenumber)) / be32_to_cpu(nc->nodes);

		wasfull = isfull(sb, nc);

		nc->node[containerentry] = cpu_to_be32(be32_to_cpu(nc->node[containerentry]) & ~0x00000001);

		asfs_bstore(sb, bh);

		if (wasfull) {
			/* This container was completely full before!  Mark the next higher up container too then! */
			return markparentempty(sb, bh);
		}
		asfs_brelse(bh);
	}

	return errorcode;
}

static int freecontainer(struct super_block *sb, struct buffer_head *bh)
{
	u32 nodenumber = be32_to_cpu(((struct fsNodeContainer *) bh->b_data)->nodenumber);
	int errorcode;

	if ((errorcode = parentnodecontainer(sb, &bh)) == 0 && bh != NULL) {	/* This line also prevents the freeing of the noderoot. */
		struct fsNodeContainer *nc = (void *) bh->b_data;
		u16 containerindex = (nodenumber - be32_to_cpu(nc->nodenumber)) / be32_to_cpu(nc->nodes);

		if ((errorcode = asfs_freeadminspace(sb, be32_to_cpu(nc->node[containerindex]) >> (sb->s_blocksize_bits - ASFS_BLCKFACCURACY))) == 0) {
			u32 *p = nc->node;
			s16 n = NODECONT_BLOCK_COUNT;

			nc->node[containerindex] = 0;
			asfs_bstore(sb, bh);

			while (n-- > 0)
				if (*p++ != 0)
					break;

			if (n < 0) {	/* This container is now completely empty!  Free this NodeIndexContainer too then! */
				return freecontainer(sb, bh);
			}
		}
		asfs_brelse(bh);
	}

	return errorcode;
}

static int internaldeletenode(struct super_block *sb, struct buffer_head *bh, struct fsNode *n)
{
	struct fsNodeContainer *nc = (void *) bh->b_data;
	u16 nodecount = (sb->s_blocksize - sizeof(struct fsNodeContainer)) / NODE_STRUCT_SIZE;
	s16 i = nodecount;
	s16 empty = 0;
	int errorcode = 0;

	n->data = 0;
	n = (struct fsNode *) nc->node;

	while (i-- > 0) {
		if (n->data == 0)
			empty++;

		n = (struct fsNode *) ((u8 *) n + NODE_STRUCT_SIZE);
	}

	asfs_bstore(sb, bh);

	if (empty == 1)		/* NodeContainer was completely full before, so we need to mark it empty now. */
		errorcode = markparentempty(sb, bh);
	else if (empty == nodecount)	/* NodeContainer is now completely empty!  Free it! */
		errorcode = freecontainer(sb, bh);

	return (errorcode);
}

int asfs_deletenode(struct super_block *sb, u32 objectnode)
{
	struct buffer_head *bh;
	struct fsObjectNode *on;
	int errorcode;

	asfs_debug("deletenode: Deleting Node %d\n", objectnode);

	if ((errorcode = asfs_getnode(sb, objectnode, &bh, &on)) == 0)
		errorcode = internaldeletenode(sb, bh, (struct fsNode *) on);

	asfs_brelse(bh);
	return (errorcode);
}

#endif
