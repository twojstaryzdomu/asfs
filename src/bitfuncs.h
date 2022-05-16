/*
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#ifndef __BITFUNCS_H
#define __BITFUNCS_H

#include <linux/types.h>
#include <asm/byteorder.h>

#include <asm/bitops.h>
#include <linux/bitops.h>

/* Finds first set bit in /data/ starting at /bitoffset/.  This function
   considers the MSB to be the first bit. */
static inline int bfffo(u32 data, int bitoffset)
{
	u32 mask = 0xffffffff >> bitoffset;
	data &= mask;
	return data == 0 ? -1 : 32-fls(data);
}

/* Finds first zero bit in /data/ starting at /bitoffset/.  This function
   considers the MSB to be the first bit. */
static inline int bfffz(u32 data, int bitoffset)
{
	return bfffo(~data, bitoffset);
}

/* Sets /bits/ bits starting from /bitoffset/ in /data/.
   /bits/ must be between 1 and 32. */
static inline u32 bfset(u32 data, int bitoffset, int bits)
{
	u32 mask = ~((1 << (32 - bits)) - 1);
	mask >>= bitoffset;
	return data | mask;
}

/* Clears /bits/ bits starting from /bitoffset/ in /data/.
   /bits/ must be between 1 and 32. */
static inline u32 bfclr(u32 data, int bitoffset, int bits)
{
	u32 mask = ~((1 << (32 - bits)) - 1);
	mask >>= bitoffset;
	return data & ~mask;
}

/* bm??? functions assumes that in-memory bitmap is in bigendian byte order */
int bmffo(u32 *, int, int);
int bmffz(u32 *, int, int);
int bmclr(u32 *, int, int, int);
int bmset(u32 *, int, int, int);

#endif
