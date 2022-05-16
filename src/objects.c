/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta11
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

#include <asm/byteorder.h>

struct fsObject *asfs_nextobject(struct fsObject *obj)
{
	int i;
	u8 *p = obj->name;

	for (i = 2; i > 0; p++)
		if (*p == '\0')
			i--;
	if ((p - (u8 *) obj) & 0x01)
		p++;

	return ((struct fsObject *) p);
}

struct fsObject *asfs_find_obj_by_name(struct super_block *sb, struct fsObjectContainer *objcont, u8 * name)
{
	struct fsObject *obj;

	obj = &(objcont->object[0]);
	while (be32_to_cpu(obj->objectnode) > 0 && ((char *) obj - (char *) objcont) + sizeof(struct fsObject) + 2 < sb->s_blocksize) {
		if (asfs_namecmp(obj->name, name, ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE, NULL) == 0) {
			asfs_debug("Object found! Node %u, Name %s, Type %x, inCont %u\n", be32_to_cpu(obj->objectnode), obj->name, obj->bits, be32_to_cpu(objcont->bheader.ownblock));
			return obj;
		}
		obj = asfs_nextobject(obj);
	}
	return NULL;
}

#ifdef CONFIG_ASFS_RW

static struct fsObject *find_obj_by_node(struct super_block *sb, struct fsObjectContainer *objcont, u32 objnode)
{
	struct fsObject *obj;

	obj = &(objcont->object[0]);
	while (be32_to_cpu(obj->objectnode) > 0 && ((char *) obj - (char *) objcont) + sizeof(struct fsObject) + 2 < sb->s_blocksize) {
		if (be32_to_cpu(obj->objectnode) == objnode) {
			return obj;
		}
		obj = asfs_nextobject(obj);
	}
	return NULL;
}

int asfs_readobject(struct super_block *sb, u32 objectnode, struct buffer_head **bh, struct fsObject **returned_object)
{
	struct fsObjectNode *on;
	int errorcode;
	u32 contblock;

	asfs_debug("Seaching object - node %d\n", objectnode);

	if ((errorcode = asfs_getnode(sb, objectnode, bh, &on)) != 0)
		return errorcode;
	contblock = be32_to_cpu(on->node.data);
	asfs_brelse(*bh);

	if (contblock > 0 && (*bh = asfs_breadcheck(sb, contblock, ASFS_OBJECTCONTAINER_ID))) {
		*returned_object = find_obj_by_node(sb, (void *) (*bh)->b_data, objectnode);
		if (*returned_object == NULL) {
			brelse(*bh);
			*bh = NULL;
			return -ENOENT;
		}
		return 0;
	} else
		return -EIO;
}

static int removeobjectcontainer(struct super_block *sb, struct buffer_head *bh)
{
	struct fsObjectContainer *oc = (void *) bh->b_data;
	int errorcode;
	struct buffer_head *block;

	asfs_debug("removeobjectcontainer: block %u\n", be32_to_cpu(oc->bheader.ownblock));

	if (oc->next != 0 && oc->next != oc->bheader.ownblock) {
		struct fsObjectContainer *next_oc;

		if ((block = asfs_breadcheck(sb, be32_to_cpu(oc->next), ASFS_OBJECTCONTAINER_ID)) == NULL)
			return -EIO;

		next_oc = (void *) block->b_data;
		next_oc->previous = oc->previous;

		asfs_bstore(sb, block);
		asfs_brelse(block);
	}

	if (oc->previous != 0 && oc->previous != oc->bheader.ownblock) {
		struct fsObjectContainer *previous_oc;

		if ((block = asfs_breadcheck(sb, be32_to_cpu(oc->previous), ASFS_OBJECTCONTAINER_ID)) == NULL)
			return -EIO;

		previous_oc = (void *) block->b_data;
		previous_oc->next = oc->next;

		asfs_bstore(sb, block);
		asfs_brelse(block);
	} else {
		struct fsObject *parent_o;

		if ((errorcode = asfs_readobject(sb, be32_to_cpu(oc->parent), &block, &parent_o)) != 0)
			return (errorcode);

		parent_o->object.dir.firstdirblock = oc->next;

		asfs_bstore(sb, block);
		asfs_brelse(block);
	}

	if ((errorcode = asfs_freeadminspace(sb, be32_to_cpu(oc->bheader.ownblock))) != 0)
		return (errorcode);

	return (0);
}

