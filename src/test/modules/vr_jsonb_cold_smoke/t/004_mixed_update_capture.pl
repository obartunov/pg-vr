# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# N16 mixed slice: a concurrent UPDATE that toasts an ORDINARY column of a
# table that also holds a (untouched, live) VR column writes TOAST chunks, so
# the decoded change passes through ReorderBufferToastReplace.  With the
# repack worker's consumer_captures_vr capability the VR datum rides the
# re-formed tuple to the pgrepack callback, where the N21 capture seam
# flattens it; the REPACK must complete with both columns intact.  Before
# this milestone the same shape failed with "logical decoding of a value
# representation is not supported" (H1).
#
# Timing-based (no injection points in this build): if a run misses the
# window it still proves non-regression; the deterministic refusal-side
# coverage lives in t/005, the capture-evidence signature in the manual
# repro scripts.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use IPC::Run qw(start);
use Time::HiRes qw(usleep);
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('mixed');
$node->init(allows_streaming => 1);    # wal_level=replica
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres',
	'CREATE TABLE jb (id int PRIMARY KEY, j jsonb, t text)');
$node->safe_psql('postgres',
	'ALTER TABLE jb ALTER COLUMN t SET STORAGE EXTERNAL');
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb SELECT g, $payload, 'x' FROM generate_series(1,1500) g;"
);
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1500\b/,
	'all rows stored as VR before REPACK');

# Background updater toasts the ordinary column only; the VR column rides
# along unchanged - the mixed slice.
my $updater_in =
  "UPDATE jb SET t = repeat('y', 8000) WHERE id = 400;\n\\watch 0.05\n";
my $updater_out = '';
my $updater_err = '';
my $updater     = start [ 'psql', '-X', '-qAt', '-d',
	$node->connstr('postgres') ],
  \$updater_in, \$updater_out, \$updater_err;

my $updater_started = 0;
foreach my $i (1 .. 600)
{
	$updater->pump_nb;
	if ($node->safe_psql('postgres',
			"SELECT n_tup_upd > 0 FROM pg_stat_user_tables WHERE relname = 'jb'"
		) eq 't')
	{
		$updater_started = 1;
		last;
	}
	usleep(100_000);
}
$updater_started or BAIL_OUT('background updater did not start');

$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) jb;');
pass('REPACK CONCURRENTLY completed with a concurrent mixed-column update');

$updater->kill_kill;

my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP|terminated by signal|server process.*exited/,
	'no crash in the server log');
unlike(
	$log,
	qr/logical decoding of a value representation is not supported/,
	'N16 did not refuse under the repack capability');

is( $node->safe_psql('postgres',
		"SELECT j = $payload FROM jb WHERE id = 400"),
	't',
	'VR column value byte-identical after REPACK');
is( $node->safe_psql('postgres',
		"SELECT t = repeat('y', 8000) FROM jb WHERE id = 400"),
	't', 'toasted ordinary column intact after REPACK');
is( $node->safe_psql('postgres', 'SELECT count(*) FROM jb'),
	'1500', 'row count intact');
$node->safe_psql('postgres', 'VACUUM jb');

$node->stop;

done_testing();
