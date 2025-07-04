/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2022, MariaDB Corporation.

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
@file include/trx0trx.h
The transaction

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#ifndef trx0trx_h
#define trx0trx_h

#include "trx0types.h"
#include "lock0types.h"
#include "que0types.h"
#include "mem0mem.h"
#include "trx0xa.h"
#include "fts0fts.h"
#include "read0types.h"
#include "ilist.h"
#include "small_vector.h"
#include "row0merge.h"

#include <vector>

// Forward declaration
struct mtr_t;
struct rw_trx_hash_element_t;

/******************************************************************//**
Set detailed error message for the transaction. */
void
trx_set_detailed_error(
/*===================*/
	trx_t*		trx,	/*!< in: transaction struct */
	const char*	msg);	/*!< in: detailed error message */
/*************************************************************//**
Set detailed error message for the transaction from a file. Note that the
file is rewinded before reading from it. */
void
trx_set_detailed_error_from_file(
/*=============================*/
	trx_t*	trx,	/*!< in: transaction struct */
	FILE*	file);	/*!< in: file to read message from */
/****************************************************************//**
Retrieves the error_info field from a trx.
@return the error info */
UNIV_INLINE
const dict_index_t*
trx_get_error_info(
/*===============*/
	const trx_t*	trx);	/*!< in: trx object */

/** @return an allocated transaction */
trx_t *trx_create();

/** At shutdown, frees a transaction object. */
void trx_free_at_shutdown(trx_t *trx);

/** Disconnect a prepared transaction from MySQL.
@param[in,out]	trx	transaction */
void trx_disconnect_prepared(trx_t *trx);

/** Initialize (resurrect) transactions at startup. */
dberr_t trx_lists_init_at_db_start();

/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_xa_low(
/*============================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	read_write);	/*!< in: true if read write transaction */
/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_low(
/*=========================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	read_write);	/*!< in: true if read write transaction */

/**
Start a transaction for internal processing.
@param trx          transaction
@param read_write   whether writes may be performed */
void trx_start_internal_low(trx_t *trx, bool read_write);

#ifdef UNIV_DEBUG
#define trx_start_if_not_started_xa(t, rw)			\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_if_not_started_xa_low((t), rw);		\
	} while (false)

#define trx_start_if_not_started(t, rw)				\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_if_not_started_low((t), rw);			\
	} while (false)

#define trx_start_internal(t)					\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_internal_low(t, true);			\
	} while (false)
#define trx_start_internal_read_only(t)				\
	do {							\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	trx_start_internal_low(t, false);			\
	} while (false)
#else
#define trx_start_if_not_started(t, rw)				\
	trx_start_if_not_started_low((t), rw)

#define trx_start_internal(t) trx_start_internal_low(t, true)
#define trx_start_internal_read_only(t) trx_start_internal_low(t, false)

#define trx_start_if_not_started_xa(t, rw)			\
	trx_start_if_not_started_xa_low((t), (rw))
#endif /* UNIV_DEBUG */

/** Start a transaction for a DDL operation.
@param trx   transaction */
void trx_start_for_ddl_low(trx_t *trx);

#ifdef UNIV_DEBUG
# define trx_start_for_ddl(t)					\
	do {							\
	ut_ad((t)->start_file == 0);				\
	(t)->start_line = __LINE__;				\
	(t)->start_file = __FILE__;				\
	t->state= TRX_STATE_NOT_STARTED;			\
	trx_start_for_ddl_low(t);				\
	} while (0)
#else
# define trx_start_for_ddl(t) trx_start_for_ddl_low(t)
#endif /* UNIV_DEBUG */

/** Commit a transaction */
void trx_commit_for_mysql(trx_t *trx) noexcept;
/** XA PREPARE a transaction.
@param[in,out]	trx	transaction to prepare */
void trx_prepare_for_mysql(trx_t* trx);
/**********************************************************************//**
This function is used to find number of prepared transactions and
their transaction objects for a recovery.
@return number of prepared transactions */
int
trx_recover_for_mysql(
/*==================*/
	XID*	xid_list,	/*!< in/out: prepared transactions */
	uint	len);		/*!< in: number of slots in xid_list */
/** Look up an X/Open distributed transaction in XA PREPARE state.
@param[in]	xid	X/Open XA transaction identifier
@return	transaction on match (the trx_t::xid will be invalidated);
note that the trx may have been committed before the caller acquires
trx_t::mutex
@retval	NULL if no match */
trx_t* trx_get_trx_by_xid(const XID* xid);
/** Durably write log until trx->commit_lsn
(if trx_t::commit_in_memory() was invoked with flush_log_later=true). */
void trx_commit_complete_for_mysql(trx_t *trx);
/****************************************************************//**
Prepares a transaction for commit/rollback. */
void
trx_commit_or_rollback_prepare(
/*===========================*/
	trx_t*	trx);	/*!< in/out: transaction */
/*********************************************************************//**
Creates a commit command node struct.
@return own: commit node struct */
commit_node_t*
trx_commit_node_create(
/*===================*/
	mem_heap_t*	heap);	/*!< in: mem heap where created */
/***********************************************************//**
Performs an execution step for a commit type node in a query graph.
@return query thread to run next, or NULL */
que_thr_t*
trx_commit_step(
/*============*/
	que_thr_t*	thr);	/*!< in: query thread */

/**********************************************************************//**
Prints info about a transaction. */
void
trx_print_low(
/*==========*/
	FILE*		f,
			/*!< in: output stream */
	const trx_t*	trx,
			/*!< in: transaction */
	ulint		n_rec_locks,
			/*!< in: trx->lock.n_rec_locks */
	ulint		n_trx_locks,
			/*!< in: length of trx->lock.trx_locks */
	ulint		heap_size);
			/*!< in: mem_heap_get_size(trx->lock.lock_heap) */