static int setrecycledinfodiff(struct super_block *sb, s32 deletedfiles, s32 deletedblocks)
{
	struct buffer_head *bh;

	if ((bh = asfs_breadcheck(sb, ASFS_SB(sb)->rootobjectcontainer, ASFS_OBJECTCONTAINER_ID))) {
		struct fsRootInfo *ri = (struct fsRootInfo *) ((u8 *) bh->b_data + sb->s_blocksize - sizeof(struct fsRootInfo));

		ri->deletedfiles = cpu_to_be32(be32_to_cpu(ri->deletedfiles) + deletedfiles);
		ri->deletedblocks = cpu_to_be32(be32_to_cpu(ri->deletedblocks) + deletedblocks);

		asfs_bstore(sb, bh);
		asfs_brelse(bh);
	} else
		return -EIO;
	return 0;
}

	/* This function removes the fsObject structure passed in from the passed
	   buffer_head.  If the ObjectContainer becomes completely empty it will be 
	   delinked from the ObjectContainer chain and marked free for reuse.
	   This function doesn't delink the object from the hashchain! */

static int simpleremoveobject(struct super_block *sb, struct buffer_head *bh, struct fsObject *o)
{
	struct fsObjectContainer *oc = (void *) bh->b_data;
	int errorcode = 0;

	asfs_debug("simpleremoveobject:\n");

	if (be32_to_cpu(oc->parent) == ASFS_RECYCLEDNODE) {
		/* This object is removed from the Recycled directory. */
		if ((errorcode = setrecycledinfodiff(sb, -1, -((be32_to_cpu(o->object.file.size) + sb->s_blocksize - 1) >> sb->s_blocksize_bits))) != 0)
			return errorcode;
	}

	if ((asfs_nextobject(oc->object))->name[0] == '\0')
		errorcode = removeobjectcontainer(sb, bh);
	else {
		struct fsObject *nexto;
		int objlen;

		nexto = asfs_nextobject(o);
		objlen = (u8 *) nexto - (u8 *) o;

		memmove(o, nexto, sb->s_blocksize - ((u8 *) nexto - (u8 *) oc));
		memset((u8 *) oc + sb->s_blocksize - objlen, 0, objlen);

		asfs_bstore(sb, bh);
	}
	return errorcode;
}

/* This function delinks the passed in ObjectNode from its hash-chain.  Handy when deleting
   the object, or when renaming/moving it. */

static int dehashobjectquick(struct super_block *sb, u32 objectnode, u8 *name, u32 parentobjectnode)
{
	struct fsObject *o;
	int errorcode = 0;
	struct buffer_head *block;

	asfs_debug("dehashobject: Delinking object %d (=ObjectNode) from hashchain. Parentnode = %d\n", objectnode, parentobjectnode);

	if ((errorcode = asfs_readobject(sb, parentobjectnode, &block, &o)) == 0 && o->object.dir.hashtable != 0) {
		u32 hashtable = be32_to_cpu(o->object.dir.hashtable);
		asfs_brelse(block);

		if ((block = asfs_breadcheck(sb, hashtable, ASFS_HASHTABLE_ID))) {
			struct buffer_head *node_bh;
			struct fsObjectNode *onptr, on;
			struct fsHashTable *ht = (void *) block->b_data;
			u32 nexthash;

			if ((errorcode = asfs_getnode(sb, objectnode, &node_bh, &onptr)) == 0) {
				u16 hashchain;

				asfs_debug("dehashobject: Read HashTable block of parent object of object to be delinked\n");

				hashchain = HASHCHAIN(asfs_hash(name, ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE));
				nexthash = be32_to_cpu(ht->hashentry[hashchain]);

				if (nexthash == objectnode) {
					/* The hashtable directly points to the fsObject to be delinked.  We simply
					   modify the Hashtable to point to the new nexthash entry. */

					asfs_debug("dehashobject: The hashtable points directly to the to be delinked object\n");

					ht->hashentry[hashchain] = onptr->next;
					asfs_bstore(sb, block);
				} else {
					struct fsObjectNode *onsearch = 0;

					on = *onptr;

					asfs_debug("dehashobject: Walking through hashchain\n");

					while (nexthash != 0 && nexthash != objectnode) {
						asfs_brelse(node_bh);
						if ((errorcode = asfs_getnode(sb, nexthash, &node_bh, &onsearch)) != 0)
							break;
						nexthash = be32_to_cpu(onsearch->next);
					}

					if (errorcode == 0) {
						if (nexthash != 0) {
							/* Previous fsObjectNode found in hash chain.  Modify the fsObjectNode to 'skip' the
							   ObjectNode which is being delinked from the hash chain. */

							onsearch->next = on.next;
							asfs_bstore(sb, node_bh);
						} else {
							printk("ASFS: Hashchain of object %d is corrupt or incorrectly linked.", objectnode);

							/*** This is strange.  We have been looking for the fsObjectNode which is located before the
							     passed in fsObjectNode in the hash-chain.  However, we never found the
							     fsObjectNode reffered to in the hash-chain!  Has to be somekind
							     of internal error... */

							errorcode = -ENOENT;
						}
					}
				}
				asfs_brelse(node_bh);
			}
			asfs_brelse(block);
		}
	}
	return errorcode;
}


	/* This function removes an object from any directory.  It takes care
	   of delinking the object from the hashchain and also frees the
	   objectnode number. */

