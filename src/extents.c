/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta11
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

	/* This function looks for the BNode equal to the key.  If no
	   exact match is available then the BNode which is slightly
	   lower than key will be returned.  If no such BNode exists
	   either, then the first BNode in this block is returned.

	   This function will return the first BNode even if there
	   are no BNode's at all in this block (this can only happen
	   for the Root of the tree).  Be sure to check if the Root
	   is not empty before calling this function. */

static struct BNode *searchforbnode(u32 key, struct BTreeContainer *tc)
{
	struct BNode *tn;
	s16 n = be16_to_cpu(tc->nodecount) - 1;

	tn = (struct BNode *) ((u8 *) tc->bnode + n * tc->nodesize);
	for (;;) {
		if (n <= 0 || key >= be32_to_cpu(tn->key))
			return tn;

		tn = (struct BNode *) ((u8 *) tn - tc->nodesize);
		n--;
	}
}

/* This function finds the BNode with the given key.  If no exact match can be
   found then this function will return either the next or previous closest
   match (don't rely on this).

   If there were no BNode's at all, then *returned_bh will be NULL. */

static int findbnode(struct super_block *sb, u32 key, struct buffer_head **returned_bh, struct BNode **returned_bnode)
{
	u32 rootblock = ASFS_SB(sb)->extentbnoderoot;

	asfs_debug("findbnode: Looking for BNode with key %d\n", key);

	while ((*returned_bh = asfs_breadcheck(sb, rootblock, ASFS_BNODECONTAINER_ID))) {
		struct fsBNodeContainer *bnc = (void *) (*returned_bh)->b_data;
		struct BTreeContainer *btc = &bnc->btc;

		if (btc->nodecount == 0) {
			*returned_bnode = NULL;
			break;
		}

		*returned_bnode = searchforbnode(key, btc);
		if (btc->isleaf == TRUE)
			break;

		rootblock = be32_to_cpu((*returned_bnode)->data);
		asfs_brelse(*returned_bh);
	}

	if (*returned_bh == NULL)
		return -EIO;

	return 0;
}

int asfs_getextent(struct super_block *sb, u32 key, struct buffer_head **ret_bh, struct fsExtentBNode **ret_ebn)
{
	int result;
	if ((result = findbnode(sb, key, ret_bh, (struct BNode **)ret_ebn)) == 0) 
		if (be32_to_cpu((*ret_ebn)->key) != key) {
			brelse(*ret_bh);
			*ret_bh = NULL;
			return -ENOENT;
		}

	return result;
}

#ifdef CONFIG_ASFS_RW

	/* This routine inserts a node sorted into a BTreeContainer.  It does
	   this by starting at the end, and moving the nodes one by one to
	   a higher slot until the empty slot has the correct position for
	   this key.  Donot use this function on completely filled
	   BTreeContainers! */

static struct BNode *insertbnode(u32 key, struct BTreeContainer *btc)
{
	struct BNode *bn;
	bn = (struct BNode *) ((u8 *) btc->bnode + btc->nodesize * (be16_to_cpu(btc->nodecount) - 1));

	for (;;) {
		if (bn < btc->bnode || key > be32_to_cpu(bn->key)) {
			bn = (struct BNode *) ((u8 *) bn + btc->nodesize);
			bn->key = cpu_to_be32(key);
			btc->nodecount = cpu_to_be16(be16_to_cpu(btc->nodecount) + 1);
			break;
		} else 
			memmove((u8 *)bn + btc->nodesize, bn, btc->nodesize);

		bn = (struct BNode *) ((u8 *) bn - btc->nodesize);
	}

	return bn;
}

