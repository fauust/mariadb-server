/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2023, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file ibuf/ibuf0ibuf.cc
Insert buffer

Created 7/19/1997 Heikki Tuuri
*******************************************************/

#include <tuple>
#include "ibuf0ibuf.h"
#include "btr0sea.h"

/** Number of bits describing a single page */
#define IBUF_BITS_PER_PAGE	4
/** The start address for an insert buffer bitmap page bitmap */
#define IBUF_BITMAP		PAGE_DATA

#include "buf0buf.h"
#include "buf0rea.h"
#include "fsp0fsp.h"
#include "trx0sys.h"
#include "fil0fil.h"
#include "rem0rec.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "row0upd.h"
#include "dict0boot.h"
#include "fut0lst.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "que0que.h"
#include "srv0start.h" /* srv_shutdown_state */
#include "rem0cmp.h"
#include "log.h"

/*	STRUCTURE OF AN INSERT BUFFER RECORD

In versions < 4.1.x:

1. The first field is the page number.
2. The second field is an array which stores type info for each subsequent
   field. We store the information which affects the ordering of records, and
   also the physical storage size of an SQL NULL value. E.g., for CHAR(10) it
   is 10 bytes.
3. Next we have the fields of the actual index record.

In versions >= 4.1.x:

Note that contary to what we planned in the 1990's, there will only be one
insert buffer tree, and that is in the system tablespace of InnoDB.

1. The first field is the space id.
2. The second field is a one-byte marker (0) which differentiates records from
   the < 4.1.x storage format.
3. The third field is the page number.
4. The fourth field contains the type info, where we have also added 2 bytes to
   store the charset. In the compressed table format of 5.0.x we must add more
   information here so that we can build a dummy 'index' struct which 5.0.x
   can use in the binary search on the index page in the ibuf merge phase.
5. The rest of the fields contain the fields of the actual index record.

In versions >= 5.0.3:

The first byte of the fourth field is an additional marker (0) if the record
is in the compact format.  The presence of this marker can be detected by
looking at the length of the field modulo DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE.

The high-order bit of the character set field in the type info is the
"nullable" flag for the field.

In versions >= 5.5:

The optional marker byte at the start of the fourth field is replaced by
mandatory 3 fields, totaling 4 bytes:

 1. 2 bytes: Counter field, used to sort records within a (space id, page
    no) in the order they were added. This is needed so that for example the
    sequence of operations "INSERT x, DEL MARK x, INSERT x" is handled
    correctly.

 2. 1 byte: Operation type (see ibuf_op_t).

 3. 1 byte: Flags. Currently only one flag exists, IBUF_REC_COMPACT.

To ensure older records, which do not have counters to enforce correct
sorting, are merged before any new records, ibuf_insert checks if we're
trying to insert to a position that contains old-style records, and if so,
refuses the insert. Thus, ibuf pages are gradually converted to the new
format as their corresponding buffer pool pages are read into memory.
*/


/*	PREVENTING DEADLOCKS IN THE INSERT BUFFER SYSTEM

If an OS thread performs any operation that brings in disk pages from
non-system tablespaces into the buffer pool, or creates such a page there,
then the operation may have as a side effect an insert buffer index tree
compression. Thus, the tree latch of the insert buffer tree may be acquired
in the x-mode, and also the file space latch of the system tablespace may
be acquired in the x-mode.

Also, an insert to an index in a non-system tablespace can have the same
effect. How do we know this cannot lead to a deadlock of OS threads? There
is a problem with the i\o-handler threads: they break the latching order
because they own x-latches to pages which are on a lower level than the
insert buffer tree latch, its page latches, and the tablespace latch an
insert buffer operation can reserve.

The solution is the following: Let all the tree and page latches connected
with the insert buffer be later in the latching order than the fsp latch and
fsp page latches.

Insert buffer pages must be such that the insert buffer is never invoked
when these pages are accessed as this would result in a recursion violating
the latching order. We let a special i/o-handler thread take care of i/o to
the insert buffer pages and the ibuf bitmap pages, as well as the fsp bitmap
pages and the first inode page, which contains the inode of the ibuf tree: let
us call all these ibuf pages. To prevent deadlocks, we do not let a read-ahead
access both non-ibuf and ibuf pages.

Then an i/o-handler for the insert buffer never needs to access recursively the
insert buffer tree and thus obeys the latching order. On the other hand, other
i/o-handlers for other tablespaces may require access to the insert buffer,
but because all kinds of latches they need to access there are later in the
latching order, no violation of the latching order occurs in this case,
either.

A problem is how to grow and contract an insert buffer tree. As it is later
in the latching order than the fsp management, we have to reserve the fsp
latch first, before adding or removing pages from the insert buffer tree.
We let the insert buffer tree have its own file space management: a free
list of pages linked to the tree root. To prevent recursive using of the
insert buffer when adding pages to the tree, we must first load these pages
to memory, obtaining a latch on them, and only after that add them to the
free list of the insert buffer tree. More difficult is removing of pages
from the free list. If there is an excess of pages in the free list of the
ibuf tree, they might be needed if some thread reserves the fsp latch,
intending to allocate more file space. So we do the following: if a thread
reserves the fsp latch, we check the writer count field of the latch. If
this field has value 1, it means that the thread did not own the latch
before entering the fsp system, and the mtr of the thread contains no
modifications to the fsp pages. Now we are free to reserve the ibuf latch,
and check if there is an excess of pages in the free list. We can then, in a
separate mini-transaction, take them out of the free list and free them to
the fsp system.

To avoid deadlocks in the ibuf system, we divide file pages into three levels:

(1) non-ibuf pages,
(2) ibuf tree pages and the pages in the ibuf tree free list, and
(3) ibuf bitmap pages.

No OS thread is allowed to access higher level pages if it has latches to
lower level pages; even if the thread owns a B-tree latch it must not access
the B-tree non-leaf pages if it has latches on lower level pages. Read-ahead
is only allowed for level 1 and 2 pages. Dedicated i/o-handler threads handle
exclusively level 1 i/o. A dedicated i/o handler thread handles exclusively
level 2 i/o. However, if an OS thread does the i/o handling for itself, i.e.,
it uses synchronous aio, it can access any pages, as long as it obeys the
access order rules. */

/** Operations that can currently be buffered. */
ulong	innodb_change_buffering;

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/** Dump the change buffer at startup */
my_bool	ibuf_dump;
/** Flag to control insert buffer debugging. */
uint	ibuf_debug;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

/** The insert buffer control structure */
ibuf_t	ibuf;

/** @name Offsets to the per-page bits in the insert buffer bitmap */
/* @{ */
#define	IBUF_BITMAP_FREE	0	/*!< Bits indicating the
					amount of free space */
#define IBUF_BITMAP_BUFFERED	2	/*!< TRUE if there are buffered
					changes for the page */
#define IBUF_BITMAP_IBUF	3	/*!< TRUE if page is a part of
					the ibuf tree, excluding the
					root page, or is in the free
					list of the ibuf */
/* @} */

#define IBUF_REC_FIELD_SPACE	0	/*!< in the pre-4.1 format,
					the page number. later, the space_id */
#define IBUF_REC_FIELD_MARKER	1	/*!< starting with 4.1, a marker
					consisting of 1 byte that is 0 */
#define IBUF_REC_FIELD_PAGE	2	/*!< starting with 4.1, the
					page number */
#define IBUF_REC_FIELD_METADATA	3	/* the metadata field */
#define IBUF_REC_FIELD_USER	4	/* first user field */

/* Various constants for checking the type of an ibuf record and extracting
data from it. For details, see the description of the record format at the
top of this file. */

/** @name Format of the IBUF_REC_FIELD_METADATA of an insert buffer record
The fourth column in the MySQL 5.5 format contains an operation
type, counter, and some flags. */
/* @{ */
#define IBUF_REC_INFO_SIZE	4	/*!< Combined size of info fields at
					the beginning of the fourth field */

/* Offsets for the fields at the beginning of the fourth field */
#define IBUF_REC_OFFSET_COUNTER	0	/*!< Operation counter */
#define IBUF_REC_OFFSET_TYPE	2	/*!< Type of operation */
#define IBUF_REC_OFFSET_FLAGS	3	/*!< Additional flags */

/* Record flag masks */
#define IBUF_REC_COMPACT	0x1	/*!< Set in
					IBUF_REC_OFFSET_FLAGS if the
					user index is in COMPACT
					format or later */


#ifndef SAFE_MUTEX
static
#endif /* SAFE_MUTEX */
/** The mutex protecting the insert buffer */
mysql_mutex_t ibuf_mutex,
	/** The mutex covering pessimistic inserts into the change buffer */
	ibuf_pessimistic_insert_mutex;

/** The area in pages from which contract looks for page numbers for merge */
constexpr ulint		IBUF_MERGE_AREA = 8;

/** In ibuf_contract() at most this number of pages is read to memory in one
batch, in order to merge the entries for them in the change buffer */
constexpr ulint		IBUF_MAX_N_PAGES_MERGED = IBUF_MERGE_AREA;

/* TODO: how to cope with drop table if there are records in the insert
buffer for the indexes of the table? Is there actually any problem,
because ibuf merge is done to a page when it is read in, and it is
still physically like the index page even if the index would have been
dropped! So, there seems to be no problem. */

/******************************************************************//**
Sets the flag in the current mini-transaction record indicating we're
inside an insert buffer routine. */
UNIV_INLINE
void
ibuf_enter(
/*=======*/
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(!mtr->is_inside_ibuf());
	mtr->enter_ibuf();
}

/******************************************************************//**
Sets the flag in the current mini-transaction record indicating we're
exiting an insert buffer routine. */
UNIV_INLINE
void
ibuf_exit(
/*======*/
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(mtr->is_inside_ibuf());
	mtr->exit_ibuf();
}

/**************************************************************//**
Commits an insert buffer mini-transaction and sets the persistent
cursor latch mode to BTR_NO_LATCHES, that is, detaches the cursor. */
UNIV_INLINE
void
ibuf_btr_pcur_commit_specify_mtr(
/*=============================*/
	btr_pcur_t*	pcur,	/*!< in/out: persistent cursor */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ut_d(ibuf_exit(mtr));
	btr_pcur_commit_specify_mtr(pcur, mtr);
}

/******************************************************************//**
Gets the ibuf header page and x-latches it.
@return insert buffer header page */
static
page_t*
ibuf_header_page_get(
/*=================*/
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(!ibuf_inside(mtr));

	buf_block_t* block = buf_page_get(
		page_id_t(IBUF_SPACE_ID, FSP_IBUF_HEADER_PAGE_NO),
		0, RW_X_LATCH, mtr);
	if (UNIV_UNLIKELY(!block)) {
		return nullptr;
	}

	buf_page_make_young_if_needed(&block->page);

	return block->page.frame;
}

/** Acquire the change buffer root page.
@param[in,out]  mtr     mini-transaction
@return change buffer root page, SX-latched */
static buf_block_t *ibuf_tree_root_get(mtr_t *mtr, dberr_t *err= nullptr)
{
  ut_ad(ibuf_inside(mtr));
  mysql_mutex_assert_owner(&ibuf_mutex);

  mtr_sx_lock_index(ibuf.index, mtr);

  buf_block_t *block=
    buf_page_get_gen(page_id_t{IBUF_SPACE_ID, FSP_IBUF_TREE_ROOT_PAGE_NO},
                     0, RW_SX_LATCH, nullptr, BUF_GET, mtr, err);
  if (block)
  {
    ut_ad(ibuf.empty == page_is_empty(block->page.frame));
    buf_page_make_young_if_needed(&block->page);
  }

  return block;
}

/******************************************************************//**
Closes insert buffer and frees the data structures. */
void
ibuf_close(void)
/*============*/
{
	if (!ibuf.index) {
		return;
	}

	mysql_mutex_destroy(&ibuf_pessimistic_insert_mutex);
	mysql_mutex_destroy(&ibuf_mutex);

	dict_table_t*	ibuf_table = ibuf.index->table;
	ibuf.index->lock.free();
	dict_mem_index_free(ibuf.index);
	dict_mem_table_free(ibuf_table);
	ibuf.index = NULL;
}

/******************************************************************//**
Updates the size information of the ibuf, assuming the segment size has not
changed. */
static
void
ibuf_size_update(
/*=============*/
	const page_t*	root)	/*!< in: ibuf tree root */
{
	mysql_mutex_assert_owner(&ibuf_mutex);

	ibuf.free_list_len = flst_get_len(root + PAGE_HEADER
					   + PAGE_BTR_IBUF_FREE_LIST);

	ibuf.height = uint8_t(1 + btr_page_get_level(root));

	/* the '1 +' is the ibuf header page */
	ibuf.size = ibuf.seg_size - (1 + ibuf.free_list_len);
}

/******************************************************************//**
Creates the insert buffer data structure at a database startup and initializes
the data structures for the insert buffer.
@return DB_SUCCESS or failure */
dberr_t
ibuf_init_at_db_start(void)
/*=======================*/
{
	page_t*		root;

	ut_ad(!ibuf.index);
	mtr_t mtr;
	mtr.start();
	compile_time_assert(IBUF_SPACE_ID == TRX_SYS_SPACE);
	compile_time_assert(IBUF_SPACE_ID == 0);
	mtr.x_lock_space(fil_system.sys_space);
	dberr_t err;
	buf_block_t* header_page = buf_page_get_gen(
		page_id_t(IBUF_SPACE_ID, FSP_IBUF_HEADER_PAGE_NO),
		0, RW_X_LATCH, nullptr, BUF_GET, &mtr, &err);

	if (!header_page) {
err_exit:
		sql_print_error("InnoDB: The change buffer is corrupted"
				" or has been removed on upgrade"
				" to MariaDB 11.0 or later");
		mtr.commit();
		if (innodb_change_buffering == IBUF_USE_NONE) {
			err = DB_SUCCESS;
		}
		return err;
	}

	fseg_n_reserved_pages(*header_page,
			      IBUF_HEADER + IBUF_TREE_SEG_HEADER
			      + header_page->page.frame, &ibuf.seg_size, &mtr);

	do {
		IF_DBUG(if (_db_keyword_(nullptr, "intermittent_read_failure",
					 1)) continue,);
		ut_ad(ibuf.seg_size >= 2);
	} while (0);

	if (buf_block_t* block =
	    buf_page_get_gen(page_id_t(IBUF_SPACE_ID,
				       FSP_IBUF_TREE_ROOT_PAGE_NO),
			     0, RW_X_LATCH, nullptr, BUF_GET, &mtr, &err)) {
		root = buf_block_get_frame(block);
	} else {
		goto err_exit;
	}

	DBUG_EXECUTE_IF("ibuf_init_corrupt",
			err = DB_CORRUPTION;
			goto err_exit;);

	if (page_is_comp(root) || fil_page_get_type(root) != FIL_PAGE_INDEX
	    || btr_page_get_index_id(root) != DICT_IBUF_ID_MIN) {
		err = DB_CORRUPTION;
		goto err_exit;
	}

	mysql_mutex_init(ibuf_mutex_key, &ibuf_mutex, nullptr);
	mysql_mutex_init(ibuf_pessimistic_insert_mutex_key,
			 &ibuf_pessimistic_insert_mutex, nullptr);

	ibuf_max_size_update(CHANGE_BUFFER_DEFAULT_SIZE);
	mysql_mutex_lock(&ibuf_mutex);
	ibuf_size_update(root);
	mysql_mutex_unlock(&ibuf_mutex);

	ibuf.empty = page_is_empty(root);
	mtr.commit();

	ibuf.index = dict_mem_index_create(
		dict_table_t::create(
			{C_STRING_WITH_LEN("innodb_change_buffer")},
			fil_system.sys_space, 1, 0, 0, 0),
		"CLUST_IND",
		DICT_CLUSTERED | DICT_IBUF, 1);
	ibuf.index->id = DICT_IBUF_ID_MIN + IBUF_SPACE_ID;
	ibuf.index->n_uniq = REC_MAX_N_FIELDS;
	ibuf.index->lock.SRW_LOCK_INIT(index_tree_rw_lock_key);
#ifdef BTR_CUR_ADAPT
	ibuf.index->search_info = btr_search_info_create(ibuf.index->heap);
#endif /* BTR_CUR_ADAPT */
	ibuf.index->page = FSP_IBUF_TREE_ROOT_PAGE_NO;
	ut_d(ibuf.index->cached = TRUE);

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
	if (!ibuf_dump) {
		return DB_SUCCESS;
	}
	ib::info() << "Dumping the change buffer";
	ibuf_mtr_start(&mtr);
	btr_pcur_t pcur;
	if (DB_SUCCESS
	    == pcur.open_leaf(true, ibuf.index, BTR_SEARCH_LEAF, &mtr)) {
		while (btr_pcur_move_to_next_user_rec(&pcur, &mtr)) {
			rec_print_old(stderr, btr_pcur_get_rec(&pcur));
		}
	}
	ibuf_mtr_commit(&mtr);
	ib::info() << "Dumped the change buffer";
#endif

	return DB_SUCCESS;
}

/*********************************************************************//**
Updates the max_size value for ibuf. */
void
ibuf_max_size_update(
/*=================*/
	ulint	new_val)	/*!< in: new value in terms of
				percentage of the buffer pool size */
{
	if (UNIV_UNLIKELY(!ibuf.index)) return;
	ulint	new_size = std::min<ulint>(
		buf_pool.curr_size() * new_val / 100, uint32_t(~0U));
	mysql_mutex_lock(&ibuf_mutex);
	ibuf.max_size = uint32_t(new_size);
	mysql_mutex_unlock(&ibuf_mutex);
}

# ifdef UNIV_DEBUG
/** Gets the desired bits for a given page from a bitmap page.
@param[in]	page		bitmap page
@param[in]	page_id		page id whose bits to get
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	bit		IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED, ...
@param[in,out]	mtr		mini-transaction holding an x-latch on the
bitmap page
@return value of bits */
#  define ibuf_bitmap_page_get_bits(page, page_id, zip_size, bit, mtr)	\
	ibuf_bitmap_page_get_bits_low(page, page_id, zip_size,		\
				      MTR_MEMO_PAGE_X_FIX, mtr, bit)
# else /* UNIV_DEBUG */
/** Gets the desired bits for a given page from a bitmap page.
@param[in]	page		bitmap page
@param[in]	page_id		page id whose bits to get
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	bit		IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED, ...
@param[in,out]	mtr		mini-transaction holding an x-latch on the
bitmap page
@return value of bits */
#  define ibuf_bitmap_page_get_bits(page, page_id, zip_size, bit, mtr)	\
	ibuf_bitmap_page_get_bits_low(page, page_id, zip_size, bit)
# endif /* UNIV_DEBUG */