static int removeobject(struct super_block *sb, struct buffer_head *bh, struct fsObject *o)
{
	struct fsObjectContainer *oc = (void *) bh->b_data;
	int errorcode;

	asfs_debug("removeobject\n");

	if ((errorcode = dehashobjectquick(sb, be32_to_cpu(o->objectnode), o->name, be32_to_cpu(oc->parent))) == 0) {
		u32 objectnode = be32_to_cpu(o->objectnode);

		if ((errorcode = simpleremoveobject(sb, bh, o)) == 0)
			errorcode = asfs_deletenode(sb, objectnode);
	}

	return (errorcode);
}

	/* This function deletes the specified object. */
int asfs_deleteobject(struct super_block *sb, struct buffer_head *bh, struct fsObject *o)
{
	int errorcode = 0;

	asfs_debug("deleteobject: Entry -- deleting object %d (%s)\n", be32_to_cpu(o->objectnode), o->name);

	if ((o->bits & OTYPE_DIR) == 0 || o->object.dir.firstdirblock == 0) {
		u8 bits = o->bits;
		u32 hashblckno = be32_to_cpu(o->object.dir.hashtable);
		u32 extentbnode = be32_to_cpu(o->object.file.data);

		if ((errorcode = removeobject(sb, bh, o)) == 0) {
			if ((bits & OTYPE_LINK) != 0) {
				asfs_debug("deleteobject: Object is soft link!\n");
				errorcode = asfs_freeadminspace(sb, extentbnode);
			} else if ((bits & OTYPE_DIR) != 0) {
				asfs_debug("deleteobject: Object is a directory!\n");
				errorcode = asfs_freeadminspace(sb, hashblckno);
			} else {
				asfs_debug("deleteobject: Object is a file\n");
				if (extentbnode != 0)
					errorcode = asfs_deleteextents(sb, extentbnode);
			}
		}
	}

	return (errorcode);
}

	/* This function takes a HashBlock pointer, an ObjectNode and an ObjectName.
	   If there is a hashblock, then this function will correctly link the object
	   into the hashchain.  If there isn't a hashblock (=0) then this function
	   does nothing.  */

static int hashobject(struct super_block *sb, u32 hashblock, struct fsObjectNode *on, u32 nodeno, u8 *objectname)
{
	struct buffer_head *hash_bh;

	asfs_debug("hashobject, using hashblock %d\n", hashblock);
	if (hashblock == 0)
		return 0;

	if ((hash_bh = asfs_breadcheck(sb, hashblock, ASFS_HASHTABLE_ID))) {
		struct fsHashTable *ht = (void *) hash_bh->b_data;
		u32 nexthash;
		u16 hashvalue, hashchain;

		hashvalue = asfs_hash(objectname, ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE);
		hashchain = HASHCHAIN(hashvalue);
		nexthash = be32_to_cpu(ht->hashentry[hashchain]);

		ht->hashentry[hashchain] = cpu_to_be32(nodeno);

		asfs_bstore(sb, hash_bh);
		asfs_brelse(hash_bh);

		on->next = cpu_to_be32(nexthash);
		on->hash16 = cpu_to_be16(hashvalue);
	} else
		return -EIO;

	return 0;
}

	/* This function returns a pointer to the first unused byte in
	   an ObjectContainer. */

