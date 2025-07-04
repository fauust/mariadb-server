/* Copyright (C) 2004-2008 MySQL AB & MySQL Finland AB & TCX DataKonsult AB
   Copyright (C) 2008-2009 Sun Microsystems, Inc.
   Copyright (c) 2009, 2021, MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */


#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation                          // gcc: Class implementation
#endif

#define MYSQL_SERVER 1
#include <my_global.h>
#include <m_ctype.h>
#include <my_dir.h>
#include <myisampack.h>
#include <my_bit.h>
#include "ha_maria.h"
#include "trnman_public.h"
#include "trnman.h"

C_MODE_START
#include "maria_def.h"
#include "ma_rt_index.h"
#include "ma_blockrec.h"
#include "ma_checkpoint.h"
#include "ma_recovery.h"
C_MODE_END
#include "ma_trnman.h"
#include "ma_loghandler.h"

//#include "sql_priv.h"
#include "protocol.h"
#include "sql_class.h"
#include "key.h"
#include "log.h"
#include "sql_parse.h"
#include "mysql/service_print_check_msg.h"
#include "debug.h"

/*
  Note that in future versions, only *transactional* Maria tables can
  rollback, so this flag should be up or down conditionally.
*/
#ifdef ARIA_HAS_TRANSACTIONS
#define TRANSACTION_STATE
#else
#define TRANSACTION_STATE HA_NO_TRANSACTIONS
#endif

#define THD_TRN (TRN*) thd_get_ha_data(thd, maria_hton)

ulong pagecache_division_limit, pagecache_age_threshold, pagecache_file_hash_size;
ulonglong pagecache_buffer_size;
const char *zerofill_error_msg=
  "Table is probably from another system and must be zerofilled or repaired ('REPAIR TABLE table_name') to be usable on this system";

/**
   As the auto-repair is initiated when opened from the SQL layer
   (open_unireg_entry(), check_and_repair()), it does not happen when Maria's
   Recovery internally opens the table to apply log records to it, which is
   good. It would happen only after Recovery, if the table is still
   corrupted.
*/
ulonglong maria_recover_options= HA_RECOVER_NONE;
handlerton *maria_hton;

/* bits in maria_recover_options */
const char *maria_recover_names[]=
{
  /*
    Compared to MyISAM, "default" was renamed to "normal" as it collided with
    SET var=default which sets to the var's default i.e. what happens when the
    var is not set i.e. HA_RECOVER_NONE.
    OFF flag is ignored.
  */
  "NORMAL", "BACKUP", "FORCE", "QUICK", "OFF", NullS
};
TYPELIB maria_recover_typelib=
{
  array_elements(maria_recover_names) - 1, "",
  maria_recover_names, NULL
};

const char *maria_stats_method_names[]=
{
  "nulls_unequal", "nulls_equal",
  "nulls_ignored", NullS
};
TYPELIB maria_stats_method_typelib=
{
  array_elements(maria_stats_method_names) - 1, "",
  maria_stats_method_names, NULL
};

/* transactions log purge mode */
const char *maria_translog_purge_type_names[]=
{
  "immediate", "external", "at_flush", NullS
};
TYPELIB maria_translog_purge_type_typelib=
{
  array_elements(maria_translog_purge_type_names) - 1, "",
  maria_translog_purge_type_names, NULL
};

/* transactional log directory sync */
const char *maria_sync_log_dir_names[]=
{
  "NEVER", "NEWFILE", "ALWAYS", NullS
};
TYPELIB maria_sync_log_dir_typelib=
{
  array_elements(maria_sync_log_dir_names) - 1, "",
  maria_sync_log_dir_names, NULL
};

/* transactional log group commit */
const char *maria_group_commit_names[]=
{
  "none", "hard", "soft", NullS
};
TYPELIB maria_group_commit_typelib=
{
  array_elements(maria_group_commit_names) - 1, "",
  maria_group_commit_names, NULL
};

/** Interval between background checkpoints in seconds */
static ulong checkpoint_interval;
static void update_checkpoint_interval(MYSQL_THD thd,
                                       struct st_mysql_sys_var *var,
                                       void *var_ptr, const void *save);
static void update_maria_group_commit(MYSQL_THD thd,
                                      struct st_mysql_sys_var *var,
                                      void *var_ptr, const void *save);
static void update_maria_group_commit_interval(MYSQL_THD thd,
                                           struct st_mysql_sys_var *var,
                                           void *var_ptr, const void *save);
/** After that many consecutive recovery failures, remove logs */
static ulong force_start_after_recovery_failures;
static void update_log_file_size(MYSQL_THD thd,
                                 struct st_mysql_sys_var *var,
                                 void *var_ptr, const void *save);

/* The 4096 is there because of MariaDB privilege tables */
static MYSQL_SYSVAR_ULONG(block_size, maria_block_size,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "Block size to be used for Aria index pages.", 0, 0,
       MARIA_KEY_BLOCK_LENGTH, 4096,
       MARIA_MAX_KEY_BLOCK_LENGTH, MARIA_MIN_KEY_BLOCK_LENGTH);

static MYSQL_SYSVAR_ULONG(checkpoint_interval, checkpoint_interval,
       PLUGIN_VAR_RQCMDARG,
       "Interval between tries to do an automatic checkpoints. In seconds; 0 means"
       " 'no automatic checkpoints' which makes sense only for testing.",
       NULL, update_checkpoint_interval, 30, 0, UINT_MAX, 1);

static MYSQL_SYSVAR_ULONG(checkpoint_log_activity, maria_checkpoint_min_log_activity,
       PLUGIN_VAR_RQCMDARG,
       "Number of bytes that the transaction log has to grow between checkpoints before a new "
       "checkpoint is written to the log.",
       NULL, NULL, 1024*1024, 0, UINT_MAX, 1);

static MYSQL_SYSVAR_ULONG(force_start_after_recovery_failures,
       force_start_after_recovery_failures,
       /*
         Read-only because setting it on the fly has no useful effect,
         should be set on command-line.
       */
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "Number of consecutive log recovery failures after which logs will be"
       " automatically deleted to cure the problem; 0 (the default) disables"
       " the feature.", NULL, NULL, 0, 0, UINT_MAX8, 1);

static MYSQL_SYSVAR_BOOL(page_checksum, maria_page_checksums, 0,
       "Maintain page checksums (can be overridden per table "
       "with PAGE_CHECKSUM clause in CREATE TABLE)", 0, 0, 1);

/* It is only command line argument */
static MYSQL_SYSVAR_CONST_STR(log_dir_path, maria_data_root,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "Path to the directory where to store transactional log",
       NULL, NULL, mysql_real_data_home);

static MYSQL_SYSVAR_ULONG(log_file_size, log_file_size,
       PLUGIN_VAR_RQCMDARG,
       "Limit for transaction log size",
       NULL, update_log_file_size, TRANSLOG_FILE_SIZE,
       TRANSLOG_MIN_FILE_SIZE, 0xffffffffL, TRANSLOG_PAGE_SIZE);

static MYSQL_SYSVAR_ENUM(group_commit, maria_group_commit,
       PLUGIN_VAR_RQCMDARG,
       "Specifies Aria group commit mode. "
       "Possible values are \"none\" (no group commit), "
       "\"hard\" (with waiting to actual commit), "
       "\"soft\" (no wait for commit (DANGEROUS!!!))",
       NULL, update_maria_group_commit,
       TRANSLOG_GCOMMIT_NONE, &maria_group_commit_typelib);

static MYSQL_SYSVAR_ULONG(group_commit_interval, maria_group_commit_interval,
       PLUGIN_VAR_RQCMDARG,
       "Interval between commits in microseconds (1/1000000 sec)."
       " 0 stands for no waiting"
       " for other threads to come and do a commit in \"hard\" mode and no"
       " sync()/commit at all in \"soft\" mode.  Option has only an effect"
       " if aria_group_commit is used",
       NULL, update_maria_group_commit_interval, 0, 0, UINT_MAX, 1);

static MYSQL_SYSVAR_ENUM(log_purge_type, log_purge_type,
       PLUGIN_VAR_RQCMDARG,
       "Specifies how Aria transactional log will be purged",
       NULL, NULL, TRANSLOG_PURGE_IMMIDIATE,
       &maria_translog_purge_type_typelib);

static MYSQL_SYSVAR_ULONGLONG(max_sort_file_size,
       maria_max_temp_length, PLUGIN_VAR_RQCMDARG,
       "Don't use the fast sort index method to created index if the "
       "temporary file would get bigger than this.",
       0, 0, MAX_FILE_SIZE & ~((ulonglong) (1*MB-1)),
       0, MAX_FILE_SIZE, 1*MB);

static MYSQL_SYSVAR_ULONG(pagecache_age_threshold,
       pagecache_age_threshold, PLUGIN_VAR_RQCMDARG,
       "This characterizes the number of hits a hot block has to be untouched "
       "until it is considered aged enough to be downgraded to a warm block. "
       "This specifies the percentage ratio of that number of hits to the "
       "total number of blocks in the page cache.", 0, 0,
       300, 100, ~ (ulong) 0L, 100);

static MYSQL_SYSVAR_ULONGLONG(pagecache_buffer_size, pagecache_buffer_size,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "The size of the buffer used for index blocks for Aria tables. "
       "Increase this to get better index handling (for all reads and "
       "multiple writes) to as much as you can afford.", 0, 0,
       KEY_CACHE_SIZE, 8192*16L, ~(ulonglong) 0, 1);

static MYSQL_SYSVAR_ULONG(pagecache_division_limit, pagecache_division_limit,
       PLUGIN_VAR_RQCMDARG,
       "The minimum percentage of warm blocks in key cache", 0, 0,
       100,  1, 100, 1);

static MYSQL_SYSVAR_ULONG(pagecache_file_hash_size, pagecache_file_hash_size,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "Number of hash buckets for open and changed files.  If you have a lot of Aria "
       "files open you should increase this for faster flush of changes. A good "
       "value is probably 1/10 of number of possible open Aria files.", 0,0,
       512, 128, 16384, 1);

static MYSQL_SYSVAR_SET(recover_options, maria_recover_options, PLUGIN_VAR_OPCMDARG,
       "Specifies how corrupted tables should be automatically repaired",
       NULL, NULL, HA_RECOVER_BACKUP|HA_RECOVER_QUICK, &maria_recover_typelib);

static MYSQL_THDVAR_ULONG(repair_threads, PLUGIN_VAR_RQCMDARG,
       "Number of threads to use when repairing Aria tables. The value of 1 "
       "disables parallel repair.",
       0, 0, 1, 1, 128, 1);

static MYSQL_THDVAR_ULONGLONG(sort_buffer_size, PLUGIN_VAR_RQCMDARG,
       "The buffer that is allocated when sorting the index when doing a "
       "REPAIR or when creating indexes with CREATE INDEX or ALTER TABLE.",
       NULL, NULL,
       SORT_BUFFER_INIT, MARIA_MIN_SORT_MEMORY, SIZE_T_MAX/16, 1);

static MYSQL_THDVAR_ENUM(stats_method, PLUGIN_VAR_RQCMDARG,
       "Specifies how Aria index statistics collection code should treat "
       "NULLs", 0, 0, 0, &maria_stats_method_typelib);

static MYSQL_SYSVAR_ENUM(sync_log_dir, sync_log_dir, PLUGIN_VAR_RQCMDARG,
       "Controls syncing directory after log file growth and new file "
       "creation", NULL, NULL, TRANSLOG_SYNC_DIR_NEWFILE,
       &maria_sync_log_dir_typelib);

#ifdef USE_ARIA_FOR_TMP_TABLES
#define USE_ARIA_FOR_TMP_TABLES_VAL 1
#else
#define USE_ARIA_FOR_TMP_TABLES_VAL 0
#endif
my_bool use_maria_for_temp_tables= USE_ARIA_FOR_TMP_TABLES_VAL;

static MYSQL_SYSVAR_BOOL(used_for_temp_tables,
       use_maria_for_temp_tables, PLUGIN_VAR_READONLY | PLUGIN_VAR_NOCMDOPT,
       "Whether temporary tables should be MyISAM or Aria", 0, 0,
       1);

static MYSQL_SYSVAR_BOOL(encrypt_tables, maria_encrypt_tables,
                         PLUGIN_VAR_OPCMDARG,
       "Encrypt tables (only for tables with ROW_FORMAT=PAGE (default) "
       "and not FIXED/DYNAMIC)",
       0, 0, 0);

#if defined HAVE_PSI_INTERFACE && !defined EMBEDDED_LIBRARY

static PSI_mutex_info all_aria_mutexes[]=
{
  { &key_THR_LOCK_maria, "THR_LOCK_maria", PSI_FLAG_GLOBAL},
  { &key_LOCK_soft_sync, "LOCK_soft_sync", PSI_FLAG_GLOBAL},
  { &key_LOCK_trn_list, "LOCK_trn_list", PSI_FLAG_GLOBAL},
  { &key_SHARE_BITMAP_lock, "SHARE::bitmap::bitmap_lock", 0},
  { &key_SORT_INFO_mutex, "SORT_INFO::mutex", 0},
  { &key_TRANSLOG_BUFFER_mutex, "TRANSLOG_BUFFER::mutex", 0},
  { &key_TRANSLOG_DESCRIPTOR_dirty_buffer_mask_lock, "TRANSLOG_DESCRIPTOR::dirty_buffer_mask_lock", 0},
  { &key_TRANSLOG_DESCRIPTOR_sent_to_disk_lock, "TRANSLOG_DESCRIPTOR::sent_to_disk_lock", 0},
  { &key_TRANSLOG_DESCRIPTOR_log_flush_lock, "TRANSLOG_DESCRIPTOR::log_flush_lock", 0},
  { &key_TRANSLOG_DESCRIPTOR_file_header_lock, "TRANSLOG_DESCRIPTOR::file_header_lock", 0},
  { &key_TRANSLOG_DESCRIPTOR_unfinished_files_lock, "TRANSLOG_DESCRIPTOR::unfinished_files_lock", 0},
  { &key_TRANSLOG_DESCRIPTOR_purger_lock, "TRANSLOG_DESCRIPTOR::purger_lock", 0},
  { &key_SHARE_intern_lock, "SHARE::intern_lock", 0},
  { &key_SHARE_key_del_lock, "SHARE::key_del_lock", 0},
  { &key_SHARE_close_lock, "SHARE::close_lock", 0},
  { &key_SERVICE_THREAD_CONTROL_lock, "SERVICE_THREAD_CONTROL::LOCK_control", 0},
  { &key_TRN_state_lock, "TRN::state_lock", 0},
  { &key_PAGECACHE_cache_lock, "PAGECACHE::cache_lock", 0}
};

static PSI_cond_info all_aria_conds[]=
{
  { &key_COND_soft_sync, "COND_soft_sync", PSI_FLAG_GLOBAL},
  { &key_SHARE_key_del_cond, "SHARE::key_del_cond", 0},
  { &key_SERVICE_THREAD_CONTROL_cond, "SERVICE_THREAD_CONTROL::COND_control", 0},
  { &key_SORT_INFO_cond, "SORT_INFO::cond", 0},
  { &key_SHARE_BITMAP_cond, "BITMAP::bitmap_cond", 0},
  { &key_TRANSLOG_BUFFER_waiting_filling_buffer, "TRANSLOG_BUFFER::waiting_filling_buffer", 0},
  { &key_TRANSLOG_BUFFER_prev_sent_to_disk_cond, "TRANSLOG_BUFFER::prev_sent_to_disk_cond", 0},
  { &key_TRANSLOG_DESCRIPTOR_log_flush_cond, "TRANSLOG_DESCRIPTOR::log_flush_cond", 0},
  { &key_TRANSLOG_DESCRIPTOR_new_goal_cond, "TRANSLOG_DESCRIPTOR::new_goal_cond", 0}
};

static PSI_rwlock_info all_aria_rwlocks[]=
{
  { &key_KEYINFO_root_lock, "KEYINFO::root_lock", 0},
  { &key_SHARE_mmap_lock, "SHARE::mmap_lock", 0},
  { &key_TRANSLOG_DESCRIPTOR_open_files_lock, "TRANSLOG_DESCRIPTOR::open_files_lock", 0}
};

static PSI_thread_info all_aria_threads[]=
{
  { &key_thread_checkpoint, "checkpoint_background", PSI_FLAG_GLOBAL},
  { &key_thread_soft_sync, "soft_sync_background", PSI_FLAG_GLOBAL},
  { &key_thread_find_all_keys, "thr_find_all_keys", 0}
};

static PSI_file_info all_aria_files[]=
{
  { &key_file_translog, "translog", 0},
  { &key_file_kfile, "MAI", 0},
  { &key_file_dfile, "MAD", 0},
  { &key_file_control, "control", PSI_FLAG_GLOBAL}
};

# ifdef HAVE_PSI_STAGE_INTERFACE
static PSI_stage_info *all_aria_stages[]=
{
  & stage_waiting_for_a_resource
};
# endif /* HAVE_PSI_STAGE_INTERFACE */

static void init_aria_psi_keys(void)
{
  const char* category= "aria";
  int count;

  count= array_elements(all_aria_mutexes);
  mysql_mutex_register(category, all_aria_mutexes, count);

  count= array_elements(all_aria_rwlocks);
  mysql_rwlock_register(category, all_aria_rwlocks, count);

  count= array_elements(all_aria_conds);
  mysql_cond_register(category, all_aria_conds, count);

  count= array_elements(all_aria_threads);
  mysql_thread_register(category, all_aria_threads, count);

  count= array_elements(all_aria_files);
  mysql_file_register(category, all_aria_files, count);
# ifdef HAVE_PSI_STAGE_INTERFACE
  count= array_elements(all_aria_stages);
  mysql_stage_register(category, all_aria_stages, count);
# endif /* HAVE_PSI_STAGE_INTERFACE */
}
#else
#define init_aria_psi_keys() /* no-op */
#endif /* HAVE_PSI_INTERFACE */

const LEX_CSTRING MA_CHECK_INFO= { STRING_WITH_LEN("info") };
const LEX_CSTRING MA_CHECK_WARNING= { STRING_WITH_LEN("warning") };
const LEX_CSTRING MA_CHECK_ERROR= { STRING_WITH_LEN("error") };

/*****************************************************************************
** MARIA tables
*****************************************************************************/

static handler *maria_create_handler(handlerton *hton,
                                     TABLE_SHARE * table,
                                     MEM_ROOT *mem_root)
{
  return new (mem_root) ha_maria(hton, table);
}


static void _ma_check_print(HA_CHECK *param, const LEX_CSTRING *msg_type,
                            const char *msgbuf)
{
  if (msg_type == &MA_CHECK_INFO)
    sql_print_information("%s.%s: %s", param->db_name, param->table_name,
                          msgbuf);
  else if (msg_type == &MA_CHECK_WARNING)
    sql_print_warning("%s.%s: %s", param->db_name, param->table_name,
                      msgbuf);
  else
    sql_print_error("%s.%s: %s", param->db_name, param->table_name, msgbuf);
}


// collect errors printed by maria_check routines