/** Gets the desired bits for a given page from a bitmap page.
@param[in]	page		bitmap page
@param[in]	page_id		page id whose bits to get
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	latch_type	MTR_MEMO_PAGE_X_FIX, MTR_MEMO_BUF_FIX, ...
@param[in,out]	mtr		mini-transaction holding latch_type on the
bitmap page
@param[in]	bit		IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED, ...
@return value of bits */
UNIV_INLINE
ulint
ibuf_bitmap_page_get_bits_low(
	const page_t*		page,
	const page_id_t		page_id,
	ulint			zip_size,
#ifdef UNIV_DEBUG
	ulint			latch_type,
	mtr_t*			mtr,
#endif /* UNIV_DEBUG */
	ulint			bit)
{
	ulint	byte_offset;
	ulint	bit_offset;
	ulint	map_byte;
	ulint	value;
	const ulint size = zip_size ? zip_size : srv_page_size;

	ut_ad(ut_is_2pow(zip_size));
	ut_ad(bit < IBUF_BITS_PER_PAGE);
	compile_time_assert(!(IBUF_BITS_PER_PAGE % 2));
	ut_ad(mtr->memo_contains_page_flagged(page, latch_type));

	bit_offset = (page_id.page_no() & (size - 1))
		* IBUF_BITS_PER_PAGE + bit;

	byte_offset = bit_offset / 8;
	bit_offset = bit_offset % 8;

	ut_ad(byte_offset + IBUF_BITMAP < srv_page_size);

	map_byte = mach_read_from_1(page + IBUF_BITMAP + byte_offset);

	value = ut_bit_get_nth(map_byte, bit_offset);

	if (bit == IBUF_BITMAP_FREE) {
		ut_ad(bit_offset + 1 < 8);

		value = value * 2 + ut_bit_get_nth(map_byte, bit_offset + 1);
	}

	return(value);
}

/** Sets the desired bit for a given page in a bitmap page.
@tparam bit	IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED, ...
@param[in,out]	block		bitmap page
@param[in]	page_id		page id whose bits to set
@param[in]	physical_size	page size
@param[in]	val		value to set
@param[in,out]	mtr		mtr containing an x-latch to the bitmap page */
template<ulint bit>
static void
ibuf_bitmap_page_set_bits(
	buf_block_t*		block,
	const page_id_t		page_id,
	ulint			physical_size,
	ulint			val,
	mtr_t*			mtr)
{
	ulint	byte_offset;
	ulint	bit_offset;

	static_assert(bit < IBUF_BITS_PER_PAGE, "wrong bit");
	compile_time_assert(!(IBUF_BITS_PER_PAGE % 2));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr->is_named_space(page_id.space()));

	bit_offset = (page_id.page_no() % physical_size)
		* IBUF_BITS_PER_PAGE + bit;

	byte_offset = bit_offset / 8;
	bit_offset = bit_offset % 8;

	ut_ad(byte_offset + IBUF_BITMAP < srv_page_size);

	byte* map_byte = &block->page.frame[IBUF_BITMAP + byte_offset];
	byte b = *map_byte;

	if (bit == IBUF_BITMAP_FREE) {
		ut_ad(bit_offset + 1 < 8);
		ut_ad(val <= 3);
		b &= static_cast<byte>(~(3U << bit_offset));
		b |= static_cast<byte>(((val & 2) >> 1) << bit_offset
				       | (val & 1) << (bit_offset + 1));
	} else {
		ut_ad(val <= 1);
		b &= static_cast<byte>(~(1U << bit_offset));
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion" /* GCC 5 may need this here */
#endif
		b |= static_cast<byte>(val << bit_offset);
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif
	}

	mtr->write<1,mtr_t::MAYBE_NOP>(*block, map_byte, b);
}

/** Calculates the bitmap page number for a given page number.
@param[in]	page_id		page id
@param[in]	size		page size
@return the bitmap page id where the file page is mapped */
inline page_id_t ibuf_bitmap_page_no_calc(const page_id_t page_id, ulint size)
{
  if (!size)
    size= srv_page_size;

  return page_id_t(page_id.space(), FSP_IBUF_BITMAP_OFFSET
		   + uint32_t(page_id.page_no() & ~(size - 1)));
}

/** Gets the ibuf bitmap page where the bits describing a given file page are
stored.
@param[in]	page_id		page id of the file page
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@return bitmap page where the file page is mapped, that is, the bitmap
page containing the descriptor bits for the file page; the bitmap page
is x-latched */
static
buf_block_t*
ibuf_bitmap_get_map_page(
	const page_id_t		page_id,
	ulint			zip_size,
	mtr_t*			mtr)
{
  return buf_page_get_gen(ibuf_bitmap_page_no_calc(page_id, zip_size),
                          zip_size, RW_X_LATCH, nullptr,
                          BUF_GET_POSSIBLY_FREED, mtr);
}

/************************************************************************//**
Sets the free bits of the page in the ibuf bitmap. This is done in a separate
mini-transaction, hence this operation does not restrict further work to only
ibuf bitmap operations, which would result if the latch to the bitmap page
were kept. */
UNIV_INLINE
void
ibuf_set_free_bits_low(
/*===================*/
	const buf_block_t*	block,	/*!< in: index page; free bits are set if
					the index is non-clustered and page
					level is 0 */
	ulint			val,	/*!< in: value to set: < 4 */
	mtr_t*			mtr)	/*!< in/out: mtr */
{
	ut_ad(mtr->is_named_space(block->page.id().space()));
	if (!page_is_leaf(block->page.frame)) {
		return;
	}

#ifdef UNIV_IBUF_DEBUG
	ut_a(val <= ibuf_index_page_calc_free(block));
#endif /* UNIV_IBUF_DEBUG */
	const page_id_t id(block->page.id());

	if (buf_block_t* bitmap_page = ibuf_bitmap_get_map_page(
			id, block->zip_size(), mtr)) {
		ibuf_bitmap_page_set_bits<IBUF_BITMAP_FREE>(
			bitmap_page, id, block->physical_size(),
			val, mtr);
	}
}

/************************************************************************//**
Sets the free bit of the page in the ibuf bitmap. This is done in a separate
mini-transaction, hence this operation does not restrict further work to only
ibuf bitmap operations, which would result if the latch to the bitmap page
were kept. */
void
ibuf_set_free_bits_func(
/*====================*/
	buf_block_t*	block,	/*!< in: index page of a non-clustered index;
				free bit is reset if page level is 0 */
#ifdef UNIV_IBUF_DEBUG
	ulint		max_val,/*!< in: ULINT_UNDEFINED or a maximum
				value which the bits must have before
				setting; this is for debugging */
#endif /* UNIV_IBUF_DEBUG */
	ulint		val)	/*!< in: value to set: < 4 */
{
  if (!page_is_leaf(block->page.frame))
    return;

  mtr_t	mtr;
  mtr.start();
  const page_id_t id(block->page.id());
  const fil_space_t *space= mtr.set_named_space_id(id.space());
  /* all callers of ibuf_update_free_bits_if_full() or ibuf_reset_free_bits()
  check this */
  ut_ad(!space->is_temporary());

  if (buf_block_t *bitmap_page=
      ibuf_bitmap_get_map_page(id, block->zip_size(), &mtr))
  {
    if (space->is_being_imported()) /* IndexPurge may invoke this */
      mtr.set_log_mode(MTR_LOG_NO_REDO);
#ifdef UNIV_IBUF_DEBUG
    if (max_val != ULINT_UNDEFINED)
    {
      ulint old_val= ibuf_bitmap_page_get_bits(bitmap_page, id,
                                               IBUF_BITMAP_FREE, &mtr);
      ut_a(old_val <= max_val);
    }

    ut_a(val <= ibuf_index_page_calc_free(block));
#endif /* UNIV_IBUF_DEBUG */

    ibuf_bitmap_page_set_bits<IBUF_BITMAP_FREE>
      (bitmap_page, id, block->physical_size(), val, &mtr);
  }

  mtr.commit();
}

/************************************************************************//**
Resets the free bits of the page in the ibuf bitmap. This is done in a
separate mini-transaction, hence this operation does not restrict
further work to only ibuf bitmap operations, which would result if the
latch to the bitmap page were kept.  NOTE: The free bits in the insert
buffer bitmap must never exceed the free space on a page.  It is safe
to decrement or reset the bits in the bitmap in a mini-transaction
that is committed before the mini-transaction that affects the free
space. */
void
ibuf_reset_free_bits(
/*=================*/
	buf_block_t*	block)	/*!< in: index page; free bits are set to 0
				if the index is a non-clustered
				non-unique, and page level is 0 */
{
	ibuf_set_free_bits(block, 0, ULINT_UNDEFINED);
}

/**********************************************************************//**
Updates the free bits for an uncompressed page to reflect the present
state.  Does this in the mtr given, which means that the latching
order rules virtually prevent any further operations for this OS
thread until mtr is committed.  NOTE: The free bits in the insert
buffer bitmap must never exceed the free space on a page.  It is safe
to set the free bits in the same mini-transaction that updated the
page. */
void
ibuf_update_free_bits_low(
/*======================*/
	const buf_block_t*	block,		/*!< in: index page */
	ulint			max_ins_size,	/*!< in: value of
						maximum insert size
						with reorganize before
						the latest operation
						performed to the page */
	mtr_t*			mtr)		/*!< in/out: mtr */
{
	ulint	before;
	ulint	after;

	ut_a(!is_buf_block_get_page_zip(block));
	ut_ad(mtr->is_named_space(block->page.id().space()));

	before = ibuf_index_page_calc_free_bits(srv_page_size,
						max_ins_size);

	after = ibuf_index_page_calc_free(block);

	/* This approach cannot be used on compressed pages, since the
	computed value of "before" often does not match the current
	state of the bitmap.  This is because the free space may
	increase or decrease when a compressed page is reorganized. */
	if (before != after) {
		ibuf_set_free_bits_low(block, after, mtr);
	}
}

/**********************************************************************//**
Updates the free bits for a compressed page to reflect the present
state.  Does this in the mtr given, which means that the latching
order rules virtually prevent any further operations for this OS
thread until mtr is committed.  NOTE: The free bits in the insert
buffer bitmap must never exceed the free space on a page.  It is safe
to set the free bits in the same mini-transaction that updated the
page. */
void
ibuf_update_free_bits_zip(
/*======================*/
	buf_block_t*	block,	/*!< in/out: index page */
	mtr_t*		mtr)	/*!< in/out: mtr */
{
	ut_ad(page_is_leaf(block->page.frame));
	ut_ad(block->zip_size());

	ulint after = ibuf_index_page_calc_free_zip(block);

	if (after == 0) {
		/* We move the page to the front of the buffer pool LRU list:
		the purpose of this is to prevent those pages to which we
		cannot make inserts using the insert buffer from slipping
		out of the buffer pool */

		buf_page_make_young(&block->page);
	}

	if (buf_block_t* bitmap_page = ibuf_bitmap_get_map_page(
		block->page.id(), block->zip_size(), mtr)) {

		ibuf_bitmap_page_set_bits<IBUF_BITMAP_FREE>(
			bitmap_page, block->page.id(),
			block->physical_size(), after, mtr);
	}
}

/**********************************************************************//**
Updates the free bits for the two pages to reflect the present state.
Does this in the mtr given, which means that the latching order rules
virtually prevent any further operations until mtr is committed.
NOTE: The free bits in the insert buffer bitmap must never exceed the
free space on a page.  It is safe to set the free bits in the same
mini-transaction that updated the pages. */
void
ibuf_update_free_bits_for_two_pages_low(
/*====================================*/
	buf_block_t*	block1,	/*!< in: index page */
	buf_block_t*	block2,	/*!< in: index page */
	mtr_t*		mtr)	/*!< in: mtr */
{
  ut_ad(mtr->is_named_space(block1->page.id().space()));
  ut_ad(block1->page.id().space() == block2->page.id().space());

  /* Avoid deadlocks by acquiring multiple bitmap page latches in
  a consistent order (smaller pointer first). */
  if (block1 > block2)
    std::swap(block1, block2);

  ibuf_set_free_bits_low(block1, ibuf_index_page_calc_free(block1), mtr);
  ibuf_set_free_bits_low(block2, ibuf_index_page_calc_free(block2), mtr);
}

/** Returns TRUE if the page is one of the fixed address ibuf pages.
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return TRUE if a fixed address ibuf i/o page */
inline bool ibuf_fixed_addr_page(const page_id_t page_id, ulint zip_size)
{
	return(page_id == page_id_t(IBUF_SPACE_ID, IBUF_TREE_ROOT_PAGE_NO)
	       || ibuf_bitmap_page(page_id, zip_size));
}

/** Checks if a page is a level 2 or 3 page in the ibuf hierarchy of pages.
Must not be called when recv_no_ibuf_operations==true.
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	x_latch		FALSE if relaxed check (avoid latching the
bitmap page)
@param[in,out]	mtr		mtr which will contain an x-latch to the
bitmap page if the page is not one of the fixed address ibuf pages, or NULL,
in which case a new transaction is created.
@return TRUE if level 2 or level 3 page */
bool
ibuf_page_low(
	const page_id_t		page_id,
	ulint			zip_size,
#ifdef UNIV_DEBUG
	bool			x_latch,
#endif /* UNIV_DEBUG */
	mtr_t*			mtr)
{
	ibool	ret;
	mtr_t	local_mtr;

	ut_ad(!recv_no_ibuf_operations);
	ut_ad(x_latch || mtr == NULL);

	if (ibuf_fixed_addr_page(page_id, zip_size)) {
		return(true);
	} else if (page_id.space() != IBUF_SPACE_ID) {
		return(false);
	}

	static_assert(IBUF_SPACE_ID == 0, "compatiblity");

#ifdef UNIV_DEBUG
	if (x_latch) {
	} else if (buf_block_t* block = buf_pool.page_fix(
			   ibuf_bitmap_page_no_calc(page_id, zip_size))) {
		local_mtr.start();
		local_mtr.memo_push(block, MTR_MEMO_BUF_FIX);
		/* We got the bitmap page without a page latch, so that
		we will not be violating the latching order when
		another bitmap page has already been latched by this
		thread. The page will be buffer-fixed, and thus it
		cannot be removed or relocated while we are looking at
		it. The contents of the page could change, but the
		IBUF_BITMAP_IBUF bit that we are interested in should
		not be modified by any other thread. Nobody should be
		calling ibuf_add_free_page() or ibuf_remove_free_page()
		while the page is linked to the insert buffer b-tree. */
		ret = ibuf_bitmap_page_get_bits_low(
			block->page.frame, page_id, zip_size,
			MTR_MEMO_BUF_FIX, &local_mtr, IBUF_BITMAP_IBUF);
		local_mtr.commit();
		return(ret);
	}
#endif /* UNIV_DEBUG */

	if (mtr == NULL) {
		mtr = &local_mtr;
		mtr_start(mtr);
	}

	buf_block_t *block = ibuf_bitmap_get_map_page(page_id, zip_size,
						      mtr);
	ret = block
		&& ibuf_bitmap_page_get_bits(block->page.frame,
					     page_id, zip_size,
					     IBUF_BITMAP_IBUF, mtr);

	if (mtr == &local_mtr) {
		mtr_commit(mtr);
	}

	return(ret);
}

#ifdef UNIV_DEBUG
# define ibuf_rec_get_page_no(mtr,rec) ibuf_rec_get_page_no_func(mtr,rec)
#else /* UNIV_DEBUG */
# define ibuf_rec_get_page_no(mtr,rec) ibuf_rec_get_page_no_func(rec)
#endif /* UNIV_DEBUG */

/********************************************************************//**
Returns the page number field of an ibuf record.
@return page number */
static
uint32_t
ibuf_rec_get_page_no_func(
/*======================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction owning rec */
#endif /* UNIV_DEBUG */
	const rec_t*	rec)	/*!< in: ibuf record */
{
	const byte*	field;
	ulint		len;

	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));
	ut_ad(rec_get_n_fields_old(rec) > 2);

	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_MARKER, &len);

	ut_a(len == 1);

	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_PAGE, &len);

	ut_a(len == 4);

	return(mach_read_from_4(field));
}

#ifdef UNIV_DEBUG
# define ibuf_rec_get_space(mtr,rec) ibuf_rec_get_space_func(mtr,rec)
#else /* UNIV_DEBUG */
# define ibuf_rec_get_space(mtr,rec) ibuf_rec_get_space_func(rec)
#endif /* UNIV_DEBUG */

/********************************************************************//**
Returns the space id field of an ibuf record. For < 4.1.x format records
returns 0.
@return space id */
static
uint32_t
ibuf_rec_get_space_func(
/*====================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction owning rec */
#endif /* UNIV_DEBUG */
	const rec_t*	rec)	/*!< in: ibuf record */
{
	const byte*	field;
	ulint		len;

	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));
	ut_ad(rec_get_n_fields_old(rec) > 2);

	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_MARKER, &len);

	ut_a(len == 1);

	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_SPACE, &len);

	ut_a(len == 4);

	return(mach_read_from_4(field));
}

#ifdef UNIV_DEBUG
# define ibuf_rec_get_info(mtr,rec,op,comp,info_len,counter)	\
	ibuf_rec_get_info_func(mtr,rec,op,comp,info_len,counter)
#else /* UNIV_DEBUG */
# define ibuf_rec_get_info(mtr,rec,op,comp,info_len,counter)	\
	ibuf_rec_get_info_func(rec,op,comp,info_len,counter)
#endif
/****************************************************************//**
Get various information about an ibuf record in >= 4.1.x format. */
static
void
ibuf_rec_get_info_func(
/*===================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction owning rec */
#endif /* UNIV_DEBUG */
	const rec_t*	rec,		/*!< in: ibuf record */
	ibuf_op_t*	op,		/*!< out: operation type, or NULL */
	ibool*		comp,		/*!< out: compact flag, or NULL */
	ulint*		info_len,	/*!< out: length of info fields at the
					start of the fourth field, or
					NULL */
	ulint*		counter)	/*!< in: counter value, or NULL */
{
	const byte*	types;
	ulint		fields;
	ulint		len;

	/* Local variables to shadow arguments. */
	ibuf_op_t	op_local;
	ibool		comp_local;
	ulint		info_len_local;
	ulint		counter_local;

	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));
	fields = rec_get_n_fields_old(rec);
	ut_a(fields > IBUF_REC_FIELD_USER);

	types = rec_get_nth_field_old(rec, IBUF_REC_FIELD_METADATA, &len);

	info_len_local = len % DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE;
	compile_time_assert(IBUF_REC_INFO_SIZE
			    < DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE);

	switch (info_len_local) {
	case 0:
	case 1:
		op_local = IBUF_OP_INSERT;
		comp_local = info_len_local;
		ut_ad(!counter);
		counter_local = ULINT_UNDEFINED;
		break;

	case IBUF_REC_INFO_SIZE:
		op_local = (ibuf_op_t) types[IBUF_REC_OFFSET_TYPE];
		comp_local = types[IBUF_REC_OFFSET_FLAGS] & IBUF_REC_COMPACT;
		counter_local = mach_read_from_2(
			types + IBUF_REC_OFFSET_COUNTER);
		break;

	default:
		ut_error;
	}

	ut_a(op_local < IBUF_OP_COUNT);
	ut_a((len - info_len_local) ==
	     (fields - IBUF_REC_FIELD_USER)
	     * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE);

	if (op) {
		*op = op_local;
	}

	if (comp) {
		*comp = comp_local;
	}

	if (info_len) {
		*info_len = info_len_local;
	}

	if (counter) {
		*counter = counter_local;
	}
}

