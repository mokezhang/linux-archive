/*
 *  linux/fs/ext2/balloc.c
 *
 *  Copyright (C) 1992, 1993  Remy Card (card@masi.ibp.fr)
 *
 *  Enhanced block allocation by Stephen Tweedie (sct@dcs.ed.ac.uk), 1993
 */

/* balloc.c contains the blocks allocation and deallocation routines */

/*

   The free blocks are managed by bitmaps.  A file system contains several
   blocks groups.  Each group contains 1 bitmap block for blocks, 1 bitmap
   block for inodes, N blocks for the inode table and data blocks.

   The file system contains group descriptors which are located after the
   super block.  Each descriptor contains the number of the bitmap block and
   the free blocks count in the block.  The descriptors are loaded in memory
   when a file system is mounted (see ext2_read_super).

*/

#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/locks.h>

#include <asm/bitops.h>

#define clear_block(addr,size) \
	__asm__("cld\n\t" \
		"rep\n\t" \
		"stosl" \
		: \
		:"a" (0), "c" (size / 4), "D" ((long) (addr)) \
		:"cx", "di")

static inline int find_first_zero_bit (unsigned long * addr, unsigned size)
{
	int res;

	if (!size)
		return 0;
	__asm__("
		cld
		movl $-1,%%eax
		repe; scasl
		je 1f
		subl $4,%%edi
		movl (%%edi),%%eax
		notl %%eax
		bsfl %%eax,%%edx
		jmp 2f
1:		xorl %%edx,%%edx
2:		subl %%ebx,%%edi
		shll $3,%%edi
		addl %%edi,%%edx"
		:"=d" (res)
		:"c" ((size + 31) >> 5), "D" (addr), "b" (addr)
		:"ax", "bx", "cx", "di");
	return res;
}

static inline int find_next_zero_bit (unsigned long * addr, int size,
				      int offset)
{
	unsigned long * p = ((unsigned long *) addr) + (offset >> 5);
	int set = 0, bit = offset & 31, res;
	
	if (bit) {
		/* Look for zero in first byte */
		__asm__("
			bsfl %1,%0
			jne 1f
			movl $32, %0
1:			"
			: "=r" (set)
			: "r" (~(*p >> bit)));
		if (set < (32 - bit))
			return set + offset;
		set = 32 - bit;
		p++;
	}
	/* No zero yet, search remaining full bytes for a zero */
	res = find_first_zero_bit (p, size - 32 * (p - addr));
	return (offset + set + res);
}

static inline char * find_first_zero_byte (char * addr, int size)
{
	char *res;

	if (!size)
		return 0;
	__asm__("
		cld
		mov $0,%%eax
		repnz; scasb
		jnz 1f
		dec %%edi
1:		"
		: "=D" (res)
		: "0" (addr), "c" (size)
		: "ax");
	return res;
}

static void read_block_bitmap (struct super_block * sb,
			       unsigned int block_group,
			       unsigned long bitmap_nr)
{
	unsigned long group_desc;
	unsigned long desc;
	struct ext2_group_desc * gdp;
	struct buffer_head * bh;
	
	group_desc = block_group / EXT2_DESC_PER_BLOCK(sb);
	desc = block_group % EXT2_DESC_PER_BLOCK(sb);
	if (!sb->u.ext2_sb.s_group_desc[group_desc])
		ext2_panic (sb, "read_block_bitmap",
			    "Group descriptor not loaded\n"
			    "block_group = %d, group_desc = %lu, desc = %lu",
			     block_group, group_desc, desc);
	gdp = (struct ext2_group_desc *) 
		sb->u.ext2_sb.s_group_desc[group_desc]->b_data;
	bh = bread (sb->s_dev, gdp[desc].bg_block_bitmap, sb->s_blocksize);
	if (!bh)
		ext2_panic (sb, "read_block_bitmap",
			    "Cannot read block bitmap\n"
			    "block_group = %d, group_desc = %lu,"
			    "desc = %lu, block_bitmap = %lu",
			    block_group, group_desc, desc,
			    gdp[desc].bg_block_bitmap);
	sb->u.ext2_sb.s_block_bitmap_number[bitmap_nr] = block_group;
	sb->u.ext2_sb.s_block_bitmap[bitmap_nr] = bh;
}

/*
 * load_block_bitmap loads the block bitmap for a blocks group
 *
 * It maintains a cache for the last bitmaps loaded.  This cache is managed
 * with a LRU algorithm.
 *
 * Notes:
 * 1/ There is one cache per mounted file system.
 * 2/ If the file system contains less than EXT2_MAX_GROUP_LOADED groups,
 *    this function reads the bitmap without maintaining a LRU cache.
 */