static u8 *emptyspaceinobjectcontainer(struct super_block *sb, struct fsObjectContainer *oc)
{
	struct fsObject *o = oc->object;
	u8 *endadr;

	endadr = (u8 *) oc + sb->s_blocksize - sizeof(struct fsObject) - 2;

	while ((u8 *) o < endadr && o->name[0] != 0)
		o = asfs_nextobject(o);

	return (u8 *) o;
}

	/* This function will look in the directory indicated by io_o
	   for an ObjectContainer block which contains bytesneeded free
	   bytes.  If none is found then this function simply creates a
	   new ObjectContainer and adds that to the indicated directory. */

static int findobjectspace(struct super_block *sb, struct buffer_head **io_bh, struct fsObject **io_o, u32 bytesneeded)
{
	struct buffer_head *bhparent = *io_bh;
	struct fsObject *oparent = *io_o;
	struct buffer_head *bh;
	u32 nextblock = be32_to_cpu(oparent->object.dir.firstdirblock);
	int errorcode = 0;

	asfs_debug("findobjectspace: Looking for %u bytes in directory with ObjectNode number %d (in block %d)\n", bytesneeded, be32_to_cpu((*io_o)->objectnode),
		   be32_to_cpu(((struct fsBlockHeader *) (*io_bh)->b_data)->ownblock));

	while (nextblock != 0 && (bh = asfs_breadcheck(sb, nextblock, ASFS_OBJECTCONTAINER_ID))) {
		struct fsObjectContainer *oc = (void *) bh->b_data;
		u8 *emptyspace;

		/* We need to find out how much free space this ObjectContainer has */

		emptyspace = emptyspaceinobjectcontainer(sb, oc);

		if ((u8 *) oc + sb->s_blocksize - emptyspace >= bytesneeded) {
			/* We found enough space in one of the ObjectContainer blocks!!
			   We return a struct fsObject *. */
			*io_bh = bh;
			*io_o = (struct fsObject *) emptyspace;
			break;
		}
		nextblock = be32_to_cpu(oc->next);
		asfs_brelse(bh);
	}

	if (nextblock == 0) {
		u32 newcontblock;
		/* If we get here, we traversed the *entire* directory (ough!) and found no empty
		   space large enough for our entry.  We allocate new space and add it to this
		   directory. */

		if ((errorcode = asfs_allocadminspace(sb, &newcontblock)) == 0 && (bh = asfs_getzeroblk(sb, newcontblock))) {
			struct fsObjectContainer *oc = (void *) bh->b_data;
			struct buffer_head *bhnext;

			asfs_debug("findobjectspace: No room was found, allocated new block at %u\n", newcontblock);

			/* Allocated new block.  We will now link it to the START of the directory chain
			   so the new free space can be found quickly when more entries need to be added. */

			oc->bheader.id = cpu_to_be32(ASFS_OBJECTCONTAINER_ID);
			oc->bheader.ownblock = cpu_to_be32(newcontblock);
			oc->parent = oparent->objectnode;
			oc->next = oparent->object.dir.firstdirblock;
			oc->previous = 0;

			oparent->object.dir.firstdirblock = cpu_to_be32(newcontblock);

			asfs_bstore(sb, bhparent);

			if (oc->next != 0 && (bhnext = asfs_breadcheck(sb, be32_to_cpu(oc->next), ASFS_OBJECTCONTAINER_ID))) {
				struct fsObjectContainer *ocnext = (void *) bhnext->b_data;
				ocnext->previous = cpu_to_be32(newcontblock);
				asfs_bstore(sb, bhnext);
				asfs_brelse(bhnext);
			}

			*io_bh = bh;
			*io_o = oc->object;
		}
	}

	asfs_debug("findobjectspace: new object will be in container block %u\n", be32_to_cpu(((struct fsBlockHeader *) (*io_bh)->b_data)->ownblock));

	return (errorcode);
}

/* io_bh & io_o refer to the direct parent of the new object.  Objectname is the
	name of the new object (name only). Does not realese io_bh !!! */