#ifdef UNIV_DEBUG
# define ibuf_rec_get_op_type(mtr,rec) ibuf_rec_get_op_type_func(mtr,rec)
#else /* UNIV_DEBUG */
# define ibuf_rec_get_op_type(mtr,rec) ibuf_rec_get_op_type_func(rec)
#endif

/****************************************************************//**
Returns the operation type field of an ibuf record.
@return operation type */
static
ibuf_op_t
ibuf_rec_get_op_type_func(
/*======================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction owning rec */
#endif /* UNIV_DEBUG */
	const rec_t*	rec)	/*!< in: ibuf record */
{
	ulint		len;

	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));
	ut_ad(rec_get_n_fields_old(rec) > 2);

	(void) rec_get_nth_field_old(rec, IBUF_REC_FIELD_MARKER, &len);

	if (len > 1) {
		/* This is a < 4.1.x format record */

		return(IBUF_OP_INSERT);
	} else {
		ibuf_op_t	op;

		ibuf_rec_get_info(mtr, rec, &op, NULL, NULL, NULL);

		return(op);
	}
}

/****************************************************************//**
Read the first two bytes from a record's fourth field (counter field in new
records; something else in older records).
@return "counter" field, or ULINT_UNDEFINED if for some reason it
can't be read */
ulint
ibuf_rec_get_counter(
/*=================*/
	const rec_t*	rec)	/*!< in: ibuf record */
{
	const byte*	ptr;
	ulint		len;

	if (rec_get_n_fields_old(rec) <= IBUF_REC_FIELD_METADATA) {

		return(ULINT_UNDEFINED);
	}

	ptr = rec_get_nth_field_old(rec, IBUF_REC_FIELD_METADATA, &len);

	if (len >= 2) {

		return(mach_read_from_2(ptr));
	} else {

		return(ULINT_UNDEFINED);
	}
}


/**
  Add accumulated operation counts to a permanent array.
  Both arrays must be of size IBUF_OP_COUNT.
*/
static void ibuf_add_ops(Atomic_counter<ulint> *out, const ulint *in)
{
  for (auto i = 0; i < IBUF_OP_COUNT; i++)
    out[i]+= in[i];
}


/****************************************************************//**
Print operation counts. The array must be of size IBUF_OP_COUNT. */
static
void
ibuf_print_ops(
/*===========*/
	const char*			op_name,/*!< in: operation name */
	const Atomic_counter<ulint>*	ops,	/*!< in: operation counts */
	FILE*				file)	/*!< in: file where to print */
{
	static const char* op_names[] = {
		"insert",
		"delete mark",
		"delete"
	};

	static_assert(array_elements(op_names) == IBUF_OP_COUNT, "");
	fputs(op_name, file);

	for (ulint i = 0; i < IBUF_OP_COUNT; i++) {
		fprintf(file, "%s " ULINTPF "%s", op_names[i],
			ulint{ops[i]}, (i < (IBUF_OP_COUNT - 1)) ? ", " : "");
	}

	putc('\n', file);
}

/********************************************************************//**
Creates a dummy index for inserting a record to a non-clustered index.
@return dummy index */
static
dict_index_t*
ibuf_dummy_index_create(
/*====================*/
	ulint		n,	/*!< in: number of fields */
	ibool		comp)	/*!< in: TRUE=use compact record format */
{
	dict_table_t*	table;
	dict_index_t*	index;

	table = dict_table_t::create({C_STRING_WITH_LEN("IBUF_DUMMY")},
				     nullptr, n, 0,
				     comp ? DICT_TF_COMPACT : 0, 0);

	index = dict_mem_index_create(table, "IBUF_DUMMY", 0, n);

	/* avoid ut_ad(index->cached) in dict_index_get_n_unique_in_tree */
	index->cached = TRUE;
	ut_d(index->is_dummy = true);

	return(index);
}
/********************************************************************//**
Add a column to the dummy index */
static
void
ibuf_dummy_index_add_col(
/*=====================*/
	dict_index_t*	index,	/*!< in: dummy index */
	const dtype_t*	type,	/*!< in: the data type of the column */
	ulint		len)	/*!< in: length of the column */
{
	ulint	i	= index->table->n_def;
	dict_mem_table_add_col(index->table, NULL, NULL,
			       dtype_get_mtype(type),
			       dtype_get_prtype(type),
			       dtype_get_len(type));
	dict_index_add_col(index, index->table,
			   dict_table_get_nth_col(index->table, i), len);
}
/********************************************************************//**
Deallocates a dummy index for inserting a record to a non-clustered index. */
static
void
ibuf_dummy_index_free(
/*==================*/
	dict_index_t*	index)	/*!< in, own: dummy index */
{
	dict_table_t*	table = index->table;

	dict_mem_index_free(index);
	dict_mem_table_free(table);
}

#ifdef UNIV_DEBUG
# define ibuf_build_entry_from_ibuf_rec(mtr,ibuf_rec,heap,pindex)	\
	ibuf_build_entry_from_ibuf_rec_func(mtr,ibuf_rec,heap,pindex)
#else /* UNIV_DEBUG */
# define ibuf_build_entry_from_ibuf_rec(mtr,ibuf_rec,heap,pindex)	\
	ibuf_build_entry_from_ibuf_rec_func(ibuf_rec,heap,pindex)
#endif

/*********************************************************************//**
Builds the entry used to

1) IBUF_OP_INSERT: insert into a non-clustered index

2) IBUF_OP_DELETE_MARK: find the record whose delete-mark flag we need to
   activate

3) IBUF_OP_DELETE: find the record we need to delete

when we have the corresponding record in an ibuf index.

NOTE that as we copy pointers to fields in ibuf_rec, the caller must
hold a latch to the ibuf_rec page as long as the entry is used!

@return own: entry to insert to a non-clustered index */
static
dtuple_t*
ibuf_build_entry_from_ibuf_rec_func(
/*================================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction owning rec */
#endif /* UNIV_DEBUG */
	const rec_t*	ibuf_rec,	/*!< in: record in an insert buffer */
	mem_heap_t*	heap,		/*!< in: heap where built */
	dict_index_t**	pindex)		/*!< out, own: dummy index that
					describes the entry */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	ulint		n_fields;
	const byte*	types;
	const byte*	data;
	ulint		len;
	ulint		info_len;
	ulint		i;
	ulint		comp;
	dict_index_t*	index;

	ut_ad(mtr->memo_contains_page_flagged(ibuf_rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));

	data = rec_get_nth_field_old(ibuf_rec, IBUF_REC_FIELD_MARKER, &len);

	ut_a(len == 1);
	ut_a(*data == 0);
	ut_a(rec_get_n_fields_old(ibuf_rec) > IBUF_REC_FIELD_USER);

	n_fields = rec_get_n_fields_old(ibuf_rec) - IBUF_REC_FIELD_USER;

	tuple = dtuple_create(heap, n_fields);

	types = rec_get_nth_field_old(ibuf_rec, IBUF_REC_FIELD_METADATA, &len);

	ibuf_rec_get_info(mtr, ibuf_rec, NULL, &comp, &info_len, NULL);

	index = ibuf_dummy_index_create(n_fields, comp);

	len -= info_len;
	types += info_len;

	ut_a(len == n_fields * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE);

	for (i = 0; i < n_fields; i++) {
		field = dtuple_get_nth_field(tuple, i);

		data = rec_get_nth_field_old(
			ibuf_rec, i + IBUF_REC_FIELD_USER, &len);

		dfield_set_data(field, data, len);

		dtype_new_read_for_order_and_null_size(
			dfield_get_type(field),
			types + i * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE);

		ibuf_dummy_index_add_col(index, dfield_get_type(field), len);
	}

	index->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(unsigned(index->n_nullable)));

	/* Prevent an ut_ad() failure in page_zip_write_rec() by
	adding system columns to the dummy table pointed to by the
	dummy secondary index.  The insert buffer is only used for
	secondary indexes, whose records never contain any system
	columns, such as DB_TRX_ID. */
	ut_d(dict_table_add_system_columns(index->table, index->table->heap));

	*pindex = index;

	return(tuple);
}

/******************************************************************//**
Get the data size.
@return size of fields */
UNIV_INLINE
ulint
ibuf_rec_get_size(
/*==============*/
	const rec_t*	rec,			/*!< in: ibuf record */
	const byte*	types,			/*!< in: fields */
	ulint		n_fields,		/*!< in: number of fields */
	ulint		comp)			/*!< in: 0=ROW_FORMAT=REDUNDANT,
						nonzero=ROW_FORMAT=COMPACT */
{
	ulint	i;
	ulint	field_offset;
	ulint	types_offset;
	ulint	size = 0;

	field_offset = IBUF_REC_FIELD_USER;
	types_offset = DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE;

	for (i = 0; i < n_fields; i++) {
		ulint		len;
		dtype_t		dtype;

		rec_get_nth_field_offs_old(rec, i + field_offset, &len);

		if (len != UNIV_SQL_NULL) {
			size += len;
		} else {
			dtype_new_read_for_order_and_null_size(&dtype, types);

			size += dtype_get_sql_null_size(&dtype, comp);
		}

		types += types_offset;
	}

	return(size);
}

#ifdef UNIV_DEBUG
# define ibuf_rec_get_volume(mtr,rec) ibuf_rec_get_volume_func(mtr,rec)
#else /* UNIV_DEBUG */
# define ibuf_rec_get_volume(mtr,rec) ibuf_rec_get_volume_func(rec)
#endif

/********************************************************************//**
Returns the space taken by a stored non-clustered index entry if converted to
an index record.
@return size of index record in bytes + an upper limit of the space
taken in the page directory */
static
ulint
ibuf_rec_get_volume_func(
/*=====================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction owning rec */
#endif /* UNIV_DEBUG */
	const rec_t*	ibuf_rec)/*!< in: ibuf record */
{
	ulint		len;
	const byte*	data;
	const byte*	types;
	ulint		n_fields;
	ulint		data_size;
	ulint		comp;
	ibuf_op_t	op;
	ulint		info_len;

	ut_ad(mtr->memo_contains_page_flagged(ibuf_rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));
	ut_ad(rec_get_n_fields_old(ibuf_rec) > 2);

	data = rec_get_nth_field_old(ibuf_rec, IBUF_REC_FIELD_MARKER, &len);
	ut_a(len == 1);
	ut_a(*data == 0);

	types = rec_get_nth_field_old(
		ibuf_rec, IBUF_REC_FIELD_METADATA, &len);

	ibuf_rec_get_info(mtr, ibuf_rec, &op, &comp, &info_len, NULL);

	if (op == IBUF_OP_DELETE_MARK || op == IBUF_OP_DELETE) {
		/* Delete-marking a record doesn't take any
		additional space, and while deleting a record
		actually frees up space, we have to play it safe and
		pretend it takes no additional space (the record
		might not exist, etc.).  */

		return(0);
	} else if (comp) {
		dtuple_t*	entry;
		ulint		volume;
		dict_index_t*	dummy_index;
		mem_heap_t*	heap = mem_heap_create(500);

		entry = ibuf_build_entry_from_ibuf_rec(mtr, ibuf_rec,
			heap, &dummy_index);

		volume = rec_get_converted_size(dummy_index, entry, 0);

		ibuf_dummy_index_free(dummy_index);
		mem_heap_free(heap);

		return(volume + page_dir_calc_reserved_space(1));
	}

	types += info_len;
	n_fields = rec_get_n_fields_old(ibuf_rec)
		- IBUF_REC_FIELD_USER;

	data_size = ibuf_rec_get_size(ibuf_rec, types, n_fields, comp);

	return(data_size + rec_get_converted_extra_size(data_size, n_fields, 0)
	       + page_dir_calc_reserved_space(1));
}

/*********************************************************************//**
Builds the tuple to insert to an ibuf tree when we have an entry for a
non-clustered index.

NOTE that the original entry must be kept because we copy pointers to
its fields.

@return own: entry to insert into an ibuf index tree */
static
dtuple_t*
ibuf_entry_build(
/*=============*/
	ibuf_op_t	op,	/*!< in: operation type */
	dict_index_t*	index,	/*!< in: non-clustered index */
	const dtuple_t*	entry,	/*!< in: entry for a non-clustered index */
	ulint		space,	/*!< in: space id */
	ulint		page_no,/*!< in: index page number where entry should
				be inserted */
	ulint		counter,/*!< in: counter value;
				ULINT_UNDEFINED=not used */
	mem_heap_t*	heap)	/*!< in: heap into which to build */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	const dfield_t*	entry_field;
	ulint		n_fields;
	byte*		buf;
	byte*		ti;
	byte*		type_info;
	ulint		i;

	ut_ad(counter != ULINT_UNDEFINED || op == IBUF_OP_INSERT);
	ut_ad(counter == ULINT_UNDEFINED || counter <= 0xFFFF);
	ut_ad(op < IBUF_OP_COUNT);

	/* We have to build a tuple with the following fields:

	1-4) These are described at the top of this file.

	5) The rest of the fields are copied from the entry.

	All fields in the tuple are ordered like the type binary in our
	insert buffer tree. */

	n_fields = dtuple_get_n_fields(entry);

	tuple = dtuple_create(heap, n_fields + IBUF_REC_FIELD_USER);

	/* 1) Space Id */

	field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_SPACE);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, space);

	dfield_set_data(field, buf, 4);

	/* 2) Marker byte */

	field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_MARKER);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 1));

	/* We set the marker byte zero */

	mach_write_to_1(buf, 0);

	dfield_set_data(field, buf, 1);

	/* 3) Page number */

	field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_PAGE);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, page_no);

	dfield_set_data(field, buf, 4);

	/* 4) Type info, part #1 */

	if (counter == ULINT_UNDEFINED) {
		i = dict_table_is_comp(index->table) ? 1 : 0;
	} else {
		ut_ad(counter <= 0xFFFF);
		i = IBUF_REC_INFO_SIZE;
	}

	ti = type_info = static_cast<byte*>(
		mem_heap_alloc(
			heap,
			i + n_fields * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE));

	switch (i) {
	default:
		ut_error;
		break;
	case 1:
		/* set the flag for ROW_FORMAT=COMPACT */
		*ti++ = 0;
		/* fall through */
	case 0:
		/* the old format does not allow delete buffering */
		ut_ad(op == IBUF_OP_INSERT);
		break;
	case IBUF_REC_INFO_SIZE:
		mach_write_to_2(ti + IBUF_REC_OFFSET_COUNTER, counter);

		ti[IBUF_REC_OFFSET_TYPE] = (byte) op;
		ti[IBUF_REC_OFFSET_FLAGS] = dict_table_is_comp(index->table)
			? IBUF_REC_COMPACT : 0;
		ti += IBUF_REC_INFO_SIZE;
		break;
	}

	/* 5+) Fields from the entry */

	for (i = 0; i < n_fields; i++) {
		ulint			fixed_len;
		const dict_field_t*	ifield;

		field = dtuple_get_nth_field(tuple, i + IBUF_REC_FIELD_USER);
		entry_field = dtuple_get_nth_field(entry, i);
		dfield_copy(field, entry_field);

		ifield = dict_index_get_nth_field(index, i);
		ut_ad(!ifield->descending);
		/* Prefix index columns of fixed-length columns are of
		fixed length.  However, in the function call below,
		dfield_get_type(entry_field) contains the fixed length
		of the column in the clustered index.  Replace it with
		the fixed length of the secondary index column. */
		fixed_len = ifield->fixed_len;

#ifdef UNIV_DEBUG
		if (fixed_len) {
			/* dict_index_add_col() should guarantee these */
			ut_ad(fixed_len <= (ulint)
			      dfield_get_type(entry_field)->len);
			if (ifield->prefix_len) {
				ut_ad(ifield->prefix_len == fixed_len);
			} else {
				ut_ad(fixed_len == (ulint)
				      dfield_get_type(entry_field)->len);
			}
		}
#endif /* UNIV_DEBUG */

		dtype_new_store_for_order_and_null_size(
			ti, dfield_get_type(entry_field), fixed_len);
		ti += DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE;
	}

	/* 4) Type info, part #2 */

	field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_METADATA);

	dfield_set_data(field, type_info, ulint(ti - type_info));

	/* Set all the types in the new tuple binary */

	dtuple_set_types_binary(tuple, n_fields + IBUF_REC_FIELD_USER);

	return(tuple);
}

/*********************************************************************//**
Builds a search tuple used to search buffered inserts for an index page.
This is for >= 4.1.x format records.
@return own: search tuple */
static
dtuple_t*
ibuf_search_tuple_build(
/*====================*/
	ulint		space,	/*!< in: space id */
	ulint		page_no,/*!< in: index page number */
	mem_heap_t*	heap)	/*!< in: heap into which to build */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	byte*		buf;

	tuple = dtuple_create(heap, IBUF_REC_FIELD_METADATA);

	/* Store the space id in tuple */

	field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_SPACE);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, space);

	dfield_set_data(field, buf, 4);

	/* Store the new format record marker byte */

	field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_MARKER);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 1));

	mach_write_to_1(buf, 0);

	dfield_set_data(field, buf, 1);

	/* Store the page number in tuple */

	field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_PAGE);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, page_no);

	dfield_set_data(field, buf, 4);

	dtuple_set_types_binary(tuple, IBUF_REC_FIELD_METADATA);

	return(tuple);
}

/*********************************************************************//**
Checks if there are enough pages in the free list of the ibuf tree that we
dare to start a pessimistic insert to the insert buffer.
@return whether enough free pages in list */
static inline bool ibuf_data_enough_free_for_insert()
{
	mysql_mutex_assert_owner(&ibuf_mutex);

	/* We want a big margin of free pages, because a B-tree can sometimes
	grow in size also if records are deleted from it, as the node pointers
	can change, and we must make sure that we are able to delete the
	inserts buffered for pages that we read to the buffer pool, without
	any risk of running out of free space in the insert buffer. */

	return(ibuf.free_list_len >= (ibuf.size / 2) + 3 * ibuf.height);
}

/*********************************************************************//**
Checks if there are enough pages in the free list of the ibuf tree that we
should remove them and free to the file space management.
@return TRUE if enough free pages in list */
UNIV_INLINE
ibool
ibuf_data_too_much_free(void)
/*=========================*/
{
	mysql_mutex_assert_owner(&ibuf_mutex);

	return(ibuf.free_list_len >= 3 + (ibuf.size / 2) + 3 * ibuf.height);
}

