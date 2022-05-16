/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta10
 *
 * Copyright (C) 2003,2004,2005  Marek 'March' Szyprowski <marek@amiga.pl>
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
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/string.h>
#include <linux/nls.h>
#include "asfs_fs.h"

static inline u8 asfs_upperchar(u8 c)
{
	if ((c >= 224 && c <= 254 && c != 247) || (c >= 'a' && c <= 'z'))
		c -= 32;
	return (c);
}

u8 asfs_lowerchar(u8 c)
{
	if ((c >= 192 && c <= 222 && c != 215) || (c >= 'A' && c <= 'Z'))
		c += 32;
	return (c);
}

static inline u8 asfs_nls_upperchar(u8 c, struct nls_table *t)
{
	if (t) {
		u8 nc = t->charset2upper[c];
		return nc ? nc : c;
	} else
		return asfs_upperchar(c);
}

/* Check if the name is valid for a asfs object. */

inline int asfs_check_name(const u8 *name, int len)
{
	int i;

	if (len > ASFS_MAXFN)
		return -ENAMETOOLONG;

	for (i = 0; i < len; i++)
		if (name[i] < ' ' || name[i] == ':' || (name[i] > 0x7e && name[i] < 0xa0))
			return -EINVAL;

	return 0;
}

/* Note: the dentry argument is the parent dentry. */

static int asfs_hash_dentry(struct dentry *dentry, struct qstr *qstr)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	const u8 *name = qstr->name;
	unsigned long hash;
	int i;
	struct nls_table *nls_io = ASFS_SB(sb)->nls_io;

	i = asfs_check_name(qstr->name,qstr->len);
	if (i)
		return i;

	hash = init_name_hash();

	if (ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE)
		for (i=qstr->len; i > 0; name++, i--)
			hash = partial_name_hash(*name, hash);
	else
		for (i=qstr->len; i > 0; name++, i--)
			hash = partial_name_hash(asfs_nls_upperchar(*name, nls_io), hash);

	qstr->hash = end_name_hash(hash);

	return 0;
}

static int asfs_compare_dentry(struct dentry *dentry, struct qstr *a, struct qstr *b)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	const u8 *aname = a->name;
	const u8 *bname = b->name;
	int len;
	struct nls_table *nls_io = ASFS_SB(sb)->nls_io;

	/* 'a' is the qstr of an already existing dentry, so the name
	 * must be valid. 'b' must be validated first.
	 */

	if (asfs_check_name(b->name,b->len))
		return 1;

	if (a->len != b->len)
		return 1;

	if (ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE) {
		for (len=a->len; len > 0; len--)
			if (*aname++ != *bname++)
				return 1;
	} else {
		for (len=a->len; len > 0; len--)
			if (asfs_nls_upperchar(*aname++, nls_io) != asfs_nls_upperchar(*bname++, nls_io))
				return 1;
	}

	return 0;
}

struct dentry_operations asfs_dentry_operations = {
	d_hash:		asfs_hash_dentry,
	d_compare:	asfs_compare_dentry,
};

int asfs_namecmp(u8 *s, u8 *ct, int casesensitive, struct nls_table *t)
{
	if (casesensitive) {
		while (*s == *ct && *ct != '\0' && *ct != '/') {
			s++;
			ct++;
		}
	} else {
		while (asfs_nls_upperchar(*s, t) == asfs_nls_upperchar(*ct, t) && *ct != '\0'
		       && *ct != '/') {
			s++;
			ct++;
		}
	}
	return (*s == '\0' && (*ct == '\0' || *ct == '/')) ? 0 : *ct - *s;
}

u16 asfs_hash(u8 *name, int casesensitive)
{
	u16 hashval = 0;
	while (name[hashval] != 0 && name[hashval] != '/')
		hashval++;
	if (casesensitive) {
		u8 c = *name;
		while (c != 0 && c != '/') {
			hashval = hashval * 13 + c;
			c = *++name;
		}
	} else {
		u8 c = *name;
		while (c != 0 && c != '/') {
			hashval = hashval * 13 + asfs_upperchar(c);
			c = *++name;
		}
	}
	return hashval;
}

void asfs_translate(u8 *to, u8 *from, struct nls_table *nls_to, struct nls_table *nls_from, int limit)
{
	wchar_t uni;
	int i, len;
	int from_len, to_len = limit;

	if (nls_to) {
		from_len = strlen(from);
		for (i=0; i < from_len && to_len > 1; ) {
			len = nls_from->char2uni(&from[i], from_len-i, &uni);
			if (len > 0) {
				i += len;
				len = nls_to->uni2char(uni, to, to_len);
				if (len > 0) {
					to += len;
					to_len -= len;
				}
			} else
				i++;
			if (len < 0) {
				*to++ = '?';
				to_len--;
			}
		}
		*to = '\0';
	} else {
		strncpy (to, from, limit);
		to[limit-1] = '\0';
	}
}
