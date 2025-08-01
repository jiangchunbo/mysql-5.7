/*****************************************************************************

Copyright (c) 1994, 2023, Oracle and/or its affiliates.
Copyright (c) 2012, Facebook Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file page/page0page.cc
Index page routines

Created 2/2/1994 Heikki Tuuri
*******************************************************/

//#include <thread>

#include "page0page.h"
#ifdef UNIV_NONINL
#include "page0page.ic"
#endif

#include "page0cur.h"
#include "page0zip.h"
#include "buf0buf.h"
#include "btr0btr.h"
#include "row0trunc.h"
#ifndef UNIV_HOTBACKUP
# include "srv0srv.h"
# include "lock0lock.h"
# include "fut0lst.h"
# include "btr0sea.h"
#endif /* !UNIV_HOTBACKUP */

/*			THE INDEX PAGE
			==============

The index page consists of a page header which contains the page's
id and other information. On top of it are the index records
in a heap linked into a one way linear list according to alphabetic order.

Just below page end is an array of pointers which we call page directory,
to about every sixth record in the list. The pointers are placed in
the directory in the alphabetical order of the records pointed to,
enabling us to make binary search using the array. Each slot n:o I
in the directory points to a record, where a 4-bit field contains a count
of those records which are in the linear list between pointer I and
the pointer I - 1 in the directory, including the record
pointed to by pointer I and not including the record pointed to by I - 1.
We say that the record pointed to by slot I, or that slot I, owns
these records. The count is always kept in the range 4 to 8, with
the exception that it is 1 for the first slot, and 1--8 for the second slot.

An essentially binary search can be performed in the list of index
records, like we could do if we had pointer to every record in the
page directory. The data structure is, however, more efficient when
we are doing inserts, because most inserts are just pushed on a heap.
Only every 8th insert requires block move in the directory pointer
table, which itself is quite small. A record is deleted from the page
by just taking it off the linear list and updating the number of owned
records-field of the record which owns it, and updating the page directory,
if necessary. A special case is the one when the record owns itself.
Because the overhead of inserts is so small, we may also increase the
page size from the projected default of 8 kB to 64 kB without too
much loss of efficiency in inserts. Bigger page becomes actual
when the disk transfer rate compared to seek and latency time rises.
On the present system, the page size is set so that the page transfer
time (3 ms) is 20 % of the disk random access time (15 ms).

When the page is split, merged, or becomes full but contains deleted
records, we have to reorganize the page.

Assuming a page size of 8 kB, a typical index page of a secondary
index contains 300 index entries, and the size of the page directory
is 50 x 4 bytes = 200 bytes. */

/***************************************************************//**
Looks for the directory slot which owns the given record.
@return the directory slot number */
ulint
page_dir_find_owner_slot(
/*=====================*/
	const rec_t*	rec)	/*!< in: the physical record */
{
	const page_t*			page;
	register uint16			rec_offs_bytes;
	register const page_dir_slot_t*	slot;
	register const page_dir_slot_t*	first_slot;
	register const rec_t*		r = rec;

	ut_ad(page_rec_check(rec));

	page = page_align(rec);
	first_slot = page_dir_get_nth_slot(page, 0);
	slot = page_dir_get_nth_slot(page, page_dir_get_n_slots(page) - 1);

	if (page_is_comp(page)) {
		while (rec_get_n_owned_new(r) == 0) {
			r = rec_get_next_ptr_const(r, TRUE);
			ut_ad(r >= page + PAGE_NEW_SUPREMUM);
			ut_ad(r < page + (UNIV_PAGE_SIZE - PAGE_DIR));
		}
	} else {
		while (rec_get_n_owned_old(r) == 0) {
			r = rec_get_next_ptr_const(r, FALSE);
			ut_ad(r >= page + PAGE_OLD_SUPREMUM);
			ut_ad(r < page + (UNIV_PAGE_SIZE - PAGE_DIR));
		}
	}

	rec_offs_bytes = mach_encode_2(r - page);

	while (UNIV_LIKELY(*(uint16*) slot != rec_offs_bytes)) {

		if (UNIV_UNLIKELY(slot == first_slot)) {
			ib::error() << "Probable data corruption on page "
				<< page_get_page_no(page)
				<< ". Original record on that page;";

			if (page_is_comp(page)) {
				fputs("(compact record)", stderr);
			} else {
				rec_print_old(stderr, rec);
			}

			ib::error() << "Cannot find the dir slot for this"
				" record on that page;";

			if (page_is_comp(page)) {
				fputs("(compact record)", stderr);
			} else {
				rec_print_old(stderr, page
					      + mach_decode_2(rec_offs_bytes));
			}

			ut_error;
		}

		slot += PAGE_DIR_SLOT_SIZE;
	}

	return(((ulint) (first_slot - slot)) / PAGE_DIR_SLOT_SIZE);
}

/**************************************************************//**
Used to check the consistency of a directory slot.
@return TRUE if succeed */
static
ibool
page_dir_slot_check(
/*================*/
	const page_dir_slot_t*	slot)	/*!< in: slot */
{
	const page_t*	page;
	ulint		n_slots;
	ulint		n_owned;

	ut_a(slot);

	page = page_align(slot);

	n_slots = page_dir_get_n_slots(page);

	ut_a(slot <= page_dir_get_nth_slot(page, 0));
	ut_a(slot >= page_dir_get_nth_slot(page, n_slots - 1));

	ut_a(page_rec_check(page_dir_slot_get_rec(slot)));

	if (page_is_comp(page)) {
		n_owned = rec_get_n_owned_new(page_dir_slot_get_rec(slot));
	} else {
		n_owned = rec_get_n_owned_old(page_dir_slot_get_rec(slot));
	}

	if (slot == page_dir_get_nth_slot(page, 0)) {
		ut_a(n_owned == 1);
	} else if (slot == page_dir_get_nth_slot(page, n_slots - 1)) {
		ut_a(n_owned >= 1);
		ut_a(n_owned <= PAGE_DIR_SLOT_MAX_N_OWNED);
	} else {
		ut_a(n_owned >= PAGE_DIR_SLOT_MIN_N_OWNED);
		ut_a(n_owned <= PAGE_DIR_SLOT_MAX_N_OWNED);
	}

	return(TRUE);
}