/** Allocate a change buffer page.
@retval true on success
@retval false if no space left */
static bool ibuf_add_free_page()
{
	mtr_t		mtr;
	page_t*		header_page;
	buf_block_t*	block;

	mtr.start();
	/* Acquire the fsp latch before the ibuf header, obeying the latching
	order */
	mtr.x_lock_space(fil_system.sys_space);
	header_page = ibuf_header_page_get(&mtr);
	if (!header_page) {
		mtr.commit();
		return false;
	}

	/* Allocate a new page: NOTE that if the page has been a part of a
	non-clustered index which has subsequently been dropped, then the
	page may have buffered inserts in the insert buffer, and these
	should be deleted from there. These get deleted when the page
	allocation creates the page in buffer. Thus the call below may end
	up calling the insert buffer routines and, as we yet have no latches
	to insert buffer tree pages, these routines can run without a risk
	of a deadlock. This is the reason why we created a special ibuf
	header page apart from the ibuf tree. */

	dberr_t err;
	block = fseg_alloc_free_page_general(
		header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER, 0, FSP_UP,
		false, &mtr, &mtr, &err);

	if (!block) {
		mtr.commit();
		return false;
	}

	ut_ad(block->page.lock.not_recursive());
	ibuf_enter(&mtr);
	mysql_mutex_lock(&ibuf_mutex);

	mtr.write<2>(*block, block->page.frame + FIL_PAGE_TYPE,
		     FIL_PAGE_IBUF_FREE_LIST);
	buf_block_t* ibuf_root = ibuf_tree_root_get(&mtr);
	if (UNIV_UNLIKELY(!ibuf_root)) {
corrupted:
		/* Do not bother to try to free the allocated block, because
		the change buffer is seriously corrupted already. */
		mysql_mutex_unlock(&ibuf_mutex);
		ibuf_mtr_commit(&mtr);
		return false;
	}

	/* Add the page to the free list and update the ibuf size data */

	err = flst_add_last(ibuf_root, PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
			    block, PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE,
			    fil_system.sys_space->free_limit, &mtr);
	if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
		goto corrupted;
	}

	/* Set the bit indicating that this page is now an ibuf tree page
	(level 2 page) */

	const page_id_t page_id(block->page.id());
	buf_block_t* bitmap_page = ibuf_bitmap_get_map_page(page_id, 0, &mtr);

	if (UNIV_UNLIKELY(!bitmap_page)) {
		goto corrupted;
	}

	ibuf.seg_size++;
	ibuf.free_list_len++;

	mysql_mutex_unlock(&ibuf_mutex);

	ibuf_bitmap_page_set_bits<IBUF_BITMAP_IBUF>(bitmap_page, page_id,
						    srv_page_size, true, &mtr);
	ibuf_mtr_commit(&mtr);
	return true;
}

/*********************************************************************//**
Removes a page from the free list and frees it to the fsp system. */
static void ibuf_remove_free_page()
{
	mtr_t	mtr;
	page_t*	header_page;

	log_free_check();

	mtr_start(&mtr);
	/* Acquire the fsp latch before the ibuf header, obeying the latching
	order */

	mtr.x_lock_space(fil_system.sys_space);
	header_page = ibuf_header_page_get(&mtr);

	/* Prevent pessimistic inserts to insert buffer trees for a while */
	ibuf_enter(&mtr);
	mysql_mutex_lock(&ibuf_pessimistic_insert_mutex);
	mysql_mutex_lock(&ibuf_mutex);

	if (!header_page || !ibuf_data_too_much_free()) {
early_exit:
		mysql_mutex_unlock(&ibuf_mutex);
		mysql_mutex_unlock(&ibuf_pessimistic_insert_mutex);

		ibuf_mtr_commit(&mtr);

		return;
	}

	buf_block_t* root = ibuf_tree_root_get(&mtr);

	if (UNIV_UNLIKELY(!root)) {
		goto early_exit;
	}

	const auto root_savepoint = mtr.get_savepoint() - 1;
	const uint32_t page_no = flst_get_last(PAGE_HEADER
					       + PAGE_BTR_IBUF_FREE_LIST
					       + root->page.frame).page;

	if (page_no >= fil_system.sys_space->free_limit) {
		goto early_exit;
	}

	mysql_mutex_unlock(&ibuf_mutex);

	/* NOTE that we must release the latch on the ibuf tree root
	because in fseg_free_page we access level 1 pages, and the root
	is a level 2 page. */
	root->page.lock.u_unlock();
	mtr.lock_register(root_savepoint, MTR_MEMO_BUF_FIX);
	ibuf_exit(&mtr);

	/* Since pessimistic inserts were prevented, we know that the
	page is still in the free list. NOTE that also deletes may take
	pages from the free list, but they take them from the start, and
	the free list was so long that they cannot have taken the last
	page from it. */

	compile_time_assert(IBUF_SPACE_ID == 0);
	const page_id_t	page_id{IBUF_SPACE_ID, page_no};
	buf_block_t* bitmap_page = nullptr;
	dberr_t err = fseg_free_page(
		header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER,
		fil_system.sys_space, page_no, &mtr);

	if (err != DB_SUCCESS) {
		goto func_exit;
	}

	ibuf_enter(&mtr);

	mysql_mutex_lock(&ibuf_mutex);
	mtr.upgrade_buffer_fix(root_savepoint, RW_X_LATCH);

	/* Remove the page from the free list and update the ibuf size data */
	if (buf_block_t* block =
	    buf_page_get_gen(page_id, 0, RW_X_LATCH, nullptr, BUF_GET,
			     &mtr, &err)) {
		err = flst_remove(root, PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
				  block,
				  PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE,
				  fil_system.sys_space->free_limit, &mtr);
	}

	mysql_mutex_unlock(&ibuf_pessimistic_insert_mutex);

	if (err == DB_SUCCESS) {
		ibuf.seg_size--;
		ibuf.free_list_len--;
		bitmap_page = ibuf_bitmap_get_map_page(page_id, 0, &mtr);
	}

func_exit:
	mysql_mutex_unlock(&ibuf_mutex);

	if (bitmap_page) {
		/* Set the bit indicating that this page is no more an
		ibuf tree page (level 2 page) */
		ibuf_bitmap_page_set_bits<IBUF_BITMAP_IBUF>(
			bitmap_page, page_id, srv_page_size, false, &mtr);
	}

	if (err == DB_SUCCESS) {
		buf_page_free(fil_system.sys_space, page_no, &mtr);
	}

	ibuf_mtr_commit(&mtr);
}

/***********************************************************************//**
Frees excess pages from the ibuf free list. This function is called when an OS
thread calls fsp services to allocate a new file segment, or a new page to a
file segment, and the thread did not own the fsp latch before this call. */
void
ibuf_free_excess_pages(void)
/*========================*/
{
	if (UNIV_UNLIKELY(!ibuf.index)) return;
	/* Free at most a few pages at a time, so that we do not delay the
	requested service too much */

	for (ulint i = 0; i < 4; i++) {

		ibool	too_much_free;

		mysql_mutex_lock(&ibuf_mutex);
		too_much_free = ibuf_data_too_much_free();
		mysql_mutex_unlock(&ibuf_mutex);

		if (!too_much_free) {
			return;
		}

		ibuf_remove_free_page();
	}
}

#ifdef UNIV_DEBUG
# define ibuf_get_merge_page_nos(rec,mtr,ids,pages,n_stored) \
	ibuf_get_merge_page_nos_func(rec,mtr,ids,pages,n_stored)
#else /* UNIV_DEBUG */
# define ibuf_get_merge_page_nos(rec,mtr,ids,pages,n_stored) \
	ibuf_get_merge_page_nos_func(rec,ids,pages,n_stored)
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Reads page numbers from a leaf in an ibuf tree.
@return a lower limit for the combined volume of records which will be
merged */
static
ulint
ibuf_get_merge_page_nos_func(
/*=========================*/
	const btr_cur_t&cur,	/*!< in: insert buffer record */
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction holding rec */
#endif /* UNIV_DEBUG */
	uint32_t*	space_ids,/*!< in/out: space id's of the pages */
	uint32_t*	page_nos,/*!< in/out: buffer for at least
				IBUF_MAX_N_PAGES_MERGED many page numbers;
				the page numbers are in an ascending order */
	ulint*		n_stored)/*!< out: number of page numbers stored to
				page_nos in this function */
{
	uint32_t prev_page_no;
	uint32_t prev_space_id;
	uint32_t first_page_no;
	uint32_t first_space_id;
	uint32_t rec_page_no;
	uint32_t rec_space_id;
	ulint	sum_volumes;
	ulint	volume_for_page;
	ulint	rec_volume;
	ulint	limit;
	ulint	n_pages;
	const rec_t* rec= btr_cur_get_rec(&cur);
	const page_t* page= btr_cur_get_page(&cur);

	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));

	*n_stored = 0;

	if (page_rec_is_supremum_low(rec - page)) {

		rec = page_rec_get_prev_const(rec);
		if (UNIV_UNLIKELY(!rec)) {
corruption:
			ut_ad("corrupted page" == 0);
			return 0;
		}
	}

	if (page_rec_is_infimum_low(rec - page)) {
		rec = page_rec_next_get<false>(page, rec);
		if (!rec || page_rec_is_supremum_low(rec - page)) {
			return 0;
		}
	}

	limit = std::min(IBUF_MAX_N_PAGES_MERGED, buf_pool.curr_size() / 4);

	first_page_no = ibuf_rec_get_page_no(mtr, rec);
	first_space_id = ibuf_rec_get_space(mtr, rec);
	n_pages = 0;
	prev_page_no = 0;
	prev_space_id = 0;

	/* Go backwards from the first rec until we reach the border of the
	'merge area', or the page start or the limit of storeable pages is
	reached */

	while (!page_rec_is_infimum_low(rec - page)
	       && UNIV_LIKELY(n_pages < limit)) {

		rec_page_no = ibuf_rec_get_page_no(mtr, rec);
		rec_space_id = ibuf_rec_get_space(mtr, rec);

		if (rec_space_id != first_space_id
		    || (rec_page_no / IBUF_MERGE_AREA)
		    != (first_page_no / IBUF_MERGE_AREA)) {

			break;
		}

		if (rec_page_no != prev_page_no
		    || rec_space_id != prev_space_id) {
			n_pages++;
		}

		prev_page_no = rec_page_no;
		prev_space_id = rec_space_id;

		if (UNIV_UNLIKELY(!(rec = page_rec_get_prev_const(rec)))) {
			goto corruption;
		}
	}

	rec = page_rec_next_get<false>(page, rec);

	/* At the loop start there is no prev page; we mark this with a pair
	of space id, page no (0, 0) for which there can never be entries in
	the insert buffer */

	prev_page_no = 0;
	prev_space_id = 0;
	sum_volumes = 0;
	volume_for_page = 0;

	while (*n_stored < limit && rec) {
		if (page_rec_is_supremum_low(rec - page)) {
			/* When no more records available, mark this with
			another 'impossible' pair of space id, page no */
			rec_page_no = 1;
			rec_space_id = 0;
		} else {
			rec_page_no = ibuf_rec_get_page_no(mtr, rec);
			rec_space_id = ibuf_rec_get_space(mtr, rec);
			/* In the system tablespace the smallest
			possible secondary index leaf page number is
			bigger than FSP_DICT_HDR_PAGE_NO (7).
			In all tablespaces, pages 0 and 1 are reserved
			for the allocation bitmap and the change
			buffer bitmap. In file-per-table tablespaces,
			a file segment inode page will be created at
			page 2 and the clustered index tree is created
			at page 3.  So for file-per-table tablespaces,
			page 4 is the smallest possible secondary
			index leaf page. CREATE TABLESPACE also initially
			uses pages 2 and 3 for the first created table,
			but that table may be dropped, allowing page 2
			to be reused for a secondary index leaf page.
			To keep this assertion simple, just
			make sure the page is >= 2. */
			ut_ad(rec_page_no >= FSP_FIRST_INODE_PAGE_NO);
		}

#ifdef UNIV_IBUF_DEBUG
		ut_a(*n_stored < IBUF_MAX_N_PAGES_MERGED);
#endif
		if ((rec_space_id != prev_space_id
		     || rec_page_no != prev_page_no)
		    && (prev_space_id != 0 || prev_page_no != 0)) {

			space_ids[*n_stored] = prev_space_id;
			page_nos[*n_stored] = prev_page_no;
			(*n_stored)++;
			sum_volumes += volume_for_page;

			if (rec_space_id != first_space_id
			    || rec_page_no / IBUF_MERGE_AREA
			    != first_page_no / IBUF_MERGE_AREA) {

				break;
			}

			volume_for_page = 0;
		}

		if (rec_page_no == 1 && rec_space_id == 0) {
			/* Supremum record */

			break;
		}

		rec_volume = ibuf_rec_get_volume(mtr, rec);

		volume_for_page += rec_volume;

		prev_page_no = rec_page_no;
		prev_space_id = rec_space_id;

		rec = page_rec_next_get<false>(page, rec);
	}

#ifdef UNIV_IBUF_DEBUG
	ut_a(*n_stored <= IBUF_MAX_N_PAGES_MERGED);
#endif
#if 0
	fprintf(stderr, "Ibuf merge batch %lu pages %lu volume\n",
		*n_stored, sum_volumes);
#endif
	return(sum_volumes);
}

/*******************************************************************//**
Get the matching records for space id.
@return current rec or NULL */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
const rec_t*
ibuf_get_user_rec(
/*===============*/
	btr_pcur_t*	pcur,		/*!< in: the current cursor */
	mtr_t*		mtr)		/*!< in: mini transaction */
{
	do {
		const rec_t* rec = btr_pcur_get_rec(pcur);

		if (page_rec_is_user_rec(rec)) {
			return(rec);
		}
	} while (btr_pcur_move_to_next(pcur, mtr));

	return(NULL);
}

/*********************************************************************//**
Reads page numbers for a space id from an ibuf tree.
@return a lower limit for the combined volume of records which will be
merged */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
ulint
ibuf_get_merge_pages(
/*=================*/
	btr_pcur_t*	pcur,	/*!< in/out: cursor */
	uint32_t	space,	/*!< in: space for which to merge */
	ulint		limit,	/*!< in: max page numbers to read */
	uint32_t*	pages,	/*!< out: pages read */
	uint32_t*	spaces,	/*!< out: spaces read */
	ulint*		n_pages,/*!< out: number of pages read */
	mtr_t*		mtr)	/*!< in: mini transaction */
{
	const rec_t*	rec;
	ulint		volume = 0;

	*n_pages = 0;

	while ((rec = ibuf_get_user_rec(pcur, mtr)) != 0
	       && ibuf_rec_get_space(mtr, rec) == space
	       && *n_pages < limit) {

		uint32_t page_no = ibuf_rec_get_page_no(mtr, rec);

		if (*n_pages == 0 || pages[*n_pages - 1] != page_no) {
			spaces[*n_pages] = space;
			pages[*n_pages] = page_no;
			++*n_pages;
		}

		volume += ibuf_rec_get_volume(mtr, rec);

		btr_pcur_move_to_next(pcur, mtr);
	}

	return(volume);
}

/**
Delete a change buffer record.
@param[in]	page_id		page identifier
@param[in,out]	pcur		persistent cursor positioned on the record
@param[in]	search_tuple	search key for (space,page_no)
@param[in,out]	mtr		mini-transaction
@return whether mtr was committed (due to pessimistic operation) */
static MY_ATTRIBUTE((warn_unused_result, nonnull))
bool ibuf_delete_rec(const page_id_t page_id, btr_pcur_t* pcur,
		     const dtuple_t* search_tuple, mtr_t* mtr);

/** Delete the change buffer records for the given page id
@param page_id page identifier */
static void ibuf_delete_recs(const page_id_t page_id)
{
  if (!ibuf.index || srv_read_only_mode)
    return;
  dfield_t dfield[IBUF_REC_FIELD_METADATA];
  dtuple_t tuple {0,IBUF_REC_FIELD_METADATA,IBUF_REC_FIELD_METADATA,
                  dfield,0,nullptr
#ifdef UNIV_DEBUG
                  ,DATA_TUPLE_MAGIC_N
#endif
  };
  byte space_id[4], page_no[4];

  mach_write_to_4(space_id, page_id.space());
  mach_write_to_4(page_no, page_id.page_no());

  dfield_set_data(&dfield[0], space_id, 4);
  dfield_set_data(&dfield[1], field_ref_zero, 1);
  dfield_set_data(&dfield[2], page_no, 4);
  dtuple_set_types_binary(&tuple, IBUF_REC_FIELD_METADATA);

  mtr_t mtr;
loop:
  btr_pcur_t pcur;
  pcur.btr_cur.page_cur.index= ibuf.index;
  ibuf_mtr_start(&mtr);
  if (btr_pcur_open(&tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF, &pcur, &mtr))
    goto func_exit;
  if (!btr_pcur_is_on_user_rec(&pcur))
  {
    ut_ad(btr_pcur_is_after_last_on_page(&pcur));
    goto func_exit;
  }

  for (;;)
  {
    ut_ad(btr_pcur_is_on_user_rec(&pcur));
    const rec_t* ibuf_rec = btr_pcur_get_rec(&pcur);
    if (ibuf_rec_get_space(&mtr, ibuf_rec) != page_id.space()
        || ibuf_rec_get_page_no(&mtr, ibuf_rec) != page_id.page_no())
      break;
    /* Delete the record from ibuf */
    if (ibuf_delete_rec(page_id, &pcur, &tuple, &mtr))
    {
      /* Deletion was pessimistic and mtr was committed:
      we start from the beginning again */
      ut_ad(mtr.has_committed());
      goto loop;
    }

    if (btr_pcur_is_after_last_on_page(&pcur))
    {
      ibuf_mtr_commit(&mtr);
      btr_pcur_close(&pcur);
      goto loop;
    }
  }
func_exit:
  ibuf_mtr_commit(&mtr);
  btr_pcur_close(&pcur);
}

/** Merge the change buffer to some pages. */
static void ibuf_read_merge_pages(const uint32_t* space_ids,
				  const uint32_t* page_nos, ulint n_stored,
				  bool slow_shutdown_cleanup)
{
	for (ulint i = 0; i < n_stored; i++) {
		const uint32_t space_id = space_ids[i];
		fil_space_t* s = fil_space_t::get(space_id);
		if (!s) {
tablespace_deleted:
			/* The tablespace was not found: remove all
			entries for it */
			ibuf_delete_for_discarded_space(space_id);
			while (i + 1 < n_stored
			       && space_ids[i + 1] == space_id) {
				i++;
			}
			continue;
		}

		const ulint zip_size = s->zip_size(), size = s->size;
		s->x_lock();
		s->release();
		mtr_t mtr;

		if (UNIV_LIKELY(page_nos[i] < size)) {
			mtr.start();
			dberr_t err;
			/* Load the page and apply change buffers. */
			std::ignore =
			buf_page_get_gen(page_id_t(space_id, page_nos[i]),
					 zip_size, RW_X_LATCH, nullptr,
					 BUF_GET_POSSIBLY_FREED,
					 &mtr, &err, true);
			mtr.commit();
			if (err == DB_TABLESPACE_DELETED) {
				s->x_unlock();
				goto tablespace_deleted;
			}
		}

		s->x_unlock();

		/* During slow shutdown cleanup, we apply all pending IBUF
		changes and need to cleanup any left-over IBUF records. There
		are a few cases when the changes are already discarded and IBUF
		bitmap is cleaned but we miss to delete the record. Even after
		the issues are fixed, we need to keep this safety measure for
		upgraded DBs with such left over records. */
		if (!slow_shutdown_cleanup) {
			continue;
		}

		/* The following code works around a hang when the
		change buffer is corrupted, likely due to the
		failure of ibuf_merge_or_delete_for_page() to
		invoke ibuf_delete_recs() if (!bitmap_bits).

		It also introduced corruption by itself in the
		following scenario:

		(1) We merged buffered changes in buf_page_get_gen()
		(2) We committed the mini-transaction
		(3) Redo log and the page with the merged changes is written
		(4) A write completion callback thread evicts the page.
		(5) Other threads buffer changes for that page.
		(6) We will wrongly discard those newly buffered changes below.

		To prevent this scenario, we will only invoke this code
		on shutdown. A call to ibuf_max_size_update(0) will cause
		ibuf_insert_low() to refuse to insert anything into the
		change buffer. */

		/* Prevent an infinite loop, by removing entries from
		the change buffer in the case the bitmap bits were
		wrongly clear even though buffered changes exist. */
		ibuf_delete_recs(page_id_t(space_id, page_nos[i]));
	}
}