/**********************************************************************//**
Prints info about a transaction.
When possible, use trx_print() instead. */
void
trx_print_latched(
/*==============*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx);		/*!< in: transaction */

/**********************************************************************//**
Prints info about a transaction.
Acquires and releases lock_sys.latch. */
void
trx_print(
/*======*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx);		/*!< in: transaction */

/**********************************************************************//**
Determines if a transaction is in the given state.
The caller must hold trx->mutex, or it must be the thread
that is serving a running transaction.
A running RW transaction must be in trx_sys.rw_trx_hash.
@return TRUE if trx->state == state */
UNIV_INLINE
bool
trx_state_eq(
/*=========*/
	const trx_t*	trx,	/*!< in: transaction */
	trx_state_t	state,	/*!< in: state;
				if state != TRX_STATE_NOT_STARTED
				asserts that
				trx->state != TRX_STATE_NOT_STARTED */
	bool		relaxed = false)
				/*!< in: whether to allow
				trx->state == TRX_STATE_NOT_STARTED
				after an error has been reported */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/**********************************************************************//**
Determines if the currently running transaction has been interrupted.
@return true if interrupted */
bool
trx_is_interrupted(
/*===============*/
	const trx_t*	trx);	/*!< in: transaction */

/*******************************************************************//**
Calculates the "weight" of a transaction. The weight of one transaction
is estimated as the number of altered rows + the number of locked rows.
@param t transaction
@return transaction weight */
#define TRX_WEIGHT(t)	((t)->undo_no + UT_LIST_GET_LEN((t)->lock.trx_locks))

/** Create the trx_t pool */
void
trx_pool_init();

/** Destroy the trx_t pool */
void
trx_pool_close();

/**
Set the transaction as a read-write transaction if it is not already
tagged as such.
@param[in,out] trx	Transaction that needs to be "upgraded" to RW from RO */
void
trx_set_rw_mode(
	trx_t*		trx);

/**
Transactions that aren't started by the MySQL server don't set
the trx_t::mysql_thd field. For such transactions we set the lock
wait timeout to 0 instead of the user configured value that comes
from innodb_lock_wait_timeout via trx_t::mysql_thd.
@param trx transaction
@return lock wait timeout in seconds */
#define trx_lock_wait_timeout_get(t)					\
	((t)->mysql_thd != NULL						\
	 ? thd_lock_wait_timeout((t)->mysql_thd)			\
	 : 0)

typedef std::vector<ib_lock_t*, ut_allocator<ib_lock_t*> >	lock_list;

/** The locks and state of an active transaction. Protected by
lock_sys.latch, trx->mutex or both. */
struct trx_lock_t
{
  /** Lock request being waited for.
  Set to nonnull when holding lock_sys.latch, lock_sys.wait_mutex and
  trx->mutex, by the thread that is executing the transaction.
  Set to nullptr when holding lock_sys.wait_mutex. */
  Atomic_relaxed<lock_t*> wait_lock;
  /** Transaction being waited for; protected by lock_sys.wait_mutex */
  trx_t *wait_trx;
  /** condition variable for !wait_lock; used with lock_sys.wait_mutex */
  pthread_cond_t cond;
  /** lock wait start time */
  Atomic_relaxed<my_hrtime_t> suspend_time;

#if  defined(UNIV_DEBUG) || !defined(DBUG_OFF)
  /** 2=high priority WSREP thread has marked this trx to abort;
  1=another transaction chose this as a victim in deadlock resolution.

  Other threads than the one that is executing the transaction may set
  flags in this while holding lock_sys.wait_mutex. */
  Atomic_relaxed<byte> was_chosen_as_deadlock_victim;

  /** Flag the lock owner as a victim in Galera conflict resolution. */
  void set_wsrep_victim()
  {
    was_chosen_as_deadlock_victim.fetch_or(2);
  }
#else /* defined(UNIV_DEBUG) || !defined(DBUG_OFF) */

  /** High priority WSREP thread has marked this trx to abort or
  another transaction chose this as a victim in deadlock resolution.

  Other threads than the one that is executing the transaction may set
  this while holding lock_sys.wait_mutex. */
  Atomic_relaxed<bool> was_chosen_as_deadlock_victim;

  /** Flag the lock owner as a victim in Galera conflict resolution. */
  void set_wsrep_victim() { was_chosen_as_deadlock_victim= true; }
#endif /* defined(UNIV_DEBUG) || !defined(DBUG_OFF) */

  /** Next available rec_pool[] entry */
  byte rec_cached;
  /** Next available table_pool[] entry */
  byte table_cached;

	que_thr_t*	wait_thr;	/*!< query thread belonging to this
					trx that is in waiting
					state. For threads suspended in a
					lock wait, this is protected by
					lock_sys.latch. Otherwise, this may
					only be modified by the thread that is
					serving the running transaction. */

  /** Pre-allocated record locks */
  struct {
    alignas(CPU_LEVEL1_DCACHE_LINESIZE) ib_lock_t lock;
  } rec_pool[8];

  /** Pre-allocated table locks */
  ib_lock_t table_pool[8];

  /** Memory heap for trx_locks. Protected by lock_sys.assert_locked()
  and lock_sys.is_writer() || trx->mutex_is_owner(). */
  mem_heap_t *lock_heap;

  /** Locks held by the transaction. Protected by lock_sys.assert_locked()
  and lock_sys.is_writer() || trx->mutex_is_owner().
  (If lock_sys.latch is only held in shared mode, then the modification
  must be protected by trx->mutex.) */
  trx_lock_list_t trx_locks;

	lock_list	table_locks;	/*!< All table locks requested by this
					transaction, including AUTOINC locks */

	/** List of pending trx_t::evict_table() */
	UT_LIST_BASE_NODE_T(dict_table_t) evicted_tables;

  /** number of record locks; protected by lock_sys.assert_locked(page_id) */
  ulint n_rec_locks;
  /** number of lock_rec_set_nth_bit() calls since the start of transaction;
  protected by lock_sys.is_writer() or trx->mutex_is_owner(). */
  ulint set_nth_bit_calls;
};

/** Logical first modification time of a table in a transaction */
class trx_mod_table_time_t
{
  /** Impossible value for trx_t::undo_no */
  static constexpr undo_no_t NONE= ~undo_no_t{0};
  /** Theoretical maximum value for trx_t::undo_no.
  DB_ROLL_PTR is only 7 bytes, so it cannot point to more than
  this many undo log records. */
  static constexpr undo_no_t LIMIT= (undo_no_t{1} << (7 * 8)) - 1;

  /** Flag in 'first' to indicate that subsequent operations are
  covered by a TRX_UNDO_EMPTY record (for the first statement to
  insert into an empty table) */
  static constexpr undo_no_t BULK= 1ULL << 63;

  /** First modification of the table, possibly ORed with BULK */
  undo_no_t first;
  /** First modification of a system versioned column
  (NONE= no versioning, BULK= the table was dropped) */
  undo_no_t first_versioned= NONE;
#ifdef UNIV_DEBUG
  /** Whether the modified table is a FTS auxiliary table */
  bool fts_aux_table= false;
#endif /* UNIV_DEBUG */

  /** Buffer to store insert opertion */
  row_merge_bulk_t *bulk_store= nullptr;

  friend struct trx_t;
public:
  /** Constructor
  @param rows   number of modified rows so far */
  trx_mod_table_time_t(undo_no_t rows) : first(rows) { ut_ad(rows < LIMIT); }

#ifdef UNIV_DEBUG
  /** Validation
  @param rows   number of modified rows so far
  @return whether the object is valid */
  bool valid(undo_no_t rows= NONE) const
  { auto f= first & LIMIT; return f <= first_versioned && f <= rows; }
#endif /* UNIV_DEBUG */
  /** @return if versioned columns were modified */
  bool is_versioned() const { return (~first_versioned & LIMIT) != 0; }
  /** @return if the table was dropped */
  bool is_dropped() const { return first_versioned == BULK; }

  /** After writing an undo log record, set is_versioned() if needed
  @param rows   number of modified rows so far */
  void set_versioned(undo_no_t rows)
  {
    ut_ad(first_versioned == NONE);
    first_versioned= rows;
    ut_ad(valid(rows));
  }

  /** After writing an undo log record, note that the table will be dropped */
  void set_dropped()
  {
    ut_ad(first_versioned == NONE);
    first_versioned= BULK;
  }

  /** Notify the start of a bulk insert operation
  @param table table to do bulk operation
  @param also_primary start bulk insert operation for primary index */
  void start_bulk_insert(dict_table_t *table, bool also_primary)
  {
    first|= BULK;
    if (!table->is_temporary())
      bulk_store= new row_merge_bulk_t(table, also_primary);
  }

  /** Notify the end of a bulk insert operation */
  void end_bulk_insert() { first&= ~BULK; }

  /** @return whether an insert is covered by TRX_UNDO_EMPTY record */
  bool is_bulk_insert() const { return first & BULK; }

  /** Invoked after partial rollback
  @param limit	number of surviving modified rows (trx_t::undo_no)
  @return	whether this should be erased from trx_t::mod_tables */
  bool rollback(undo_no_t limit)
  {
    ut_ad(valid());
    if ((LIMIT & first) >= limit)
      return true;
    if (first_versioned < limit)
      first_versioned= NONE;
    return false;
  }

#ifdef UNIV_DEBUG
  void set_aux_table() { fts_aux_table= true; }

  bool is_aux_table() const { return fts_aux_table; }
#endif /* UNIV_DEBUG */

  /** @return the first undo record that modified the table */
  undo_no_t get_first() const
  {
    ut_ad(valid());
    return LIMIT & first;
  }

  /** Add the tuple to the transaction bulk buffer for the given index.
  @param entry  tuple to be inserted
  @param index  bulk insert for the index
  @param trx    transaction */
  dberr_t bulk_insert_buffered(const dtuple_t &entry,
                               const dict_index_t &index, trx_t *trx)
  {
    return bulk_store->bulk_insert_buffered(entry, index, trx);
  }

  /** Do bulk insert operation present in the buffered operation
  @return DB_SUCCESS or error code */
  dberr_t write_bulk(dict_table_t *table, trx_t *trx);

  /** @return whether the buffer storage exist */
  bool bulk_buffer_exist() const
  {
    return bulk_store && is_bulk_insert();
  }

  /** @return whether InnoDB has to skip sort for clustered index */
  bool skip_sort_pk() const
  {
    return bulk_store && !bulk_store->m_sort_primary_key;
  }

  /** Free bulk insert operation */
  void clear_bulk_buffer()
  {
    delete bulk_store;
    bulk_store= nullptr;
  }
};

/** Collection of persistent tables and their first modification
in a transaction.
We store pointers to the table objects in memory because
we know that a table object will not be destroyed while a transaction
that modified it is running. */
typedef std::map<
	dict_table_t*, trx_mod_table_time_t,
	std::less<dict_table_t*>,
	ut_allocator<std::pair<dict_table_t* const, trx_mod_table_time_t> > >
	trx_mod_tables_t;

/** The transaction handle

Normally, there is a 1:1 relationship between a transaction handle
(trx) and a session (client connection). One session is associated
with exactly one user transaction. There are some exceptions to this:

* For DDL operations, a subtransaction is allocated that modifies the
data dictionary tables. Lock waits and deadlocks are prevented by
acquiring the dict_sys.latch before starting the subtransaction
and releasing it after committing the subtransaction.

* The purge system uses a special transaction that is not associated
with any session.

* If the system crashed or it was quickly shut down while there were
transactions in the ACTIVE or PREPARED state, these transactions would
no longer be associated with a session when the server is restarted.

A session may be served by at most one thread at a time. The serving
thread of a session might change in some MySQL implementations.
Therefore we do not have pthread_self() assertions in the code.

Normally, only the thread that is currently associated with a running
transaction may access (read and modify) the trx object, and it may do
so without holding any mutex. The following are exceptions to this:

* trx_rollback_recovered() may access resurrected (connectionless)
transactions (state == TRX_STATE_ACTIVE && is_recovered)
while the system is already processing new user transactions (!is_recovered).

* trx_print_low() may access transactions not associated with the current
thread. The caller must be holding lock_sys.latch.

* When a transaction handle is in the trx_sys.trx_list, some of its fields
must not be modified without holding trx->mutex.

* The locking code (in particular, lock_deadlock_recursive() and
lock_rec_convert_impl_to_expl()) will access transactions associated
to other connections. The locks of transactions are protected by
lock_sys.latch (insertions also by trx->mutex). */

/** Represents an instance of rollback segment along with its state variables.*/
struct trx_undo_ptr_t {
	trx_rseg_t*	rseg;		/*!< rollback segment assigned to the
					transaction, or NULL if not assigned
					yet */
	trx_undo_t*	undo;		/*!< pointer to the undo log, or
					NULL if nothing logged yet */
};

/** An instance of temporary rollback segment. */
struct trx_temp_undo_t {
	/** temporary rollback segment, or NULL if not assigned yet */
	trx_rseg_t*	rseg;
	/** pointer to the undo log, or NULL if nothing logged yet */
	trx_undo_t*	undo;
};

/** Rollback segments assigned to a transaction for undo logging. */
struct trx_rsegs_t {
	/** undo log ptr holding reference to a rollback segment that resides in
	system/undo tablespace used for undo logging of tables that needs
	to be recovered on crash. */
	trx_undo_ptr_t	m_redo;

	/** undo log for temporary tables; discarded immediately after
	transaction commit/rollback */
	trx_temp_undo_t	m_noredo;
};

struct trx_t : ilist_node<>
{
private:
  /**
    Least significant 31 bits is count of references.

    We can't release the locks nor commit the transaction until this reference
    is 0. We can change the state to TRX_STATE_COMMITTED_IN_MEMORY to signify
    that it is no longer "active".

    If the most significant bit is set this transaction should stop inheriting
    (GAP)locks. Generally set to true during transaction prepare for RC or lower
    isolation, if requested. Needed for replication replay where
    we don't want to get blocked on GAP locks taken for protecting
    concurrent unique insert or replace operation.
  */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE)
  Atomic_relaxed<uint32_t> skip_lock_inheritance_and_n_ref;


public:
  /** Transaction identifier (0 if no locks were acquired).
  Set by trx_sys_t::register_rw() or trx_resurrect() before
  the transaction is added to trx_sys.rw_trx_hash.
  Cleared in commit_in_memory() after commit_state(),
  trx_sys_t::deregister_rw(), release_locks(). */
  trx_id_t id;
  union
  {
    /** The largest encountered transaction identifier for which no
    transaction was observed to be active. This is a cache to speed up
    trx_sys_t::find_same_or_older().

    This will be zero-initialized in Pool::Pool() and not initialized
    when a transaction object in the pool is freed and reused. The
    idea is that new transactions can reuse the result of
    an expensive trx_sys_t::find_same_or_older_low() invocation that
    was performed in an earlier transaction that used the same
    memory area. */
    trx_id_t max_inactive_id;
    /** Same as max_inactive_id, for purge_sys.query->trx which may be
    accessed by multiple concurrent threads in in
    trx_sys_t::find_same_or_older_in_purge(). Writes are protected by
    trx_t::mutex. */
    Atomic_relaxed<trx_id_t> max_inactive_id_atomic;
  };

private:
  /** mutex protecting state and some of lock
  (some are protected by lock_sys.latch) */
  srw_spin_mutex mutex;
#ifdef UNIV_DEBUG
  /** The owner of mutex (0 if none); protected by mutex */
  std::atomic<pthread_t> mutex_owner{0};
#endif /* UNIV_DEBUG */
public:
  void mutex_init() { mutex.init(); }
  void mutex_destroy() { mutex.destroy(); }

  /** Acquire the mutex */
  void mutex_lock()
  {
    ut_ad(!mutex_is_owner());
    mutex.wr_lock();
    assert(!mutex_owner.exchange(pthread_self(),
                                 std::memory_order_relaxed));
  }
  /** Release the mutex */
  void mutex_unlock()
  {
    assert(mutex_owner.exchange(0, std::memory_order_relaxed) ==
           pthread_self());
    mutex.wr_unlock();
  }
#ifndef SUX_LOCK_GENERIC
  bool mutex_is_locked() const noexcept { return mutex.is_locked(); }
#endif
#ifdef UNIV_DEBUG
  /** @return whether the current thread holds the mutex */
  bool mutex_is_owner() const
  {
    return mutex_owner.load(std::memory_order_relaxed) ==
      pthread_self();
  }
#endif /* UNIV_DEBUG */

  /** State of the trx from the point of view of concurrency control
  and the valid state transitions.

  Possible states:

  TRX_STATE_NOT_STARTED
  TRX_STATE_ABORTED
  TRX_STATE_ACTIVE
  TRX_STATE_PREPARED
  TRX_STATE_PREPARED_RECOVERED (special case of TRX_STATE_PREPARED)
  TRX_STATE_COMMITTED_IN_MEMORY (alias below COMMITTED)

  Valid state transitions are:

  Regular transactions:
  * NOT_STARTED -> ACTIVE -> COMMITTED -> NOT_STARTED
  * NOT_STARTED -> ABORTED (when thd_mark_transaction_to_rollback() is called)
  * ABORTED -> NOT_STARTED (acknowledging the rollback of a transaction)

  Auto-commit non-locking read-only:
  * NOT_STARTED -> ACTIVE -> NOT_STARTED

  XA (2PC):
  * NOT_STARTED -> ACTIVE -> PREPARED -> COMMITTED -> NOT_STARTED

  Recovered XA:
  * NOT_STARTED -> PREPARED -> COMMITTED -> (freed)

  Recovered XA followed by XA ROLLBACK:
  * NOT_STARTED -> PREPARED -> ACTIVE -> COMMITTED -> (freed)

  XA (2PC) (shutdown or disconnect before ROLLBACK or COMMIT):
  * NOT_STARTED -> PREPARED -> (freed)

  Disconnected XA PREPARE transaction can become recovered:
  * ... -> ACTIVE -> PREPARED (connected) -> PREPARED (disconnected)

  Latching and various transaction lists membership rules:

  XA (2PC) transactions are always treated as non-autocommit.

  Transitions to ACTIVE or NOT_STARTED occur when transaction
  is not in rw_trx_hash.

  Autocommit non-locking read-only transactions move between states
  without holding any mutex. They are not in rw_trx_hash.

  All transactions, unless they are determined to be ac-nl-ro,
  explicitly tagged as read-only or read-write, will first be put
  on the read-only transaction list. Only when a !read-only transaction
  in the read-only list tries to acquire an X or IX lock on a table
  do we remove it from the read-only list and put it on the read-write
  list. During this switch we assign it a rollback segment.

  When a transaction is NOT_STARTED or ABORTED, it can be in trx_list.
  It cannot be in rw_trx_hash.

  ACTIVE->PREPARED->COMMITTED and ACTIVE->COMMITTED is only possible when
  trx is in rw_trx_hash. These transitions are protected by trx_t::mutex.

  COMMITTED->NOT_STARTED is possible when trx_t::mutex is being held.
  The transaction would already have been removed from rw_trx_hash by
  trx_sys_t::deregister_rw() on the transition to COMMITTED.

  Transitions between NOT_STARTED and ABORTED can be performed at any time by
  the thread that is associated with the transaction. */
  Atomic_relaxed<trx_state_t> state;

  /** The locks of the transaction. Protected by lock_sys.latch
  (insertions also by trx_t::mutex). */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) trx_lock_t lock;

#ifdef WITH_WSREP
  /** whether wsrep_on(mysql_thd) held at the start of transaction */
  byte wsrep;
  bool is_wsrep() const { return UNIV_UNLIKELY(wsrep); }
  bool is_wsrep_UK_scan() const { return UNIV_UNLIKELY(wsrep & 2); }
#else /* WITH_WSREP */
  bool is_wsrep() const { return false; }
#endif /* WITH_WSREP */

  /** @return whether the transaction has been started */
  bool is_started() const noexcept
  {
    static_assert(TRX_STATE_NOT_STARTED == 0, "");
    static_assert(TRX_STATE_ABORTED == 1, "");
    return state > TRX_STATE_ABORTED;
  }

  /** Consistent read view of the transaction */
  ReadView read_view;

	/* These fields are not protected by any mutex. */

	/** false=normal transaction, true=recovered (must be rolled back)
	or disconnected transaction in XA PREPARE STATE.

	This field is accessed by the thread that owns the transaction,
	without holding any mutex.
	There is only one foreign-thread access in trx_print_low()
	and a possible race condition with trx_disconnect_prepared(). */
	bool		is_recovered;
	const char*	op_info;	/*!< English text describing the
					current operation, or an empty
					string */
  /** TRX_ISO_REPEATABLE_READ, ... */
  unsigned isolation_level:2;
  /** when set, REPEATABLE READ will actually be Snapshot Isolation, due to
  detecting write/write conflicts and disabling "semi-consistent read" */
  unsigned snapshot_isolation:1;
  /** normally set; "SET foreign_key_checks=0" can be issued to suppress
  foreign key checks, in table imports, for example */
  unsigned check_foreigns:1;
  /** normally set; "SET unique_checks=0, foreign_key_checks=0"
  enables bulk insert into an empty table */
  unsigned check_unique_secondary:1;
  /** whether an insert into an empty table is active
  Possible states are
  TRX_NO_BULK
  TRX_DML_BULK
  TRX_DDL_BULK
  @see trx_bulk_insert in trx0types.h */
  unsigned bulk_insert:2;
	/*------------------------------*/
	/* MySQL has a transaction coordinator to coordinate two phase
	commit between multiple storage engines and the binary log. When
	an engine participates in a transaction, it's responsible for
	registering itself using the trans_register_ha() API. */
	bool		is_registered;	/* This flag is set to true after the
					transaction has been registered with
					the coordinator using the XA API, and
					is set to false  after commit or
					rollback. */
	/** whether this is holding the prepare mutex */
	bool		active_commit_ordered;
	/*------------------------------*/
	bool		flush_log_later;/* In 2PC, we hold the
					prepare_commit mutex across
					both phases. In that case, we
					defer flush of the logs to disk
					until after we release the
					mutex. */
	ulint		duplicates;	/*!< TRX_DUP_IGNORE | TRX_DUP_REPLACE */
  /** whether this modifies InnoDB dictionary tables */
  bool dict_operation;
#ifdef UNIV_DEBUG
  /** copy of dict_operation during commit() */
  bool was_dict_operation;
#endif
	/** whether dict_sys.latch is held exclusively; protected by
	dict_sys.latch */
	bool dict_operation_lock_mode;

	/** wall-clock time of the latest transition to TRX_STATE_ACTIVE;
	used for diagnostic purposes only */
	time_t		start_time;
	/** microsecond_interval_timer() of transaction start */
	ulonglong	start_time_micro;
	lsn_t		commit_lsn;	/*!< lsn at the time of the commit */
	/*------------------------------*/
	THD*		mysql_thd;	/*!< MySQL thread handle corresponding
					to this trx, or NULL */

	const char*	mysql_log_file_name;
					/*!< if MySQL binlog is used, this field
					contains a pointer to the latest file
					name; this is NULL if binlog is not
					used */
	ulonglong	mysql_log_offset;
					/*!< if MySQL binlog is used, this
					field contains the end offset of the
					binlog entry */
	/*------------------------------*/
	ib_uint32_t	n_mysql_tables_in_use; /*!< number of Innobase tables
					used in the processing of the current
					SQL statement in MySQL */
	ib_uint32_t	mysql_n_tables_locked;
					/*!< how many tables the current SQL
					statement uses, except those
					in consistent read */

  /** DB_SUCCESS or error code; usually only the thread that is running
  the transaction is allowed to modify this field. The only exception is
  when a thread invokes lock_sys_t::cancel() in order to abort a
  lock_wait(). That is protected by lock_sys.wait_mutex and lock.wait_lock. */
  dberr_t error_state;

	const dict_index_t*error_info;	/*!< if the error number indicates a
					duplicate key error, a pointer to
					the problematic index is stored here */
	ulint		error_key_num;	/*!< if the index creation fails to a
					duplicate key error, a mysql key
					number of that index is stored here */
	que_t*		graph;		/*!< query currently run in the session,
					or NULL if none; NOTE that the query
					belongs to the session, and it can
					survive over a transaction commit, if
					it is a stored procedure with a COMMIT
					WORK statement, for instance */
	/*------------------------------*/
	undo_no_t	undo_no;	/*!< next undo log record number to
					assign; since the undo log is
					private for a transaction, this
					is a simple ascending sequence
					with no gaps; thus it represents
					the number of modified/inserted
					rows in a transaction */
	undo_no_t	last_stmt_start;
					/*!< undo_no when the last sql statement
					was started: in case of an error, trx
					is rolled back down to this number */
	trx_rsegs_t	rsegs;		/* rollback segments for undo logging */
	undo_no_t	roll_limit;	/*!< least undo number to undo during
					a partial rollback; 0 otherwise */
	bool		in_rollback;	/*!< true when the transaction is
					executing a partial or full rollback */
	ulint		pages_undone;	/*!< number of undo log pages undone
					since the last undo log truncation */
	/*------------------------------*/
	ulint		n_autoinc_rows;	/*!< no. of AUTO-INC rows required for
					an SQL statement. This is useful for
					multi-row INSERTs */
  typedef small_vector<lock_t*, 4> autoinc_lock_vector;
  /** AUTO_INCREMENT locks held by this transaction; a subset of trx_locks,
  protected by lock_sys.latch. */
  autoinc_lock_vector autoinc_locks;
	/*------------------------------*/
	bool		read_only;	/*!< true if transaction is flagged
					as a READ-ONLY transaction.
					if auto_commit && !will_lock
					then it will be handled as a
					AC-NL-RO-SELECT (Auto Commit Non-Locking
					Read Only Select). A read only
					transaction will not be assigned an
					UNDO log. */
	bool		auto_commit;	/*!< true if it is an autocommit */
	bool		will_lock;	/*!< set to inform trx_start_low() that
					the transaction may acquire locks */
	/* True if transaction has to read the undo log and
	log the DML changes for online DDL table */
	bool		apply_online_log = false;

	/*------------------------------*/
	fts_trx_t*	fts_trx;	/*!< FTS information, or NULL if
					transaction hasn't modified tables
					with FTS indexes (yet). */
	doc_id_t	fts_next_doc_id;/* The document id used for updates */
	/*------------------------------*/
	ib_uint32_t	flush_tables;	/*!< if "covering" the FLUSH TABLES",
					count of tables being flushed. */

	/*------------------------------*/
#ifdef UNIV_DEBUG
	unsigned	start_line;	/*!< Track where it was started from */
	const char*	start_file;	/*!< Filename where it was started */
#endif /* UNIV_DEBUG */

	XID		xid;		/*!< X/Open XA transaction
					identification to identify a
					transaction branch */
	trx_mod_tables_t mod_tables;	/*!< List of tables that were modified
					by this transaction */
	/*------------------------------*/
	char*		detailed_error;	/*!< detailed error message for last
					error, or empty. */
	rw_trx_hash_element_t *rw_trx_hash_element;
	LF_PINS *rw_trx_hash_pins;
	ulint		magic_n;

	/** @return whether any persistent undo log has been generated */
	bool has_logged_persistent() const
	{
		return(rsegs.m_redo.undo);
	}

	/** @return whether any undo log has been generated */
	bool has_logged() const
	{
		return(has_logged_persistent() || rsegs.m_noredo.undo);
	}

	/** @return rollback segment for modifying temporary tables */
	trx_rseg_t* get_temp_rseg()
	{
		if (trx_rseg_t* rseg = rsegs.m_noredo.rseg) {
			ut_ad(id != 0);
			return(rseg);
		}

		return(assign_temp_rseg());
	}

  /** Transition to committed state, to release implicit locks. */
  inline void commit_state();

  /** Release any explicit locks of a committing transaction. */
  inline void release_locks();

  /** Evict a table definition due to the rollback of ALTER TABLE.
  @param table_id   table identifier
  @param reset_only whether to only reset dict_table_t::def_trx_id */
  void evict_table(table_id_t table_id, bool reset_only= false);

  /** Initiate rollback.
  @param savept   pointer to savepoint; nullptr=entire transaction
  @return error code or DB_SUCCESS */
  dberr_t rollback(const undo_no_t *savept= nullptr) noexcept;
  /** Roll back an active transaction.
  @param savept   pointer to savepoint; nullptr=entire transaction
  @return error code or DB_SUCCESS */
  dberr_t rollback_low(const undo_no_t *savept= nullptr) noexcept;
  /** Finish rollback.
  @return whether the rollback was completed normally
  @retval false if the rollback was aborted by shutdown */
  bool rollback_finish() noexcept;
private:
  /** Apply any changes to tables for which online DDL is in progress. */
  ATTRIBUTE_COLD void apply_log();
  /** Process tables that were modified by the committing transaction. */
  inline void commit_tables();
  /** Mark a transaction committed in the main memory data structures.
  @param mtr  mini-transaction (if there are any persistent modifications) */
  inline void commit_in_memory(const mtr_t *mtr);
  /** Write log for committing the transaction. */
  void commit_persist() noexcept;
  /** Clean up the transaction after commit_in_memory()
  @return false (always) */
  bool commit_cleanup() noexcept;
  /** Commit the transaction in a mini-transaction.
  @param mtr  mini-transaction (if there are any persistent modifications) */
  void commit_low(mtr_t *mtr= nullptr);
  /** Commit an empty transaction.
  @param mtr   mini-transaction */
  void commit_empty(mtr_t *mtr);
  /** Commit an empty transaction.
  @param mtr   mini-transaction */
  /** Assign the transaction its history serialisation number and write the
  UNDO log to the assigned rollback segment.
  @param mtr   mini-transaction */
  inline void write_serialisation_history(mtr_t *mtr);
public:
  /** Commit the transaction. */
  void commit() noexcept;

  /** Try to drop a persistent table.
  @param table       persistent table
  @param fk          whether to drop FOREIGN KEY metadata
  @return error code */
  dberr_t drop_table(const dict_table_t &table);
  /** Try to drop the foreign key constraints for a persistent table.
  @param name        name of persistent table
  @return error code */
  dberr_t drop_table_foreign(const table_name_t &name);
  /** Try to drop the statistics for a persistent table.
  @param name        name of persistent table
  @return error code */
  dberr_t drop_table_statistics(const table_name_t &name);
  /** Commit the transaction, possibly after drop_table().
  @param deleted   handles of data files that were deleted */
  void commit(std::vector<pfs_os_file_t> &deleted);


  bool is_referenced() const
  {
    return (skip_lock_inheritance_and_n_ref & ~(1U << 31)) > 0;
  }


  void reference()
  {
    ut_d(auto old_n_ref =)
    skip_lock_inheritance_and_n_ref.fetch_add(1);
    ut_ad(int32_t(old_n_ref << 1) >= 0);
  }

  void release_reference()
  {
    ut_d(auto old_n_ref =)
    skip_lock_inheritance_and_n_ref.fetch_sub(1);
    ut_ad(int32_t(old_n_ref << 1) > 0);
  }

  bool is_not_inheriting_locks() const
  {
    return skip_lock_inheritance_and_n_ref >> 31;
  }

  void set_skip_lock_inheritance()
  {
    ut_d(auto old_n_ref=) skip_lock_inheritance_and_n_ref.fetch_add(1U << 31);
    ut_ad(!(old_n_ref >> 31));
  }

  void reset_skip_lock_inheritance()
  {
    skip_lock_inheritance_and_n_ref.fetch_and(~1U << 31);
  }

  /** @return whether the table has lock on
  mysql.innodb_table_stats or mysql.innodb_index_stats */
  bool has_stats_table_lock() const;

  /** Free the memory to trx_pools */
  void free();


  void assert_freed() const
  {
    ut_ad(state == TRX_STATE_NOT_STARTED);
    ut_ad(!id);
    ut_ad(!*detailed_error);
    ut_ad(!mutex_is_owner());
    ut_ad(!has_logged());
    ut_ad(!is_referenced());
    ut_ad(!is_wsrep());
    ut_ad(!lock.was_chosen_as_deadlock_victim);
    ut_ad(mod_tables.empty());
    ut_ad(!read_view.is_open());
    ut_ad(!lock.wait_thr);
    ut_ad(!lock.wait_lock);
    ut_ad(UT_LIST_GET_LEN(lock.trx_locks) == 0);
    ut_ad(lock.table_locks.empty());
    ut_ad(autoinc_locks.empty());
    ut_ad(UT_LIST_GET_LEN(lock.evicted_tables) == 0);
    ut_ad(!dict_operation);
    ut_ad(!apply_online_log);
    ut_ad(!is_not_inheriting_locks());
    ut_ad(check_foreigns);
    ut_ad(check_unique_secondary);
    ut_ad(bulk_insert == TRX_NO_BULK);
  }

  /** This has to be invoked on SAVEPOINT or at the end of a statement.
  Even if a TRX_UNDO_EMPTY record was written for this table to cover an
  insert into an empty table, subsequent operations will have to be covered
  by row-level undo log records, so that ROLLBACK TO SAVEPOINT or a
  rollback to the start of a statement will work.
  @param table   table on which any preceding bulk insert ended */
  void end_bulk_insert(const dict_table_t &table)
  {
    auto it= mod_tables.find(const_cast<dict_table_t*>(&table));
    if (it != mod_tables.end())
      it->second.end_bulk_insert();
  }

  /** @return whether this is a non-locking autocommit transaction */
  bool is_autocommit_non_locking() const { return auto_commit && !will_lock; }

  /** This has to be invoked on SAVEPOINT or at the start of a statement.
  Even if TRX_UNDO_EMPTY records were written for any table to cover an
  insert into an empty table, subsequent operations will have to be covered
  by row-level undo log records, so that ROLLBACK TO SAVEPOINT or a
  rollback to the start of a statement will work. */
  void end_bulk_insert()
  {
    if (bulk_insert == TRX_DDL_BULK)
      return;
    for (auto& t : mod_tables)
      t.second.end_bulk_insert();
  }

  /** @return whether a bulk insert into empty table is in progress */
  bool is_bulk_insert() const
  {
    switch (bulk_insert) {
    case TRX_NO_BULK:
      return false;
    case TRX_DDL_BULK:
      return true;
    default:
      ut_ad(bulk_insert == TRX_DML_BULK);
    }
    if (check_unique_secondary || check_foreigns)
      return false;
    for (const auto& t : mod_tables)
      if (t.second.is_bulk_insert())
        return true;
    return false;
  }

  /**
  @return logical modification time of a table
  @retval nullptr if the table doesn't have bulk buffer or
  can skip sorting for primary key */
  trx_mod_table_time_t *use_bulk_buffer(dict_index_t *index) noexcept
  {
    if (UNIV_LIKELY(!bulk_insert))
      return nullptr;
    ut_ad(index->table->skip_alter_undo || !check_unique_secondary);
    ut_ad(index->table->skip_alter_undo || !check_foreigns);
    auto it= mod_tables.find(index->table);
    if (it == mod_tables.end() || !it->second.bulk_buffer_exist())
      return nullptr;
    /* Avoid using bulk buffer for load statement */
    if (index->is_clust() && it->second.skip_sort_pk())
      return nullptr;
    return &it->second;
  }

  /** Do the bulk insert for the buffered insert operation
  for the transaction.
  @return DB_SUCCESS or error code */
  template<trx_bulk_insert type= TRX_DML_BULK>
  dberr_t bulk_insert_apply()
  {
    static_assert(type != TRX_NO_BULK, "");
    return bulk_insert == type ? bulk_insert_apply_low(): DB_SUCCESS;
  }

private:
  /** Apply the buffered bulk inserts. */
  dberr_t bulk_insert_apply_low();

  /** Rollback the bulk insert operation for the transaction */
  void bulk_rollback_low();
  /** Assign a rollback segment for modifying temporary tables.
  @return the assigned rollback segment */
  trx_rseg_t *assign_temp_rseg();
};