static int load__block_bitmap (struct super_block * sb,
			       unsigned int block_group)
{
	int i, j;
	unsigned long block_bitmap_number;
	struct buffer_head * block_bitmap;

	if (block_group >= sb->u.ext2_sb.s_groups_count)
		ext2_panic (sb, "load_block_bitmap",
			    "block_group >= groups_count\n"
			    "block_group = %d, groups_count = %lu",
			    block_group, sb->u.ext2_sb.s_groups_count);

	if (sb->u.ext2_sb.s_groups_count <= EXT2_MAX_GROUP_LOADED) {
		if (sb->u.ext2_sb.s_block_bitmap[block_group]) {
			if (sb->u.ext2_sb.s_block_bitmap_number[block_group] !=
			    block_group)
				ext2_panic (sb, "load_block_bitmap",
					    "block_group != block_bitmap_number");
			else
				return block_group;
		} else {
			read_block_bitmap (sb, block_group, block_group);
			return block_group;
		}
	}

	for (i = 0; i < sb->u.ext2_sb.s_loaded_block_bitmaps &&
		    sb->u.ext2_sb.s_block_bitmap_number[i] != block_group; i++)
		;
	if (i < sb->u.ext2_sb.s_loaded_block_bitmaps &&
  	    sb->u.ext2_sb.s_block_bitmap_number[i] == block_group) {
		block_bitmap_number = sb->u.ext2_sb.s_block_bitmap_number[i];
		block_bitmap = sb->u.ext2_sb.s_block_bitmap[i];
		for (j = i; j > 0; j--) {
			sb->u.ext2_sb.s_block_bitmap_number[j] =
				sb->u.ext2_sb.s_block_bitmap_number[j - 1];
			sb->u.ext2_sb.s_block_bitmap[j] =
				sb->u.ext2_sb.s_block_bitmap[j - 1];
		}
		sb->u.ext2_sb.s_block_bitmap_number[0] = block_bitmap_number;
		sb->u.ext2_sb.s_block_bitmap[0] = block_bitmap;
	} else {
		if (sb->u.ext2_sb.s_loaded_block_bitmaps < 
		    EXT2_MAX_GROUP_LOADED)
			sb->u.ext2_sb.s_loaded_block_bitmaps++;
		else
			brelse (sb->u.ext2_sb.s_block_bitmap
				[EXT2_MAX_GROUP_LOADED - 1]);
		for (j = sb->u.ext2_sb.s_loaded_block_bitmaps - 1; j > 0;  j--) {
			sb->u.ext2_sb.s_block_bitmap_number[j] =
				sb->u.ext2_sb.s_block_bitmap_number[j - 1];
			sb->u.ext2_sb.s_block_bitmap[j] =
				sb->u.ext2_sb.s_block_bitmap[j - 1];
		}
		read_block_bitmap (sb, block_group, 0);
	}
	return 0;
}

static inline int load_block_bitmap (struct super_block * sb,
				     unsigned int block_group)
{
	if (sb->u.ext2_sb.s_loaded_block_bitmaps > 0 &&
	    sb->u.ext2_sb.s_block_bitmap_number[0] == block_group)
		return 0;
	
	if (sb->u.ext2_sb.s_groups_count <= EXT2_MAX_GROUP_LOADED && 
	    sb->u.ext2_sb.s_block_bitmap_number[block_group] == block_group &&
	    sb->u.ext2_sb.s_block_bitmap[block_group]) 
		return block_group;

	return load__block_bitmap (sb, block_group);
}