/** Contract the change buffer by reading pages to the buffer pool.
@return a lower limit for the combined size in bytes of entries which
will be merged from ibuf trees to the pages read
@retval 0 if ibuf.empty */
ATTRIBUTE_COLD ulint ibuf_contract()
{
	if (UNIV_UNLIKELY(!ibuf.index)) return 0;
	mtr_t		mtr;
	btr_cur_t	cur;
	ulint		sum_sizes;
	uint32_t	page_nos[IBUF_MAX_N_PAGES_MERGED];
	uint32_t	space_ids[IBUF_MAX_N_PAGES_MERGED];

	ibuf_mtr_start(&mtr);

	if (cur.open_leaf(true, ibuf.index, BTR_SEARCH_LEAF, &mtr) !=
	    DB_SUCCESS) {
		return 0;
	}

	ut_ad(page_validate(btr_cur_get_page(&cur), ibuf.index));

	if (page_is_empty(btr_cur_get_page(&cur))) {
		/* If a B-tree page is empty, it must be the root page
		and the whole B-tree must be empty. InnoDB does not
		allow empty B-tree pages other than the root. */
		ut_ad(ibuf.empty);
		ut_ad(btr_cur_get_block(&cur)->page.id()
		      == page_id_t(IBUF_SPACE_ID, FSP_IBUF_TREE_ROOT_PAGE_NO));

		ibuf_mtr_commit(&mtr);

		return(0);
	}

	ulint n_pages = 0;
	sum_sizes = ibuf_get_merge_page_nos(cur, &mtr,
					    space_ids, page_nos, &n_pages);
	ibuf_mtr_commit(&mtr);

	ibuf_read_merge_pages(space_ids, page_nos, n_pages, true);

	return(sum_sizes + 1);
}

/*********************************************************************//**
Contracts insert buffer trees by reading pages referring to space_id
to the buffer pool.
@returns number of pages merged.*/
ulint
ibuf_merge_space(
/*=============*/
	ulint		space)	/*!< in: tablespace id to merge */
{
	if (UNIV_UNLIKELY(!ibuf.index)) return 0;
	mtr_t		mtr;
	btr_pcur_t	pcur;

	dfield_t dfield[IBUF_REC_FIELD_METADATA];
	dtuple_t tuple {0, IBUF_REC_FIELD_METADATA,
			IBUF_REC_FIELD_METADATA,dfield,0,nullptr
#ifdef UNIV_DEBUG
			, DATA_TUPLE_MAGIC_N
#endif
	};
	byte space_id[4];

	mach_write_to_4(space_id, space);

	dfield_set_data(&dfield[0], space_id, 4);
	dfield_set_data(&dfield[1], field_ref_zero, 1);
	dfield_set_data(&dfield[2], field_ref_zero, 4);

	dtuple_set_types_binary(&tuple, IBUF_REC_FIELD_METADATA);
	ulint		n_pages = 0;

	ut_ad(space < SRV_SPACE_ID_UPPER_BOUND);

	log_free_check();
	ibuf_mtr_start(&mtr);

	/* Position the cursor on the first matching record. */

	pcur.btr_cur.page_cur.index = ibuf.index;
	dberr_t err = btr_pcur_open(&tuple, PAGE_CUR_GE, BTR_SEARCH_LEAF,
				    &pcur, &mtr);
	ut_ad(err != DB_SUCCESS || page_validate(btr_pcur_get_page(&pcur),
						 ibuf.index));

	ulint		sum_sizes = 0;
	uint32_t	pages[IBUF_MAX_N_PAGES_MERGED];
	uint32_t	spaces[IBUF_MAX_N_PAGES_MERGED];

	if (err != DB_SUCCESS) {
	} else if (page_is_empty(btr_pcur_get_page(&pcur))) {
		/* If a B-tree page is empty, it must be the root page
		and the whole B-tree must be empty. InnoDB does not
		allow empty B-tree pages other than the root. */
		ut_ad(ibuf.empty);
		ut_ad(btr_pcur_get_block(&pcur)->page.id()
		      == page_id_t(IBUF_SPACE_ID, FSP_IBUF_TREE_ROOT_PAGE_NO));
	} else {

		sum_sizes = ibuf_get_merge_pages(
			&pcur, uint32_t(space), IBUF_MAX_N_PAGES_MERGED,
			&pages[0], &spaces[0], &n_pages,
			&mtr);
		ib::info() << "Size of pages merged " << sum_sizes;
	}

	ibuf_mtr_commit(&mtr);

	if (n_pages > 0) {
		ut_ad(n_pages <= UT_ARR_SIZE(pages));

#ifdef UNIV_DEBUG
		for (ulint i = 0; i < n_pages; ++i) {
			ut_ad(spaces[i] == space);
		}
#endif /* UNIV_DEBUG */

		ibuf_read_merge_pages(spaces, pages, n_pages, false);
	}

	return(n_pages);
}

/** Determine if a change buffer record has been encountered already.
@param rec   change buffer record in the MySQL 5.5 format
@param hash  hash table of encountered records
@param size  number of elements in hash
@retval true if a distinct record
@retval false if this may be duplicating an earlier record */
static bool ibuf_get_volume_buffered_hash(const rec_t *rec, ulint *hash,
                                          ulint size)
{
  ut_ad(rec_get_n_fields_old(rec) > IBUF_REC_FIELD_USER);
  const ulint start= rec_get_field_start_offs(rec, IBUF_REC_FIELD_USER);
  const ulint len= rec_get_data_size_old(rec) - start;
  const uint32_t fold= my_crc32c(0, rec + start, len);
  hash+= (fold / (CHAR_BIT * sizeof *hash)) % size;
  ulint bitmask= static_cast<ulint>(1) << (fold % (CHAR_BIT * sizeof(*hash)));

  if (*hash & bitmask)
    return false;

  /* We have not seen this record yet. Remember it. */
  *hash|= bitmask;
  return true;
}

#ifdef UNIV_DEBUG
# define ibuf_get_volume_buffered_count(mtr,rec,hash,size,n_recs)	\
	ibuf_get_volume_buffered_count_func(mtr,rec,hash,size,n_recs)
#else /* UNIV_DEBUG */
# define ibuf_get_volume_buffered_count(mtr,rec,hash,size,n_recs)	\
	ibuf_get_volume_buffered_count_func(rec,hash,size,n_recs)
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Update the estimate of the number of records on a page, and
get the space taken by merging the buffered record to the index page.
@return size of index record in bytes + an upper limit of the space
taken in the page directory */
static
ulint
ibuf_get_volume_buffered_count_func(
/*================================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,	/*!< in: mini-transaction owning rec */
#endif /* UNIV_DEBUG */
	const rec_t*	rec,	/*!< in: insert buffer record */
	ulint*		hash,	/*!< in/out: hash array */
	ulint		size,	/*!< in: number of elements in hash array */
	lint*		n_recs)	/*!< in/out: estimated number of records
				on the page that rec points to */
{
	ulint		len;
	ibuf_op_t	ibuf_op;
	const byte*	types;
	ulint		n_fields;

	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(ibuf_inside(mtr));

	n_fields = rec_get_n_fields_old(rec);
	ut_ad(n_fields > IBUF_REC_FIELD_USER);
	n_fields -= IBUF_REC_FIELD_USER;

	rec_get_nth_field_offs_old(rec, 1, &len);
	/* This function is only invoked when buffering new
	operations.  All pre-4.1 records should have been merged
	when the database was started up. */
	ut_a(len == 1);

	if (rec_get_deleted_flag(rec, 0)) {
		/* This record has been merged already,
		but apparently the system crashed before
		the change was discarded from the buffer.
		Pretend that the record does not exist. */
		return(0);
	}

	types = rec_get_nth_field_old(rec, IBUF_REC_FIELD_METADATA, &len);

	switch (UNIV_EXPECT(int(len % DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE),
			    IBUF_REC_INFO_SIZE)) {
	default:
		ut_error;
	case 0:
		/* This ROW_TYPE=REDUNDANT record does not include an
		operation counter.  Exclude it from the *n_recs,
		because deletes cannot be buffered if there are
		old-style inserts buffered for the page. */

		len = ibuf_rec_get_size(rec, types, n_fields, 0);

		return(len
		       + rec_get_converted_extra_size(len, n_fields, 0)
		       + page_dir_calc_reserved_space(1));
	case 1:
		/* This ROW_TYPE=COMPACT record does not include an
		operation counter.  Exclude it from the *n_recs,
		because deletes cannot be buffered if there are
		old-style inserts buffered for the page. */
		goto get_volume_comp;

	case IBUF_REC_INFO_SIZE:
		ibuf_op = (ibuf_op_t) types[IBUF_REC_OFFSET_TYPE];
		break;
	}

	switch (ibuf_op) {
	case IBUF_OP_INSERT:
		/* Inserts can be done by updating a delete-marked record.
		Because delete-mark and insert operations can be pointing to
		the same records, we must not count duplicates. */
	case IBUF_OP_DELETE_MARK:
		/* There must be a record to delete-mark.
		See if this record has been already buffered. */
		if (n_recs && ibuf_get_volume_buffered_hash(rec, hash, size)) {
			(*n_recs)++;
		}

		if (ibuf_op == IBUF_OP_DELETE_MARK) {
			/* Setting the delete-mark flag does not
			affect the available space on the page. */
			return(0);
		}
		break;
	case IBUF_OP_DELETE:
		/* A record will be removed from the page. */
		if (n_recs) {
			(*n_recs)--;
		}
		/* While deleting a record actually frees up space,
		we have to play it safe and pretend that it takes no
		additional space (the record might not exist, etc.). */
		return(0);
	default:
		ut_error;
	}

	ut_ad(ibuf_op == IBUF_OP_INSERT);

get_volume_comp:
	{
		dtuple_t*	entry;
		ulint		volume;
		dict_index_t*	dummy_index;
		mem_heap_t*	heap = mem_heap_create(500);

		entry = ibuf_build_entry_from_ibuf_rec(
			mtr, rec, heap, &dummy_index);

		volume = rec_get_converted_size(dummy_index, entry, 0);

		ibuf_dummy_index_free(dummy_index);
		mem_heap_free(heap);

		return(volume + page_dir_calc_reserved_space(1));
	}
}

/*********************************************************************//**
Gets an upper limit for the combined size of entries buffered in the insert
buffer for a given page.
@return upper limit for the volume of buffered inserts for the index
page, in bytes; srv_page_size, if the entries for the index page span
several pages in the insert buffer */
static
ulint
ibuf_get_volume_buffered(
/*=====================*/
	const btr_pcur_t*pcur,	/*!< in: pcur positioned at a place in an
				insert buffer tree where we would insert an
				entry for the index page whose number is
				page_no, latch mode has to be BTR_MODIFY_PREV
				or BTR_MODIFY_TREE */
	ulint		space,	/*!< in: space id */
	ulint		page_no,/*!< in: page number of an index page */
	lint*		n_recs,	/*!< in/out: minimum number of records on the
				page after the buffered changes have been
				applied, or NULL to disable the counting */
	mtr_t*		mtr)	/*!< in: mini-transaction of pcur */
{
	ulint		volume;
	const rec_t*	rec;
	const page_t*	page;
	const page_t*	prev_page;
	const page_t*	next_page;
	/* bitmap of buffered recs */
	ulint		hash_bitmap[128 / sizeof(ulint)];

	ut_ad((pcur->latch_mode == BTR_MODIFY_PREV)
	      || (pcur->latch_mode == BTR_MODIFY_TREE));

	/* Count the volume of inserts earlier in the alphabetical order than
	pcur */

	volume = 0;

	if (n_recs) {
		memset(hash_bitmap, 0, sizeof hash_bitmap);
	}

	rec = btr_pcur_get_rec(pcur);
	page = btr_pcur_get_page(pcur);
	ut_ad(page_validate(page, ibuf.index));

	if (rec == page + PAGE_OLD_SUPREMUM
	    && UNIV_UNLIKELY(!(rec = page_rec_get_prev_const(rec)))) {
corruption:
		ut_ad("corrupted page" == 0);
		return srv_page_size;
	}

	uint32_t prev_page_no;

	while (rec != page + PAGE_OLD_INFIMUM) {
		ut_ad(page_align(rec) == page);

		if (page_no != ibuf_rec_get_page_no(mtr, rec)
		    || space != ibuf_rec_get_space(mtr, rec)) {

			goto count_later;
		}

		volume += ibuf_get_volume_buffered_count(
			mtr, rec,
			hash_bitmap, UT_ARR_SIZE(hash_bitmap), n_recs);

		if (UNIV_UNLIKELY(!(rec = page_rec_get_prev_const(rec)))) {
			goto corruption;
		}
	}

	/* Look at the previous page */

	prev_page_no = btr_page_get_prev(page);

	if (prev_page_no == FIL_NULL) {

		goto count_later;
	}

	if (buf_block_t* block =
	    buf_page_get(page_id_t(IBUF_SPACE_ID, prev_page_no),
			 0, RW_X_LATCH, mtr)) {
		prev_page = buf_block_get_frame(block);
		ut_ad(page_validate(prev_page, ibuf.index));
	} else {
		return srv_page_size;
	}

	static_assert(FIL_PAGE_NEXT % 4 == 0, "alignment");
	static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");

	if (UNIV_UNLIKELY(memcmp_aligned<4>(prev_page + FIL_PAGE_NEXT,
					    page + FIL_PAGE_OFFSET, 4))) {
		return srv_page_size;
	}

	rec = page_rec_get_prev_const(page_get_supremum_rec(prev_page));

	if (UNIV_UNLIKELY(!rec)) {
		goto corruption;
	}

	for (;;) {
		ut_ad(page_align(rec) == prev_page);

		if (page_rec_is_infimum(rec)) {

			/* We cannot go to yet a previous page, because we
			do not have the x-latch on it, and cannot acquire one
			because of the latching order: we have to give up */

			return(srv_page_size);
		}

		if (page_no != ibuf_rec_get_page_no(mtr, rec)
		    || space != ibuf_rec_get_space(mtr, rec)) {

			goto count_later;
		}

		volume += ibuf_get_volume_buffered_count(
			mtr, rec,
			hash_bitmap, UT_ARR_SIZE(hash_bitmap), n_recs);

		if (UNIV_UNLIKELY(!(rec = page_rec_get_prev_const(rec)))) {
			goto corruption;
		}
	}

count_later:
	rec = btr_pcur_get_rec(pcur);

	if (rec != page + PAGE_OLD_SUPREMUM) {
		rec = page_rec_next_get<false>(page, rec);
	}

	for (; rec != page + PAGE_OLD_SUPREMUM;
	     rec = page_rec_next_get<false>(page, rec)) {
		if (UNIV_UNLIKELY(!rec)) {
			return srv_page_size;
		}
		if (page_no != ibuf_rec_get_page_no(mtr, rec)
		    || space != ibuf_rec_get_space(mtr, rec)) {

			return(volume);
		}

		volume += ibuf_get_volume_buffered_count(
			mtr, rec,
			hash_bitmap, UT_ARR_SIZE(hash_bitmap), n_recs);
	}

	/* Look at the next page */

	uint32_t next_page_no = btr_page_get_next(page);

	if (next_page_no == FIL_NULL) {

		return(volume);
	}

	if (buf_block_t* block =
	    buf_page_get(page_id_t(IBUF_SPACE_ID, next_page_no),
			 0, RW_X_LATCH, mtr)) {
		next_page = buf_block_get_frame(block);
		ut_ad(page_validate(next_page, ibuf.index));
	} else {
		return srv_page_size;
	}

	static_assert(FIL_PAGE_PREV % 4 == 0, "alignment");
	static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");

	if (UNIV_UNLIKELY(memcmp_aligned<4>(next_page + FIL_PAGE_PREV,
					    page + FIL_PAGE_OFFSET, 4))) {
		return 0;
	}

	rec = page_rec_next_get<false>(next_page,
				       next_page + PAGE_OLD_INFIMUM);

	for (;; rec = page_rec_next_get<false>(next_page, rec)) {
		if (!rec || rec == next_page + PAGE_OLD_SUPREMUM) {
			/* We give up */
			return(srv_page_size);
		}

		ut_ad(page_align(rec) == next_page);

		if (page_no != ibuf_rec_get_page_no(mtr, rec)
		    || space != ibuf_rec_get_space(mtr, rec)) {

			return(volume);
		}

		volume += ibuf_get_volume_buffered_count(
			mtr, rec,
			hash_bitmap, UT_ARR_SIZE(hash_bitmap), n_recs);
	}
}

/*********************************************************************//**
Reads the biggest tablespace id from the high end of the insert buffer
tree and updates the counter in fil_system. */
void
ibuf_update_max_tablespace_id(void)
/*===============================*/
{
	if (UNIV_UNLIKELY(!ibuf.index)) return;
	const rec_t*	rec;
	const byte*	field;
	ulint		len;
	btr_pcur_t	pcur;
	mtr_t		mtr;

	ut_ad(!ibuf.index->table->not_redundant());

	ibuf_mtr_start(&mtr);

	if (pcur.open_leaf(false, ibuf.index, BTR_SEARCH_LEAF, &mtr)
	    != DB_SUCCESS) {
func_exit:
		ibuf_mtr_commit(&mtr);
		return;
	}

	ut_ad(page_validate(btr_pcur_get_page(&pcur), ibuf.index));

	if (!btr_pcur_move_to_prev(&pcur, &mtr)
	    || btr_pcur_is_before_first_on_page(&pcur)) {
		goto func_exit;
	}

	rec = btr_pcur_get_rec(&pcur);

	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_SPACE, &len);

	ut_a(len == 4);

	const uint32_t max_space_id = mach_read_from_4(field);

	ibuf_mtr_commit(&mtr);

	/* printf("Maximum space id in insert buffer %lu\n", max_space_id); */

	fil_set_max_space_id_if_bigger(max_space_id);
}

#ifdef UNIV_DEBUG
# define ibuf_get_entry_counter_low(mtr,rec,space,page_no)	\
	ibuf_get_entry_counter_low_func(mtr,rec,space,page_no)
#else /* UNIV_DEBUG */
# define ibuf_get_entry_counter_low(mtr,rec,space,page_no)	\
	ibuf_get_entry_counter_low_func(rec,space,page_no)
