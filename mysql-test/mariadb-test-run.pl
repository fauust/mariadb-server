#!/usr/bin/env perl
# -*- cperl -*-

# Copyright (c) 2004, 2014, Oracle and/or its affiliates.
# Copyright (c) 2009, 2022, MariaDB Corporation
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

#
##############################################################################
#
#  mysql-test-run.pl
#
#  Tool used for executing a suite of .test files
#
#  See the "MySQL Test framework manual" for more information
#  https://mariadb.com/kb/en/library/mysqltest/
#
#
##############################################################################

use strict;
use warnings;

BEGIN {
  # Check that mysql-test-run.pl is started from mysql-test/
  unless ( -f "mariadb-test-run.pl" )
  {
    print "**** ERROR **** ",
      "You must start mysql-test-run from the mysql-test/ directory\n";
    exit(1);
  }
  # Check that lib exist
  unless ( -d "lib/" )
  {
    print "**** ERROR **** ",
      "Could not find the lib/ directory \n";
    exit(1);
  }

  # Check backward compatibility support
  # By setting the environment variable MTR_VERSION
  # it's possible to use a previous version of
  # mysql-test-run.pl
  my $version= $ENV{MTR_VERSION} || 2;
  if ( $version == 1 )
  {
    print "=======================================================\n";
    print "  WARNING: Using mariadb-test-run.pl version 1!  \n";
    print "=======================================================\n";
    # Should use exec() here on *nix but this appears not to work on Windows
    exit(system($^X, "lib/v1/mariadb-test-run.pl", @ARGV) >> 8);
  }
  elsif ( $version == 2 )
  {
    # This is the current version, just continue
    ;
  }
  else
  {
    print "ERROR: Version $version of mariadb-test-run does not exist!\n";
    exit(1);
  }
}

use lib "lib";

use Cwd ;
use POSIX ":sys_wait_h";
use Getopt::Long qw(:config bundling);
use My::File::Path; # Patched version of File::Path
use File::Basename;
use File::Copy;
use File::Find;
use File::Temp qw/tempdir/;
use File::Spec::Functions qw/splitdir rel2abs/;
use My::Platform;
use My::SafeProcess;
use My::ConfigFactory;
use My::Options;
use My::Tee;
use My::Find;
use My::SysInfo;
use My::CoreDump;
use My::Debugger;
use mtr_cases;
use mtr_report;
use mtr_match;
use mtr_unique;
use mtr_results;
use IO::Socket::INET;
use IO::Select;
use Time::HiRes qw(gettimeofday);

sub realpath($) { (IS_WINDOWS) ? $_[0] : Cwd::realpath($_[0]) }

require "mtr_process.pl";
require "mtr_io.pl";
require "mtr_gprof.pl";
require "mtr_misc.pl";

my $opt_valgrind;
my $valgrind_reports= 0;

$SIG{INT}= sub { mtr_error("Got ^C signal"); };
$SIG{HUP}= sub { mtr_error("Hangup detected on controlling terminal"); };

our $mysql_version_id;
my $mysql_version_extra;
our $glob_mysql_test_dir;
our $basedir;
our $bindir;

our $path_charsetsdir;
our $path_client_bindir;
our $path_client_libdir;
our $path_language;

our $path_current_testlog;
our $path_testlog;

our $opt_open_files_limit;

our $default_vardir;
our $opt_vardir;                # Path to use for var/ dir
our $plugindir;
our $opt_xml_report;            # XML output
our $client_plugindir;
my $path_vardir_trace;          # unix formatted opt_vardir for trace files
my $opt_tmpdir;                 # Path to use for tmp/ dir
my $opt_tmpdir_pid;

my $opt_start;
my $opt_start_dirty;
my $opt_start_exit;
my $start_only;
my $file_wsrep_provider;
my $num_saved_cores= 0;  # Number of core files saved in vardir/log/ so far.

our @global_suppressions;

END {
  if ( defined $opt_tmpdir_pid and $opt_tmpdir_pid == $$ )
  {
    if (!$opt_start_exit)
    {
      # Remove the tempdir this process has created
      mtr_verbose("Removing tmpdir $opt_tmpdir");
      rmtree($opt_tmpdir);
    }
    else
    {
      mtr_warning("tmpdir $opt_tmpdir should be removed after the server has finished");
    }
  }
}

sub env_or_val($$) { defined $ENV{$_[0]} ? $ENV{$_[0]} : $_[1] }

my $path_config_file;           # The generated config file, var/my.cnf

# Visual Studio produces executables in different sub-directories based on the
# configuration used to build them.  To make life easier, an environment
# variable or command-line option may be specified to control which set of
# executables will be used by the test suite.
our $multiconfig = $ENV{'MTR_VS_CONFIG'};

my @DEFAULT_SUITES= qw(
    main-
    archive-
    atomic-
    binlog-
    binlog_encryption-
    client-
    csv-
    compat/oracle-
    compat/mssql-
    compat/maxdb-
    encryption-
    federated-
    funcs_1-
    funcs_2-
    gcol-
    handler-
    heap-
    innodb-
    innodb_fts-
    innodb_gis-
    innodb_i_s-
    innodb_zip-
    json-
    maria-
    mariabackup-
    multi_source-
    optimizer_unfixed_bugs-
    parts-
    perfschema-
    plugins-
    roles-
    rpl-
    stress-
    sys_vars-
    sql_sequence-
    unit-
    vcol-
    versioning-
    period-
    sysschema-
  );
my $opt_suites;

our $opt_verbose= 0;  # Verbose output, enable with --verbose
our $exe_patch;
our $exe_mysql;
our $exe_mysql_plugin;
our $exe_mysqladmin;
our $exe_mysqltest;
our $exe_libtool;
our $exe_mysql_embedded;
our $exe_mariadb_conv;

our $opt_big_test= 0;
our $opt_staging_run= 0;

our @opt_combinations;

our @opt_extra_mysqld_opt;
our @opt_mysqld_envs;

my $opt_stress;
my $opt_tail_lines= 20;

my $opt_dry_run;

my $opt_compress;
my $opt_ssl;
my $opt_skip_ssl;
my @opt_skip_test_list;
our $opt_ssl_supported;
our $opt_ps_protocol;
my $opt_sp_protocol;
our $opt_cursor_protocol;
my $opt_view_protocol;
my $opt_non_blocking_api;

our $opt_debug;
my $debug_d= "d,*";
my $opt_debug_common;
our $opt_debug_server;
our @opt_cases;                  # The test cases names in argv
our $opt_embedded_server;

# Options used when connecting to an already running server
my %opts_extern;
sub using_extern { return (keys %opts_extern > 0);};

our $opt_fast= 0;
our $opt_force= 0;
our $opt_skip_not_found= 0;
our $opt_mem= $ENV{'MTR_MEM'};
our $opt_clean_vardir= $ENV{'MTR_CLEAN_VARDIR'};
our $opt_catalogs= 0;
our $opt_catalog_name="";
our $catalog_name="def";

our $opt_gcov;
our $opt_gprof;
our %gprof_dirs;

my $config; # The currently running config
my $current_config_name; # The currently running config file template

our @opt_experimentals;
our $experimental_test_cases= [];

our $baseport;
# $opt_build_thread may later be set from $opt_port_base
my $opt_build_thread= $ENV{'MTR_BUILD_THREAD'} || "auto";
my $opt_port_base= $ENV{'MTR_PORT_BASE'} || "auto";
my $build_thread= 0;

my $opt_record;

our $opt_resfile= $ENV{'MTR_RESULT_FILE'} || 0;

my $opt_skip_core;

our $opt_check_testcases= 1;
my $opt_mark_progress;
my $opt_max_connections;
our $opt_report_times= 0;

my $opt_sleep;

our $opt_retry= 1;
our $opt_retry_failure= env_or_val(MTR_RETRY_FAILURE => 2);
our $opt_testcase_timeout= $ENV{MTR_TESTCASE_TIMEOUT} ||  15; # minutes
our $opt_suite_timeout   = $ENV{MTR_SUITE_TIMEOUT}    || 360; # minutes
our $opt_shutdown_timeout= $ENV{MTR_SHUTDOWN_TIMEOUT} ||  10; # seconds
our $opt_start_timeout   = $ENV{MTR_START_TIMEOUT}    || 180; # seconds

sub suite_timeout { return $opt_suite_timeout * 60; };

my $opt_wait_all;
my $opt_user_args;
my $opt_repeat= 1;
my $opt_reorder= 1;
my $opt_force_restart= 0;

our $opt_user = "root";

my %mysqld_logs;
my $opt_debug_sync_timeout= 300; # Default timeout for WAIT_FOR actions.
my $warn_seconds = 60;

my $rebootstrap_re= '--innodb[-_](?:page[-_]size|checksum[-_]algorithm|undo[-_]tablespaces|log[-_]group[-_]home[-_]dir|data[-_]home[-_]dir)|data[-_]file[-_]path|force_rebootstrap';

sub testcase_timeout ($) { return $opt_testcase_timeout * 60; }
sub check_timeout ($) { return testcase_timeout($_[0]); }

our $opt_warnings= 1;

our %mysqld_variables;
our @optional_plugins;

my $source_dist=  -d "../sql";

my $opt_max_save_core= env_or_val(MTR_MAX_SAVE_CORE => 5);
my $opt_max_save_datadir= env_or_val(MTR_MAX_SAVE_DATADIR => 20);
my $opt_max_test_fail= env_or_val(MTR_MAX_TEST_FAIL => 10);
my $opt_core_on_failure= 0;

my $opt_parallel= $ENV{MTR_PARALLEL} || 1;
# Some galera tests starts 6 galera nodes. Each galera node requires
# three ports: 6*3 = 18. Plus 6 ports are needed for 6 mariadbd servers.
# Since the number of ports is rounded up to 10 everywhere, we will
# take 30 as the default value:
my $opt_port_group_size = $ENV{MTR_PORT_GROUP_SIZE} || 30;

# lock file to stop tests
my $opt_stop_file= $ENV{MTR_STOP_FILE};
# print messages when test suite is stopped (for buildbot)
my $opt_stop_keep_alive= $ENV{MTR_STOP_KEEP_ALIVE};

select(STDOUT);
$| = 1; # Automatically flush STDOUT

main();

sub main {
  $ENV{MTR_PERL}= mixed_path($^X);

  # Default, verbosity on
  report_option('verbose', 0);

  # This is needed for test log evaluation in "gen-build-status-page"
  # in all cases where the calling tool does not log the commands
  # directly before it executes them, like "make test-force-pl" in RPM builds.
  mtr_report("Logging: $0 ", join(" ", @ARGV));

  command_line_setup();

  # --help will not reach here, so now it's safe to assume we have binaries
  My::SafeProcess::find_bin();

  print "vardir: $opt_vardir\n";
  initialize_servers();
  init_timers();

  unless (IS_WINDOWS) {
    binmode(STDOUT,":via(My::Tee)") or die "binmode(STDOUT, :via(My::Tee)):$!";
    binmode(STDERR,":via(My::Tee)") or die "binmode(STDERR, :via(My::Tee)):$!";
  }

  mtr_report("Checking supported features...");

  executable_setup();

  # --debug[-common] implies we run debug server
  $opt_debug_server= 1 if $opt_debug || $opt_debug_common;

  if (using_extern())
  {
    # Connect to the running mysqld and find out what it supports
    collect_mysqld_features_from_running_server();
  }
  else
  {
    # Run the mysqld to find out what features are available
    collect_mysqld_features();
  }
  check_ssl_support();
  check_debug_support();
  environment_setup();

  if (!$opt_suites) {
    $opt_suites= join ',', collect_default_suites(@DEFAULT_SUITES);
  }
  mtr_report("Using suites: $opt_suites") unless @opt_cases;

  mtr_report("Collecting tests...");
  my $tests= collect_test_cases($opt_reorder, $opt_suites, \@opt_cases, \@opt_skip_test_list);
  if (@$tests == 0) {
    mtr_report("No tests to run...");
    exit 0;
  }

  mark_time_used('collect');

  if (!using_extern())
  {
    mysql_install_db(default_mysqld(), "$opt_vardir/install.db");
    make_readonly("$opt_vardir/install.db");
  }
  if ($opt_dry_run)
  {
    for (@$tests) {
      print $_->fullname(), "\n";
    }
    exit 0;
  }

  if ($opt_gcov) {
    system './dgcov.pl --purge';
  }
  
  #######################################################################
  my $num_tests= $mtr_report::tests_total= @$tests;
  if ( $opt_parallel eq "auto" ) {
    # Try to find a suitable value for number of workers
    if (IS_WINDOWS)
    {
      $opt_parallel= $ENV{NUMBER_OF_PROCESSORS} || 1;
    }
    elsif (IS_MAC || IS_FREEBSD)
    {
      $opt_parallel= `sysctl -n hw.ncpu`;
    }
    else
    {
      my $sys_info= My::SysInfo->new();
      $opt_parallel= $sys_info->num_cpus()+int($sys_info->min_bogomips()/500)-4;
    }
    my $max_par= $ENV{MTR_MAX_PARALLEL} || 8;
    $opt_parallel= $max_par if ($opt_parallel > $max_par);
    $opt_parallel= $num_tests if ($opt_parallel > $num_tests);
    $opt_parallel= 1 if ($opt_parallel < 1);
    mtr_report("Using parallel: $opt_parallel");
  }
  $ENV{MTR_PARALLEL} = $opt_parallel;

  if ($opt_parallel > 1 && ($opt_start_exit || $opt_stress)) {
    mtr_warning("Parallel cannot be used with --start-and-exit or --stress\n" .
               "Setting parallel to 1");
    $opt_parallel= 1;
  }

  # Create server socket on any free port
  my $server = new IO::Socket::INET
    (
     LocalAddr => 'localhost',
     Proto => 'tcp',
     Listen => $opt_parallel,
    );
  mtr_error("Could not create testcase server port: $!") unless $server;
  my $server_port = $server->sockport();

  if ($opt_resfile) {
    resfile_init("$opt_vardir/mtr-results.txt");
    print_global_resfile();
  }

  # Create child processes
  my %children;
  for my $child_num (1..$opt_parallel){
    my $child_pid= My::SafeProcess::Base::_safe_fork();
    if ($child_pid == 0){
      $server= undef; # Close the server port in child
      $tests= {}; # Don't need the tests list in child

      # Use subdir of var and tmp unless only one worker
      if ($opt_parallel > 1) {
	set_vardir("$opt_vardir/$child_num");
	$opt_tmpdir= "$opt_tmpdir/$child_num";
      }

      init_timers();
      run_worker($server_port, $child_num);
      exit(1);
    }

    $children{$child_pid}= 1;
  }
  #######################################################################

  mtr_report();
  mtr_print_thick_line();
  mtr_print_header($opt_parallel > 1);

  mark_time_used('init');

  my ($prefix, $fail, $completed, $extra_warnings)=
    Manager::run($server, $tests, \%children);

  exit(0) if $opt_start_exit;

  # Send Ctrl-C to any children still running
  kill("INT", keys(%children));

  if (!IS_WINDOWS) { 
    # Wait for children to exit
    foreach my $pid (keys %children)
    {
      my $ret_pid= waitpid($pid, 0);
      if ($ret_pid == -1) {
        # Child was automatically reaped. Probably not possible
        # unless you $SIG{CHLD}= 'IGNORE'
        mtr_warning("Child ${pid} was automatically reaped (this should never happen)");
      } elsif ($ret_pid != $pid) {
        confess("Unexpected PID ${ret_pid} instead of expected ${pid}");
      }
      my $exit_status= ($? >> 8);
      mtr_verbose2("Child ${pid} exited with status ${exit_status}");
      delete $children{$ret_pid};
    }
  }

  if ( not @$completed ) {
    my $test_name= mtr_grab_file($path_testlog);
    $test_name =~ s/^CURRENT_TEST:\s//;
    chomp($test_name);
    my $tinfo = My::Test->new(name => $test_name);
    $tinfo->{result}= 'MTR_RES_FAILED';
    $tinfo->{comment}=' ';
    mtr_report_test($tinfo);
    mtr_error("Test suite aborted");
  }

  if ( @$completed != $num_tests){

    # Not all tests completed, failure
    mtr_report();
    mtr_report("Only ", int(@$completed), " of $num_tests completed.");
  }

  if ($opt_valgrind) {
    # Create minimalistic "test" for the reporting
    my $tinfo = My::Test->new
      (
       suite          => { name => 'valgrind', },
       name           => 'valgrind_report',
      );
    # Set dummy worker id to align report with normal tests
    $tinfo->{worker} = 0 if $opt_parallel > 1;
    if ($valgrind_reports) {
      $tinfo->{result}= 'MTR_RES_FAILED';
      $tinfo->{comment}= "Valgrind reported failures at shutdown, see above";
      $tinfo->{failures}= 1;
    } else {
      $tinfo->{result}= 'MTR_RES_PASSED';
    }
    mtr_report_test($tinfo);
    push @$completed, $tinfo;
    ++$num_tests
  }

  mtr_print_line();

  print_total_times($opt_parallel) if $opt_report_times;
  mtr_report_stats($prefix, $fail, $completed, $extra_warnings);

  if ($opt_gcov) {
    mtr_report("Running dgcov");
    system "./dgcov.pl --generate > $opt_vardir/last_changes.dgcov";
  }

  if ( @$completed != $num_tests)
  {
    mtr_error("Not all tests completed (only ". scalar(@$completed) .
              " of $num_tests)");
  }

  remove_vardir_subs() if $opt_clean_vardir;

  exit(0);
}


package Manager;
use POSIX ":sys_wait_h";
use File::Basename;
use File::Find;
use IO::Socket::INET;
use IO::Select;
use mtr_report;
use My::Platform;

my $num_saved_datadir;  # Number of datadirs saved in vardir/log/ so far.
my $num_failed_test; # Number of tests failed so far
my $test_failure;    # Set true if test suite failed
my $extra_warnings; # Warnings found during server shutdowns

my $completed;
my %running;
my $result;
my $exe_mysqld; # Used as hint to CoreDump
my %names;

sub parse_protocol($$) {
  my $sock= shift;
  my $line= shift;

  if ($line eq 'TESTRESULT'){
    mtr_verbose2("Got TESTRESULT from ". $names{$sock});
    $result= My::Test::read_test($sock);

    # Report test status
    mtr_report_test($result);

    if ( $result->is_failed() ) {

      # Save the workers "savedir" in var/log
      my $worker_savedir= $result->{savedir};
      my $worker_savename= basename($worker_savedir);
      my $savedir= "$opt_vardir/log/$worker_savename";

      # Move any core files from e.g. mysqltest
      foreach my $coref (glob("core*"), glob("*.dmp"))
      {
        mtr_report(" - found '$coref', moving it to '$worker_savedir'");
        ::move($coref, $worker_savedir);
      }

      find(
      {
        no_chdir => 1,
        wanted => sub
        {
          My::CoreDump::core_wanted(\$num_saved_cores,
                                    $opt_max_save_core,
                                    @opt_cases == 0,
                                    $exe_mysqld, $opt_parallel);
        }
      },
      $worker_savedir);

      if ($num_saved_datadir >= $opt_max_save_datadir)
      {
        mtr_report(" - skipping '$worker_savedir/'");
        main::rmtree($worker_savedir);
      }
      else
      {
        mtr_report(" - saving '$worker_savedir/' to '$savedir/'");
        rename($worker_savedir, $savedir);
        $num_saved_datadir++;
      }
      main::resfile_print_test();
      $num_failed_test++ unless ($result->{retries} ||
                                $result->{exp_fail});

      $test_failure= 1;
      if ( !$opt_force ) {
        # Test has failed, force is off
        push(@$completed, $result);
        if ($result->{'dont_kill_server'})
        {
          mtr_verbose2("${line}: saying BYE to ". $names{$sock});
          print $sock "BYE\n";
          return 2;
        }
        return ["Failure", 1, $completed, $extra_warnings];
      }
      elsif ($opt_max_test_fail > 0 and
            $num_failed_test >= $opt_max_test_fail) {
        push(@$completed, $result);
        mtr_report("Too many tests($num_failed_test) failed!",
                  "Terminating...");
        return ["Too many failed", 1, $completed, $extra_warnings];
      }
    }

    main::resfile_print_test();
    # Retry test run after test failure
    my $retries= $result->{retries} || 2;
    my $test_has_failed= $result->{failures} || 0;
    if ($test_has_failed and $retries <= $opt_retry){
      # Test should be run one more time unless it has failed
      # too many times already
      my $tname= $result->{name};
      my $failures= $result->{failures};
      if ($opt_retry > 1 and $failures >= $opt_retry_failure){
        mtr_report("\nTest $tname has failed $failures times,",
                  "no more retries!\n");
      }
      else {
        mtr_report("\nRetrying test $tname, ".
                  "attempt($retries/$opt_retry)...\n");
        #saving the log file as filename.failed in case of retry
        if ( $result->is_failed() ) {
          my $worker_logdir= $result->{savedir};
          my $log_file_name=dirname($worker_logdir)."/".$result->{shortname}.".log";

          if (-e $log_file_name) {
            $result->{'logfile-failed'} = ::mtr_lastlinesfromfile($log_file_name, 20);
          } else {
            $result->{'logfile-failed'} = "";
          }

          rename $log_file_name, $log_file_name.".failed";
        }
        {
          local @$result{'retries', 'result'};
          delete $result->{result};
          $result->{retries}= $retries+1;
          $result->write_test($sock, 'TESTCASE');
        }
        push(@$completed, $result);
        return 2;
      }
    }

    # Repeat test $opt_repeat number of times
    my $repeat= $result->{repeat} || 1;
    if ($repeat < $opt_repeat)
    {
      $result->{retries}= 0;
      $result->{rep_failures}++ if $result->{failures};
      $result->{failures}= 0;
      delete($result->{result});
      $result->{repeat}= $repeat+1;
      $result->write_test($sock, 'TESTCASE');
      return 2;
    }

    # Remove from list of running
    mtr_error("'", $result->{name},"' is not known to be running")
      unless delete $running{$result->key()};

    # Save result in completed list
    push(@$completed, $result);

  } # if ($line eq 'TESTRESULT')
  elsif ($line=~ /^START (.*)$/){
    # Send first test
    $names{$sock}= $1;
  }
  elsif ($line eq 'WARNINGS'){
    my $fake_test= My::Test::read_test($sock);
    my $test_list= join (" ", @{$fake_test->{testnames}});
    push @$extra_warnings, $test_list;
    my $report= $fake_test->{'warnings'};
    mtr_report("***Warnings generated in error logs during shutdown ".
              "after running tests: $test_list\n\n$report");
    $test_failure= 1;
    if ( !$opt_force ) {
      # Test failure due to warnings, force is off
      mtr_verbose2("Socket loop exiting 3");
      return ["Warnings in log", 1, $completed, $extra_warnings];
    }
    return 1;
  }
  elsif ($line =~ /^SPENT/) {
    main::add_total_times($line);
  }
  elsif ($line eq 'VALGREP' && $opt_valgrind) {
    $valgrind_reports= 1;
  }
  else {
    mtr_error("Unknown response: '$line' from client");
  }
  return 0;
}

