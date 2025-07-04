--source include/long_test.inc
--source include/have_debug.inc
--source include/have_log_bin.inc

if (!$BIG_TEST)
{
  --source include/not_valgrind.inc
  --source include/not_msan.inc
}

#
# Testing of atomic create table with crashes in a lot of different places
#
# Things tested:
# With myisam and InnoDB engines to ensure that cover both normal and
# online alter table paths.
# Alter table with new columns
# Alter table which only touches .frm
# Alter table disable keys (has it own code path)
# Alter table with rename
# Alter table with rename and only options that touches .frm
# Alter table with rename and add new columns
# Alter table with storage engine change (with and without column definition
# changes)
# Alter table with storage engine change and rename
# Alter table to another database

--disable_query_log
call mtr.add_suppression("InnoDB: .* does not exist in the InnoDB internal");
# Speed up wait_until_connected_again.inc
let NO_WSREP=1;
--enable_query_log
let $MYSQLD_DATADIR= `SELECT @@datadir`;

create database test2;
RESET MASTER;

if ($engine_count == "")
{
  let $engine_count=2;
  let $engines='myisam','innodb';
}
if ($extra_engine == "")
{
  let $extra_engine=aria;
}

let $crash_count=13;
let $crash_points='ddl_log_alter_after_create_frm', 'ddl_log_alter_after_create_table', 'ddl_log_alter_after_prepare_inplace','ddl_log_alter_after_copy', 'ddl_log_alter_after_log', 'ddl_log_alter_after_rename_to_backup', 'ddl_log_alter_after_rename_to_backup_log', 'ddl_log_alter_rename_frm', 'ddl_log_alter_after_rename_to_original', 'ddl_log_alter_before_rename_triggers', 'ddl_log_alter_after_rename_triggers', 'ddl_log_alter_after_delete_backup', 'ddl_log_alter_after_drop_original_table';

let $statement_count=16;
let $statements='ALTER TABLE t1 ADD COLUMN c INT, COMMENT "new"',
                'ALTER TABLE t1 COMMENT "new"',
                'ALTER TABLE t1 change column a c int COMMENT "new"',
                'ALTER TABLE t1 ADD COLUMN c INT, COMMENT "new", rename t2',
                'ALTER TABLE t1 disable keys',
                'ALTER TABLE t1 ADD COLUMN c INT, ALGORITHM=copy, COMMENT "new"',
                'ALTER TABLE t1 rename t2',
                'ALTER TABLE t1 COMMENT "new", rename t2',
                'ALTER TABLE t1 change column a c int COMMENT "new", rename t2',
                'ALTER TABLE t1 ENGINE=$extra_engine, COMMENT "new"',
                'ALTER TABLE t1 change column a c int COMMENT "new", engine=$extra_engine',
                'ALTER TABLE t1 ADD COLUMN c INT, COMMENT "new", rename t2, engine=$extra_engine',
                'ALTER TABLE t1 ADD COLUMN c INT, COMMENT "new", rename test2.t2',
                'ALTER TABLE t1 COMMENT "new", rename test2.t2',
                'ALTER TABLE t1 ADD key(b), COMMENT "new"',
                'ALTER TABLE t1 DROP INDEX a';

# If there is a need of testing one specific state (crash point and query),
# one can use the comments below to execute one specific test combination
#let $crash_count=1;
#let $crash_points='ddl_log_alter_after_create_frm';
#let $statement_count= 1;
#let $statements='ALTER TABLE t1 ADD COLUMN c int, COMMENT "new"';
#let $engine_count=1;
#let $engines='rocksdb';
#--source include/have_rocksdb.inc

let $old_debug=`select @@debug_dbug`;
let $e=0;
let $keep_include_silent=1;
let $grep_script=ALTER;
--disable_query_log

while ($e < $engine_count)
{
  inc $e;
  let $engine=`select ELT($e, $engines)`;
  let $default_engine=$engine;

  --echo
  --echo engine: $engine
  --echo

  let $r=0;
  while ($r < $statement_count)
  {
    inc $r;
    let $statement=`select ELT($r, $statements)`;
    --echo
    --echo query: $statement
    --echo
    let $c=0;
    while ($c < $crash_count)
    {
      inc $c;
      let $crash=`select ELT($c, $crash_points)`;

      --eval create table t1 (a int, b int, key(a)) engine=$engine
      insert into t1 values (1,1),(2,2);
      commit;
      flush tables;

      FLUSH BINARY LOGS;
      --let $start_binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
      --echo crash point: $crash
      if ($crash_count > 1)
      {
         --exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
      }
#     The following can be used for testing one specific failure
#     if ($crash == "ddl_log_alter_after_log")
#     {
#       if ($r == 2)
#       {
#         --remove_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
#       }
#     }
      --disable_reconnect
      --eval set @@debug_dbug="+d,$crash",@debug_crash_counter=1
      let $errno=0;
      --error 0,2013
      --eval $statement;
      let $error=$errno;
      --enable_reconnect
      --source include/wait_until_connected_again.inc
      --disable_query_log
      --eval set @@debug_dbug="$old_debug"

      if ($error == 0)
      {
        echo "No crash!";
      }
      if ($error != 0)
      {
        --list_files $MYSQLD_DATADIR/test t*
        --list_files $MYSQLD_DATADIR/test *sql*
        --list_files $MYSQLD_DATADIR/test2 t*
        --list_files $MYSQLD_DATADIR/test2 *sql*
        # Check which tables still exists
        --error 0,1
        --file_exists $MYSQLD_DATADIR/test/t1.frm
        let $error2=$errno;
        if ($error2 == 0)
        {
          show create table t1;
          select count(*) from t1;
        }
        if ($error2 == 1)
        {
          --error 0,1
          --file_exists $MYSQLD_DATADIR/test/t2.frm
          let $error3=$errno;
          if ($error3 == 0)
          {
            show create table t2;
            select count(*) from t2;
          }
          if ($error3 == 1)
          {
            --echo "Table is in test2"
            show create table test2.t2;
            select count(*) from test2.t2;
          }
        }
        --let $binlog_file=$start_binlog_file
        --let $binlog_output_name=master-bin.000001

        --source include/show_binlog_events.inc
        if ($error)
        {
          --let $binlog_file= query_get_value(SHOW MASTER STATUS, File, 1)
          --let $binlog_output_name=master-bin.000002
          if ($binlog_file != $start_binlog_file)
          {
            --source include/show_binlog_events.inc
          }
        }
      }
      --disable_warnings
      drop table if exists t1,t2;
      drop table if exists test2.t2;
      --enable_warnings
    }
  }
}
drop database test2;
--enable_query_log
