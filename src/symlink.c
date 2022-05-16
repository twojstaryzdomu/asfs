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
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/pagemap.h>
#include <linux/nls.h>
#include "asfs_fs.h"

#include <asm/byteorder.h>
#include <asm/uaccess.h>

int asfs_symlink_readpage(struct file *file, struct page *page)
{
	struct buffer_head *bh;
	struct fsSoftLink *slinkcont;
	struct inode *inode = page->mapping->host;
	struct super_block *sb = inode->i_sb;
	struct nls_table *nls_io = ASFS_SB(sb)->nls_io;
	struct nls_table *nls_disk = ASFS_SB(sb)->nls_disk;
	char *link = kmap(page);
	int i = 0, j = 0;
	char c, lc = 0, *prefix, *lf, *p;
	wchar_t uni;
	int clen;

	asfs_debug("asfs_read_symlink from node %d\n", (int)inode->i_ino);

	if (!(bh = asfs_breadcheck(sb, ASFS_I(inode)->firstblock, ASFS_SOFTLINK_ID))) {
		SetPageError(page);
		kunmap(page);
		unlock_page(page);
		return -EIO;
	}
	slinkcont = (struct fsSoftLink *) bh->b_data;

	lf = slinkcont->string;
	prefix = ASFS_SB(sb)->prefix ? ASFS_SB(sb)->prefix : "/";

	asfs_debug("asfs_read_symlink: link target %s\n", lf);

	if ((p = strchr(lf,':'))) {	/* Handle assign or volume name */
		if (ASFS_SB(sb)->root_volume &&
		    strncmp(lf, ASFS_SB(sb)->root_volume, strlen(ASFS_SB(sb)->root_volume)) == 0) {
			/* global root volume name found */
			link[i++] = '/';
			lf = p+1;
		} else {
			/* adding volume prefix */ 
			while (i < 1023 && (c = prefix[i]))
				link[i++] = c;
			while (i < 1023 && lf[j] != ':')
			{
				c = lf[j++];
				if (ASFS_SB(sb)->flags & ASFS_VOL_LOWERCASE)
					c = asfs_lowerchar(c);
				if (nls_io)
				{
					clen = nls_disk->char2uni(&c, 1, &uni);
					if (clen>0) {
						clen = nls_io->uni2char(uni, &link[i], NLS_MAX_CHARSET_SIZE);
						if (clen>0)
							i += clen;
					}
					if (clen<0)
						link[i++] = '?';
				} else
					link[i++] = c;
			}
			if (i < 1023)
				link[i++] = '/';
			j++;
		}
		lc = '/';
	}

	while (i < 1023 && (c = lf[j])) {
		if (c == '/' && lc == '/' && i < 1020) {	/* parent dir */
			link[i++] = '.';
			link[i++] = '.';
		}
		lc = c;
		if (nls_io)
		{
			clen = nls_disk->char2uni(&c, 1, &uni);
			if (clen>0) {
				clen = nls_io->uni2char(uni, &link[i], NLS_MAX_CHARSET_SIZE);
				if (clen>0)
					i += clen;
			}
			if (clen<0)
				link[i++] = '?';
		} else
			link[i++] = c;
		j++;
	}
	link[i] = '\0';

	SetPageUptodate(page);
	kunmap(page);
	unlock_page(page);
	asfs_brelse(bh);
	return 0;
}

#ifdef CONFIG_ASFS_RW

int asfs_write_symlink(struct inode *symfile, const char *symname)
{
	struct super_block *sb = symfile->i_sb;
	struct buffer_head *bh;
	struct fsSoftLink *slinkcont;
	struct nls_table *nls_io = ASFS_SB(sb)->nls_io;
	struct nls_table *nls_disk = ASFS_SB(sb)->nls_disk;
	char *p, c, lc;
	int i, maxlen, pflen;
	wchar_t uni;
	int clen, blen;

	asfs_debug("asfs_write_symlink %s to node %d\n", symname, (int)symfile->i_ino);

	if (!(bh = asfs_breadcheck(sb, ASFS_I(symfile)->firstblock, ASFS_SOFTLINK_ID))) {
		return -EIO;
	}
	slinkcont = (struct fsSoftLink *) bh->b_data;

	/* translating symlink target path */

	maxlen = sb->s_blocksize - sizeof(struct fsSoftLink) - 2;
	i  = 0;
	p  = slinkcont->string;
	lc = '/';

	if (*symname == '/') {
		while (*symname == '/')
			symname++;
		if (ASFS_SB(sb)->prefix &&
		    strncmp(symname-1, ASFS_SB(sb)->prefix, (pflen = strlen(ASFS_SB(sb)->prefix))) == 0) {
			/* found volume prefix, ommiting it */
			symname += pflen;
			blen = strlen(symname);
			while (*symname != '/' && *symname != '\0') {
				if (nls_io) {
					clen = nls_io->char2uni(symname, blen, &uni);
					if (clen>0) {
						symname += clen;
						blen -= clen;
						clen = nls_disk->uni2char(uni, p, NLS_MAX_CHARSET_SIZE);
						if (clen>0)
							p += clen;
					}
					else
					{
						symname++;
						blen--;
					}
					if (clen<0)
						*p++ = '?';
				} else {
					*p++ = *symname++;
				}
				i++;
			}
			symname++;
			*p++ = ':';
		} else if (ASFS_SB(sb)->root_volume) {	/* adding root volume name */
			while (ASFS_SB(sb)->root_volume[i])
				*p++ = ASFS_SB(sb)->root_volume[i++];
			*p++ = ':';
		} else {	/* do nothing */
			*p++ = '/';
		}
		i++;
	}

	blen = strlen(symname);
	while (i < maxlen && (c = *symname)) {
		if (c == '.' && lc == '/' && symname[1] == '.' && symname[2] == '/') {
			*p++ = '/';
			i++;
			symname += 3;
			blen -= 3;
			lc = '/';
		} else if (c == '.' && lc == '/' && symname[1] == '/') {
			symname += 2;
			blen -= 2;
			lc = '/';
		} else {
			if (nls_io) {
				clen = nls_io->char2uni(symname, blen, &uni);
				if (clen>0) {
					symname += clen;
					blen -= clen;
					clen = nls_disk->uni2char(uni, p, NLS_MAX_CHARSET_SIZE);
					if (clen>0)
						lc = *p;
					p += clen;
				} else {
					symname++;
					blen--;
				}
				if (clen<0) {
					*p++ = '?';
					lc = '?';
				}
			} else {
				*p++ = c;
				lc = *p;
				symname++;
				blen--;
			}
			i++;
		}
		if (lc == '/')
			while (*symname == '/')
			{
				symname++;
				blen--;
			}
	}
	*p = 0;

	asfs_debug("asfs_write_symlink: saved %s\n", slinkcont->string);

	asfs_bstore(sb, bh);
	asfs_brelse(bh);

	return 0;
}

#endif