static void _ma_check_print_msg(HA_CHECK *param, const LEX_CSTRING *msg_type,
                                const char *fmt, va_list args)
{
  THD *thd= (THD *) param->thd;
  size_t msg_length __attribute__((unused));
  char msgbuf[MYSQL_ERRMSG_SIZE];

  if (param->testflag & T_SUPPRESS_ERR_HANDLING)
    return;

  msg_length= my_vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
  msgbuf[sizeof(msgbuf) - 1]= 0;                // healthy paranoia

  DBUG_PRINT(msg_type->str, ("message: %s", msgbuf));

  if (!thd->vio_ok())
  {
    _ma_check_print(param, msg_type, msgbuf);
    return;
  }

  if (param->testflag &
      (T_CREATE_MISSING_KEYS | T_SAFE_REPAIR | T_AUTO_REPAIR))
  {
    myf flag= 0;
    if (msg_type == &MA_CHECK_INFO)
      flag= ME_NOTE;
    else if (msg_type == &MA_CHECK_WARNING)
      flag= ME_WARNING;
    my_message(ER_NOT_KEYFILE, msgbuf, MYF(flag));
    if (thd->variables.log_warnings > 2)
      _ma_check_print(param, msg_type, msgbuf);
    return;
  }
  print_check_msg(thd, param->db_name, param->table_name,
                  param->op_name, msg_type->str, msgbuf, 0);
  if (thd->variables.log_warnings > 2)
    _ma_check_print(param, msg_type, msgbuf);
  return;
}


/*
  Convert TABLE object to Maria key and column definition

  SYNOPSIS
    table2maria()
      table_arg   in     TABLE object.
      keydef_out  out    Maria key definition.
      recinfo_out out    Maria column definition.
      records_out out    Number of fields.

  DESCRIPTION
    This function will allocate and initialize Maria key and column
    definition for further use in ma_create or for a check for underlying
    table conformance in merge engine.

    The caller needs to free *recinfo_out after use. Since *recinfo_out
    and *keydef_out are allocated with a my_multi_malloc, *keydef_out
    is freed automatically when *recinfo_out is freed.

  RETURN VALUE
    0  OK
    # error code
*/

static int table2maria(TABLE *table_arg, data_file_type row_type,
                       MARIA_KEYDEF **keydef_out,
                       MARIA_COLUMNDEF **recinfo_out, uint *records_out,
                       MARIA_CREATE_INFO *create_info)
{
  uint i, j, recpos, minpos, fieldpos, temp_length, length;
  enum ha_base_keytype type= HA_KEYTYPE_BINARY;
  uchar *record;
  KEY *pos;
  MARIA_KEYDEF *keydef;
  MARIA_COLUMNDEF *recinfo, *recinfo_pos;
  HA_KEYSEG *keyseg;
  TABLE_SHARE *share= table_arg->s;
  uint options= share->db_options_in_use;
  DBUG_ENTER("table2maria");

  if (row_type == BLOCK_RECORD)
    options|= HA_OPTION_PACK_RECORD;

  if (!(my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
          recinfo_out, (share->fields * 2 + 2) * sizeof(MARIA_COLUMNDEF),
          keydef_out, share->keys * sizeof(MARIA_KEYDEF),
          &keyseg,
          (share->key_parts + share->keys) * sizeof(HA_KEYSEG),
          NullS)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM); /* purecov: inspected */
  keydef= *keydef_out;
  recinfo= *recinfo_out;
  pos= table_arg->key_info;
  for (i= 0; i < share->keys; i++, pos++)
  {
    keydef[i].flag= (uint16) (pos->flags & (HA_NOSAME | HA_FULLTEXT |
                                            HA_SPATIAL));
    keydef[i].key_alg= pos->algorithm == HA_KEY_ALG_UNDEF ?
      (pos->flags & HA_SPATIAL ? HA_KEY_ALG_RTREE : HA_KEY_ALG_BTREE) :
      pos->algorithm;
    keydef[i].block_length= pos->block_size;
    keydef[i].seg= keyseg;
    keydef[i].keysegs= pos->user_defined_key_parts;
    for (j= 0; j < pos->user_defined_key_parts; j++)
    {
      Field *field= pos->key_part[j].field;

      if (!table_arg->field[field->field_index]->stored_in_db())
      {
        my_free(*recinfo_out);
        if (table_arg->s->long_unique_table)
        {
          my_error(ER_TOO_LONG_KEY, MYF(0), table_arg->file->max_key_length());
          DBUG_RETURN(HA_ERR_INDEX_COL_TOO_LONG);
        }
        my_error(ER_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN, MYF(0));
        DBUG_RETURN(HA_ERR_UNSUPPORTED);
      }

      type= field->key_type();
      keydef[i].seg[j].flag= pos->key_part[j].key_part_flag;

      if (options & HA_OPTION_PACK_KEYS ||
          (pos->flags & (HA_PACK_KEY | HA_BINARY_PACK_KEY |
                         HA_SPACE_PACK_USED)))
      {
        if (pos->key_part[j].length > 8 &&
            (type == HA_KEYTYPE_TEXT ||
             type == HA_KEYTYPE_NUM ||
             (type == HA_KEYTYPE_BINARY && !field->zero_pack())))
        {
          /* No blobs here */
          if (j == 0)
            keydef[i].flag|= HA_PACK_KEY;
          if (!(field->flags & ZEROFILL_FLAG) &&
              (field->type() == MYSQL_TYPE_STRING ||
               field->type() == MYSQL_TYPE_VAR_STRING ||
               ((int) (pos->key_part[j].length - field->decimals())) >= 4))
            keydef[i].seg[j].flag|= HA_SPACE_PACK;
        }
        else if (j == 0 && (!(pos->flags & HA_NOSAME) || pos->key_length > 16))
          keydef[i].flag|= HA_BINARY_PACK_KEY;
      }
      keydef[i].seg[j].type= (int) type;
      keydef[i].seg[j].start= pos->key_part[j].offset;
      keydef[i].seg[j].length= pos->key_part[j].length;
      keydef[i].seg[j].bit_start= keydef[i].seg[j].bit_length= 0;
      keydef[i].seg[j].bit_pos= 0;
      keydef[i].seg[j].language= field->charset()->number;

      if (field->null_ptr)
      {
        keydef[i].seg[j].null_bit= field->null_bit;
        keydef[i].seg[j].null_pos= (uint) (field->null_ptr-
                                           (uchar*) table_arg->record[0]);
      }
      else
      {
        keydef[i].seg[j].null_bit= 0;
        keydef[i].seg[j].null_pos= 0;
      }
      if (field->type() == MYSQL_TYPE_BLOB ||
          field->type() == MYSQL_TYPE_GEOMETRY)
      {
        keydef[i].seg[j].flag|= HA_BLOB_PART;
        /* save number of bytes used to pack length */
        keydef[i].seg[j].bit_start= (uint) (field->pack_length() -
                                            portable_sizeof_char_ptr);
      }
      else if (field->type() == MYSQL_TYPE_BIT)
      {
        keydef[i].seg[j].bit_length= ((Field_bit *) field)->bit_len;
        keydef[i].seg[j].bit_start= ((Field_bit *) field)->bit_ofs;
        keydef[i].seg[j].bit_pos= (uint) (((Field_bit *) field)->bit_ptr -
                                          (uchar*) table_arg->record[0]);
      }
    }
    keyseg+= pos->user_defined_key_parts;
  }
  if (table_arg->found_next_number_field)
    keydef[share->next_number_index].flag|= HA_AUTO_KEY;
  record= table_arg->record[0];
  recpos= 0;
  recinfo_pos= recinfo;
  create_info->null_bytes= table_arg->s->null_bytes;

  while (recpos < (uint) share->stored_rec_length)
  {
    Field **field, *found= 0;
    minpos= share->stored_rec_length;
    length= 0;

    for (field= table_arg->field; *field; field++)
    {
      if ((fieldpos= (*field)->offset(record)) >= recpos &&
          fieldpos < minpos)
      {
        /* skip null fields */
        if (!(temp_length= (*field)->pack_length_in_rec()))
          continue; /* Skip null-fields */
        if (! found || fieldpos < minpos ||
            (fieldpos == minpos && temp_length < length))
        {
          minpos= fieldpos;
          found= *field;
          length= temp_length;
        }
      }
    }
    DBUG_PRINT("loop", ("found: %p  recpos: %d  minpos: %d  length: %d",
                        found, recpos, minpos, length));
    if (!found)
      break;

    if (found->flags & BLOB_FLAG)
      recinfo_pos->type= FIELD_BLOB;
    else if (found->type() == MYSQL_TYPE_TIMESTAMP)
      recinfo_pos->type= FIELD_NORMAL;
    else if (found->type() == MYSQL_TYPE_VARCHAR)
      recinfo_pos->type= FIELD_VARCHAR;
    else if (!(options & HA_OPTION_PACK_RECORD) ||
             (found->zero_pack() && (found->flags & PRI_KEY_FLAG)))
      recinfo_pos->type= FIELD_NORMAL;
    else if (found->zero_pack())
      recinfo_pos->type= FIELD_SKIP_ZERO;
    else
      recinfo_pos->type= ((length <= 3 ||
                           (found->flags & ZEROFILL_FLAG)) ?
                          FIELD_NORMAL :
                          found->type() == MYSQL_TYPE_STRING ||
                          found->type() == MYSQL_TYPE_VAR_STRING ?
                          FIELD_SKIP_ENDSPACE :
                          FIELD_SKIP_PRESPACE);
    if (found->null_ptr)
    {
      recinfo_pos->null_bit= found->null_bit;
      recinfo_pos->null_pos= (uint) (found->null_ptr -
                                     (uchar*) table_arg->record[0]);
    }
    else
    {
      recinfo_pos->null_bit= 0;
      recinfo_pos->null_pos= 0;
    }
    (recinfo_pos++)->length= (uint16) length;
    recpos= minpos + length;
    DBUG_PRINT("loop", ("length: %d  type: %d",
                        recinfo_pos[-1].length,recinfo_pos[-1].type));
  }
  *records_out= (uint) (recinfo_pos - recinfo);
  DBUG_RETURN(0);
}


/*
  Check for underlying table conformance

  SYNOPSIS
    maria_check_definition()
      t1_keyinfo       in    First table key definition
      t1_recinfo       in    First table record definition
      t1_keys          in    Number of keys in first table
      t1_recs          in    Number of records in first table
      t2_keyinfo       in    Second table key definition
      t2_recinfo       in    Second table record definition
      t2_keys          in    Number of keys in second table
      t2_recs          in    Number of records in second table
      strict           in    Strict check switch

  DESCRIPTION
    This function compares two Maria definitions. By intention it was done
    to compare merge table definition against underlying table definition.
    It may also be used to compare dot-frm and MAI definitions of Maria
    table as well to compare different Maria table definitions.

    For merge table it is not required that number of keys in merge table
    must exactly match number of keys in underlying table. When calling this
    function for underlying table conformance check, 'strict' flag must be
    set to false, and converted merge definition must be passed as t1_*.

    Otherwise 'strict' flag must be set to 1 and it is not required to pass
    converted dot-frm definition as t1_*.

  RETURN VALUE
    0 - Equal definitions.
    1 - Different definitions.

  TODO
    - compare FULLTEXT keys;
    - compare SPATIAL keys;
    - compare FIELD_SKIP_ZERO which is converted to FIELD_NORMAL correctly
      (should be correctly detected in table2maria).

  FIXME:
    maria_check_definition() is never used! CHECK TABLE does not detect the
    corruption! Do maria_check_definition() like check_definition() is done
    by MyISAM (related to MDEV-25803).
*/

