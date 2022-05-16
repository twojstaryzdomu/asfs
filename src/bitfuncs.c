/*
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <linux/types.h>
#include <linux/bitops.h>
#include "bitfuncs.h"

/* Bitmap (bm) functions:
   These functions perform bit-operations on regions of memory which
   are a multiple of 4 bytes in length. Bitmap is in bigendian byte order.
*/

/* This function finds the first set bit in a region of memory starting
   with /bitoffset/.  The region of memory is /longs/ longs long.  It
   returns the bitoffset of the first set bit it finds. */

int bmffo(u32 *bitmap, int longs, int bitoffset)
{
	u32 *scan = bitmap;
	int longoffset, bit;

	longoffset = bitoffset >> 5;
	longs -= longoffset;
	scan += longoffset;

	bitoffset = bitoffset & 0x1F;

	if (bitoffset != 0) {
		if ((bit = bfffo(be32_to_cpu(*scan), bitoffset)) >= 0) {
			return (bit + ((scan - bitmap) << 5));
		}
		scan++;
		longs--;
	}

	while (longs-- > 0) {
		if (*scan++ != 0) {
			return (bfffo(be32_to_cpu(*--scan), 0) + ((scan - bitmap) << 5));
		}
	}

	return (-1);
}

/* This function finds the first unset bit in a region of memory starting
   with /bitoffset/.  The region of memory is /longs/ longs long.  It
   returns the bitoffset of the first unset bit it finds. */

int bmffz(u32 *bitmap, int longs, int bitoffset)
{
	u32 *scan = bitmap;
	int longoffset, bit;

	longoffset = bitoffset >> 5;
	longs -= longoffset;
	scan += longoffset;

	bitoffset = bitoffset & 0x1F;

	if (bitoffset != 0) {
		if ((bit = bfffz(be32_to_cpu(*scan), bitoffset)) >= 0) {
			return (bit + ((scan - bitmap) << 5));
		}
		scan++;
		longs--;
	}

	while (longs-- > 0) {
		if (*scan++ != 0xFFFFFFFF) {
			return (bfffz(be32_to_cpu(*--scan), 0) + ((scan - bitmap) << 5));
		}
	}

	return (-1);
}

/* This function clears /bits/ bits in a region of memory starting
   with /bitoffset/.  The region of memory is /longs/ longs long.  If
   the region of memory is too small to clear /bits/ bits then this
   function exits after having cleared all bits till the end of the
   memory region.  In any case it returns the number of bits which
   were actually cleared. */

int bmclr(u32 *bitmap, int longs, int bitoffset, int bits)
{
	u32 *scan = bitmap;
	int longoffset;
	int orgbits = bits;

	longoffset = bitoffset >> 5;
	longs -= longoffset;
	scan += longoffset;

	bitoffset = bitoffset & 0x1F;

	if (bitoffset != 0) {
		if (bits < 32) {
			*scan = cpu_to_be32(bfclr(be32_to_cpu(*scan), bitoffset, bits));
		} else {
			*scan = cpu_to_be32(bfclr(be32_to_cpu(*scan), bitoffset, 32));
		}
		scan++;
		longs--;
		bits -= 32 - bitoffset;
	}

	while (bits > 0 && longs-- > 0) {
		if (bits > 31) {
			*scan++ = 0;
		} else {
			*scan = cpu_to_be32(bfclr(be32_to_cpu(*scan), 0, bits));
		}
		bits -= 32;
	}

	if (bits <= 0) {
		return (orgbits);
	}
	return (orgbits - bits);
}

/* This function sets /bits/ bits in a region of memory starting
   with /bitoffset/.  The region of memory is /longs/ longs long.  If
   the region of memory is too small to set /bits/ bits then this
   function exits after having set all bits till the end of the
   memory region.  In any case it returns the number of bits which
   were actually set. */

int bmset(u32 *bitmap, int longs, int bitoffset, int bits)
{
	u32 *scan = bitmap;
	int longoffset;
	int orgbits = bits;

	longoffset = bitoffset >> 5;
	longs -= longoffset;
	scan += longoffset;

	bitoffset = bitoffset & 0x1F;

	if (bitoffset != 0) {
		if (bits < 32) {
			*scan = cpu_to_be32(bfset(be32_to_cpu(*scan), bitoffset, bits));
		} else {
			*scan = cpu_to_be32(bfset(be32_to_cpu(*scan), bitoffset, 32));
		}
		scan++;
		longs--;
		bits -= 32 - bitoffset;
	}

	while (bits > 0 && longs-- > 0) {
		if (bits > 31) {
			*scan++ = 0xFFFFFFFF;
		} else {
			*scan = cpu_to_be32(bfset(be32_to_cpu(*scan), 0, bits));
		}
		bits -= 32;
	}

	if (bits <= 0) {
		return (orgbits);
	}
	return (orgbits - bits);
}