void ext2_free_block (struct super_block * sb, unsigned long block)
{
	struct buffer_head * bh;
	struct buffer_head * bh2;
	unsigned long block_group;
	unsigned long bit;
	unsigned long group_desc;
	unsigned long desc;
	int bitmap_nr;
	struct ext2_group_desc * gdp;
	struct ext2_super_block * es;

	if (!sb) {
		printk ("ext2_free_block: nonexistent device");
		return;
	}
	lock_super (sb);
	es = sb->u.ext2_sb.s_es;
	if (block < es->s_first_data_block || block >= es->s_blocks_count) {
		ext2_error (sb, "ext2_free_block", "block not in datazone");
		unlock_super (sb);
		return;
	}

	ext2_debug ("freeing block %lu\n", block);

#if 0	/* XXX - This is incompatible with the secure rm implemented in 0.4 */
	bh = get_hash_table (sb->s_dev, block, sb->s_blocksize);
	if (bh)
		bh->b_dirt = 0;
	brelse (bh);
#endif
	block_group = (block - es->s_first_data_block) /
		      EXT2_BLOCKS_PER_GROUP(sb);
	bit = (block - es->s_first_data_block) % EXT2_BLOCKS_PER_GROUP(sb);
	bitmap_nr = load_block_bitmap (sb, block_group);
	bh = sb->u.ext2_sb.s_block_bitmap[bitmap_nr];
	if (!bh)
		ext2_panic (sb, "ext2_free_block",
			    "Unable to load group bitmap\n"
			    "block_group = %lu", block_group);
	if (!clear_bit (bit, bh->b_data))
		ext2_warning (sb, "ext2_free_block",
			      "bit already cleared for block %lu", block);
	else {
		group_desc = block_group / EXT2_DESC_PER_BLOCK(sb);
		desc = block_group % EXT2_DESC_PER_BLOCK(sb);
		bh2 = sb->u.ext2_sb.s_group_desc[group_desc];
		if (!bh2)
			ext2_panic (sb, "ext2_free_block",
				    "Group descriptor not loaded\n"
				    "group_desc = %lu", group_desc);
		gdp = (struct ext2_group_desc *) bh2->b_data;
		gdp[desc].bg_free_blocks_count++;
		bh2->b_dirt = 1;
		es->s_free_blocks_count++;
		sb->u.ext2_sb.s_sbh->b_dirt = 1;
	}
	bh->b_dirt = 1;
	if (sb->s_flags & MS_SYNC) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	sb->s_dirt = 1;
	unlock_super (sb);
	return;
}

/*
 * ext2_new_block uses a goal block to assist allocation.  If the goal is
 * free, or there is a free block within 32 blocks of the goal, that block
 * is allocated.  Otherwise a forward search is made for a free block; within 
 * each block group the search first looks for an entire free byte in the block
 * bitmap, and then for any free bit if that fails.
 */