#endif
/****************************************************************//**
Helper function for ibuf_get_entry_counter_func. Checks if rec is for
(space, page_no), and if so, reads counter value from it and returns
that + 1.
@retval ULINT_UNDEFINED if the record does not contain any counter
@retval 0 if the record is not for (space, page_no)
@retval 1 + previous counter value, otherwise */
static
ulint
ibuf_get_entry_counter_low_func(
/*============================*/
#ifdef UNIV_DEBUG
	mtr_t*		mtr,		/*!< in: mini-transaction of rec */
#endif /* UNIV_DEBUG */
	const rec_t*	rec,		/*!< in: insert buffer record */
	ulint		space,		/*!< in: space id */
	ulint		page_no)	/*!< in: page number */
{
	ulint		counter;
	const byte*	field;
	ulint		len;

	ut_ad(ibuf_inside(mtr));
	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));
	ut_ad(rec_get_n_fields_old(rec) > 2);

	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_MARKER, &len);

	ut_a(len == 1);

	/* Check the tablespace identifier. */
	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_SPACE, &len);

	ut_a(len == 4);

	if (mach_read_from_4(field) != space) {

		return(0);
	}

	/* Check the page offset. */
	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_PAGE, &len);
	ut_a(len == 4);

	if (mach_read_from_4(field) != page_no) {

		return(0);
	}

	/* Check if the record contains a counter field. */
	field = rec_get_nth_field_old(rec, IBUF_REC_FIELD_METADATA, &len);

	switch (len % DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE) {
	default:
		ut_error;
	case 0: /* ROW_FORMAT=REDUNDANT */
	case 1: /* ROW_FORMAT=COMPACT */
		return(ULINT_UNDEFINED);

	case IBUF_REC_INFO_SIZE:
		counter = mach_read_from_2(field + IBUF_REC_OFFSET_COUNTER);
		ut_a(counter < 0xFFFF);
		return(counter + 1);
	}
}

#ifdef UNIV_DEBUG
# define ibuf_get_entry_counter(space,page_no,rec,mtr,exact_leaf) \
	ibuf_get_entry_counter_func(space,page_no,rec,mtr,exact_leaf)
#else /* UNIV_DEBUG */
# define ibuf_get_entry_counter(space,page_no,rec,mtr,exact_leaf) \
	ibuf_get_entry_counter_func(space,page_no,rec,exact_leaf)
#endif /* UNIV_DEBUG */

/****************************************************************//**
Calculate the counter field for an entry based on the current
last record in ibuf for (space, page_no).
@return the counter field, or ULINT_UNDEFINED
if we should abort this insertion to ibuf */
static
ulint
ibuf_get_entry_counter_func(
/*========================*/
	ulint		space,		/*!< in: space id of entry */
	ulint		page_no,	/*!< in: page number of entry */
	const rec_t*	rec,		/*!< in: the record preceding the
					insertion point */
#ifdef UNIV_DEBUG
	mtr_t*		mtr,		/*!< in: mini-transaction */
#endif /* UNIV_DEBUG */
	ibool		only_leaf)	/*!< in: TRUE if this is the only
					leaf page that can contain entries
					for (space,page_no), that is, there
					was no exact match for (space,page_no)
					in the node pointer */
{
	ut_ad(ibuf_inside(mtr));
	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX));
	ut_ad(page_validate(page_align(rec), ibuf.index));

	if (page_rec_is_supremum(rec)) {
		/* This is just for safety. The record should be a
		page infimum or a user record. */
		ut_ad(0);
		return(ULINT_UNDEFINED);
	} else if (!page_rec_is_infimum(rec)) {
		return(ibuf_get_entry_counter_low(mtr, rec, space, page_no));
	} else if (only_leaf || !page_has_prev(page_align(rec))) {
		/* The parent node pointer did not contain the
		searched for (space, page_no), which means that the
		search ended on the correct page regardless of the
		counter value, and since we're at the infimum record,
		there are no existing records. */
		return(0);
	} else {
		/* We used to read the previous page here. It would
		break the latching order, because the caller has
		buffer-fixed an insert buffer bitmap page. */
		return(ULINT_UNDEFINED);
	}
}


/** Translates the ibuf free bits to the free space on a page in bytes.
@param[in]	physical_size	page_size
@param[in]	bits		value for ibuf bitmap bits
@return maximum insert size after reorganize for the page */
inline ulint
ibuf_index_page_calc_free_from_bits(ulint physical_size, ulint bits)
{
	ut_ad(bits < 4);
	ut_ad(physical_size > IBUF_PAGE_SIZE_PER_FREE_SPACE);

	if (bits == 3) {
		bits = 4;
	}

	return bits * physical_size / IBUF_PAGE_SIZE_PER_FREE_SPACE;
}

/** Buffer an operation in the insert/delete buffer, instead of doing it
directly to the disk page, if this is possible.
@param[in]	mode		BTR_MODIFY_PREV or BTR_INSERT_TREE
@param[in]	op		operation type
@param[in]	no_counter	TRUE=use 5.0.3 format; FALSE=allow delete
buffering
@param[in]	entry		index entry to insert
@param[in]	entry_size	rec_get_converted_size(index, entry)
@param[in,out]	index		index where to insert; must not be unique
or clustered
@param[in]	page_id		page id where to insert
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	thr		query thread
@return DB_SUCCESS, DB_STRONG_FAIL or other error */
static TRANSACTIONAL_TARGET MY_ATTRIBUTE((warn_unused_result))
dberr_t
ibuf_insert_low(
	btr_latch_mode		mode,
	ibuf_op_t		op,
	ibool			no_counter,
	const dtuple_t*		entry,
	ulint			entry_size,
	dict_index_t*		index,
	const page_id_t		page_id,
	ulint			zip_size,
	que_thr_t*		thr)
{
	big_rec_t*	dummy_big_rec;
	btr_pcur_t	pcur;
	btr_cur_t*	cursor;
	dtuple_t*	ibuf_entry;
	mem_heap_t*	offsets_heap	= NULL;
	mem_heap_t*	heap;
	rec_offs*	offsets		= NULL;
	ulint		buffered;
	lint		min_n_recs;
	rec_t*		ins_rec;
	buf_block_t*	bitmap_page;
	buf_block_t*	block		= NULL;
	page_t*		root;
	dberr_t		err;
	mtr_t		mtr;
	mtr_t		bitmap_mtr;

	ut_a(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_spatial(index));
	ut_ad(dtuple_check_typed(entry));
	ut_ad(!no_counter || op == IBUF_OP_INSERT);
	ut_ad(page_id.space() == index->table->space_id);
	ut_a(op < IBUF_OP_COUNT);

	/* Perform dirty comparison of ibuf.max_size and ibuf.size to
	reduce ibuf_mutex contention. */
	if (ibuf.size >= ibuf.max_size) {
		return(DB_STRONG_FAIL);
	}

	heap = mem_heap_create(1024);

	/* Build the entry which contains the space id and the page number
	as the first fields and the type information for other fields, and
	which will be inserted to the insert buffer. Using a counter value
	of 0xFFFF we find the last record for (space, page_no), from which
	we can then read the counter value N and use N + 1 in the record we
	insert. (We patch the ibuf_entry's counter field to the correct
	value just before actually inserting the entry.) */

	ibuf_entry = ibuf_entry_build(
		op, index, entry, page_id.space(), page_id.page_no(),
		no_counter ? ULINT_UNDEFINED : 0xFFFF, heap);

	/* Open a cursor to the insert buffer tree to calculate if we can add
	the new entry to it without exceeding the free space limit for the
	page. */

	if (mode == BTR_INSERT_TREE) {
		for (;;) {
			mysql_mutex_lock(&ibuf_pessimistic_insert_mutex);
			mysql_mutex_lock(&ibuf_mutex);

			if (UNIV_LIKELY(ibuf_data_enough_free_for_insert())) {

				break;
			}

			mysql_mutex_unlock(&ibuf_mutex);
			mysql_mutex_unlock(&ibuf_pessimistic_insert_mutex);

			if (!ibuf_add_free_page()) {

				mem_heap_free(heap);
				return(DB_STRONG_FAIL);
			}
		}
	}

	ibuf_mtr_start(&mtr);
	pcur.btr_cur.page_cur.index = ibuf.index;

	err = btr_pcur_open(ibuf_entry, PAGE_CUR_LE, mode, &pcur, &mtr);
	if (err != DB_SUCCESS) {
func_exit:
		ibuf_mtr_commit(&mtr);
		ut_free(pcur.old_rec_buf);
		mem_heap_free(heap);
		return err;
	}

	ut_ad(page_validate(btr_pcur_get_page(&pcur), ibuf.index));

	/* Find out the volume of already buffered inserts for the same index
	page */
	min_n_recs = 0;
	buffered = ibuf_get_volume_buffered(&pcur,
					    page_id.space(),
					    page_id.page_no(),
					    op == IBUF_OP_DELETE
					    ? &min_n_recs
					    : NULL, &mtr);

	const ulint physical_size = zip_size ? zip_size : srv_page_size;

	if (op == IBUF_OP_DELETE
	    && (min_n_recs < 2 || buf_pool.watch_occurred(page_id))) {
		/* The page could become empty after the record is
		deleted, or the page has been read in to the buffer
		pool.  Refuse to buffer the operation. */

		/* The buffer pool watch is needed for IBUF_OP_DELETE
		because of latching order considerations.  We can
		check buf_pool_watch_occurred() only after latching
		the insert buffer B-tree pages that contain buffered
		changes for the page.  We never buffer IBUF_OP_DELETE,
		unless some IBUF_OP_INSERT or IBUF_OP_DELETE_MARK have
		been previously buffered for the page.  Because there
		are buffered operations for the page, the insert
		buffer B-tree page latches held by mtr will guarantee
		that no changes for the user page will be merged
		before mtr_commit(&mtr).  We must not mtr_commit(&mtr)
		until after the IBUF_OP_DELETE has been buffered. */

fail_exit:
		if (mode == BTR_INSERT_TREE) {
			mysql_mutex_unlock(&ibuf_mutex);
			mysql_mutex_unlock(&ibuf_pessimistic_insert_mutex);
		}

		err = DB_STRONG_FAIL;
		goto func_exit;
	}

	/* After this point, the page could still be loaded to the
	buffer pool, but we do not have to care about it, since we are
	holding a latch on the insert buffer leaf page that contains
	buffered changes for (space, page_no).  If the page enters the
	buffer pool, buf_page_t::read_complete() for (space, page_no) will
	have to acquire a latch on the same insert buffer leaf page,
	which it cannot do until we have buffered the IBUF_OP_DELETE
	and done mtr_commit(&mtr) to release the latch. */

	ibuf_mtr_start(&bitmap_mtr);

	bitmap_page = ibuf_bitmap_get_map_page(page_id, zip_size, &bitmap_mtr);

	/* We check if the index page is suitable for buffered entries */

	if (!bitmap_page || buf_pool.page_hash_contains(
		    page_id, buf_pool.page_hash.cell_get(page_id.fold()))) {
commit_exit:
		ibuf_mtr_commit(&bitmap_mtr);
		goto fail_exit;
	} else if (!lock_sys.rd_lock_try()) {
		goto commit_exit;
	} else {
		hash_cell_t* cell = lock_sys.rec_hash.cell_get(page_id.fold());
		lock_sys.rec_hash.latch(cell)->acquire();
		const lock_t* lock = lock_sys_t::get_first(*cell, page_id);
		lock_sys.rec_hash.latch(cell)->release();
		lock_sys.rd_unlock();
		if (lock) {
			goto commit_exit;
		}
	}

	if (op == IBUF_OP_INSERT) {
		ulint	bits = ibuf_bitmap_page_get_bits(
			bitmap_page->page.frame, page_id, physical_size,
			IBUF_BITMAP_FREE, &bitmap_mtr);

		if (buffered + entry_size + page_dir_calc_reserved_space(1)
		    > ibuf_index_page_calc_free_from_bits(physical_size,
							  bits)) {
			/* Release the bitmap page latch early. */
			ibuf_mtr_commit(&bitmap_mtr);
			goto fail_exit;
		}
	}

	if (!no_counter) {
		/* Patch correct counter value to the entry to
		insert. This can change the insert position, which can
		result in the need to abort in some cases. */
		ulint		counter = ibuf_get_entry_counter(
			page_id.space(), page_id.page_no(),
			btr_pcur_get_rec(&pcur), &mtr,
			btr_pcur_get_btr_cur(&pcur)->low_match
			< IBUF_REC_FIELD_METADATA);
		dfield_t*	field;

		if (counter == ULINT_UNDEFINED) {
			goto commit_exit;
		}

		field = dtuple_get_nth_field(
			ibuf_entry, IBUF_REC_FIELD_METADATA);
		mach_write_to_2(
			(byte*) dfield_get_data(field)
			+ IBUF_REC_OFFSET_COUNTER, counter);
	}

	/* Set the bitmap bit denoting that the insert buffer contains
	buffered entries for this index page, if the bit is not set yet */
	index->set_modified(bitmap_mtr);
	ibuf_bitmap_page_set_bits<IBUF_BITMAP_BUFFERED>(
		bitmap_page, page_id, physical_size, true, &bitmap_mtr);
	ibuf_mtr_commit(&bitmap_mtr);

	cursor = btr_pcur_get_btr_cur(&pcur);

	if (mode == BTR_MODIFY_PREV) {
		err = btr_cur_optimistic_insert(
			BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG,
			cursor, &offsets, &offsets_heap,
			ibuf_entry, &ins_rec,
			&dummy_big_rec, 0, thr, &mtr);
		block = btr_cur_get_block(cursor);
		ut_ad(block->page.id().space() == IBUF_SPACE_ID);

		/* If this is the root page, update ibuf.empty. */
		if (block->page.id().page_no() == FSP_IBUF_TREE_ROOT_PAGE_NO) {
			const page_t*	root = buf_block_get_frame(block);

			ut_ad(page_get_space_id(root) == IBUF_SPACE_ID);
			ut_ad(page_get_page_no(root)
			      == FSP_IBUF_TREE_ROOT_PAGE_NO);

			ibuf.empty = page_is_empty(root);
		}
	} else {
		ut_ad(mode == BTR_INSERT_TREE);

		/* We acquire an sx-latch to the root page before the insert,
		because a pessimistic insert releases the tree x-latch,
		which would cause the sx-latching of the root after that to
		break the latching order. */
		if (buf_block_t* ibuf_root = ibuf_tree_root_get(&mtr)) {
			root = ibuf_root->page.frame;
		} else {
			err = DB_CORRUPTION;
			mysql_mutex_unlock(&ibuf_pessimistic_insert_mutex);
			mysql_mutex_unlock(&ibuf_mutex);
			goto ibuf_insert_done;
		}

		err = btr_cur_optimistic_insert(
			BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG,
			cursor, &offsets, &offsets_heap,
			ibuf_entry, &ins_rec,
			&dummy_big_rec, 0, thr, &mtr);

		if (err == DB_FAIL) {
			err = btr_cur_pessimistic_insert(
				BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG,
				cursor, &offsets, &offsets_heap,
				ibuf_entry, &ins_rec,
				&dummy_big_rec, 0, thr, &mtr);
		}

		mysql_mutex_unlock(&ibuf_pessimistic_insert_mutex);
		ibuf_size_update(root);
		mysql_mutex_unlock(&ibuf_mutex);
		ibuf.empty = page_is_empty(root);

		block = btr_cur_get_block(cursor);
		ut_ad(block->page.id().space() == IBUF_SPACE_ID);
	}

ibuf_insert_done:
	if (offsets_heap) {
		mem_heap_free(offsets_heap);
	}

	if (err == DB_SUCCESS && op != IBUF_OP_DELETE) {
		/* Update the page max trx id field */
		page_update_max_trx_id(block, NULL,
				       thr_get_trx(thr)->id, &mtr);
	}

	goto func_exit;
}