int asfs_createobject(struct super_block *sb, struct buffer_head **io_bh, struct fsObject **io_o, struct fsObject *src_o, u8 *objectname, int force)
{
	int errorcode;
	u32 object_size;
	u32 hashblock = be32_to_cpu((*io_o)->object.dir.hashtable);

	asfs_debug("createobject: Creating object '%s' in dir '%s'.\n", objectname, (*io_o)->name);

	if (!force && ASFS_SB(sb)->freeblocks < ASFS_ALWAYSFREE)
		return -ENOSPC;

	if (!force && be32_to_cpu((*io_o)->objectnode) == ASFS_RECYCLEDNODE)
		return -EINVAL;

	object_size = sizeof(struct fsObject) + strlen(objectname) + 2;

	if ((errorcode = findobjectspace(sb, io_bh, io_o, object_size)) == 0) {
		struct fsObject *o2 = *io_o;
		u8 *name = o2->name;
		u8 *objname = objectname;
		struct buffer_head *node_bh;
		struct fsObjectNode *on;
		u32 nodeno;

		**io_o = *src_o;	/* Copying whole object data... */

		while (*objname != 0)	/* Copying name */
			*name++ = *objname++;

		*name++ = 0;
		*name = 0;	/* zero byte for comment */

		if (o2->objectnode != 0)	/* ObjectNode reuse or creation */
			errorcode = asfs_getnode(sb, o2->objectnode, &node_bh, &on);
		else {
			if ((errorcode = asfs_createnode(sb, &node_bh, (struct fsNode **) &on, &nodeno)) == 0) {
				on->hash16 = cpu_to_be16(asfs_hash(o2->name, ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE));
				o2->objectnode = cpu_to_be32(nodeno);
			}
			asfs_debug("createnode returned with errorcode: %d\n", errorcode);
		}

		if (errorcode == 0) {	/* in io_bh there is a container with created object */
			on->node.data = ((struct fsBlockHeader *) (*io_bh)->b_data)->ownblock;
			if ((errorcode = hashobject(sb, hashblock, on, be32_to_cpu(o2->objectnode), objectname)) == 0) {
				asfs_bstore(sb, node_bh);
				asfs_brelse(node_bh);
			} else
				errorcode = -EIO;
		}

		if (errorcode == 0) {	/* HashBlock reuse or creation:*/

			if ((o2->bits & OTYPE_DIR) != 0 && o2->object.dir.hashtable == 0) {
				struct buffer_head *hashbh;
				u32 hashblock;

				asfs_debug("creating Hashblock\n");

				if ((errorcode = asfs_allocadminspace(sb, &hashblock)) == 0 && (hashbh = asfs_getzeroblk(sb, hashblock))) {	    
					struct fsHashTable *ht = (void *) hashbh->b_data;

					o2->object.dir.hashtable = cpu_to_be32(hashblock);

					ht->bheader.id = cpu_to_be32(ASFS_HASHTABLE_ID);
					ht->bheader.ownblock = cpu_to_be32(hashblock);
					ht->parent = o2->objectnode;

					asfs_bstore(sb, hashbh);
					asfs_brelse(hashbh);
				}
			}
		}

		if (errorcode == 0) {	/* SoftLink creation: */
			if ((o2->bits & (OTYPE_LINK | OTYPE_HARDLINK)) == OTYPE_LINK && o2->object.file.data == 0) {
				struct buffer_head *bh2;
				u32 slinkblock;

				if ((errorcode = asfs_allocadminspace(sb, &slinkblock)) == 0 && (bh2 = asfs_getzeroblk(sb, slinkblock))) {
					struct fsSoftLink *sl = (void *) bh2->b_data;
					o2->object.file.data = cpu_to_be32(slinkblock);
					sl->bheader.id = cpu_to_be32(ASFS_SOFTLINK_ID);
					sl->bheader.ownblock = cpu_to_be32(slinkblock);
					sl->parent = o2->objectnode;
					sl->next = 0;
					sl->previous = 0;
					asfs_bstore(sb, bh2);
					asfs_brelse(bh2);
				}
			}
		}
	}
	asfs_debug("createobject: done.\n");

	return (errorcode);
}

	/* This function extends the file object 'o' with a number  of blocks 
		(hopefully, if any blocks has been found!). Only new Extents will 
      be created -- the size of the file will not be altered, and changing 
		it is left up to the caller.  If the file did not have any blocks 
		yet, then the o->object.file.data will be set to the first (new) 
		ExtentBNode. It returns the number of added blocks through 
		addedblocks pointer */