static int getparentbtreecontainer(struct super_block *sb, struct buffer_head *bh, struct buffer_head **parent_bh)
{
	u32 rootblock = ASFS_SB(sb)->extentbnoderoot;
	u32 childkey = be32_to_cpu(((struct fsBNodeContainer *) bh->b_data)->btc.bnode[0].key);
	u32 childblock = be32_to_cpu(((struct fsBNodeContainer *) bh->b_data)->bheader.ownblock);

	asfs_debug("getparentbtreecontainer: Getting parent of block %d\n", childblock);

	/* This function gets the BTreeContainer parent of the passed in buffer_head. If
	   there is no parent this function sets dest_cont io_bh to NULL */

	if (rootblock != childblock) {
		while ((*parent_bh = asfs_breadcheck(sb, rootblock, ASFS_BNODECONTAINER_ID))) {
			struct fsBNodeContainer *bnc = (void *) (*parent_bh)->b_data;
			struct BTreeContainer *btc = &bnc->btc;
			struct BNode *bn;
			s16 n = be16_to_cpu(btc->nodecount);

			if (btc->isleaf == TRUE) {
				asfs_brelse(*parent_bh);
				break;
			}

			while (n-- > 0)
				if (be32_to_cpu(btc->bnode[n].data) == childblock)
					return 0;	/* Found parent!! */

			bn = searchforbnode(childkey, btc);	/* This searchforbnode() doesn't have to get EXACT key matches. */
			rootblock = be32_to_cpu(bn->data);
			asfs_brelse(*parent_bh);
		}
		if (*parent_bh == NULL)
			return -EIO;
	}

	*parent_bh = NULL;
	return 0;
}

/* Spits a btreecontainer. It realses passed in bh! */

static int splitbtreecontainer(struct super_block *sb, struct buffer_head *bh)
{
	struct buffer_head *bhparent;
	struct BNode *bn;
	int errorcode;

	asfs_debug("splitbtreecontainer: splitting block %u\n", be32_to_cpu(((struct fsBlockHeader *) bh->b_data)->ownblock));

	if ((errorcode = getparentbtreecontainer(sb, bh, &bhparent)) == 0) {
		if (bhparent == NULL) {
			u32 newbcontblock;
			u32 bcontblock;
			/* We need to create Root tree-container - adding new level to extent tree */

			asfs_debug("splitbtreecontainer: creating root tree-container.\n");

			bhparent = bh;
			if ((errorcode = asfs_allocadminspace(sb, &newbcontblock)) == 0 && (bh = asfs_getzeroblk(sb, newbcontblock))) {
				struct fsBNodeContainer *bnc = (void *) bh->b_data;
				struct fsBNodeContainer *bncparent = (void *) bhparent->b_data;
				struct BTreeContainer *btcparent = &bncparent->btc;

				bcontblock = be32_to_cpu(bncparent->bheader.ownblock);
				memcpy(bh->b_data, bhparent->b_data, sb->s_blocksize);
				bnc->bheader.ownblock = cpu_to_be32(newbcontblock);
				asfs_bstore(sb, bh);

				memset(bhparent->b_data, '\0', sb->s_blocksize);	/* Not strictly needed, but makes things more clear. */
				bncparent->bheader.id = cpu_to_be32(ASFS_BNODECONTAINER_ID);
				bncparent->bheader.ownblock = cpu_to_be32(bcontblock);
				btcparent->isleaf = FALSE;
				btcparent->nodesize = sizeof(struct BNode);
				btcparent->nodecount = 0;

				bn = insertbnode(0, btcparent);
				bn->data = cpu_to_be32(newbcontblock);

				asfs_bstore(sb, bhparent);
			}
			if (bh == NULL)
				errorcode = -EIO;
		}

		if (errorcode == 0) {
			struct fsBNodeContainer *bncparent = (void *) bhparent->b_data;
			struct BTreeContainer *btcparent = &bncparent->btc;
			int branches1 = (sb->s_blocksize - sizeof(struct fsBNodeContainer)) / btcparent->nodesize;

			if (be16_to_cpu(btcparent->nodecount) == branches1) {
				/* We need to split the parent tree-container first! */
				if ((errorcode = splitbtreecontainer(sb, bhparent)) == 0) {
					/* bhparent might have changed after the split and has been released */
					if ((errorcode = getparentbtreecontainer(sb, bh, &bhparent)) == 0) {	
						bncparent = (void *) bhparent->b_data;
						btcparent = &bncparent->btc;
					}
				}
			}

			if (errorcode == 0) {
				u32 newbcontblock;
				struct buffer_head *bhnew;

				/* We can split this container and add it to the parent
				   because the parent has enough room. */

				if ((errorcode = asfs_allocadminspace(sb, &newbcontblock)) == 0 && (bhnew = asfs_getzeroblk(sb, newbcontblock))) {
					struct fsBNodeContainer *bncnew = (void *) bhnew->b_data;
					struct BTreeContainer *btcnew = &bncnew->btc;
					struct fsBNodeContainer *bnc = (void *) bh->b_data;
					struct BTreeContainer *btc = &bnc->btc;
					int branches2 = (sb->s_blocksize - sizeof(struct fsBNodeContainer)) / btc->nodesize;
					u32 newkey;

					bncnew->bheader.id = cpu_to_be32(ASFS_BNODECONTAINER_ID);
					bncnew->bheader.ownblock = cpu_to_be32(newbcontblock);

					btcnew->isleaf = btc->isleaf;
					btcnew->nodesize = btc->nodesize;

					btcnew->nodecount = cpu_to_be16(branches2 - branches2 / 2);

					memcpy(btcnew->bnode, (u8 *) btc->bnode + branches2 / 2 * btc->nodesize, (branches2 - branches2 / 2) * btc->nodesize);
					newkey = be32_to_cpu(btcnew->bnode[0].key);

					asfs_bstore(sb, bhnew);
					asfs_brelse(bhnew);

					btc->nodecount = cpu_to_be16(branches2 / 2);
					asfs_bstore(sb, bh);

					bn = insertbnode(newkey, btcparent);
					bn->data = cpu_to_be32(newbcontblock);
					asfs_bstore(sb, bhparent);
				}
			}
		}
		asfs_brelse(bhparent);
	}
	asfs_brelse(bh);

	return errorcode;
}