int maria_check_definition(MARIA_KEYDEF *t1_keyinfo,
                           MARIA_COLUMNDEF *t1_recinfo,
                           uint t1_keys, uint t1_recs,
                           MARIA_KEYDEF *t2_keyinfo,
                           MARIA_COLUMNDEF *t2_recinfo,
                           uint t2_keys, uint t2_recs, bool strict)
{
  uint i, j;
  DBUG_ENTER("maria_check_definition");
  if ((strict ? t1_keys != t2_keys : t1_keys > t2_keys))
  {
    DBUG_PRINT("error", ("Number of keys differs: t1_keys=%u, t2_keys=%u",
                         t1_keys, t2_keys));
    DBUG_RETURN(1);
  }
  if (t1_recs != t2_recs)
  {
    DBUG_PRINT("error", ("Number of recs differs: t1_recs=%u, t2_recs=%u",
                         t1_recs, t2_recs));
    DBUG_RETURN(1);
  }
  for (i= 0; i < t1_keys; i++)
  {
    HA_KEYSEG *t1_keysegs= t1_keyinfo[i].seg;
    HA_KEYSEG *t2_keysegs= t2_keyinfo[i].seg;
    if (t1_keyinfo[i].flag & HA_FULLTEXT && t2_keyinfo[i].flag & HA_FULLTEXT)
      continue;
    else if (t1_keyinfo[i].flag & HA_FULLTEXT ||
             t2_keyinfo[i].flag & HA_FULLTEXT)
    {
       DBUG_PRINT("error", ("Key %d has different definition", i));
       DBUG_PRINT("error", ("t1_fulltext= %d, t2_fulltext=%d",
                            MY_TEST(t1_keyinfo[i].flag & HA_FULLTEXT),
                            MY_TEST(t2_keyinfo[i].flag & HA_FULLTEXT)));
       DBUG_RETURN(1);
    }
    if (t1_keyinfo[i].flag & HA_SPATIAL && t2_keyinfo[i].flag & HA_SPATIAL)
      continue;
    else if (t1_keyinfo[i].flag & HA_SPATIAL ||
             t2_keyinfo[i].flag & HA_SPATIAL)
    {
       DBUG_PRINT("error", ("Key %d has different definition", i));
       DBUG_PRINT("error", ("t1_spatial= %d, t2_spatial=%d",
                            MY_TEST(t1_keyinfo[i].flag & HA_SPATIAL),
                            MY_TEST(t2_keyinfo[i].flag & HA_SPATIAL)));
       DBUG_RETURN(1);
    }
    if (t1_keyinfo[i].keysegs != t2_keyinfo[i].keysegs ||
        t1_keyinfo[i].key_alg != t2_keyinfo[i].key_alg)
    {
      DBUG_PRINT("error", ("Key %d has different definition", i));
      DBUG_PRINT("error", ("t1_keysegs=%d, t1_key_alg=%d",
                           t1_keyinfo[i].keysegs, t1_keyinfo[i].key_alg));
      DBUG_PRINT("error", ("t2_keysegs=%d, t2_key_alg=%d",
                           t2_keyinfo[i].keysegs, t2_keyinfo[i].key_alg));
      DBUG_RETURN(1);
    }
    for (j=  t1_keyinfo[i].keysegs; j--;)
    {
      uint8 t1_keysegs_j__type= t1_keysegs[j].type;
      /*
        Table migration from 4.1 to 5.1. In 5.1 a *TEXT key part is
        always HA_KEYTYPE_VARTEXT2. In 4.1 we had only the equivalent of
        HA_KEYTYPE_VARTEXT1. Since we treat both the same on MyISAM
        level, we can ignore a mismatch between these types.
      */
      if ((t1_keysegs[j].flag & HA_BLOB_PART) &&
          (t2_keysegs[j].flag & HA_BLOB_PART))
      {
        if ((t1_keysegs_j__type == HA_KEYTYPE_VARTEXT2) &&
            (t2_keysegs[j].type == HA_KEYTYPE_VARTEXT1))
          t1_keysegs_j__type= HA_KEYTYPE_VARTEXT1; /* purecov: tested */
        else if ((t1_keysegs_j__type == HA_KEYTYPE_VARBINARY2) &&
                 (t2_keysegs[j].type == HA_KEYTYPE_VARBINARY1))
          t1_keysegs_j__type= HA_KEYTYPE_VARBINARY1; /* purecov: inspected */
      }

      if (t1_keysegs_j__type != t2_keysegs[j].type ||
          t1_keysegs[j].language != t2_keysegs[j].language ||
          t1_keysegs[j].null_bit != t2_keysegs[j].null_bit ||
          t1_keysegs[j].length != t2_keysegs[j].length)
      {
        DBUG_PRINT("error", ("Key segment %d (key %d) has different "
                             "definition", j, i));
        DBUG_PRINT("error", ("t1_type=%d, t1_language=%d, t1_null_bit=%d, "
                             "t1_length=%d",
                             t1_keysegs[j].type, t1_keysegs[j].language,
                             t1_keysegs[j].null_bit, t1_keysegs[j].length));
        DBUG_PRINT("error", ("t2_type=%d, t2_language=%d, t2_null_bit=%d, "
                             "t2_length=%d",
                             t2_keysegs[j].type, t2_keysegs[j].language,
                             t2_keysegs[j].null_bit, t2_keysegs[j].length));

        DBUG_RETURN(1);
      }
    }
  }

  for (i= 0; i < t1_recs; i++)
  {
    MARIA_COLUMNDEF *t1_rec= &t1_recinfo[i];
    MARIA_COLUMNDEF *t2_rec= &t2_recinfo[i];
    /*
      FIELD_SKIP_ZERO can be changed to FIELD_NORMAL in maria_create,
      see NOTE1 in ma_create.c
    */
    if ((t1_rec->type != t2_rec->type &&
         !(t1_rec->type == (int) FIELD_SKIP_ZERO &&
           t1_rec->length == 1 &&
           t2_rec->type == (int) FIELD_NORMAL)) ||
        t1_rec->length != t2_rec->length ||
        t1_rec->null_bit != t2_rec->null_bit)
    {
      DBUG_PRINT("error", ("Field %d has different definition", i));
      DBUG_PRINT("error", ("t1_type=%d, t1_length=%d, t1_null_bit=%d",
                           t1_rec->type, t1_rec->length, t1_rec->null_bit));
      DBUG_PRINT("error", ("t2_type=%d, t2_length=%d, t2_null_bit=%d",
                           t2_rec->type, t2_rec->length, t2_rec->null_bit));
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


extern "C" {

int _ma_killed_ptr(HA_CHECK *param)
{
  if (!param->thd || likely(thd_killed((THD*)param->thd)) == 0)
    return 0;
  my_errno= HA_ERR_ABORTED_BY_USER;
  return 1;
}


/*
  Report progress to mysqld

  This is a bit more complex than what a normal progress report
  function normally is.

  The reason is that this is called by enable_index/repair which
  is one stage in ALTER TABLE and we can't use the external
  stage/max_stage for this.

  thd_progress_init/thd_progress_next_stage is to be called by
  high level commands like CHECK TABLE or REPAIR TABLE, not
  by sub commands like enable_index().

  In ma_check.c it's easier to work with stages than with a total
  progress, so we use internal stage/max_stage here to keep the
  code simple.
*/

void _ma_report_progress(HA_CHECK *param, ulonglong progress,
                         ulonglong max_progress)
{
  if (param->thd)
    thd_progress_report((THD*)param->thd,
                        progress + max_progress * param->stage,
                        max_progress * param->max_stage);
}


void _ma_check_print_error(HA_CHECK *param, const char *fmt, ...)
{
  va_list args;
  DBUG_ENTER("_ma_check_print_error");
  param->error_printed++;
  param->out_flag |= O_DATA_LOST;
  if (param->testflag & T_SUPPRESS_ERR_HANDLING)
    DBUG_VOID_RETURN;
  va_start(args, fmt);
  _ma_check_print_msg(param, &MA_CHECK_ERROR, fmt, args);
  va_end(args);
  DBUG_VOID_RETURN;
}


void _ma_check_print_info(HA_CHECK *param, const char *fmt, ...)
{
  va_list args;
  DBUG_ENTER("_ma_check_print_info");
  va_start(args, fmt);
  _ma_check_print_msg(param, &MA_CHECK_INFO, fmt, args);
  va_end(args);
  DBUG_VOID_RETURN;
}


void _ma_check_print_warning(HA_CHECK *param, const char *fmt, ...)
{
  va_list args;
  DBUG_ENTER("_ma_check_print_warning");
  param->warning_printed++;
  param->out_flag |= O_DATA_LOST;
  va_start(args, fmt);
  _ma_check_print_msg(param, &MA_CHECK_WARNING, fmt, args);
  va_end(args);
  DBUG_VOID_RETURN;
}

/*
  Create a transaction object

  SYNOPSIS
    info	Maria handler

  RETURN
    0 		ok
    #		Error number (HA_ERR_OUT_OF_MEM)
*/

static int maria_create_trn_for_mysql(MARIA_HA *info)
{
  THD *thd= ((TABLE*) info->external_ref)->in_use;
  TRN *trn= THD_TRN;
  DBUG_ENTER("maria_create_trn_for_mysql");

  if (!trn)  /* no transaction yet - open it now */
  {
    trn= trnman_new_trn(& thd->transaction->wt);
    if (unlikely(!trn))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    thd_set_ha_data(thd, maria_hton, trn);
    if (thd->variables.option_bits & (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
      trans_register_ha(thd, TRUE, maria_hton, trn->trid);
  }
  _ma_set_trn_for_table(info, trn);
  if (!trnman_increment_locked_tables(trn))
  {
    trans_register_ha(thd, FALSE, maria_hton, trn->trid);
    trnman_new_statement(trn);
  }
#ifdef EXTRA_DEBUG
  if (info->lock_type == F_WRLCK &&
      ! (trnman_get_flags(trn) & TRN_STATE_INFO_LOGGED))
  {
    trnman_set_flags(trn, trnman_get_flags(trn) | TRN_STATE_INFO_LOGGED |
                     TRN_STATE_TABLES_CAN_CHANGE);
    (void) translog_log_debug_info(trn, LOGREC_DEBUG_INFO_QUERY,
                                   (uchar*) thd->query(),
                                   thd->query_length());
  }
  else
  {
    DBUG_PRINT("info", ("lock_type: %d  trnman_flags: %u",
                        info->lock_type, trnman_get_flags(trn)));
  }

#endif
  DBUG_RETURN(0);
}

my_bool ma_killed_in_mariadb(MARIA_HA *info)
{
  return (((TABLE*) (info->external_ref))->in_use->killed != 0);
}

void maria_debug_crash_here(const char *keyword)
{
#ifndef DBUG_OFF
  debug_crash_here(keyword);
#endif /* DBUG_OFF */
}

} /* extern "C" */

/**
  Transactional table doing bulk insert with one single UNDO
  (UNDO_BULK_INSERT) and with repair.
*/
#define BULK_INSERT_SINGLE_UNDO_AND_REPAIR    1
/**
  Transactional table doing bulk insert with one single UNDO
  (UNDO_BULK_INSERT) and without repair.
*/
#define BULK_INSERT_SINGLE_UNDO_AND_NO_REPAIR 2
/**
  None of BULK_INSERT_SINGLE_UNDO_AND_REPAIR and
  BULK_INSERT_SINGLE_UNDO_AND_NO_REPAIR.
*/
#define BULK_INSERT_NONE      0

ha_maria::ha_maria(handlerton *hton, TABLE_SHARE *table_arg):
handler(hton, table_arg), file(0),
int_table_flags(HA_NULL_IN_KEY | HA_CAN_FULLTEXT | HA_CAN_SQL_HANDLER |
                HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
                HA_DUPLICATE_POS | HA_CAN_INDEX_BLOBS | HA_AUTO_PART_KEY |
                HA_FILE_BASED | HA_CAN_GEOMETRY | TRANSACTION_STATE |
                HA_CAN_BIT_FIELD | HA_CAN_RTREEKEYS | HA_CAN_REPAIR |
                HA_CAN_VIRTUAL_COLUMNS | HA_CAN_EXPORT |
                HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT |
                HA_CAN_TABLES_WITHOUT_ROLLBACK),
can_enable_indexes(0), bulk_insert_single_undo(BULK_INSERT_NONE)
{}


handler *ha_maria::clone(const char *name __attribute__((unused)),
                         MEM_ROOT *mem_root)
{
  ha_maria *new_handler=
    static_cast <ha_maria *>(handler::clone(file->s->open_file_name.str,
                                            mem_root));
  if (new_handler)
  {
    new_handler->file->state= file->state;
    /* maria_create_trn_for_mysql() is never called for clone() tables */
    new_handler->file->trn= file->trn;
    DBUG_ASSERT(new_handler->file->trn_prev == 0 &&
                new_handler->file->trn_next == 0);
  }
  return new_handler;
}


static const char *ha_maria_exts[]=
{
  MARIA_NAME_IEXT,
  MARIA_NAME_DEXT,
  NullS
};


const char *ha_maria::index_type(uint key_number)
{
  return ((table->key_info[key_number].flags & HA_FULLTEXT) ?
          "FULLTEXT" :
          (table->key_info[key_number].flags & HA_SPATIAL) ?
          "SPATIAL" :
          (table->key_info[key_number].algorithm == HA_KEY_ALG_RTREE) ?
          "RTREE" : "BTREE");
}


ulong ha_maria::index_flags(uint inx, uint part, bool all_parts) const
{
  ulong flags;
  if (table_share->key_info[inx].algorithm == HA_KEY_ALG_FULLTEXT)
    flags= 0;
  else
  if ((table_share->key_info[inx].flags & HA_SPATIAL ||
      table_share->key_info[inx].algorithm == HA_KEY_ALG_RTREE))
  {
    /* All GIS scans are non-ROR scans. We also disable IndexConditionPushdown */
    flags= HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE |
           HA_READ_ORDER | HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR;
  }
  else
  {
    flags= HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE |
          HA_READ_ORDER | HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN;
  }
  return flags;
}


double ha_maria::scan_time()
{
  if (file->s->data_file_type == BLOCK_RECORD)
    return (ulonglong2double(stats.data_file_length - file->s->block_size) /
            file->s->block_size) + 2;
  return handler::scan_time();
}

/*
  We need to be able to store at least 2 keys on an index page as the
  splitting algorithms depends on this. (With only one key on a page
  we also can't use any compression, which may make the index file much
  larger)
  We use MARIA_MAX_KEY_LENGTH to limit the key size as we don't want to use
  too much stack when searching in the b_tree.

  We also need to reserve place for a record pointer (8) and 3 bytes
  per key segment to store the length of the segment + possible null bytes.
  These extra bytes are required here so that maria_create() will surely
  accept any keys created which the returned key data storage length.
*/

uint ha_maria::max_supported_key_length() const
{
  return maria_max_key_length();
}

/* Name is here without an extension */

int ha_maria::open(const char *name, int mode, uint test_if_locked)
{
  uint i;

#ifdef NOT_USED
  /*
    If the user wants to have memory mapped data files, add an
    open_flag. Do not memory map temporary tables because they are
    expected to be inserted and thus extended a lot. Memory mapping is
    efficient for files that keep their size, but very inefficient for
    growing files. Using an open_flag instead of calling ma_extra(...
    HA_EXTRA_MMAP ...) after maxs_open() has the advantage that the
    mapping is not repeated for every open, but just done on the initial
    open, when the MyISAM share is created. Every time the server
    requires to open a new instance of a table it calls this method. We
    will always supply HA_OPEN_MMAP for a permanent table. However, the
    Maria storage engine will ignore this flag if this is a secondary
    open of a table that is in use by other threads already (if the
    Maria share exists already).
  */
  if (!(test_if_locked & HA_OPEN_TMP_TABLE) && opt_maria_use_mmap)
    test_if_locked|= HA_OPEN_MMAP;
#endif

  if (maria_recover_options & HA_RECOVER_ANY)
  {
    /* user asked to trigger a repair if table was not properly closed */
    test_if_locked|= HA_OPEN_ABORT_IF_CRASHED;
  }

  if (aria_readonly)
    test_if_locked|= HA_OPEN_IGNORE_MOVED_STATE;

  if (!(file= maria_open(name, mode, test_if_locked | HA_OPEN_FROM_SQL_LAYER,
                         s3_open_args())))
  {
    if (my_errno == HA_ERR_OLD_FILE)
    {
      push_warning(current_thd, Sql_condition::WARN_LEVEL_NOTE,
                   ER_CRASHED_ON_USAGE,
                   zerofill_error_msg);
    }
    return (my_errno ? my_errno : -1);
  }
  if (aria_readonly)
    file->s->options|= HA_OPTION_READ_ONLY_DATA;

  file->s->chst_invalidator= query_cache_invalidate_by_MyISAM_filename_ref;
  /* Set external_ref, mainly for temporary tables */
  file->external_ref= (void*) table;            // For ma_killed()

  if (test_if_locked & (HA_OPEN_IGNORE_IF_LOCKED | HA_OPEN_TMP_TABLE))
    maria_extra(file, HA_EXTRA_NO_WAIT_LOCK, 0);

  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);
  if (!(test_if_locked & HA_OPEN_WAIT_IF_LOCKED))
    maria_extra(file, HA_EXTRA_WAIT_LOCK, 0);
  if ((data_file_type= file->s->data_file_type) != STATIC_RECORD)
    int_table_flags |= HA_REC_NOT_IN_SEQ;
  if (!file->s->base.born_transactional)
  {
    /*
      INSERT DELAYED cannot work with transactional tables (because it cannot
      stand up to "when client gets ok the data is safe on disk": the record
      may not even be inserted). In the future, we could enable it back (as a
      client doing INSERT DELAYED knows the specificities; but we then should
      make sure to regularly commit in the delayed_insert thread).
    */
    int_table_flags|= HA_CAN_INSERT_DELAYED | HA_NO_TRANSACTIONS;
  }
  else
    int_table_flags|= HA_CRASH_SAFE;

  if (file->s->options & (HA_OPTION_CHECKSUM | HA_OPTION_COMPRESS_RECORD))
    int_table_flags |= HA_HAS_NEW_CHECKSUM;

  /*
    We can only do online backup on transactional tables with checksum.
    Checksums are needed to avoid half writes.
  */
  if (file->s->options & HA_OPTION_PAGE_CHECKSUM &&
      file->s->base.born_transactional)
    int_table_flags |= HA_CAN_ONLINE_BACKUPS;

  /*
    For static size rows, tell MariaDB that we will access all bytes
    in the record when writing it.  This signals MariaDB to initialize
    the full row to ensure we don't get any errors from valgrind and
    that all bytes in the row is properly reset.
  */
  if (file->s->data_file_type == STATIC_RECORD &&
      (file->s->has_varchar_fields || file->s->has_null_fields))
    int_table_flags|= HA_RECORD_MUST_BE_CLEAN_ON_WRITE;

  for (i= 0; i < table->s->keys; i++)
  {
    plugin_ref parser= table->key_info[i].parser;
    if (table->key_info[i].flags & HA_USES_PARSER)
      file->s->keyinfo[i].parser=
        (struct st_mysql_ftparser *)plugin_decl(parser)->info;
    table->key_info[i].block_size= file->s->keyinfo[i].block_length;
  }
  my_errno= 0;

  /* Count statistics of usage for newly open normal files */
  if (file->s->reopen == 1 && ! (test_if_locked & HA_OPEN_TMP_TABLE))
  {
    if (file->s->delay_key_write)
      feature_files_opened_with_delayed_keys++;
  }

  return my_errno;
}


int ha_maria::close(void)
{
  MARIA_HA *tmp= file;
  if (!tmp)
    return 0;
  DBUG_ASSERT(file->trn == 0 || file->trn == &dummy_transaction_object);
  DBUG_ASSERT(file->trn_next == 0 && file->trn_prev == 0);
  file= 0;
  return maria_close(tmp);
}


int ha_maria::write_row(const uchar * buf)
{
  /*
     If we have an auto_increment column and we are writing a changed row
     or a new row, then update the auto_increment value in the record.
  */
  if (table->next_number_field && buf == table->record[0])
  {
    int error;
    if ((error= update_auto_increment()))
      return error;
  }
  return maria_write(file, buf);
}


int ha_maria::check(THD * thd, HA_CHECK_OPT * check_opt)
{
  int error, fatal_error;
  HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);
  MARIA_SHARE *share= file->s;
  const char *old_proc_info;
  TRN *old_trn= file->trn;

  if (!file || !param) return HA_ADMIN_INTERNAL_ERROR;

  unmap_file(file);
  register_handler(file);
  maria_chk_init(param);
  param->thd= thd;
  param->op_name= "check";
  param->db_name= table->s->db.str;
  param->table_name= table->alias.c_ptr();
  param->testflag= check_opt->flags | T_CHECK | T_SILENT;
  param->stats_method= (enum_handler_stats_method)THDVAR(thd,stats_method);

  if (!(table->db_stat & HA_READ_ONLY))
    param->testflag |= T_STATISTICS;
  param->using_global_keycache= 1;

  if (!maria_is_crashed(file) &&
      (((param->testflag & T_CHECK_ONLY_CHANGED) &&
        !(share->state.changed & (STATE_CHANGED | STATE_CRASHED_FLAGS |
                                  STATE_IN_REPAIR)) &&
        share->state.open_count == 0) ||
       ((param->testflag & T_FAST) && (share->state.open_count ==
                                      (uint) (share->global_changed ? 1 :
                                              0)))))
    return HA_ADMIN_ALREADY_DONE;

  maria_chk_init_for_check(param, file);
  param->max_allowed_lsn= translog_get_horizon();

  if ((file->s->state.changed & (STATE_CRASHED_FLAGS | STATE_MOVED)) ==
      STATE_MOVED)
  {
    _ma_check_print_error(param, "%s", zerofill_error_msg);
    return HA_ADMIN_CORRUPT;
  }

  old_proc_info= thd_proc_info(thd, "Checking status");
  thd_progress_init(thd, 3);
  error= maria_chk_status(param, file);                // Not fatal
  /* maria_chk_size() will flush the page cache for this file */
  if (maria_chk_size(param, file))
    error= 1;
  if (!error)
    error|= maria_chk_del(param, file, param->testflag);
  thd_proc_info(thd, "Checking keys");
  thd_progress_next_stage(thd);
  if (!error)
    error= maria_chk_key(param, file);
  thd_proc_info(thd, "Checking data");
  thd_progress_next_stage(thd);
  if (!error)
  {
    if ((!(param->testflag & T_QUICK) &&
         ((share->options &
           (HA_OPTION_PACK_RECORD | HA_OPTION_COMPRESS_RECORD)) ||
          (param->testflag & (T_EXTEND | T_MEDIUM)))) || maria_is_crashed(file))
    {
      ulonglong old_testflag= param->testflag;
      param->testflag |= T_MEDIUM;

      /* BLOCK_RECORD does not need a cache as it is using the page cache */
      if (file->s->data_file_type != BLOCK_RECORD)
        error= init_io_cache(&param->read_cache, file->dfile.file,
                             my_default_record_cache_size, READ_CACHE,
                             share->pack.header_length, 1, MYF(MY_WME));
      if (!error)
        error= maria_chk_data_link(param, file,
                                   MY_TEST(param->testflag & T_EXTEND));

      if (file->s->data_file_type != BLOCK_RECORD)
        end_io_cache(&param->read_cache);
      param->testflag= old_testflag;
    }
  }
  fatal_error= error;
  if (param->error_printed &&
      param->error_printed == (param->skip_lsn_error_count +
                               param->not_visible_rows_found) &&
      !(share->state.changed & (STATE_CRASHED_FLAGS | STATE_IN_REPAIR)))
  {
    _ma_check_print_error(param, "%s", zerofill_error_msg);
    /* This ensures that a future REPAIR TABLE will only do a zerofill */
    file->update|= STATE_MOVED;
    share->state.changed|= STATE_MOVED;
    fatal_error= 0;
  }
  if (!fatal_error)
  {
    if ((share->state.changed & (STATE_CHANGED | STATE_MOVED |
                                 STATE_CRASHED_FLAGS |
                                 STATE_IN_REPAIR | STATE_NOT_ANALYZED)) ||
        (param->testflag & T_STATISTICS) || maria_is_crashed(file))
    {
      file->update |= HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
      mysql_mutex_lock(&share->intern_lock);
      DBUG_PRINT("info", ("Resetting crashed state"));
      share->state.changed&= ~(STATE_CHANGED | STATE_CRASHED_FLAGS |
                               STATE_IN_REPAIR);
      if (!(table->db_stat & HA_READ_ONLY))
      {
        int tmp;
        if ((tmp= maria_update_state_info(param, file,
                                          UPDATE_TIME | UPDATE_OPEN_COUNT |
                                          UPDATE_STAT)))
          error= tmp;
      }
      mysql_mutex_unlock(&share->intern_lock);
      info(HA_STATUS_NO_LOCK | HA_STATUS_TIME | HA_STATUS_VARIABLE |
           HA_STATUS_CONST);

      /*
        Write a 'table is ok' message to error log if table is ok and
        we have written to error log that table was getting checked
      */
      if (!error && !(table->db_stat & HA_READ_ONLY) &&
          !maria_is_crashed(file) && thd->error_printed_to_log &&
          (param->warning_printed || param->error_printed ||
           param->note_printed))
        _ma_check_print_info(param, "Table is fixed");
    }
  }
  else if (!maria_is_crashed(file) && !thd->killed)
  {
    maria_mark_crashed(file);
    file->update |= HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
  }

  /* Reset trn, that may have been set by repair */
  if (old_trn && old_trn != file->trn)
  {
    DBUG_ASSERT(old_trn->used_instances == 0);
    _ma_set_trn_for_table(file, old_trn);
  }
  thd_proc_info(thd, old_proc_info);
  thd_progress_end(thd);
  return error ? HA_ADMIN_CORRUPT : HA_ADMIN_OK;
}


/*
  Analyze the key distribution in the table
  As the table may be only locked for read, we have to take into account that
  two threads may do an analyze at the same time!
*/

int ha_maria::analyze(THD *thd, HA_CHECK_OPT * check_opt)
{
  int error= 0;
  HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);
  MARIA_SHARE *share= file->s;
  const char *old_proc_info;

  if (!param)
    return HA_ADMIN_INTERNAL_ERROR;

  maria_chk_init(param);
  param->thd= thd;
  param->op_name= "analyze";
  param->db_name= table->s->db.str;
  param->table_name= table->alias.c_ptr();
  param->testflag= (T_FAST | T_CHECK | T_SILENT | T_STATISTICS |
                   T_DONT_CHECK_CHECKSUM);
  param->using_global_keycache= 1;
  param->stats_method= (enum_handler_stats_method)THDVAR(thd,stats_method);

  if (!(share->state.changed & STATE_NOT_ANALYZED))
    return HA_ADMIN_ALREADY_DONE;

  old_proc_info= thd_proc_info(thd, "Scanning");
  thd_progress_init(thd, 1);
  error= maria_chk_key(param, file);
  if (!error)
  {
    mysql_mutex_lock(&share->intern_lock);
    error= maria_update_state_info(param, file, UPDATE_STAT);
    mysql_mutex_unlock(&share->intern_lock);
  }
  else if (!maria_is_crashed(file) && !thd->killed)
    maria_mark_crashed(file);
  thd_proc_info(thd, old_proc_info);
  thd_progress_end(thd);
  return error ? HA_ADMIN_CORRUPT : HA_ADMIN_OK;
}

int ha_maria::repair(THD * thd, HA_CHECK_OPT *check_opt)
{
  int error;
  HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);
  ha_rows start_records;
  const char *old_proc_info;

  if (!file || !param)
    return HA_ADMIN_INTERNAL_ERROR;

  maria_chk_init(param);
  param->thd= thd;
  param->op_name= "repair";
  file->error_count=0;

  /*
    The following can only be true if the table was marked as STATE_MOVED
    during a CHECK TABLE and the table has not been used since then
  */
  if ((file->s->state.changed & STATE_MOVED) &&
      !(file->s->state.changed & STATE_CRASHED_FLAGS))
  {
    param->db_name= table->s->db.str;
    param->table_name= table->alias.c_ptr();
    param->testflag= check_opt->flags;
    _ma_check_print_info(param, "Running zerofill on moved table");
    return zerofill(thd, check_opt);
  }

  param->testflag= ((check_opt->flags & ~(T_EXTEND)) |
                   T_SILENT | T_FORCE_CREATE | T_CALC_CHECKSUM |
                   (check_opt->flags & T_EXTEND ? T_REP : T_REP_BY_SORT));
  param->orig_sort_buffer_length= THDVAR(thd, sort_buffer_size);
  param->backup_time= check_opt->start_time;
  start_records= file->state->records;
  old_proc_info= thd_proc_info(thd, "Checking table");
  thd_progress_init(thd, 1);
  while ((error= repair(thd, param, 0)) && param->retry_repair)
  {
    param->retry_repair= 0;
    file->state->records= start_records;
    if (test_all_bits(param->testflag,
                      (uint) (T_RETRY_WITHOUT_QUICK | T_QUICK)))
    {
      param->testflag&= ~(T_RETRY_WITHOUT_QUICK | T_QUICK);
      /* Ensure we don't loose any rows when retrying without quick */
      param->testflag|= T_SAFE_REPAIR;
      if (thd->vio_ok())
        _ma_check_print_info(param, "Retrying repair without quick");
      else
        sql_print_information("Retrying repair of: '%s' without quick",
                              table->s->path.str);
      continue;
    }
    param->testflag &= ~T_QUICK;
    if (param->testflag & T_REP_BY_SORT)
    {
      param->testflag= (param->testflag & ~T_REP_BY_SORT) | T_REP;
      if (thd->vio_ok())
        _ma_check_print_info(param, "Retrying repair with keycache");
      sql_print_information("Retrying repair of: '%s' with keycache",
                            table->s->path.str);
      continue;
    }
    break;
  }
  /*
    Commit is needed in the case of tables are locked to ensure that repair
    is registered in the recovery log
  */
  if (implicit_commit(thd, TRUE))
    error= HA_ADMIN_COMMIT_ERROR;

  if (!error && start_records != file->state->records &&
      !(check_opt->flags & T_VERY_SILENT))
  {
    char llbuff[22], llbuff2[22];
    sql_print_information("Found %s of %s rows when repairing '%s'",
                          llstr(file->state->records, llbuff),
                          llstr(start_records, llbuff2),
                          table->s->path.str);
  }
  thd_proc_info(thd, old_proc_info);
  thd_progress_end(thd);
  return error;
}

int ha_maria::zerofill(THD * thd, HA_CHECK_OPT *check_opt)
{
  int error;
  HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);
  TRN *old_trn;
  MARIA_SHARE *share= file->s;

  if (!file || !param)
    return HA_ADMIN_INTERNAL_ERROR;

  unmap_file(file);
  old_trn= file->trn;
  maria_chk_init(param);
  param->thd= thd;
  param->op_name= "zerofill";
  param->testflag= check_opt->flags | T_SILENT | T_ZEROFILL;
  param->orig_sort_buffer_length= THDVAR(thd, sort_buffer_size);
  param->db_name= table->s->db.str;
  param->table_name= table->alias.c_ptr();

  error=maria_zerofill(param, file, share->open_file_name.str);

  /* Reset trn, that may have been set by repair */
  if (old_trn && old_trn != file->trn)
    _ma_set_trn_for_table(file, old_trn);

  if (!error)
  {
    TrID create_trid= trnman_get_min_safe_trid();
    mysql_mutex_lock(&share->intern_lock);
    share->state.changed|= STATE_NOT_MOVABLE;
    maria_update_state_info(param, file, UPDATE_TIME | UPDATE_OPEN_COUNT);
    _ma_update_state_lsns_sub(share, LSN_IMPOSSIBLE, create_trid,
                              TRUE, TRUE);
    mysql_mutex_unlock(&share->intern_lock);
  }
  return error;
}

