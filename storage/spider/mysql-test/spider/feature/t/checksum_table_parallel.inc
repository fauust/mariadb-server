--echo
--echo this test is for MDEV-16967
--echo
--echo drop and create databases

--connection master_1
--disable_warnings
CREATE DATABASE auto_test_local;
USE auto_test_local;

--connection child2_1
SET @old_log_output = @@global.log_output;
SET GLOBAL log_output = 'TABLE,FILE';
CREATE DATABASE auto_test_remote;
USE auto_test_remote;
--connection child2_1_2
USE auto_test_remote;

--connection child2_2
SET @old_log_output = @@global.log_output;
SET GLOBAL log_output = 'TABLE,FILE';
CREATE DATABASE auto_test_remote2;
USE auto_test_remote2;
--connection child2_2_2
USE auto_test_remote2;
--enable_warnings

--echo
--echo create table and insert

--connection child2_1
--disable_query_log
echo CHILD2_1_CREATE_TABLES;
eval $CHILD2_1_CREATE_TABLES;
--enable_query_log
TRUNCATE TABLE mysql.general_log;

--connection child2_2
--disable_query_log
echo CHILD2_2_CREATE_TABLES;
eval $CHILD2_2_CREATE_TABLES;
--enable_query_log
TRUNCATE TABLE mysql.general_log;

--connection master_1
--disable_query_log
echo CREATE TABLE tbl_a (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
) MASTER_1_ENGINE MASTER_1_CHARSET MASTER_1_COMMENT_2_1;
eval CREATE TABLE tbl_a (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
) $MASTER_1_ENGINE $MASTER_1_CHARSET $MASTER_1_COMMENT_2_1;
--enable_query_log
INSERT INTO tbl_a (pkey) VALUES (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);
INSERT INTO tbl_a (pkey) VALUES (10),(11),(12),(13),(14),(15),(16),(17),(18),(19);
INSERT INTO tbl_a (pkey) VALUES (20),(21),(22),(23),(24),(25),(26),(27),(28),(29);

--echo
--echo select test 1

--disable_ps2_protocol
--connection child2_1
TRUNCATE TABLE mysql.general_log;
LOCK TABLE tbl_a WRITE;

--connection child2_2
TRUNCATE TABLE mysql.general_log;
LOCK TABLE tbl_a WRITE;

--connection master_1
send_eval $MASTER_1_CHECKSUM_TABLE;

--connection child2_1_2
SELECT SLEEP(1);
--disable_view_protocol
eval $CHILD2_1_SELECT_ARGUMENT1;

--connection child2_2_2
eval $CHILD2_2_SELECT_ARGUMENT1;
--enable_view_protocol

--connection child2_1
UNLOCK TABLES;
eval $CHILD2_1_SELECT_TABLES;

--connection child2_2
UNLOCK TABLES;
eval $CHILD2_2_SELECT_TABLES;

--connection master_1
reap;
--enable_ps2_protocol

--echo
--echo deinit
--disable_warnings

--connection master_1
DROP DATABASE IF EXISTS auto_test_local;

--connection child2_1
DROP DATABASE IF EXISTS auto_test_remote;
SET GLOBAL log_output = @old_log_output;

--connection child2_2
DROP DATABASE IF EXISTS auto_test_remote2;
SET GLOBAL log_output = @old_log_output;

--enable_warnings