sub run ($$$) {
  my ($server, $tests, $children) = @_;
  my $suite_timeout= main::start_timer(main::suite_timeout());
  $exe_mysqld= main::find_mysqld($bindir) || ""; # Used as hint to CoreDump
  $num_saved_datadir= 0;  # Number of datadirs saved in vardir/log/ so far.
  $num_failed_test= 0; # Number of tests failed so far
  $test_failure= 0;    # Set true if test suite failed
  $extra_warnings= []; # Warnings found during server shutdowns
  $completed= [];

  my $s= IO::Select->new();
  my $childs= 0;

  $s->add($server);
  while (1) {
    if ($opt_stop_file)
    {
      if (mtr_wait_lock_file($opt_stop_file, $opt_stop_keep_alive))
      {
        # We were waiting so restart timer process
        my $suite_timeout= main::start_timer(main::suite_timeout());
      }
    }

    main::mark_time_used('admin');
    my @ready = $s->can_read(1); # Wake up once every second
    if (@ready > 0) {
      mtr_verbose2("Got ". (0 + @ready). " connection(s)");
    }
    main::mark_time_idle();
    my $i= 0;
    sock_loop: foreach my $sock (@ready) {
      ++$i;
      if ($sock == $server) {
	# New client connected
        ++$childs;
	my $child= $sock->accept();
        mtr_verbose2("Connection ${i}: Worker connected (got ${childs} childs)");
	$s->add($child);
	print $child "HELLO\n";
      }
      else {
        my $j= 0;
        $sock->blocking(0);
        while (my $line= <$sock>) {
          ++$j;
          chomp($line);
          mtr_verbose2("Connection ${i}.${j}". (exists $names{$sock} ? " from $names{$sock}" : "") .": $line");

          $sock->blocking(1);
          my $res= parse_protocol($sock, $line);
          $sock->blocking(0);
          if (ref $res eq 'ARRAY') {
            return @$res;
          } elsif ($res == 1) {
             next;
          } elsif ($res == 2) {
             next sock_loop;
          }
          if (IS_WINDOWS and !IS_CYGWIN) {
            # Strawberry and ActiveState don't support blocking(0), the next iteration will be blocked!
            # If there is next response now in the buffer and it is TESTRESULT we are affected by MDEV-30836 and the manager will hang.
            last;
          }
        }
        $sock->blocking(1);
        if ($j == 0) {
          # Client disconnected
          --$childs;
          mtr_verbose2((exists $names{$sock} ? $names{$sock} : "Worker"). " closed socket (left ${childs} childs)");
          $s->remove($sock);
          $sock->close;
          next;
        }

	# Find next test to schedule
	# - Try to use same configuration as worker used last time

	my $next;
	my $second_best;
	for(my $i= 0; $i <= @$tests; $i++)
	{
	  my $t= $tests->[$i];

	  last unless defined $t;

	  if (main::run_testcase_check_skip_test($t)){
	    # Move the test to completed list
	    #mtr_report("skip - Moving test $i to completed");
	    push(@$completed, splice(@$tests, $i, 1));

	    # Since the test at pos $i was taken away, next
	    # test will also be at $i -> redo
	    redo;
	  }

	  # From secondary choices, we prefer to pick a 'long-running' test if
          # possible; this helps avoid getting stuck with a few of those at the
          # end of high --parallel runs, with most workers being idle.
	  if (!defined $second_best ||
              ($t->{'long_test'} && !($tests->[$second_best]{'long_test'}))){
	    #mtr_report("Setting second_best to $i");
	    $second_best= $i;
	  }

	  # Smart allocation of next test within this thread.

	  if ($opt_reorder and $opt_parallel > 1 and defined $result)
	  {
	    my $wid= $result->{worker};
	    # Reserved for other thread, try next
	    next if (defined $t->{reserved} and $t->{reserved} != $wid);
	    if (! defined $t->{reserved})
	    {
	      # Force-restart not relevant when comparing *next* test
	      $t->{criteria} =~ s/force-restart$/no-restart/;
	      my $criteria= $t->{criteria};
	      # Reserve similar tests for this worker, but not too many
	      my $maxres= (@$tests - $i) / $opt_parallel + 1;
	      for (my $j= $i+1; $j <= $i + $maxres; $j++)
	      {
		my $tt= $tests->[$j];
		last unless defined $tt;
		last if $tt->{criteria} ne $criteria;
		$tt->{reserved}= $wid;
	      }
	    }
	  }

	  # At this point we have found next suitable test
	  $next= splice(@$tests, $i, 1);
	  last;
	} # for(my $i= 0; $i <= @$tests; $i++)

	# Use second best choice if no other test has been found
	if (!$next and defined $second_best){
	  #mtr_report("Take second best choice $second_best");
	  mtr_error("Internal error, second best too large($second_best)")
	    if $second_best >  $#$tests;
	  $next= splice(@$tests, $second_best, 1);
	  delete $next->{reserved};
	}

	if ($next) {
	  # We don't need this any more
	  delete $next->{criteria};
	  $next->write_test($sock, 'TESTCASE');
	  $running{$next->key()}= $next;
	}
	else {
	  # No more test, tell child to exit
	  mtr_verbose2("Saying BYE to ". $names{$sock});
	  print $sock "BYE\n";
	} # else (!$next)
      } # else ($sock != $server)
    } # foreach my $sock (@ready)

    if (!IS_WINDOWS) {
      foreach my $pid (keys %$children)
      {
        my $res= waitpid($pid, WNOHANG);
        if ($res == $pid || $res == -1) {
          if ($res == -1) {
            # Child was automatically reaped. Probably not possible
            # unless you $SIG{CHLD}= 'IGNORE'
            mtr_warning("Child ${pid} was automatically reaped (this should never happen)");
          }
          my $exit_status= ($? >> 8);
          mtr_verbose2("Child ${pid} exited with status ${exit_status}");
          delete $children->{$pid};
          if (!%$children && $childs) {
            mtr_verbose2("${childs} children didn't close socket before dying!");
            $childs= 0;
          }
        } elsif ($res != 0) {
          confess("Unexpected result ${res} on waitpid(${pid}, WNOHANG)");
        }
      }
    }

    if ($childs == 0){
      return ("Completed", $test_failure, $completed, $extra_warnings);
    }

    # ----------------------------------------------------
    # Check if test suite timer expired
    # ----------------------------------------------------
    if ( main::has_expired($suite_timeout) )
    {
      mtr_report("Test suite timeout! Terminating...");
      return ("Timeout", 1, $completed, $extra_warnings);
    }
  }
}

1;

package main;

sub run_worker ($) {
  my ($server_port, $thread_num)= @_;

  $SIG{INT}= sub { exit(1); };
  $SIG{HUP}= sub { exit(1); };

  # Connect to server
  my $server = new IO::Socket::INET
    (
     PeerAddr => 'localhost',
     PeerPort => $server_port,
     Proto    => 'tcp'
    );
  mtr_error("Could not connect to server at port $server_port: $!")
    unless $server;

  # --------------------------------------------------------------------------
  # Set worker name
  # --------------------------------------------------------------------------
  report_option('name',"worker[". sprintf("%02d", $thread_num). "]");
  my $proc_title= basename($0). " ${mtr_report::name} :". $server->sockport(). " -> :${server_port}";
  $0= $proc_title;
  mtr_verbose2("Running at PID $$");

  # --------------------------------------------------------------------------
  # Set different ports per thread
  # --------------------------------------------------------------------------
  set_build_thread_ports($thread_num);

  # --------------------------------------------------------------------------
  # Turn off verbosity in workers, unless explicitly specified
  # --------------------------------------------------------------------------
  report_option('verbose', undef) if ($opt_verbose == 0);

  environment_setup();

  # Read hello from server which it will send when shared
  # resources have been setup
  my $hello= <$server>;

  setup_vardir();
  check_running_as_root();

  if ( using_extern() ) {
    create_config_file_for_extern(%opts_extern);
  }

  # Ask server for first test
  print $server "START ${mtr_report::name}\n";

  mark_time_used('init');

  while (my $line= <$server>){
    chomp($line);
    if ($line eq 'TESTCASE'){
      my $test= My::Test::read_test($server);
      $0= $proc_title. " ". $test->{name};

      # Clear comment and logfile, to avoid
      # reusing them from previous test
      delete($test->{'comment'});
      delete($test->{'logfile'});

      # A sanity check. Should this happen often we need to look at it.
      if (defined $test->{reserved} && $test->{reserved} != $thread_num) {
	my $tres= $test->{reserved};
	mtr_warning("Test reserved for w$tres picked up by w$thread_num");
      }
      $test->{worker} = $thread_num if $opt_parallel > 1;

      run_testcase($test, $server);
      #$test->{result}= 'MTR_RES_PASSED';
      # Send it back, now with results set
      mtr_verbose2('Writing TESTRESULT');
      $test->write_test($server, 'TESTRESULT');
      mark_time_used('restart');
    }
    elsif ($line eq 'BYE'){
      mtr_verbose2("Manager said BYE");
      # We need to gracefully shut down the servers to see any
      # Valgrind memory leak errors etc. since last server restart.
      if ($opt_warnings) {
        stop_servers(reverse all_servers());
        if(check_warnings_post_shutdown($server)) {
          # Warnings appeared in log file(s) during final server shutdown.
          exit(1);
        }
      }
      else {
        stop_all_servers($opt_shutdown_timeout);
      }
      mark_time_used('restart');
      my $valgrind_reports= 0;
      if ($opt_valgrind) {
        $valgrind_reports= valgrind_exit_reports();
	print $server "VALGREP\n" if $valgrind_reports;
      }
      if ( $opt_gprof ) {
	gprof_collect (find_mysqld($bindir), keys %gprof_dirs);
      }
      mark_time_used('admin');
      print_times_used($server, $thread_num);
      exit($valgrind_reports);
    }
    else {
      mtr_error("Could not understand server, '$line'");
    }
  }

  stop_all_servers();

  exit(1);
}


# Setup any paths that are $opt_vardir related
sub set_vardir {
  ($opt_vardir)= @_;

  $path_vardir_trace= $opt_vardir;
  # Chop off any "c:", DBUG likes a unix path ex: c:/src/... => /src/...
  $path_vardir_trace=~ s/^\w://;

  # Location of my.cnf that all clients use
  $path_config_file= "$opt_vardir/my.cnf";

  $path_testlog=         "$opt_vardir/log/mysqltest.log";
  $path_current_testlog= "$opt_vardir/log/current_test";

}


sub print_global_resfile {
  resfile_global("start_time", isotime $^T);
  resfile_global("user_id", $<);
  resfile_global("embedded-server", $opt_embedded_server ? 1 : 0);
  resfile_global("ps-protocol", $opt_ps_protocol ? 1 : 0);
  resfile_global("sp-protocol", $opt_sp_protocol ? 1 : 0);
  resfile_global("view-protocol", $opt_view_protocol ? 1 : 0);
  resfile_global("cursor-protocol", $opt_cursor_protocol ? 1 : 0);
  resfile_global("ssl", $opt_ssl ? 1 : 0);
  resfile_global("compress", $opt_compress ? 1 : 0);
  resfile_global("parallel", $opt_parallel);
  resfile_global("check-testcases", $opt_check_testcases ? 1 : 0);
  resfile_global("mariadbd", \@opt_extra_mysqld_opt);
  resfile_global("debug", $opt_debug ? 1 : 0);
  resfile_global("gcov", $opt_gcov ? 1 : 0);
  resfile_global("gprof", $opt_gprof ? 1 : 0);
  resfile_global("mem", $opt_mem);
  resfile_global("tmpdir", $opt_tmpdir);
  resfile_global("vardir", $opt_vardir);
  resfile_global("fast", $opt_fast ? 1 : 0);
  resfile_global("force-restart", $opt_force_restart ? 1 : 0);
  resfile_global("reorder", $opt_reorder ? 1 : 0);
  resfile_global("sleep", $opt_sleep);
  resfile_global("repeat", $opt_repeat);
  resfile_global("user", $opt_user);
  resfile_global("testcase-timeout", $opt_testcase_timeout);
  resfile_global("suite-timeout", $opt_suite_timeout);
  resfile_global("shutdown-timeout", $opt_shutdown_timeout ? 1 : 0);
  resfile_global("warnings", $opt_warnings ? 1 : 0);
  resfile_global("max-connections", $opt_max_connections);
  resfile_global("product", "MariaDB");
  resfile_global("xml-report", $opt_xml_report);
  # Somewhat hacky code to convert numeric version back to dot notation
  my $v1= int($mysql_version_id / 10000);
  my $v2= int(($mysql_version_id % 10000)/100);
  my $v3= $mysql_version_id % 100;
  resfile_global("version", "$v1.$v2.$v3");
}



sub command_line_setup {
  my $opt_comment;
  my $opt_usage;
  my $opt_list_options;

  # Read the command line options
  # Note: Keep list in sync with usage at end of this file
  Getopt::Long::Configure("pass_through");
  my %options=(
             # Control what engine/variation to run
             'embedded-server'          => \$opt_embedded_server,
             'ps-protocol'              => \$opt_ps_protocol,
             'sp-protocol'              => \$opt_sp_protocol,
             'view-protocol'            => \$opt_view_protocol,
             'cursor-protocol'          => \$opt_cursor_protocol,
             'non-blocking-api'         => \$opt_non_blocking_api,
             'ssl|with-openssl'         => \$opt_ssl,
             'skip-ssl'                 => \$opt_skip_ssl,
             'compress'                 => \$opt_compress,
             'vs-config=s'              => \$multiconfig,

	     # Max number of parallel threads to use
	     'parallel=s'               => \$opt_parallel,

             # Config file to use as template for all tests
	     'defaults-file=s'          => \&collect_option,
	     # Extra config file to append to all generated configs
	     'defaults-extra-file=s'    => \&collect_option,

             # Control what test suites or cases to run
             'force+'                   => \$opt_force,
             'skip-not-found'           => \$opt_skip_not_found,
             'suite|suites=s'           => \$opt_suites,
             'skip-rpl'                 => \&collect_option,
             'skip-test=s'              => \&collect_option,
             'do-test=s'                => \&collect_option,
             'start-from=s'             => \&collect_option,
             'big-test+'                => \$opt_big_test,
	     'combination=s'            => \@opt_combinations,
             'experimental=s'           => \@opt_experimentals,
             'staging-run'              => \$opt_staging_run,

             # Specify ports
	     'build-thread|mtr-build-thread=i' => \$opt_build_thread,
	     'port-base|mtr-port-base=i'       => \$opt_port_base,
	     'port-group-size=s'        => \$opt_port_group_size,

             # Test case authoring
             'record'                   => \$opt_record,
             'check-testcases!'         => \$opt_check_testcases,
             'mark-progress'            => \$opt_mark_progress,

             # Extra options used when starting mariadbd
             'mariadbd=s'               => \@opt_extra_mysqld_opt,
             'mariadbd-env=s'           => \@opt_mysqld_envs,
             # mysqld is an alias for mariadbd
             'mysqld=s'                 => \@opt_extra_mysqld_opt,
             'mysqld-env=s'             => \@opt_mysqld_envs,

             # Run test on running server
             'extern=s'                  => \%opts_extern, # Append to hash

             # Debugging
             'debug'                    => \$opt_debug,
             'debug-common'             => \$opt_debug_common,
             'debug-server'             => \$opt_debug_server,
             'max-save-core=i'          => \$opt_max_save_core,
             'max-save-datadir=i'       => \$opt_max_save_datadir,
             'max-test-fail=i'          => \$opt_max_test_fail,
             'core-on-failure'          => \$opt_core_on_failure,

             # Coverage, profiling etc
             'gcov'                     => \$opt_gcov,
             'gprof'                    => \$opt_gprof,
	     'debug-sync-timeout=i'     => \$opt_debug_sync_timeout,

	     # Directories
             'tmpdir=s'                 => \$opt_tmpdir,
             'vardir=s'                 => \$opt_vardir,
             'mem'                      => \$opt_mem,
	     'clean-vardir'             => \$opt_clean_vardir,
             'client-bindir=s'          => \$path_client_bindir,
             'client-libdir=s'          => \$path_client_libdir,

             # Misc
             'comment=s'                => \$opt_comment,
             'fast'                     => \$opt_fast,
	     'force-restart'            => \$opt_force_restart,
             'reorder!'                 => \$opt_reorder,
             'enable-disabled'          => \&collect_option,
             'verbose|v+'               => \$opt_verbose,
             'verbose-restart'          => \&report_option,
             'sleep=i'                  => \$opt_sleep,
             'start-dirty'              => \$opt_start_dirty,
             'start-and-exit'           => \$opt_start_exit,
             'start'                    => \$opt_start,
	     'user-args'                => \$opt_user_args,
             'wait-all'                 => \$opt_wait_all,
	     'print-testcases'          => \&collect_option,
	     'repeat=i'                 => \$opt_repeat,
	     'retry=i'                  => \$opt_retry,
	     'retry-failure=i'          => \$opt_retry_failure,
             'timer!'                   => \&report_option,
             'user=s'                   => \$opt_user,
             'testcase-timeout=i'       => \$opt_testcase_timeout,
             'suite-timeout=i'          => \$opt_suite_timeout,
             'shutdown-timeout=i'       => \$opt_shutdown_timeout,
             'warnings!'                => \$opt_warnings,
	     'timestamp'                => \&report_option,
	     'timediff'                 => \&report_option,
             'stop-file=s'              => \$opt_stop_file,
             'stop-keep-alive=i'        => \$opt_stop_keep_alive,
	     'max-connections=i'        => \$opt_max_connections,
	     'report-times'             => \$opt_report_times,
	     'result-file'              => \$opt_resfile,
	     'stress=s'                 => \$opt_stress,
	     'tail-lines=i'             => \$opt_tail_lines,
             'dry-run'                  => \$opt_dry_run,

             'help|h'                   => \$opt_usage,
	     # list-options is internal, not listed in help
	     'list-options'             => \$opt_list_options,
             'skip-test-list=s'         => \@opt_skip_test_list,
             'xml-report=s'             => \$opt_xml_report,
             'open-files-limit=i',      => \$opt_open_files_limit,

             My::Debugger::options(),
             My::CoreDump::options(),
             My::Platform::options()
           );

  # fix options (that take an optional argument and *only* after = sign
  @ARGV = My::Debugger::fix_options(@ARGV);
  GetOptions(%options) or usage("Can't read options");
  usage("") if $opt_usage;
  list_options(\%options) if $opt_list_options;

  # --------------------------------------------------------------------------
  # Setup verbosity
  # --------------------------------------------------------------------------
  if ($opt_verbose != 0){
    report_option('verbose', $opt_verbose);
  }

  # Negative values aren't meaningful on integer options
  foreach(grep(/=i$/, keys %options))
  {
    if (defined ${$options{$_}} &&
        do { no warnings "numeric"; int ${$options{$_}} < 0})
    {
      my $v= (split /=/)[0];
      die("$v doesn't accept a negative value:");
    }
  }

  # Find the absolute path to the test directory
  $glob_mysql_test_dir= cwd();
  if ($glob_mysql_test_dir =~ / /)
  {
    die("Working directory \"$glob_mysql_test_dir\" contains space\n".
	"Bailing out, cannot function properly with space in path");
  }
  if (IS_CYGWIN)
  {
    if (My::Platform::check_cygwin_subshell()) {
      die("Cygwin /bin/sh subshell requires fix with --cygwin-subshell-fix=do\n");
    }
    # Use mixed path format i.e c:/path/to/
    $glob_mysql_test_dir= mixed_path($glob_mysql_test_dir);
  }

  # In most cases, the base directory we find everything relative to,
  # is the parent directory of the "mysql-test" directory. For source
  # distributions, TAR binary distributions and some other packages.
  $basedir= dirname($glob_mysql_test_dir);

  # In the RPM case, binaries and libraries are installed in the
  # default system locations, instead of having our own private base
  # directory. And we install "/usr/share/mysql-test". Moving up one
  # more directory relative to "mysql-test" gives us a usable base
  # directory for RPM installs.
  if ( ! $source_dist and ! -d "$basedir/bin" )
  {
    $basedir= dirname($basedir);
  }
  # For .deb, it's like RPM, but installed in /usr/share/mysql/mysql-test.
  # So move up one more directory level yet.
  if ( ! $source_dist and ! -d "$basedir/bin" )
  {
    $basedir= dirname($basedir);
  }

  # Respect MTR_BINDIR variable, which is typically set in to the 
  # build directory in out-of-source builds.
  $bindir=$ENV{MTR_BINDIR}||$basedir;
  
  fix_vs_config_dir();

  # Look for the client binaries directory
  if ($path_client_bindir)
  {
    # --client-bindir=path set on command line, check that the path exists
    $path_client_bindir= mtr_path_exists($path_client_bindir);
  }
  else
  {
    $path_client_bindir= mtr_path_exists("$bindir/client_release",
					 "$bindir/client_debug",
					 "$bindir/client/$multiconfig",
					 "$bindir/client$multiconfig",
					 "$bindir/client",
					 "$bindir/bin");
  }

  # Look for language files and charsetsdir, use same share
  $path_language=   mtr_path_exists("$bindir/share/mariadb",
                                    "$bindir/share/mysql",
                                    "$bindir/sql/share",
                                    "$bindir/share");
  my $path_share= $path_language;
  $path_charsetsdir =   mtr_path_exists("$basedir/share/mariadb/charsets",
                                    "$basedir/share/mysql/charsets",
                                    "$basedir/sql/share/charsets",
                                    "$basedir/share/charsets");
  if ( $opt_comment )
  {
    mtr_report();
    mtr_print_thick_line('#');
    mtr_report("# $opt_comment");
    mtr_print_thick_line('#');
  }

  if ( @opt_experimentals )
  {
    # $^O on Windows considered not generic enough
    my $plat= (IS_WINDOWS) ? 'windows' : $^O;

    # read the list of experimental test cases from the files specified on
    # the command line
    $experimental_test_cases = [];
    foreach my $exp_file (@opt_experimentals)
    {
      open(FILE, "<", $exp_file)
	or mtr_error("Can't read experimental file: $exp_file");
      mtr_report("Using experimental file: $exp_file");
      while(<FILE>) {
	chomp;
	# remove comments (# foo) at the beginning of the line, or after a 
	# blank at the end of the line
	s/(\s+|^)#.*$//;
	# If @ platform specifier given, use this entry only if it contains
	# @<platform> or @!<xxx> where xxx != platform
	if (/\@.*/)
	{
	  next if (/\@!$plat/);
	  next unless (/\@$plat/ or /\@!/);
	  # Then remove @ and everything after it
	  s/\@.*$//;
	}
	# remove whitespace
	s/^\s+//;
	s/\s+$//;
	# if nothing left, don't need to remember this line
	if ( $_ eq "" ) {
	  next;
	}
	# remember what is left as the name of another test case that should be
	# treated as experimental
	print " - $_\n";
	push @$experimental_test_cases, $_;
      }
      close FILE;
    }
  }

  foreach my $arg ( @ARGV )
  {
    if ( $arg =~ /^--skip-/ )
    {
      push(@opt_extra_mysqld_opt, $arg);
    }
    elsif ( $arg =~ /^--$/ )
    {
      # It is an effect of setting 'pass_through' in option processing
      # that the lone '--' separating options from arguments survives,
      # simply ignore it.
    }
    elsif ( $arg =~ /^-/ )
    {
      usage("Invalid option \"$arg\"");
    }
    else
    {
      push(@opt_cases, $arg);
    }
  }

  if ( @opt_cases )
  {
    # Run big tests if explicitly specified on command line
    $opt_big_test= 1;
  }

  # --------------------------------------------------------------------------
  # Find out type of logging that are being used
  # --------------------------------------------------------------------------
  foreach my $arg ( @opt_extra_mysqld_opt )
  {
    if ( $arg =~ /binlog[-_]format=(\S+)/ )
    {
      # Save this for collect phase
      collect_option('binlog-format', $1);
      mtr_report("Using binlog format '$1'");
    }
  }


  # --------------------------------------------------------------------------
  # Find out default storage engine being used(if any)
  # --------------------------------------------------------------------------
  foreach my $arg ( @opt_extra_mysqld_opt )
  {
    if ( $arg =~ /default-storage-engine=(\S+)/ )
    {
      # Save this for collect phase
      collect_option('default-storage-engine', $1);
      mtr_report("Using default engine '$1'")
    }
  }

  if (IS_WINDOWS and defined $opt_mem) {
    mtr_report("--mem not supported on Windows, ignored");
    $opt_mem= undef;
  }

  if ($opt_port_base ne "auto")
  {
    if (my $rem= $opt_port_base % 10)
    {
      mtr_warning ("Port base $opt_port_base rounded down to multiple of 10");
      $opt_port_base-= $rem;
    }
    $opt_build_thread= $opt_port_base / 10 - 1000;
  }

  # --------------------------------------------------------------------------
  # Check if we should speed up tests by trying to run on tmpfs
  # --------------------------------------------------------------------------
  if ( defined $opt_mem)
  {
    mtr_error("Can't use --mem and --vardir at the same time ")
      if $opt_vardir;
    mtr_error("Can't use --mem and --tmpdir at the same time ")
      if $opt_tmpdir;

    # Search through list of locations that are known
    # to be "fast disks" to find a suitable location
    my @tmpfs_locations= ("/run/shm", "/dev/shm", "/tmp");

    # Use $ENV{'MTR_MEM'} as first location to look (if defined)
    unshift(@tmpfs_locations, $ENV{'MTR_MEM'}) if defined $ENV{'MTR_MEM'};

    foreach my $fs (@tmpfs_locations)
    {
      if ( -d $fs && ! -l $fs  && -w $fs )
      {
	my $template= "var_${opt_build_thread}_XXXX";
	$opt_mem= tempdir( $template, DIR => $fs, CLEANUP => 0);
	last;
      }
    }
  }

  # --------------------------------------------------------------------------
  # Set the "var/" directory, the base for everything else
  # --------------------------------------------------------------------------
  my $vardir_location= (defined $ENV{MTR_BINDIR} 
                          ? "$ENV{MTR_BINDIR}/mysql-test" 
                          : $glob_mysql_test_dir);
  $vardir_location= realpath $vardir_location;
  $default_vardir= "$vardir_location/var";

  if ( ! $opt_vardir )
  {
    $opt_vardir= $default_vardir;
  }

  # We make the path absolute, as the server will do a chdir() before usage
  unless ( $opt_vardir =~ m,^/, or
           (IS_WINDOWS and $opt_vardir =~ m,^[a-z]:[/\\],i) )
  {
    # Make absolute path, relative test dir
    $opt_vardir= "$glob_mysql_test_dir/$opt_vardir";
  }

  set_vardir($opt_vardir);

  # --------------------------------------------------------------------------
  # Set the "tmp" directory
  # --------------------------------------------------------------------------
  if ( ! $opt_tmpdir )
  {
    $opt_tmpdir=       "$opt_vardir/tmp" unless $opt_tmpdir;

    if (check_socket_path_length("$opt_tmpdir/mysql_testsocket.sock"))
    {
      mtr_report("Too long tmpdir path '$opt_tmpdir'",
		 " creating a shorter one...");

      # Create temporary directory in standard location for temporary files
      $opt_tmpdir= tempdir( TMPDIR => 1, CLEANUP => 0 );
      mtr_report(" - using tmpdir: '$opt_tmpdir'\n");

      # Remember pid that created dir so it's removed by correct process
      $opt_tmpdir_pid= $$;
    }
  }
  $opt_tmpdir =~ s,/+$,,;       # Remove ending slash if any

  # --------------------------------------------------------------------------
  # fast option
  # --------------------------------------------------------------------------
  if ($opt_fast){
    $opt_shutdown_timeout= 0; # Kill processes instead of nice shutdown
  }

  # --------------------------------------------------------------------------
  # Check parallel value
  # --------------------------------------------------------------------------
  if ($opt_parallel ne "auto" && $opt_parallel < 1)
  {
    mtr_error("0 or negative parallel value makes no sense, use 'auto' or positive number");
  }

  # --------------------------------------------------------------------------
  # Record flag
  # --------------------------------------------------------------------------
  if ( $opt_record and ! @opt_cases )
  {
    mtr_error("Will not run in record mode without a specific test case");
  }

  if ( $opt_record ) {
    # Use only one worker with --record
    $opt_parallel= 1;
  }

  # --------------------------------------------------------------------------
  # Embedded server flag
  # --------------------------------------------------------------------------
  if ( $opt_embedded_server )
  {
    $opt_skip_ssl= 1;              # Turn off use of SSL

    # Turn off use of bin log
    push(@opt_extra_mysqld_opt, "--skip-log-bin");

    if ( using_extern() )
    {
      mtr_error("Can't use --extern with --embedded-server");
    }
  }

  # --------------------------------------------------------------------------
  # Big test and staging_run flags
  # --------------------------------------------------------------------------
   if ( $opt_big_test )
   {
     $ENV{'BIG_TEST'}= 1;
   }
  $ENV{'STAGING_RUN'}= $opt_staging_run;

  # --------------------------------------------------------------------------
  # Gcov flag
  # --------------------------------------------------------------------------
  if ( ($opt_gcov or $opt_gprof) and ! $source_dist )
  {
    mtr_error("Coverage test needs the source - please use source dist");
  }

  $ENV{ASAN_OPTIONS}= "abort_on_error=1:" . ($ENV{ASAN_OPTIONS} || '');
  $ENV{ASAN_OPTIONS}= "suppressions=${glob_mysql_test_dir}/asan.supp:" .
    $ENV{ASAN_OPTIONS}
    if -f "$glob_mysql_test_dir/asan.supp" and not IS_WINDOWS;
  # The following can be useful when a test fails without any asan report
  # on stderr like with openssl_1.test
  # $ENV{ASAN_OPTIONS}= "log_path=${opt_vardir}/log/asan:" . $ENV{ASAN_OPTIONS};

  # Add leak suppressions
  $ENV{LSAN_OPTIONS}= "suppressions=${glob_mysql_test_dir}/lsan.supp:print_suppressions=0"
    if -f "$glob_mysql_test_dir/lsan.supp" and not IS_WINDOWS;

  mtr_verbose("ASAN_OPTIONS=$ENV{ASAN_OPTIONS}");

  # --------------------------------------------------------------------------
  # Modified behavior with --start options
  # --------------------------------------------------------------------------
  if ($opt_start or $opt_start_dirty or $opt_start_exit) {
    collect_option ('quick-collect', 1);
    $start_only= 1;
  }
  if ($opt_debug)
  {
    $opt_testcase_timeout= 7 * 24 * 60;
    $opt_suite_timeout= 7 * 24 * 60;
    $opt_retry= 1;
    $opt_retry_failure= 1;
  }

  # --------------------------------------------------------------------------
  # Check use of user-args
  # --------------------------------------------------------------------------

  if ($opt_user_args) {
    mtr_error("--user-args only valid with --start options")
      unless $start_only;
    mtr_error("--user-args cannot be combined with named suites or tests")
      if $opt_suites || @opt_cases;
  }

  # --------------------------------------------------------------------------
  # Check use of wait-all
  # --------------------------------------------------------------------------

  if ($opt_wait_all && ! $start_only)
  {
    mtr_error("--wait-all can only be used with --start options");
  }

  # --------------------------------------------------------------------------
  # Gather stress-test options and modify behavior
  # --------------------------------------------------------------------------

  if ($opt_stress)
  {
    $opt_stress=~ s/,/ /g;
    $opt_user_args= 1;
    mtr_error("--stress cannot be combined with named ordinary suites or tests")
      if $opt_suites || @opt_cases;
    $opt_suites="stress";
    @opt_cases= ("wrapper");
    $ENV{MST_OPTIONS}= $opt_stress;
  }

  # --------------------------------------------------------------------------
  # Check timeout arguments
  # --------------------------------------------------------------------------

  mtr_error("Invalid value '$opt_testcase_timeout' supplied ".
	    "for option --testcase-timeout")
    if ($opt_testcase_timeout <= 0);
  mtr_error("Invalid value '$opt_suite_timeout' supplied ".
	    "for option --testsuite-timeout")
    if ($opt_suite_timeout <= 0);

  if ($opt_debug_common)
  {
    $opt_debug= 1;
    $debug_d= "d,query,info,error,enter,exit";
  }

  My::CoreDump::pre_setup();
}


