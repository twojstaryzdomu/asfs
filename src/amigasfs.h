#ifndef __LINUX_AMIGASFS_H
#define __LINUX_AMIGASFS_H

#include <linux/types.h>

/* some helper macros... */
#define ASFS_MAKE_ID(a,b,c,d) (((a)&0xff)<<24|((b)&0xff)<<16|((c)&0xff)<<8|((d)&0xff))

/* Amiga SFS block IDs */
#define ASFS_ROOTID                 ASFS_MAKE_ID('S','F','S','\0')
#define ASFS_OBJECTCONTAINER_ID     ASFS_MAKE_ID('O','B','J','C')
#define ASFS_BNODECONTAINER_ID      ASFS_MAKE_ID('B','N','D','C')
#define ASFS_NODECONTAINER_ID       ASFS_MAKE_ID('N','D','C',' ')
#define ASFS_HASHTABLE_ID           ASFS_MAKE_ID('H','T','A','B')
#define ASFS_SOFTLINK_ID            ASFS_MAKE_ID('S','L','N','K')
#define ASFS_ADMINSPACECONTAINER_ID ASFS_MAKE_ID('A','D','M','C')
#define ASFS_BITMAP_ID              ASFS_MAKE_ID('B','T','M','P')
#define ASFS_TRANSACTIONFAILURE_ID  ASFS_MAKE_ID('T','R','F','A')

/* Amiga SFS defines and magic values */

#define ASFS_MAGIC 0xa0ff
#define ASFS_MAXFN (105u)
#define ASFS_MAXFILESIZE 0x8FFFFFFE

#define ASFS_STRUCTURE_VERISON (3)
#define ASFS_BLCKFACCURACY	(5)

#define ASFS_ROOTBITS_CASESENSITIVE (128)
#define ASFS_READONLY (512)
#define ASFS_VOL_LOWERCASE (1024)

#define ASFS_ROOTNODE   (1)
#define ASFS_RECYCLEDNODE (2)

#define OTYPE_HIDDEN      (1)
#define OTYPE_HARDLINK    (32)
#define OTYPE_LINK        (64)
#define OTYPE_DIR         (128)

#define MSB_MASK (1ul << 31)

#define NODE_STRUCT_SIZE (10)	/* (sizeof(struct fsObjectNode)) */
#define NODECONT_BLOCK_COUNT ((sb->s_blocksize - sizeof(struct fsNodeContainer)) / sizeof(u32))

#define ASFS_ALWAYSFREE (16)		/* keep this amount of blocks free */

#define ASFS_BLOCKCHUNKS (16)		/* try to allocate this number of blocks in one request */

#ifndef TRUE
#define TRUE		1
#endif
#ifndef FALSE
#define FALSE		0
#endif

/* amigados protection bits */

#define FIBB_SCRIPT    6	/* program is a script (execute) file */
#define FIBB_PURE      5	/* program is reentrant and rexecutable */
#define FIBB_ARCHIVE   4	/* cleared whenever file is changed */
#define FIBB_READ      3	/* ignored by old filesystem */
#define FIBB_WRITE     2	/* ignored by old filesystem */
#define FIBB_EXECUTE   1	/* ignored by system, used by Shell */
#define FIBB_DELETE    0	/* prevent file from being deleted */

#define FIBF_SCRIPT    (1<<FIBB_SCRIPT)
#define FIBF_PURE      (1<<FIBB_PURE)
#define FIBF_ARCHIVE   (1<<FIBB_ARCHIVE)
#define FIBF_READ      (1<<FIBB_READ)
#define FIBF_WRITE     (1<<FIBB_WRITE)
#define FIBF_EXECUTE   (1<<FIBB_EXECUTE)
#define FIBF_DELETE    (1<<FIBB_DELETE)

/* name hashing macro */

#define HASHCHAIN(x) (u16)(x % (u16)(((sb->s_blocksize) - sizeof(struct fsHashTable))>>2))

/* Each block has its own header with checksum and id, its called fsBlockHeader */

struct fsBlockHeader {
	u32 id;			/* 4 character id string of this block */
	u32 checksum;		/* The checksum */
	u32 ownblock;		/* The blocknumber of the block this block is stored at */
};

/* On-disk "super block", called fsRootBlock */

struct fsRootBlock {
	struct fsBlockHeader bheader;

	u16 version;		/* Version number of the filesystem block structure */
	u16 sequencenumber;	/* The Root with the highest sequencenumber is valid */