int ha_maria::optimize(THD * thd, HA_CHECK_OPT *check_opt)
{
  int error;
  HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);

  if (!file || !param)
    return HA_ADMIN_INTERNAL_ERROR;

  maria_chk_init(param);
  param->thd= thd;
  param->op_name= "optimize";
  param->testflag= (check_opt->flags | T_SILENT | T_FORCE_CREATE |
                   T_REP_BY_SORT | T_STATISTICS | T_SORT_INDEX);
  param->orig_sort_buffer_length= THDVAR(thd, sort_buffer_size);
  thd_progress_init(thd, 1);
  if ((error= repair(thd, param, 1)) && param->retry_repair)
  {
    sql_print_warning("Warning: Optimize table got errno %d on %s.%s, retrying",
                      my_errno, param->db_name, param->table_name);
    param->testflag &= ~T_REP_BY_SORT;
    error= repair(thd, param, 0);
  }
  thd_progress_end(thd);
  return error;
}


int ha_maria::repair(THD *thd, HA_CHECK *param, bool do_optimize)
{
  int error= 0;
  ulonglong local_testflag= param->testflag;
  bool optimize_done= !do_optimize, statistics_done= 0, full_repair_done= 0;
  const char *old_proc_info= thd->proc_info;
  char fixed_name[FN_REFLEN];
  MARIA_SHARE *share= file->s;
  ha_rows rows= file->state->records;
  TRN *old_trn= file->trn;
  my_bool locking= 0;
  DBUG_ENTER("ha_maria::repair");

  /*
    Normally this method is entered with a properly opened table. If the
    repair fails, it can be repeated with more elaborate options. Under
    special circumstances it can happen that a repair fails so that it
    closed the data file and cannot re-open it. In this case file->dfile
    is set to -1. We must not try another repair without an open data
    file. (Bug #25289)
  */
  if (file->dfile.file == -1)
  {
    sql_print_information("Retrying repair of: '%s' failed. "
                          "Please try REPAIR EXTENDED or aria_chk",
                          table->s->path.str);
    DBUG_RETURN(HA_ADMIN_FAILED);
  }

  /*
    If transactions was not enabled for a transactional table then
    file->s->status is not up to date. This is needed for repair_by_sort
    to work
  */
  if (share->base.born_transactional && !share->now_transactional)
    _ma_copy_nontrans_state_information(file);

  param->db_name= table->s->db.str;
  param->table_name= table->alias.c_ptr();
  param->tmpfile_createflag= O_RDWR | O_TRUNC;
  param->using_global_keycache= 1;
  param->thd= thd;
  param->tmpdir= &mysql_tmpdir_list;
  param->out_flag= 0;
  share->state.dupp_key= MI_MAX_KEY;
  strmov(fixed_name, share->open_file_name.str);
  unmap_file(file);

  /*
    Don't lock tables if we have used LOCK TABLE or if we come from
    enable_index()
  */
  if (!thd->locked_tables_mode && ! (param->testflag & T_NO_LOCKS))
  {
    locking= 1;
    if (maria_lock_database(file, table->s->tmp_table ? F_EXTRA_LCK : F_WRLCK))
    {
      _ma_check_print_error(param, ER_THD(thd, ER_CANT_LOCK), my_errno);
      DBUG_RETURN(HA_ADMIN_FAILED);
    }
  }

  if (!do_optimize ||
      (((share->data_file_type == BLOCK_RECORD) ?
        (share->state.changed & STATE_NOT_OPTIMIZED_ROWS) :
        (file->state->del ||
         share->state.split != file->state->records)) &&
       (!(param->testflag & T_QUICK) ||
        (share->state.changed & (STATE_NOT_OPTIMIZED_KEYS |
                                 STATE_NOT_OPTIMIZED_ROWS)))))
  {
    ulonglong key_map= ((local_testflag & T_CREATE_MISSING_KEYS) ?
                        maria_get_mask_all_keys_active(share->base.keys) :
                        share->state.key_map);
    ulonglong save_testflag= param->testflag;
    if (maria_test_if_sort_rep(file, file->state->records, key_map, 0) &&
        (local_testflag & T_REP_BY_SORT))
    {
      local_testflag |= T_STATISTICS;
      param->testflag |= T_STATISTICS;           // We get this for free
      statistics_done= 1;
      /* TODO: Remove BLOCK_RECORD test when parallel works with blocks */
      if (THDVAR(thd,repair_threads) > 1 &&
          share->data_file_type != BLOCK_RECORD)
      {
        char buf[40];
        /* TODO: respect maria_repair_threads variable */
        my_snprintf(buf, 40, "Repair with %d threads", my_count_bits(key_map));
        thd_proc_info(thd, buf);
        param->testflag|= T_REP_PARALLEL;
        error= maria_repair_parallel(param, file, fixed_name,
                                     MY_TEST(param->testflag & T_QUICK));
        /* to reset proc_info, as it was pointing to local buffer */
        thd_proc_info(thd, "Repair done");
      }
      else
      {
        thd_proc_info(thd, "Repair by sorting");
        param->testflag|= T_REP_BY_SORT;
        error= maria_repair_by_sort(param, file, fixed_name,
                                    MY_TEST(param->testflag & T_QUICK));
      }
      if (error && file->create_unique_index_by_sort &&
          share->state.dupp_key != MAX_KEY)
      {
        my_errno= HA_ERR_FOUND_DUPP_KEY;
        print_keydup_error(table, &table->key_info[share->state.dupp_key],
                           MYF(0));
      }
    }
    else
    {
      thd_proc_info(thd, "Repair with keycache");
      param->testflag &= ~(T_REP_BY_SORT | T_REP_PARALLEL);
      error= maria_repair(param, file, fixed_name,
                          MY_TEST(param->testflag & T_QUICK));
    }
    param->testflag= save_testflag | (param->testflag & T_RETRY_WITHOUT_QUICK);
    optimize_done= 1;
    /*
      set full_repair_done if we re-wrote all rows and all keys
      (and thus removed all transid's from the table
    */
    full_repair_done= !MY_TEST(param->testflag & T_QUICK);
  }
  if (!error)
  {
    if ((local_testflag & T_SORT_INDEX) &&
        (share->state.changed & STATE_NOT_SORTED_PAGES))
    {
      optimize_done= 1;
      thd_proc_info(thd, "Sorting index");
      error= maria_sort_index(param, file, fixed_name);
    }
    if (!error && !statistics_done && (local_testflag & T_STATISTICS))
    {
      if (share->state.changed & STATE_NOT_ANALYZED)
      {
        optimize_done= 1;
        thd_proc_info(thd, "Analyzing");
        error= maria_chk_key(param, file);
      }
      else
        local_testflag &= ~T_STATISTICS;        // Don't update statistics
    }
  }
  thd_proc_info(thd, "Saving state");
  if (full_repair_done && !error &&
      !(param->testflag & T_NO_CREATE_RENAME_LSN))
  {
    /* Set trid (needed if the table was moved from another system) */
    share->state.create_trid= trnman_get_min_safe_trid();
  }
  mysql_mutex_lock(&share->intern_lock);
  if (!error)
  {
    if ((share->state.changed & STATE_CHANGED) || maria_is_crashed(file))
    {
      DBUG_PRINT("info", ("Resetting crashed state"));
      share->state.changed&= ~(STATE_CHANGED | STATE_CRASHED_FLAGS |
                               STATE_IN_REPAIR | STATE_MOVED);
      file->update |= HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
    }
    /*
      repair updates share->state.state. Ensure that file->state is up to date
    */
    if (file->state != &share->state.state)
      *file->state= share->state.state;

    if (share->base.auto_key)
      _ma_update_auto_increment_key(param, file, 1);
    if (optimize_done)
      error= maria_update_state_info(param, file,
                                     UPDATE_TIME | UPDATE_OPEN_COUNT |
                                     (local_testflag &
                                      T_STATISTICS ? UPDATE_STAT : 0));
    /* File is repaired; Mark the file as moved to this system */
    (void) _ma_set_uuid(share, 0);

    info(HA_STATUS_NO_LOCK | HA_STATUS_TIME | HA_STATUS_VARIABLE |
         HA_STATUS_CONST);
    if (rows != file->state->records && !(param->testflag & T_VERY_SILENT))
    {
      char llbuff[22], llbuff2[22];
      _ma_check_print_warning(param, "Number of rows changed from %s to %s",
                              llstr(rows, llbuff),
                              llstr(file->state->records, llbuff2));
      /*
        ma_check_print_warning() may generate an error in case of creating keys
        for ALTER TABLE. In this case we should signal an error.
      */
      error= thd->is_error();
    }
  }
  else
  {
    maria_mark_crashed_on_repair(file);
    file->update |= HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
    maria_update_state_info(param, file, 0);
  }
  mysql_mutex_unlock(&share->intern_lock);
  thd_proc_info(thd, old_proc_info);
  thd_progress_end(thd);                        // Mark done
  if (locking)
    maria_lock_database(file, F_UNLCK);

  /* Reset trn, that may have been set by repair */
  if (old_trn && old_trn != file->trn)
    _ma_set_trn_for_table(file, old_trn);
  error= error ? HA_ADMIN_FAILED :
    (optimize_done ?
     (write_log_record_for_repair(param, file) ? HA_ADMIN_FAILED :
      HA_ADMIN_OK) : HA_ADMIN_ALREADY_DONE);
  DBUG_RETURN(error);
}


/*
  Assign table indexes to a specific key cache.
*/

int ha_maria::assign_to_keycache(THD * thd, HA_CHECK_OPT *check_opt)
{
#if 0 && NOT_IMPLEMENTED
  PAGECACHE *new_pagecache= check_opt->pagecache;
  const char *errmsg= 0;
  int error= HA_ADMIN_OK;
  ulonglong map;
  TABLE_LIST *table_list= table->pos_in_table_list;
  DBUG_ENTER("ha_maria::assign_to_keycache");

  table->keys_in_use_for_query.clear_all();

  if (table_list->process_index_hints(table))
    DBUG_RETURN(HA_ADMIN_FAILED);
  map= ~(ulonglong) 0;
  if (!table->keys_in_use_for_query.is_clear_all())
    /* use all keys if there's no list specified by the user through hints */
    map= table->keys_in_use_for_query.to_ulonglong();

  if ((error= maria_assign_to_pagecache(file, map, new_pagecache)))
  {
    char buf[STRING_BUFFER_USUAL_SIZE];
    my_snprintf(buf, sizeof(buf),
                "Failed to flush to index file (errno: %d)", error);
    errmsg= buf;
    error= HA_ADMIN_CORRUPT;
  }

  if (error != HA_ADMIN_OK)
  {
    /* Send error to user */
    HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);
    if (!param)
      return HA_ADMIN_INTERNAL_ERROR;

    maria_chk_init(param);
    param->thd= thd;
    param->op_name= "assign_to_keycache";
    param->db_name= table->s->db.str;
    param->table_name= table->s->table_name.str;
    param->testflag= 0;
    _ma_check_print_error(param, errmsg);
  }
  DBUG_RETURN(error);
#else
  return  HA_ADMIN_NOT_IMPLEMENTED;
#endif
}


/*
  Preload pages of the index file for a table into the key cache.
*/

int ha_maria::preload_keys(THD * thd, HA_CHECK_OPT *check_opt)
{
  ulonglong map;
  TABLE_LIST *table_list= table->pos_in_table_list;

  DBUG_ENTER("ha_maria::preload_keys");

  table->keys_in_use_for_query.clear_all();

  if (table_list->process_index_hints(table))
    DBUG_RETURN(HA_ADMIN_FAILED);

  map= ~(ulonglong) 0;
  /* Check validity of the index references */
  if (!table->keys_in_use_for_query.is_clear_all())
    /* use all keys if there's no list specified by the user through hints */
    map= table->keys_in_use_for_query.to_ulonglong();

  maria_extra(file, HA_EXTRA_PRELOAD_BUFFER_SIZE,
              (void*) &thd->variables.preload_buff_size);

  int error;

  if ((error= maria_preload(file, map, table_list->ignore_leaves)))
  {
    char buf[MYSQL_ERRMSG_SIZE+20];
    const char *errmsg;

    switch (error) {
    case HA_ERR_NON_UNIQUE_BLOCK_SIZE:
      errmsg= "Indexes use different block sizes";
      break;
    case HA_ERR_OUT_OF_MEM:
      errmsg= "Failed to allocate buffer";
      break;
    default:
      my_snprintf(buf, sizeof(buf),
                  "Failed to read from index file (errno: %d)", my_errno);
      errmsg= buf;
    }

    HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);
    if (!param)
      return HA_ADMIN_INTERNAL_ERROR;

    maria_chk_init(param);
    param->thd= thd;
    param->op_name= "preload_keys";
    param->db_name= table->s->db.str;
    param->table_name= table->s->table_name.str;
    param->testflag= 0;
    _ma_check_print_error(param, "%s", errmsg);
    DBUG_RETURN(HA_ADMIN_FAILED);
  }
  DBUG_RETURN(HA_ADMIN_OK);
}


/*
  Disable indexes, making it persistent if requested.

  SYNOPSIS
    disable_indexes()

  DESCRIPTION
    See handler::ha_disable_indexes()

  RETURN
    0  ok
    HA_ERR_WRONG_COMMAND  mode not implemented.
*/

int ha_maria::disable_indexes(key_map map, bool persist)
{
  int error;

  if (!persist)
  {
    /* call a storage engine function to switch the key map */
    DBUG_ASSERT(map.is_clear_all());
    error= maria_disable_indexes(file);
  }
  else
  {
    /* auto-inc key cannot be disabled */
    if (table->s->next_number_index < MAX_KEY)
      DBUG_ASSERT(map.is_set(table->s->next_number_index));

    /* unique keys cannot be disabled either */
    for (uint i=0; i < table->s->keys; i++)
      DBUG_ASSERT(!(table->key_info[i].flags & HA_NOSAME) || map.is_set(i));

    ulonglong ullmap= map.to_ulonglong();

    /* make sure auto-inc key is enabled even if it's > 64 */
    if (map.length() > MARIA_KEYMAP_BITS &&
        table->s->next_number_index < MAX_KEY)
      maria_set_key_active(ullmap, table->s->next_number_index);

    maria_extra(file, HA_EXTRA_NO_KEYS, &ullmap);
    info(HA_STATUS_CONST);                      // Read new key info
    error= 0;
  }
  return error;
}


/*
  Enable indexes, making it persistent if requested.

  SYNOPSIS
    enable_indexes()

  DESCRIPTION
    Enable indexes, which might have been disabled by disable_index() before.
    If persist=false, it works only if both data and indexes are empty,
    since the Aria repair would enable them persistently.
    To be sure in these cases, call handler::delete_all_rows() before.

    See also handler::ha_enable_indexes()

  RETURN
    0  ok
    !=0  Error, among others:
    HA_ERR_CRASHED  data or index is non-empty. Delete all rows and retry.
    HA_ERR_WRONG_COMMAND  mode not implemented.
*/