#
# To make it easier for different devs to work on the same host,
# an environment variable can be used to control all ports. A small
# number is to be used, 0 - 16 or similar.
#
# Note the MASTER_MYPORT has to be set the same in all 4.x and 5.x
# versions of this script, else a 4.0 test run might conflict with a
# 5.1 test run, even if different MTR_BUILD_THREAD is used. This means
# all port numbers might not be used in this version of the script.
#
# Also note the limitation of ports we are allowed to hand out. This
# differs between operating systems and configuration, see
# http://www.ncftp.com/ncftpd/doc/misc/ephemeral_ports.html
# But a fairly safe range seems to be 5001 - 32767
#
sub set_build_thread_ports($) {
  my $thread= shift || 0;

  if ( lc($opt_build_thread) eq 'auto' ) {
    my $found_free = 0;
    $build_thread = 300;	# Start attempts from here
    my $build_thread_upper = $build_thread + ($opt_parallel > 1500
                                              ? 3000
                                              : 2 * $opt_parallel) + 300;
    while (! $found_free)
    {
      $build_thread= mtr_get_unique_id($build_thread, $build_thread_upper);
      if ( !defined $build_thread ) {
        mtr_error("Could not get a unique build thread id");
      }
      $found_free= check_ports_free($build_thread);
      # If not free, release and try from next number
      if (! $found_free) {
        mtr_release_unique_id();
        $build_thread++;
      }
    }
  }
  else
  {
    $build_thread = $opt_build_thread + $thread - 1;
    if (! check_ports_free($build_thread)) {
      # Some port was not free(which one has already been printed)
      mtr_error("Some port(s) was not free")
    }
  }
  $ENV{MTR_BUILD_THREAD}= $build_thread;

  # Calculate baseport
  $baseport= $build_thread * $opt_port_group_size + 10000;
  if ( $baseport < 5001 or $baseport + $opt_port_group_size >= 32767 )
  {
    mtr_error("MTR_BUILD_THREAD number results in a port",
              "outside 5001 - 32767",
              "($baseport - $baseport + $opt_port_group_size)");
  }

  mtr_report("Using MTR_BUILD_THREAD $build_thread,",
	     "with reserved ports $baseport..".($baseport+($opt_port_group_size-1)));

}


sub collect_mysqld_features {
  #
  # Execute "mysqld --no-defaults --help --verbose" to get a
  # list of all features and settings
  #
  # --no-defaults and --skip-grant-tables are to avoid loading
  # system-wide configs and plugins
  #
  # --datadir must exist, mysqld will chdir into it
  #
  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--datadir=.");
  mtr_add_arg($args, "--basedir=%s", $basedir);
  mtr_add_arg($args, "--lc-messages-dir=%s", $path_language);
  mtr_add_arg($args, "--skip-grant-tables");
  mtr_add_arg($args, "--log-warnings=0");
  mtr_add_arg($args, "--log-slow-admin-statements=0");
  mtr_add_arg($args, "--log-queries-not-using-indexes=0");
  mtr_add_arg($args, "--log-slow-slave-statements=0");
  mtr_add_arg($args, "--verbose");
  mtr_add_arg($args, "--help");

  my $exe_mysqld= find_mysqld($bindir);
  my $cmd= join(" ", $exe_mysqld, @$args);

  mtr_verbose("cmd: $cmd");

  my $list= `$cmd`;

  # to simplify the parsing, we'll merge all nicely formatted --help texts
  $list =~ s/\n {22}(\S)/ $1/g;

  my @list= split '\R', $list;

  $mysql_version_id= 0;
  my $exe= basename($exe_mysqld);
  while (defined(my $line = shift @list)){
     if ($line =~ /\W\Q$exe\E\s+Ver\s(\d+)\.(\d+)\.(\d+)(\S*)/ ) {
      $mysql_version_id= $1*10000 + $2*100 + $3;
      mtr_report("MariaDB Version $1.$2.$3$4");
      last;
    }
  }
  mtr_error("Could not find version of MariaDB")
     unless $mysql_version_id > 0;

  for (@list)
  {
    # first part of the help - command-line options.
    if (/Copyright/ .. /^-{30,}/) {
      # here we want to detect all not mandatory plugins
      # they are listed in the --help output as
      #  --archive[=name]
      # Enable or disable ARCHIVE plugin. Possible values are ON, OFF,
      # FORCE (don't start if the plugin fails to load),
      # FORCE_PLUS_PERMANENT (like FORCE, but the plugin can not be uninstalled).
      # For Innodb I_S plugins that are referenced in sys schema
      # do not make them optional, to prevent diffs in tests.
      push @optional_plugins, $1
        if /^  --([-a-z0-9]+)\[=name\] +Enable or disable \w+ plugin. One of: ON, OFF, FORCE/
           and $1 ne "innodb-metrics"
           and $1 ne "innodb-buffer-page"
           and $1 ne "innodb-lock-waits"
           and $1 ne "innodb-locks"
           and $1 ne "innodb-trx"
           and $1 ne "gssapi";
      next;
    }

    last if /^\r?$/; # then goes a list of variables, it ends with an empty line

    # Put a variable into hash
    /^([\S]+)[ \t]+(.*?)\r?$/ or die "Could not parse mysqld --help: $_\n";
    $mysqld_variables{$1}= $2;
  }
  mtr_error("Could not find variabes list") unless %mysqld_variables;
}


sub collect_mysqld_features_from_running_server ()
{
  my $mysql= mtr_exe_exists("$path_client_bindir/mariadb");

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--user=%s", $opt_user);

  while (my ($option, $value)= each( %opts_extern )) {
    mtr_add_arg($args, "--$option=$value");
  }

  mtr_add_arg($args, "--silent"); # Tab separated output
  mtr_add_arg($args, "-e \"use mysql; SHOW VARIABLES\"");
  my $cmd= "$mysql " . join(' ', @$args);
  mtr_verbose("cmd: $cmd");

  my $list = `$cmd` or
    mtr_error("Could not connect to extern server using command: '$cmd'");
  foreach my $line (split('\R', $list ))
  {
    # Put variables into hash
    if ( $line =~ /^([\S]+)[ \t]+(.*?)\r?$/ )
    {
      my $name= $1;
      my $value=$2;
      $name =~ s/_/-/g;
      # print "$name=\"$value\"\n";
      $mysqld_variables{$name}= $value;
    }
  }

  # Parse version
  my $version_str= $mysqld_variables{'version'};
  if ( $version_str =~ /^([0-9]*)\.([0-9]*)\.([0-9]*)([^\s]*)/ )
  {
    #print "Major: $1 Minor: $2 Build: $3\n";
    $mysql_version_id= $1*10000 + $2*100 + $3;
    #print "mysql_version_id: $mysql_version_id\n";
    mtr_report("MariaDB Version $1.$2.$3");
    $mysql_version_extra= $4;
  }
  mtr_error("Could not find version of MariaDBL") unless $mysql_version_id;
}

sub find_mysqld {

  my ($mysqld_basedir)= $ENV{MTR_BINDIR_FORCED} || $ENV{MTR_BINDIR} || @_;

  my @mysqld_names= ("mariadbd", "mysqld", "mysqld-max-nt", "mysqld-max",
		     "mysqld-nt");

  if ( $opt_debug_server ){
    # Put mysqld-debug first in the list of binaries to look for
    mtr_verbose("Adding mysqld-debug first in list of binaries to look for");
    unshift(@mysqld_names, "mysqld-debug");
  }

  return my_find_bin($mysqld_basedir,
		     ["sql", "libexec", "sbin", "bin"],
		     [@mysqld_names]);
}


sub executable_setup () {

  $exe_patch='patch' if `patch -v`;

  # Look for the client binaries
  $exe_mysqladmin=     mtr_exe_exists("$path_client_bindir/mariadb-admin");
  $exe_mysql=          mtr_exe_exists("$path_client_bindir/mariadb");
  $exe_mysql_plugin=   mtr_exe_exists("$path_client_bindir/mariadb-plugin");
  $exe_mariadb_conv=   mtr_exe_exists("$path_client_bindir/mariadb-conv");

  $exe_mysql_embedded= mtr_exe_maybe_exists("$bindir/libmysqld/examples/mysql_embedded");

  # Look for mysqltest executable
  if ( $opt_embedded_server )
  {
    $exe_mysqltest=
      mtr_exe_exists("$bindir/libmysqld/examples$multiconfig/mysqltest_embedded",
                     "$path_client_bindir/mysqltest_embedded");
  }
  else
  {
    if ( defined $ENV{'MYSQL_TEST'} )
    {
      $exe_mysqltest=$ENV{'MYSQL_TEST'};
      print "===========================================================\n";
      print "WARNING:The mysqltest binary is fetched from $exe_mysqltest\n";
      print "===========================================================\n";
    }
    else
    {
      $exe_mysqltest= mtr_exe_exists("$path_client_bindir/mariadb-test");
    }
  }

}


sub client_debug_arg($$) {
  my ($args, $client_name)= @_;

  # Workaround for Bug #50627: drop any debug opt
  return if $client_name =~ /^mariadb-binlog/;

  if ( $opt_debug ) {
    mtr_add_arg($args,
		"--loose-debug=d,info,warning,warnings:t:A,%s/log/%s.trace",
		$path_vardir_trace, $client_name)
  }
}


sub client_arguments ($;$) {
 my $client_name= shift;
  my $group_suffix= shift;
  my $client_exe= mtr_exe_exists("$path_client_bindir/$client_name");

  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  if (defined($group_suffix)) {
    mtr_add_arg($args, "--defaults-group-suffix=%s", $group_suffix);
    client_debug_arg($args, "$client_name-$group_suffix");
  }
  else
  {
    client_debug_arg($args, $client_name);
  }
  return mtr_args2str($client_exe, @$args);
}


sub mysqlbinlog_arguments () {
  my $exe= mtr_exe_exists("$path_client_bindir/mariadb-binlog");

  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--local-load=%s", $opt_tmpdir);
  client_debug_arg($args, "mysqlbinlog");
  return mtr_args2str($exe, @$args);
}


sub mysqlslap_arguments () {
  my $exe= mtr_exe_maybe_exists("$path_client_bindir/mariadb-slap");
  if ( $exe eq "" ) {
    # mysqlap was not found

    if (defined $mysql_version_id and $mysql_version_id >= 50100 ) {
      mtr_error("Could not find the mariadb-slap binary");
    }
    return ""; # Don't care about mariadb-slap
  }

  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  client_debug_arg($args, "mysqlslap");
  return mtr_args2str($exe, @$args);
}


sub mysqldump_arguments ($) {
  my($group_suffix) = @_;
  my $exe= mtr_exe_exists("$path_client_bindir/mariadb-dump");

  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--defaults-group-suffix=%s", $group_suffix);
  client_debug_arg($args, "mysqldump-$group_suffix");
  return mtr_args2str($exe, @$args);
}


sub mysql_client_test_arguments(){
  my $exe;
  # mysql_client_test executable may _not_ exist
  if ( $opt_embedded_server ) {
    $exe= mtr_exe_maybe_exists(
            "$bindir/libmysqld/examples$multiconfig/mysql_client_test_embedded",
		"$bindir/bin/mysql_client_test_embedded");
  } else {
    $exe= mtr_exe_maybe_exists("$bindir/tests$multiconfig/mysql_client_test",
			       "$bindir/bin/mysql_client_test");
  }

  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--testcase");
  mtr_add_arg($args, "--vardir=$opt_vardir");
  client_debug_arg($args,"mysql_client_test");
  my $ret=mtr_args2str($exe, @$args);
  return $ret;
}

sub tool_arguments ($$) {
  my($sedir, $tool_name) = @_;
  my $exe= my_find_bin($bindir,
		       [$sedir, "bin"],
		       $tool_name);

  my $args;
  mtr_init_args(\$args);
  client_debug_arg($args, $tool_name);
  return mtr_args2str($exe, @$args);
}

# This is not used to actually start a mysqld server, just to allow test
# scripts to run the mysqld binary to test invalid server startup options.
sub mysqld_client_arguments () {
  my $default_mysqld= default_mysqld();
  my $exe = find_mysqld($bindir);
  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--basedir=%s", $basedir);
  mtr_add_arg($args, "--character-sets-dir=%s", $default_mysqld->value("character-sets-dir"));
  mtr_add_arg($args, "--language=%s", $default_mysqld->value("language"));
  return mtr_args2str($exe, @$args);
}


sub have_maria_support () {
  my $maria_var= $mysqld_variables{'aria-recover-options'};
  return defined $maria_var;
}