/* Returns created extentbnode - returned_bh need to saved and realesed in caller funkction! */

static int createextentbnode(struct super_block *sb, u32 key, struct buffer_head **returned_bh, struct BNode **returned_bnode)
{
	int errorcode;

	asfs_debug("createbnode: Creating BNode with key %d\n", key);

	while ((errorcode = findbnode(sb, key, returned_bh, returned_bnode)) == 0) {
		struct fsBNodeContainer *bnc = (void *) (*returned_bh)->b_data;
		struct BTreeContainer *btc = &bnc->btc;
		int extbranches = (sb->s_blocksize - sizeof(struct fsBNodeContainer)) / btc->nodesize;

		asfs_debug("createbnode: findbnode found block %d\n", be32_to_cpu(((struct fsBlockHeader *) (*returned_bh)->b_data)->ownblock));

		if (be16_to_cpu(btc->nodecount) < extbranches) {
			/* Simply insert new node in this BTreeContainer */
			asfs_debug("createbnode: Simple insert\n");
			*returned_bnode = insertbnode(key, btc);
			break;
		} else if ((errorcode = splitbtreecontainer(sb, *returned_bh)) != 0)
			break;

		/* Loop and try insert it the normal way again :-) */
	}

	return (errorcode);
}


/* This routine removes a node from a BTreeContainer indentified
   by its key.  If no such key exists this routine does nothing.
   It correctly handles empty BTreeContainers. */

static void removebnode(u32 key, struct BTreeContainer *btc)
{
	struct BNode *bn = btc->bnode;
	int n = 0;

	asfs_debug("removebnode: key %d\n", key);

	while (n < be16_to_cpu(btc->nodecount)) {
		if (be32_to_cpu(bn->key) == key) {
			btc->nodecount = cpu_to_be16(be16_to_cpu(btc->nodecount) - 1);
			memmove(bn, (u8 *) bn + btc->nodesize, (be16_to_cpu(btc->nodecount) - n) * btc->nodesize);
			break;
		}
		bn = (struct BNode *) ((u8 *) bn + btc->nodesize);
		n++;
	}
}