int ha_maria::enable_indexes(key_map map, bool persist)
{
  int error;
  ha_rows start_rows= file->state->records;
  DBUG_PRINT("info", ("ha_maria::enable_indexes mode: %d", persist));
  if (maria_is_all_keys_active(file->s->state.key_map, file->s->base.keys))
  {
    /* All indexes are enabled already. */
    return 0;
  }

  DBUG_ASSERT(map.is_prefix(table->s->keys));
  if (!persist)
  {
    error= maria_enable_indexes(file);
    /*
       Do not try to repair on error,
       as this could make the enabled state persistent,
       but mode==HA_KEY_SWITCH_ALL forbids it.
    */
  }
  else
  {
    THD *thd= table->in_use;
    HA_CHECK *param= (HA_CHECK*) thd->alloc(sizeof *param);
    if (!param)
      return HA_ADMIN_INTERNAL_ERROR;

    const char *save_proc_info= thd_proc_info(thd, "Creating index");

    maria_chk_init(param);
    param->op_name= "recreating_index";
    param->testflag= (T_SILENT | T_REP_BY_SORT | T_QUICK |
                     T_CREATE_MISSING_KEYS | T_SAFE_REPAIR);
    /*
      Don't lock and unlock table if it's locked.
      Normally table should be locked.  This test is mostly for safety.
    */
    if (likely(file->lock_type != F_UNLCK))
      param->testflag|= T_NO_LOCKS;

    if (file->create_unique_index_by_sort)
      param->testflag|= T_CREATE_UNIQUE_BY_SORT;

    if (bulk_insert_single_undo == BULK_INSERT_SINGLE_UNDO_AND_NO_REPAIR)
    {
      bulk_insert_single_undo= BULK_INSERT_SINGLE_UNDO_AND_REPAIR;
      /*
        Don't bump create_rename_lsn, because UNDO_BULK_INSERT
        should not be skipped in case of crash during repair.
      */
      param->testflag|= T_NO_CREATE_RENAME_LSN;
    }

    param->myf_rw &= ~MY_WAIT_IF_FULL;
    param->orig_sort_buffer_length= THDVAR(thd,sort_buffer_size);
    param->stats_method= (enum_handler_stats_method)THDVAR(thd,stats_method);
    param->tmpdir= &mysql_tmpdir_list;

    /*
      Don't retry repair if we get duplicate key error if
      create_unique_index_by_sort is enabled
      This can be set when doing an ALTER TABLE and enabling unique keys
    */
    if ((error= (repair(thd, param, 0) != HA_ADMIN_OK)) && param->retry_repair &&
        (my_errno != HA_ERR_FOUND_DUPP_KEY ||
         !file->create_unique_index_by_sort))
    {
      sql_print_warning("Warning: Enabling keys got errno %d on %s.%s, "
                        "retrying",
                        my_errno, param->db_name, param->table_name);
      /* Repairing by sort failed. Now try standard repair method. */
      param->testflag &= ~T_REP_BY_SORT;
      file->state->records= start_rows;
      error= (repair(thd, param, 0) != HA_ADMIN_OK);
      /*
        If the standard repair succeeded, clear all error messages which
        might have been set by the first repair. They can still be seen
        with SHOW WARNINGS then.
      */
      if (!error)
        thd->clear_error();
    }
    info(HA_STATUS_CONST);
    thd_proc_info(thd, save_proc_info);
  }
  DBUG_EXECUTE_IF("maria_flush_whole_log",
                  {
                    DBUG_PRINT("maria_flush_whole_log", ("now"));
                    translog_flush(translog_get_horizon());
                  });
  DBUG_EXECUTE_IF("maria_crash_enable_index",
                  {
                    DBUG_PRINT("maria_crash_enable_index", ("now"));
                    DBUG_SUICIDE();
                  });
  return error;
}


/*
  Test if indexes are disabled.


  SYNOPSIS
    indexes_are_disabled()
      no parameters


  RETURN
    0  indexes are not disabled
    1  all indexes are disabled
   [2  non-unique indexes are disabled - NOT YET IMPLEMENTED]
*/

int ha_maria::indexes_are_disabled(void)
{
  return maria_indexes_are_disabled(file);
}


/*
  prepare for a many-rows insert operation
  e.g. - disable indexes (if they can be recreated fast) or
  activate special bulk-insert optimizations

  SYNOPSIS
   start_bulk_insert(rows, flags)
   rows        Rows to be inserted
                0 if we don't know
   flags       Flags to control index creation

  NOTICE
    Do not forget to call end_bulk_insert() later!
*/

void ha_maria::start_bulk_insert(ha_rows rows, uint flags)
{
  DBUG_ENTER("ha_maria::start_bulk_insert");
  THD *thd= table->in_use;
  MARIA_SHARE *share= file->s;
  bool index_disabled= 0;
  DBUG_PRINT("info", ("start_bulk_insert: rows %lu", (ulong) rows));

  /* don't enable row cache if too few rows */
  if ((!rows || rows > MARIA_MIN_ROWS_TO_USE_WRITE_CACHE) && !has_long_unique())
  {
    ulonglong size= thd->variables.read_buff_size, tmp;
    if (rows)
    {
      if (file->state->records)
      {
        MARIA_INFO maria_info;
        maria_status(file, &maria_info, HA_STATUS_NO_LOCK |HA_STATUS_VARIABLE);
        set_if_smaller(size, maria_info.mean_reclength * rows);
      }
      else if (table->s->avg_row_length)
        set_if_smaller(size, (size_t) (table->s->avg_row_length * rows));
    }
    tmp= (ulong) size;                          // Safe becasue of limits
    maria_extra(file, HA_EXTRA_WRITE_CACHE, (void*) &tmp);
  }

  can_enable_indexes= (maria_is_all_keys_active(share->state.key_map,
                                                share->base.keys));
  bulk_insert_single_undo= BULK_INSERT_NONE;

  if (!(specialflag & SPECIAL_SAFE_MODE))
  {
    /*
       Only disable old index if the table was empty and we are inserting
       a lot of rows.
       We should not do this for only a few rows as this is slower and
       we don't want to update the key statistics based of only a few rows.
       Index file rebuild requires an exclusive lock, so if versioning is on
       don't do it (see how ha_maria::store_lock() tries to predict repair).
       We can repair index only if we have an exclusive (TL_WRITE) lock or
       if this is inside an ALTER TABLE, in which case lock_type == TL_UNLOCK.

       To see if table is empty, we shouldn't rely on the old record
       count from our transaction's start (if that old count is 0 but
       now there are records in the table, we would wrongly destroy
       them).  So we need to look at share->state.state.records.  As a
       safety net for now, we don't remove the test of
       file->state->records, because there is uncertainty on what will
       happen during repair if the two states disagree.

       We also have to check in case of transactional tables that the
       user has not used LOCK TABLE on the table twice.
    */
    if ((file->state->records == 0) &&
        (share->state.state.records == 0) && can_enable_indexes &&
        (!rows || rows >= MARIA_MIN_ROWS_TO_DISABLE_INDEXES) &&
        (file->lock.type == TL_WRITE || file->lock.type == TL_UNLOCK) &&
        (!share->have_versioning || !share->now_transactional ||
         file->used_tables->use_count == 1))
    {
      /**
         @todo for a single-row INSERT SELECT, we will go into repair, which
         is more costly (flushes, syncs) than a row write.
      */
      if (file->open_flags & HA_OPEN_INTERNAL_TABLE)
      {
        /* Internal table; If we get a duplicate something is very wrong */
        file->update|= HA_STATE_CHANGED;
        index_disabled= share->base.keys > 0;
        maria_clear_all_keys_active(file->s->state.key_map);
      }
      else
      {
        my_bool all_keys= MY_TEST(flags & HA_CREATE_UNIQUE_INDEX_BY_SORT);
        /*
          Deactivate all indexes that can be recreated fast.
          These include packed keys on which sorting will use more temporary
          space than the max allowed file length or for which the unpacked keys
          will take much more space than packed keys.
          Note that 'rows' may be zero for the case when we don't know how many
          rows we will put into the file.
        */
        MARIA_SHARE *share= file->s;
        MARIA_KEYDEF    *key=share->keyinfo;
        uint          i;

        DBUG_ASSERT(share->state.state.records == 0 &&
                    (!rows || rows >= MARIA_MIN_ROWS_TO_DISABLE_INDEXES));
        for (i=0 ; i < share->base.keys ; i++,key++)
        {
          if (!(key->flag & (HA_SPATIAL | HA_AUTO_KEY | HA_RTREE_INDEX)) &&
              ! maria_too_big_key_for_sort(key,rows) && share->base.auto_key != i+1 &&
              (all_keys || !(key->flag & HA_NOSAME)) &&
              table->key_info[i].algorithm != HA_KEY_ALG_LONG_HASH)
          {
            maria_clear_key_active(share->state.key_map, i);
            index_disabled= 1;
            file->update|= HA_STATE_CHANGED;
            file->create_unique_index_by_sort= all_keys;
          }
        }
      }
      if (share->now_transactional)
      {
        bulk_insert_single_undo= BULK_INSERT_SINGLE_UNDO_AND_NO_REPAIR;
        write_log_record_for_bulk_insert(file);
        /*
          Pages currently in the page cache have type PAGECACHE_LSN_PAGE, we
          are not allowed to overwrite them with PAGECACHE_PLAIN_PAGE, so
          throw them away. It is not losing data, because we just wrote and
          forced an UNDO which will for sure empty the table if we crash. The
          upcoming unique-key insertions however need a proper index, so we
          cannot leave the corrupted on-disk index file, thus we truncate it.

          The following call will log the truncate and update the lsn for the table
          to ensure that all redo's before this will be ignored.
        */
        maria_delete_all_rows(file);
        _ma_tmp_disable_logging_for_table(file, TRUE);
      }
    }
    else if (!file->bulk_insert &&
             (!rows || rows >= MARIA_MIN_ROWS_TO_USE_BULK_INSERT))
    {
      maria_init_bulk_insert(file,
                             (size_t) thd->variables.bulk_insert_buff_size,
                             rows);
    }
  }
  can_enable_indexes= index_disabled;
  DBUG_VOID_RETURN;
}


/*
  end special bulk-insert optimizations,
  which have been activated by start_bulk_insert().

  SYNOPSIS
    end_bulk_insert()
    no arguments

  RETURN
    0     OK
    != 0  Error
*/

int ha_maria::end_bulk_insert()
{
  int first_error, first_errno= 0, error;
  my_bool abort= file->s->deleting, empty_table= 0;
  bool enable_persistently= true;
  DBUG_ENTER("ha_maria::end_bulk_insert");

  if ((first_error= maria_end_bulk_insert(file, abort)))
  {
    first_errno= my_errno;
    abort= 1;
  }

  if ((error= maria_extra(file, HA_EXTRA_NO_CACHE, 0)))
  {
    if (!first_error)
    {
      first_error= error;
      first_errno= my_errno;
    }
    abort= 1;
  }

  if (bulk_insert_single_undo != BULK_INSERT_NONE)
  {
    if (log_not_redoable_operation("BULK_INSERT"))
    {
      /* Got lock timeout. revert back to empty file and give error */
      if (!first_error)
      {
        first_error= 1;
        first_errno= my_errno;
      }
      enable_persistently= false;
      empty_table= 1;
      /*
        Ignore all changed pages, required by _ma_renable_logging_for_table()
      */
      _ma_flush_table_files(file, MARIA_FLUSH_DATA|MARIA_FLUSH_INDEX,
                            FLUSH_IGNORE_CHANGED, FLUSH_IGNORE_CHANGED);
    }
  }

  if (!abort && can_enable_indexes)
  {
    if ((error= enable_indexes(key_map(table->s->keys), enable_persistently)))
    {
      if (!first_error)
      {
        first_error= 1;
        first_errno= my_errno;
      }
    }
  }
  if (bulk_insert_single_undo != BULK_INSERT_NONE)
  {
    /*
      Table was transactional just before start_bulk_insert().
      No need to flush pages if we did a repair (which already flushed).
    */
    if ((error= _ma_reenable_logging_for_table(file,
                                               bulk_insert_single_undo ==
                                               BULK_INSERT_SINGLE_UNDO_AND_NO_REPAIR)) &&
        !empty_table)
    {
      if (!first_error)
      {
        first_error= 1;
        first_errno= my_errno;
      }
    }
    bulk_insert_single_undo= BULK_INSERT_NONE;  // Safety if called again
  }
  if (empty_table)
    maria_delete_all_rows(file);

  can_enable_indexes= 0;
  if (first_error)
    my_errno= first_errno;
  DBUG_RETURN(first_error);
}


bool ha_maria::check_and_repair(THD *thd)
{
  int error, crashed;
  HA_CHECK_OPT check_opt;
  const CSET_STRING query_backup= thd->query_string;
  DBUG_ENTER("ha_maria::check_and_repair");

  check_opt.init();
  check_opt.flags= T_MEDIUM | T_AUTO_REPAIR;

  error= 1;
  if (!aria_readonly &&
      (file->s->state.changed & (STATE_CRASHED_FLAGS | STATE_MOVED)) ==
      STATE_MOVED)
  {
    /* Remove error about crashed table */
    thd->get_stmt_da()->clear_warning_info(thd->query_id);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_CRASHED_ON_USAGE,
                        "Zerofilling moved table %s", table->s->path.str);
    sql_print_information("Zerofilling moved table:  '%s'",
                          table->s->path.str);
    if (!(error= zerofill(thd, &check_opt)))
      DBUG_RETURN(0);
  }

  /*
    if we got this far - the table is crashed.
    but don't auto-repair if maria_recover_options is not set
  */
  if (!maria_recover_options)
    DBUG_RETURN(error);

  error= 0;
  // Don't use quick if deleted rows
  if (!file->state->del && (maria_recover_options & HA_RECOVER_QUICK))
    check_opt.flags |= T_QUICK;

  thd->set_query((char*) table->s->table_name.str,
                 (uint) table->s->table_name.length, system_charset_info);

  if (!(crashed= maria_is_crashed(file)))
  {
    sql_print_warning("Checking table:   '%s'", table->s->path.str);
    crashed= check(thd, &check_opt);
  }

  if (crashed)
  {
    bool save_log_all_errors;
    sql_print_warning("Recovering table: '%s'", table->s->path.str);
    save_log_all_errors= thd->log_all_errors;
    thd->log_all_errors|= (thd->variables.log_warnings > 2);
    check_opt.flags=
      ((maria_recover_options & HA_RECOVER_BACKUP ? T_BACKUP_DATA : 0) |
       (maria_recover_options & HA_RECOVER_FORCE ? 0 : T_SAFE_REPAIR) |
       T_AUTO_REPAIR);
    if (repair(thd, &check_opt))
      error= 1;
    thd->log_all_errors= save_log_all_errors;
  }
  thd->set_query(query_backup);
  DBUG_RETURN(error);
}


bool ha_maria::is_crashed() const
{
  return (file->s->state.changed & (STATE_CRASHED_FLAGS | STATE_MOVED) ||
          (my_disable_locking && file->s->state.open_count));
}

#define CHECK_UNTIL_WE_FULLY_IMPLEMENTED_VERSIONING(msg) \
  do { \
    if (file->lock.type == TL_WRITE_CONCURRENT_INSERT && !table->s->sequence) \
    { \
      my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), msg); \
      return 1; \
    } \
  } while(0)

int ha_maria::update_row(const uchar * old_data, const uchar * new_data)
{
  CHECK_UNTIL_WE_FULLY_IMPLEMENTED_VERSIONING("UPDATE in WRITE CONCURRENT");
  return maria_update(file, old_data, new_data);
}


int ha_maria::delete_row(const uchar * buf)
{
  CHECK_UNTIL_WE_FULLY_IMPLEMENTED_VERSIONING("DELETE in WRITE CONCURRENT");
  return maria_delete(file, buf);
}

int ha_maria::index_read_map(uchar * buf, const uchar * key,
			     key_part_map keypart_map,
			     enum ha_rkey_function find_flag)
{
  DBUG_ASSERT(inited == INDEX);
  register_handler(file);
  int error= maria_rkey(file, buf, active_index, key, keypart_map, find_flag);
  return error;
}


int ha_maria::index_read_idx_map(uchar * buf, uint index, const uchar * key,
				 key_part_map keypart_map,
				 enum ha_rkey_function find_flag)
{
  int error;
  register_handler(file);

  /* Use the pushed index condition if it matches the index we're scanning */
  end_range= NULL;
  if (index == pushed_idx_cond_keyno)
    ma_set_index_cond_func(file, handler_index_cond_check, this);

  error= maria_rkey(file, buf, index, key, keypart_map, find_flag);

  ma_set_index_cond_func(file, NULL, 0);
  return error;
}


int ha_maria::index_read_last_map(uchar * buf, const uchar * key,
				  key_part_map keypart_map)
{
  DBUG_ENTER("ha_maria::index_read_last_map");
  DBUG_ASSERT(inited == INDEX);
  register_handler(file);
  int error= maria_rkey(file, buf, active_index, key, keypart_map,
                        HA_READ_PREFIX_LAST);
  DBUG_RETURN(error);
}


int ha_maria::index_next(uchar * buf)
{
  DBUG_ASSERT(inited == INDEX);
  register_handler(file);
  int error= maria_rnext(file, buf, active_index);
  return error;
}


int ha_maria::index_prev(uchar * buf)
{
  DBUG_ASSERT(inited == INDEX);
  register_handler(file);
  int error= maria_rprev(file, buf, active_index);
  return error;
}


int ha_maria::index_first(uchar * buf)
{
  DBUG_ASSERT(inited == INDEX);
  register_handler(file);
  int error= maria_rfirst(file, buf, active_index);
  return error;
}


int ha_maria::index_last(uchar * buf)
{
  DBUG_ASSERT(inited == INDEX);
  register_handler(file);
  int error= maria_rlast(file, buf, active_index);
  return error;
}


int ha_maria::index_next_same(uchar * buf,
                              const uchar *key __attribute__ ((unused)),
                              uint length __attribute__ ((unused)))
{
  int error;
  DBUG_ASSERT(inited == INDEX);
  register_handler(file);
  /*
    TODO: Delete this loop in Maria 1.5 as versioning will ensure this never
    happens
  */
  do
  {
    error= maria_rnext_same(file,buf);
  } while (error == HA_ERR_RECORD_DELETED);
  return error;
}


int ha_maria::index_init(uint idx, bool sorted)
{
  active_index=idx;
  if (pushed_idx_cond_keyno == idx)
    ma_set_index_cond_func(file, handler_index_cond_check, this);
  return 0;
}


int ha_maria::index_end()
{
  active_index=MAX_KEY;
  ma_set_index_cond_func(file, NULL, 0);
  in_range_check_pushed_down= FALSE;
  ds_mrr.dsmrr_close();
  return 0;
}


int ha_maria::rnd_init(bool scan)
{
  if (scan)
    return maria_scan_init(file);
  return maria_reset(file);                        // Free buffers
}


int ha_maria::rnd_end()
{
  ds_mrr.dsmrr_close();
  /* Safe to call even if we don't have started a scan */
  maria_scan_end(file);
  return 0;
}


int ha_maria::rnd_next(uchar *buf)
{
  register_handler(file);
  return maria_scan(file, buf);
}


int ha_maria::remember_rnd_pos()
{
  register_handler(file);
  return (*file->s->scan_remember_pos)(file, &remember_pos);
}


int ha_maria::restart_rnd_next(uchar *buf)
{
  int error;
  register_handler(file);
  if ((error= (*file->s->scan_restore_pos)(file, remember_pos)))
    return error;
  return rnd_next(buf);
}


int ha_maria::rnd_pos(uchar *buf, uchar *pos)
{
  register_handler(file);
  int error= maria_rrnd(file, buf, my_get_ptr(pos, ref_length));
  return error;
}