sub environment_setup {

  umask(022);

  $ENV{'USE_RUNNING_SERVER'}= using_extern();

  my @ld_library_paths;

  if ($path_client_libdir)
  {
    # Use the --client-libdir passed on commandline
    push(@ld_library_paths, "$path_client_libdir");
  }
  else
  {
    # Setup LD_LIBRARY_PATH so the libraries from this distro/clone
    # are used in favor of the system installed ones
    if ( $source_dist )
    {
      push(@ld_library_paths, "$basedir/libmysql/.libs/",
	   "$basedir/libmysql_r/.libs/",
	   "$basedir/zlib/.libs/");
      if ($^O eq "darwin")
      {
        # it is MAC OS and we have to add dynamic libraries paths
        push @ld_library_paths, grep {<$_/*.dylib>} 
          (<$bindir/storage/*/.libs/>,<$bindir/plugin/*/.libs/>,
          <$bindir/plugin/*/*/.libs/>,<$bindir/storage/*/*/.libs>);
      }
    }
    else
    {
      push(@ld_library_paths, "$basedir/lib", "$basedir/lib/mysql");
    }
  }

  $ENV{'LD_LIBRARY_PATH'}= join(":", @ld_library_paths,
				$ENV{'LD_LIBRARY_PATH'} ?
				split(':', $ENV{'LD_LIBRARY_PATH'}) : ());

  My::Debugger::pre_setup();

  mtr_debug("LD_LIBRARY_PATH: $ENV{'LD_LIBRARY_PATH'}");

  $ENV{'DYLD_LIBRARY_PATH'}= join(":", @ld_library_paths,
				  $ENV{'DYLD_LIBRARY_PATH'} ?
				  split(':', $ENV{'DYLD_LIBRARY_PATH'}) : ());
  mtr_debug("DYLD_LIBRARY_PATH: $ENV{'DYLD_LIBRARY_PATH'}");

  # The environment variable used for shared libs on AIX
  $ENV{'SHLIB_PATH'}= join(":", @ld_library_paths,
                           $ENV{'SHLIB_PATH'} ?
                           split(':', $ENV{'SHLIB_PATH'}) : ());
  mtr_debug("SHLIB_PATH: $ENV{'SHLIB_PATH'}");

  # The environment variable used for shared libs on hp-ux
  $ENV{'LIBPATH'}= join(":", @ld_library_paths,
                        $ENV{'LIBPATH'} ?
                        split(':', $ENV{'LIBPATH'}) : ());
  mtr_debug("LIBPATH: $ENV{'LIBPATH'}");

  $ENV{'UMASK'}=              "0660"; # The octal *string*
  $ENV{'UMASK_DIR'}=          "0770"; # The octal *string*

  #
  # MariaDB tests can produce output in various character sets
  # (especially, ctype_xxx.test). To avoid confusing Perl
  # with output which is incompatible with the current locale
  # settings, we reset the current values of LC_ALL and LC_CTYPE to "C".
  # For details, please see
  # Bug#27636 tests fails if LC_* variables set to *_*.UTF-8
  #
  $ENV{'LC_ALL'}=             "C";
  $ENV{'LC_CTYPE'}=           "C";
  $ENV{'LC_COLLATE'}=         "C";

  $ENV{'OPENSSL_CONF'}= $mysqld_variables{'version-ssl-library'} gt 'OpenSSL 1.1.1'
                       ? "$glob_mysql_test_dir/lib/openssl.cnf" : '/dev/null';

  $ENV{'MYSQL_TEST_DIR'}=     $glob_mysql_test_dir;
  $ENV{'DEFAULT_MASTER_PORT'}= $mysqld_variables{'port'};
  $ENV{'MYSQL_TMP_DIR'}=      $opt_tmpdir;
  $ENV{'MYSQLTEST_VARDIR'}=   $opt_vardir;
  $ENV{'MYSQLTEST_REAL_VARDIR'}= realpath $opt_vardir;
  $ENV{'MYSQL_BINDIR'}=       $bindir;
  $ENV{'MYSQL_SHAREDIR'}=     $path_language;
  $ENV{'MYSQL_CHARSETSDIR'}=  $path_charsetsdir;
  
  if (IS_WINDOWS)
  {
    $ENV{'SECURE_LOAD_PATH'}= $glob_mysql_test_dir."\\std_data";
  }
  else
  {
    $ENV{'SECURE_LOAD_PATH'}= $glob_mysql_test_dir."/std_data";
  }

  #
  # Some stupid^H^H^H^H^H^Hignorant network providers set up "wildcard DNS"
  # servers that return some given web server address for any lookup of a
  # non-existent host name. This confuses test cases that want to test the
  # behaviour when connecting to a non-existing host, so we need to be able
  # to disable those tests when DNS is broken.
  #
  $ENV{HAVE_BROKEN_DNS}= defined(gethostbyname('invalid_hostname'));

  # ----------------------------------------------------
  # mysql clients
  # ----------------------------------------------------
  $ENV{'MYSQL_CHECK'}=              client_arguments("mariadb-check");
  $ENV{'MYSQL_DUMP'}=               mysqldump_arguments(".1");
  $ENV{'MYSQL_DUMP_SLAVE'}=         mysqldump_arguments(".2");
  $ENV{'MYSQL_SLAP'}=               mysqlslap_arguments();
  $ENV{'MYSQL_IMPORT'}=             client_arguments("mariadb-import");
  $ENV{'MYSQL_SHOW'}=               client_arguments("mariadb-show");
  $ENV{'MYSQL_BINLOG'}=             mysqlbinlog_arguments();
  $ENV{'MYSQL'}=                    client_arguments("mariadb");
  $ENV{'MYSQL_SLAVE'}=              client_arguments("mariadb", ".2");
  $ENV{'MYSQL_UPGRADE'}=            client_arguments("mariadb-upgrade");
  $ENV{'MYSQLADMIN'}=               client_arguments("mariadb-admin");
  $ENV{'MYSQL_CLIENT_TEST'}=        mysql_client_test_arguments();
  $ENV{'EXE_MYSQL'}=                $exe_mysql;
  $ENV{'MYSQL_PLUGIN'}=             $exe_mysql_plugin;
  $ENV{'MYSQL_EMBEDDED'}=           $exe_mysql_embedded;
  $ENV{'MARIADB_CONV'}=             "$exe_mariadb_conv --character-sets-dir=$path_charsetsdir";
  if(IS_WINDOWS)
  {
     $ENV{'MYSQL_INSTALL_DB_EXE'}=  mtr_exe_exists("$bindir/sql$multiconfig/mariadb-install-db",
       "$bindir/bin/mariadb-install-db");
     $ENV{'MARIADB_UPGRADE_SERVICE_EXE'}= mtr_exe_exists("$bindir/sql$multiconfig/mariadb-upgrade-service",
      "$bindir/bin/mariadb-upgrade-service");
     $ENV{'MARIADB_UPGRADE_EXE'}= mtr_exe_exists("$path_client_bindir/mariadb-upgrade");
  }

  my $client_config_exe=
    mtr_exe_maybe_exists(
        "$bindir/libmariadb/mariadb_config$multiconfig/mariadb_config",
               "$bindir/bin/mariadb_config");
  if ($client_config_exe)
  {
    my $tls_info= `$client_config_exe --tlsinfo`;
    ($ENV{CLIENT_TLS_LIBRARY},$ENV{CLIENT_TLS_LIBRARY_VERSION})=
      split(/ /, $tls_info, 2);
  }
  my $exe_mysqld= find_mysqld($basedir);
  $ENV{'MYSQLD'}= $exe_mysqld;
  my $extra_opts= join (" ", @opt_extra_mysqld_opt);
  $ENV{'MYSQLD_CMD'}= "$exe_mysqld --defaults-group-suffix=.1 ".
    "--defaults-file=$path_config_file $extra_opts";

  # ----------------------------------------------------
  # bug25714 executable may _not_ exist in
  # some versions, test using it should be skipped
  # ----------------------------------------------------
  my $exe_bug25714=
      mtr_exe_maybe_exists("$bindir/tests$multiconfig/bug25714");
  $ENV{'MYSQL_BUG25714'}=  native_path($exe_bug25714);

  # ----------------------------------------------------
  # mysql_fix_privilege_tables.sql
  # ----------------------------------------------------
  my $file_mysql_fix_privilege_tables=
    mtr_file_exists("$bindir/scripts/mysql_fix_privilege_tables.sql",
		    "$bindir/share/mysql_fix_privilege_tables.sql",
		    "$bindir/share/mariadb/mysql_fix_privilege_tables.sql",
		    "$bindir/share/mysql/mysql_fix_privilege_tables.sql");
  $ENV{'MYSQL_FIX_PRIVILEGE_TABLES'}=  $file_mysql_fix_privilege_tables;

  # ----------------------------------------------------
  # my_print_defaults
  # ----------------------------------------------------
  my $exe_my_print_defaults=
    mtr_exe_exists("$bindir/extra$multiconfig/my_print_defaults",
		   "$path_client_bindir/my_print_defaults");
  $ENV{'MYSQL_MY_PRINT_DEFAULTS'}= native_path($exe_my_print_defaults);

  # ----------------------------------------------------
  # myisam tools
  # ----------------------------------------------------
  $ENV{'MYISAMLOG'}= tool_arguments("storage/myisam", "myisamlog", );
  $ENV{'MYISAMCHK'}= tool_arguments("storage/myisam", "myisamchk");
  $ENV{'MYISAMPACK'}= tool_arguments("storage/myisam", "myisampack");
  $ENV{'MYISAM_FTDUMP'}= tool_arguments("storage/myisam", "myisam_ftdump");

  # ----------------------------------------------------
  # aria tools
  # ----------------------------------------------------
  if (have_maria_support())
  {
    $ENV{'MARIA_CHK'}= tool_arguments("storage/maria", "aria_chk");
    $ENV{'MARIA_PACK'}= tool_arguments("storage/maria", "aria_pack");
  }

  # ----------------------------------------------------
  # mysqlhotcopy
  # ----------------------------------------------------
  my $mysqlhotcopy=
    mtr_pl_maybe_exists("$bindir/scripts/mysqlhotcopy") ||
    mtr_pl_maybe_exists("$path_client_bindir/mysqlhotcopy");
  if ($mysqlhotcopy)
  {
    $ENV{'MYSQLHOTCOPY'}= $mysqlhotcopy;
  }

  # ----------------------------------------------------
  # perror
  # ----------------------------------------------------
  my $exe_perror= mtr_exe_exists("$bindir/extra$multiconfig/perror",
				 "$path_client_bindir/perror");
  $ENV{'MY_PERROR'}= native_path($exe_perror);

  # ----------------------------------------------------
  # mysql_tzinfo_to_sql
  # ----------------------------------------------------
  my $exe_mysql_tzinfo_to_sql= mtr_exe_exists("$basedir/sql$multiconfig/mariadb-tzinfo-to-sql",
                                 "$path_client_bindir/mariadb-tzinfo-to-sql",
                                 "$bindir/sql$multiconfig/mariadb-tzinfo-to-sql");
  $ENV{'MYSQL_TZINFO_TO_SQL'}= native_path($exe_mysql_tzinfo_to_sql);

  # ----------------------------------------------------
  # replace
  # ----------------------------------------------------
  my $exe_replace= mtr_exe_exists(vs_config_dirs('extra', 'replace'),
                                 "$basedir/extra/replace",
                                 "$bindir/extra$multiconfig/replace",
                                 "$path_client_bindir/replace");
  $ENV{'REPLACE'}= native_path($exe_replace);

  # ----------------------------------------------------
  # innochecksum
  # ----------------------------------------------------
  my $exe_innochecksum=
    mtr_exe_maybe_exists("$bindir/extra$multiconfig/innochecksum",
		         "$path_client_bindir/innochecksum");
  $ENV{'INNOCHECKSUM'}= native_path($exe_innochecksum) if $exe_innochecksum;

  # ----------------------------------------------------
  # mariabackup
  # ----------------------------------------------------
  my $exe_mariabackup= mtr_exe_maybe_exists(
      "$bindir/extra/mariabackup$multiconfig/mariabackup",
      "$path_client_bindir/mariabackup");

  $ENV{XTRABACKUP}= native_path($exe_mariabackup) if $exe_mariabackup;

  my $exe_xbstream= mtr_exe_maybe_exists(
        "$bindir/extra/mariabackup/$multiconfig/mbstream",
        "$path_client_bindir/mbstream");
  $ENV{XBSTREAM}= native_path($exe_xbstream) if $exe_xbstream;

  $ENV{INNOBACKUPEX}= "$exe_mariabackup --innobackupex";

  # Add dir of this perl to aid mysqltest in finding perl
  my $perldir= dirname($^X);
  my $pathsep= ":";
  $pathsep= ";" if IS_WINDOWS && ! IS_CYGWIN;
  $ENV{'PATH'}= "$ENV{'PATH'}".$pathsep.$perldir;
}


sub remove_vardir_subs() {
  foreach my $sdir ( glob("$opt_vardir/*") ) {
    mtr_verbose("Removing subdir $sdir");
    rmtree($sdir);
  }
}

#
# Remove var and any directories in var/ created by previous
# tests
#
sub remove_stale_vardir () {

  mtr_report("Removing old var directory...");

  # Safety!
  mtr_error("No, don't remove the vardir when running with --extern")
    if using_extern();

  mtr_verbose("opt_vardir: $opt_vardir");
  if ( $opt_vardir eq $default_vardir )
  {
    #
    # Running with "var" in mysql-test dir
    #
    if ( -l $opt_vardir)
    {
      # var is a symlink

      if ( $opt_mem )
      {
	# Remove the directory which the link points at
	mtr_verbose("Removing " . readlink($opt_vardir));
	rmtree(readlink($opt_vardir));

	# Remove the "var" symlink
	mtr_verbose("unlink($opt_vardir)");
	unlink($opt_vardir);
      }
      else
      {
	# Some users creates a soft link in mysql-test/var to another area
	# - allow it, but remove all files in it

	mtr_report(" - WARNING: Using the 'mysql-test/var' symlink");

	# Make sure the directory where it points exist
        if (! -d readlink($opt_vardir))
        {
          mtr_report("The destination for symlink $opt_vardir does not exist; Removing it and creating a new var directory");
          unlink($opt_vardir);
        }
	remove_vardir_subs();
      }
    }
    else
    {
      # Remove the entire "var" dir
      mtr_verbose("Removing $opt_vardir/");
      rmtree("$opt_vardir/");
    }

    if ( $opt_mem )
    {
      # A symlink from var/ to $opt_mem will be set up
      # remove the $opt_mem dir to assure the symlink
      # won't point at an old directory
      mtr_verbose("Removing $opt_mem");
      rmtree($opt_mem);
    }

  }
  else
  {
    #
    # Running with "var" in some other place
    #

    # Don't remove the var/ dir in mysql-test dir as it may be in
    # use by another mysql-test-run run with --vardir
    # mtr_verbose("Removing $default_vardir");
    # rmtree($default_vardir);

    # Remove the "var" dir
    mtr_verbose("Removing $opt_vardir/");
    rmtree("$opt_vardir/");
  }
  # Remove the "tmp" dir
  mtr_verbose("Removing $opt_tmpdir/");
  rmtree("$opt_tmpdir/");
}

sub set_plugin_var($) {
  local $_ = $_[0];
  s/\.\w+$//;
  $ENV{"\U${_}_SO"} = $_[0];
}