	u32 datecreated;	/* Creation date (when first formatted).  Cannot be changed. */
	u8 bits;		/* various settings, see defines below. */
	u8 pad1;
	u16 pad2;

	u32 reserved1[2];

	u32 firstbyteh;		/* The first byte of our partition from the start of the */
	u32 firstbyte;		/* disk.  firstbyteh = upper 32 bits, firstbyte = lower 32 bits. */

	u32 lastbyteh;		/* The last byte of our partition, excluding this one. */
	u32 lastbyte;

	u32 totalblocks;	/* size of this partition in blocks */
	u32 blocksize;		/* blocksize used */

	u32 reserved2[2];
	u32 reserved3[8];

	u32 bitmapbase;		/* location of the bitmap */
	u32 adminspacecontainer;	/* location of first adminspace container */
	u32 rootobjectcontainer;	/* location of the root objectcontainer */
	u32 extentbnoderoot;	/* location of the root of the extentbnode B-tree */
	u32 objectnoderoot;	/* location of the root of the objectnode tree */

	u32 reserved4[3];
};

/* On disk inode, called fsObject */

struct fsObject {
	u16 owneruid;
	u16 ownergid;
	u32 objectnode;
	u32 protection;

	union {
		struct {
			u32 data;
			u32 size;
		} file;

		struct {
			u32 hashtable;	/* for directories & root, 0 means no hashblock */
			u32 firstdirblock;
		} dir;
	} object;

	u32 datemodified;
	u8 bits;

	u8 name[0];
	u8 comment[0];
};

/* On disk block containging a number of fsObjects */

struct fsObjectContainer {
	struct fsBlockHeader bheader;

	u32 parent;
	u32 next;
	u32 previous;		/* 0 for the first block in the directory chain */

	struct fsObject object[0];
};

/* BTree structures, used to collect file data position on disk */

struct fsExtentBNode {
	u32 key;		/* data! */
	u32 next;
	u32 prev;
	u16 blocks;		/* The size in blocks of the region this Extent controls */
};

struct BNode {
	u32 key;
	u32 data;
};

struct BTreeContainer {
	u16 nodecount;
	u8 isleaf;
	u8 nodesize;		/* Must be a multiple of 2 */

	struct BNode bnode[0];
};

/* On disk block with BTreeContainer */

struct fsBNodeContainer {
	struct fsBlockHeader bheader;
	struct BTreeContainer btc;
};

/* On disk block  with soft link data */

struct fsSoftLink {
	struct fsBlockHeader bheader;
	u32 parent;
	u32 next;
	u32 previous;
	u8 string[0];
};

/* On disk block with hashtable data */

struct fsHashTable {
	struct fsBlockHeader bheader;
	u32 parent;
	u32 hashentry[0];
};

/* On disk block with node index and some helper structures */

struct fsNodeContainer {
	struct fsBlockHeader bheader;
	u32 nodenumber;
	u32 nodes;
	u32 node[0];
};

struct fsNode {
	u32 data;
};

struct fsObjectNode {
	struct fsNode node;
	u32 next;
	u16 hash16;
} __attribute__ ((packed));

/* Some adminspace and bitmap block structures */

struct fsAdminSpace {
	u32 space;
	u32 bits;		
/* Set bits are used blocks, bit 31 is	the first block in the AdminSpace. */
};

struct fsAdminSpaceContainer {
	struct fsBlockHeader bheader;

	u32 next;
	u32 previous;

	u8 bits;
	u8 pad1;
	u16 pad2;

	struct fsAdminSpace adminspace[0];
};

struct fsBitmap {
	struct fsBlockHeader bheader;

	u32 bitmap[0];

/* Bits are 1 if the block is free, and 0 if full.
   Bitmap must consist of an integral number of longwords. */
};

/* The fsRootInfo structure has all kinds of information about the format
   of the disk. */

struct fsRootInfo {
	u32 deletedblocks;	/* Amount in blocks which deleted files consume. */
	u32 deletedfiles;	/* Number of deleted files in recycled. */
	u32 freeblocks;		/* Cached number of free blocks on disk. */

	u32 datecreated;

	u32 lastallocatedblock;	/* Block which was most recently allocated */
	u32 lastallocatedadminspace;	/* AdminSpaceContainer which most recently was used to allocate a block */
	u32 lastallocatedextentnode;	/* ExtentNode which was most recently created */
	u32 lastallocatedobjectnode;	/* ObjectNode which was most recently created */

	u32 rovingpointer;
};

#endif