void ha_maria::position(const uchar *record)
{
  my_off_t row_position= maria_position(file);
  my_store_ptr(ref, ref_length, row_position);
}


int ha_maria::info(uint flag)
{
  MARIA_INFO maria_info;
  char name_buff[FN_REFLEN];

  (void) maria_status(file, &maria_info, flag);
  if (flag & HA_STATUS_VARIABLE)
  {
    stats.records=           maria_info.records;
    stats.deleted=           maria_info.deleted;
    stats.data_file_length=  maria_info.data_file_length;
    stats.index_file_length= maria_info.index_file_length;
    stats.delete_length=     maria_info.delete_length;
    stats.check_time=        maria_info.check_time;
    stats.mean_rec_length=   maria_info.mean_reclength;
    stats.checksum=          file->state->checksum;
  }
  if (flag & HA_STATUS_CONST)
  {
    TABLE_SHARE *share= table->s;
    stats.max_data_file_length=  maria_info.max_data_file_length;
    stats.max_index_file_length= maria_info.max_index_file_length;
    stats.create_time= maria_info.create_time;
    ref_length= maria_info.reflength;
    share->db_options_in_use= maria_info.options;
    stats.block_size= maria_block_size;
    stats.mrr_length_per_rec= maria_info.reflength + 8; // 8 = MY_MAX(sizeof(void *))

    /* Update share */
    share->keys_in_use.set_prefix(share->keys);
    share->keys_in_use.intersect_extended(maria_info.key_map);
    share->keys_for_keyread.intersect(share->keys_in_use);
    share->db_record_offset= maria_info.record_offset;
    if (share->key_parts)
    {
      double *from= maria_info.rec_per_key;
      KEY *key, *key_end;
      for (key= table->key_info, key_end= key + share->keys;
           key < key_end ; key++)
      {
        ulong *to= key->rec_per_key;
        /* Some temporary tables does not allocate rec_per_key */
        if (to)
        {
          for (ulong *end= to+ key->user_defined_key_parts ;
               to < end ;
               to++, from++)
            *to= (ulong) (*from + 0.5);
        }
      }
    }
    /*
       Set data_file_name and index_file_name to point at the symlink value
       if table is symlinked (Ie;  Real name is not same as generated name)
    */
    data_file_name= index_file_name= 0;
    fn_format(name_buff, file->s->open_file_name.str, "", MARIA_NAME_DEXT,
              MY_APPEND_EXT | MY_UNPACK_FILENAME);
    if (strcmp(name_buff, maria_info.data_file_name) &&
        maria_info.data_file_name[0])
      data_file_name= maria_info.data_file_name;
    fn_format(name_buff, file->s->open_file_name.str, "", MARIA_NAME_IEXT,
              MY_APPEND_EXT | MY_UNPACK_FILENAME);
    if (strcmp(name_buff, maria_info.index_file_name) &&
        maria_info.index_file_name[0])
      index_file_name=maria_info.index_file_name;
  }
  if (flag & HA_STATUS_ERRKEY)
  {
    errkey= maria_info.errkey;
    my_store_ptr(dup_ref, ref_length, maria_info.dup_key_pos);
  }
  if (flag & HA_STATUS_TIME)
    stats.update_time= maria_info.update_time;
  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value= maria_info.auto_increment;

  return 0;
}


int ha_maria::extra(enum ha_extra_function operation)
{
  int tmp;
  TRN *old_trn= file->trn;
  if ((specialflag & SPECIAL_SAFE_MODE) && operation == HA_EXTRA_KEYREAD)
    return 0;
#ifdef NOT_USED
  if (operation == HA_EXTRA_MMAP && !opt_maria_use_mmap)
    return 0;
#endif
  if (operation == HA_EXTRA_WRITE_CACHE && has_long_unique())
    return 0;

  /*
    We have to set file->trn here because in some cases we call
    extern_lock(F_UNLOCK) (which resets file->trn) followed by maria_close()
    without calling commit/rollback in between.  If file->trn is not set
    we can't remove file->share from the transaction list in the extra() call.

    In current code we don't have to do this for HA_EXTRA_PREPARE_FOR_RENAME
    as this is only used the intermediate table used by ALTER TABLE which
    is not part of the transaction (it's not in the TRN list). Better to
    keep this for now, to not break anything in a stable release.
    When HA_EXTRA_PREPARE_FOR_RENAME is not handled below, we can change
    the warnings in _ma_remove_table_from_trnman() to asserts.

    table->in_use is not set in the case this is a done as part of closefrm()
    as part of drop table.
  */

  if (file->s->now_transactional && table->in_use &&
      (operation == HA_EXTRA_PREPARE_FOR_DROP ||
       operation == HA_EXTRA_PREPARE_FOR_RENAME ||
       operation == HA_EXTRA_PREPARE_FOR_FORCED_CLOSE))
  {
    THD *thd= table->in_use;
    file->trn= THD_TRN;
  }
  DBUG_ASSERT(file->s->base.born_transactional || file->trn == 0 ||
              file->trn == &dummy_transaction_object);

  tmp= maria_extra(file, operation, 0);
  /*
    Restore trn if it was changed above.
    Note that table could be removed from trn->used_tables and
    trn->used_instances if trn was set and some of the above operations
    was used. This is ok as the table should not be part of any transaction
    after this and thus doesn't need to be part of any of the above lists.
  */
  file->trn= old_trn;
  return tmp;
}

int ha_maria::reset(void)
{
  ma_set_index_cond_func(file, NULL, 0);
  ds_mrr.dsmrr_close();
  if (file->trn)
  {
    /* Next statement is a new statement. Ensure it's logged */
    trnman_set_flags(file->trn,
                     trnman_get_flags(file->trn) & ~TRN_STATE_INFO_LOGGED);
  }
  return maria_reset(file);
}

/* To be used with WRITE_CACHE and EXTRA_CACHE */

int ha_maria::extra_opt(enum ha_extra_function operation, ulong cache_size)
{
  if ((specialflag & SPECIAL_SAFE_MODE) && operation == HA_EXTRA_WRITE_CACHE)
    return 0;
  return maria_extra(file, operation, (void*) &cache_size);
}


bool ha_maria::auto_repair(int error) const
{
  /* Always auto-repair moved tables (error == HA_ERR_OLD_FILE) */
  return ((MY_TEST(maria_recover_options & HA_RECOVER_ANY) &&
           error == HA_ERR_CRASHED_ON_USAGE) ||
          error == HA_ERR_OLD_FILE);

}


int ha_maria::delete_all_rows()
{
  THD *thd= table->in_use;
  TRN *trn= file->trn;
  CHECK_UNTIL_WE_FULLY_IMPLEMENTED_VERSIONING("TRUNCATE in WRITE CONCURRENT");
#ifdef EXTRA_DEBUG
  if (trn && ! (trnman_get_flags(trn) & TRN_STATE_INFO_LOGGED))
  {
    trnman_set_flags(trn, trnman_get_flags(trn) | TRN_STATE_INFO_LOGGED |
                     TRN_STATE_TABLES_CAN_CHANGE);
    (void) translog_log_debug_info(trn, LOGREC_DEBUG_INFO_QUERY,
                                   (uchar*) thd->query(), thd->query_length());
  }
#endif
  /*
    If we are under LOCK TABLES, we have to do a commit as
    delete_all_rows() can't be rolled back
  */
  if (table->in_use->locked_tables_mode && trn &&
      trnman_has_locked_tables(trn))
  {
    int error;
    if ((error= implicit_commit(thd, 1)))
      return error;
  }

  /* Note that this can't be rolled back */
  return maria_delete_all_rows(file);
}


int ha_maria::delete_table(const char *name)
{
  THD *thd= current_thd;
  (void) translog_log_debug_info(0, LOGREC_DEBUG_INFO_QUERY,
                                 (uchar*) thd->query(), thd->query_length());
  return maria_delete_table(name);
}


/* This is mainly for temporary tables, so no logging necessary */

void ha_maria::drop_table(const char *name)
{
  DBUG_ASSERT(!file || file->s->temporary);
  file->s->deleting= 1;                         // Do not flush data
  (void) ha_close();
  (void) maria_delete_table_files(name, 1, MY_WME);
}


void ha_maria::change_table_ptr(TABLE *table_arg, TABLE_SHARE *share)
{
  handler::change_table_ptr(table_arg, share);
  if (file)
    file->external_ref= table_arg;
}


int ha_maria::external_lock(THD *thd, int lock_type)
{
  int result= 0, result2;
  DBUG_ENTER("ha_maria::external_lock");
  file->external_ref= (void*) table;            // For ma_killed()
  /*
    We don't test now_transactional because it may vary between lock/unlock
    and thus confuse our reference counting.
    It is critical to skip non-transactional tables: user-visible temporary
    tables get an external_lock() when read/written for the first time, but no
    corresponding unlock (they just stay locked and are later dropped while
    locked); if a tmp table was transactional, "SELECT FROM non_tmp, tmp"
    would never commit as its "locked_tables" count would stay 1.
    When Maria has has_transactions()==TRUE, open_temporary_table()
    (sql_base.cc) will use TRANSACTIONAL_TMP_TABLE and thus the
    external_lock(F_UNLCK) will happen and we can then allow the user to
    create transactional temporary tables.
  */
  if (file->s->base.born_transactional)
  {
    /* Transactional table */
    if (lock_type != F_UNLCK)
    {
      if (file->trn)
      {
        /* This can only happen with tables created with clone() */
        DBUG_PRINT("info",("file->trn: %p", file->trn));
        trnman_increment_locked_tables(file->trn);
      }

      if (!thd->transaction->on)
      {
        /*
          No need to log REDOs/UNDOs. If this is an internal temporary table
          which will be renamed to a permanent table (like in ALTER TABLE),
          the rename happens after unlocking so will be durable (and the table
          will get its create_rename_lsn).
          Note: if we wanted to enable users to have an old backup and apply
          tons of archived logs to roll-forward, we could then not disable
          REDOs/UNDOs in this case.
        */
        DBUG_PRINT("info", ("Disabling logging for table"));
        _ma_tmp_disable_logging_for_table(file, TRUE);
        file->autocommit= 0;
      }
      else
        file->autocommit= !(thd->variables.option_bits &
                            (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN));
#ifndef ARIA_HAS_TRANSACTIONS
      /*
        Until Aria has full transactions support, including MVCC support for
        delete and update and purging of old states, we have to commit for
        every statement.
      */
      file->autocommit=1;
#endif
    }
    else
    {
      /* We have to test for THD_TRN to protect against implicit commits */
      TRN *trn= (file->trn != &dummy_transaction_object && THD_TRN ? file->trn : 0);
      /* End of transaction */

      /*
        We always re-enable, don't rely on thd->transaction.on as it is
        sometimes reset to true after unlocking (see mysql_truncate() for a
        partitioned table based on Maria).
        Note that we can come here without having an exclusive lock on the
        table, for example in this case:
        external_lock(F_(WR|RD)LCK); thr_lock() which fails due to lock
        abortion; external_lock(F_UNLCK). Fortunately, the re-enabling happens
        only if we were the thread which disabled logging.
      */
      if (_ma_reenable_logging_for_table(file, TRUE))
        DBUG_RETURN(1);
      _ma_reset_trn_for_table(file);
      /*
        Ensure that file->state points to the current number of rows. This
        is needed if someone calls maria_info() without first doing an
        external lock of the table
      */
      file->state= &file->s->state.state;
      if (trn)
      {
        DBUG_PRINT("info",
                   ("locked_tables: %u", trnman_has_locked_tables(trn)));
        DBUG_ASSERT(trnman_has_locked_tables(trn) > 0);
        if (trnman_has_locked_tables(trn) &&
            !trnman_decrement_locked_tables(trn))
        {
          /*
            OK should not have been sent to client yet (ACID).
            This is a bit excessive, ACID requires this only if there are some
            changes to commit (rollback shouldn't be tested).
          */
          DBUG_ASSERT(!thd->get_stmt_da()->is_sent() ||
                      thd->killed);
          /*
            If autocommit, commit transaction. This can happen when open and
            lock tables as part of creating triggers, in which case commit
            is not called.
            Until ARIA_HAS_TRANSACTIONS is not defined, always commit.
          */
          if (file->autocommit)
          {
            if (ma_commit(trn))
              result= HA_ERR_COMMIT_ERROR;
            thd_set_ha_data(thd, maria_hton, 0);
          }
        }
        trnman_set_flags(trn, trnman_get_flags(trn) & ~ TRN_STATE_INFO_LOGGED);
      }
    }
  } /* if transactional table */
  if ((result2= maria_lock_database(file, !table->s->tmp_table ?
                                    lock_type : ((lock_type == F_UNLCK) ?
                                                 F_UNLCK : F_EXTRA_LCK))))
    result= result2;
  if (!file->s->base.born_transactional)
    file->state= &file->s->state.state;         // Restore state if clone

  /* Remember stack end for this thread */
  file->stack_end_ptr= &ha_thd()->mysys_var->stack_ends_here;
  DBUG_RETURN(result);
}

int ha_maria::start_stmt(THD *thd, thr_lock_type lock_type)
{
  TRN *trn;
  if (file->s->base.born_transactional)
  {
    trn= THD_TRN;
    DBUG_ASSERT(trn); // this may be called only after external_lock()
    DBUG_ASSERT(trnman_has_locked_tables(trn));
    DBUG_ASSERT(lock_type != TL_UNLOCK);
    DBUG_ASSERT(file->trn == trn);

    /*
      As external_lock() was already called, don't increment locked_tables.
      Note that we call the function below possibly several times when
      statement starts (once per table). This is ok as long as that function
      does cheap operations. Otherwise, we will need to do it only on first
      call to start_stmt().
    */
    trnman_new_statement(trn);

#ifdef EXTRA_DEBUG
    if (!(trnman_get_flags(trn) & TRN_STATE_INFO_LOGGED) &&
        trnman_get_flags(trn) & TRN_STATE_TABLES_CAN_CHANGE)
    {
      trnman_set_flags(trn, trnman_get_flags(trn) | TRN_STATE_INFO_LOGGED);
      (void) translog_log_debug_info(trn, LOGREC_DEBUG_INFO_QUERY,
                                     (uchar*) thd->query(),
                                     thd->query_length());
    }
#endif
  }
  return 0;
}


/*
  Reset THD_TRN and all file->trn related to the transaction
  This is needed as some calls, like extra() or external_lock() may access
  it before next transaction is started
*/

static void reset_thd_trn(THD *thd, MARIA_HA *first_table)
{
  DBUG_ENTER("reset_thd_trn");
  thd_set_ha_data(thd, maria_hton, 0);
  MARIA_HA *next;
  for (MARIA_HA *table= first_table; table ; table= next)
  {
    next= table->trn_next;
    _ma_reset_trn_for_table(table);

    /*
      If table has changed by this statement, invalidate it from the query
      cache
    */
    if (table->row_changes != table->start_row_changes)
    {
      table->start_row_changes= table->row_changes;
      DBUG_ASSERT(table->s->chst_invalidator != NULL);
      (*table->s->chst_invalidator)(table->s->data_file_name.str);
    }
  }
  DBUG_VOID_RETURN;
}

bool ha_maria::has_active_transaction(THD *thd)
{
  return (maria_hton && THD_TRN);
}

/**
  Performs an implicit commit of the Maria transaction and creates a new
  one.

  This can be considered a hack. When Maria loses HA_NO_TRANSACTIONS it will
  be participant in the connection's transaction and so the implicit commits
  (ha_commit()) (like in end_active_trans()) will do the implicit commit
  without need to call this function which can then be removed.

  @param  thd              THD object
  @param  new_trn          if a new transaction should be created; a new
                           transaction is not needed when we know that the
                           tables will be unlocked very soon.
*/

int ha_maria::implicit_commit(THD *thd, bool new_trn)
{
#ifndef MARIA_CANNOT_ROLLBACK
#error this method should be removed
#endif
  TRN *trn;
  int error;
  uint locked_tables;
  extern my_bool plugins_are_initialized;
  MARIA_HA *used_tables, *trn_next;
  DBUG_ENTER("ha_maria::implicit_commit");

  if (!maria_hton || !plugins_are_initialized || !(trn= THD_TRN))
    DBUG_RETURN(0);
  if (!new_trn && (thd->locked_tables_mode == LTM_LOCK_TABLES ||
                   thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES))
  {
    /*
      No commit inside LOCK TABLES.

      Note that we come here only at the end of the top statement
      (dispatch_command()), we are never committing inside a sub-statement./
    */
    DBUG_PRINT("info", ("locked_tables, skipping"));
    DBUG_RETURN(0);
  }

  /* Prepare to move used_instances and locked tables to new TRN object */
  locked_tables= trnman_has_locked_tables(trn);
  trnman_reset_locked_tables(trn, 0);
  relink_trn_used_instances(&used_tables, trn);

  error= 0;
  if (unlikely(ma_commit(trn)))
    error= HA_ERR_COMMIT_ERROR;
  if (!new_trn)
  {
    reset_thd_trn(thd, used_tables);
    goto end;
  }

  /*
    We need to create a new transaction and put it in THD_TRN. Indeed,
    tables may be under LOCK TABLES, and so they will start the next
    statement assuming they have a trn (see ha_maria::start_stmt()).
  */
  trn= trnman_new_trn(& thd->transaction->wt);
  thd_set_ha_data(thd, maria_hton, trn);
  if (unlikely(trn == NULL))
  {
    reset_thd_trn(thd, used_tables);
    error= HA_ERR_OUT_OF_MEM;
    goto end;
  }
  /*
    Move all locked tables to the new transaction
    We must do it here as otherwise file->thd and file->state may be
    stale pointers. We can't do this in start_stmt() as we don't know
    when we should call _ma_setup_live_state() and in some cases, like
    in check table, we use the table without calling start_stmt().
  */

  for (MARIA_HA *handler= used_tables; handler ;
       handler= trn_next)
  {
    trn_next= handler->trn_next;
    DBUG_ASSERT(handler->s->base.born_transactional);

    /* If handler uses versioning */
    if (handler->s->lock_key_trees)
    {
      /* _ma_set_trn_for_table() will be called indirectly */
      if (_ma_setup_live_state(handler))
        error= HA_ERR_OUT_OF_MEM;
    }
    else
      _ma_set_trn_for_table(handler, trn);
  }
  /* This is just a commit, tables stay locked if they were: */
  trnman_reset_locked_tables(trn, locked_tables);

end:
  DBUG_RETURN(error);
}