#
# Create var and the directories needed in var
#
sub setup_vardir() {
  mtr_report("Creating var directory '$opt_vardir'...");

  if ( $opt_vardir eq $default_vardir )
  {
    #
    # Running with "var" in mysql-test dir
    #
    if ( -l $opt_vardir )
    {
      #  it's a symlink

      # Make sure the directory where it points exist
      if (! -d readlink($opt_vardir))
      {
        mtr_report("The destination for symlink $opt_vardir does not exist; Removing it and creating a new var directory");
        unlink($opt_vardir);
      }
    }
    elsif ( $opt_mem )
    {
      # Runinng with "var" as a link to some "memory" location, normally tmpfs
      mtr_verbose("Creating $opt_mem");
      mkpath($opt_mem);

      mtr_report(" - symlinking 'var' to '$opt_mem'");
      symlink($opt_mem, $opt_vardir);
    }
  }

  if ( ! -d $opt_vardir )
  {
    mtr_verbose("Creating $opt_vardir");
    mkpath($opt_vardir);
  }

  # Ensure a proper error message if vardir couldn't be created
  unless ( -d $opt_vardir and -w $opt_vardir )
  {
    mtr_error("Writable 'var' directory is needed, use the " .
	      "'--vardir=<path>' option");
  }

  mkpath("$opt_vardir/log");
  mkpath("$opt_vardir/run");

  # Create var/tmp and tmp - they might be different
  mkpath("$opt_vardir/tmp");
  mkpath($opt_tmpdir) if ($opt_tmpdir ne "$opt_vardir/tmp");

  # On some operating systems, there is a limit to the length of a
  # UNIX domain socket's path far below PATH_MAX.
  # Don't allow that to happen
  if (check_socket_path_length("$opt_tmpdir/testsocket.sock")){
    mtr_error("Socket path '$opt_tmpdir' too long, it would be ",
	      "truncated and thus not possible to use for connection to ",
	      "MariaDB Server. Set a shorter with --tmpdir=<path> option");
  }

  # copy all files from std_data into var/std_data
  # and make them world readable
  copytree("$glob_mysql_test_dir/std_data", "$opt_vardir/std_data", "0022");

  unless($plugindir)
  {
    # create a plugin dir and copy or symlink plugins into it
    if ($source_dist)
    {
      $plugindir="$opt_vardir/plugins";
      # Source builds collect both client plugins and server plugins in the
      # same directory.
      $client_plugindir= $plugindir;
      mkpath($plugindir);
      if (IS_WINDOWS)
      {
        if (!$opt_embedded_server)
        {
          for (<$bindir/storage/*$multiconfig/*.dll>,
               <$bindir/plugin/*$multiconfig/*.dll>,
               <$bindir/libmariadb$multiconfig/*.dll>,
               <$bindir/sql$multiconfig/*.dll>)
          {
            my $pname=basename($_);
            copy rel2abs($_), "$plugindir/$pname";
            set_plugin_var($pname);
          }
        }
      }
      else
      {
        my $opt_use_copy= 1;
        if (symlink "$opt_vardir/run", "$plugindir/symlink_test")
        {
          $opt_use_copy= 0;
          unlink "$plugindir/symlink_test";
        }

        for (<$bindir/storage/*$multiconfig/*.so>,
             <$bindir/plugin/*$multiconfig/*.so>,
             <$bindir/libmariadb/plugins/*/*.so>,
             <$bindir/libmariadb/$multiconfig/*.so>,
             <$bindir/sql$multiconfig/*.so>)
        {
          my $pname=basename($_);
          if ($opt_use_copy)
          {
            copy rel2abs($_), "$plugindir/$pname";
          }
          else
          {
            symlink rel2abs($_), "$plugindir/$pname";
          }
          set_plugin_var($pname);
        }
      }
    }
    else
    {
      # hm, what paths work for debs and for rpms ?
      for (<$bindir/lib64/mysql/plugin/*.so>,
           <$bindir/lib/mysql/plugin/*.so>,
           <$bindir/lib64/mariadb/plugin/*.so>,
           <$bindir/lib/mariadb/plugin/*.so>,
           <$bindir/lib/plugin/*.so>,             # bintar
           <$bindir/lib/plugin/*.dll>)
      {
        my $pname= basename($_);
        set_plugin_var($pname);
        $plugindir= dirname($_) unless $plugindir;
      }

      # Note: client plugins can be installed separately from server plugins,
      #       as is the case for Debian packaging.
      for (<$bindir/lib/*/libmariadb3/plugin>)
      {
        $client_plugindir= $_ if <$_/*.so>;
      }
      $client_plugindir= $plugindir unless $client_plugindir;
    }
  }

  # Remove old log files
  foreach my $name (glob("r/*.progress r/*.log r/*.warnings"))
  {
    unlink($name);
  }
}


#
# Check if running as root
# i.e a file can be read regardless what mode we set it to
#
sub  check_running_as_root () {
  my $test_file= "$opt_vardir/test_running_as_root.txt";
  mtr_tofile($test_file, "MySQL");
  chmod(oct("0000"), $test_file);

  my $result="";
  if (open(FILE,"<",$test_file))
  {
    $result= join('', <FILE>);
    close FILE;
  }

  # Some filesystems( for example CIFS) allows reading a file
  # although mode was set to 0000, but in that case a stat on
  # the file will not return 0000
  my $file_mode= (stat($test_file))[2] & 07777;

  mtr_verbose("result: $result, file_mode: $file_mode");
  if ($result eq "MySQL" && $file_mode == 0)
  {
    mtr_warning("running this script as _root_ will cause some " .
                "tests to be skipped");
    $ENV{'MYSQL_TEST_ROOT'}= "1";
  }

  chmod(oct("0755"), $test_file);
  unlink($test_file);
}


sub check_ssl_support {
  if ($opt_skip_ssl)
  {
    mtr_report(" - skipping SSL");
    $opt_ssl_supported= 0;
    $opt_ssl= 0;
    return;
  }

  if ( ! $mysqld_variables{'ssl'} )
  {
    if ( $opt_ssl)
    {
      mtr_error("Couldn't find support for SSL");
      return;
    }
    mtr_report(" - skipping SSL, mysqld not compiled with SSL");
    $opt_ssl_supported= 0;
    $opt_ssl= 0;
    return;
  }
  mtr_report(" - SSL connections supported");
  $opt_ssl_supported= 1;
}

sub check_debug_support {
  if (defined $mysqld_variables{'debug-dbug'})
  {
    mtr_report(" - binaries are debug compiled");
  }
  elsif ($opt_debug_server)
  {
    mtr_error("Can't use --debug[-server], binary does not support it");
  }
}


#
# Helper function to find the correct value for the multiconfig
# if it was not set explicitly.
#
# the configuration with the most recent build dir in sql/ is selected.
#
# note: looking for all BuildLog.htm files everywhere in the tree with the
# help of File::Find would be possibly more precise, but it is also
# many times slower. Thus we are only looking at the server, client
# executables, and plugins - that is, something that can affect the test suite
#
sub fix_vs_config_dir () {
  return $multiconfig="/$multiconfig" if $multiconfig;

  my $modified = 1e30;
  $multiconfig="";


  for (<$bindir/sql/*/mysqld.exe>,
      <$bindir/sql/*/mysqld>
  ) { #/
    if (-M $_ < $modified)
    {
      $modified = -M _;
      $multiconfig = basename(dirname($_));
    }
  }

  mtr_report("VS config: $multiconfig");
  $multiconfig="/$multiconfig" if $multiconfig;
}


#
# Helper function to handle configuration-based subdirectories which Visual
# Studio uses for storing binaries.  If multiconfig is set, this returns
# a path based on that setting; if not, it returns paths for the default
# /release/ and /debug/ subdirectories.
#
# $exe can be undefined, if the directory itself will be used
#
sub vs_config_dirs ($$) {
  my ($path_part, $exe) = @_;

  $exe = "" if not defined $exe;

  # Don't look in these dirs when not on windows
  return () unless IS_WINDOWS;

  if ($multiconfig)
  {
    return ("$basedir/$path_part/$multiconfig/$exe");
  }

  return ("$basedir/$path_part/release/$exe",
          "$basedir/$path_part/relwithdebinfo/$exe",
          "$basedir/$path_part/debug/$exe");
}

sub mysql_server_start($) {
  my ($mysqld, $tinfo) = @_;

  if ( $mysqld->{proc} )
  {
    # Already started

    # Write start of testcase to log file
    mark_log($mysqld->value('log-error'), $tinfo);

    return;
  }

  my $datadir= $mysqld->value('datadir');
  if (not $opt_start_dirty)
  {

    my @options= ('log-bin', 'relay-log');
    foreach my $option_name ( @options )  {
      next unless $mysqld->option($option_name);

      my $file_name= $mysqld->value($option_name);
      next unless
        defined $file_name and
          -e $file_name;

      mtr_debug(" -removing '$file_name'");
      unlink($file_name) or die ("unable to remove file '$file_name'");
    }

    if (-d $datadir ) {
      mtr_verbose(" - removing '$datadir'");
      rmtree($datadir);
    }
  }

  my $mysqld_basedir= $mysqld->value('basedir');
  my $extra_opts= get_extra_opts($mysqld, $tinfo);

  if ( $basedir eq $mysqld_basedir )
  {
    if (! $opt_start_dirty)	# If dirty, keep possibly grown system db
    {
      # Some InnoDB options are incompatible with the default bootstrap.
      # If they are used, re-bootstrap
      my @rebootstrap_opts;
      @rebootstrap_opts = grep {/$rebootstrap_re/o} @$extra_opts if $extra_opts;
      if (@rebootstrap_opts)
      {
        mtr_verbose("Re-bootstrap with @rebootstrap_opts");
        mysql_install_db($mysqld, undef, \@rebootstrap_opts);
      }
      else {
        # Copy datadir from installed system db
        my $path= ($opt_parallel == 1) ? "$opt_vardir" : "$opt_vardir/..";
        my $install_db= "$path/install.db";
        copytree($install_db, $datadir) if -d $install_db;
        mtr_error("Failed to copy system db to '$datadir'") unless -d $datadir;
      }
    }
  }
  else
  {
    mysql_install_db($mysqld); # For versional testing

    mtr_error("Failed to install system db to '$datadir'")
      unless -d $datadir;

  }

  # Create the servers tmpdir
  my $tmpdir= $mysqld->value('tmpdir');
  mkpath($tmpdir) unless -d $tmpdir;

  # Write start of testcase to log file
  mark_log($mysqld->value('log-error'), $tinfo);

  # Run <tname>-master.sh
  if ($mysqld->option('#!run-master-sh') and
      defined $tinfo->{master_sh} and 
      run_system('/bin/sh ' . $tinfo->{master_sh}) )
  {
    $tinfo->{'comment'}= "Failed to execute '$tinfo->{master_sh}'";
    return 1;
  }

  # Run <tname>-slave.sh
  if ($mysqld->option('#!run-slave-sh') and
      defined $tinfo->{slave_sh} and
      run_system('/bin/sh ' . $tinfo->{slave_sh}))
  {
    $tinfo->{'comment'}= "Failed to execute '$tinfo->{slave_sh}'";
    return 1;
  }

  if (!$opt_embedded_server)
  {
    mysqld_start($mysqld, $extra_opts) or
      mtr_error("Failed to start mysqld ".$mysqld->name()." with command "
        . $ENV{MYSQLD_LAST_CMD});

    # Save this test case information, so next can examine it
    $mysqld->{'started_tinfo'}= $tinfo;
  }

  # If wsrep is on, we need to wait until the first
  # server starts and bootstraps the cluster before
  # starting other servers. The bootsrap server in the
  # configuration should always be the first which has
  # wsrep_on=ON
  if (wsrep_on($mysqld) && wsrep_is_bootstrap_server($mysqld))
  {
    mtr_verbose("Waiting for wsrep bootstrap server to start");
    if ($mysqld->{WAIT}->($mysqld))
    {
      return 1;
    }
  }
}

sub mysql_server_wait {
  my ($mysqld, $tinfo) = @_;
  my $expect_file= "$opt_vardir/tmp/".$mysqld->name().".expect";

  if (!sleep_until_file_created($mysqld->value('pid-file'), $expect_file,
                                $opt_start_timeout, $mysqld->{'proc'},
                                $warn_seconds))
  {
    $tinfo->{comment}= "Failed to start ".$mysqld->name() . "\n";
    return 1;
  }

  if (wsrep_on($mysqld))
  {
    mtr_verbose("Waiting for wsrep server " . $mysqld->name() . " to be ready");
    if (!wait_wsrep_ready($tinfo, $mysqld))
    {
      return 1;
    }
  }

  return 0;
}

sub create_config_file_for_extern {
  my %opts=
    (
     socket     => '/tmp/mysqld.sock',
     port       => 3306,
     user       => $opt_user,
     password   => '',
     @_
    );

  mtr_report("Creating my.cnf file for extern server...");
  my $F= IO::File->new($path_config_file, "w")
    or mtr_error("Can't write to $path_config_file: $!");

  print $F "[client]\n";
  while (my ($option, $value)= each( %opts )) {
    print $F "$option= $value\n";
    mtr_report(" $option= $value");
  }

  print $F <<EOF

# binlog reads from [client] and [mysqlbinlog]
[mysqlbinlog]
character-sets-dir= $path_charsetsdir
local-load= $opt_tmpdir

EOF
;

  $F= undef; # Close file
}


#
# Kill processes left from previous runs, normally
# there should be none so make sure to warn
# if there is one
#
sub kill_leftovers ($) {
  my $rundir= shift;
  return unless ( -d $rundir );

  mtr_report("Checking leftover processes...");

  # Scan the "run" directory for process id's to kill
  opendir(RUNDIR, $rundir)
    or mtr_error("kill_leftovers, can't open dir \"$rundir\": $!");
  while ( my $elem= readdir(RUNDIR) )
  {
    # Only read pid from files that end with .pid
    if ( $elem =~ /.*[.]pid$/ )
    {
      my $pidfile= "$rundir/$elem";
      next unless -f $pidfile;
      my $pid= mtr_fromfile($pidfile);
      unlink($pidfile);
      unless ($pid=~ /^(\d+)/){
	# The pid was not a valid number
	mtr_warning("Got invalid pid '$pid' from '$elem'");
	next;
      }
      mtr_report(" - found old pid $pid in '$elem', killing it...");

      my $ret= kill("KILL", $pid);
      if ($ret == 0) {
	mtr_report("   process did not exist!");
	next;
      }

      my $check_counter= 100;
      while ($ret > 0 and $check_counter--) {
	mtr_milli_sleep(100);
	$ret= kill(0, $pid);
      }
      mtr_report($check_counter ? "   ok!" : "   failed!");
    }
    else
    {
      mtr_warning("Found non pid file '$elem' in '$rundir'")
	if -f "$rundir/$elem";
    }
  }
  closedir(RUNDIR);
}

#
# Check that all the ports that are going to
# be used are free
#
sub check_ports_free ($)
{
  my $bthread= shift;
  my $portbase = $bthread * $opt_port_group_size + 10000;
  for ($portbase..$portbase+($opt_port_group_size-1)){
    if (mtr_ping_port($_)){
      mtr_report(" - 'localhost:$_' was not free");
      return 0; # One port was not free
    }
  }

  return 1; # All ports free
}


sub initialize_servers {

  if ( using_extern() )
  {
    # Running against an already started server, if the specified
    # vardir does not already exist it should be created
    if ( ! -d $opt_vardir )
    {
      setup_vardir();
    }
    else
    {
      mtr_verbose("No need to create '$opt_vardir' it already exists");
    }
  }
  else
  {
    # Kill leftovers from previous run
    # using any pidfiles found in var/run
    kill_leftovers("$opt_vardir/run");

    if ( ! $opt_start_dirty )
    {
      remove_stale_vardir();
      setup_vardir();
    }
  }
}


#
# Remove all newline characters expect after semicolon
#
sub sql_to_bootstrap {
  my ($sql) = @_;
  my @lines= split(/\R/, $sql);
  my $result= "\n";
  my $delimiter= ';';

  foreach my $line (@lines) {

    # Change current delimiter if line starts with "delimiter"
    if ( $line =~ /^delimiter (.*)/ ) {
      my $new= $1;
      # Remove old delimiter from end of new
      $new=~ s/\Q$delimiter\E$//;
      $delimiter = $new;
      mtr_debug("changed delimiter to $delimiter");
      # No need to add the delimiter to result
      next;
    }

    # Add newline if line ends with $delimiter
    # and convert the current delimiter to semicolon
    if ( $line =~ /\Q$delimiter\E$/ ){
      $line =~ s/\Q$delimiter\E$/;/;
      $result.= "$line\n";
      mtr_debug("Added default delimiter");
      next;
    }

    # Remove comments starting with --
    if ( $line =~ /^\s*--/ ) {
      mtr_debug("Discarded $line");
      next;
    }

    # Replace @HOSTNAME with localhost
    $line=~ s/\'\@HOSTNAME\@\'/localhost/;

    # Default, just add the line without newline
    # but with a space as separator
    $result.= "$line ";

  }
  return $result;
}


sub default_mysqld {
  # Generate new config file from template
  environment_setup();
  my $config= My::ConfigFactory->new_config
    ( {
       basedir         => $basedir,
       testdir         => $glob_mysql_test_dir,
       template_path   => "include/default_my.cnf",
       vardir          => $opt_vardir,
       tmpdir          => $opt_tmpdir,
       baseport        => 0,
       user            => $opt_user,
       password        => '',
      }
    );

  my $mysqld= $config->group('mysqld.1')
    or mtr_error("Couldn't find mysqld.1 in default config");
  return $mysqld;
}


sub mysql_install_db {
  my ($mysqld, $datadir, $extra_opts)= @_;

  my $install_datadir= $datadir || $mysqld->value('datadir');
  my $install_basedir= $mysqld->value('basedir');
  my $install_lang= $mysqld->value('lc-messages-dir');
  my $install_chsdir= $mysqld->value('character-sets-dir');

  mtr_report("Installing system database...");

  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--disable-getopt-prefix-matching");
  mtr_add_arg($args, "--bootstrap");
  mtr_add_arg($args, "--basedir=%s", $install_basedir);
  mtr_add_arg($args, "--datadir=%s", $install_datadir);
  mtr_add_arg($args, "--plugin-dir=%s", $plugindir);
  mtr_add_arg($args, "--default-storage-engine=myisam");
  mtr_add_arg($args, "--loose-skip-plugin-$_") for @optional_plugins;
  # starting from 10.0 bootstrap scripts require InnoDB
  mtr_add_arg($args, "--loose-innodb");
  mtr_add_arg($args, "--loose-innodb-log-file-size=10M");
  mtr_add_arg($args, "--disable-sync-frm");
  mtr_add_arg($args, "--tmpdir=%s", "$opt_vardir/tmp/");
  mtr_add_arg($args, "--core-file");
  mtr_add_arg($args, "--console");
  mtr_add_arg($args, "--character-set-server=latin1");
  mtr_add_arg($args, "--loose-disable-performance-schema");

  if ( $opt_debug )
  {
    mtr_add_arg($args, "--debug-dbug=$debug_d:t:i:A,%s/log/bootstrap.trace",
		$path_vardir_trace);
  }

  mtr_add_arg($args, "--lc-messages-dir=%s", $install_lang);
  mtr_add_arg($args, "--character-sets-dir=%s", $install_chsdir);

  # InnoDB arguments that affect file location and sizes may
  # need to be given to the bootstrap process as well as the
  # server process.
  foreach my $extra_opt ( @opt_extra_mysqld_opt ) {
    if ($extra_opt =~ /--innodb/) {
      mtr_add_arg($args, $extra_opt);
    }
  }

  # If DISABLE_GRANT_OPTIONS is defined when the server is compiled (e.g.,
  # configure --disable-grant-options), mysqld will not recognize the
  # --bootstrap or --skip-grant-tables options.  The user can set
  # MYSQLD_BOOTSTRAP to the full path to a mysqld which does accept
  # --bootstrap, to accommodate this.
  my $exe_mysqld_bootstrap =
    $ENV{'MYSQLD_BOOTSTRAP'} || find_mysqld($install_basedir);

  # ----------------------------------------------------------------------
  # export MYSQLD_BOOTSTRAP_CMD variable containing <path>/mysqld <args>
  # ----------------------------------------------------------------------
  $ENV{'MYSQLD_BOOTSTRAP_CMD'}= "$exe_mysqld_bootstrap " . join(" ", @$args)
    unless defined $ENV{'MYSQLD_BOOTSTRAP_CMD'};

  # Extra options can come not only from the command line, but also
  # from option files or combinations. We want them on a command line
  # that is executed now, because otherwise the datadir might be 
  # incompatible with the test settings, but not on the general
  # $MYSQLD_BOOTSTRAP_CMD line
  foreach my $extra_opt ( @$extra_opts ) {
    mtr_add_arg($args, $extra_opt);
  }

  # ----------------------------------------------------------------------
  # Create the bootstrap.sql file
  # ----------------------------------------------------------------------
  my $bootstrap_sql_file= "$opt_vardir/log/bootstrap.sql";
  $ENV{'MYSQL_BOOTSTRAP_SQL_FILE'}= $bootstrap_sql_file;

  if (! -e $bootstrap_sql_file)
  {
    My::Debugger::setup_boot_args(\$args, \$exe_mysqld_bootstrap, $bootstrap_sql_file);

    my $path_sql= my_find_file($install_basedir,
             ["mysql", "sql/share", "share/mariadb",
              "share/mysql", "share", "scripts"],
             "mysql_system_tables.sql",
             NOT_REQUIRED);

    if (-f $path_sql )
    {
      my $sql_dir= dirname($path_sql);
      # Use the mysql database for system tables
      mtr_tofile($bootstrap_sql_file, "use mysql;\n");

      # Add the offical mysql system tables
      # for a production system
      mtr_appendfile_to_file("$sql_dir/mysql_system_tables.sql",
           $bootstrap_sql_file);

      my $gis_sp_path = $source_dist ? "$bindir/scripts" : $sql_dir;
      mtr_appendfile_to_file("$gis_sp_path/maria_add_gis_sp_bootstrap.sql",
           $bootstrap_sql_file);

      # Add the performance tables
      # for a production system
      mtr_appendfile_to_file("$sql_dir/mysql_performance_tables.sql",
                            $bootstrap_sql_file);

      # Add the mysql system tables initial data
      # for a production system
      mtr_appendfile_to_file("$sql_dir/mysql_system_tables_data.sql",
           $bootstrap_sql_file);

      # Add test data for timezone - this is just a subset, on a real
      # system these tables will be populated either by mysql_tzinfo_to_sql
      # or by downloading the timezone table package from our website
      mtr_appendfile_to_file("$sql_dir/mysql_test_data_timezone.sql",
           $bootstrap_sql_file);

      # Fill help tables, just an empty file when running from bk repo
      # but will be replaced by a real fill_help_tables.sql when
      # building the source dist
      mtr_appendfile_to_file("$sql_dir/fill_help_tables.sql",
           $bootstrap_sql_file);

      # Append sys schema
      mtr_appendfile_to_file("$gis_sp_path/mysql_sys_schema.sql",
           $bootstrap_sql_file);

      mtr_tofile($bootstrap_sql_file, "CREATE DATABASE IF NOT EXISTS test CHARACTER SET latin1 COLLATE latin1_swedish_ci;\n");

      # mysql.gtid_slave_pos was created in InnoDB, but many tests
      # run without InnoDB. Alter it to Aria now
      mtr_tofile($bootstrap_sql_file, "ALTER TABLE gtid_slave_pos ENGINE=Aria transactional=0;\n");
    }
    else
    {
      # Install db from init_db.sql that exist in early 5.1 and 5.0
      # versions of MySQL
      my $init_file= "$install_basedir/mysql-test/lib/init_db.sql";
      mtr_report(" - from '$init_file'");
      my $text= mtr_grab_file($init_file) or
        mtr_error("Can't open '$init_file': $!");

      mtr_tofile($bootstrap_sql_file,
           sql_to_bootstrap($text));
    }

    # Remove anonymous users
    mtr_tofile($bootstrap_sql_file,
         "DELETE FROM mysql.global_priv where user= '';\n");

    # Create mtr database
    mtr_tofile($bootstrap_sql_file,
         "CREATE DATABASE mtr CHARSET=latin1;\n");

    # Add help tables and data for warning detection and supression
    mtr_tofile($bootstrap_sql_file,
               sql_to_bootstrap(mtr_grab_file("include/mtr_warnings.sql")));

    # Add procedures for checking server is restored after testcase
    mtr_tofile($bootstrap_sql_file,
               sql_to_bootstrap(mtr_grab_file("include/mtr_check.sql")));
  }

  # Log bootstrap command
  my $path_bootstrap_log= "$opt_vardir/log/bootstrap.log";
  mtr_tofile($path_bootstrap_log,
	     "$exe_mysqld_bootstrap " . join(" ", @$args) . "\n");

  # Create directories mysql
  mkpath("$install_datadir/mysql");

  my $realtime= gettimeofday();
  if ( My::SafeProcess->run
       (
	name          => "bootstrap",
	path          => $exe_mysqld_bootstrap,
	args          => \$args,
	input         => $bootstrap_sql_file,
	output        => $path_bootstrap_log,
	error         => $path_bootstrap_log,
	append        => 1,
	verbose       => $opt_verbose,
       ) != 0)
  {
    find(
    {
      no_chdir => 1,
      wanted => sub
      {
        My::CoreDump::core_wanted(\$num_saved_cores,
                                  $opt_max_save_core,
                                  @opt_cases == 0,
                                  $exe_mysqld_bootstrap, $opt_parallel);
      }
    },
    $install_datadir);

    my $data= mtr_grab_file($path_bootstrap_log);
    mtr_error("Error executing mariadbd --bootstrap\n" .
              "Could not install system database from $bootstrap_sql_file\n" .
	      "The $path_bootstrap_log file contains:\n$data\n");
  }
  else
  {
    mtr_verbose("Spent " . sprintf("%.3f", (gettimeofday() - $realtime)) . " seconds in bootstrap");
  }
}


sub run_testcase_check_skip_test($)
{
  my ($tinfo)= @_;

  # ----------------------------------------------------------------------
  # Skip some tests silently
  # ----------------------------------------------------------------------

  my $start_from= $mtr_cases::start_from;
  if ( $start_from )
  {
    if ($tinfo->{'name'} eq $start_from ||
        $tinfo->{'shortname'} eq $start_from)
    {
      ## Found parting test. Run this test and all tests after this one
      $mtr_cases::start_from= "";
    }
    else
    {
      $tinfo->{'result'}= 'MTR_RES_SKIPPED';
      return 1;
    }
  }

  # ----------------------------------------------------------------------
  # If marked to skip, just print out and return.
  # Note that a test case not marked as 'skip' can still be
  # skipped later, because of the test case itself in cooperation
  # with the mysqltest program tells us so.
  # ----------------------------------------------------------------------

  if ( $tinfo->{'skip'} )
  {
    mtr_report_test_skipped($tinfo) unless $start_only;
    return 1;
  }

  return 0;
}


sub run_query {
  my ($tinfo, $mysqld, $query)= @_;

  my $args;
  mtr_init_args(\$args);
  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--defaults-group-suffix=%s", $mysqld->after('mysqld'));

  mtr_add_arg($args, "-e %s", $query);

  my $res= My::SafeProcess->run
    (
     name          => "run_query -> ".$mysqld->name(),
     path          => $exe_mysql,
     args          => \$args,
     output        => '/dev/null',
     error         => '/dev/null'
    );

  return $res
}


sub do_before_run_mysqltest($)
{
  my $tinfo= shift;
  my $resfile= $tinfo->{result_file};
  return unless defined $resfile;

  # Remove old files produced by mysqltest
  die "unsupported result file name $resfile, stoping" unless
         $resfile =~ /^(.*?)((?:,\w+)*)\.(rdiff|result|result~)$/;
  my ($base_file, $suites, $ext)= ($1, $2, $3);
  # if the result file is a diff, make a proper result file
  if ($ext eq 'rdiff') {
    my $base_result = $tinfo->{base_result};
    my $resdir= dirname($resfile);
    # we'll use a separate extension for generated result files
    # to be able to distinguish them from manually created
    # version-controlled results, and to ignore them in git.
    my $dest = "$base_file$suites.result~";
    my @cmd = ($exe_patch);
    if ($^O eq "MSWin32") {
      push @cmd, '--binary';
    }
    push @cmd, (qw/-r - -f -s -o/, $dest . $$, $base_result, $resfile);
    if (-w $resdir) {
      # don't rebuild a file if it's up to date
      unless (-e $dest and -M $dest < -M $resfile
                       and -M $dest < -M $base_result) {
        run_system(@cmd);
        rename $cmd[-3], $dest or unlink $cmd[-3];
      }
    } else {
      $dest = $opt_tmpdir . '/' . basename($dest);
      $cmd[-3] = $dest . $$;
      run_system(@cmd);
      rename $cmd[-3], $dest or unlink $cmd[-3];
    }

    $tinfo->{result_file} = $dest;
  }

  unlink("$base_file.reject");
  unlink("$base_file.progress");
  unlink("$base_file.log");
  unlink("$base_file.warnings");
}


#
# Check all server for sideffects
#
# RETURN VALUE
#  0 ok
#  1 Check failed
#  >1 Fatal errro

sub check_testcase($$)
{
  my ($tinfo, $mode)= @_;
  my $tname= $tinfo->{name};

  # Start the mysqltest processes in parallel to save time
  # also makes it possible to wait for any process to exit during the check
  my %started;
  foreach my $mysqld ( mysqlds() )
  {
    # Skip if server has been restarted with additional options
    if ( defined $mysqld->{'proc'} && ! exists $mysqld->{'restart_opts'} )
    {
      my $proc= start_check_testcase($tinfo, $mode, $mysqld);
      $started{$proc->pid()}= $proc;
    }
  }

  # Return immediately if no check proceess was started
  return 0 unless ( keys %started );

  my $timeout= start_timer(check_timeout($tinfo));

  while (1){
    my $result;
    my $proc= My::SafeProcess->wait_any_timeout($timeout);
    mtr_report("Got $proc");

    if ( delete $started{$proc->pid()} ) {

      my $err_file= $proc->user_data();
      my $base_file= mtr_match_extension($err_file, "err"); # Trim extension

      # One check testcase process returned
      my $res= $proc->exit_status();

      if ( $res == 0){
	# Check completed without problem

	# Remove the .err file the check generated
	unlink($err_file);

	# Remove the .result file the check generated
	if ( $mode eq 'after' ){
	  unlink("$base_file.result");
	}

	if ( keys(%started) == 0){
	  # All checks completed
	  mark_time_used('check');
	  return 0;
	}
	# Wait for next process to exit
	next;
      }
      else
      {
	if ( $mode eq "after" and $res == 1 )
	{
	  # Test failed, grab the report mysqltest has created
	  my $report= mtr_grab_file($err_file);
	  $tinfo->{check}.=
	    "\nMTR's internal check of the test case '$tname' failed.
This means that the test case does not preserve the state that existed
before the test case was executed.  Most likely the test case did not
do a proper clean-up. It could also be caused by the previous test run
by this thread, if the server wasn't restarted.
This is the diff of the states of the servers before and after the
test case was executed:\n";
	  $tinfo->{check}.= $report;

	  # Check failed, mark the test case with that info
	  $tinfo->{'check_testcase_failed'}= 1;
	  $result= 1;
	}
	elsif ( $res )
	{
	  my $report= mtr_grab_file($err_file);
	  $tinfo->{comment}.=
	    "Could not execute 'check-testcase' $mode ".
	      "testcase '$tname' (res: $res):\n";
	  $tinfo->{comment}.= $report;

	  $result= 2;
	}
        else
        {
          # Remove the .result file the check generated
          unlink("$base_file.result");
        }
	# Remove the .err file the check generated
	unlink($err_file);

      }
    }
    elsif ( $proc->{timeout} ) {
      $tinfo->{comment}.= "Timeout for 'check-testcase' expired after "
	.check_timeout($tinfo)." seconds";
      $result= 4;
    }
    else {
      # Unknown process returned, most likley a crash, abort everything
      $tinfo->{comment}=
	"The server $proc crashed while running ".
	"'check testcase $mode test'".
	get_log_from_proc($proc, $tinfo->{name});
      $result= 3;
    }

    # Kill any check processes still running
    map($_->kill(), values(%started));

    mtr_warning("Check-testcase failed, this could also be caused by the" .
		" previous test run by this worker thread")
      if $result > 1 && $mode eq "before";
    mark_time_used('check');

    return $result;
  }

  mtr_error("INTERNAL_ERROR: check_testcase");
}


# Start run mysqltest on one server
#
# RETURN VALUE
#  0 OK
#  1 Check failed
#
sub start_run_one ($$) {
  my ($mysqld, $run)= @_;

  my $name= "$run-".$mysqld->name();

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--defaults-group-suffix=%s", $mysqld->after('mysqld'));

  mtr_add_arg($args, "--silent");
  mtr_add_arg($args, "--test-file=%s", "include/$run.test");

  my $errfile= "$opt_vardir/tmp/$name.err";
  my $proc= My::SafeProcess->new
    (
     name          => $name,
     path          => $exe_mysqltest,
     error         => $errfile,
     output        => $errfile,
     args          => \$args,
     user_data     => $errfile,
     verbose       => $opt_verbose,
    );
  mtr_verbose("Started $proc");
  return $proc;
}


#
# Run script on all servers, collect results
#
# RETURN VALUE
#  0 ok
#  1 Failure

sub run_on_all($$)
{
  my ($tinfo, $run)= @_;

  # Start the mysqltest processes in parallel to save time
  # also makes it possible to wait for any process to exit during the check
  # and to have a timeout process
  my %started;
  foreach my $mysqld ( mysqlds() )
  {
    if ( defined $mysqld->{'proc'} )
    {
      my $proc= start_run_one($mysqld, $run);
      $started{$proc->pid()}= $proc;
    }
  }

  # Return immediately if no check proceess was started
  return 0 unless ( keys %started );

  my $timeout= start_timer(check_timeout($tinfo));

  while (1){
    my $result;
    my $proc= My::SafeProcess->wait_any_timeout($timeout);
    mtr_report("Got $proc");

    if ( delete $started{$proc->pid()} ) {

      # One mysqltest process returned
      my $err_file= $proc->user_data();
      my $res= $proc->exit_status();

      # Append the report from .err file
      $tinfo->{comment}.= " == $err_file ==\n";
      $tinfo->{comment}.= mtr_grab_file($err_file);
      $tinfo->{comment}.= "\n";

      # Remove the .err file
      unlink($err_file);

      if ( keys(%started) == 0){
	# All completed
	return 0;
      }

      # Wait for next process to exit
      next;
    }
    elsif ($proc->{timeout}) {
      $tinfo->{comment}.= "Timeout for '$run' expired after "
	.check_timeout($tinfo)." seconds";
    }
    else {
      # Unknown process returned, most likley a crash, abort everything
      $tinfo->{comment}.=
	"The server $proc crashed while running '$run'".
	get_log_from_proc($proc, $tinfo->{name});
    }

    # Kill any check processes still running
    map($_->kill(), values(%started));

    return 1;
  }
  mtr_error("INTERNAL_ERROR: run_on_all");
}


sub mark_log {
  my ($log, $tinfo)= @_;
  my $log_msg= "CURRENT_TEST: $tinfo->{name}\n";
  pre_write_errorlog($log, $tinfo->{name});
  mtr_tofile($log, $log_msg);
}


sub find_testcase_skipped_reason($)
{
  my ($tinfo)= @_;

  # Set default message
  $tinfo->{'comment'}= "Detected by testcase(no log file)";

  # Open the test log file
  my $F= IO::File->new($path_current_testlog)
    or return;
  my $reason;

  while ( my $line= <$F> )
  {
    # Look for "reason: <reason for skipping test>"
    if ( $line =~ /reason: (.*)/ )
    {
      $reason= $1;
    }
  }

  if ( ! $reason )
  {
    mtr_warning("Could not find reason for skipping test in $path_current_testlog");
    $reason= "Detected by testcase(reason unknown) ";
  }
  $tinfo->{'comment'}= $reason;
}


sub find_analyze_request
{
  # Open the test log file
  my $F= IO::File->new($path_current_testlog)
    or return;
  my $analyze;

  while ( my $line= <$F> )
  {
    # Look for "reason: <reason for skipping test>"
    if ( $line =~ /analyze: (.*)/ )
    {
      $analyze= $1;
    }
  }

  return $analyze;
}

# The test can leave a file in var/tmp/ to signal
# that all servers should be restarted
sub restart_forced_by_test($)
{
  my $file = shift;
  my $restart = 0;
  foreach my $mysqld ( mysqlds() )
  {
    my $datadir = $mysqld->value('datadir');
    my $force_restart_file = "$datadir/mtr/$file";
    if ( -f $force_restart_file )
    {
      mtr_verbose("Restart of servers forced by test");
      $restart = 1;
      last;
    }
  }
  return $restart;
}

# Return timezone value of tinfo or default value
sub timezone {
  my ($tinfo)= @_;
  local $_ = $tinfo->{timezone};
  return 'DEFAULT' unless defined $_;
  no warnings 'uninitialized';
  s/\$\{(\w+)\}/$ENV{$1}/ge;
  s/\$(\w+)/$ENV{$1}/ge;
  $_;
}

sub mycnf_create {
  my ($config) = @_;
  my $res;

  foreach my $group ($config->option_groups()) {
    $res .= "[$group->{name}]\n";

    foreach my $option ($group->options()) {
      $res .= $option->name();
      my $value= $option->value();
      if (defined $value) {
	$res .= "=$value";
      }
      $res .= "\n";
    }
    $res .= "\n";
  }
  $res;
}

sub config_files($) {
  my ($tinfo) = @_;
  (
    'my.cnf' => \&mycnf_create,
    $tinfo->{suite}->config_files()
  );
}

sub _like   { return $config ? $config->like($_[0]) : (); }
sub mysqlds { return _like('mysqld\.'); }

sub fix_servers($) {
  my ($tinfo) = @_;
  return () unless $config;
  my %servers = (
    qr/mysqld\./ => {
      SORT => 300,
      START => \&mysql_server_start,
      WAIT => \&mysql_server_wait,
    },
    $tinfo->{suite}->servers()
  );
  for ($config->groups()) {
    while (my ($re,$prop) = each %servers) {
      @$_{keys %$prop} = values %$prop if $_->{name} =~ /^$re/;
    }
  }
}

sub all_servers {
  return unless $config;
  ( sort { $a->{SORT} <=> $b->{SORT} }
       grep { defined $_->{SORT} } $config->groups() );
}

# Storage for changed environment variables
our %old_env;

sub resfile_report_test ($) {
  my $tinfo=  shift;

  resfile_new_test();

  resfile_test_info("name", $tinfo->{name});
  resfile_test_info("variation", $tinfo->{combination})
    if $tinfo->{combination};
  resfile_test_info("start_time", isotime time);
}


#
# Run a single test case
#
# RETURN VALUE
#  0 OK
#  > 0 failure
#

sub run_testcase ($$) {
  my ($tinfo, $server_socket)= @_;
  my $print_freq=20;

  mtr_verbose("Running test:", $tinfo->{name});
  $ENV{'MTR_TEST_NAME'} = $tinfo->{name};
  resfile_report_test($tinfo) if $opt_resfile;

  for my $key (grep { /^MTR_COMBINATION/ } keys %ENV)
  {
    delete $ENV{$key};
  }

  if (ref $tinfo->{combinations} eq 'ARRAY')
  {
    for (my $i = 0; $i < @{$tinfo->{combinations}}; ++$i )
    {
      my $combination = $tinfo->{combinations}->[$i];
      # Allow only alphanumerics plus _ - + . in combination names,
      # or anything beginning with -- (the latter comes from --combination)
      if ($combination && $combination !~ /^\w[-\w\.\+]*$/
                      && $combination !~ /^--/)
      {
        mtr_error("Combination '$combination' contains illegal characters");
      }
      $ENV{"MTR_COMBINATION_". uc(${combination})} = 1;
    }
    $ENV{"MTR_COMBINATIONS"} = join(',', @{$tinfo->{combinations}});
  }
  elsif (exists $tinfo->{combinations})
  {
    die 'Unexpected type of $tinfo->{combinations}';
  }

  # -------------------------------------------------------
  # Init variables that can change between each test case
  # -------------------------------------------------------
  my $timezone= timezone($tinfo);
  if ($timezone ne 'DEFAULT') {
    $ENV{'TZ'}= $timezone;
  } else {
    delete($ENV{'TZ'});
  }
  $ENV{MTR_SUITE_DIR} = $tinfo->{suite}->{dir};
  mtr_verbose("Setting timezone: $timezone");

  if ( ! using_extern() )
  {
    my @restart= servers_need_restart($tinfo);
    if ( @restart != 0) {
      # Remember that we restarted for this test case (count restarts)
      $tinfo->{'restarted'}= 1;
      stop_servers(reverse @restart);
      if ($opt_warnings) {
        check_warnings_post_shutdown($server_socket);
      }
    }

    if ( started(all_servers()) == 0 )
    {

      # Remove old datadirs
      clean_datadir() unless $opt_start_dirty;

      # Restore old ENV
      while (my ($option, $value)= each( %old_env )) {
	if (defined $value){
	  mtr_verbose("Restoring $option to $value");
	  $ENV{$option}= $value;

	} else {
	  mtr_verbose("Removing $option");
	  delete($ENV{$option});
	}
      }
      %old_env= ();

      mtr_verbose("Generating my.cnf from '$tinfo->{template_path}'");

      # Generate new config file from template
      $config= My::ConfigFactory->new_config
	( {
           testname        => $tinfo->{name},
	   basedir         => $basedir,
	   testdir         => $glob_mysql_test_dir,
	   template_path   => $tinfo->{template_path},
	   extra_template_path => $tinfo->{extra_template_path},
	   vardir          => $opt_vardir,
	   tmpdir          => $opt_tmpdir,
	   baseport        => $baseport,
	   user            => $opt_user,
	   password        => '',
	   ssl             => $opt_ssl_supported,
	   embedded        => $opt_embedded_server,
	  }
	);

      fix_servers($tinfo);

      # Write config files:
      my %config_files = config_files($tinfo);
      while (my ($file, $generate) = each %config_files) {
        next unless $generate;
        my ($path) = "$opt_vardir/$file";
        open (F, '>', $path) or die "Could not open '$path': $!";
        print F &$generate($config);
        close F;
      }

      # Remember current config so a restart can occur when a test need
      # to use a different one
      $current_config_name= $tinfo->{template_path};

      #
      # Set variables in the ENV section
      #
      foreach my $option ($config->options_in_group("ENV"))
      {
        my ($name, $val)= ($option->name(), $option->value());

	# Save old value to restore it before next time
	$old_env{$name}= $ENV{$name};

        unless (defined $val) {
          mtr_warning("Uninitialized value for ", $name,
            ", group [ENV], file ", $current_config_name);
        } else {
          mtr_verbose($name, "=", $val);
          $ENV{$name}= $val;
        }
      }
    }

    # Set up things for catalogs
    # The values of MARIADB_TOPDIR and MARIAD_DATADIR should
    # be taken from the values used by the default (first)
    # connection that is used by mariadb-test.
    my ($mysqld, @servers);
    @servers= all_servers();
    $mysqld= $servers[0];
    $ENV{'MARIADB_TOPDIR'}= $mysqld->value('datadir');
    if (!$opt_catalogs)
    {
      $ENV{'MARIADB_DATADIR'}= $mysqld->value('datadir');
    }
    else
    {
      $ENV{'MARIADB_DATADIR'}= $mysqld->value('datadir') . "/" . $catalog_name;
    }

    # Write start of testcase to log
    mark_log($path_current_testlog, $tinfo);

    # Make sure the safe_process also exits from now on
    if ($opt_start_exit) {
      My::SafeProcess->start_exit();
    }

    if (start_servers($tinfo))
    {
      report_failure_and_restart($tinfo);
      unlink $path_current_testlog;
      return 1;
    }
  }
  mark_time_used('restart');

  # --------------------------------------------------------------------
  # If --start or --start-dirty given, stop here to let user manually
  # run tests
  # If --wait-all is also given, do the same, but don't die if one
  # server exits
  # ----------------------------------------------------------------------

  if ( $start_only )
  {
    mtr_print("\nStarted", started(all_servers()));
    mtr_print("Using config for test", $tinfo->{name});
    mtr_print("Port and socket path for server(s):");
    foreach my $mysqld ( mysqlds() )
    {
      mtr_print ($mysqld->name() . "  " . $mysqld->value('port') .
	      "  " . $mysqld->value('socket'));
    }
    if ( $opt_start_exit )
    {
      mtr_print("Server(s) started, not waiting for them to finish");
      if (IS_WINDOWS)
      {
	POSIX::_exit(0);	# exit hangs here in ActiveState Perl
      }
      else
      {
	exit(0);
      }
    }
    mtr_print("Waiting for server(s) to exit...");
    if ( $opt_wait_all ) {
      My::SafeProcess->wait_all();
      mtr_print( "All servers exited" );
      exit(1);
    }
    else {
      my $proc= My::SafeProcess->wait_any();
      if ( grep($proc eq $_, started(all_servers())) )
      {
        mtr_print("Server $proc died");
        exit(1);
      }
      mtr_print("Unknown process $proc died");
      exit(1);
    }
  }

  my $test_timeout= start_timer(testcase_timeout($tinfo));

  do_before_run_mysqltest($tinfo);

  mark_time_used('admin');

  if ( $opt_check_testcases and check_testcase($tinfo, "before") ){
    # Failed to record state of server or server crashed
    report_failure_and_restart($tinfo);

    return 1;
  }

  my $test= $tinfo->{suite}->start_test($tinfo);
  # Set to a list of processes we have to keep waiting (expectedly died servers)
  my %keep_waiting_proc = ();
  my $print_timeout= start_timer($print_freq * 60);

  while (1)
  {
    my $proc;
    if (%keep_waiting_proc)
    {
      # Any other process exited?
      $proc = My::SafeProcess->check_any();
      if ($proc)
      {
	mtr_verbose ("Found exited process $proc");
      }
      else
      {
	# Also check if timer has expired, if so cancel waiting
	if ( has_expired($test_timeout) )
	{
	  %keep_waiting_proc = ();
	}
      }
    }
    if (!%keep_waiting_proc && !$proc)
    {
      if ($test_timeout > $print_timeout)
      {
        $proc= My::SafeProcess->wait_any_timeout($print_timeout);
        if ($proc->{timeout})
        {
          #print out that the test is still on
          mtr_print("Test still running: $tinfo->{name}");
          #reset the timer
          $print_timeout= start_timer($print_freq * 60);
          next;
        }
      }
      else
      {
        $proc= My::SafeProcess->wait_any_timeout($test_timeout);
      }
    }

    if ($proc and $proc eq $test) # mysqltest itself exited
    {
      my $res= $test->exit_status();

      if (($res == 0 or $res == 62) and $opt_warnings and check_warnings($tinfo) )
      {
        # If test case suceeded, but it has produced unexpected
        # warnings, continue with $res == 1;
        # but if the test was skipped, it should remain skipped
        $res= 1 if $res == 0;
        resfile_output($tinfo->{'warnings'}) if $opt_resfile;
      }

      if ( $res == 0 )
      {
        my $check_res;
	if ( restart_forced_by_test('force_restart') )
        {
          stop_all_servers($opt_shutdown_timeout);
        }
        elsif ( $opt_check_testcases and
                $check_res= check_testcase($tinfo, "after"))
        {
          if ($check_res == 1) {
            # Test case had sideeffects, not fatal error, just continue
            if ($opt_warnings) {
              # Checking error logs for warnings, so need to stop server
              # gracefully so that memory leaks etc. can be properly detected.
              stop_servers(reverse all_servers());
              check_warnings_post_shutdown($server_socket);
              # Even if we got warnings here, we should not fail this
              # particular test, as the warnings may be caused by an earlier
              # test.
            } else {
              # Not checking warnings, so can do a hard shutdown.
              stop_all_servers($opt_shutdown_timeout);
            }
            mtr_report("Resuming tests...\n");
            resfile_output($tinfo->{'check'}) if $opt_resfile;
          }
          else {
            # Test case check failed fatally, probably a server crashed
            report_failure_and_restart($tinfo);
            return 1;
          }
        }
        mtr_report_test_passed($tinfo);
      }
      elsif ( $res == 62 )
      {
        # Testcase itself tell us to skip this one
        $tinfo->{skip_detected_by_test}= 1;
        # Try to get reason from test log file
        find_testcase_skipped_reason($tinfo);
        mtr_report_test_skipped($tinfo);
        # Restart if skipped due to missing perl, it may have had side effects
	if ( restart_forced_by_test('force_restart_if_skipped') ||
             $tinfo->{'comment'} =~ /^perl not found/ )
        {
          stop_all_servers($opt_shutdown_timeout);
        }
      }
      elsif ( $res == 65 )
      {
        # Testprogram killed by signal
        $tinfo->{comment}=
          "testprogram crashed(returned code $res)";
        report_failure_and_restart($tinfo);
      }
      elsif ( $res == 1 )
      {
        # Check if the test tool requests that
        # an analyze script should be run
        my $analyze= find_analyze_request();
        if ($analyze){
          run_on_all($tinfo, "analyze-$analyze");
        }

        # Wait a bit and see if a server died, if so report that instead
        mtr_milli_sleep(100);
        my $srvproc= My::SafeProcess::check_any();
        if ($srvproc && grep($srvproc eq $_, started(all_servers()))) {
          $proc= $srvproc;
          goto SRVDIED;
        }

        # Test case failure reported by mysqltest
        report_failure_and_restart($tinfo);
      }
      else
      {
        # mysqltest failed, probably crashed
        $tinfo->{comment}=
          "mysqltest failed with unexpected return code $res\n";
        report_failure_and_restart($tinfo);
      }

      # Save info from this testcase run to mysqltest.log
      if( -f $path_current_testlog)
      {
        if ($opt_resfile && $res && $res != 62) {
          resfile_output_file($path_current_testlog);
        }
        mtr_appendfile_to_file($path_current_testlog, $path_testlog);
        unlink($path_current_testlog);
      }

      return ($res == 62) ? 0 : $res;
    } # if ($proc and $proc eq $test)
    elsif ($proc)
    {
      # It was not mysqltest that exited, add to a wait-to-be-started-again list.
      $keep_waiting_proc{$proc} = 1;
    }

    mtr_verbose("Got " . join(",", keys(%keep_waiting_proc)));

    mark_time_used('test');
    foreach my $wait_for_proc (keys(%keep_waiting_proc)) {
      # ----------------------------------------------------
      # Check if it was an expected crash
      # ----------------------------------------------------
      my @mysqld = grep($wait_for_proc eq $_->{proc}, mysqlds());
      goto SRVDIED unless @mysqld;
      my $check_crash = check_expected_crash_and_restart($mysqld[0]);
      if ($check_crash == 0) # unexpected exit/crash of $wait_for_proc
      {
        $proc= $mysqld[0]->{proc};
        goto SRVDIED;
      }
      elsif ($check_crash == 1) # $wait_for_proc was started again by check_expected_crash_and_restart()
      {
        delete $keep_waiting_proc{$wait_for_proc};
      }
      elsif ($check_crash == 2) # we must keep waiting
      {
        # do nothing
      }
    } # foreach my $wait_for_proc

    next;

  SRVDIED:
    # ----------------------------------------------------
    # Stop the test case timer
    # ----------------------------------------------------
    $test_timeout= 0;

    # ----------------------------------------------------
    # Check if it was a server that died
    # ----------------------------------------------------
    if ( grep($proc eq $_, started(all_servers())) )
    {
      # Server failed, probably crashed
      $tinfo->{comment}=
	"Server $proc failed during test run" .
	get_log_from_proc($proc, $tinfo->{name});

      # ----------------------------------------------------
      # It's not mysqltest that has exited, kill it
      # ----------------------------------------------------
      $test->kill();

      report_failure_and_restart($tinfo);
      return 1;
    }

    # Try to dump core for mysqltest and all servers
    foreach my $proc ($test, started(all_servers()))
    {
      mtr_print("Trying to dump core for $proc");
      if ($proc->dump_core())
      {
	$proc->wait_one(20);
      }
    }

    # ----------------------------------------------------
    # It's not mysqltest that has exited, kill it
    # ----------------------------------------------------
    $test->kill();

    # ----------------------------------------------------
    # Check if testcase timer expired
    # ----------------------------------------------------
    if ( $proc->{timeout} )
    {
      my $log_file_name= $opt_vardir."/log/".$tinfo->{shortname}.".log";
      $tinfo->{comment}=
        "Test case timeout after ".testcase_timeout($tinfo).
	  " seconds\n\n";
      # Add 20 last executed commands from test case log file
      if  (-e $log_file_name)
      {
        $tinfo->{comment}.=
	   "== $log_file_name == \n".
	     mtr_lastlinesfromfile($log_file_name, 20)."\n";
      }
      $tinfo->{'timeout'}= testcase_timeout($tinfo); # Mark as timeout
      run_on_all($tinfo, 'analyze-timeout');

      report_failure_and_restart($tinfo);
      return 1;
    }

    mtr_error("Unhandled process $proc exited");
  }
  mtr_error("Should never come here");
}


# Keep track of last position in mysqld error log where we scanned for
# warnings, so we can attribute any warnings found to the correct test
# suite or server restart.
our $last_warning_position= { };

# Called just before a mysqld server is started or a testcase is run,
# to keep track of which tests have been run since last restart, and
# of when the error log is reset.
#
# Second argument $test_name is test name, or undef for server restart.
sub pre_write_errorlog {
  my ($error_log, $test_name)= @_;

  if (! -e $error_log) {
    # If the error log is moved away, reset the warning parse position.
    delete $last_warning_position->{$error_log};
  }

  if (defined($test_name)) {
    $last_warning_position->{$error_log}{test_names}= []
      unless exists($last_warning_position->{$error_log}{test_names});
    push @{$last_warning_position->{$error_log}{test_names}}, $test_name;
  } else {
    # Server restart, so clear the list of tests run since last restart.
    # (except the last one (if any), which is the test about to be run).
    if (defined($last_warning_position->{$error_log}{test_names}) &&
        @{$last_warning_position->{$error_log}{test_names}}) {
      $last_warning_position->{$error_log}{test_names}=
        [$last_warning_position->{$error_log}{test_names}[-1]];
    } else {
      $last_warning_position->{$error_log}{test_names}= [];
    }
  }
}

# Extract server log from after the last occurrence of named test
# Return as an array of lines
#

sub extract_server_log ($$) {
  my ($error_log, $tname) = @_;
  
  return unless $error_log;

  # Open the servers .err log file and read all lines
  # belonging to current test into @lines
  my $Ferr = IO::File->new($error_log)
    or mtr_error("Could not open file '$error_log' for reading: $!");

  my @lines;
  my $found_test= 0;		# Set once we've found the log of this test
  while ( my $line = <$Ferr> )
  {
    if ($found_test)
    {
      # If test wasn't last after all, discard what we found, test again.
      if ( $line =~ /^CURRENT_TEST:/)
      {
	@lines= ();
	$found_test= $line =~ /^CURRENT_TEST: $tname/;
      }
      else
      {
	push(@lines, $line);
	if (scalar(@lines) > 1000000) {
	  $Ferr = undef;
	  mtr_warning("Too much log in $error_log, bailing out from extracting");
	  return ();
	}
      }
    }
    else
    {
      # Search for beginning of test, until found
      $found_test= 1 if ($line =~ /^CURRENT_TEST: $tname/);
    }
  }
  $Ferr = undef; # Close error log file

  return @lines;
}

# Get log from server identified from its $proc object, from named test
# Return as a single string
#

sub get_log_from_proc ($$) {
  my ($proc, $name)= @_;
  my $srv_log= "";

  foreach my $mysqld (all_servers()) {
    if ($mysqld->{proc} eq $proc) {
      my @srv_lines= extract_server_log($mysqld->if_exist('log-error'), $name);
      $srv_log= "\nServer log from this test:\n" .
	"----------SERVER LOG START-----------\n". join ("", @srv_lines) .
	"----------SERVER LOG END-------------\n";
      last;
    }
  }
  return $srv_log;
}

#
# Perform a rough examination of the servers
# error log and write all lines that look
# suspicious into $error_log.warnings
#

sub extract_warning_lines ($$) {
  my ($error_log, $append) = @_;

  # Open the servers .err log file and read all lines
  # belonging to current tets into @lines
  my $Ferr = IO::File->new($error_log)
    or return [];
  my $last_pos= $last_warning_position->{$error_log}{seek_pos};
  $Ferr->seek($last_pos, 0) if defined($last_pos);
  # If the seek fails, we will parse the whole error log, at least we will not
  # miss any warnings.

  my @lines= <$Ferr>;
  $last_warning_position->{$error_log}{seek_pos}= $Ferr->tell();
  $Ferr = undef; # Close error log file

  # mysql_client_test.test sends a COM_DEBUG packet to the server
  # to provoke a safemalloc leak report, ignore any warnings
  # between "Begin/end safemalloc memory dump"
  if ( grep(/Begin safemalloc memory dump:/, @lines) > 0)
  {
    my $discard_lines= 1;
    foreach my $line ( @lines )
    {
      if ($line =~ /Begin safemalloc memory dump:/){
	$discard_lines = 1;
      } elsif ($line =~ /End safemalloc memory dump./){
	$discard_lines = 0;
      }

      if ($discard_lines){
	$line = "ignored";
      }
    }
  }

  # Write all suspicious lines to $error_log.warnings file
  my $warning_log = "$error_log.warnings";
  my $Fwarn = IO::File->new($warning_log, $append ? "a+" : "w")
    or die("Could not open file '$warning_log' for writing: $!");
  if (!$append)
  {
    print $Fwarn "Suspicious lines from $error_log\n";
  }

  my @patterns =
    (
     qr/^Warning|(mysqld|mariadbd): Warning|\[Warning\]/,
     qr/^Error:|\[ERROR\]/,
     qr/^==\d+==\s+\S/, # valgrind errors
     qr/InnoDB: Warning|InnoDB: Error/,
     qr/^safe_mutex:|allocated at line/,
     qr/missing DBUG_RETURN/,
     qr/Attempting backtrace/,
     qr/Assertion .* failed/,
     qr/Sanitizer/,
     qr/runtime error:/,
    );
  # These are taken from the include/mtr_warnings.sql global suppression
  # list. They occur delayed, so they can be parsed during shutdown rather
  # than during the per-test check.
  #
  # ToDo: having the warning suppressions inside the mysqld we are trying to
  # check is in any case horrible. We should change it to all be done here
  # within the Perl code, which is both simpler, easier, faster, and more
  # robust. We could still have individual test cases put in suppressions by
  # parsing statically or by writing dynamically to a CSV table read by the
  # Perl code.
  my @antipatterns =
    (
     @global_suppressions,
     qr/error .*connecting to master/,
     qr/InnoDB: Dumping buffer pool.*/,
     qr/InnoDB: Buffer pool.*/,
     qr/InnoDB: Could not free any blocks in the buffer pool!/,
     qr/InnoDB: innodb_open_files .* should not be greater than/,
     qr/InnoDB: Trying to delete tablespace.*but there are.*pending/,
     qr/InnoDB: Tablespace 1[0-9]* was not found at .*, and innodb_force_recovery was set/,
     qr/InnoDB: Long wait \([0-9]+ seconds\) for double-write buffer flush/,
     qr/Slave: Unknown table 't1' .* 1051/,
     qr/Slave SQL:.*(Internal MariaDB error code: [[:digit:]]+|Query:.*)/,
     qr/slave SQL thread aborted/,
     qr/unknown option '--loose[-_]/,
     qr/unknown variable 'loose[-_]/,
     #qr/Invalid .*old.* table or database name/,
     qr/Now setting lower_case_table_names to [02]/,
     qr/Setting lower_case_table_names=2/,
     qr/You have forced lower_case_table_names to 0/,
     qr/deprecated/,
     qr/Slave SQL thread retried transaction/,
     qr/Slave \(additional info\)/,
     qr/Incorrect information in file/,
     qr/Simulating error for/,
     qr/Slave I\/O: Get master SERVER_ID failed with error:.*/,
     qr/Slave I\/O: Get master clock failed with error:.*/,
     qr/Slave I\/O: Get master COLLATION_SERVER failed with error:.*/,
     qr/Slave I\/O: Get master TIME_ZONE failed with error:.*/,
     qr/Slave I\/O: Get master \@\@GLOBAL.gtid_domain_id failed with error:.*/,
     qr/Slave I\/O: Setting \@slave_connect_state failed with error:.*/,
     qr/Slave I\/O: Setting \@slave_gtid_strict_mode failed with error:.*/,
     qr/Slave I\/O: Setting \@slave_gtid_ignore_duplicates failed with error:.*/,
     qr/Slave I\/O: Setting \@slave_until_gtid failed with error:.*/,
     qr/Slave I\/O: Get master GTID position failed with error:.*/,
     qr/Slave I\/O: error reconnecting to master '.*' - retry-time: [1-3]  retries/,
     qr/Slave I\/0: Master command COM_BINLOG_DUMP failed/,
     qr/Error reading packet/,
     qr/Lost connection to MariaDB server at 'reading initial communication packet'/,
     qr/Could not read packet:.* state: [2-3] /,
     qr/Could not read packet:.* errno: 104 /,
     qr/Could not read packet:.* errno: 0 .* length: 0/,
     qr/Could not write packet:.* errno: 32 /,
     qr/Could not write packet:.* errno: 104 /,
     qr/Semisync ack receiver got error 1158/,
     qr/Semisync ack receiver got hangup/,
     qr/Connection was killed/,
     qr/Failed on request_dump/,
     qr/Slave: Can't drop database.* database doesn't exist/,
     qr/Slave: Operation DROP USER failed for 'create_rout_db'/,
     qr|Checking table:   '\..mtr.test_suppressions'|,
     qr|Table \./test/bug53592 has a primary key in InnoDB data dictionary, but not in|,
     qr|Table '\..mtr.test_suppressions' is marked as crashed and should be repaired|,
     qr|Table 'test_suppressions' is marked as crashed and should be repaired|,
     qr|Can't open shared library|,
     qr|Couldn't load plugin named .*EXAMPLE.*|,
     qr|InnoDB: Error: table 'test/bug39438'|,
     qr| entry '.*' ignored in --skip-name-resolve mode|,
     qr|mysqld got signal 6|,
     qr|Error while setting value 'pool-of-threads' to 'thread_handling'|,
     qr|Access denied for user|,
     qr|Aborted connection|,
     qr|table.*is full|,
     qr/\[ERROR\] (mysqld|mariadbd): \Z/,  # Warning from Aria recovery
     qr|Linux Native AIO|, # warning that aio does not work on /dev/shm
     qr|InnoDB: io_setup\(\) attempt|,
     qr|InnoDB: io_setup\(\) failed with EAGAIN|,
     qr|io_uring_queue_init\(\) failed with|,
     qr|InnoDB: liburing disabled|,
     qr/InnoDB: Failed to set O_DIRECT on file/,
     qr|setrlimit could not change the size of core files to 'infinity';|,
     qr|failed to retrieve the MAC address|,
     qr|Plugin 'FEEDBACK' init function returned error|,
     qr|Plugin 'FEEDBACK' registration as a INFORMATION SCHEMA failed|,
     qr|'log-bin-use-v1-row-events' is MySQL .* compatible option|,
     qr|InnoDB: Setting thread \d+ nice to \d+ failed, current nice \d+, errno 13|, # setpriority() fails under valgrind
     qr|Failed to setup SSL|,
     qr|SSL error: Failed to set ciphers to use|,
     qr/Plugin 'InnoDB' will be forced to shutdown/,
     qr|Could not increase number of max_open_files to more than|,
     qr|Changed limits: max_open_files|,
     qr/InnoDB: Error table encrypted but encryption service not available.*/,
     qr/InnoDB: Could not find a valid tablespace file for*/,
     qr/InnoDB: Tablespace open failed for*/,
     qr/InnoDB: Failed to find tablespace for table*/,
     qr/InnoDB: Space */,
     qr|InnoDB: You may have to recover from a backup|,
     qr|InnoDB: It is also possible that your operatingsystem has corrupted its own file cache|,
     qr|InnoDB: and rebooting your computer removes the error|,
     qr|InnoDB: If the corrupt page is an index page you can also try to|,
     qr|nnoDB: fix the corruption by dumping, dropping, and reimporting|,
     qr|InnoDB: the corrupt table. You can use CHECK|,
     qr|InnoDB: TABLE to scan your table for corruption|,
     qr/InnoDB: See also */,
     qr/InnoDB: Cannot open .*ib_buffer_pool.* for reading: No such file or directory*/,
     qr/InnoDB: Table .*mysql.*innodb_table_stats.* not found./,
     qr/InnoDB: User stopword table .* does not exist./,
     qr/Dump thread [0-9]+ last sent to server [0-9]+ binlog file:pos .+/,
     qr/Detected table cache mutex contention at instance .* waits. Additional table cache instance cannot be activated: consider raising table_open_cache_instances. Number of active instances/,
     qr/WSREP: Failed to guess base node address/,
     qr/WSREP: Guessing address for incoming client/,

     qr/InnoDB: Difficult to find free blocks in the buffer pool*/,
     # Disable test for UBSAN on dynamically loaded objects
     qr/runtime error: member call.*object.*'Handler_share'/,
     qr/sql_type\.cc.* runtime error: member call.*object.* 'Type_collection'/,
    );

  my $matched_lines= [];
  LINE: foreach my $line ( @lines )
  {
    PAT: foreach my $pat ( @patterns )
    {
      if ( $line =~ /$pat/ )
      {
        foreach my $apat (@antipatterns)
        {
          next LINE if $line =~ $apat;
        }
	print $Fwarn $line;
        push @$matched_lines, $line;
	last PAT;
      }
    }
  }
  $Fwarn = undef; # Close file

  if (scalar(@$matched_lines) > 0 &&
      defined($last_warning_position->{$error_log}{test_names})) {
    return ($last_warning_position->{$error_log}{test_names}, $matched_lines);
  } else {
    return ([], $matched_lines);
  }
}


# Run include/check-warnings.test
#
# RETURN VALUE
#  0 OK
#  1 Check failed
#
sub start_check_warnings ($$) {
  my $tinfo=    shift;
  my $mysqld=   shift;

  my $name= "warnings-".$mysqld->name();

  my $log_error= $mysqld->value('log-error');
  # To be communicated to the test
  $ENV{MTR_LOG_ERROR}= $log_error;
  extract_warning_lines($log_error, 0);

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--defaults-group-suffix=%s", $mysqld->after('mysqld'));
  mtr_add_arg($args, "--test-file=%s", "include/check-warnings.test");

  if ( $opt_embedded_server )
  {

    # Get the args needed for the embedded server
    # and append them to args prefixed
    # with --sever-arg=

    my $mysqld=  $config->group('embedded')
      or mtr_error("Could not get [embedded] section");

    my $mysqld_args;
    mtr_init_args(\$mysqld_args);
    my $extra_opts= get_extra_opts($mysqld, $tinfo);
    mysqld_arguments($mysqld_args, $mysqld, $extra_opts);
    mtr_add_arg($args, "--server-arg=%s", $_) for @$mysqld_args;
  }

  my $errfile= "$opt_vardir/tmp/$name.err";
  my $proc= My::SafeProcess->new
    (
     name          => $name,
     path          => $exe_mysqltest,
     error         => $errfile,
     output        => $errfile,
     args          => \$args,
     user_data     => [$errfile, $mysqld],
     verbose       => $opt_verbose,
    );
  mtr_verbose("Started $proc");
  return $proc;
}


#
# Loop through our list of processes and check the error log
# for unexpected errors and warnings
#
sub check_warnings ($) {
  my ($tinfo)= @_;
  my $res= 0;

  my $tname= $tinfo->{name};

  # Clear previous warnings
  delete($tinfo->{warnings});

  # Start the mysqltest processes in parallel to save time
  # also makes it possible to wait for any process to exit during the check
  my %started;
  foreach my $mysqld ( mysqlds() )
  {
    if ( defined $mysqld->{'proc'} )
    {
      my $proc= start_check_warnings($tinfo, $mysqld);
      $started{$proc->pid()}= $proc;
    }
  }

  # Return immediately if no check proceess was started
  return 0 unless ( keys %started );

  my $timeout= start_timer(check_timeout($tinfo));

  my $result= 0;
  while (1){
    my $proc= My::SafeProcess->wait_any_timeout($timeout);
    mtr_report("Got $proc");

    if ( delete $started{$proc->pid()} ) {
      # One check warning process returned
      my $res= $proc->exit_status();
      my ($err_file, $mysqld)= @{$proc->user_data()};

      if ( $res == 0 or $res == 62 ){

	if ( $res == 0 ) {
	  # Check completed with problem
	  my $report= mtr_grab_file($err_file);
	  # Log to var/log/warnings file
	  mtr_tofile("$opt_vardir/log/warnings",
		     $tname."\n".$report);

	  $tinfo->{'warnings'}.= $report;
	  $result= 1;
	}

	if ( $res == 62 ) {
	  # Test case was ok and called "skip"
	  # Remove the .err file the check generated
	  unlink($err_file);
	}

	if ( keys(%started) == 0){
	  # All checks completed
	  mark_time_used('ch-warn');
	  return $result;
	}
	# Wait for next process to exit
	next;
      }
      else
      {
	my $report= mtr_grab_file($err_file);
	$tinfo->{comment}.=
	  "Could not execute 'check-warnings' for ".
	    "testcase '$tname' (res: $res) server: '".
              $mysqld->name() ."':\n";
	$tinfo->{comment}.= $report;

	$result= 2;
      }
    }
    elsif ( $proc->{timeout} ) {
      $tinfo->{comment}.= "Timeout for 'check warnings' expired after "
	.check_timeout($tinfo)." seconds";
      $result= 4;
    }
    else {
      # Unknown process returned, most likley a crash, abort everything
      $tinfo->{comment}=
	"The server $proc crashed while running 'check warnings'".
	get_log_from_proc($proc, $tinfo->{name});
      $result= 3;
    }

    # Kill any check processes still running
    map($_->kill(), values(%started));

    mark_time_used('ch-warn');
    return $result;
  }

  mtr_error("INTERNAL_ERROR: check_warnings");
}

# Check for warnings generated during shutdown of a mysqld server.
# If any, report them to master server, and return true; else just return
# false.

sub check_warnings_post_shutdown {
  my ($server_socket)= @_;
  my $testname_hash= { };
  my $report= '';
  foreach my $mysqld ( mysqlds())
  {
    my ($testlist, $match_lines)=
        extract_warning_lines($mysqld->value('log-error'), 1);
    $testname_hash->{$_}= 1 for @$testlist;
    $report.= join('', @$match_lines);
  }
  my @warning_tests= keys(%$testname_hash);
  if (@warning_tests) {
    my $fake_test= My::Test->new(testnames => \@warning_tests);
    $fake_test->{'warnings'}= $report;
    $fake_test->write_test($server_socket, 'WARNINGS');
  }
}

#
# Check for the file indicating expected crash and restart it.
#
sub check_expected_crash_and_restart {
  my $mysqld = shift;

  # Check if crash expected by looking at the .expect file
  # in var/tmp
  my $expect_file= "$opt_vardir/tmp/".$mysqld->name().".expect";
  if ( -f $expect_file )
  {
    mtr_verbose("Crash was expected, file '$expect_file' exists");

    for (my $waits = 0;  $waits < 50;  mtr_milli_sleep(100), $waits++)
    {
      # Race condition seen on Windows: try again until file not empty
      next if -z $expect_file;
      # If last line in expect file starts with "wait"
      # sleep a little and try again, thus allowing the
      # test script to control when the server should start
      # up again. Keep trying for up to 5s at a time.
      my $last_line= mtr_lastlinesfromfile($expect_file, 1);
      if ($last_line =~ /^wait/ )
      {
        mtr_verbose("Test says wait before restart") if $waits == 0;
        next;
      }
      delete $ENV{MTR_BINDIR_FORCED};

      # Ignore any partial or unknown command
      next unless $last_line =~ /^restart/;
      # If last line begins "restart:", the rest of the line is read as
      # extra command line options to add to the restarted mysqld.
      # Anything other than 'wait' or 'restart:' (with a colon) will
      # result in a restart with original mysqld options.
      if ($last_line =~ /restart_bindir\s+(\S+)(:.+)?/) {
        $ENV{MTR_BINDIR_FORCED}= $1;
        if ($2) {
          my @rest_opt= split(' ', $2);
          $mysqld->{'restart_opts'}= \@rest_opt;
        }
      } elsif ($last_line =~ /restart:(.+)/) {
        my @rest_opt= split(' ', $1);
        $mysqld->{'restart_opts'}= \@rest_opt;
      } else {
        delete $mysqld->{'restart_opts'};
      }
      unlink($expect_file);

      # Start server with same settings as last time
      return mysqld_start($mysqld, $mysqld->{'started_opts'});
    }
    # Loop ran through: we should keep waiting after a re-check
    return 2;
  }

  # Not an expected crash
  return 0;
}


# Remove all files and subdirectories of a directory
sub clean_dir {
  my ($dir)= @_;
  mtr_verbose("clean_dir: $dir");
  finddepth(
	  { no_chdir => 1,
	    wanted => sub {
	      if (-d $_){
		# A dir
		if ($_ eq $dir){
		  # The dir to clean
		  return;
		} else {
		  mtr_verbose("rmdir: '$_'");
		  rmdir($_) or mtr_warning("rmdir($_) failed: $!");
		}
	      } else {
		# Hopefully a file
		mtr_verbose("unlink: '$_'");
		unlink($_) or mtr_warning("unlink($_) failed: $!");
	      }
	    }
	  },
	    $dir);
}


sub clean_datadir {
  mtr_verbose("Cleaning datadirs...");

  if (started(all_servers()) != 0){
    mtr_error("Trying to clean datadir before all servers stopped");
  }

  for (all_servers())
  {
    my $dir= "$opt_vardir/".$_->{name};
    mtr_verbose(" - removing '$dir'");
    rmtree($dir);
  }

  # Remove all files in tmp and var/tmp
  clean_dir("$opt_vardir/tmp");
  if ($opt_tmpdir ne "$opt_vardir/tmp"){
    clean_dir($opt_tmpdir);
  }
}


#
# Save datadir before it's removed
#
sub save_datadir_after_failure($$) {
  my ($dir, $savedir)= @_;

  mtr_report(" - saving '$dir'");
  my $dir_name= basename($dir);
  rename("$dir", "$savedir/$dir_name");
}


sub after_failure ($) {
  my ($tinfo)= @_;

  mtr_report("Saving datadirs...");

  my $save_dir= "$opt_vardir/log/";
  $save_dir.= $tinfo->{name};
  # Add combination name if any
  $save_dir.= '-' . join(',', sort @{$tinfo->{combinations}})
    if defined $tinfo->{combinations};

  # Save savedir  path for server
  $tinfo->{savedir}= $save_dir;

  mkpath($save_dir) if ! -d $save_dir;

  #
  # Create a log of files in vardir (good for buildbot)
  #
  if (!IS_WINDOWS)
  {
    my $Flog= IO::File->new("$opt_vardir/log/files.log", "w");
    if ($Flog)
    {
      print $Flog scalar(`/bin/ls -Rl $opt_vardir/*`);
      close($Flog);
    }
  }

  # Save the used config files
  my %config_files = config_files($tinfo);
  while (my ($file, $generate) = each %config_files) {
    copy("$opt_vardir/$file", $save_dir);
  }

  # Copy the tmp dir
  copytree("$opt_vardir/tmp/", "$save_dir/tmp/");

  foreach (all_servers()) {
    my $dir= "$opt_vardir/".$_->{name};
    save_datadir_after_failure($dir, $save_dir);
  }
}


sub report_failure_and_restart ($) {
  my $tinfo= shift;

  if ($opt_valgrind && ($tinfo->{'warnings'} || $tinfo->{'timeout'}) &&
      $opt_core_on_failure == 0)
  {
    # In these cases we may want valgrind report from normal termination
    $tinfo->{'dont_kill_server'}= 1;
  }
  # Shutdown properly if not to be killed (for valgrind)
  stop_all_servers($tinfo->{'dont_kill_server'} ? $opt_shutdown_timeout : 0);

  $tinfo->{'result'}= 'MTR_RES_FAILED';

  my $test_failures= $tinfo->{'failures'} || 0;
  $tinfo->{'failures'}=  $test_failures + 1;


  if ( $tinfo->{comment} )
  {
    # The test failure has been detected by mysql-test-run.pl
    # when starting the servers or due to other error, the reason for
    # failing the test is saved in "comment"
    ;
  }

  if ( !defined $tinfo->{logfile} )
  {
    my $logfile= $path_current_testlog;
    if ( defined $logfile )
    {
      if ( -f $logfile )
      {
	# Test failure was detected by test tool and its report
	# about what failed has been saved to file. Save the report
	# in tinfo
	$tinfo->{logfile}= mtr_fromfile($logfile);
	# If no newlines in the test log:
	# (it will contain the CURRENT_TEST written by mtr, so is not empty)
	if ($tinfo->{logfile} !~ /\n/)
	{
	  # Show how far it got before suddenly failing
	  $tinfo->{comment}.= "mysqltest failed but provided no output\n";
	  my $log_file_name= $opt_vardir."/log/".$tinfo->{shortname}.".log";
	  if (-e $log_file_name) {
	    $tinfo->{comment}.=
	      "The result from queries just before the failure was:".
	      "\n< snip >\n".
	      mtr_lastlinesfromfile($log_file_name, $opt_tail_lines)."\n";
	  }
	}
      }
      else
      {
	# The test tool report didn't exist, display an
	# error message
	$tinfo->{logfile}= "Could not open test tool report '$logfile'";
      }
    }
  }

  after_failure($tinfo);

  mtr_report_test($tinfo);

}


sub run_system(@) {
  mtr_verbose("Running '$_[0]'");
  my $ret= system(@_) >> 8;
  return $ret;
}


sub mysqld_stop {
  my $mysqld= shift or die "usage: mysqld_stop(<mysqld>)";

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--no-defaults");
  mtr_add_arg($args, "--character-sets-dir=%s", $mysqld->value('character-sets-dir'));
  mtr_add_arg($args, "--user=%s", $opt_user);
  mtr_add_arg($args, "--password=");
  mtr_add_arg($args, "--port=%d", $mysqld->value('port'));
  mtr_add_arg($args, "--host=%s", $mysqld->value('#host'));
  mtr_add_arg($args, "--connect_timeout=20");
  mtr_add_arg($args, "--protocol=tcp");

  mtr_add_arg($args, "shutdown");

  My::SafeProcess->run
    (
     name          => "mysqladmin shutdown ".$mysqld->name(),
     path          => $exe_mysqladmin,
     args          => \$args,
     error         => "$opt_vardir/log/mysqladmin.err",

    );
}


sub mysqld_arguments ($$$) {
  my $args=              shift;
  my $mysqld=            shift;
  my $extra_opts=        shift;

  my @defaults = grep(/^--defaults-file=/, @$extra_opts);
  if (@defaults > 0) {
    mtr_add_arg($args, pop(@defaults))
  }
  else {
    mtr_add_arg($args, "--defaults-file=%s",  $path_config_file);
  }

  # When mysqld is run by a root user(euid is 0), it will fail
  # to start unless we specify what user to run as, see BUG#30630
  my $euid= $>;
  if (!IS_WINDOWS and $euid == 0 and
      (grep(/^--user/, @$extra_opts)) == 0) {
    mtr_add_arg($args, "--user=root");
  }

  if (!using_extern() and !$opt_user_args)
  {
    # Turn on logging to file
    mtr_add_arg($args, "%s--log-output=file");
  }

  # Check if "extra_opt" contains --log-bin
  my $skip_binlog= not grep /^--(loose-)?log-bin/, @$extra_opts;

  my $found_skip_core= 0;
  foreach my $arg ( @$extra_opts )
  {
    # Skip --defaults-file option since it's handled above.
    next if $arg =~ /^--defaults-file/;

    # Allow --skip-core-file to be set in <testname>-[master|slave].opt file
    if ($arg eq "--skip-core-file")
    {
      $found_skip_core= 1;
    }
    elsif ($skip_binlog and mtr_match_prefix($arg, "--binlog-format"))
    {
      ; # Dont add --binlog-format when running without binlog
    }
    elsif ($arg eq "--loose-skip-log-bin" and
           $mysqld->option("log-slave-updates"))
    {
      ; # Dont add --skip-log-bin when mysqld have --log-slave-updates in config
    }
    else
    {
      mtr_add_arg($args, "%s", $arg);
    }
  }
  $opt_skip_core = $found_skip_core;
  if ( !$found_skip_core && !$opt_user_args )
  {
    mtr_add_arg($args, "%s", "--core-file");
  }

  # Enable the debug sync facility, set default wait timeout.
  # Facility stays disabled if timeout value is zero.
  mtr_add_arg($args, "--loose-debug-sync-timeout=%s",
              $opt_debug_sync_timeout) unless $opt_user_args;

  return $args;
}



sub mysqld_start ($$) {
  my $mysqld=            shift;
  my $extra_opts=        shift;

  mtr_verbose(My::Options::toStr("mysqld_start", @$extra_opts));

  my $exe= find_mysqld($mysqld->value('basedir'));

  mtr_error("Internal error: mysqld should never be started for embedded")
    if $opt_embedded_server;

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--defaults-group-suffix=%s", $mysqld->after('mysqld'));

  # Add any additional options from an in-test restart
  my @all_opts= @$extra_opts;
  if (exists $mysqld->{'restart_opts'}) {
    push (@all_opts, @{$mysqld->{'restart_opts'}});
    mtr_verbose(My::Options::toStr("mysqld_start restart",
				   @{$mysqld->{'restart_opts'}}));
  }
  mysqld_arguments($args,$mysqld,\@all_opts);

  if ( $opt_debug )
  {
    mtr_add_arg($args, "--debug-dbug=$debug_d:t:i:A,%s/log/%s.trace",
		$path_vardir_trace, $mysqld->name());
  }


  # Command line for mysqld started for *this particular test*.
  # Differs from "generic" MYSQLD_CMD by including all command line
  # options from *.opt and *.combination files.
  $ENV{'MYSQLD_LAST_CMD'}= "$exe  @$args";
  my $oldexe= $exe;

  My::Debugger::setup_args(\$args, \$exe, $mysqld->name());
  $ENV{'VALGRIND_TEST'}= $opt_valgrind = int(($exe || '') eq 'valgrind');

  # Remove the old pidfile if any
  unlink($mysqld->value('pid-file'));

  my $output= $mysqld->value('log-error');

  if ( $opt_valgrind and $opt_debug )
  {
    # When both --valgrind and --debug is selected, send
    # all output to the trace file, making it possible to
    # see the exact location where valgrind complains

    # Write a message about this to the normal log file
    my $trace_name= "$opt_vardir/log/".$mysqld->name().".trace";
    mtr_tofile($output,
               "NOTE: When running with --valgrind --debug the output from ",
	       "mysqld (where the valgrind messages shows up) is stored ",
	       "together with the trace file to make it ",
	       "easier to find the exact position of valgrind errors.",
	       "See trace file $trace_name.\n");
    $output= $trace_name;

  }
  # Remember this log file for valgrind error report search
  $mysqld_logs{$output}= 1 if $opt_valgrind;
  # Remember data dir for gmon.out files if using gprof
  $gprof_dirs{$mysqld->value('datadir')}= 1 if $opt_gprof;

  if ( defined $exe )
  {
    mtr_tofile($output, "\$ $exe @$args\n");
    pre_write_errorlog($output);
    $mysqld->{'proc'}= My::SafeProcess->new
      (
       name          => $mysqld->name(),
       path          => $exe,
       args          => \$args,
       output        => $output,
       error         => $output,
       append        => 1,
       verbose       => $opt_verbose,
       nocore        => $opt_skip_core,
       host          => undef,
       shutdown      => sub { mysqld_stop($mysqld) },
       envs          => \@opt_mysqld_envs,
      );
    mtr_verbose("Started $mysqld->{proc}");
  }

  $mysqld->{'started_opts'}= $extra_opts;

  my $expect_file= "$opt_vardir/tmp/".$mysqld->name().".expect";
  my $rc= $oldexe eq ($exe || '') ||
         sleep_until_file_created($mysqld->value('pid-file'), $expect_file,
           $opt_start_timeout, $mysqld->{'proc'}, $warn_seconds);
  if (!$rc)
  {
    # Report failure about the last test case before exit
    my $test_name= mtr_grab_file($path_current_testlog);
    $test_name =~ s/^CURRENT_TEST:\s//;
    my $tinfo = My::Test->new(name => $test_name);
    $tinfo->{result}= 'MTR_RES_FAILED';
    $tinfo->{failures}= 1;
    $tinfo->{logfile}=get_log_from_proc($mysqld->{'proc'}, $tinfo->{name});
    report_option('verbose', 1);
    mtr_report_test($tinfo);
  }
  return $rc;
}


sub stop_all_servers () {
  my $shutdown_timeout = $_[0] or 0;

  mtr_verbose("Stopping all servers...");

  # Kill all started servers
  My::SafeProcess::shutdown($shutdown_timeout,
			    started(all_servers()));

  # Remove pidfiles
  foreach my $server ( all_servers() )
  {
    my $pid_file= $server->if_exist('pid-file');
    unlink($pid_file) if defined $pid_file;
  }

  # Mark servers as stopped
  map($_->{proc}= undef, all_servers());

}


# Find out if server should be restarted for this test
sub server_need_restart {
  my ($tinfo, $server)= @_;

  if ( using_extern() )
  {
    mtr_verbose_restart($server, "no restart for --extern server");
    return 0;
  }

  if ( $tinfo->{'force_restart'} ) {
    mtr_verbose_restart($server, "forced in .opt file");
    return 1;
  }

  if ( $opt_force_restart ) {
    mtr_verbose_restart($server, "forced restart turned on");
    return 1;
  }

  if ( $tinfo->{template_path} ne $current_config_name)
  {
    mtr_verbose_restart($server, "using different config file");
    return 1;
  }

  if ( $tinfo->{'master_sh'}  || $tinfo->{'slave_sh'} )
  {
    mtr_verbose_restart($server, "sh script to run");
    return 1;
  }

  if ( ! started($server) )
  {
    mtr_verbose_restart($server, "not started");
    return 1;
  }

  my $started_tinfo= $server->{'started_tinfo'};
  if ( defined $started_tinfo )
  {

    # Check if timezone of  test that server was started
    # with differs from timezone of next test
    if ( timezone($started_tinfo) ne timezone($tinfo) )
    {
      mtr_verbose_restart($server, "different timezone");
      return 1;
    }
  }

  if ($server->name() =~ /^mysqld\./)
  {

    # Check that running process was started with same options
    # as the current test requires
    my $extra_opts= get_extra_opts($server, $tinfo);
    my $started_opts= $server->{'started_opts'};

    # Also, always restart if server had been restarted with additional
    # options within test.
    if (!My::Options::same($started_opts, $extra_opts) ||
        exists $server->{'restart_opts'})
    {
      delete $server->{'restart_opts'};
      my $use_dynamic_option_switch= 0;
      my $restart_opts = delete $server->{'restart_opts'} || [];
      if (!$use_dynamic_option_switch)
      {
	mtr_verbose_restart($server, "running with different options '" .
			    join(" ", @{$extra_opts}) . "' != '" .
			    join(" ", @{$started_opts}, @{$restart_opts}) . "'" );
	return 1;
      }

      mtr_verbose(My::Options::toStr("started_opts", @$started_opts));
      mtr_verbose(My::Options::toStr("extra_opts", @$extra_opts));

      # Get diff and check if dynamic switch is possible
      my @diff_opts= My::Options::diff($started_opts, $extra_opts);
      mtr_verbose(My::Options::toStr("diff_opts", @diff_opts));

      my $query= My::Options::toSQL(@diff_opts);
      mtr_verbose("Attempting dynamic switch '$query'");
      if (run_query($tinfo, $server, $query)){
	mtr_verbose("Restart: running with different options '" .
		    join(" ", @{$extra_opts}) . "' != '" .
		    join(" ", @{$started_opts}) . "'" );
	return 1;
      }

      # Remember the dynamically set options
      $server->{'started_opts'}= $extra_opts;
    }
  }

  # Default, no restart
  return 0;
}


sub servers_need_restart($) {
  my ($tinfo)= @_;
  return grep { server_need_restart($tinfo, $_); } all_servers();
}



############################################

############################################

#
# Filter a list of servers and return the SafeProcess
# for only those that are started or stopped
#
sub started { return grep(defined $_, map($_->{proc}, @_));  }
sub stopped { return grep(!defined $_, map($_->{proc}, @_)); }


sub get_extra_opts {
  # No extra options if --user-args
  return \@opt_extra_mysqld_opt if $opt_user_args;

  my ($mysqld, $tinfo)= @_;

  my $opts=
    $mysqld->option("#!use-slave-opt") ?
      $tinfo->{slave_opt} : $tinfo->{master_opt};

  # Expand environment variables
  foreach my $opt ( @$opts )
  {
    no warnings 'uninitialized';
    $opt =~ s/\$\{(\w+)\}/$ENV{$1}/ge;
    $opt =~ s/\$(\w+)/$ENV{$1}/ge;
  }
  return $opts;
}


sub stop_servers($$) {
  my (@servers)= @_;

  mtr_report("Stopping ", started(@servers));

  My::SafeProcess::shutdown($opt_shutdown_timeout,
                             started(@servers));

  foreach my $server (@servers)
  {
    # Mark server as stopped
    $server->{proc}= undef;

    # Forget history
    delete $server->{'started_tinfo'};
    delete $server->{'started_opts'};
    delete $server->{'started_cnf'};
  }
}

#
# run_query_output
#
# Run a query against a server using mysql client. The output of
# the query will be written into outfile.
#
sub run_query_output {
  my ($mysqld, $query, $outfile)= @_;
  my $args;

  mtr_init_args(\$args);
  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--defaults-group-suffix=%s", $mysqld->after('mysqld'));
  mtr_add_arg($args, "--silent");
  mtr_add_arg($args, "--execute=%s", $query);

  my $res= My::SafeProcess->run
  (
    name          => "run_query_output -> ".$mysqld->name(),
    path          => $exe_mysql,
    args          => \$args,
    output        => $outfile,
    error         => $outfile
  );

  return $res
}


#
# wsrep_wait_ready
#
# Wait until the server has been joined to the cluster and is
# ready for operation.
#
# RETURN
# 1 Server is ready
# 0 Server didn't transition to ready state within start timeout
#
sub wait_wsrep_ready($$) {
  my ($tinfo, $mysqld)= @_;

  my $sleeptime= 100; # Milliseconds
  my $loops= ($opt_start_timeout * 1000) / $sleeptime;

  my $name= $mysqld->name();
  my $outfile= "$opt_vardir/tmp/$name.wsrep_ready";
  my $query= "SET SESSION wsrep_sync_wait = 0;
              SELECT VARIABLE_NAME, VARIABLE_VALUE
              FROM INFORMATION_SCHEMA.GLOBAL_STATUS
              WHERE VARIABLE_NAME = 'wsrep_ready'";

  for (my $loop= 1; $loop <= $loops; $loop++)
  {
    # Careful... if MTR runs with option 'verbose' then the
    # file contains also SafeProcess verbose output
    if (run_query_output($mysqld, $query, $outfile) == 0 &&
        mtr_grab_file($outfile) =~ /WSREP_READY\s+ON/)
    {
      unlink($outfile);
      return 1;
    }
    mtr_milli_sleep($sleeptime);
  }

  $tinfo->{logfile}= "WSREP did not transition to state READY";
  return 0;
}

#
# wsrep_is_bootstrap_server
#
# Check if the server is the first one to be started in the
# cluster.
#
# RETURN
# 1 The server is a bootstrap server
# 0 The server is not a bootstrap server
#
sub wsrep_is_bootstrap_server($) {
  my $mysqld= shift;

  my $cluster_address= $mysqld->if_exist('wsrep-cluster-address') ||
                       $mysqld->if_exist('wsrep_cluster_address');
  if (defined $cluster_address)
  {
    return $cluster_address eq "gcomm://" || $cluster_address eq "'gcomm://'";
  }
  return 0;
}

#
# wsrep_on
#
# Check if wsrep has been enabled for a server.
#
# RETURN
# 1 Wsrep has been enabled
# 0 Wsrep is not enabled
#
sub wsrep_on($) {
  my $mysqld= shift;
  #check if wsrep_on=  is set in configuration
  if ($mysqld->if_exist('wsrep-on')) {
    my $on= "".$mysqld->value('wsrep-on');
    if ($on eq "1" || $on eq "ON") {
      return 1;
    }
  }
  return 0;
}


#
# start_servers
#
# Start servers not already started
#
# RETURN
#  0 OK
#  1 Start failed
#
sub start_servers($) {
  my ($tinfo)= @_;

  for (all_servers()) {
    $_->{START}->($_, $tinfo) if $_->{START};
  }

  for (all_servers()) {
    next unless $_->{WAIT} and started($_);
    if ($_->{WAIT}->($_, $tinfo)) {
      return 1;
    }
  }
  return 0;
}


#
# Run include/check-testcase.test
# Before a testcase, run in record mode and save result file to var/tmp
# After testcase, run and compare with the recorded file, they should be equal!
#
# RETURN VALUE
#  The newly started process
#
sub start_check_testcase ($$$) {
  my $tinfo=    shift;
  my $mode=     shift;
  my $mysqld=   shift;

  my $name= "check-".$mysqld->name();
  # Replace dots in name with underscore to avoid that mysqltest
  # misinterpret's what the filename extension is :(
  $name=~ s/\./_/g;

  my $args;
  mtr_init_args(\$args);

  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  mtr_add_arg($args, "--defaults-group-suffix=%s", $mysqld->after('mysqld'));
  mtr_add_arg($args, "--result-file=%s", "$opt_vardir/tmp/$name.result");
  mtr_add_arg($args, "--test-file=%s", "include/check-testcase.test");
  mtr_add_arg($args, "--verbose");

  if ( $mode eq "before" )
  {
    mtr_add_arg($args, "--record");
  }
  my $errfile= "$opt_vardir/tmp/$name.err";

  my $exe= $exe_mysqltest;
  My::Debugger::setup_client_args(\$args, \$exe);
  my $proc= My::SafeProcess->new
    (
     name          => $name,
     path          => $exe,
     error         => $errfile,
     output        => $errfile,
     args          => \$args,
     user_data     => $errfile,
     verbose       => $opt_verbose,
    );

  mtr_report("Started $proc");
  return $proc;
}


sub run_mysqltest ($) {
  my $proc= start_mysqltest(@_);
  $proc->wait();
}


sub start_mysqltest ($) {
  my ($tinfo)= @_;
  my $exe= $exe_mysqltest;
  my $args;

  mark_time_used('admin');

  mtr_init_args(\$args);

  mtr_add_arg($args, "--defaults-file=%s", $path_config_file);
  if ($opt_verbose > 1) {
    mtr_add_arg($args, "--verbose");
  } else {
    mtr_add_arg($args, "--silent");
  }
  mtr_add_arg($args, "--tmpdir=%s", $opt_tmpdir);
  mtr_add_arg($args, "--character-sets-dir=%s", $path_charsetsdir);
  mtr_add_arg($args, "--logdir=%s/log", $opt_vardir);

  # Log line number and time  for each line in .test file
  mtr_add_arg($args, "--mark-progress")
    if $opt_mark_progress;

  mtr_add_arg($args, "--database=test");

  if ( $opt_ps_protocol )
  {
    mtr_add_arg($args, "--ps-protocol");
  }

  if ( $opt_sp_protocol )
  {
    mtr_add_arg($args, "--sp-protocol");
  }

  if ( $opt_view_protocol )
  {
    mtr_add_arg($args, "--view-protocol");
  }

  if ( $opt_cursor_protocol )
  {
    mtr_add_arg($args, "--cursor-protocol");
  }

  if ( $opt_non_blocking_api )
  {
    mtr_add_arg($args, "--non-blocking-api");
  }

  mtr_add_arg($args, "--timer-file=%s/log/timer", $opt_vardir);

  if ( $opt_compress )
  {
    mtr_add_arg($args, "--compress");
  }

  if ( $opt_sleep )
  {
    mtr_add_arg($args, "--sleep=%d", $opt_sleep);
  }

  if ( $opt_ssl )
  {
    # Turn on SSL for _all_ test cases if option --ssl was used
    mtr_add_arg($args, "--ssl");
  }

  if ( $opt_max_connections ) {
    mtr_add_arg($args, "--max-connections=%d", $opt_max_connections);
  }

  if ( $opt_embedded_server )
  {

    # Get the args needed for the embedded server
    # and append them to args prefixed
    # with --sever-arg=

    my $mysqld=  $config->group('embedded')
      or mtr_error("Could not get [embedded] section");

    my $mysqld_args;
    mtr_init_args(\$mysqld_args);
    my $extra_opts= get_extra_opts($mysqld, $tinfo);
    mysqld_arguments($mysqld_args, $mysqld, $extra_opts);
    mtr_add_arg($args, "--server-arg=%s", $_) for @$mysqld_args;
  }

  # ----------------------------------------------------------------------
  # export MYSQL_TEST variable containing <path>/mysqltest <args>
  # ----------------------------------------------------------------------
  $ENV{'MYSQL_TEST'}= mtr_args2str($exe_mysqltest, @$args);

  if ($opt_force > 1)
  {
    mtr_add_arg($args, "--continue-on-error");
  }

  my $suite = $tinfo->{suite};
  if ($suite->{parent}) {
    mtr_add_arg($args, "--overlay-dir=%s/", $suite->{dir});
    mtr_add_arg($args, "--suite-dir=%s/", $suite->{parent}->{dir});
  } else {
    mtr_add_arg($args, "--suite-dir=%s/", $suite->{dir});
  }

  mtr_add_arg($args, "--test-file=%s", $tinfo->{'path'});

  # Number of lines of resut to include in failure report
  mtr_add_arg($args, "--tail-lines=%d", $opt_tail_lines);

  if ( defined $tinfo->{'result_file'} ) {
    mtr_add_arg($args, "--result-file=%s", $tinfo->{'result_file'});
  }

  mtr_add_arg($args, "--wait-for-pos-timeout=%d", $opt_debug_sync_timeout);

  client_debug_arg($args, "mysqltest");

  if ( $opt_record )
  {
    mtr_add_arg($args, "--record");

    # When recording to a non existing result file
    # the name of that file is in "record_file"
    if ( defined $tinfo->{'record_file'} ) {
      mtr_add_arg($args, "--result-file=%s", $tinfo->{record_file});
    }
  }

  My::Debugger::setup_client_args(\$args, \$exe);

  my $proc= My::SafeProcess->new
    (
     name          => "mysqltest",
     path          => $exe,
     args          => \$args,
     append        => 1,
     error         => $path_current_testlog,
     verbose       => $opt_verbose,
     open_files_limit => $opt_open_files_limit,
    );
  mtr_verbose("Started $proc");
  return $proc;
}

#
# Search server logs for valgrind reports printed at mysqld termination
#
sub valgrind_exit_reports() {
  my $found_err= 0;

  foreach my $log_file (keys %mysqld_logs)
  {
    my @culprits= ();
    my $valgrind_rep= "";
    my $found_report= 0;
    my $err_in_report= 0;

    my $LOGF = IO::File->new($log_file)
      or mtr_error("Could not open file '$log_file' for reading: $!");

    while ( my $line = <$LOGF> )
    {
      if ($line =~ /^CURRENT_TEST: (.+)$/)
      {
        my $testname= $1;
        # If we have a report, report it if needed and start new list of tests
        if ($found_report)
        {
          if ($err_in_report)
          {
            mtr_print ("Valgrind report from $log_file after tests:\n",
                        @culprits);
            mtr_print_line();
            print ("$valgrind_rep\n");
            $found_err= 1;
            $err_in_report= 0;
          }
          # Make ready to collect new report
          @culprits= ();
          $found_report= 0;
          $valgrind_rep= "";
        }
        push (@culprits, $testname);
        next;
      }
      # This line marks the start of a valgrind report
      $found_report= 1 if $line =~ /^==\d+== .* SUMMARY:/;

      if ($found_report) {
        $line=~ s/^==\d+== //;
        $valgrind_rep .= $line;
        $err_in_report= 1 if $line =~ /ERROR SUMMARY: [1-9]/;
        $err_in_report= 1 if $line =~ /definitely lost: [1-9]/;
        $err_in_report= 1 if $line =~ /possibly lost: [1-9]/;
      }
    }

    $LOGF= undef;

    if ($err_in_report) {
      mtr_print ("Valgrind report from $log_file after tests:\n", @culprits);
      mtr_print_line();
      print ("$valgrind_rep\n");
      $found_err= 1;
    }
  }

  return $found_err;
}

#
# Usage
#
sub usage ($) {
  my ($message)= @_;

  if ( $message )
  {
    print STDERR "$message\n";
    print STDERR "For full list of options, use $0 --help\n";
    exit(1);
  }

  local $"= ','; # for @DEFAULT_SUITES below

  print <<HERE . My::Debugger::help() . My::CoreDump::help() . <<HERE;

$0 [ OPTIONS ] [ TESTCASE ]

Where test case can be specified as:

testcase[.test]         Runs the test case named 'testcase' from all suits
path-to-testcase
[suite.]testcase[,combination]

Examples:

alias
main.alias              'main' is the name of the main test suite
rpl.rpl_invoked_features,mix,innodb
suite/rpl/t/rpl.rpl_invoked_features

Options to control what engine/variation to run:

  embedded-server       Use the embedded server, i.e. no mysqld daemons
  ps-protocol           Use the binary protocol between client and server
  cursor-protocol       Use the cursor protocol between client and server
                        (implies --ps-protocol)
  view-protocol         Create a view to execute all non updating queries
  sp-protocol           Create a stored procedure to execute all queries
  non-blocking-api      Use the non-blocking client API
  compress              Use the compressed protocol between client and server
  ssl                   Use ssl protocol between client and server
  skip-ssl              Don't start server with support for ssl connections
  vs-config             Visual Studio configuration used to create executables
                        (default: MTR_VS_CONFIG environment variable)
  parallel=#            How many parallel test should be run
  defaults-file=<config template> Use fixed config template for all
                        tests
  defaults-extra-file=<config template> Extra config template to add to
                        all generated configs
  combination=<opt>     Use at least twice to run tests with specified
                        options to mysqld
  dry-run               Don't run any tests, print the list of tests
                        that were selected for execution

Options to control directories to use
  tmpdir=DIR            The directory where temporary files are stored
                        (default: ./var/tmp).
  vardir=DIR            The directory where files generated from the test run
                        is stored (default: ./var). Specifying a ramdisk or
                        tmpfs will speed up tests.
  mem[=DIR]             Run testsuite in "memory" using tmpfs or ramdisk
                        Attempts to use DIR first if specified else
                        uses a builtin list of standard locations
                        for tmpfs (/run/shm, /dev/shm, /tmp)
                        The option can also be set using environment
                        variable MTR_MEM=[DIR]
  clean-vardir          Clean vardir if tests were successful and if
                        running in "memory". Otherwise this option is ignored
  client-bindir=PATH    Path to the directory where client binaries are located
  client-libdir=PATH    Path to the directory where client libraries are located


Options to control what test suites or cases to run

  force                 Continue after a failure. When specified once, a
                        failure in a test file will abort this test file, and
                        the execution will continue from the next test file.
                        When specified twice, execution will continue executing
                        the failed test file from the next command.
  skip-not-found        It is not an error if a test was not found in a
                        specified test suite. Test will be marked as skipped.
  do-test=PREFIX or REGEX
                        Run test cases which name are prefixed with PREFIX
                        or fulfills REGEX
  skip-test=PREFIX or REGEX
                        Skip test cases which name are prefixed with PREFIX
                        or fulfills REGEX
  start-from=PREFIX     Run test cases starting test prefixed with PREFIX where
                        prefix may be suite.testname or just testname
  suite[s]=NAME1,..,NAMEN
                        Collect tests in suites from the comma separated
                        list of suite names.
                        The default is: "@DEFAULT_SUITES"
  skip-rpl              Skip the replication test cases.
  big-test              Also run tests marked as "big". Repeat this option
                        twice to run only "big" tests.
  staging-run           Run a limited number of tests (no slow tests). Used
                        for running staging trees with valgrind.
  enable-disabled       Run also tests marked as disabled
  print-testcases       Don't run the tests but print details about all the
                        selected tests, in the order they would be run.
  skip-test-list=FILE   Skip the tests listed in FILE. Each line in the file
                        is an entry and should be formatted as: 
                        <TESTNAME> : <COMMENT>
  force-restart         Always restart servers between tests. This makes it
                        easier to see from which test warnings may come from.

Options that specify ports

  mtr-port-base=#       Base for port numbers, ports from this number to
  port-base=#           number+9 are reserved. Should be divisible by 10;
                        if not it will be rounded down. May be set with
                        environment variable MTR_PORT_BASE. If this value is
                        set and is not "auto", it overrides build-thread.
  mtr-build-thread=#    Specify unique number to calculate port number(s) from.
  build-thread=#        Can be set in environment variable MTR_BUILD_THREAD.
                        Set  MTR_BUILD_THREAD="auto" to automatically aquire
                        a build thread id that is unique to current host
  port-group-size=N     Reserve groups of TCP ports of size N for each MTR thread


Options for test case authoring

  record TESTNAME       (Re)genereate the result file for TESTNAME
  check-testcases       Check testcases for sideeffects
  mark-progress         Log line number and elapsed time to <testname>.progress

Options that pass on options (these may be repeated)

  mariadbd=ARGS         Specify additional arguments to "mariadbd"
  mysqld                Alias for mariadbd.
  mariadbd-env=VAR=VAL  Specify additional environment settings for "mariadbd"
  mysqld-env            Alias for mariadbd-env.

Options to run test on running server

  extern option=value   Run only the tests against an already started server
                        the options to use for connection to the extern server
                        must be specified using name-value pair notation
                        For example:
                         ./$0 --extern socket=/tmp/mysqld.sock

Options for debugging the product

  debug                 Dump trace output for all servers and client programs
  debug-common          Same as debug, but sets 'd' debug flags to
                        "query,info,error,enter,exit"
  debug-server          Use debug version of server, but without turning on
                        tracing
  max-save-core         Limit the number of core files saved (to avoid filling
                        up disks for heavily crashing server). Defaults to
                        $opt_max_save_core. Set its default with
                        MTR_MAX_SAVE_CORE
  max-save-datadir      Limit the number of datadir saved (to avoid filling
                        up disks for heavily crashing server). Defaults to
                        $opt_max_save_datadir. Set its default with
                        MTR_MAX_SAVE_DATADIR
  max-test-fail         Limit the number of test failures before aborting
                        the current test run. Defaults to
                        $opt_max_test_fail, set to 0 for no limit. Set
                        it's default with MTR_MAX_TEST_FAIL
  core-in-failure	Generate a core even if run server is run with valgrind
HERE

Misc options
  user=USER             User for connecting to mysqld(default: $opt_user)
  comment=STR           Write STR to the output
  timer                 Show test case execution time.
  verbose               More verbose output(use multiple times for even more)
  verbose-restart       Write when and why servers are restarted
  start                 Only initialize and start the servers, using the
                        startup settings for the first specified test case
                        Example:
                         $0 --start alias &
  start-and-exit        Same as --start, but mysql-test-run terminates and
                        leaves just the server running
  start-dirty           Only start the servers (without initialization) for
                        the first specified test case
  user-args             In combination with start* and no test name, drops
                        arguments to mariadbd except those specified with
                        --mariadbd (if any)
  wait-all              If --start or --start-dirty option is used, wait for all
                        servers to exit before finishing the process
  fast                  Run as fast as possible, don't wait for servers
                        to shutdown etc.
  force-restart         Always restart servers between tests
  parallel=N            Run tests in N parallel threads (default 1)
                        Use parallel=auto for auto-setting of N
  repeat=N              Run each test N number of times
  retry=N               Retry tests that fail up to N times (default $opt_retry).
                        Retries are also limited by the maximum number of
                        failures before stopping, set with the --retry-failure
                        option
  retry-failure=N       When using the --retry option to retry failed tests,
                        stop when N failures have occurred (default $opt_retry_failure)
  reorder               Reorder tests to get fewer server restarts
  help                  Get this help text

  testcase-timeout=MINUTES Max test case run time (default $opt_testcase_timeout)
  suite-timeout=MINUTES Max test suite run time (default $opt_suite_timeout)
  shutdown-timeout=SECONDS Max number of seconds to wait for server shutdown
                        before killing servers (default $opt_shutdown_timeout)
  warnings              Scan the log files for warnings. Use --nowarnings
                        to turn off.

  stop-file=file        (also MTR_STOP_FILE environment variable) if this
                        file detected mysql test will not start new tests
                        until the file will be removed.
  stop-keep-alive=sec   (also MTR_STOP_KEEP_ALIVE environment variable)
                        works with stop-file, print messages every sec
                        seconds when mysql test is waiting to removing
                        the file (for buildbot)

  sleep=SECONDS         Passed to mysqltest, will be used as fixed sleep time
  debug-sync-timeout=NUM Set default timeout for WAIT_FOR debug sync
                        actions. Disable facility with NUM=0.
  gcov                  Collect coverage information after the test.
                        The result is a gcov file per source and header file.
  gprof                 Collect profiling information using gprof.
  experimental=<file>   Refer to list of tests considered experimental;
                        failures will be marked exp-fail instead of fail.
  timestamp             Print timestamp before each test report line
  timediff              With --timestamp, also print time passed since
                        *previous* test started
  max-connections=N     Max number of open connection to server in mysqltest
  open-files-limit=N    Max number of open files allowed for any of the children
                        of my_safe_process. Default is 1024.
  report-times          Report how much time has been spent on different
                        phases of test execution.
  stress=ARGS           Run stress test, providing options to
                        mysql-stress-test.pl. Options are separated by comma.
  xml-report=<file>     Output jUnit xml file of the results.
  tail-lines=N          Number of lines of the result to include in a failure
                        report.

Some options that control enabling a feature for normal test runs,
can be turned off by prepending 'no' to the option, e.g. --notimer.
This applies to reorder, timer, check-testcases and warnings.

HERE
  exit(1);

}

sub list_options ($) {
  my $hash= shift;

  for (keys %$hash) {
    s/([:=].*|[+!])$//;
    s/\|/\n--/g;
    print "--$_\n";
  }

  exit(1);
}