int asfs_deletebnode(struct super_block *sb, struct buffer_head *bh, u32 key)
{
	struct fsBNodeContainer *bnc1 = (void *) bh->b_data;
	struct BTreeContainer *btc = &bnc1->btc;
	u16 branches = (sb->s_blocksize - sizeof(struct fsBNodeContainer)) / btc->nodesize;
	int errorcode = 0;

	/* Deletes specified internal node. */

	removebnode(key, btc);
	asfs_bstore(sb, bh);

	/* Now checks if the container still contains enough nodes,
	   and takes action accordingly. */

	asfs_debug("deletebnode: branches = %d, btc->nodecount = %d\n", branches, be16_to_cpu(btc->nodecount));

	if (be16_to_cpu(btc->nodecount) < (branches + 1) / 2) {
		struct buffer_head *bhparent;
		struct buffer_head *bhsec;

		/* nodecount has become to low.  We need to merge this Container
		   with a neighbouring Container, or we need to steal a few nodes
		   from a neighbouring Container. */

		/* We get the parent of the container here, so we can find out what
		   containers neighbour the container which currently hasn't got enough nodes. */

		if ((errorcode = getparentbtreecontainer(sb, bh, &bhparent)) == 0) {
			if (bhparent != NULL) {
				struct fsBNodeContainer *bncparent = (void *) bhparent->b_data;
				struct BTreeContainer *btcparent = &bncparent->btc;
				s16 n;

				asfs_debug("deletebnode: get parent returned block %d.\n", be32_to_cpu(((struct fsBlockHeader *) bhparent->b_data)->ownblock));

				for (n = 0; n < be16_to_cpu(btcparent->nodecount); n++)
					if (btcparent->bnode[n].data == bnc1->bheader.ownblock)
						break;
				/* n is now the offset of our own bnode. */

				if (n < be16_to_cpu(btcparent->nodecount) - 1) {	/* Check if we have a next neighbour. */
					asfs_debug("deletebnode: using next container - merging blocks %d and %d\n", be32_to_cpu(bnc1->bheader.ownblock), be32_to_cpu(btcparent->bnode[n+1].data));

					if ((bhsec = asfs_breadcheck(sb, be32_to_cpu(btcparent->bnode[n + 1].data), ASFS_BNODECONTAINER_ID))) {
						struct fsBNodeContainer *bnc_next = (void *) bhsec->b_data;
						struct BTreeContainer *btc_next = &bnc_next->btc;

						if (be16_to_cpu(btc_next->nodecount) + be16_to_cpu(btc->nodecount) > branches) {	/* Check if we need to steal nodes. */
							s16 nodestosteal = (be16_to_cpu(btc_next->nodecount) + be16_to_cpu(btc->nodecount)) / 2 - be16_to_cpu(btc->nodecount);

							/* Merging them is not possible.  Steal a few nodes then. */
							memcpy((u8 *) btc->bnode + be16_to_cpu(btc->nodecount) * btc->nodesize, btc_next->bnode, nodestosteal * btc->nodesize);
							btc->nodecount = cpu_to_be16(be16_to_cpu(btc->nodecount) + nodestosteal);
							asfs_bstore(sb, bh);

							memcpy(btc_next->bnode, (u8 *) btc_next->bnode + btc_next->nodesize * nodestosteal,
							       btc->nodesize * (be16_to_cpu(btc_next->nodecount) - nodestosteal));
							btc_next->nodecount = cpu_to_be16(be16_to_cpu(btc_next->nodecount) - nodestosteal);
							asfs_bstore(sb, bhsec);

							btcparent->bnode[n + 1].key = btc_next->bnode[0].key;
							asfs_bstore(sb, bhparent);
						} else {	/* Merging is possible. */
							memcpy((u8 *) btc->bnode + btc->nodesize * be16_to_cpu(btc->nodecount), btc_next->bnode, btc->nodesize * be16_to_cpu(btc_next->nodecount));
							btc->nodecount = cpu_to_be16(be16_to_cpu(btc->nodecount) + be16_to_cpu(btc_next->nodecount));
							asfs_bstore(sb, bh);

							if ((errorcode = asfs_freeadminspace(sb, be32_to_cpu(((struct fsBlockHeader *) bhsec->b_data)->ownblock))) == 0)
								errorcode = asfs_deletebnode(sb, bhparent, be32_to_cpu(btcparent->bnode[n + 1].key));
						}
						asfs_brelse(bhsec);
					}
				} else if (n > 0) {	/* Check if we have a previous neighbour. */
					asfs_debug("deletebnode: using prev container.\n");

					if ((bhsec = asfs_breadcheck(sb, be32_to_cpu(btcparent->bnode[n - 1].data), ASFS_BNODECONTAINER_ID)) == 0) {
						struct fsBNodeContainer *bnc2 = (void *) bhsec->b_data;
						struct BTreeContainer *btc2 = &bnc2->btc;

						if (be16_to_cpu(btc2->nodecount) + be16_to_cpu(btc->nodecount) > branches) {
							/* Merging them is not possible.  Steal a few nodes then. */
							s16 nodestosteal = (be16_to_cpu(btc2->nodecount) + be16_to_cpu(btc->nodecount)) / 2 - be16_to_cpu(btc->nodecount);

							memmove((u8 *) btc->bnode + nodestosteal * btc->nodesize, btc->bnode, be16_to_cpu(btc->nodecount) * btc->nodesize);
							btc->nodecount = cpu_to_be16(be16_to_cpu(btc->nodecount) + nodestosteal);
							memcpy(btc->bnode, (u8 *) btc2->bnode + (be16_to_cpu(btc2->nodecount) - nodestosteal) * btc2->nodesize, nodestosteal * btc->nodesize);

							asfs_bstore(sb, bh);

							btc2->nodecount = cpu_to_be16(be16_to_cpu(btc2->nodecount) - nodestosteal);
							asfs_bstore(sb, bhsec);

							btcparent->bnode[n].key = btc->bnode[0].key;
							asfs_bstore(sb, bhparent);
						} else {	/* Merging is possible. */
							memcpy((u8 *) btc2->bnode + be16_to_cpu(btc2->nodecount) * btc2->nodesize, btc->bnode, be16_to_cpu(btc->nodecount) * btc->nodesize);
							btc2->nodecount = cpu_to_be16(be16_to_cpu(btc2->nodecount) + be16_to_cpu(btc->nodecount));
							asfs_bstore(sb, bhsec);

							if ((errorcode = asfs_freeadminspace(sb, be32_to_cpu(((struct fsBlockHeader *) bhsec->b_data)->ownblock))) == 0)
								errorcode = asfs_deletebnode(sb, bhparent, be32_to_cpu(btcparent->bnode[n].key));
						}
						asfs_brelse(bhsec);
					}
				}
				/*      else    
				   {
				   // Never happens, except for root and then we don't care.
				   } */
			} else if (btc->nodecount == 1) {
				/* No parent, so must be root. */

				asfs_debug("deletebnode: no parent so must be root\n");

				if (btc->isleaf == FALSE) {
					struct fsBNodeContainer *bnc3 = (void *) bh->b_data;

					/* The current root has only 1 node.  We now copy the data of this node into the
					   root and promote that data to be the new root.  The rootblock number stays the
					   same that way. */

					if ((bhsec = asfs_breadcheck(sb, be32_to_cpu(btc->bnode[0].data), ASFS_BNODECONTAINER_ID))) {
						u32 blockno = be32_to_cpu(((struct fsBlockHeader *) bh->b_data)->ownblock);
						memcpy(bh->b_data, bhsec->b_data, sb->s_blocksize);
						bnc3->bheader.ownblock = cpu_to_be32(blockno);

						asfs_bstore(sb, bh);
						errorcode = asfs_freeadminspace(sb, be32_to_cpu(((struct fsBlockHeader *) bhsec->b_data)->ownblock));
						asfs_brelse(bhsec);
					} else
						errorcode = -EIO;
				}
				/* If not, then root contains leafs. */
			}

			asfs_debug("deletebnode: almost done\n");
			/* otherwise, it must be the root, and the root is allowed
			   to contain less than the minimum amount of nodes. */

		}
		if (bhparent != NULL)
			asfs_brelse(bhparent);
	}

	return errorcode;
}

   /* Deletes an fsExtentBNode structure by key and any fsExtentBNodes linked to it.
      This function DOES NOT fix the next pointer in a possible fsExtentBNode which
      might have been pointing to the first BNode we are deleting.  Make sure you check
      this yourself, if needed.

      If key is zero, than this function does nothing. */