int asfs_addblockstofile(struct super_block *sb, struct buffer_head *objbh, struct fsObject *o, u32 blocks, u32 * newspace, u32 * addedblocks)
{
	u32 lastextentbnode;
	int errorcode = 0;
	struct fsExtentBNode *ebnp;
	struct buffer_head *block = NULL;


	asfs_debug("extendblocksinfile: Trying to increasing number of blocks by %d.\n", blocks);

	lastextentbnode = be32_to_cpu(o->object.file.data);

	if (lastextentbnode != 0) {
		while (lastextentbnode != 0 && errorcode == 0) {
			if (block != NULL)
				asfs_brelse(block);
			errorcode = asfs_getextent(sb, lastextentbnode, &block, &ebnp);
			lastextentbnode = be32_to_cpu(ebnp->next);
		}
		lastextentbnode = be32_to_cpu(ebnp->key);
	}

	if (errorcode == 0) {
		u32 searchstart;

		u32 found_block;
		u32 found_blocks;

		*addedblocks = 0;
		*newspace = 0;

		if (lastextentbnode != 0)
			searchstart = be32_to_cpu(ebnp->key) + be16_to_cpu(ebnp->blocks);
		else
			searchstart = 0; //ASFS_SB(sb)->block_rovingblockptr;

		if ((errorcode = asfs_findspace(sb, blocks, searchstart, searchstart, &found_block, &found_blocks)) != 0) {
			asfs_brelse(block);
			asfs_debug("extendblocksinfile: findspace returned %s\n", errorcode == -ENOSPC ? "ENOSPC" : "error");
			return errorcode;
		}

		blocks = found_blocks;
		errorcode = asfs_markspace(sb, found_block, found_blocks);
		*addedblocks = found_blocks;
		*newspace = found_block;

		asfs_debug("extendblocksinfile: block = %u, lastextentbnode = %u, extentblocks = %d\n", found_block, lastextentbnode, blocks);

		if ((errorcode = asfs_addblocks(sb, blocks, found_block, be32_to_cpu(o->objectnode), &lastextentbnode)) != 0) {
			asfs_debug("extendblocksinfile: addblocks returned errorcode %d\n", errorcode);
			return errorcode;
		}

		if (o->object.file.data == 0)
			o->object.file.data = cpu_to_be32(lastextentbnode);
	}

	if (block)
		asfs_brelse(block);
	asfs_bstore(sb, objbh);

	asfs_debug("addblockstofile: done. added %d blocks\n", *addedblocks);

	return errorcode;
}

	/* The Object indicated by bh1 & o1, gets renamed to newname and placed
	   in the directory indicated by bhparent & oparent. */