/* Transaction isolation levels (trx->isolation_level) */
#define TRX_ISO_READ_UNCOMMITTED	0	/* dirty read: non-locking
						SELECTs are performed so that
						we do not look at a possible
						earlier version of a record;
						thus they are not 'consistent'
						reads under this isolation
						level; otherwise like level
						2 */

#define TRX_ISO_READ_COMMITTED		1	/* somewhat Oracle-like
						isolation, except that in
						range UPDATE and DELETE we
						must block phantom rows
						with next-key locks;
						SELECT ... FOR UPDATE and ...
						LOCK IN SHARE MODE only lock
						the index records, NOT the
						gaps before them, and thus
						allow free inserting;
						each consistent read reads its
						own snapshot */

#define TRX_ISO_REPEATABLE_READ		2	/* this is the default;
						all consistent reads in the
						same trx read the same
						snapshot;
						full next-key locking used
						in locking reads to block
						insertions into gaps */

#define TRX_ISO_SERIALIZABLE		3	/* all plain SELECTs are
						converted to LOCK IN SHARE
						MODE reads */

/* Treatment of duplicate values (trx->duplicates; for example, in inserts).
Multiple flags can be combined with bitwise OR. */
#define TRX_DUP_IGNORE	1U	/* duplicate rows are to be updated */
#define TRX_DUP_REPLACE	2U	/* duplicate rows are to be replaced */


/** Commit node states */
enum commit_node_state {
	COMMIT_NODE_SEND = 1,	/*!< about to send a commit signal to
				the transaction */
	COMMIT_NODE_WAIT	/*!< commit signal sent to the transaction,
				waiting for completion */
};

/** Commit command node in a query graph */
struct commit_node_t{
	que_common_t	common;	/*!< node type: QUE_NODE_COMMIT */
	enum commit_node_state
			state;	/*!< node execution state */
};


#include "trx0trx.inl"

#endif