int asfs_deleteextents(struct super_block *sb, u32 key)
{
	struct buffer_head *bh;
	struct fsExtentBNode *ebn;
	int errorcode = 0;

	asfs_debug("deleteextents: Entry -- deleting extents from key %d\n", key);

	while (key != 0 && (errorcode = findbnode(sb, key, &bh, (struct BNode **) &ebn)) == 0) {
		/* node to be deleted located. */
		key = be32_to_cpu(ebn->next);
		if ((errorcode = asfs_freespace(sb, be32_to_cpu(ebn->key), be16_to_cpu(ebn->blocks))) != 0)
			break;

		if ((errorcode = asfs_deletebnode(sb, bh, be32_to_cpu(ebn->key))) != 0)
			break;

		asfs_brelse(bh);
	}

	return (errorcode);
}

   /* This function adds /blocks/ blocks starting at block /newspace/ to a file
      identified by /objectnode/ and /lastextentbnode/.  /io_lastextentbnode/ can
      be zero if there is no ExtentBNode chain attached to this file yet.
      /blocks/ ranges from 1 to 8192.  To be able to extend Extents which are
      almost full, it is wise to make this value no higher than 8192 blocks.
      /io_lastextentbnode/ will contain the new lastextentbnode value when this
      function completes.
      If there was no chain yet, then this function will create a new one.  */