THR_LOCK_DATA **ha_maria::store_lock(THD *thd,
                                     THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type)
{
  /* Test if we can fix test below */
  DBUG_ASSERT(lock_type != TL_UNLOCK &&
              (lock_type == TL_IGNORE || file->lock.type == TL_UNLOCK));
  if (lock_type != TL_IGNORE && file->lock.type == TL_UNLOCK)
  {
    const enum enum_sql_command sql_command= thd->lex->sql_command;
    /*
      We have to disable concurrent inserts for INSERT ... SELECT or
      INSERT/UPDATE/DELETE with sub queries if we are using statement based
      logging.  We take the safe route here and disable this for all commands
      that only does reading that are not SELECT.
    */
    if (lock_type <= TL_READ_HIGH_PRIORITY &&
        !thd->is_current_stmt_binlog_format_row() &&
        (sql_command != SQLCOM_SELECT &&
         sql_command != SQLCOM_LOCK_TABLES) &&
        (thd->variables.option_bits & OPTION_BIN_LOG) &&
        mysql_bin_log.is_open())
      lock_type= TL_READ_NO_INSERT;
    else if (lock_type == TL_WRITE_CONCURRENT_INSERT)
    {
      const enum enum_duplicates duplicates= thd->lex->duplicates;
      /*
        Explanation for the 3 conditions below, in order:

        - Bulk insert may use repair, which will cause problems if other
        threads try to read/insert to the table: disable versioning.
        Note that our read of file->state->records is incorrect, as such
        variable may have changed when we come to start_bulk_insert() (worse
        case: we see != 0 so allow versioning, start_bulk_insert() sees 0 and
        uses repair). This is prevented because start_bulk_insert() will not
        try repair if we enabled versioning.
        - INSERT SELECT ON DUPLICATE KEY UPDATE comes here with
        TL_WRITE_CONCURRENT_INSERT but shouldn't because it can do
        update/delete of a row and versioning doesn't support that
        - same for LOAD DATA CONCURRENT REPLACE.
      */
      if ((file->state->records == 0) ||
          (sql_command == SQLCOM_INSERT_SELECT && duplicates == DUP_UPDATE) ||
          (sql_command == SQLCOM_LOAD && duplicates == DUP_REPLACE))
        lock_type= TL_WRITE;
    }
    file->lock.type= lock_type;
  }
  *to++= &file->lock;
  return to;
}


void ha_maria::update_create_info(HA_CREATE_INFO *create_info)
{
  ha_maria::info(HA_STATUS_AUTO | HA_STATUS_CONST);
  if (!(create_info->used_fields & HA_CREATE_USED_AUTO))
  {
    create_info->auto_increment_value= stats.auto_increment_value;
  }
  create_info->data_file_name= data_file_name;
  create_info->index_file_name= index_file_name;
  /*
    Keep user-specified row_type for ALTER,
    but show the actually used one in SHOW
  */
  if (create_info->row_type != ROW_TYPE_DEFAULT &&
      !(thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE))
    create_info->row_type= get_row_type();
  /*
    Show always page checksums, as this can be forced with
    maria_page_checksums variable
  */
  if (create_info->page_checksum == HA_CHOICE_UNDEF)
    create_info->page_checksum=
      (file->s->options & HA_OPTION_PAGE_CHECKSUM) ? HA_CHOICE_YES :
      HA_CHOICE_NO;
}


enum row_type ha_maria::get_row_type() const
{
  switch (file->s->data_file_type) {
  case STATIC_RECORD:     return ROW_TYPE_FIXED;
  case DYNAMIC_RECORD:    return ROW_TYPE_DYNAMIC;
  case BLOCK_RECORD:      return ROW_TYPE_PAGE;
  case COMPRESSED_RECORD: return ROW_TYPE_COMPRESSED;
  default:                return ROW_TYPE_NOT_USED;
  }
}


static enum data_file_type maria_row_type(HA_CREATE_INFO *info)
{
  if (info->transactional == HA_CHOICE_YES)
    return BLOCK_RECORD;
  switch (info->row_type) {
  case ROW_TYPE_FIXED:   return STATIC_RECORD;
  case ROW_TYPE_DYNAMIC: return DYNAMIC_RECORD;
  default:               return BLOCK_RECORD;
  }
}


int ha_maria::create(const char *name, TABLE *table_arg,
                     HA_CREATE_INFO *ha_create_info)
{
  int error;
  uint create_flags= 0, record_count= 0, i;
  char buff[FN_REFLEN];
  MARIA_KEYDEF *keydef;
  MARIA_COLUMNDEF *recinfo;
  MARIA_CREATE_INFO create_info;
  TABLE_SHARE *share= table_arg->s;
  uint options= share->db_options_in_use;
  ha_table_option_struct *table_options= table_arg->s->option_struct;
  enum data_file_type row_type;
  THD *thd= current_thd;
  DBUG_ENTER("ha_maria::create");

  for (i= 0; i < share->keys; i++)
  {
    if (table_arg->key_info[i].flags & HA_USES_PARSER)
    {
      create_flags|= HA_CREATE_RELIES_ON_SQL_LAYER;
      break;
    }
  }
  /* Note: BLOCK_RECORD is used if table is transactional */
  row_type= maria_row_type(ha_create_info);
  if (ha_create_info->transactional == HA_CHOICE_YES &&
      ha_create_info->row_type != ROW_TYPE_PAGE &&
      ha_create_info->row_type != ROW_TYPE_NOT_USED &&
      ha_create_info->row_type != ROW_TYPE_DEFAULT)
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_ILLEGAL_HA_CREATE_OPTION,
                 "Row format set to PAGE because of TRANSACTIONAL=1 option");

  if (share->table_type == TABLE_TYPE_SEQUENCE)
  {
    /* For sequences, the simples record type is appropriate */
    row_type= STATIC_RECORD;
    ha_create_info->transactional= HA_CHOICE_NO;
  }

  bzero((char*) &create_info, sizeof(create_info));
  if ((error= table2maria(table_arg, row_type, &keydef, &recinfo,
                          &record_count, &create_info)))
    DBUG_RETURN(error); /* purecov: inspected */
  create_info.max_rows= share->max_rows;
  create_info.reloc_rows= share->min_rows;
  create_info.with_auto_increment= share->next_number_key_offset == 0;
  create_info.auto_increment= (ha_create_info->auto_increment_value ?
                               ha_create_info->auto_increment_value -1 :
                               (ulonglong) 0);
  create_info.data_file_length= ((ulonglong) share->max_rows *
                                 share->avg_row_length);
  create_info.data_file_name= ha_create_info->data_file_name;
  create_info.index_file_name= ha_create_info->index_file_name;
  create_info.language= share->table_charset->number;
  if (ht != maria_hton)
  {
    /* S3 engine */
    create_info.s3_block_size= (ulong) table_options->s3_block_size;
    create_info.compression_algorithm= table_options->compression_algorithm;
  }

  /*
    Table is transactional:
    - If the user specify that table is transactional (in this case
      row type is forced to BLOCK_RECORD)
    - If they specify BLOCK_RECORD without specifying transactional behaviour

    Shouldn't this test be pushed down to maria_create()? Because currently,
    ma_test1 -T crashes: it creates a table with DYNAMIC_RECORD but has
    born_transactional==1, which confuses some recovery-related code.
  */
  create_info.transactional= (row_type == BLOCK_RECORD &&
                              ha_create_info->transactional != HA_CHOICE_NO);

  if (ha_create_info->tmp_table())
  {
    create_flags|= HA_CREATE_TMP_TABLE | HA_CREATE_DELAY_KEY_WRITE;
    if (ha_create_info->options & HA_LEX_CREATE_GLOBAL_TMP_TABLE)
      create_flags|= HA_CREATE_GLOBAL_TMP_TABLE;
    create_info.transactional= 0;
  }
  if (ha_create_info->options & HA_CREATE_KEEP_FILES)
    create_flags|= HA_CREATE_KEEP_FILES;
  if (options & HA_OPTION_PACK_RECORD)
    create_flags|= HA_PACK_RECORD;
  if (options & HA_OPTION_CHECKSUM)
    create_flags|= HA_CREATE_CHECKSUM;
  if (options & HA_OPTION_DELAY_KEY_WRITE)
    create_flags|= HA_CREATE_DELAY_KEY_WRITE;
  if ((ha_create_info->page_checksum == HA_CHOICE_UNDEF &&
       maria_page_checksums) ||
       ha_create_info->page_checksum ==  HA_CHOICE_YES)
    create_flags|= HA_CREATE_PAGE_CHECKSUM;

  (void) translog_log_debug_info(0, LOGREC_DEBUG_INFO_QUERY,
                                 (uchar*) thd->query(), thd->query_length());

  create_info.encrypted= maria_encrypt_tables && ht == maria_hton;
  /* TODO: Check that the following fn_format is really needed */
  error=
    maria_create(fn_format(buff, name, "", "",
                           MY_UNPACK_FILENAME | MY_APPEND_EXT),
                 row_type, share->keys, keydef,
                 record_count,  recinfo,
                 0, (MARIA_UNIQUEDEF *) 0,
                 &create_info, create_flags);

  my_free(recinfo);
  DBUG_RETURN(error);
}


int ha_maria::rename_table(const char *from, const char *to)
{
  THD *thd= current_thd;
  (void) translog_log_debug_info(0, LOGREC_DEBUG_INFO_QUERY,
                                 (uchar*) thd->query(), thd->query_length());
  return maria_rename(from, to);
}


void ha_maria::get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values)
{
  ulonglong nr;
  int error;
  uchar key[MARIA_MAX_KEY_BUFF];
  enum ha_rkey_function search_flag= HA_READ_PREFIX_LAST;

  if (!table->s->next_number_key_offset)
  {                                             // Autoincrement at key-start
    ha_maria::info(HA_STATUS_AUTO);
    *first_value= stats.auto_increment_value;
    /* Maria has only table-level lock for now, so reserves to +inf */
    *nb_reserved_values= ULONGLONG_MAX;
    return;
  }

  /* it's safe to call the following if bulk_insert isn't on */
  maria_flush_bulk_insert(file, table->s->next_number_index);

  if (unlikely(table->key_info[table->s->next_number_index].
                  key_part[table->s->next_number_keypart].key_part_flag &
                    HA_REVERSE_SORT))
    search_flag= HA_READ_KEY_EXACT;

  (void) extra(HA_EXTRA_KEYREAD);
  key_copy(key, table->record[0],
           table->key_info + table->s->next_number_index,
           table->s->next_number_key_offset);
  error= maria_rkey(file, table->record[1], (int) table->s->next_number_index,
                    key, make_prev_keypart_map(table->s->next_number_keypart),
                    search_flag);
  if (error)
    nr= 1;
  else
  {
    /* Get data from record[1] */
    nr= ((ulonglong) table->next_number_field->
         val_int_offset(table->s->rec_buff_length) + 1);
  }
  extra(HA_EXTRA_NO_KEYREAD);
  *first_value= nr;
  /*
    MySQL needs to call us for next row: assume we are inserting ("a",null)
    here, we return 3, and next this statement will want to insert ("b",null):
    there is no reason why ("b",3+1) would be the good row to insert: maybe it
    already exists, maybe 3+1 is too large...
  */
  *nb_reserved_values= 1;
}


/*
  Find out how many rows there is in the given range

  SYNOPSIS
    records_in_range()
    inx                 Index to use
    min_key             Start of range.  Null pointer if from first key
    max_key             End of range. Null pointer if to last key
    pages               Store first and last page for the range in case of
                        b-trees. In other cases it's not touched.

  NOTES
    min_key.flag can have one of the following values:
      HA_READ_KEY_EXACT         Include the key in the range
      HA_READ_AFTER_KEY         Don't include key in range

    max_key.flag can have one of the following values:
      HA_READ_BEFORE_KEY        Don't include key in range
      HA_READ_AFTER_KEY         Include all 'end_key' values in the range

  RETURN
   HA_POS_ERROR         Something is wrong with the index tree.
   0                    There is no matching keys in the given range
   number > 0           There is approximately 'number' matching rows in
                        the range.
*/

ha_rows ha_maria::records_in_range(uint inx, const key_range *min_key,
                                   const key_range *max_key, page_range *pages)
{
  register_handler(file);
  return (ha_rows) maria_records_in_range(file, (int) inx, min_key, max_key,
                                          pages);
}


FT_INFO *ha_maria::ft_init_ext(uint flags, uint inx, String * key)
{
  return maria_ft_init_search(flags, file, inx,
                              (uchar *) key->ptr(), key->length(),
                              key->charset(), table->record[0]);
}


int ha_maria::ft_read(uchar * buf)
{
  int error;

  if (!ft_handler)
    return -1;

  register_handler(file);

  thread_safe_increment(table->in_use->status_var.ha_read_next_count,
                        &LOCK_status);  // why ?

  error= ft_handler->please->read_next(ft_handler, (char*) buf);

  return error;
}


bool ha_maria::check_if_incompatible_data(HA_CREATE_INFO *create_info,
                                          uint table_changes)
{
  DBUG_ENTER("check_if_incompatible_data");
  uint options= table->s->db_options_in_use;
  enum ha_choice page_checksum= table->s->page_checksum;

  if (page_checksum == HA_CHOICE_UNDEF)
    page_checksum= file->s->options & HA_OPTION_PAGE_CHECKSUM ? HA_CHOICE_YES
                                                              : HA_CHOICE_NO;

  if (create_info->auto_increment_value != stats.auto_increment_value ||
      create_info->data_file_name != data_file_name ||
      create_info->index_file_name != index_file_name ||
      create_info->page_checksum != page_checksum ||
      create_info->transactional != table->s->transactional ||
      (maria_row_type(create_info) != data_file_type &&
       create_info->row_type != ROW_TYPE_DEFAULT) ||
      table_changes == IS_EQUAL_NO ||
      (table_changes & IS_EQUAL_PACK_LENGTH)) // Not implemented yet
    DBUG_RETURN(COMPATIBLE_DATA_NO);

  if ((options & (HA_OPTION_CHECKSUM |
                  HA_OPTION_DELAY_KEY_WRITE)) !=
      (create_info->table_options & (HA_OPTION_CHECKSUM |
                              HA_OPTION_DELAY_KEY_WRITE)))
    DBUG_RETURN(COMPATIBLE_DATA_NO);
  DBUG_RETURN(COMPATIBLE_DATA_YES);
}


static int maria_hton_panic(handlerton *hton, ha_panic_function flag)
{
  /* If no background checkpoints, we need to do one now */
  int ret=0;

  if (!checkpoint_interval && !aria_readonly)
    ret= ma_checkpoint_execute(CHECKPOINT_FULL, FALSE);

  ret|= maria_panic(flag);

  maria_hton= 0;
  return ret;
}


static int maria_commit(handlerton *hton __attribute__ ((unused)),
                        THD *thd, bool all)
{
  TRN *trn= THD_TRN;
  int res= 0;
  MARIA_HA *used_instances;
  DBUG_ENTER("maria_commit");

  /* No commit inside lock_tables() */
  if ((!trn ||
       thd->locked_tables_mode == LTM_LOCK_TABLES ||
       thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES))
    DBUG_RETURN(0);

  /* statement or transaction ? */
  if ((thd->variables.option_bits & (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) &&
      !all)
    DBUG_RETURN(0); // end of statement

  used_instances= (MARIA_HA*) trn->used_instances;
  trnman_reset_locked_tables(trn, 0);
  trnman_set_flags(trn, trnman_get_flags(trn) & ~TRN_STATE_INFO_LOGGED);
  trn->used_instances= 0;
  if (ma_commit(trn))
    res= HA_ERR_COMMIT_ERROR;
  reset_thd_trn(thd, used_instances);
  thd_set_ha_data(thd, maria_hton, 0);
  DBUG_RETURN(res);
}

#ifdef MARIA_CANNOT_ROLLBACK
static int maria_rollback(handlerton *hton, THD *thd, bool all)
{
  TRN *trn= THD_TRN;
  DBUG_ENTER("maria_rollback");
  if (!trn)
    DBUG_RETURN(0);
  if (trn->undo_lsn)
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_DATA_WAS_COMMITED_UNDER_ROLLBACK,
                        ER_THD(thd, ER_DATA_WAS_COMMITED_UNDER_ROLLBACK),
                        "Aria");
  if (all)
    DBUG_RETURN(maria_commit(hton, thd, all));
  /* Statement rollbacks are ignored. Commit will happen in external_lock */
  DBUG_RETURN(0);
}

#else