/** Buffer an operation in the change buffer, instead of applying it
directly to the file page, if this is possible. Does not do it if the index
is clustered or unique.
@param[in]	op		operation type
@param[in]	entry		index entry to insert
@param[in,out]	index		index where to insert
@param[in]	page_id		page id where to insert
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	thr		query thread
@return true if success */
TRANSACTIONAL_TARGET
bool
ibuf_insert(
	ibuf_op_t		op,
	const dtuple_t*		entry,
	dict_index_t*		index,
	const page_id_t		page_id,
	ulint			zip_size,
	que_thr_t*		thr)
{
	if (!index->is_committed()) {
		return false;
	}

	dberr_t		err;
	ulint		entry_size;
	ibool		no_counter;
	/* Read the settable global variable only once in
	this function, so that we will have a consistent view of it. */
	ibuf_use_t	use		= ibuf_use_t(innodb_change_buffering);
	DBUG_ENTER("ibuf_insert");

	DBUG_PRINT("ibuf", ("op: %d, space: " UINT32PF ", page_no: " UINT32PF,
			    op, page_id.space(), page_id.page_no()));

	ut_ad(dtuple_check_typed(entry));
	ut_ad(page_id.space() != SRV_TMP_SPACE_ID);
	ut_ad(index->is_btree());
	ut_a(!dict_index_is_clust(index));
	ut_ad(!index->table->is_temporary());

	no_counter = use <= IBUF_USE_INSERT;

	switch (op) {
	case IBUF_OP_INSERT:
		switch (use) {
		case IBUF_USE_NONE:
		case IBUF_USE_DELETE:
		case IBUF_USE_DELETE_MARK:
			DBUG_RETURN(false);
		case IBUF_USE_INSERT:
		case IBUF_USE_INSERT_DELETE_MARK:
		case IBUF_USE_ALL:
			goto check_watch;
		}
		break;
	case IBUF_OP_DELETE_MARK:
		switch (use) {
		case IBUF_USE_NONE:
		case IBUF_USE_INSERT:
			DBUG_RETURN(false);
		case IBUF_USE_DELETE_MARK:
		case IBUF_USE_DELETE:
		case IBUF_USE_INSERT_DELETE_MARK:
		case IBUF_USE_ALL:
			ut_ad(!no_counter);
			goto check_watch;
		}
		break;
	case IBUF_OP_DELETE:
		switch (use) {
		case IBUF_USE_NONE:
		case IBUF_USE_INSERT:
		case IBUF_USE_INSERT_DELETE_MARK:
			DBUG_RETURN(false);
		case IBUF_USE_DELETE_MARK:
		case IBUF_USE_DELETE:
		case IBUF_USE_ALL:
			ut_ad(!no_counter);
			goto skip_watch;
		}
		break;
	case IBUF_OP_COUNT:
		break;
	}

	/* unknown op or use */
	ut_error;

check_watch:
	/* If a thread attempts to buffer an insert on a page while a
	purge is in progress on the same page, the purge must not be
	buffered, because it could remove a record that was
	re-inserted later.  For simplicity, we block the buffering of
	all operations on a page that has a purge pending.

	We do not check this in the IBUF_OP_DELETE case, because that
	would always trigger the buffer pool watch during purge and
	thus prevent the buffering of delete operations.  We assume
	that the issuer of IBUF_OP_DELETE has called
	buf_pool_t::watch_set(). */

	if (buf_pool.page_hash_contains<true>(
		    page_id, buf_pool.page_hash.cell_get(page_id.fold()))) {
		/* A buffer pool watch has been set or the
		page has been read into the buffer pool.
		Do not buffer the request.  If a purge operation
		is being buffered, have this request executed
		directly on the page in the buffer pool after the
		buffered entries for this page have been merged. */
		DBUG_RETURN(false);
	}

skip_watch:
	entry_size = rec_get_converted_size(index, entry, 0);

	if (entry_size
	    >= page_get_free_space_of_empty(dict_table_is_comp(index->table))
	    / 2) {

		DBUG_RETURN(false);
	}

	err = ibuf_insert_low(BTR_MODIFY_PREV, op, no_counter,
			      entry, entry_size,
			      index, page_id, zip_size, thr);
	if (err == DB_FAIL) {
		err = ibuf_insert_low(BTR_INSERT_TREE,
				      op, no_counter, entry, entry_size,
				      index, page_id, zip_size, thr);
	}

	ut_a(err == DB_SUCCESS || err == DB_STRONG_FAIL
	     || err == DB_TOO_BIG_RECORD);

	DBUG_RETURN(err == DB_SUCCESS);
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/********************************************************************//**
During merge, inserts to an index page a secondary index entry extracted
from the insert buffer.
@return	error code */
static
dberr_t
ibuf_insert_to_index_page_low(
/*==========================*/
	const dtuple_t*	entry,	/*!< in: buffered entry to insert */
	rec_offs**	offsets,/*!< out: offsets on *rec */
	mem_heap_t*	heap,	/*!< in/out: memory heap */
	mtr_t*		mtr,	/*!< in/out: mtr */
	page_cur_t*	page_cur)/*!< in/out: cursor positioned on the record
				after which to insert the buffered entry */
{
  if (page_cur_tuple_insert(page_cur, entry, offsets, &heap, 0, mtr))
    return DB_SUCCESS;

  /* Page reorganization or recompression should already have been
  attempted by page_cur_tuple_insert(). Besides, per
  ibuf_index_page_calc_free_zip() the page should not have been
  recompressed or reorganized. */
  ut_ad(!is_buf_block_get_page_zip(page_cur->block));

  /* If the record did not fit, reorganize */
  if (dberr_t err= btr_page_reorganize(page_cur, mtr))
    return err;

  /* This time the record must fit */
  if (page_cur_tuple_insert(page_cur, entry, offsets, &heap, 0, mtr))
    return DB_SUCCESS;

  return DB_CORRUPTION;
}

/************************************************************************
During merge, inserts to an index page a secondary index entry extracted
from the insert buffer. */
static
dberr_t
ibuf_insert_to_index_page(
/*======================*/
	const dtuple_t*	entry,	/*!< in: buffered entry to insert */
	buf_block_t*	block,	/*!< in/out: index page where the buffered entry
				should be placed */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_cur_t	page_cur;
	page_t*		page		= buf_block_get_frame(block);
	rec_t*		rec;
	rec_offs*	offsets;
	mem_heap_t*	heap;

	DBUG_PRINT("ibuf", ("page " UINT32PF ":" UINT32PF,
			    block->page.id().space(),
			    block->page.id().page_no()));

	ut_ad(!dict_index_is_online_ddl(index));// this is an ibuf_dummy index
	ut_ad(ibuf_inside(mtr));
	ut_ad(dtuple_check_typed(entry));
#ifdef BTR_CUR_HASH_ADAPT
	/* A change buffer merge must occur before users are granted
	any access to the page. No adaptive hash index entries may
	point to a freshly read page. */
	ut_ad(!block->index);
	assert_block_ahi_empty(block);
#endif /* BTR_CUR_HASH_ADAPT */
	ut_ad(mtr->is_named_space(block->page.id().space()));
        const auto comp = page_is_comp(page);

	if (UNIV_UNLIKELY(index->table->not_redundant() != !!comp)) {
		return DB_CORRUPTION;
	}

	if (comp) {
		rec = const_cast<rec_t*>(
			page_rec_next_get<true>(page,
						page + PAGE_NEW_INFIMUM));
		if (!rec || rec == page + PAGE_NEW_SUPREMUM) {
			return DB_CORRUPTION;
		}
	} else {
		rec = const_cast<rec_t*>(
			page_rec_next_get<false>(page,
						page + PAGE_OLD_INFIMUM));
		if (!rec || rec == page + PAGE_OLD_SUPREMUM) {
			return DB_CORRUPTION;
		}
	}

	if (!rec_n_fields_is_sane(index, rec, entry)) {
		return DB_CORRUPTION;
	}

	ulint up_match = 0, low_match = 0;
	page_cur.index = index;
	page_cur.block = block;

	if (page_cur_search_with_match(entry, PAGE_CUR_LE,
				       &up_match, &low_match, &page_cur,
				       nullptr)) {
		return DB_CORRUPTION;
	}

	dberr_t err = DB_SUCCESS;

	heap = mem_heap_create(
		sizeof(upd_t)
		+ REC_OFFS_HEADER_SIZE * sizeof(*offsets)
		+ dtuple_get_n_fields(entry)
		* (sizeof(upd_field_t) + sizeof *offsets));

	if (UNIV_UNLIKELY(low_match == dtuple_get_n_fields(entry))) {
		upd_t*		update;

		rec = page_cur_get_rec(&page_cur);

		/* This is based on
		row_ins_sec_index_entry_by_modify(BTR_MODIFY_LEAF). */
		ut_ad(rec_get_deleted_flag(rec, page_is_comp(page)));

		offsets = rec_get_offsets(rec, index, NULL, index->n_fields,
					  ULINT_UNDEFINED, &heap);
		update = row_upd_build_sec_rec_difference_binary(
			rec, index, offsets, entry, heap);

		if (update->n_fields == 0) {
			/* The records only differ in the delete-mark.
			Clear the delete-mark, like we did before
			Bug #56680 was fixed. */
			btr_rec_set_deleted<false>(block, rec, mtr);
			goto updated_in_place;
		}

		/* Copy the info bits. Clear the delete-mark. */
		update->info_bits = rec_get_info_bits(rec, page_is_comp(page));
		update->info_bits &= byte(~REC_INFO_DELETED_FLAG);
		page_zip_des_t* page_zip = buf_block_get_page_zip(block);

		/* We cannot invoke btr_cur_optimistic_update() here,
		because we do not have a btr_cur_t or que_thr_t,
		as the insert buffer merge occurs at a very low level. */
		if (!row_upd_changes_field_size_or_external(index, offsets,
							    update)
		    && (!page_zip || btr_cur_update_alloc_zip(
				page_zip, &page_cur, offsets,
				rec_offs_size(offsets), false, mtr))) {
			/* This is the easy case. Do something similar
			to btr_cur_update_in_place(). */
			rec = page_cur_get_rec(&page_cur);
			btr_cur_upd_rec_in_place(rec, index, offsets,
						 update, block, mtr);

			DBUG_EXECUTE_IF(
				"crash_after_log_ibuf_upd_inplace",
				log_buffer_flush_to_disk();
				ib::info() << "Wrote log record for ibuf"
					" update in place operation";
				DBUG_SUICIDE();
			);

			goto updated_in_place;
		}

		/* btr_cur_update_alloc_zip() may have changed this */
		rec = page_cur_get_rec(&page_cur);

		/* A collation may identify values that differ in
		storage length.
		Some examples (1 or 2 bytes):
		utf8_turkish_ci: I = U+0131 LATIN SMALL LETTER DOTLESS I
		utf8_general_ci: S = U+00DF LATIN SMALL LETTER SHARP S
		utf8_general_ci: A = U+00E4 LATIN SMALL LETTER A WITH DIAERESIS

		latin1_german2_ci: SS = U+00DF LATIN SMALL LETTER SHARP S

		Examples of a character (3-byte UTF-8 sequence)
		identified with 2 or 4 characters (1-byte UTF-8 sequences):

		utf8_unicode_ci: 'II' = U+2171 SMALL ROMAN NUMERAL TWO
		utf8_unicode_ci: '(10)' = U+247D PARENTHESIZED NUMBER TEN
		*/

		/* Delete the different-length record, and insert the
		buffered one. */

		page_cur_delete_rec(&page_cur, offsets, mtr);
		if (!(page_cur_move_to_prev(&page_cur))) {
			err = DB_CORRUPTION;
			goto updated_in_place;
		}
	} else {
		offsets = NULL;
	}

	err = ibuf_insert_to_index_page_low(entry, &offsets, heap, mtr,
                                            &page_cur);
updated_in_place:
	mem_heap_free(heap);

	return err;
}

/****************************************************************//**
During merge, sets the delete mark on a record for a secondary index
entry. */
static
void
ibuf_set_del_mark(
/*==============*/
	const dtuple_t*		entry,	/*!< in: entry */
	buf_block_t*		block,	/*!< in/out: block */
	dict_index_t*		index,	/*!< in: record descriptor */
	mtr_t*			mtr)	/*!< in: mtr */
{
	page_cur_t	page_cur;
	page_cur.block = block;
	page_cur.index = index;
	ulint		up_match = 0, low_match = 0;

	ut_ad(ibuf_inside(mtr));
	ut_ad(dtuple_check_typed(entry));

	if (!page_cur_search_with_match(entry, PAGE_CUR_LE,
					&up_match, &low_match, &page_cur,
					nullptr)
	    && low_match == dtuple_get_n_fields(entry)) {
		rec_t* rec = page_cur_get_rec(&page_cur);

		/* Delete mark the old index record. According to a
		comment in row_upd_sec_index_entry(), it can already
		have been delete marked if a lock wait occurred in
		row_ins_sec_index_entry() in a previous invocation of
		row_upd_sec_index_entry(). */

		if (UNIV_LIKELY
		    (!rec_get_deleted_flag(
			    rec, dict_table_is_comp(index->table)))) {
			btr_rec_set_deleted<true>(block, rec, mtr);
		}
	} else {
		const page_t*		page
			= page_cur_get_page(&page_cur);
		const buf_block_t*	block
			= page_cur_get_block(&page_cur);

		ib::error() << "Unable to find a record to delete-mark";
		fputs("InnoDB: tuple ", stderr);
		dtuple_print(stderr, entry);
		fputs("\n"
		      "InnoDB: record ", stderr);
		rec_print(stderr, page_cur_get_rec(&page_cur), index);

		ib::error() << "page " << block->page.id() << " ("
			<< page_get_n_recs(page) << " records, index id "
			<< btr_page_get_index_id(page) << ").";

		ib::error() << BUG_REPORT_MSG;
		ut_ad(0);
	}
}

/****************************************************************//**
During merge, delete a record for a secondary index entry. */
static
void
ibuf_delete(
/*========*/
	const dtuple_t*	entry,	/*!< in: entry */
	buf_block_t*	block,	/*!< in/out: block */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in/out: mtr; must be committed
				before latching any further pages */
{
	page_cur_t	page_cur;
	page_cur.block = block;
	page_cur.index = index;
	ulint		up_match = 0, low_match = 0;

	ut_ad(ibuf_inside(mtr));
	ut_ad(dtuple_check_typed(entry));
	ut_ad(!index->is_spatial());
	ut_ad(!index->is_clust());

	if (!page_cur_search_with_match(entry, PAGE_CUR_LE,
					&up_match, &low_match, &page_cur,
					nullptr)
	    && low_match == dtuple_get_n_fields(entry)) {
		page_zip_des_t*	page_zip= buf_block_get_page_zip(block);
		page_t*		page	= buf_block_get_frame(block);
		rec_t*		rec	= page_cur_get_rec(&page_cur);

		/* TODO: the below should probably be a separate function,
		it's a bastardized version of btr_cur_optimistic_delete. */

		rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
		rec_offs*	offsets	= offsets_;
		mem_heap_t*	heap = NULL;
		ulint		max_ins_size = 0;

		rec_offs_init(offsets_);

		offsets = rec_get_offsets(rec, index, offsets, index->n_fields,
					  ULINT_UNDEFINED, &heap);

		if (page_get_n_recs(page) <= 1
		    || !(REC_INFO_DELETED_FLAG
			 & rec_get_info_bits(rec, page_is_comp(page)))) {
			/* Refuse to purge the last record or a
			record that has not been marked for deletion. */
			ib::error() << "Unable to purge a record";
			fputs("InnoDB: tuple ", stderr);
			dtuple_print(stderr, entry);
			fputs("\n"
			      "InnoDB: record ", stderr);
			rec_print_new(stderr, rec, offsets);
			fprintf(stderr, "\nspace " UINT32PF " offset " UINT32PF
				" (%u records, index id %llu)\n"
				"InnoDB: Submit a detailed bug report"
				" to https://jira.mariadb.org/\n",
				block->page.id().space(),
				block->page.id().page_no(),
				(unsigned) page_get_n_recs(page),
				(ulonglong) btr_page_get_index_id(page));

			ut_ad(0);
			return;
		}

		if (!page_zip) {
			max_ins_size
				= page_get_max_insert_size_after_reorganize(
					page, 1);
		}
#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
		page_cur_delete_rec(&page_cur, offsets, mtr);
#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

		if (page_zip) {
			ibuf_update_free_bits_zip(block, mtr);
		} else {
			ibuf_update_free_bits_low(block, max_ins_size, mtr);
		}

		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
	}
}

/*********************************************************************//**
Restores insert buffer tree cursor position
@return whether the position was restored */
static MY_ATTRIBUTE((nonnull))
bool
ibuf_restore_pos(
/*=============*/
	const page_id_t	page_id,/*!< in: page identifier */
	const dtuple_t*	search_tuple,
				/*!< in: search tuple for entries of page_no */
	btr_latch_mode	mode,	/*!< in: BTR_MODIFY_LEAF or BTR_PURGE_TREE */
	btr_pcur_t*	pcur,	/*!< in/out: persistent cursor whose
				position is to be restored */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	if (UNIV_LIKELY(pcur->restore_position(mode, mtr) ==
	      btr_pcur_t::SAME_ALL)) {
		return true;
	}

	if (fil_space_t* s = fil_space_t::get(page_id.space())) {
		ib::error() << "ibuf cursor restoration fails!"
			" ibuf record inserted to page "
			<< page_id
			<< " in file " << s->chain.start->name;
		s->release();

		ib::error() << BUG_REPORT_MSG;

		rec_print_old(stderr, btr_pcur_get_rec(pcur));
		rec_print_old(stderr, pcur->old_rec);
		dtuple_print(stderr, search_tuple);
	}

	ibuf_btr_pcur_commit_specify_mtr(pcur, mtr);
	return false;
}

/**
Delete a change buffer record.
@param[in]	page_id		page identifier
@param[in,out]	pcur		persistent cursor positioned on the record
@param[in]	search_tuple	search key for (space,page_no)
@param[in,out]	mtr		mini-transaction
@return whether mtr was committed (due to pessimistic operation) */
static MY_ATTRIBUTE((warn_unused_result, nonnull))
bool ibuf_delete_rec(const page_id_t page_id, btr_pcur_t* pcur,
		     const dtuple_t* search_tuple, mtr_t* mtr)
{
	dberr_t		err;

	ut_ad(ibuf_inside(mtr));
	ut_ad(page_rec_is_user_rec(btr_pcur_get_rec(pcur)));
	ut_ad(ibuf_rec_get_page_no(mtr, btr_pcur_get_rec(pcur))
	      == page_id.page_no());
	ut_ad(ibuf_rec_get_space(mtr, btr_pcur_get_rec(pcur))
	      == page_id.space());

	switch (btr_cur_optimistic_delete(btr_pcur_get_btr_cur(pcur),
					  BTR_CREATE_FLAG, mtr)) {
	case DB_FAIL:
		break;
	case DB_SUCCESS:
		if (page_is_empty(btr_pcur_get_page(pcur))) {
			/* If a B-tree page is empty, it must be the root page
			and the whole B-tree must be empty. InnoDB does not
			allow empty B-tree pages other than the root. */
			ut_d(const page_t* root = btr_pcur_get_page(pcur));

			ut_ad(page_get_space_id(root) == IBUF_SPACE_ID);
			ut_ad(page_get_page_no(root)
			      == FSP_IBUF_TREE_ROOT_PAGE_NO);

			/* ibuf.empty is protected by the root page latch.
			Before the deletion, it had to be FALSE. */
			ut_ad(!ibuf.empty);
			ibuf.empty = true;
		}
		/* fall through */
	default:
		return(FALSE);
	}

	/* We have to resort to a pessimistic delete from ibuf.
	Delete-mark the record so that it will not be applied again,
	in case the server crashes before the pessimistic delete is
	made persistent. */
	btr_rec_set_deleted<true>(btr_pcur_get_block(pcur),
				  btr_pcur_get_rec(pcur), mtr);

	btr_pcur_store_position(pcur, mtr);
	ibuf_btr_pcur_commit_specify_mtr(pcur, mtr);

	ibuf_mtr_start(mtr);
	mysql_mutex_lock(&ibuf_mutex);
	mtr_x_lock_index(ibuf.index, mtr);

	if (!ibuf_restore_pos(page_id, search_tuple,
			      BTR_PURGE_TREE_ALREADY_LATCHED, pcur, mtr)) {
		mysql_mutex_unlock(&ibuf_mutex);
		goto func_exit;
	}

	if (buf_block_t* ibuf_root = ibuf_tree_root_get(mtr)) {
		btr_cur_pessimistic_delete(&err, TRUE,
					   btr_pcur_get_btr_cur(pcur),
					   BTR_CREATE_FLAG, false, mtr);
		ut_a(err == DB_SUCCESS);

		ibuf_size_update(ibuf_root->page.frame);
		ibuf.empty = page_is_empty(ibuf_root->page.frame);
	}

	mysql_mutex_unlock(&ibuf_mutex);
	ibuf_btr_pcur_commit_specify_mtr(pcur, mtr);

func_exit:
	ut_ad(mtr->has_committed());
	btr_pcur_close(pcur);

	return(TRUE);
}

/** Check whether buffered changes exist for a page.
@param[in]	id		page identifier
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return whether buffered changes exist */
bool ibuf_page_exists(const page_id_t id, ulint zip_size)
{
	ut_ad(!fsp_is_system_temporary(id.space()));

	const ulint physical_size = zip_size ? zip_size : srv_page_size;

	if (ibuf_fixed_addr_page(id, physical_size)
	    || fsp_descr_page(id, physical_size)) {
		return false;
	}

	mtr_t mtr;
	bool bitmap_bits = false;

	ibuf_mtr_start(&mtr);
	if (const buf_block_t* bitmap_page = ibuf_bitmap_get_map_page(
		    id, zip_size, &mtr)) {
		bitmap_bits = ibuf_bitmap_page_get_bits(
			bitmap_page->page.frame, id, zip_size,
			IBUF_BITMAP_BUFFERED, &mtr) != 0;
	}
	ibuf_mtr_commit(&mtr);
	return bitmap_bits;
}

/** Reset the bits in the bitmap page for the given block and page id.
@param b        X-latched secondary index page (nullptr to discard changes)
@param page_id  page identifier
@param zip_size ROW_FORMAT=COMPRESSED page size, or 0
@param mtr      mini-transaction */
static void ibuf_reset_bitmap(buf_block_t *b, page_id_t page_id,
                              ulint zip_size, mtr_t *mtr)
{
 buf_block_t *bitmap= ibuf_bitmap_get_map_page(page_id, zip_size, mtr);
 if (!bitmap)
   return;

 const ulint physical_size = zip_size ? zip_size : srv_page_size;
 /* FIXME: update the bitmap byte only once! */
 ibuf_bitmap_page_set_bits<IBUF_BITMAP_BUFFERED>(bitmap, page_id,
                                                 physical_size, false, mtr);

 if (b)
   ibuf_bitmap_page_set_bits<IBUF_BITMAP_FREE>(bitmap, page_id, physical_size,
                                               ibuf_index_page_calc_free(b),
                                               mtr);
}

/** When an index page is read from a disk to the buffer pool, this function
applies any buffered operations to the page and deletes the entries from the
insert buffer. If the page is not read, but created in the buffer pool, this
function deletes its buffered entries from the insert buffer; there can
exist entries for such a page if the page belonged to an index which
subsequently was dropped.
@param block    X-latched page to try to apply changes to, or NULL to discard
@param page_id  page identifier
@param zip_size ROW_FORMAT=COMPRESSED page size, or 0
@return error code */
dberr_t ibuf_merge_or_delete_for_page(buf_block_t *block,
                                      const page_id_t page_id,
                                      ulint zip_size)
{
	if (trx_sys_hdr_page(page_id)) {
		return DB_SUCCESS;
	}

	ut_ad(!block || page_id == block->page.id());
	ut_ad(!block || block->page.frame);
	ut_ad(!block || !block->page.is_ibuf_exist());
	ut_ad(!block || !block->page.is_reinit());
	ut_ad(!trx_sys_hdr_page(page_id));
	ut_ad(page_id < page_id_t(SRV_SPACE_ID_UPPER_BOUND, 0));

	const ulint physical_size = zip_size ? zip_size : srv_page_size;

	if (ibuf_fixed_addr_page(page_id, physical_size)
	    || fsp_descr_page(page_id, physical_size)) {
		return DB_SUCCESS;
	}

	btr_pcur_t	pcur;
#ifdef UNIV_IBUF_DEBUG
	ulint		volume			= 0;
#endif /* UNIV_IBUF_DEBUG */
	dberr_t		err = DB_SUCCESS;
	mtr_t		mtr;

	fil_space_t* space = fil_space_t::get(page_id.space());

	if (UNIV_UNLIKELY(!space)) {
		block = nullptr;
	} else {
		ulint	bitmap_bits = 0;

		ibuf_mtr_start(&mtr);

		buf_block_t* bitmap_page = ibuf_bitmap_get_map_page(
			page_id, zip_size, &mtr);

		if (bitmap_page
		    && fil_page_get_type(bitmap_page->page.frame)
		    != FIL_PAGE_TYPE_ALLOCATED) {
			bitmap_bits = ibuf_bitmap_page_get_bits(
				bitmap_page->page.frame, page_id, zip_size,
				IBUF_BITMAP_BUFFERED, &mtr);
		}

		ibuf_mtr_commit(&mtr);

		if (!bitmap_bits) {
		done:
			/* No changes are buffered for this page. */
			space->release();
			return DB_SUCCESS;
		}

		if (!block
		    || DB_SUCCESS
		    == fseg_page_is_allocated(space, page_id.page_no())) {
			ibuf_mtr_start(&mtr);
			mtr.set_named_space(space);
			ibuf_reset_bitmap(block, page_id, zip_size, &mtr);
			ibuf_mtr_commit(&mtr);
			if (!block
			    || btr_page_get_index_id(block->page.frame)
			    != DICT_IBUF_ID_MIN + IBUF_SPACE_ID) {
				ibuf_delete_recs(page_id);
			}
			goto done;
		}
	}

	if (!block) {
	} else if (!fil_page_index_page_check(block->page.frame)
		   || !page_is_leaf(block->page.frame)) {
		space->set_corrupted();
		err = DB_CORRUPTION;
		block = nullptr;
	} else {
		/* Move the ownership of the x-latch on the page to this OS
		thread, so that we can acquire a second x-latch on it. This
		is needed for the insert operations to the index page to pass
		the debug checks. */

		block->page.lock.claim_ownership();
	}

	mem_heap_t* heap = mem_heap_create(512);

	const dtuple_t* search_tuple = ibuf_search_tuple_build(
		page_id.space(), page_id.page_no(), heap);

	/* Counts for merged & discarded operations. */
	ulint mops[IBUF_OP_COUNT];
	ulint dops[IBUF_OP_COUNT];

	memset(mops, 0, sizeof(mops));
	memset(dops, 0, sizeof(dops));
	pcur.btr_cur.page_cur.index = ibuf.index;

loop:
	ibuf_mtr_start(&mtr);

	/* Position pcur in the insert buffer at the first entry for this
	index page */
	if (btr_pcur_open_on_user_rec(search_tuple,
				      BTR_MODIFY_LEAF, &pcur, &mtr)
	    != DB_SUCCESS) {
		err = DB_CORRUPTION;
		goto reset_bit;
	}

	if (block) {
		block->page.fix();
		block->page.lock.x_lock_recursive();
		mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);
	}

	if (space) {
		mtr.set_named_space(space);
	}

	if (!btr_pcur_is_on_user_rec(&pcur)) {
		ut_ad(btr_pcur_is_after_last_on_page(&pcur));
		goto reset_bit;
	}

	for (;;) {
		rec_t*	rec;

		ut_ad(btr_pcur_is_on_user_rec(&pcur));

		rec = btr_pcur_get_rec(&pcur);

		/* Check if the entry is for this index page */
		if (ibuf_rec_get_page_no(&mtr, rec) != page_id.page_no()
		    || ibuf_rec_get_space(&mtr, rec) != page_id.space()) {

			if (block != NULL) {
				page_header_reset_last_insert(block, &mtr);
			}

			goto reset_bit;
		}

		if (err) {
			fputs("InnoDB: Discarding record\n ", stderr);
			rec_print_old(stderr, rec);
			fputs("\nInnoDB: from the insert buffer!\n\n", stderr);
		} else if (block != NULL && !rec_get_deleted_flag(rec, 0)) {
			/* Now we have at pcur a record which should be
			applied on the index page; NOTE that the call below
			copies pointers to fields in rec, and we must
			keep the latch to the rec page until the
			insertion is finished! */
			dtuple_t*	entry;
			trx_id_t	max_trx_id;
			dict_index_t*	dummy_index;
			ibuf_op_t	op = ibuf_rec_get_op_type(&mtr, rec);

			max_trx_id =
				page_get_max_trx_id(btr_pcur_get_page(&pcur));
			page_update_max_trx_id(block,
					       buf_block_get_page_zip(block),
					       max_trx_id, &mtr);

			ut_ad(page_validate(page_align(rec), ibuf.index));

			entry = ibuf_build_entry_from_ibuf_rec(
				&mtr, rec, heap, &dummy_index);
			ut_ad(!dummy_index->table->space);
			dummy_index->table->space = space;
			dummy_index->table->space_id = space->id;

			ut_ad(page_validate(block->page.frame, dummy_index));

			switch (op) {
			case IBUF_OP_INSERT:
#ifdef UNIV_IBUF_DEBUG
				volume += rec_get_converted_size(
					dummy_index, entry, 0);

				volume += page_dir_calc_reserved_space(1);

				ut_a(volume <= (4U << srv_page_size_shift)
				     / IBUF_PAGE_SIZE_PER_FREE_SPACE);
#endif
				ibuf_insert_to_index_page(
					entry, block, dummy_index, &mtr);
				break;

			case IBUF_OP_DELETE_MARK:
				ibuf_set_del_mark(
					entry, block, dummy_index, &mtr);
				break;

			case IBUF_OP_DELETE:
				ibuf_delete(entry, block, dummy_index, &mtr);
				/* Because ibuf_delete() will latch an
				insert buffer bitmap page, commit mtr
				before latching any further pages.
				Store and restore the cursor position. */
				ut_ad(rec == btr_pcur_get_rec(&pcur));
				ut_ad(page_rec_is_user_rec(rec));
				ut_ad(ibuf_rec_get_page_no(&mtr, rec)
				      == page_id.page_no());
				ut_ad(ibuf_rec_get_space(&mtr, rec)
				      == page_id.space());

				/* Mark the change buffer record processed,
				so that it will not be merged again in case
				the server crashes between the following
				mtr_commit() and the subsequent mtr_commit()
				of deleting the change buffer record. */
				btr_rec_set_deleted<true>(
					btr_pcur_get_block(&pcur),
					btr_pcur_get_rec(&pcur), &mtr);

				btr_pcur_store_position(&pcur, &mtr);
				ibuf_btr_pcur_commit_specify_mtr(&pcur, &mtr);

				ibuf_mtr_start(&mtr);
				mtr.set_named_space(space);

				block->page.lock.x_lock_recursive();
				block->fix();
				mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);

				if (!ibuf_restore_pos(page_id, search_tuple,
						      BTR_MODIFY_LEAF,
						      &pcur, &mtr)) {

					ut_ad(mtr.has_committed());
					mops[op]++;
					ibuf_dummy_index_free(dummy_index);
					goto loop;
				}

				break;
			default:
				ut_error;
			}

			mops[op]++;

			ibuf_dummy_index_free(dummy_index);
		} else {
			dops[ibuf_rec_get_op_type(&mtr, rec)]++;
		}

		/* Delete the record from ibuf */
		if (ibuf_delete_rec(page_id, &pcur, search_tuple, &mtr)) {
			/* Deletion was pessimistic and mtr was committed:
			we start from the beginning again */

			ut_ad(mtr.has_committed());
			goto loop;
		} else if (btr_pcur_is_after_last_on_page(&pcur)) {
			ibuf_mtr_commit(&mtr);
			goto loop;
		}
	}

reset_bit:
	if (space) {
		ibuf_reset_bitmap(block, page_id, zip_size, &mtr);
	}

	ibuf_mtr_commit(&mtr);
	ut_free(pcur.old_rec_buf);

	if (space) {
		space->release();
	}

	mem_heap_free(heap);

	ibuf.n_merges++;
	ibuf_add_ops(ibuf.n_merged_ops, mops);
	ibuf_add_ops(ibuf.n_discarded_ops, dops);

	return err;
}