/*************************************************************//**
Sets the max trx id field value. */
void
page_set_max_trx_id(
/*================*/
	buf_block_t*	block,	/*!< in/out: page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page, or NULL */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction, or NULL */
{
	page_t*		page		= buf_block_get_frame(block);
#ifndef UNIV_HOTBACKUP
	ut_ad(!mtr || mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
#endif /* !UNIV_HOTBACKUP */

	/* It is not necessary to write this change to the redo log, as
	during a database recovery we assume that the max trx id of every
	page is the maximum trx id assigned before the crash. */

	if (page_zip) {
		mach_write_to_8(page + (PAGE_HEADER + PAGE_MAX_TRX_ID), trx_id);
		page_zip_write_header(page_zip,
				      page + (PAGE_HEADER + PAGE_MAX_TRX_ID),
				      8, mtr);
#ifndef UNIV_HOTBACKUP
	} else if (mtr) {
		mlog_write_ull(page + (PAGE_HEADER + PAGE_MAX_TRX_ID),
			       trx_id, mtr);
#endif /* !UNIV_HOTBACKUP */
	} else {
		mach_write_to_8(page + (PAGE_HEADER + PAGE_MAX_TRX_ID), trx_id);
	}
}

/************************************************************//**
Allocates a block of memory from the heap of an index page.
@return pointer to start of allocated buffer, or NULL if allocation fails */
byte*
page_mem_alloc_heap(
/*================*/
	page_t*		page,	/*!< in/out: index page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page with enough
				space available for inserting the record,
				or NULL */
	ulint		need,	/*!< in: total number of bytes needed */
	ulint*		heap_no)/*!< out: this contains the heap number
				of the allocated record
				if allocation succeeds */
{
	byte*	block;
	ulint	avl_space;

	ut_ad(page && heap_no);

    // 可用空间
	avl_space = page_get_max_insert_size(page, 1);

	if (avl_space >= need) {

                // 先获取当前PAGE_HEAP_TOP位置的地址
		block = page_header_get_ptr(page, PAGE_HEAP_TOP);

                // 然后增加need个字节，并修改PAGE_HEAP_TOP的值
		page_header_set_ptr(page, page_zip, PAGE_HEAP_TOP,
				    block + need);
		*heap_no = page_dir_get_n_heap(page);

		page_dir_set_n_heap(page, page_zip, 1 + *heap_no);

		return(block);
	}

	return(NULL);
}

#ifndef UNIV_HOTBACKUP
/**********************************************************//**
Writes a log record of page creation. */
UNIV_INLINE
void
page_create_write_log(
/*==================*/
	buf_frame_t*	frame,	/*!< in: a buffer frame where the page is
				created */
	mtr_t*		mtr,	/*!< in: mini-transaction handle */
	ibool		comp,	/*!< in: TRUE=compact page format */
	bool		is_rtree) /*!< in: whether it is R-tree */
{
	mlog_id_t	type;

	if (is_rtree) {
		type = comp ? MLOG_COMP_PAGE_CREATE_RTREE
			    : MLOG_PAGE_CREATE_RTREE;
	} else {
		type = comp ? MLOG_COMP_PAGE_CREATE : MLOG_PAGE_CREATE;
	}

	mlog_write_initial_log_record(frame, type, mtr);
}
#else /* !UNIV_HOTBACKUP */
# define page_create_write_log(frame,mtr,comp,is_rtree) ((void) 0)
#endif /* !UNIV_HOTBACKUP */

/** The page infimum and supremum of an empty page in ROW_FORMAT=REDUNDANT */
static const byte infimum_supremum_redundant[] = {
	/* the infimum record */
	0x08/*end offset*/,
	0x01/*n_owned*/,
	0x00, 0x00/*heap_no=0*/,
	0x03/*n_fields=1, 1-byte offsets*/,
	0x00, 0x74/* pointer to supremum */,
	'i', 'n', 'f', 'i', 'm', 'u', 'm', 0,
	/* the supremum record */
	0x09/*end offset*/,
	0x01/*n_owned*/,
	0x00, 0x08/*heap_no=1*/,
	0x03/*n_fields=1, 1-byte offsets*/,
	0x00, 0x00/* end of record list */,
	's', 'u', 'p', 'r', 'e', 'm', 'u', 'm', 0
};

/** The page infimum and supremum of an empty page in ROW_FORMAT=COMPACT */
static const byte infimum_supremum_compact[] = {
	/* the infimum record */
	0x01/*n_owned=1*/,
	0x00, 0x02/* heap_no=0, REC_STATUS_INFIMUM */,
	0x00, 0x0d/* pointer to supremum */,
	'i', 'n', 'f', 'i', 'm', 'u', 'm', 0,
	/* the supremum record */
	0x01/*n_owned=1*/,
	0x00, 0x0b/* heap_no=1, REC_STATUS_SUPREMUM */,  // 00000000 00001011 13个bit表示heap_no， 3个bit表示记录类型，0表示普通记录，1表示非叶子节点记录，2表示最小记录，3表示最大记录
	0x00, 0x00/* end of record list */,
	's', 'u', 'p', 'r', 'e', 'm', 'u', 'm'
};

/**********************************************************//**
The index page creation function.
@return pointer to the page */
static
page_t*
page_create_low(
/*============*/
	buf_block_t*	block,		/*!< in: a buffer block where the
					page is created */
	ulint		comp,		/*!< in: nonzero=compact page format */
	bool		is_rtree)	/*!< in: if it is an R-Tree page */
{
	page_t*		page;

#if PAGE_BTR_IBUF_FREE_LIST + FLST_BASE_NODE_SIZE > PAGE_DATA
# error "PAGE_BTR_IBUF_FREE_LIST + FLST_BASE_NODE_SIZE > PAGE_DATA"
#endif
#if PAGE_BTR_IBUF_FREE_LIST_NODE + FLST_NODE_SIZE > PAGE_DATA
# error "PAGE_BTR_IBUF_FREE_LIST_NODE + FLST_NODE_SIZE > PAGE_DATA"
#endif

	buf_block_modify_clock_inc(block);

	page = buf_block_get_frame(block);

	if (is_rtree) {
		fil_page_set_type(page, FIL_PAGE_RTREE);
	} else {
		fil_page_set_type(page, FIL_PAGE_INDEX);
	}

	memset(page + PAGE_HEADER, 0, PAGE_HEADER_PRIV_END);
	page[PAGE_HEADER + PAGE_N_DIR_SLOTS + 1] = 2;
	page[PAGE_HEADER + PAGE_DIRECTION + 1] = PAGE_NO_DIRECTION;

	if (comp) {
		page[PAGE_HEADER + PAGE_N_HEAP] = 0x80;/*page_is_comp()*/
		page[PAGE_HEADER + PAGE_N_HEAP + 1] = PAGE_HEAP_NO_USER_LOW;
		page[PAGE_HEADER + PAGE_HEAP_TOP + 1] = PAGE_NEW_SUPREMUM_END;
		memcpy(page + PAGE_DATA, infimum_supremum_compact,
		       sizeof infimum_supremum_compact);
		memset(page
		       + PAGE_NEW_SUPREMUM_END, 0,
		       UNIV_PAGE_SIZE - PAGE_DIR - PAGE_NEW_SUPREMUM_END);
		page[UNIV_PAGE_SIZE - PAGE_DIR - PAGE_DIR_SLOT_SIZE * 2 + 1]
			= PAGE_NEW_SUPREMUM;
		page[UNIV_PAGE_SIZE - PAGE_DIR - PAGE_DIR_SLOT_SIZE + 1]
			= PAGE_NEW_INFIMUM;
	} else {
		page[PAGE_HEADER + PAGE_N_HEAP + 1] = PAGE_HEAP_NO_USER_LOW;
		page[PAGE_HEADER + PAGE_HEAP_TOP + 1] = PAGE_OLD_SUPREMUM_END;
		memcpy(page + PAGE_DATA, infimum_supremum_redundant,
		       sizeof infimum_supremum_redundant);
		memset(page
		       + PAGE_OLD_SUPREMUM_END, 0,
		       UNIV_PAGE_SIZE - PAGE_DIR - PAGE_OLD_SUPREMUM_END);
		page[UNIV_PAGE_SIZE - PAGE_DIR - PAGE_DIR_SLOT_SIZE * 2 + 1]
			= PAGE_OLD_SUPREMUM;
		page[UNIV_PAGE_SIZE - PAGE_DIR - PAGE_DIR_SLOT_SIZE + 1]
			= PAGE_OLD_INFIMUM;
	}

	return(page);
}

/** Parses a redo log record of creating a page.
@param[in,out]	block	buffer block, or NULL
@param[in]	comp	nonzero=compact page format
@param[in]	is_rtree whether it is rtree page */
void
page_parse_create(
	buf_block_t*	block,
	ulint		comp,
	bool		is_rtree)
{
	if (block != NULL) {
		page_create_low(block, comp, is_rtree);
	}
}

/**********************************************************//**
Create an uncompressed B-tree or R-tree index page.
@return pointer to the page */
page_t*
page_create(
/*========*/
	buf_block_t*	block,		/*!< in: a buffer block where the
					page is created */
	mtr_t*		mtr,		/*!< in: mini-transaction handle */
	ulint		comp,		/*!< in: nonzero=compact page format */
	bool		is_rtree)	/*!< in: whether it is a R-Tree page */
{
	ut_ad(mtr->is_named_space(block->page.id.space()));
	page_create_write_log(buf_block_get_frame(block), mtr, comp, is_rtree);

	return(page_create_low(block, comp, is_rtree));
}

/**********************************************************//**
Create a compressed B-tree index page.
@return pointer to the page */
page_t*
page_create_zip(
/*============*/
	buf_block_t*		block,		/*!< in/out: a buffer frame
						where the page is created */
	dict_index_t*		index,		/*!< in: the index of the
						page, or NULL when applying
						TRUNCATE log
						record during recovery */
	ulint			level,		/*!< in: the B-tree level
						of the page */
	trx_id_t		max_trx_id,	/*!< in: PAGE_MAX_TRX_ID */
	const redo_page_compress_t* page_comp_info,
						/*!< in: used for applying
						TRUNCATE log
						record during recovery */
	mtr_t*			mtr)		/*!< in/out: mini-transaction
						handle */
{
	page_t*			page;
	page_zip_des_t*		page_zip = buf_block_get_page_zip(block);
	bool			is_spatial;

	ut_ad(block);
	ut_ad(page_zip);
	ut_ad(index == NULL || dict_table_is_comp(index->table));
	is_spatial = index ? dict_index_is_spatial(index)
			   : page_comp_info->type & DICT_SPATIAL;

	page = page_create_low(block, TRUE, is_spatial);
	mach_write_to_2(PAGE_HEADER + PAGE_LEVEL + page, level);
	mach_write_to_8(PAGE_HEADER + PAGE_MAX_TRX_ID + page, max_trx_id);

	if (truncate_t::s_fix_up_active) {
		/* Compress the index page created when applying
		TRUNCATE log during recovery */
		if (!page_zip_compress(page_zip, page, index, page_zip_level,
				       page_comp_info, NULL)) {
			/* The compression of a newly created
			page should always succeed. */
			ut_error;
		}

	} else if (!page_zip_compress(page_zip, page, index,
				      page_zip_level, NULL, mtr)) {
		/* The compression of a newly created
		page should always succeed. */
		ut_error;
	}

	return(page);
}

/**********************************************************//**
Empty a previously created B-tree index page. */
void
page_create_empty(
/*==============*/
	buf_block_t*	block,	/*!< in/out: B-tree block */
	dict_index_t*	index,	/*!< in: the index of the page */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	trx_id_t	max_trx_id = 0;
	const page_t*	page	= buf_block_get_frame(block);
	page_zip_des_t*	page_zip= buf_block_get_page_zip(block);

	ut_ad(fil_page_index_page_check(page));

	/* Multiple transactions cannot simultaneously operate on the
	same temp-table in parallel.
	max_trx_id is ignored for temp tables because it not required
	for MVCC. */
	if (dict_index_is_sec_or_ibuf(index)
	    && !dict_table_is_temporary(index->table)
	    && page_is_leaf(page)) {
		max_trx_id = page_get_max_trx_id(page);
		ut_ad(max_trx_id);
	}

	if (page_zip) {
		page_create_zip(block, index,
				page_header_get_field(page, PAGE_LEVEL),
				max_trx_id, NULL, mtr);
	} else {
		page_create(block, mtr, page_is_comp(page),
			    dict_index_is_spatial(index));

		if (max_trx_id) {
			page_update_max_trx_id(
				block, page_zip, max_trx_id, mtr);
		}
	}
}

/*************************************************************//**
Differs from page_copy_rec_list_end, because this function does not
touch the lock table and max trx id on page or compress the page.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit(). */
void
page_copy_rec_list_end_no_locks(
/*============================*/
	buf_block_t*	new_block,	/*!< in: index page to copy to */
	buf_block_t*	block,		/*!< in: index page of rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		new_page	= buf_block_get_frame(new_block);
	page_cur_t	cur1;
	rec_t*		cur2;
	mem_heap_t*	heap		= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets		= offsets_;
	rec_offs_init(offsets_);

	page_cur_position(rec, block, &cur1);

	if (page_cur_is_before_first(&cur1)) {

		page_cur_move_to_next(&cur1);
	}

	btr_assert_not_corrupted(new_block, index);
	ut_a(page_is_comp(new_page) == page_rec_is_comp(rec));
	ut_a(mach_read_from_2(new_page + UNIV_PAGE_SIZE - 10) == (ulint)
	     (page_is_comp(new_page) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM));

	cur2 = page_get_infimum_rec(buf_block_get_frame(new_block));

	/* Copy records from the original page to the new page */
        // cur1指向的是临时页中的第一条记录
	while (!page_cur_is_after_last(&cur1)) {
		rec_t*	cur1_rec = page_cur_get_rec(&cur1);
		rec_t*	ins_rec;
		offsets = rec_get_offsets(cur1_rec, index, offsets,
					  ULINT_UNDEFINED, &heap);
                // 把cur1指向的记录插入到cur2的后面
		ins_rec = page_cur_insert_rec_low(cur2, index,
						  cur1_rec, offsets, mtr);
// 测试
//                std::this_thread::sleep_for(std::chrono::seconds(60));

		if (UNIV_UNLIKELY(!ins_rec)) {
			ib::fatal() << "Rec offset " << page_offset(rec)
				<< ", cur1 offset "
				<< page_offset(page_cur_get_rec(&cur1))
				<< ", cur2 offset " << page_offset(cur2);
		}
                // 移动到下一条记录
		page_cur_move_to_next(&cur1);
		cur2 = ins_rec;
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

#ifndef UNIV_HOTBACKUP
/*************************************************************//**
Copies records from page to new_page, from a given record onward,
including that record. Infimum and supremum records are not copied.
The records are copied to the start of the record list on new_page.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return pointer to the original successor of the infimum record on
new_page, or NULL on zip overflow (new_block will be decompressed) */
rec_t*
page_copy_rec_list_end(
/*===================*/
	buf_block_t*	new_block,	/*!< in/out: index page to copy to */
	buf_block_t*	block,		/*!< in: index page containing rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		new_page	= buf_block_get_frame(new_block);
	page_zip_des_t*	new_page_zip	= buf_block_get_page_zip(new_block);
	page_t*		page		= page_align(rec);
	rec_t*		ret		= page_rec_get_next(
		page_get_infimum_rec(new_page));
	ulint		num_moved	= 0;
	rtr_rec_move_t*	rec_move	= NULL;
	mem_heap_t*	heap		= NULL;

#ifdef UNIV_ZIP_DEBUG
	if (new_page_zip) {
		page_zip_des_t*	page_zip = buf_block_get_page_zip(block);
		ut_a(page_zip);

		/* Strict page_zip_validate() may fail here.
		Furthermore, btr_compress() may set FIL_PAGE_PREV to
		FIL_NULL on new_page while leaving it intact on
		new_page_zip.  So, we cannot validate new_page_zip. */
		ut_a(page_zip_validate_low(page_zip, page, index, TRUE));
	}
#endif /* UNIV_ZIP_DEBUG */
	ut_ad(buf_block_get_frame(block) == page);
	ut_ad(page_is_leaf(page) == page_is_leaf(new_page));
	ut_ad(page_is_comp(page) == page_is_comp(new_page));
	/* Here, "ret" may be pointing to a user record or the
	predefined supremum record. */

	mtr_log_t	log_mode = MTR_LOG_NONE;

	if (new_page_zip) {
		log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);
	}

	if (page_dir_get_n_heap(new_page) == PAGE_HEAP_NO_USER_LOW) {
		page_copy_rec_list_end_to_created_page(new_page, rec,
						       index, mtr);
	} else {
		if (dict_index_is_spatial(index)) {
			ulint	max_to_move = page_get_n_recs(
						buf_block_get_frame(block));
			heap = mem_heap_create(256);

			rec_move = static_cast<rtr_rec_move_t*>(mem_heap_alloc(
					heap,
					sizeof (*rec_move) * max_to_move));

			/* For spatial index, we need to insert recs one by one
			to keep recs ordered. */
			rtr_page_copy_rec_list_end_no_locks(new_block,
							    block, rec, index,
							    heap, rec_move,
							    max_to_move,
							    &num_moved,
							    mtr);
		} else {
			page_copy_rec_list_end_no_locks(new_block, block, rec,
							index, mtr);
		}
	}

	/* Update PAGE_MAX_TRX_ID on the uncompressed page.
	Modifications will be redo logged and copied to the compressed
	page in page_zip_compress() or page_zip_reorganize() below.
	Multiple transactions cannot simultaneously operate on the
	same temp-table in parallel.
	max_trx_id is ignored for temp tables because it not required
	for MVCC. */
	if (dict_index_is_sec_or_ibuf(index)
	    && page_is_leaf(page)
	    && !dict_table_is_temporary(index->table)) {
		page_update_max_trx_id(new_block, NULL,
				       page_get_max_trx_id(page), mtr);
	}

	if (new_page_zip) {
		mtr_set_log_mode(mtr, log_mode);

		if (!page_zip_compress(new_page_zip,
				       new_page,
				       index,
				       page_zip_level,
				       NULL, mtr)) {
			/* Before trying to reorganize the page,
			store the number of preceding records on the page. */
			ulint	ret_pos
				= page_rec_get_n_recs_before(ret);
			/* Before copying, "ret" was the successor of
			the predefined infimum record.  It must still
			have at least one predecessor (the predefined
			infimum record, or a freshly copied record
			that is smaller than "ret"). */
			ut_a(ret_pos > 0);

			if (!page_zip_reorganize(new_block, index, mtr)) {

				if (!page_zip_decompress(new_page_zip,
							 new_page, FALSE)) {
					ut_error;
				}
				ut_ad(page_validate(new_page, index));

				if (heap) {
					mem_heap_free(heap);
				}

				return(NULL);
			} else {
				/* The page was reorganized:
				Seek to ret_pos. */
				ret = new_page + PAGE_NEW_INFIMUM;

				do {
					ret = rec_get_next_ptr(ret, TRUE);
				} while (--ret_pos);
			}
		}
	}

	/* Update the lock table and possible hash index */

	if (dict_index_is_spatial(index) && rec_move) {
		lock_rtr_move_rec_list(new_block, block, rec_move, num_moved);
	} else if (!dict_table_is_locking_disabled(index->table)) {
		lock_move_rec_list_end(new_block, block, rec);
	}

	if (heap) {
		mem_heap_free(heap);
	}

	btr_search_move_or_delete_hash_entries(new_block, block, index);

	return(ret);
}

/*************************************************************//**
Copies records from page to new_page, up to the given record,
NOT including that record. Infimum and supremum records are not copied.
The records are copied to the end of the record list on new_page.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return pointer to the original predecessor of the supremum record on
new_page, or NULL on zip overflow (new_block will be decompressed) */
rec_t*
page_copy_rec_list_start(
/*=====================*/
	buf_block_t*	new_block,	/*!< in/out: index page to copy to */
	buf_block_t*	block,		/*!< in: index page containing rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		new_page	= buf_block_get_frame(new_block);
	page_zip_des_t*	new_page_zip	= buf_block_get_page_zip(new_block);
	page_cur_t	cur1;
	rec_t*		cur2;
	mem_heap_t*	heap		= NULL;
	ulint		num_moved	= 0;
	rtr_rec_move_t*	rec_move	= NULL;
	rec_t*		ret
		= page_rec_get_prev(page_get_supremum_rec(new_page));
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets		= offsets_;
	rec_offs_init(offsets_);

	/* Here, "ret" may be pointing to a user record or the
	predefined infimum record. */

	if (page_rec_is_infimum(rec)) {

		return(ret);
	}

	mtr_log_t	log_mode = MTR_LOG_NONE;

	if (new_page_zip) {
		log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);
	}

	page_cur_set_before_first(block, &cur1);
	page_cur_move_to_next(&cur1);

	cur2 = ret;

	/* Copy records from the original page to the new page */
	if (dict_index_is_spatial(index)) {
		ulint		max_to_move = page_get_n_recs(
						buf_block_get_frame(block));
		heap = mem_heap_create(256);

		rec_move = static_cast<rtr_rec_move_t*>(mem_heap_alloc(
					heap,
					sizeof (*rec_move) * max_to_move));

		/* For spatial index, we need to insert recs one by one
		to keep recs ordered. */
		rtr_page_copy_rec_list_start_no_locks(new_block,
						      block, rec, index, heap,
						      rec_move, max_to_move,
						      &num_moved, mtr);
	} else {

		while (page_cur_get_rec(&cur1) != rec) {
			rec_t*	cur1_rec = page_cur_get_rec(&cur1);
			offsets = rec_get_offsets(cur1_rec, index, offsets,
						  ULINT_UNDEFINED, &heap);
			cur2 = page_cur_insert_rec_low(cur2, index,
						       cur1_rec, offsets, mtr);
			ut_a(cur2);

			page_cur_move_to_next(&cur1);
		}
	}

	/* Update PAGE_MAX_TRX_ID on the uncompressed page.
	Modifications will be redo logged and copied to the compressed
	page in page_zip_compress() or page_zip_reorganize() below.
	Multiple transactions cannot simultaneously operate on the
	same temp-table in parallel.
	max_trx_id is ignored for temp tables because it not required
	for MVCC. */
	if (dict_index_is_sec_or_ibuf(index)
	    && page_is_leaf(page_align(rec))
	    && !dict_table_is_temporary(index->table)) {
		page_update_max_trx_id(new_block, NULL,
				       page_get_max_trx_id(page_align(rec)),
				       mtr);
	}

	if (new_page_zip) {
		mtr_set_log_mode(mtr, log_mode);

		DBUG_EXECUTE_IF("page_copy_rec_list_start_compress_fail",
				goto zip_reorganize;);

		if (!page_zip_compress(new_page_zip, new_page, index,
				       page_zip_level, NULL, mtr)) {
			ulint	ret_pos;
#ifndef NDEBUG
zip_reorganize:
#endif /* NDEBUG */
			/* Before trying to reorganize the page,
			store the number of preceding records on the page. */
			ret_pos = page_rec_get_n_recs_before(ret);
			/* Before copying, "ret" was the predecessor
			of the predefined supremum record.  If it was
			the predefined infimum record, then it would
			still be the infimum, and we would have
			ret_pos == 0. */

			if (UNIV_UNLIKELY
			    (!page_zip_reorganize(new_block, index, mtr))) {

				if (UNIV_UNLIKELY
				    (!page_zip_decompress(new_page_zip,
							  new_page, FALSE))) {
					ut_error;
				}
				ut_ad(page_validate(new_page, index));

				if (UNIV_LIKELY_NULL(heap)) {
					mem_heap_free(heap);
				}

				return(NULL);
			}

			/* The page was reorganized: Seek to ret_pos. */
			ret = page_rec_get_nth(new_page, ret_pos);
		}
	}

	/* Update the lock table and possible hash index */

	if (dict_index_is_spatial(index)) {
		lock_rtr_move_rec_list(new_block, block, rec_move, num_moved);
	} else if (!dict_table_is_locking_disabled(index->table)) {
		lock_move_rec_list_start(new_block, block, rec, ret);
	}

	if (heap) {
		mem_heap_free(heap);
	}

	btr_search_move_or_delete_hash_entries(new_block, block, index);

	return(ret);
}

/**********************************************************//**
Writes a log record of a record list end or start deletion. */
UNIV_INLINE
void
page_delete_rec_list_write_log(
/*===========================*/
	rec_t*		rec,	/*!< in: record on page */
	dict_index_t*	index,	/*!< in: record descriptor */
	mlog_id_t	type,	/*!< in: operation type:
				MLOG_LIST_END_DELETE, ... */
	mtr_t*		mtr)	/*!< in: mtr */
{
	byte*	log_ptr;
	ut_ad(type == MLOG_LIST_END_DELETE
	      || type == MLOG_LIST_START_DELETE
	      || type == MLOG_COMP_LIST_END_DELETE
	      || type == MLOG_COMP_LIST_START_DELETE);

	log_ptr = mlog_open_and_write_index(mtr, rec, index, type, 2);
	if (log_ptr) {
		/* Write the parameter as a 2-byte ulint */
		mach_write_to_2(log_ptr, page_offset(rec));
		mlog_close(mtr, log_ptr + 2);
	}
}
#else /* !UNIV_HOTBACKUP */
# define page_delete_rec_list_write_log(rec,index,type,mtr) ((void) 0)
#endif /* !UNIV_HOTBACKUP */

/**********************************************************//**
Parses a log record of a record list end or start deletion.
@return end of log record or NULL */
byte*
page_parse_delete_rec_list(
/*=======================*/
	mlog_id_t	type,	/*!< in: MLOG_LIST_END_DELETE,
				MLOG_LIST_START_DELETE,
				MLOG_COMP_LIST_END_DELETE or
				MLOG_COMP_LIST_START_DELETE */
	byte*		ptr,	/*!< in: buffer */
	byte*		end_ptr,/*!< in: buffer end */
	buf_block_t*	block,	/*!< in/out: buffer block or NULL */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in: mtr or NULL */
{
	page_t*	page;
	ulint	offset;

	ut_ad(type == MLOG_LIST_END_DELETE
	      || type == MLOG_LIST_START_DELETE
	      || type == MLOG_COMP_LIST_END_DELETE
	      || type == MLOG_COMP_LIST_START_DELETE);

	/* Read the record offset as a 2-byte ulint */

	if (end_ptr < ptr + 2) {

		return(NULL);
	}

	offset = mach_read_from_2(ptr);
	ptr += 2;

	if (!block) {

		return(ptr);
	}

	page = buf_block_get_frame(block);

	ut_ad(!!page_is_comp(page) == dict_table_is_comp(index->table));

	if (type == MLOG_LIST_END_DELETE
	    || type == MLOG_COMP_LIST_END_DELETE) {
		page_delete_rec_list_end(page + offset, block, index,
					 ULINT_UNDEFINED, ULINT_UNDEFINED,
					 mtr);
	} else {
		page_delete_rec_list_start(page + offset, block, index, mtr);
	}

	return(ptr);
}

/*************************************************************//**
Deletes records from a page from a given record onward, including that record.
The infimum and supremum records are not deleted. */
void
page_delete_rec_list_end(
/*=====================*/
	rec_t*		rec,	/*!< in: pointer to record on page */
	buf_block_t*	block,	/*!< in: buffer block of the page */
	dict_index_t*	index,	/*!< in: record descriptor */
	ulint		n_recs,	/*!< in: number of records to delete,
				or ULINT_UNDEFINED if not known */
	ulint		size,	/*!< in: the sum of the sizes of the
				records in the end of the chain to
				delete, or ULINT_UNDEFINED if not known */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_dir_slot_t*slot;
	ulint		slot_index;
	rec_t*		last_rec;
	rec_t*		prev_rec;
	ulint		n_owned;
	page_zip_des_t*	page_zip	= buf_block_get_page_zip(block);
	page_t*		page		= page_align(rec);
	mem_heap_t*	heap		= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(size == ULINT_UNDEFINED || size < UNIV_PAGE_SIZE);
	ut_ad(!page_zip || page_rec_is_comp(rec));
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	if (page_rec_is_supremum(rec)) {
		ut_ad(n_recs == 0 || n_recs == ULINT_UNDEFINED);
		/* Nothing to do, there are no records bigger than the
		page supremum. */
		return;
	}

	if (recv_recovery_is_on()) {
		/* If we are replaying a redo log record, we must
		replay it exactly. Since MySQL 5.6.11, we should be
		generating a redo log record for page creation if
		the page would become empty. Thus, this branch should
		only be executed when applying redo log that was
		generated by an older version of MySQL. */
	} else if (page_rec_is_infimum(rec)
		   || n_recs == page_get_n_recs(page)) {
delete_all:
		/* We are deleting all records. */
		page_create_empty(block, index, mtr);
		return;
	} else if (page_is_comp(page)) {
		if (page_rec_get_next_low(page + PAGE_NEW_INFIMUM, 1) == rec) {
			/* We are deleting everything from the first
			user record onwards. */
			goto delete_all;
		}
	} else {
		if (page_rec_get_next_low(page + PAGE_OLD_INFIMUM, 0) == rec) {
			/* We are deleting everything from the first
			user record onwards. */
			goto delete_all;
		}
	}

	/* Reset the last insert info in the page header and increment
	the modify clock for the frame */

	page_header_set_ptr(page, page_zip, PAGE_LAST_INSERT, NULL);

	/* The page gets invalid for optimistic searches: increment the
	frame modify clock */

	buf_block_modify_clock_inc(block);

	page_delete_rec_list_write_log(rec, index, page_is_comp(page)
				       ? MLOG_COMP_LIST_END_DELETE
				       : MLOG_LIST_END_DELETE, mtr);

	if (page_zip) {
		mtr_log_t	log_mode;

		ut_a(page_is_comp(page));
		/* Individual deletes are not logged */

		log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);

		do {
                        // 一开始cur会执行rec记录，从这条记录开始删，删到最大记录结束
			page_cur_t	cur;
			page_cur_position(rec, block, &cur);

			offsets = rec_get_offsets(rec, index, offsets,
						  ULINT_UNDEFINED, &heap);
			rec = rec_get_next_ptr(rec, TRUE);
#ifdef UNIV_ZIP_DEBUG
			ut_a(page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
			page_cur_delete_rec(&cur, index, offsets, mtr);
		} while (page_offset(rec) != PAGE_NEW_SUPREMUM);

		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}

		/* Restore log mode */

		mtr_set_log_mode(mtr, log_mode);
		return;
	}

	prev_rec = page_rec_get_prev(rec);

	last_rec = page_rec_get_prev(page_get_supremum_rec(page));

	if ((size == ULINT_UNDEFINED) || (n_recs == ULINT_UNDEFINED)) {
		rec_t*		rec2		= rec;
		/* Calculate the sum of sizes and the number of records */
		size = 0;
		n_recs = 0;

		do {
			ulint	s;
			offsets = rec_get_offsets(rec2, index, offsets,
						  ULINT_UNDEFINED, &heap);
			s = rec_offs_size(offsets);
			ut_ad(rec2 - page + s - rec_offs_extra_size(offsets)
			      < UNIV_PAGE_SIZE);
			ut_ad(size + s < UNIV_PAGE_SIZE);
			size += s;
			n_recs++;

			rec2 = page_rec_get_next(rec2);
		} while (!page_rec_is_supremum(rec2));

		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
	}

	ut_ad(size < UNIV_PAGE_SIZE);

	/* Update the page directory; there is no need to balance the number
	of the records owned by the supremum record, as it is allowed to be
	less than PAGE_DIR_SLOT_MIN_N_OWNED */

	if (page_is_comp(page)) {
		rec_t*	rec2	= rec;
		ulint	count	= 0;

		while (rec_get_n_owned_new(rec2) == 0) {
			count++;

			rec2 = rec_get_next_ptr(rec2, TRUE);
		}

		ut_ad(rec_get_n_owned_new(rec2) > count);

		n_owned = rec_get_n_owned_new(rec2) - count;
		slot_index = page_dir_find_owner_slot(rec2);
		ut_ad(slot_index > 0);
		slot = page_dir_get_nth_slot(page, slot_index);
	} else {
		rec_t*	rec2	= rec;
		ulint	count	= 0;

		while (rec_get_n_owned_old(rec2) == 0) {
			count++;

			rec2 = rec_get_next_ptr(rec2, FALSE);
		}

		ut_ad(rec_get_n_owned_old(rec2) > count);

		n_owned = rec_get_n_owned_old(rec2) - count;
		slot_index = page_dir_find_owner_slot(rec2);
		ut_ad(slot_index > 0);
		slot = page_dir_get_nth_slot(page, slot_index);
	}

	page_dir_slot_set_rec(slot, page_get_supremum_rec(page));
	page_dir_slot_set_n_owned(slot, NULL, n_owned);

	page_dir_set_n_slots(page, NULL, slot_index + 1);

	/* Remove the record chain segment from the record chain */
	page_rec_set_next(prev_rec, page_get_supremum_rec(page));

	/* Catenate the deleted chain segment to the page free list */

	page_rec_set_next(last_rec, page_header_get_ptr(page, PAGE_FREE));
	page_header_set_ptr(page, NULL, PAGE_FREE, rec);

	page_header_set_field(page, NULL, PAGE_GARBAGE, size
			      + page_header_get_field(page, PAGE_GARBAGE));

	page_header_set_field(page, NULL, PAGE_N_RECS,
			      (ulint)(page_get_n_recs(page) - n_recs));
}

/*************************************************************//**
Deletes records from page, up to the given record, NOT including
that record. Infimum and supremum records are not deleted. */
void
page_delete_rec_list_start(
/*=======================*/
	rec_t*		rec,	/*!< in: record on page */
	buf_block_t*	block,	/*!< in: buffer block of the page */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_cur_t	cur1;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets		= offsets_;
	mem_heap_t*	heap		= NULL;

	rec_offs_init(offsets_);

	ut_ad((ibool) !!page_rec_is_comp(rec)
	      == dict_table_is_comp(index->table));
#ifdef UNIV_ZIP_DEBUG
	{
		page_zip_des_t*	page_zip= buf_block_get_page_zip(block);
		page_t*		page	= buf_block_get_frame(block);

		/* page_zip_validate() would detect a min_rec_mark mismatch
		in btr_page_split_and_insert()
		between btr_attach_half_pages() and insert_page = ...
		when btr_page_get_split_rec_to_left() holds
		(direction == FSP_DOWN). */
		ut_a(!page_zip
		     || page_zip_validate_low(page_zip, page, index, TRUE));
	}
#endif /* UNIV_ZIP_DEBUG */

	if (page_rec_is_infimum(rec)) {
		return;
	}

	if (page_rec_is_supremum(rec)) {
		/* We are deleting all records. */
		page_create_empty(block, index, mtr);
		return;
	}

	mlog_id_t	type;

	if (page_rec_is_comp(rec)) {
		type = MLOG_COMP_LIST_START_DELETE;
	} else {
		type = MLOG_LIST_START_DELETE;
	}

	page_delete_rec_list_write_log(rec, index, type, mtr);

        // cur1指向当前页中的第一条记录
	page_cur_set_before_first(block, &cur1);
	page_cur_move_to_next(&cur1);

	/* Individual deletes are not logged */

	mtr_log_t	log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);

	while (page_cur_get_rec(&cur1) != rec) {
		offsets = rec_get_offsets(page_cur_get_rec(&cur1), index,
					  offsets, ULINT_UNDEFINED, &heap);
                // 删除cur1执行的记录，并且会移动到下一条记录
		page_cur_delete_rec(&cur1, index, offsets, mtr);
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	/* Restore log mode */

	mtr_set_log_mode(mtr, log_mode);
}

#ifndef UNIV_HOTBACKUP
/*************************************************************//**
Moves record list end to another page. Moved records include
split_rec.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return TRUE on success; FALSE on compression failure (new_block will
be decompressed) */
ibool
page_move_rec_list_end(
/*===================*/
	buf_block_t*	new_block,	/*!< in/out: index page where to move */
	buf_block_t*	block,		/*!< in: index page from where to move */
	rec_t*		split_rec,	/*!< in: first record to move */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		new_page	= buf_block_get_frame(new_block);
	ulint		old_data_size;
	ulint		new_data_size;
	ulint		old_n_recs;
	ulint		new_n_recs;

	ut_ad(!dict_index_is_spatial(index));

	old_data_size = page_get_data_size(new_page);
	old_n_recs = page_get_n_recs(new_page);
#ifdef UNIV_ZIP_DEBUG
	{
		page_zip_des_t*	new_page_zip
			= buf_block_get_page_zip(new_block);
		page_zip_des_t*	page_zip
			= buf_block_get_page_zip(block);
		ut_a(!new_page_zip == !page_zip);
		ut_a(!new_page_zip
		     || page_zip_validate(new_page_zip, new_page, index));
		ut_a(!page_zip
		     || page_zip_validate(page_zip, page_align(split_rec),
					  index));
	}
#endif /* UNIV_ZIP_DEBUG */

	if (UNIV_UNLIKELY(!page_copy_rec_list_end(new_block, block,
						  split_rec, index, mtr))) {
		return(FALSE);
	}

	new_data_size = page_get_data_size(new_page);
	new_n_recs = page_get_n_recs(new_page);

	ut_ad(new_data_size >= old_data_size);

	page_delete_rec_list_end(split_rec, block, index,
				 new_n_recs - old_n_recs,
				 new_data_size - old_data_size, mtr);

	return(TRUE);
}

/*************************************************************//**
Moves record list start to another page. Moved records do not include
split_rec.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return TRUE on success; FALSE on compression failure */
ibool
page_move_rec_list_start(
/*=====================*/
	buf_block_t*	new_block,	/*!< in/out: index page where to move */
	buf_block_t*	block,		/*!< in/out: page containing split_rec */
	rec_t*		split_rec,	/*!< in: first record not to move */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	if (UNIV_UNLIKELY(!page_copy_rec_list_start(new_block, block,
						    split_rec, index, mtr))) {
		return(FALSE);
	}

	page_delete_rec_list_start(split_rec, block, index, mtr);

	return(TRUE);
}
#endif /* !UNIV_HOTBACKUP */

/**************************************************************//**
Used to delete n slots from the directory. This function updates
also n_owned fields in the records, so that the first slot after
the deleted ones inherits the records of the deleted slots. */
UNIV_INLINE
void
page_dir_delete_slot(
/*=================*/
	page_t*		page,	/*!< in/out: the index page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page, or NULL */
	ulint		slot_no)/*!< in: slot to be deleted */
{
	page_dir_slot_t*	slot;
	ulint			n_owned;
	ulint			i;
	ulint			n_slots;

	ut_ad(!page_zip || page_is_comp(page));
	ut_ad(slot_no > 0);
	ut_ad(slot_no + 1 < page_dir_get_n_slots(page));

	n_slots = page_dir_get_n_slots(page);

	/* 1. Reset the n_owned fields of the slots to be
	deleted */
	slot = page_dir_get_nth_slot(page, slot_no);
	n_owned = page_dir_slot_get_n_owned(slot);
	page_dir_slot_set_n_owned(slot, page_zip, 0);

	/* 2. Update the n_owned value of the first non-deleted slot */

	slot = page_dir_get_nth_slot(page, slot_no + 1);
	page_dir_slot_set_n_owned(slot, page_zip,
				  n_owned + page_dir_slot_get_n_owned(slot));

	/* 3. Destroy the slot by copying slots */
	for (i = slot_no + 1; i < n_slots; i++) {
		rec_t*	rec = (rec_t*)
			page_dir_slot_get_rec(page_dir_get_nth_slot(page, i));
		page_dir_slot_set_rec(page_dir_get_nth_slot(page, i - 1), rec);
	}

	/* 4. Zero out the last slot, which will be removed */
	mach_write_to_2(page_dir_get_nth_slot(page, n_slots - 1), 0);

	/* 5. Update the page header */
	page_header_set_field(page, page_zip, PAGE_N_DIR_SLOTS, n_slots - 1);
}

/**************************************************************//**
Used to add n slots to the directory. Does not set the record pointers
in the added slots or update n_owned values: this is the responsibility
of the caller. */
UNIV_INLINE
void
page_dir_add_slot(
/*==============*/
	page_t*		page,	/*!< in/out: the index page */
	page_zip_des_t*	page_zip,/*!< in/out: comprssed page, or NULL */
	ulint		start)	/*!< in: the slot above which the new slots
				are added */
{
	page_dir_slot_t*	slot;
	ulint			n_slots;

	n_slots = page_dir_get_n_slots(page);

	ut_ad(start < n_slots - 1);

	/* Update the page header */
	page_dir_set_n_slots(page, page_zip, n_slots + 1);

	/* Move slots up */
	slot = page_dir_get_nth_slot(page, n_slots);
	memmove(slot, slot + PAGE_DIR_SLOT_SIZE,
		(n_slots - 1 - start) * PAGE_DIR_SLOT_SIZE);
}

/****************************************************************//**
Splits a directory slot which owns too many records. */
void
page_dir_split_slot(
/*================*/
	page_t*		page,	/*!< in/out: index page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page whose
				uncompressed part will be written, or NULL */
	ulint		slot_no)/*!< in: the directory slot */
{
	rec_t*			rec;
	page_dir_slot_t*	new_slot;
	page_dir_slot_t*	prev_slot;
	page_dir_slot_t*	slot;
	ulint			i;
	ulint			n_owned;

	ut_ad(page);
	ut_ad(!page_zip || page_is_comp(page));
	ut_ad(slot_no > 0);

	slot = page_dir_get_nth_slot(page, slot_no);

	n_owned = page_dir_slot_get_n_owned(slot);
	ut_ad(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED + 1);

	/* 1. We loop to find a record approximately in the middle of the
	records owned by the slot. */

	prev_slot = page_dir_get_nth_slot(page, slot_no - 1);
	rec = (rec_t*) page_dir_slot_get_rec(prev_slot);

	for (i = 0; i < n_owned / 2; i++) {
		rec = page_rec_get_next(rec);
	}

	ut_ad(n_owned / 2 >= PAGE_DIR_SLOT_MIN_N_OWNED);

	/* 2. We add one directory slot immediately below the slot to be
	split. */

	page_dir_add_slot(page, page_zip, slot_no - 1);

	/* The added slot is now number slot_no, and the old slot is
	now number slot_no + 1 */

	new_slot = page_dir_get_nth_slot(page, slot_no);
	slot = page_dir_get_nth_slot(page, slot_no + 1);

	/* 3. We store the appropriate values to the new slot. */

	page_dir_slot_set_rec(new_slot, rec);
	page_dir_slot_set_n_owned(new_slot, page_zip, n_owned / 2);

	/* 4. Finally, we update the number of records field of the
	original slot */

	page_dir_slot_set_n_owned(slot, page_zip, n_owned - (n_owned / 2));
}

/*************************************************************//**
Tries to balance the given directory slot with too few records with the upper
neighbor, so that there are at least the minimum number of records owned by
the slot; this may result in the merging of two slots. */
void
page_dir_balance_slot(
/*==================*/
	page_t*		page,	/*!< in/out: index page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page, or NULL */
	ulint		slot_no)/*!< in: the directory slot */
{
	page_dir_slot_t*	slot;
	page_dir_slot_t*	up_slot;
	ulint			n_owned;
	ulint			up_n_owned;
	rec_t*			old_rec;
	rec_t*			new_rec;

	ut_ad(page);
	ut_ad(!page_zip || page_is_comp(page));
	ut_ad(slot_no > 0);

	slot = page_dir_get_nth_slot(page, slot_no);

	/* The last directory slot cannot be balanced with the upper
	neighbor, as there is none. */

	if (UNIV_UNLIKELY(slot_no == page_dir_get_n_slots(page) - 1)) {

		return;
	}

	up_slot = page_dir_get_nth_slot(page, slot_no + 1);

	n_owned = page_dir_slot_get_n_owned(slot);
	up_n_owned = page_dir_slot_get_n_owned(up_slot);

	ut_ad(n_owned == PAGE_DIR_SLOT_MIN_N_OWNED - 1);

	/* If the upper slot has the minimum value of n_owned, we will merge
	the two slots, therefore we assert: */
	ut_ad(2 * PAGE_DIR_SLOT_MIN_N_OWNED - 1 <= PAGE_DIR_SLOT_MAX_N_OWNED);

	if (up_n_owned > PAGE_DIR_SLOT_MIN_N_OWNED) {

		/* In this case we can just transfer one record owned
		by the upper slot to the property of the lower slot */
		old_rec = (rec_t*) page_dir_slot_get_rec(slot);

		if (page_is_comp(page)) {
			new_rec = rec_get_next_ptr(old_rec, TRUE);

			rec_set_n_owned_new(old_rec, page_zip, 0);
			rec_set_n_owned_new(new_rec, page_zip, n_owned + 1);
		} else {
			new_rec = rec_get_next_ptr(old_rec, FALSE);

			rec_set_n_owned_old(old_rec, 0);
			rec_set_n_owned_old(new_rec, n_owned + 1);
		}

		page_dir_slot_set_rec(slot, new_rec);

		page_dir_slot_set_n_owned(up_slot, page_zip, up_n_owned -1);
	} else {
		/* In this case we may merge the two slots */
		page_dir_delete_slot(page, page_zip, slot_no);
	}
}

/************************************************************//**
Returns the nth record of the record list.
This is the inverse function of page_rec_get_n_recs_before().
@return nth record */
const rec_t*
page_rec_get_nth_const(
/*===================*/
	const page_t*	page,	/*!< in: page */
	ulint		nth)	/*!< in: nth record */
{
	const page_dir_slot_t*	slot;
	ulint			i;
	ulint			n_owned;
	const rec_t*		rec;

	if (nth == 0) {
		return(page_get_infimum_rec(page));
	}

	ut_ad(nth < UNIV_PAGE_SIZE / (REC_N_NEW_EXTRA_BYTES + 1));

	for (i = 0;; i++) {

		slot = page_dir_get_nth_slot(page, i);
		n_owned = page_dir_slot_get_n_owned(slot);

		if (n_owned > nth) {
			break;
		} else {
			nth -= n_owned;
		}
	}

	ut_ad(i > 0);
	slot = page_dir_get_nth_slot(page, i - 1);
	rec = page_dir_slot_get_rec(slot);

	if (page_is_comp(page)) {
		do {
			rec = page_rec_get_next_low(rec, TRUE);
			ut_ad(rec);
		} while (nth--);
	} else {
		do {
			rec = page_rec_get_next_low(rec, FALSE);
			ut_ad(rec);
		} while (nth--);
	}

	return(rec);
}

/***************************************************************//**
Returns the number of records before the given record in chain.
The number includes infimum and supremum records.
@return number of records */
ulint
page_rec_get_n_recs_before(
/*=======================*/
	const rec_t*	rec)	/*!< in: the physical record */
{
	const page_dir_slot_t*	slot;
	const rec_t*		slot_rec;
	const page_t*		page;
	ulint			i;
	lint			n	= 0;

	ut_ad(page_rec_check(rec));

	page = page_align(rec);
	if (page_is_comp(page)) {
		while (rec_get_n_owned_new(rec) == 0) {

			rec = rec_get_next_ptr_const(rec, TRUE);
			n--;
		}

		for (i = 0; ; i++) {
			slot = page_dir_get_nth_slot(page, i);
			slot_rec = page_dir_slot_get_rec(slot);

			n += rec_get_n_owned_new(slot_rec);

			if (rec == slot_rec) {

				break;
			}
		}
	} else {
		while (rec_get_n_owned_old(rec) == 0) {

			rec = rec_get_next_ptr_const(rec, FALSE);
			n--;
		}

		for (i = 0; ; i++) {
			slot = page_dir_get_nth_slot(page, i);
			slot_rec = page_dir_slot_get_rec(slot);

			n += rec_get_n_owned_old(slot_rec);

			if (rec == slot_rec) {

				break;
			}
		}
	}

	n--;

	ut_ad(n >= 0);
	ut_ad((ulong) n < UNIV_PAGE_SIZE / (REC_N_NEW_EXTRA_BYTES + 1));

	return((ulint) n);
}

#ifndef UNIV_HOTBACKUP
/************************************************************//**
Prints record contents including the data relevant only in
the index page context. */
void
page_rec_print(
/*===========*/
	const rec_t*	rec,	/*!< in: physical record */
	const ulint*	offsets)/*!< in: record descriptor */
{
	ut_a(!page_rec_is_comp(rec) == !rec_offs_comp(offsets));
	rec_print_new(stderr, rec, offsets);
	if (page_rec_is_comp(rec)) {
		ib::info() << "n_owned: " << rec_get_n_owned_new(rec)
			<< "; heap_no: " << rec_get_heap_no_new(rec)
			<< "; next rec: " << rec_get_next_offs(rec, TRUE);
	} else {
		ib::info() << "n_owned: " << rec_get_n_owned_old(rec)
			<< "; heap_no: " << rec_get_heap_no_old(rec)
			<< "; next rec: " << rec_get_next_offs(rec, FALSE);
	}

	page_rec_check(rec);
	rec_validate(rec, offsets);
}

# ifdef UNIV_BTR_PRINT
/***************************************************************//**
This is used to print the contents of the directory for
debugging purposes. */
void
page_dir_print(
/*===========*/
	page_t*	page,	/*!< in: index page */
	ulint	pr_n)	/*!< in: print n first and n last entries */
{
	ulint			n;
	ulint			i;
	page_dir_slot_t*	slot;

	n = page_dir_get_n_slots(page);

	fprintf(stderr, "--------------------------------\n"
		"PAGE DIRECTORY\n"
		"Page address %p\n"
		"Directory stack top at offs: %lu; number of slots: %lu\n",
		page, (ulong) page_offset(page_dir_get_nth_slot(page, n - 1)),
		(ulong) n);
	for (i = 0; i < n; i++) {
		slot = page_dir_get_nth_slot(page, i);
		if ((i == pr_n) && (i < n - pr_n)) {
			fputs("    ...   \n", stderr);
		}
		if ((i < pr_n) || (i >= n - pr_n)) {
			fprintf(stderr,
				"Contents of slot: %lu: n_owned: %lu,"
				" rec offs: %lu\n",
				(ulong) i,
				(ulong) page_dir_slot_get_n_owned(slot),
				(ulong)
				page_offset(page_dir_slot_get_rec(slot)));
		}
	}
	fprintf(stderr, "Total of %lu records\n"
		"--------------------------------\n",
		(ulong) (PAGE_HEAP_NO_USER_LOW + page_get_n_recs(page)));
}

/***************************************************************//**
This is used to print the contents of the page record list for
debugging purposes. */
void
page_print_list(
/*============*/
	buf_block_t*	block,	/*!< in: index page */
	dict_index_t*	index,	/*!< in: dictionary index of the page */
	ulint		pr_n)	/*!< in: print n first and n last entries */
{
	page_t*		page		= block->frame;
	page_cur_t	cur;
	ulint		count;
	ulint		n_recs;
	mem_heap_t*	heap		= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_a((ibool)!!page_is_comp(page) == dict_table_is_comp(index->table));

	fprint(stderr,
		"--------------------------------\n"
		"PAGE RECORD LIST\n"
		"Page address %p\n", page);

	n_recs = page_get_n_recs(page);

	page_cur_set_before_first(block, &cur);
	count = 0;
	for (;;) {
		offsets = rec_get_offsets(cur.rec, index, offsets,
					  ULINT_UNDEFINED, &heap);
		page_rec_print(cur.rec, offsets);

		if (count == pr_n) {
			break;
		}
		if (page_cur_is_after_last(&cur)) {
			break;
		}
		page_cur_move_to_next(&cur);
		count++;
	}

	if (n_recs > 2 * pr_n) {
		fputs(" ... \n", stderr);
	}

	while (!page_cur_is_after_last(&cur)) {
		page_cur_move_to_next(&cur);

		if (count + pr_n >= n_recs) {
			offsets = rec_get_offsets(cur.rec, index, offsets,
						  ULINT_UNDEFINED, &heap);
			page_rec_print(cur.rec, offsets);
		}
		count++;
	}

	fprintf(stderr,
		"Total of %lu records \n"
		"--------------------------------\n",
		(ulong) (count + 1));

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

/***************************************************************//**
Prints the info in a page header. */
void
page_header_print(
/*==============*/
	const page_t*	page)
{
	fprintf(stderr,
		"--------------------------------\n"
		"PAGE HEADER INFO\n"
		"Page address %p, n records %lu (%s)\n"
		"n dir slots %lu, heap top %lu\n"
		"Page n heap %lu, free %lu, garbage %lu\n"
		"Page last insert %lu, direction %lu, n direction %lu\n",
		page, (ulong) page_header_get_field(page, PAGE_N_RECS),
		page_is_comp(page) ? "compact format" : "original format",
		(ulong) page_header_get_field(page, PAGE_N_DIR_SLOTS),
		(ulong) page_header_get_field(page, PAGE_HEAP_TOP),
		(ulong) page_dir_get_n_heap(page),
		(ulong) page_header_get_field(page, PAGE_FREE),
		(ulong) page_header_get_field(page, PAGE_GARBAGE),
		(ulong) page_header_get_field(page, PAGE_LAST_INSERT),
		(ulong) page_header_get_field(page, PAGE_DIRECTION),
		(ulong) page_header_get_field(page, PAGE_N_DIRECTION));
}

/***************************************************************//**
This is used to print the contents of the page for
debugging purposes. */
void
page_print(
/*=======*/
	buf_block_t*	block,	/*!< in: index page */
	dict_index_t*	index,	/*!< in: dictionary index of the page */
	ulint		dn,	/*!< in: print dn first and last entries
				in directory */
	ulint		rn)	/*!< in: print rn first and last records
				in directory */
{
	page_t*	page = block->frame;

	page_header_print(page);
	page_dir_print(page, dn);
	page_print_list(block, index, rn);
}
# endif /* UNIV_BTR_PRINT */
#endif /* !UNIV_HOTBACKUP */

/***************************************************************//**
The following is used to validate a record on a page. This function
differs from rec_validate as it can also check the n_owned field and
the heap_no field.
@return TRUE if ok */
ibool
page_rec_validate(
/*==============*/
	const rec_t*	rec,	/*!< in: physical record */
	const ulint*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint		n_owned;
	ulint		heap_no;
	const page_t*	page;

	page = page_align(rec);
	ut_a(!page_is_comp(page) == !rec_offs_comp(offsets));

	page_rec_check(rec);
	rec_validate(rec, offsets);

	if (page_rec_is_comp(rec)) {
		n_owned = rec_get_n_owned_new(rec);
		heap_no = rec_get_heap_no_new(rec);
	} else {
		n_owned = rec_get_n_owned_old(rec);
		heap_no = rec_get_heap_no_old(rec);
	}

	if (UNIV_UNLIKELY(!(n_owned <= PAGE_DIR_SLOT_MAX_N_OWNED))) {
		ib::warn() << "Dir slot of rec " << page_offset(rec)
			<< ", n owned too big " << n_owned;
		return(FALSE);
	}

	if (UNIV_UNLIKELY(!(heap_no < page_dir_get_n_heap(page)))) {
		ib::warn() << "Heap no of rec " << page_offset(rec)
			<< " too big " << heap_no << " "
			<< page_dir_get_n_heap(page);
		return(FALSE);
	}

	return(TRUE);
}

#ifndef UNIV_HOTBACKUP
#ifdef UNIV_DEBUG
/***************************************************************//**
Checks that the first directory slot points to the infimum record and
the last to the supremum. This function is intended to track if the
bug fixed in 4.0.14 has caused corruption to users' databases. */
void
page_check_dir(
/*===========*/
	const page_t*	page)	/*!< in: index page */
{
	ulint	n_slots;
	ulint	infimum_offs;
	ulint	supremum_offs;

	n_slots = page_dir_get_n_slots(page);
	infimum_offs = mach_read_from_2(page_dir_get_nth_slot(page, 0));
	supremum_offs = mach_read_from_2(page_dir_get_nth_slot(page,
							       n_slots - 1));

	if (UNIV_UNLIKELY(!page_rec_is_infimum_low(infimum_offs))) {

		ib::fatal() << "Page directory corruption: infimum not"
			" pointed to";
	}

	if (UNIV_UNLIKELY(!page_rec_is_supremum_low(supremum_offs))) {

		ib::fatal() << "Page directory corruption: supremum not"
			" pointed to";
	}
}
#endif /* UNIV_DEBUG */
#endif /* !UNIV_HOTBACKUP */

/***************************************************************//**
This function checks the consistency of an index page when we do not
know the index. This is also resilient so that this should never crash
even if the page is total garbage.
@return TRUE if ok */
ibool
page_simple_validate_old(
/*=====================*/
	const page_t*	page)	/*!< in: index page in ROW_FORMAT=REDUNDANT */
{
	const page_dir_slot_t*	slot;
	ulint			slot_no;
	ulint			n_slots;
	const rec_t*		rec;
	const byte*		rec_heap_top;
	ulint			count;
	ulint			own_count;
	ibool			ret	= FALSE;

	ut_a(!page_is_comp(page));

	/* Check first that the record heap and the directory do not
	overlap. */

	n_slots = page_dir_get_n_slots(page);

	if (UNIV_UNLIKELY(n_slots > UNIV_PAGE_SIZE / 4)) {
		ib::error() << "Nonsensical number " << n_slots
			<< " of page dir slots";

		goto func_exit;
	}

	rec_heap_top = page_header_get_ptr(page, PAGE_HEAP_TOP);

	if (UNIV_UNLIKELY(rec_heap_top
			  > page_dir_get_nth_slot(page, n_slots - 1))) {
		ib::error()
			<< "Record heap and dir overlap on a page, heap top "
			<< page_header_get_field(page, PAGE_HEAP_TOP)
			<< ", dir "
			<< page_offset(page_dir_get_nth_slot(page,
							     n_slots - 1));

		goto func_exit;
	}

	/* Validate the record list in a loop checking also that it is
	consistent with the page record directory. */

	count = 0;
	own_count = 1;
	slot_no = 0;
	slot = page_dir_get_nth_slot(page, slot_no);

	rec = page_get_infimum_rec(page);

	for (;;) {
		if (UNIV_UNLIKELY(rec > rec_heap_top)) {
			ib::error() << "Record " << (rec - page)
				<< " is above rec heap top "
				<< (rec_heap_top - page);

			goto func_exit;
		}

		if (UNIV_UNLIKELY(rec_get_n_owned_old(rec))) {
			/* This is a record pointed to by a dir slot */
			if (UNIV_UNLIKELY(rec_get_n_owned_old(rec)
					  != own_count)) {

				ib::error() << "Wrong owned count "
					<< rec_get_n_owned_old(rec)
					<< ", " << own_count << ", rec "
					<< (rec - page);

				goto func_exit;
			}

			if (UNIV_UNLIKELY
			    (page_dir_slot_get_rec(slot) != rec)) {
				ib::error() << "Dir slot does not point"
					" to right rec " << (rec - page);

				goto func_exit;
			}

			own_count = 0;

			if (!page_rec_is_supremum(rec)) {
				slot_no++;
				slot = page_dir_get_nth_slot(page, slot_no);
			}
		}

		if (page_rec_is_supremum(rec)) {

			break;
		}

		if (UNIV_UNLIKELY
		    (rec_get_next_offs(rec, FALSE) < FIL_PAGE_DATA
		     || rec_get_next_offs(rec, FALSE) >= UNIV_PAGE_SIZE)) {

			ib::error() << "Next record offset nonsensical "
				<< rec_get_next_offs(rec, FALSE) << " for rec "
				<< (rec - page);

			goto func_exit;
		}

		count++;

		if (UNIV_UNLIKELY(count > UNIV_PAGE_SIZE)) {
			ib::error() << "Page record list appears"
				" to be circular " << count;
			goto func_exit;
		}

		rec = page_rec_get_next_const(rec);
		own_count++;
	}

	if (UNIV_UNLIKELY(rec_get_n_owned_old(rec) == 0)) {
		ib::error() << "n owned is zero in a supremum rec";

		goto func_exit;
	}

	if (UNIV_UNLIKELY(slot_no != n_slots - 1)) {
		ib::error() <<  "n slots wrong "
			<< slot_no << ", " << (n_slots - 1);
		goto func_exit;
	}

	if (UNIV_UNLIKELY(page_header_get_field(page, PAGE_N_RECS)
			  + PAGE_HEAP_NO_USER_LOW
			  != count + 1)) {
		ib::error() <<  "n recs wrong "
			<< page_header_get_field(page, PAGE_N_RECS)
			+ PAGE_HEAP_NO_USER_LOW << " " << (count + 1);

		goto func_exit;
	}

	/* Check then the free list */
	rec = page_header_get_ptr(page, PAGE_FREE);

	while (rec != NULL) {
		if (UNIV_UNLIKELY(rec < page + FIL_PAGE_DATA
				  || rec >= page + UNIV_PAGE_SIZE)) {
			ib::error() << "Free list record has"
				" a nonsensical offset " << (rec - page);

			goto func_exit;
		}

		if (UNIV_UNLIKELY(rec > rec_heap_top)) {
			ib::error() << "Free list record " << (rec - page)
				<< " is above rec heap top "
				<< (rec_heap_top - page);

			goto func_exit;
		}

		count++;

		if (UNIV_UNLIKELY(count > UNIV_PAGE_SIZE)) {
			ib::error() << "Page free list appears"
				" to be circular " << count;
			goto func_exit;
		}

		rec = page_rec_get_next_const(rec);
	}

	if (UNIV_UNLIKELY(page_dir_get_n_heap(page) != count + 1)) {

		ib::error() <<  "N heap is wrong "
			<< page_dir_get_n_heap(page) << ", " << (count + 1);

		goto func_exit;
	}

	ret = TRUE;

func_exit:
	return(ret);
}

/***************************************************************//**
This function checks the consistency of an index page when we do not
know the index. This is also resilient so that this should never crash
even if the page is total garbage.
@return TRUE if ok */
ibool
page_simple_validate_new(
/*=====================*/
	const page_t*	page)	/*!< in: index page in ROW_FORMAT!=REDUNDANT */
{
	const page_dir_slot_t*	slot;
	ulint			slot_no;
	ulint			n_slots;
	const rec_t*		rec;
	const byte*		rec_heap_top;
	ulint			count;
	ulint			own_count;
	ibool			ret	= FALSE;

	ut_a(page_is_comp(page));

	/* Check first that the record heap and the directory do not
	overlap. */

	n_slots = page_dir_get_n_slots(page);

	if (UNIV_UNLIKELY(n_slots > UNIV_PAGE_SIZE / 4)) {
		ib::error() << "Nonsensical number " << n_slots
			<< " of page dir slots";

		goto func_exit;
	}

	rec_heap_top = page_header_get_ptr(page, PAGE_HEAP_TOP);

	if (UNIV_UNLIKELY(rec_heap_top
			  > page_dir_get_nth_slot(page, n_slots - 1))) {

		ib::error() << "Record heap and dir overlap on a page,"
			" heap top "
			<< page_header_get_field(page, PAGE_HEAP_TOP)
			<< ", dir " << page_offset(
				page_dir_get_nth_slot(page, n_slots - 1));

		goto func_exit;
	}

	/* Validate the record list in a loop checking also that it is
	consistent with the page record directory. */

	count = 0;
	own_count = 1;
	slot_no = 0;
	slot = page_dir_get_nth_slot(page, slot_no);

	rec = page_get_infimum_rec(page);

	for (;;) {
		if (UNIV_UNLIKELY(rec > rec_heap_top)) {

			ib::error() << "Record " << page_offset(rec)
				<< " is above rec heap top "
				<< page_offset(rec_heap_top);

			goto func_exit;
		}

		if (UNIV_UNLIKELY(rec_get_n_owned_new(rec))) {
			/* This is a record pointed to by a dir slot */
			if (UNIV_UNLIKELY(rec_get_n_owned_new(rec)
					  != own_count)) {

				ib::error() << "Wrong owned count "
					<< rec_get_n_owned_new(rec) << ", "
					<< own_count << ", rec "
					<< page_offset(rec);

				goto func_exit;
			}

			if (UNIV_UNLIKELY
			    (page_dir_slot_get_rec(slot) != rec)) {
				ib::error() << "Dir slot does not point"
					" to right rec " << page_offset(rec);

				goto func_exit;
			}

			own_count = 0;

			if (!page_rec_is_supremum(rec)) {
				slot_no++;
				slot = page_dir_get_nth_slot(page, slot_no);
			}
		}

		if (page_rec_is_supremum(rec)) {

			break;
		}

		if (UNIV_UNLIKELY
		    (rec_get_next_offs(rec, TRUE) < FIL_PAGE_DATA
		     || rec_get_next_offs(rec, TRUE) >= UNIV_PAGE_SIZE)) {

			ib::error() << "Next record offset nonsensical "
				<< rec_get_next_offs(rec, TRUE)
				<< " for rec " << page_offset(rec);

			goto func_exit;
		}

		count++;

		if (UNIV_UNLIKELY(count > UNIV_PAGE_SIZE)) {
			ib::error() << "Page record list appears to be"
				" circular " << count;
			goto func_exit;
		}

		rec = page_rec_get_next_const(rec);
		own_count++;
	}

	if (UNIV_UNLIKELY(rec_get_n_owned_new(rec) == 0)) {
		ib::error() << "n owned is zero in a supremum rec";

		goto func_exit;
	}

	if (UNIV_UNLIKELY(slot_no != n_slots - 1)) {
		ib::error() << "n slots wrong " << slot_no << ", "
			<< (n_slots - 1);
		goto func_exit;
	}

	if (UNIV_UNLIKELY(page_header_get_field(page, PAGE_N_RECS)
			  + PAGE_HEAP_NO_USER_LOW
			  != count + 1)) {
		ib::error() << "n recs wrong "
			<< page_header_get_field(page, PAGE_N_RECS)
			+ PAGE_HEAP_NO_USER_LOW << " " << (count + 1);

		goto func_exit;
	}

	/* Check then the free list */
	rec = page_header_get_ptr(page, PAGE_FREE);

	while (rec != NULL) {
		if (UNIV_UNLIKELY(rec < page + FIL_PAGE_DATA
				  || rec >= page + UNIV_PAGE_SIZE)) {

			ib::error() << "Free list record has"
				" a nonsensical offset " << page_offset(rec);

			goto func_exit;
		}

		if (UNIV_UNLIKELY(rec > rec_heap_top)) {
			ib::error() << "Free list record " << page_offset(rec)
				<< " is above rec heap top "
				<< page_offset(rec_heap_top);

			goto func_exit;
		}

		count++;

		if (UNIV_UNLIKELY(count > UNIV_PAGE_SIZE)) {
			ib::error() << "Page free list appears to be"
				" circular " << count;
			goto func_exit;
		}

		rec = page_rec_get_next_const(rec);
	}

	if (UNIV_UNLIKELY(page_dir_get_n_heap(page) != count + 1)) {

		ib::error() << "N heap is wrong "
			<< page_dir_get_n_heap(page) << ", " << (count + 1);

		goto func_exit;
	}

	ret = TRUE;

func_exit:
	return(ret);
}

/***************************************************************//**
This function checks if the page in which record is present is a
non-leaf node of a spatial index.
param[in]	rec	Btree record
param[in]	index	index
@return TRUE if ok */
bool
page_is_spatial_non_leaf(
/*====================*/
	const rec_t*	rec,
	dict_index_t*	index)
{
     return (dict_index_is_spatial(index) && !page_is_leaf(page_align(rec)));
}

/***************************************************************//**
This function checks the consistency of an index page.
@return TRUE if ok */
ibool
page_validate(
/*==========*/
	const page_t*	page,	/*!< in: index page */
	dict_index_t*	index)	/*!< in: data dictionary index containing
				the page record type definition */
{
	const page_dir_slot_t*	slot;
	mem_heap_t*		heap;
	byte*			buf;
	ulint			count;
	ulint			own_count;
	ulint			rec_own_count;
	ulint			slot_no;
	ulint			data_size;
	const rec_t*		rec;
	const rec_t*		old_rec		= NULL;
	ulint			offs;
	ulint			n_slots;
	ibool			ret		= FALSE;
	ulint			i;
	ulint*			offsets		= NULL;
	ulint*			old_offsets	= NULL;

#ifdef UNIV_GIS_DEBUG
	if (dict_index_is_spatial(index)) {
		fprintf(stderr, "Page no: %lu\n", page_get_page_no(page));
	}
#endif /* UNIV_DEBUG */

	if (UNIV_UNLIKELY((ibool) !!page_is_comp(page)
			  != dict_table_is_comp(index->table))) {
		ib::error() << "'compact format' flag mismatch";
		goto func_exit2;
	}
	if (page_is_comp(page)) {
		if (UNIV_UNLIKELY(!page_simple_validate_new(page))) {
			goto func_exit2;
		}
	} else {
		if (UNIV_UNLIKELY(!page_simple_validate_old(page))) {
			goto func_exit2;
		}
	}

	/* Multiple transactions cannot simultaneously operate on the
	same temp-table in parallel.
	max_trx_id is ignored for temp tables because it not required
	for MVCC. */
	if (dict_index_is_sec_or_ibuf(index)
	    && !dict_table_is_temporary(index->table)
	    && page_is_leaf(page)
	    && !page_is_empty(page)) {
		trx_id_t	max_trx_id	= page_get_max_trx_id(page);
		trx_id_t	sys_max_trx_id	= trx_sys_get_max_trx_id();

		if (max_trx_id == 0 || max_trx_id > sys_max_trx_id) {
			ib::error() << "PAGE_MAX_TRX_ID out of bounds: "
				<< max_trx_id << ", " << sys_max_trx_id;
			goto func_exit2;
		}
	}

	heap = mem_heap_create(UNIV_PAGE_SIZE + 200);

	/* The following buffer is used to check that the
	records in the page record heap do not overlap */

	buf = static_cast<byte*>(mem_heap_zalloc(heap, UNIV_PAGE_SIZE));

	/* Check first that the record heap and the directory do not
	overlap. */

	n_slots = page_dir_get_n_slots(page);

	if (UNIV_UNLIKELY(!(page_header_get_ptr(page, PAGE_HEAP_TOP)
			    <= page_dir_get_nth_slot(page, n_slots - 1)))) {

		ib::warn() << "Record heap and dir overlap on space "
			<< page_get_space_id(page) << " page "
			<< page_get_page_no(page) << " index " << index->name
			<< ", " << page_header_get_ptr(page, PAGE_HEAP_TOP)
			<< ", " << page_dir_get_nth_slot(page, n_slots - 1);

		goto func_exit;
	}

	/* Validate the record list in a loop checking also that
	it is consistent with the directory. */
	count = 0;
	data_size = 0;
	own_count = 1;
	slot_no = 0;
	slot = page_dir_get_nth_slot(page, slot_no);

	rec = page_get_infimum_rec(page);

	for (;;) {
		offsets = rec_get_offsets(rec, index, offsets,
					  ULINT_UNDEFINED, &heap);

		if (page_is_comp(page) && page_rec_is_user_rec(rec)
		    && UNIV_UNLIKELY(rec_get_node_ptr_flag(rec)
				     == page_is_leaf(page))) {
			ib::error() << "'node_ptr' flag mismatch";
			goto func_exit;
		}

		if (UNIV_UNLIKELY(!page_rec_validate(rec, offsets))) {
			goto func_exit;
		}

#ifndef UNIV_HOTBACKUP
		/* Check that the records are in the ascending order */
		if (count >= PAGE_HEAP_NO_USER_LOW
		    && !page_rec_is_supremum(rec)) {

			int	ret = cmp_rec_rec(
				rec, old_rec, offsets, old_offsets, index,
				page_is_spatial_non_leaf(rec, index));

			/* For spatial index, on nonleaf leavel, we
			allow recs to be equal. */
			bool rtr_equal_nodeptrs =
				(ret == 0 && dict_index_is_spatial(index)
				&& !page_is_leaf(page));

			if (ret <= 0 && !rtr_equal_nodeptrs) {

				ib::error() << "Records in wrong order on"
					" space " << page_get_space_id(page)
					<< " page " << page_get_page_no(page)
					<< " index " << index->name;

				fputs("\nInnoDB: previous record ", stderr);
				/* For spatial index, print the mbr info.*/
				if (index->type & DICT_SPATIAL) {
					putc('\n', stderr);
					rec_print_mbr_rec(stderr,
						old_rec, old_offsets);
					fputs("\nInnoDB: record ", stderr);
					putc('\n', stderr);
					rec_print_mbr_rec(stderr, rec, offsets);
					putc('\n', stderr);
					putc('\n', stderr);

				} else {
					rec_print_new(stderr, old_rec, old_offsets);
					fputs("\nInnoDB: record ", stderr);
					rec_print_new(stderr, rec, offsets);
					putc('\n', stderr);
				}

				goto func_exit;
			}
		}
#endif /* !UNIV_HOTBACKUP */

		if (page_rec_is_user_rec(rec)) {

			data_size += rec_offs_size(offsets);

#if UNIV_GIS_DEBUG
			/* For spatial index, print the mbr info.*/
			if (index->type & DICT_SPATIAL) {
				rec_print_mbr_rec(stderr, rec, offsets);
				putc('\n', stderr);
			}
#endif /* UNIV_GIS_DEBUG */
		}

		offs = page_offset(rec_get_start(rec, offsets));
		i = rec_offs_size(offsets);
		if (UNIV_UNLIKELY(offs + i >= UNIV_PAGE_SIZE)) {
			ib::error() << "Record offset out of bounds";
			goto func_exit;
		}

		while (i--) {
			if (UNIV_UNLIKELY(buf[offs + i])) {
				/* No other record may overlap this */
				ib::error() << "Record overlaps another";
				goto func_exit;
			}

			buf[offs + i] = 1;
		}

		if (page_is_comp(page)) {
			rec_own_count = rec_get_n_owned_new(rec);
		} else {
			rec_own_count = rec_get_n_owned_old(rec);
		}

		if (UNIV_UNLIKELY(rec_own_count)) {
			/* This is a record pointed to by a dir slot */
			if (UNIV_UNLIKELY(rec_own_count != own_count)) {
				ib::error() << "Wrong owned count "
					<< rec_own_count << ", " << own_count;
				goto func_exit;
			}

			if (page_dir_slot_get_rec(slot) != rec) {
				ib::error() << "Dir slot does not"
					" point to right rec";
				goto func_exit;
			}

			page_dir_slot_check(slot);

			own_count = 0;
			if (!page_rec_is_supremum(rec)) {
				slot_no++;
				slot = page_dir_get_nth_slot(page, slot_no);
			}
		}

		if (page_rec_is_supremum(rec)) {
			break;
		}

		count++;
		own_count++;
		old_rec = rec;
		rec = page_rec_get_next_const(rec);

		/* set old_offsets to offsets; recycle offsets */
		{
			ulint* offs = old_offsets;
			old_offsets = offsets;
			offsets = offs;
		}
	}

	if (page_is_comp(page)) {
		if (UNIV_UNLIKELY(rec_get_n_owned_new(rec) == 0)) {

			goto n_owned_zero;
		}
	} else if (UNIV_UNLIKELY(rec_get_n_owned_old(rec) == 0)) {
n_owned_zero:
		ib::error() <<  "n owned is zero";
		goto func_exit;
	}

	if (UNIV_UNLIKELY(slot_no != n_slots - 1)) {
		ib::error() << "n slots wrong " << slot_no << " "
			<< (n_slots - 1);
		goto func_exit;
	}

	if (UNIV_UNLIKELY(page_header_get_field(page, PAGE_N_RECS)
			  + PAGE_HEAP_NO_USER_LOW
			  != count + 1)) {
		ib::error() << "n recs wrong "
			<< page_header_get_field(page, PAGE_N_RECS)
			+ PAGE_HEAP_NO_USER_LOW << " " << (count + 1);
		goto func_exit;
	}

	if (UNIV_UNLIKELY(data_size != page_get_data_size(page))) {
		ib::error() << "Summed data size " << data_size
			<< ", returned by func " << page_get_data_size(page);
		goto func_exit;
	}

	/* Check then the free list */
	rec = page_header_get_ptr(page, PAGE_FREE);

	while (rec != NULL) {
		offsets = rec_get_offsets(rec, index, offsets,
					  ULINT_UNDEFINED, &heap);
		if (UNIV_UNLIKELY(!page_rec_validate(rec, offsets))) {

			goto func_exit;
		}

		count++;
		offs = page_offset(rec_get_start(rec, offsets));
		i = rec_offs_size(offsets);
		if (UNIV_UNLIKELY(offs + i >= UNIV_PAGE_SIZE)) {
			ib::error() << "Record offset out of bounds";
			goto func_exit;
		}

		while (i--) {

			if (UNIV_UNLIKELY(buf[offs + i])) {
				ib::error() << "Record overlaps another"
					" in free list";
				goto func_exit;
			}

			buf[offs + i] = 1;
		}

		rec = page_rec_get_next_const(rec);
	}

	if (UNIV_UNLIKELY(page_dir_get_n_heap(page) != count + 1)) {
		ib::error() << "N heap is wrong "
			<< page_dir_get_n_heap(page) << " " << count + 1;
		goto func_exit;
	}

	ret = TRUE;

func_exit:
	mem_heap_free(heap);

	if (UNIV_UNLIKELY(ret == FALSE)) {
func_exit2:
		ib::error() << "Apparent corruption in space "
			<< page_get_space_id(page) << " page "
			<< page_get_page_no(page) << " index " << index->name;
	}

	return(ret);
}

#ifndef UNIV_HOTBACKUP
/***************************************************************//**
Looks in the page record list for a record with the given heap number.
@return record, NULL if not found */
const rec_t*
page_find_rec_with_heap_no(
/*=======================*/
	const page_t*	page,	/*!< in: index page */
	ulint		heap_no)/*!< in: heap number */
{
	const rec_t*	rec;

	if (page_is_comp(page)) {
		rec = page + PAGE_NEW_INFIMUM;

		for (;;) {
			ulint	rec_heap_no = rec_get_heap_no_new(rec);

			if (rec_heap_no == heap_no) {

				return(rec);
			} else if (rec_heap_no == PAGE_HEAP_NO_SUPREMUM) {

				return(NULL);
			}

			rec = page + rec_get_next_offs(rec, TRUE);
		}
	} else {
		rec = page + PAGE_OLD_INFIMUM;

		for (;;) {
			ulint	rec_heap_no = rec_get_heap_no_old(rec);

			if (rec_heap_no == heap_no) {

				return(rec);
			} else if (rec_heap_no == PAGE_HEAP_NO_SUPREMUM) {

				return(NULL);
			}

			rec = page + rec_get_next_offs(rec, FALSE);
		}
	}
}
#endif /* !UNIV_HOTBACKUP */

/*******************************************************//**
Removes the record from a leaf page. This function does not log
any changes. It is used by the IMPORT tablespace functions.
The cursor is moved to the next record after the deleted one.
@return true if success, i.e., the page did not become too empty */
bool
page_delete_rec(
/*============*/
	const dict_index_t*	index,	/*!< in: The index that the record
					belongs to */
	page_cur_t*		pcur,	/*!< in/out: page cursor on record
					to delete */
	page_zip_des_t*		page_zip,/*!< in: compressed page descriptor */
	const ulint*		offsets)/*!< in: offsets for record */
{
	bool		no_compress_needed;
	buf_block_t*	block = pcur->block;
	page_t*		page = buf_block_get_frame(block);

	ut_ad(page_is_leaf(page));

	if (!rec_offs_any_extern(offsets)
	    && ((page_get_data_size(page) - rec_offs_size(offsets)
		< BTR_CUR_PAGE_COMPRESS_LIMIT(index))
		|| (mach_read_from_4(page + FIL_PAGE_NEXT) == FIL_NULL
		    && mach_read_from_4(page + FIL_PAGE_PREV) == FIL_NULL)
		|| (page_get_n_recs(page) < 2))) {

		ulint	root_page_no = dict_index_get_page(index);

		/* The page fillfactor will drop below a predefined
		minimum value, OR the level in the B-tree contains just
		one page, OR the page will become empty: we recommend
		compression if this is not the root page. */

		no_compress_needed = page_get_page_no(page) == root_page_no;
	} else {
		no_compress_needed = true;
	}

	if (no_compress_needed) {
#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

		page_cur_delete_rec(pcur, index, offsets, 0);

#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
	}

	return(no_compress_needed);
}

/** Get the last non-delete-marked record on a page.
@param[in]	page	index tree leaf page
@return the last record, not delete-marked
@retval infimum record if all records are delete-marked */
const rec_t*
page_find_rec_max_not_deleted(
	const page_t*	page)
{
	const rec_t*	rec = page_get_infimum_rec(page);
	const rec_t*	prev_rec = NULL; // remove warning

	/* Because the page infimum is never delete-marked,
	prev_rec will always be assigned to it first. */
	ut_ad(!rec_get_deleted_flag(rec, page_rec_is_comp(rec)));
	if (page_is_comp(page)) {
		do {
			if (!rec_get_deleted_flag(rec, true)) {
				prev_rec = rec;
			}
			rec = page_rec_get_next_low(rec, true);
		} while (rec != page + PAGE_NEW_SUPREMUM);
	} else {
		do {
			if (!rec_get_deleted_flag(rec, false)) {
				prev_rec = rec;
			}
			rec = page_rec_get_next_low(rec, false);
		} while (rec != page + PAGE_OLD_SUPREMUM);
	}
	return(prev_rec);
}

/** Issue a warning when the checksum that is stored in the page is valid,
but different than the global setting innodb_checksum_algorithm.
@param[in]	current_algo	current checksum algorithm
@param[in]	page_checksum	page valid checksum
@param[in]	page_id		page identifier */
void
page_warn_strict_checksum(
	srv_checksum_algorithm_t	curr_algo,
	srv_checksum_algorithm_t	page_checksum,
	const page_id_t&		page_id)
{
	srv_checksum_algorithm_t	curr_algo_nonstrict;
	switch (curr_algo) {
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		curr_algo_nonstrict = SRV_CHECKSUM_ALGORITHM_CRC32;
		break;
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		curr_algo_nonstrict = SRV_CHECKSUM_ALGORITHM_INNODB;
		break;
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		curr_algo_nonstrict = SRV_CHECKSUM_ALGORITHM_NONE;
		break;
	default:
		ut_error;
	}

	ib::warn() << "innodb_checksum_algorithm is set to \""
		<< buf_checksum_algorithm_name(curr_algo) << "\""
		<< " but the page " << page_id << " contains a valid checksum \""
		<< buf_checksum_algorithm_name(page_checksum) << "\". "
		<< " Accepting the page as valid. Change"
		<< " innodb_checksum_algorithm to \""
		<< buf_checksum_algorithm_name(curr_algo_nonstrict)
		<< "\" to silently accept such pages or rewrite all pages"
		<< " so that they contain \""
		<< buf_checksum_algorithm_name(curr_algo_nonstrict)
		<< "\" checksum.";
}