static int maria_rollback(handlerton *hton __attribute__ ((unused)),
                          THD *thd, bool all)
{
  TRN *trn= THD_TRN;
  DBUG_ENTER("maria_rollback");

  DBUG_ASSERT(trnman_has_locked_tables(trn) == 0);
  trnman_reset_locked_tables(trn, 0);
  /* statement or transaction ? */
  if ((thd->variables.option_bits & (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) &&
      !all)
  {
    trnman_rollback_statement(trn);
    DBUG_RETURN(0); // end of statement
  }
  reset_thd_trn(thd, (MARIA_HA*) trn->used_instances);
  DBUG_RETURN(trnman_rollback_trn(trn) ?
              HA_ERR_OUT_OF_MEM : 0); // end of transaction
}
#endif /* MARIA_CANNOT_ROLLBACK */


/**
  @brief flush log handler

  @param hton            maria handlerton (unused)

  @retval FALSE OK
  @retval TRUE  Error
*/

bool maria_flush_logs(handlerton *hton)
{
  return MY_TEST(translog_purge_at_flush());
}


int maria_checkpoint_state(handlerton *hton, bool disabled)
{
  maria_checkpoint_disabled= (my_bool) disabled;
  return 0;
}


/*
  Handle backup calls
*/

void maria_prepare_for_backup()
{
  translog_disable_purge();
}

void maria_end_backup()
{
  translog_enable_purge();
}



#define SHOW_MSG_LEN (FN_REFLEN + 20)
/**
  @brief show status handler

  @param hton            maria handlerton
  @param thd             thread handler
  @param print           print function
  @param stat            type of status
*/

bool maria_show_status(handlerton *hton,
                       THD *thd,
                       stat_print_fn *print,
                       enum ha_stat_type stat)
{
  const LEX_CSTRING *engine_name= hton_name(hton);
  switch (stat) {
  case HA_ENGINE_LOGS:
  {
    TRANSLOG_ADDRESS horizon= translog_get_horizon();
    uint32 last_file= LSN_FILE_NO(horizon);
    uint32 first_needed= translog_get_first_needed_file();
    uint32 first_file= translog_get_first_file(horizon);
    uint32 i;
    const char unknown[]= "unknown";
    const char needed[]= "in use";
    const char unneeded[]= "free";
    char path[FN_REFLEN];

    if (first_file == 0)
    {
      const char error[]= "error";
      print(thd, engine_name->str, engine_name->length,
            STRING_WITH_LEN(""), error, sizeof(error) - 1);
      break;
    }

    for (i= first_file; i <= last_file; i++)
    {
      char *file;
      const char *status;
      size_t length, status_len;
      MY_STAT stat_buff, *stat;
      const char error[]= "can't stat";
      char object[SHOW_MSG_LEN];
      file= translog_filename_by_fileno(i, path);
      if (!(stat= mysql_file_stat(key_file_translog, file, &stat_buff, MYF(0))))
      {
        status= error;
        status_len= sizeof(error) - 1;
        length= my_snprintf(object, SHOW_MSG_LEN, "Size unknown ; %s", file);
      }
      else
      {
        if (first_needed == 0)
        {
          status= unknown;
          status_len= sizeof(unknown) - 1;
        }
        else if (i < first_needed)
        {
          status= unneeded;
          status_len= sizeof(unneeded) - 1;
        }
        else
        {
          status= needed;
          status_len= sizeof(needed) - 1;
        }
        length= my_snprintf(object, SHOW_MSG_LEN, "Size %12llu ; %s",
                            (ulonglong) stat->st_size, file);
      }

      print(thd, engine_name->str, engine_name->length,
            object, length, status, status_len);
    }
    break;
  }
  case HA_ENGINE_STATUS:
  case HA_ENGINE_MUTEX:
  default:
    break;
  }
  return 0;
}


/**
  Callback to delete all logs in directory. This is lower-level than other
  functions in ma_loghandler.c which delete logs, as it does not rely on
  translog_init() having been called first.

  @param  directory        directory where file is
  @param  filename         base name of the file to delete
*/

static my_bool translog_callback_delete_all(const char *directory,
                                            const char *filename)
{
  char complete_name[FN_REFLEN];
  fn_format(complete_name, filename, directory, "", MYF(MY_UNPACK_FILENAME));
  return mysql_file_delete(key_file_translog, complete_name, MYF(MY_WME));
}


/**
  Helper function for option aria-force-start-after-recovery-failures.
  Deletes logs if too many failures. Otherwise, increments the counter of
  failures in the control file.
  Notice how this has to be called _before_ translog_init() (if log is
  corrupted, translog_init() might crash the server, so we need to remove logs
  before).

  @param  log_dir          directory where logs to be deleted are
*/

static int mark_recovery_start(const char* log_dir)
{
  int res;
  DBUG_ENTER("mark_recovery_start");
  if (!(maria_recover_options & HA_RECOVER_ANY))
    ma_message_no_user(ME_WARNING, "Please consider using option"
                       " --aria-recover-options[=...] to automatically check and"
                       " repair tables when logs are removed by option"
                       " --aria-force-start-after-recovery-failures=#");
  if (recovery_failures >= force_start_after_recovery_failures)
  {
    /*
      Remove logs which cause the problem; keep control file which has
      critical info like uuid, max_trid (removing control file may make
      correct tables look corrupted!).
    */
    char msg[100];
    res= translog_walk_filenames(log_dir, &translog_callback_delete_all);
    my_snprintf(msg, sizeof(msg),
                "%s logs after %u consecutive failures of"
                " recovery from logs",
                (res ? "failed to remove some" : "removed all"),
                recovery_failures);
    ma_message_no_user((res ? 0 : ME_WARNING), msg);
  }
  else
    res= ma_control_file_write_and_force(last_checkpoint_lsn, last_logno,
                                         max_trid_in_control_file,
                                         recovery_failures + 1);
  DBUG_RETURN(res);
}


/**
  Helper function for option aria-force-start-after-recovery-failures.
  Records in the control file that recovery was a success, so that it's not
  counted for aria-force-start-after-recovery-failures.
*/

static int mark_recovery_success(void)
{
  /* success of recovery, reset recovery_failures: */
  int res;
  DBUG_ENTER("mark_recovery_success");
  res= ma_control_file_write_and_force(last_checkpoint_lsn, last_logno,
                                       max_trid_in_control_file, 0);
  DBUG_RETURN(res);
}


/*
  Return 1 if table has changed during the current transaction
*/

bool ha_maria::is_changed() const
{
  return file->state->changed;
}


static int ha_maria_init(void *p)
{
  int res= 0, tmp;
  const char *log_dir= maria_data_root;

  /*
    If aria_readonly is set, then we don't run recovery and we don't allow
    opening of tables that are crashed. Used by mysqld --help
   */
  if ((aria_readonly= opt_help != 0))
  {
    maria_recover_options= 0;
    checkpoint_interval= 0;
  }

#ifdef HAVE_PSI_INTERFACE
  init_aria_psi_keys();
#endif

  maria_hton= (handlerton *)p;
  maria_hton->db_type= DB_TYPE_ARIA;
  maria_hton->create= maria_create_handler;
  maria_hton->panic= maria_hton_panic;
  maria_hton->tablefile_extensions= ha_maria_exts;
  maria_hton->commit= maria_commit;
  maria_hton->rollback= maria_rollback;
  maria_hton->checkpoint_state= maria_checkpoint_state;
  maria_hton->flush_logs= maria_flush_logs;
  maria_hton->show_status= maria_show_status;
  maria_hton->prepare_for_backup= maria_prepare_for_backup;
  maria_hton->end_backup= maria_end_backup;

  /* TODO: decide if we support Maria being used for log tables */
  maria_hton->flags= (HTON_CAN_RECREATE | HTON_SUPPORT_LOG_TABLES |
                      HTON_NO_ROLLBACK |
                      HTON_TRANSACTIONAL_AND_NON_TRANSACTIONAL);
  bzero(maria_log_pagecache, sizeof(*maria_log_pagecache));
  maria_tmpdir= &mysql_tmpdir_list;             /* For REDO */
  ma_debug_crash_here= maria_debug_crash_here;

  if (!aria_readonly)
    res= maria_upgrade();
  res= res || maria_init();
  tmp= ma_control_file_open(!aria_readonly, !aria_readonly, !aria_readonly,
                            control_file_open_flags);
  res= res || aria_readonly ? tmp == CONTROL_FILE_LOCKED : tmp != 0;
  res= res ||
    ((force_start_after_recovery_failures != 0 && !aria_readonly) &&
     mark_recovery_start(log_dir)) ||
    !init_pagecache(maria_pagecache,
                    (size_t) pagecache_buffer_size, pagecache_division_limit,
                    pagecache_age_threshold, maria_block_size, pagecache_file_hash_size,
                    0) ||
    !init_pagecache(maria_log_pagecache,
                    TRANSLOG_PAGECACHE_SIZE, 0, 0,
                    TRANSLOG_PAGE_SIZE, 0, 0) ||
    (!aria_readonly &&
     translog_init(maria_data_root, log_file_size,
                   MYSQL_VERSION_ID, server_id, maria_log_pagecache,
                   TRANSLOG_DEFAULT_FLAGS, 0)) ||
    (!aria_readonly &&
     (maria_recovery_from_log() ||
      ((force_start_after_recovery_failures != 0 ||
        maria_recovery_changed_data || recovery_failures) &&
       mark_recovery_success()))) ||
    (aria_readonly && trnman_init(MAX_INTERNAL_TRID-16)) ||
    ma_checkpoint_init(checkpoint_interval);
  maria_multi_threaded= maria_in_ha_maria= TRUE;
  maria_create_trn_hook= maria_create_trn_for_mysql;
  maria_pagecache->extra_debug= 1;
  maria_assert_if_crashed_table= debug_assert_if_crashed_table;

  if (res)
  {
    maria_hton= 0;
    maria_panic(HA_PANIC_CLOSE);
  }

  ma_killed= ma_killed_in_mariadb;
  if (res)
    maria_panic(HA_PANIC_CLOSE);

  return res ? HA_ERR_INITIALIZATION : 0;
}


#ifdef HAVE_QUERY_CACHE
/**
  @brief Register a named table with a call back function to the query cache.

  @param thd The thread handle
  @param table_key A pointer to the table name in the table cache
  @param key_length The length of the table name
  @param[out] engine_callback The pointer to the storage engine call back
    function, currently 0
  @param[out] engine_data Engine data will be set to 0.

  @note Despite the name of this function, it is used to check each statement
    before it is cached and not to register a table or callback function.

  @see handler::register_query_cache_table

  @return The error code. The engine_data and engine_callback will be set to 0.
    @retval TRUE Success
    @retval FALSE An error occurred
*/

my_bool ha_maria::register_query_cache_table(THD *thd, const char *table_name,
					     uint table_name_len,
					     qc_engine_callback
					     *engine_callback,
					     ulonglong *engine_data)
{
  ulonglong actual_data_file_length;
  ulonglong current_data_file_length;
  DBUG_ENTER("ha_maria::register_query_cache_table");

  /*
    No call back function is needed to determine if a cached statement
    is valid or not.
  */
  *engine_callback= 0;

  /*
    No engine data is needed.
  */
  *engine_data= 0;

  if (file->s->now_transactional && file->s->have_versioning)
    DBUG_RETURN(file->trn->trid >= file->s->state.last_change_trn);

  /*
    If a concurrent INSERT has happened just before the currently processed
    SELECT statement, the total size of the table is unknown.

    To determine if the table size is known, the current thread's snap shot of
    the table size with the actual table size are compared.

    If the table size is unknown the SELECT statement can't be cached.
  */

  /*
    POSIX visibility rules specify that "2. Whatever memory values a
    thread can see when it unlocks a mutex <...> can also be seen by any
    thread that later locks the same mutex". In this particular case,
    concurrent insert thread had modified the data_file_length in
    MYISAM_SHARE before it has unlocked (or even locked)
    structure_guard_mutex. So, here we're guaranteed to see at least that
    value after we've locked the same mutex. We can see a later value
    (modified by some other thread) though, but it's ok, as we only want
    to know if the variable was changed, the actual new value doesn't matter
  */
  actual_data_file_length= file->s->state.state.data_file_length;
  current_data_file_length= file->state->data_file_length;

  /* Return whether is ok to try to cache current statement. */
  DBUG_RETURN(!(file->s->non_transactional_concurrent_insert &&
                current_data_file_length != actual_data_file_length));
}
#endif

static struct st_mysql_sys_var *system_variables[]= {
  MYSQL_SYSVAR(block_size),
  MYSQL_SYSVAR(checkpoint_interval),
  MYSQL_SYSVAR(checkpoint_log_activity),
  MYSQL_SYSVAR(force_start_after_recovery_failures),
  MYSQL_SYSVAR(group_commit),
  MYSQL_SYSVAR(group_commit_interval),
  MYSQL_SYSVAR(log_dir_path),
  MYSQL_SYSVAR(log_file_size),
  MYSQL_SYSVAR(log_purge_type),
  MYSQL_SYSVAR(max_sort_file_size),
  MYSQL_SYSVAR(page_checksum),
  MYSQL_SYSVAR(pagecache_age_threshold),
  MYSQL_SYSVAR(pagecache_buffer_size),
  MYSQL_SYSVAR(pagecache_division_limit),
  MYSQL_SYSVAR(pagecache_file_hash_size),
  MYSQL_SYSVAR(recover_options),
  MYSQL_SYSVAR(repair_threads),
  MYSQL_SYSVAR(sort_buffer_size),
  MYSQL_SYSVAR(stats_method),
  MYSQL_SYSVAR(sync_log_dir),
  MYSQL_SYSVAR(used_for_temp_tables),
  MYSQL_SYSVAR(encrypt_tables),
  NULL
};


/**
   @brief Updates the checkpoint interval and restarts the background thread.
*/

static void update_checkpoint_interval(MYSQL_THD thd,
                                        struct st_mysql_sys_var *var,
                                        void *var_ptr, const void *save)
{
  ma_checkpoint_end();
  ma_checkpoint_init(*(ulong *)var_ptr= (ulong)(*(long *)save));
}


/**
   @brief Updates group commit mode
*/

static void update_maria_group_commit(MYSQL_THD thd,
                                      struct st_mysql_sys_var *var,
                                      void *var_ptr, const void *save)
{
  ulong value= (ulong)*((long *)var_ptr);
  DBUG_ENTER("update_maria_group_commit");
  DBUG_PRINT("enter", ("old value: %lu  new value %lu  rate %lu",
                       value, (ulong)(*(long *)save),
                       maria_group_commit_interval));
  /* old value */
  switch (value) {
  case TRANSLOG_GCOMMIT_NONE:
    break;
  case TRANSLOG_GCOMMIT_HARD:
    translog_hard_group_commit(FALSE);
    break;
  case TRANSLOG_GCOMMIT_SOFT:
    translog_soft_sync(FALSE);
    if (maria_group_commit_interval)
      translog_soft_sync_end();
    break;
  default:
    DBUG_ASSERT(0); /* impossible */
  }
  value= *(ulong *)var_ptr= (ulong)(*(long *)save);
  translog_sync();
  /* new value */
  switch (value) {
  case TRANSLOG_GCOMMIT_NONE:
    break;
  case TRANSLOG_GCOMMIT_HARD:
    translog_hard_group_commit(TRUE);
    break;
  case TRANSLOG_GCOMMIT_SOFT:
    translog_soft_sync(TRUE);
    /* variable change made under global lock so we can just read it */
    if (maria_group_commit_interval)
      translog_soft_sync_start();
    break;
  default:
    DBUG_ASSERT(0); /* impossible */
  }
  DBUG_VOID_RETURN;
}

/**
   @brief Updates group commit interval
*/

static void update_maria_group_commit_interval(MYSQL_THD thd,
                                               struct st_mysql_sys_var *var,
                                               void *var_ptr, const void *save)
{
  ulong new_value= (ulong)*((long *)save);
  ulong *value_ptr= (ulong*) var_ptr;
  DBUG_ENTER("update_maria_group_commit_interval");
  DBUG_PRINT("enter", ("old value: %lu  new value %lu  group commit %lu",
                        *value_ptr, new_value, maria_group_commit));

  /* variable change made under global lock so we can just read it */
  switch (maria_group_commit) {
    case TRANSLOG_GCOMMIT_NONE:
      *value_ptr= new_value;
      translog_set_group_commit_interval(new_value);
      break;
    case TRANSLOG_GCOMMIT_HARD:
      *value_ptr= new_value;
      translog_set_group_commit_interval(new_value);
      break;
    case TRANSLOG_GCOMMIT_SOFT:
      if (*value_ptr)
        translog_soft_sync_end();
      translog_set_group_commit_interval(new_value);
      if ((*value_ptr= new_value))
        translog_soft_sync_start();
      break;
    default:
      DBUG_ASSERT(0); /* impossible */
  }
  DBUG_VOID_RETURN;
}

/**
   @brief Updates the transaction log file limit.
*/

static void update_log_file_size(MYSQL_THD thd,
                                 struct st_mysql_sys_var *var,
                                 void *var_ptr, const void *save)
{
  uint32 size= (uint32)((ulong)(*(long *)save));
  translog_set_file_size(size);
  *(ulong *)var_ptr= size;
}


static SHOW_VAR status_variables[]= {
  {"pagecache_blocks_not_flushed", (char*) &maria_pagecache_var.global_blocks_changed, SHOW_LONG},
  {"pagecache_blocks_unused",      (char*) &maria_pagecache_var.blocks_unused, SHOW_LONG},
  {"pagecache_blocks_used",        (char*) &maria_pagecache_var.blocks_used, SHOW_LONG},
  {"pagecache_read_requests",      (char*) &maria_pagecache_var.global_cache_r_requests, SHOW_LONGLONG},
  {"pagecache_reads",              (char*) &maria_pagecache_var.global_cache_read, SHOW_LONGLONG},
  {"pagecache_write_requests",     (char*) &maria_pagecache_var.global_cache_w_requests, SHOW_LONGLONG},
  {"pagecache_writes",             (char*) &maria_pagecache_var.global_cache_write, SHOW_LONGLONG},
  {"transaction_log_syncs",        (char*) &translog_syncs, SHOW_LONGLONG},
  {NullS, NullS, SHOW_LONG}
};

/****************************************************************************
 * Maria MRR implementation: use DS-MRR
 ***************************************************************************/

int ha_maria::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                    uint n_ranges, uint mode,
                                    HANDLER_BUFFER *buf)
{
  return ds_mrr.dsmrr_init(this, seq, seq_init_param, n_ranges, mode, buf);
}

int ha_maria::multi_range_read_next(range_id_t *range_info)
{
  return ds_mrr.dsmrr_next(range_info);
}

ha_rows ha_maria::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                               void *seq_init_param,
                                               uint n_ranges, uint *bufsz,
                                               uint *flags, Cost_estimate *cost)
{
  /*
    This call is here because there is no location where this->table would
    already be known.
    TODO: consider moving it into some per-query initialization call.
  */
  ds_mrr.init(this, table);
  return ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges, bufsz,
                                 flags, cost);
}

ha_rows ha_maria::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                       uint key_parts, uint *bufsz,
                                       uint *flags, Cost_estimate *cost)
{
  ds_mrr.init(this, table);
  return ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz, flags, cost);
}

int ha_maria::multi_range_read_explain_info(uint mrr_mode, char *str,
                                            size_t size)
{
  return ds_mrr.dsmrr_explain_info(mrr_mode, str, size);
}
/* MyISAM MRR implementation ends */


/* Index condition pushdown implementation*/


Item *ha_maria::idx_cond_push(uint keyno_arg, Item* idx_cond_arg)
{
  /*
    Check if the key contains a blob field. If it does then MyISAM
    should not accept the pushed index condition since MyISAM will not
    read the blob field from the index entry during evaluation of the
    pushed index condition and the BLOB field might be part of the
    range evaluation done by the ICP code.
  */
  const KEY *key= &table_share->key_info[keyno_arg];

  for (uint k= 0; k < key->user_defined_key_parts; ++k)
  {
    const KEY_PART_INFO *key_part= &key->key_part[k];
    if (key_part->key_part_flag & HA_BLOB_PART)
    {
      /* Let the server handle the index condition */
      return idx_cond_arg;
    }
  }

  pushed_idx_cond_keyno= keyno_arg;
  pushed_idx_cond= idx_cond_arg;
  in_range_check_pushed_down= TRUE;
  if (active_index == pushed_idx_cond_keyno)
    ma_set_index_cond_func(file, handler_index_cond_check, this);
  return NULL;
}

/**
  Find record by unique constrain (used in temporary tables)

  @param record          (IN|OUT) the record to find
  @param constrain_no    (IN) number of constrain (for this engine)

  @note It is like hp_search but uses function for raw where hp_search
        uses functions for index.

  @retval  0 OK
  @retval  1 Not found
  @retval -1 Error
*/

int ha_maria::find_unique_row(uchar *record, uint constrain_no)
{
  int rc;
  register_handler(file);
  if (file->s->state.header.uniques)
  {
    DBUG_ASSERT(file->s->state.header.uniques > constrain_no);
    MARIA_UNIQUEDEF *def= file->s->uniqueinfo + constrain_no;
    ha_checksum unique_hash= _ma_unique_hash(def, record);
    rc= _ma_check_unique(file, def, record, unique_hash, HA_OFFSET_ERROR);
    if (rc)
    {
      file->cur_row.lastpos= file->dup_key_pos;
      if ((*file->read_record)(file, record, file->cur_row.lastpos))
        return -1;
      file->update|= HA_STATE_AKTIV;                     /* Record is read */
    }
    // invert logic
    rc= !MY_TEST(rc);
  }
  else
  {
    /*
     It is case when just unique index used instead unicue constrain
     (conversion from heap table).
     */
    DBUG_ASSERT(file->s->state.header.keys > constrain_no);
    MARIA_KEY key;
    file->once_flags|= USE_PACKED_KEYS;
    (*file->s->keyinfo[constrain_no].make_key)
      (file, &key, constrain_no, file->lastkey_buff2, record, 0, 0);
    rc= maria_rkey(file, record, constrain_no, key.data, key.data_length,
                   HA_READ_KEY_EXACT);
    rc= MY_TEST(rc);
  }
  return rc;
}


/**
   Check if a table needs to be repaired
*/

int ha_maria::check_for_upgrade(HA_CHECK_OPT *check)
{
  if (table->s->mysql_version && table->s->mysql_version <= 100509 &&
      (file->s->base.extra_options & MA_EXTRA_OPTIONS_ENCRYPTED))
  {
    /*
      Encrypted tables before 10.5.9 had a bug where LSN was not
      stored on the pages. These must be repaired!
    */
    return HA_ADMIN_NEEDS_ALTER;
  }
  return HA_ADMIN_OK;
}


struct st_mysql_storage_engine maria_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(aria)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &maria_storage_engine,
  "Aria",
  "MariaDB Corporation Ab",
  "Crash-safe tables with MyISAM heritage. Used for internal temporary tables and privilege tables",
  PLUGIN_LICENSE_GPL,
  ha_maria_init,                /* Plugin Init      */
  NULL,                         /* Plugin Deinit    */
  0x0105,                       /* 1.5              */
  status_variables,             /* status variables */
  system_variables,             /* system variables */
  "1.5",                        /* string version   */
  MariaDB_PLUGIN_MATURITY_STABLE /* maturity         */
}
maria_declare_plugin_end;