int ext2_new_block (struct super_block * sb, unsigned long goal)
{
	struct buffer_head * bh;
	char *p, *r;
	int i, j, k;
	unsigned long lmap;
	unsigned long group_desc;
	unsigned long desc;
	int bitmap_nr;
	struct ext2_group_desc * gdp;
	struct ext2_super_block * es;

#ifdef EXT2FS_DEBUG
	static int goal_hits = 0, goal_attempts = 0;
#endif
	if (!sb) {
		printk ("ext2_new_block: nonexistent device");
		return 0;
	}
	lock_super (sb);
	es = sb->u.ext2_sb.s_es;
	if (es->s_free_blocks_count <= es->s_r_blocks_count && !suser()) {
		unlock_super (sb);
		return 0;
	}

	ext2_debug ("goal=%lu.\n", goal);
	
repeat:
	/* First, test whether the goal block is free. */
	i = ((goal - es->s_first_data_block) / EXT2_BLOCKS_PER_GROUP(sb));
	group_desc = i / EXT2_DESC_PER_BLOCK(sb);
	desc = i % EXT2_DESC_PER_BLOCK(sb);
	gdp = (struct ext2_group_desc *) 
		sb->u.ext2_sb.s_group_desc[group_desc]->b_data;
	if (!gdp) {
		ext2_panic (sb, "ext2_new_block",
			    "Descriptor not loaded for group %d", i);
	}
	if (gdp[desc].bg_free_blocks_count > 0) {
		j = ((goal - es->s_first_data_block) %
		       EXT2_BLOCKS_PER_GROUP(sb));
#ifdef EXT2FS_DEBUG
		if (j)
			goal_attempts++;
#endif
		bitmap_nr = load_block_bitmap (sb, i);
		bh = sb->u.ext2_sb.s_block_bitmap[bitmap_nr];
		if (!bh) {
			ext2_panic (sb, "ext2_new_block",
				    "Cannot load bitmap %d", bitmap_nr);
			unlock_super (sb);
			return 0;
		}

		ext2_debug ("goal is at %d[%lu,%lu]:%d.\n", i, group_desc,
			     desc, j);

		if (!test_bit(j, bh->b_data)) {
#ifdef EXT2FS_DEBUG
			goal_hits++;
			ext2_debug ("goal bit allocated.\n");
#endif
			goto got_block;
		}
		if (j) {
			/* The goal was occupied; search forward for a free 
			   block within the next 32 blocks */
			lmap = ((((unsigned long *) bh->b_data)[j >> 5]) >>
				((j & 31) + 1));
			if (j < EXT2_BLOCKS_PER_GROUP(sb) - 32)
				lmap |= (((unsigned long *) bh->b_data)[(j >> 5) + 1]) <<
				 (31 - (j & 31));
			else
				lmap |= 0xffffffff << (31 - (j & 31));
			if (lmap != 0xffffffffl) {
				__asm__ ("bsfl %1,%0"
					 : "=r" (k)
					 : "r" (~lmap));
				k++;
				if ((j + k) < EXT2_BLOCKS_PER_GROUP(sb)) {
					j += k;
					goto got_block;
				}
			}
		}
	
		ext2_debug ("Bit not found near goal\n");

		/* There has been no free block found in the near vicinity
		   of the goal: do a search forward through the block groups,
		   searching in each group first for an entire free byte in
		   the bitmap and then for any free bit.
		   
		   Search first in the remainder of the current group; then,
		   cyclicly search throught the rest of the groups. */
		p = ((char *) bh->b_data) + (j >> 3);
		r = find_first_zero_byte (p, 
					  (EXT2_BLOCKS_PER_GROUP(sb) - j + 7) >> 3);
		k = (r - ((char *) bh->b_data)) << 3;
		if (k < EXT2_BLOCKS_PER_GROUP(sb)) {
			j = k;
			goto got_block;
		}
		k = find_next_zero_bit ((unsigned long *) bh->b_data, 
					EXT2_BLOCKS_PER_GROUP(sb),
					j);
		if (k < EXT2_BLOCKS_PER_GROUP(sb)) {
			j = k;
			goto got_block;
		}
	}

	ext2_debug ("Bit not found in block group %d.\n", i);

	/* Now search the rest of the groups.  We assume that group_desc, desc,
	   i and gdp correctly point to the last group visited. */
	for (k = 0; k < sb->u.ext2_sb.s_groups_count; k++) {
		i++;
		if (i >= sb->u.ext2_sb.s_groups_count) {
			i = 0;
			group_desc = 0;
			desc = 0;
			gdp = (struct ext2_group_desc *) 
				sb->u.ext2_sb.s_group_desc[group_desc]->b_data;
		}
		else {
			desc++;
			if (desc >= EXT2_DESC_PER_BLOCK(sb)) {
				group_desc++;
				desc = 0;
				gdp = (struct ext2_group_desc *) 
					sb->u.ext2_sb.s_group_desc[group_desc]
					->b_data;
			}
		}
		if (!gdp) {
			ext2_panic (sb, "ext2_new_block",
				    "Descriptor not loaded for group %d", i);
		}
		if (gdp[desc].bg_free_blocks_count > 0)
			break;
	}
	if (k >= sb->u.ext2_sb.s_groups_count) {
		unlock_super (sb);
		return 0;
	}
	bitmap_nr = load_block_bitmap (sb, i);
	bh = sb->u.ext2_sb.s_block_bitmap[bitmap_nr];
	if (!bh)
		ext2_panic (sb, "ext2_new_block",
			    "Unable to load bitmap for group %d", i);
	r = find_first_zero_byte (bh->b_data, 
				  EXT2_BLOCKS_PER_GROUP(sb) >> 3);
	j = (r - bh->b_data) << 3;
	if (j >= EXT2_BLOCKS_PER_GROUP(sb))
		j = find_first_zero_bit ((unsigned long *) bh->b_data,
					 EXT2_BLOCKS_PER_GROUP(sb));
	if (j >= EXT2_BLOCKS_PER_GROUP(sb)) {
		ext2_error (sb, "ext2_new_block",
			    "Unable to locate free bit in block group %d", i);
		unlock_super (sb);
		return 0;
	}
		
got_block:

	ext2_debug ("using block group %d(%lu,%lu,%d)\n", 
		    i, group_desc, desc, gdp[desc].bg_free_blocks_count);

	if (set_bit (j, bh->b_data)) {
		ext2_warning (sb, "ext2_new_block",
			      "bit already set for block %d", j);
		goto repeat;
	}
	bh->b_dirt = 1;
	if (sb->s_flags & MS_SYNC) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}

	ext2_debug ("found bit %d\n", j);

	j += i * EXT2_BLOCKS_PER_GROUP(sb) + es->s_first_data_block;
	if (j >= es->s_blocks_count) {
		ext2_error (sb, "ext2_new_block",
			    "block >= blocks count\n"
			    "block_group = %d, block=%d", i, j);
		unlock_super (sb);
		return 0;
	}
	if (!(bh = getblk (sb->s_dev, j, sb->s_blocksize))) {
		ext2_error (sb, "ext2_new_block", "cannot get block %d", j);
		unlock_super (sb);
		return 0;
	}
	clear_block (bh->b_data, sb->s_blocksize);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse (bh);

	ext2_debug ("allocating block %d. "
		    "Goal hits %d of %d.\n", j, goal_hits, goal_attempts);

	gdp[desc].bg_free_blocks_count--;
	sb->u.ext2_sb.s_group_desc[group_desc]->b_dirt = 1;
	es->s_free_blocks_count--;
	sb->u.ext2_sb.s_sbh->b_dirt = 1;
	sb->s_dirt = 1;
	unlock_super (sb);
	return j;
}