int asfs_renameobject(struct super_block *sb, struct buffer_head *bh1, struct fsObject *o1, struct buffer_head *bhparent, struct fsObject *oparent, u8 * newname)
{
	struct fsObject object;
	u32 oldparentnode = be32_to_cpu(((struct fsObjectContainer *) bh1->b_data)->parent);
	u8 oldname[107];
	int errorcode;

	asfs_debug("renameobject: Renaming '%s' to '%s' in dir '%s'\n", o1->name, newname, oparent->name);

	object = *o1;
	strcpy(oldname, o1->name);

	if ((errorcode = dehashobjectquick(sb, be32_to_cpu(o1->objectnode), o1->name, oldparentnode)) == 0) {
		u32 parentobjectnode = be32_to_cpu(oparent->objectnode);

		if ((errorcode = simpleremoveobject(sb, bh1, o1)) == 0) {
			struct buffer_head *bh2 = bhparent;
			struct fsObject *o2;

			/* oparent might changed after simpleremoveobject */
			oparent = o2 = find_obj_by_node(sb, (struct fsObjectContainer *) bhparent->b_data, parentobjectnode);

			/* In goes the Parent bh & o, out comes the New object's bh & o :-) */
			if ((errorcode = asfs_createobject(sb, &bh2, &o2, &object, newname, TRUE)) == 0) {
				asfs_bstore(sb, bh2);
				if (be32_to_cpu(oparent->objectnode) == ASFS_RECYCLEDNODE) {
					asfs_debug("renameobject: Updating recycled dir info\n");
					if ((errorcode = setrecycledinfodiff(sb, 1, (be32_to_cpu(o2->object.file.size) + sb->s_blocksize - 1) >> sb->s_blocksize_bits)) != 0) {
						brelse(bh2);
						return errorcode;
					}
				}
				brelse(bh2);
				asfs_debug("renameobject: Succesfully created & stored new object.\n");
			} else { /* recreate object in old place, maybe this will not fail, but who knows... */
				asfs_debug("renameobject: Creating new object failed. Trying to recreate it in source directory.\n");
				if (asfs_readobject(sb, oldparentnode, &bh1, &o1) == 0) {
					struct buffer_head *bh2 = bh1;
					if (asfs_createobject(sb, &bh2, &o1, &object, oldname, TRUE) == 0) {
						asfs_bstore(sb, bh2);
						if (oldparentnode == ASFS_RECYCLEDNODE) {
							asfs_debug("renameobject: Updating recycled dir info\n");
							setrecycledinfodiff(sb, 1, (be32_to_cpu(o1->object.file.size) + sb->s_blocksize - 1) >> sb->s_blocksize_bits);
						}
						brelse(bh2);
					}
					brelse(bh1);
				}
			}
		}
	}
	return errorcode;
}

		/* Truncates the specified file to /newsize/ bytes */

int asfs_truncateblocksinfile(struct super_block *sb, struct buffer_head *bh, struct fsObject *o, u32 newsize)
{
	struct buffer_head *ebh;
	struct fsExtentBNode *ebn;
	int errorcode;
	u32 pos = 0;
	u32 newblocks = (newsize + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	u32 filedata = be32_to_cpu(o->object.file.data);
	u32 eprev, ekey;
	u16 eblocks;

	asfs_debug("trucateblocksinfile: newsize %u\n", newsize);

	if (filedata == 0)
		return 0;

	for (;;) {
		if ((errorcode = asfs_getextent(sb, filedata, &ebh, &ebn)) != 0)
			return errorcode;
		if (pos + be16_to_cpu(ebn->blocks) >= newblocks)
			break;
		pos += be16_to_cpu(ebn->blocks);
		if ((filedata = be32_to_cpu(ebn->next)) == 0)
			break;
		asfs_brelse(ebh);
	};

	eblocks = newblocks - pos;
	ekey = be32_to_cpu(ebn->key);
	eprev = be32_to_cpu(ebn->prev);

	if (be16_to_cpu(ebn->blocks) < eblocks) {
		printk("ASFS: Extent chain is too short or damaged!\n");
		asfs_brelse(ebh);
		return -ENOENT;
	}
	if (be16_to_cpu(ebn->blocks) - eblocks > 0 && (errorcode = asfs_freespace(sb, be32_to_cpu(ebn->key) + eblocks, be16_to_cpu(ebn->blocks) - eblocks)) != 0) {
		asfs_brelse(ebh);
		return errorcode;
	}
	if (be32_to_cpu(ebn->next) > 0 && (errorcode = asfs_deleteextents(sb, be32_to_cpu(ebn->next))) != 0) {
		asfs_brelse(ebh);
		return errorcode;
	}
	ebn->blocks = cpu_to_be16(eblocks);
	ebn->next = 0;
	asfs_bstore(sb, ebh);

	if (eblocks == 0) {
		if (eprev & MSB_MASK) {
			o->object.file.data = 0;
			asfs_bstore(sb, bh);
		} else {
			struct buffer_head *ebhp;
			struct fsExtentBNode *ebnp;

			if ((errorcode = asfs_getextent(sb, eprev & !MSB_MASK, &ebhp, &ebnp)) != 0) {
				asfs_brelse(ebh);
				return errorcode;
			}

			ebnp->next = 0;
			asfs_bstore(sb, ebhp);
			asfs_brelse(ebhp);
		}
		if ((errorcode = asfs_deletebnode(sb, ebh, ekey)) != 0) {
			asfs_brelse(ebh);
			return errorcode;
		}
	}
	asfs_brelse(ebh);

	return 0;
}
#endif