/** Delete all change buffer entries for a tablespace,
in DISCARD TABLESPACE, IMPORT TABLESPACE, or read-ahead.
@param[in]	space		missing or to-be-discarded tablespace */
void ibuf_delete_for_discarded_space(uint32_t space)
{
	if (UNIV_UNLIKELY(!ibuf.index)) return;

	btr_pcur_t	pcur;
	const rec_t*	ibuf_rec;
	mtr_t		mtr;

	/* Counts for discarded operations. */
	ulint		dops[IBUF_OP_COUNT];

	dfield_t	dfield[IBUF_REC_FIELD_METADATA];
	dtuple_t	search_tuple {0,IBUF_REC_FIELD_METADATA,
				      IBUF_REC_FIELD_METADATA,dfield,0
				      ,nullptr
#ifdef UNIV_DEBUG
				      ,DATA_TUPLE_MAGIC_N
#endif /* UNIV_DEBUG */
	};
	byte space_id[4];
	mach_write_to_4(space_id, space);

	dfield_set_data(&dfield[0], space_id, 4);
	dfield_set_data(&dfield[1], field_ref_zero, 1);
	dfield_set_data(&dfield[2], field_ref_zero, 4);
	dtuple_set_types_binary(&search_tuple, IBUF_REC_FIELD_METADATA);
	/* Use page number 0 to build the search tuple so that we get the
	cursor positioned at the first entry for this space id */

	memset(dops, 0, sizeof(dops));
	pcur.btr_cur.page_cur.index = ibuf.index;

loop:
	log_free_check();
	ibuf_mtr_start(&mtr);

	/* Position pcur in the insert buffer at the first entry for the
	space */
	if (btr_pcur_open_on_user_rec(&search_tuple,
				      BTR_MODIFY_LEAF, &pcur, &mtr)
	    != DB_SUCCESS) {
		goto leave_loop;
	}

	if (!btr_pcur_is_on_user_rec(&pcur)) {
		ut_ad(btr_pcur_is_after_last_on_page(&pcur));
		goto leave_loop;
	}

	for (;;) {
		ut_ad(btr_pcur_is_on_user_rec(&pcur));

		ibuf_rec = btr_pcur_get_rec(&pcur);

		/* Check if the entry is for this space */
		if (ibuf_rec_get_space(&mtr, ibuf_rec) != space) {

			goto leave_loop;
		}

		uint32_t page_no = ibuf_rec_get_page_no(&mtr, ibuf_rec);

		dops[ibuf_rec_get_op_type(&mtr, ibuf_rec)]++;

		/* Delete the record from ibuf */
		if (ibuf_delete_rec(page_id_t(space, page_no),
				    &pcur, &search_tuple, &mtr)) {
			/* Deletion was pessimistic and mtr was committed:
			we start from the beginning again */

			ut_ad(mtr.has_committed());
clear:
			ut_free(pcur.old_rec_buf);
			goto loop;
		}

		if (btr_pcur_is_after_last_on_page(&pcur)) {
			ibuf_mtr_commit(&mtr);
			goto clear;
		}
	}

leave_loop:
	ibuf_mtr_commit(&mtr);
	ut_free(pcur.old_rec_buf);

	ibuf_add_ops(ibuf.n_discarded_ops, dops);
}

/******************************************************************//**
Looks if the insert buffer is empty.
@return true if empty */
bool
ibuf_is_empty(void)
/*===============*/
{
	mtr_t		mtr;

	ibuf_mtr_start(&mtr);

	ut_d(mysql_mutex_lock(&ibuf_mutex));
	const buf_block_t* root = ibuf_tree_root_get(&mtr);
	bool is_empty = root && page_is_empty(root->page.frame);
	ut_ad(!root || is_empty == ibuf.empty);
	ut_d(mysql_mutex_unlock(&ibuf_mutex));
	ibuf_mtr_commit(&mtr);

	return(is_empty);
}

/******************************************************************//**
Prints info of ibuf. */
void
ibuf_print(
/*=======*/
	FILE*	file)	/*!< in: file where to print */
{
  if (UNIV_UNLIKELY(!ibuf.index)) return;

  mysql_mutex_lock(&ibuf_mutex);
  if (ibuf.empty)
  {
    mysql_mutex_unlock(&ibuf_mutex);
    return;
  }

  const uint32_t size= ibuf.size;
  const uint32_t free_list_len= ibuf.free_list_len;
  const uint32_t seg_size= ibuf.seg_size;
  mysql_mutex_unlock(&ibuf_mutex);

  fprintf(file,
          "-------------\n"
          "INSERT BUFFER\n"
          "-------------\n"
          "size %" PRIu32 ", free list len %" PRIu32 ","
          " seg size %" PRIu32 ", " ULINTPF " merges\n",
          size, free_list_len, seg_size, ulint{ibuf.n_merges});
  ibuf_print_ops("merged operations:\n", ibuf.n_merged_ops, file);
  ibuf_print_ops("discarded operations:\n", ibuf.n_discarded_ops, file);
}

/** Check the insert buffer bitmaps on IMPORT TABLESPACE.
@param[in]	trx	transaction
@param[in,out]	space	tablespace being imported
@return DB_SUCCESS or error code */
dberr_t ibuf_check_bitmap_on_import(const trx_t* trx, fil_space_t* space)
{
	ut_ad(trx->mysql_thd);
	ut_ad(space->is_being_imported());

	const unsigned zip_size = space->zip_size();
	const unsigned physical_size = space->physical_size();

	uint32_t size= std::min(space->free_limit, space->size);

	if (size == 0) {
		return(DB_TABLE_NOT_FOUND);
	}

	mtr_t mtr;

	/* The two bitmap pages (allocation bitmap and ibuf bitmap) repeat
	every page_size pages. For example if page_size is 16 KiB, then the
	two bitmap pages repeat every 16 KiB * 16384 = 256 MiB. In the loop
	below page_no is measured in number of pages since the beginning of
	the space, as usual. */

	for (uint32_t page_no = 0; page_no < size; page_no += physical_size) {
		if (trx_is_interrupted(trx)) {
			return(DB_INTERRUPTED);
		}

		mtr_start(&mtr);

		buf_block_t* bitmap_page = ibuf_bitmap_get_map_page(
			page_id_t(space->id, page_no), zip_size, &mtr);
		if (!bitmap_page) {
			mtr.commit();
			return DB_CORRUPTION;
		}

		if (buf_is_zeroes(span<const byte>(bitmap_page->page.frame,
						   physical_size))) {
			/* This means we got all-zero page instead of
			ibuf bitmap page. The subsequent page should be
			all-zero pages. */
#ifdef UNIV_DEBUG
			for (uint32_t curr_page = page_no + 1;
			     curr_page < physical_size; curr_page++) {

				buf_block_t* block = buf_page_get(
					page_id_t(space->id, curr_page),
					zip_size, RW_S_LATCH, &mtr);
				page_t*	page = buf_block_get_frame(block);
				ut_ad(buf_is_zeroes(span<const byte>(
							    page,
							    physical_size)));
			}
#endif /* UNIV_DEBUG */
			mtr_commit(&mtr);
			continue;
		}

		for (uint32_t i = FSP_IBUF_BITMAP_OFFSET + 1; i < physical_size;
		     i++) {
			const uint32_t offset = page_no + i;
			const page_id_t	cur_page_id(space->id, offset);

			if (ibuf_bitmap_page_get_bits(
				    bitmap_page->page.frame,
				    cur_page_id, zip_size,
				    IBUF_BITMAP_IBUF, &mtr)) {

				mtr_commit(&mtr);

				ib_errf(trx->mysql_thd,
					IB_LOG_LEVEL_ERROR,
					 ER_INNODB_INDEX_CORRUPT,
					 "File %s page %u"
					 " is wrongly flagged to belong to the"
					 " insert buffer",
					space->chain.start->name, offset);
				return(DB_CORRUPTION);
			}

			if (ibuf_bitmap_page_get_bits(
				    bitmap_page->page.frame,
				    cur_page_id, zip_size,
				    IBUF_BITMAP_BUFFERED, &mtr)) {

				ib_errf(trx->mysql_thd,
					IB_LOG_LEVEL_WARN,
					ER_INNODB_INDEX_CORRUPT,
					"Buffered changes"
					" for file %s page %u are lost",
					space->chain.start->name, offset);

				/* Tolerate this error, so that
				slightly corrupted tables can be
				imported and dumped.  Clear the bit. */
				ibuf_bitmap_page_set_bits<IBUF_BITMAP_BUFFERED>(
					bitmap_page, cur_page_id,
					physical_size, false, &mtr);
			}
		}

		mtr_commit(&mtr);
	}

	return(DB_SUCCESS);
}

void ibuf_set_bitmap_for_bulk_load(buf_block_t *block, mtr_t *mtr, bool reset)
{
  ut_a(page_is_leaf(block->page.frame));
  const page_id_t id{block->page.id()};
  const auto zip_size= block->zip_size();

  if (buf_block_t *bitmap_page= ibuf_bitmap_get_map_page(id, zip_size, mtr))
  {
    if (ibuf_bitmap_page_get_bits(bitmap_page->page.frame, id, zip_size,
                                  IBUF_BITMAP_BUFFERED, mtr))
      ibuf_delete_recs(id);

    ulint free_val= reset ? 0 : ibuf_index_page_calc_free(block);
    /* FIXME: update the bitmap byte only once! */
    ibuf_bitmap_page_set_bits<IBUF_BITMAP_FREE>
      (bitmap_page, id, block->physical_size(), free_val, mtr);
    ibuf_bitmap_page_set_bits<IBUF_BITMAP_BUFFERED>
      (bitmap_page, id, block->physical_size(), false, mtr);
  }
}