unsigned long ext2_count_free_blocks (struct super_block * sb)
{
#ifdef EXT2FS_DEBUG
	struct ext2_super_block * es;
	unsigned long desc_count, bitmap_count, x;
	unsigned long group_desc;
	unsigned long desc;
	int bitmap_nr;
	struct ext2_group_desc * gdp;
	int i;
	
	lock_super (sb);
	es = sb->u.ext2_sb.s_es;
	desc_count = 0;
	bitmap_count = 0;
	group_desc = 0;
	desc = 0;
	gdp = NULL;
	for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
		if (!gdp) {
			if (!sb->u.ext2_sb.s_group_desc[group_desc]) {
				printk ("ext2_count_free_block: "
					"Descriptor not loaded\n");
				break;
			}
			gdp = (struct ext2_group_desc *) 
				sb->u.ext2_sb.s_group_desc[group_desc]->b_data;
		}
		desc_count += gdp[desc].bg_free_blocks_count;
		bitmap_nr = load_block_bitmap (sb, i);
		if (sb->u.ext2_sb.s_block_bitmap[bitmap_nr])
			x = ext2_count_free 
				(sb->u.ext2_sb.s_block_bitmap[bitmap_nr],
				 sb->s_blocksize);
		else {
			x = 0;
			printk ("Cannot load bitmap for group %d\n", i);
		}
		printk ("group %d: stored = %d, counted = %lu\n",
			i, gdp[desc].bg_free_blocks_count, x);
		bitmap_count += x;
		desc++;
		if (desc == EXT2_DESC_PER_BLOCK(sb)) {
			group_desc++;
			desc = 0;
			gdp = NULL;
		}
	}
	printk("ext2_count_free_blocks: stored = %lu, computed = %lu, %lu\n",
	       es->s_free_blocks_count, desc_count, bitmap_count);
	unlock_super (sb);
	return bitmap_count;
#else
	return sb->u.ext2_sb.s_es->s_free_blocks_count;
#endif
}

void ext2_check_blocks_bitmap (struct super_block * sb)
{
	struct ext2_super_block * es;
	unsigned long desc_count, bitmap_count, x;
	unsigned long group_desc;
	unsigned long desc;
	int bitmap_nr;
	struct ext2_group_desc * gdp;
	int i;
	
	lock_super (sb);
	es = sb->u.ext2_sb.s_es;
	desc_count = 0;
	bitmap_count = 0;
	group_desc = 0;
	desc = 0;
	gdp = NULL;
	for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
		if (!gdp) {
			if (!sb->u.ext2_sb.s_group_desc[group_desc]) {
				ext2_error (sb, "ext2_check_blocks_bitmap",
					    "Descriptor not loaded for group %d",
					    i);
				break;
			}
			gdp = (struct ext2_group_desc *) 
				sb->u.ext2_sb.s_group_desc[group_desc]->b_data;
		}
		desc_count += gdp[desc].bg_free_blocks_count;
		bitmap_nr = load_block_bitmap (sb, i);
		if (sb->u.ext2_sb.s_block_bitmap[bitmap_nr])
			x = ext2_count_free 
				(sb->u.ext2_sb.s_block_bitmap[bitmap_nr],
				 sb->s_blocksize);
		else {
			x = 0;
			ext2_error (sb, "ext2_check_blocks_bitmap",
				    "Cannot load bitmap for group %d\n", i);
		}
		if (gdp[desc].bg_free_blocks_count != x)
			ext2_error (sb, "ext2_check_blocks_bitmap",
				    "Wrong free blocks count for group %d, "
				    "stored = %d, counted = %lu", i,
				    gdp[desc].bg_free_blocks_count, x);
		bitmap_count += x;
		desc++;
		if (desc == EXT2_DESC_PER_BLOCK(sb)) {
			group_desc++;
			desc = 0;
			gdp = NULL;
		}
	}
	if (es->s_free_blocks_count != bitmap_count)
		ext2_error (sb, "ext2_check_blocks_bitmap",
			    "Wrong free blocks count in super block, "
			    "stored = %lu, counted = %lu",
			    es->s_free_blocks_count, bitmap_count);
	unlock_super (sb);
}