int asfs_addblocks(struct super_block *sb, u16 blocks, u32 newspace, u32 objectnode, u32 *io_lastextentbnode)
{
	struct buffer_head *bh;
	struct fsExtentBNode *ebn;
	int errorcode = 0;

	if (*io_lastextentbnode != 0) {
		/* There was already a ExtentBNode chain for this file.  Extending it. */

		asfs_debug("  addblocks: Extending existing ExtentBNode chain.\n");

		if ((errorcode = asfs_getextent(sb, *io_lastextentbnode, &bh, &ebn)) == 0) {
			if (be32_to_cpu(ebn->key) + be16_to_cpu(ebn->blocks) == newspace && be16_to_cpu(ebn->blocks) + blocks < 65536) {
				/* It is possible to extent the last ExtentBNode! */
				asfs_debug("  addblocks: Extending last ExtentBNode.\n");

				ebn->blocks = cpu_to_be16(be16_to_cpu(ebn->blocks) + blocks);

				asfs_bstore(sb, bh);
				asfs_brelse(bh);
			} else {
				/* It isn't possible to extent the last ExtentBNode so we create
				   a new one and link it to the last ExtentBNode. */

				ebn->next = cpu_to_be32(newspace);
				asfs_bstore(sb, bh);
				asfs_brelse(bh);

				if ((errorcode = createextentbnode(sb, newspace, &bh, (struct BNode **) &ebn)) == 0) {
					asfs_debug("  addblocks: Created new ExtentBNode.\n");

					ebn->key = cpu_to_be32(newspace);
					ebn->prev = cpu_to_be32(*io_lastextentbnode);
					ebn->next = 0;
					ebn->blocks = cpu_to_be16(blocks);

					*io_lastextentbnode = newspace;

					asfs_bstore(sb, bh);
					asfs_brelse(bh);

					ASFS_SB(sb)->block_rovingblockptr = newspace + blocks;

	/* to be changed in the future */
/*					if (ASFS_SB(sb)->block_rovingblockptr >= ASFS_SB(sb)->totalblocks)
						ASFS_SB(sb)->block_rovingblockptr = 0;*/
				}
			}
		}
	} else {
		/* There is no ExtentBNode chain yet for this file.  Attaching one! */
		if ((errorcode = createextentbnode(sb, newspace, &bh, (struct BNode **) &ebn)) == 0) {
			asfs_debug("  addblocks: Created new ExtentBNode chain.\n");

			ebn->key = cpu_to_be32(newspace);
			ebn->prev = cpu_to_be32(objectnode + 0x80000000);
			ebn->next = 0;
			ebn->blocks = cpu_to_be16(blocks);

			*io_lastextentbnode = newspace;

			asfs_bstore(sb, bh);
			asfs_brelse(bh);

			ASFS_SB(sb)->block_rovingblockptr = newspace + blocks;

/*			if (ASFS_SB(sb)->block_rovingblockptr >= ASFS_SB(sb)->totalblocks)
				ASFS_SB(sb)->block_rovingblockptr = 0;*/
		}
	}

	asfs_debug("  addblocks: done.\n");

	return errorcode;
}
#endif
