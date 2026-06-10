# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# REPACK CONCURRENTLY x VR, no-chunk capture slice: a concurrent same-body
# UPDATE (SET j = j) keeps the VR datum in the new tuple without writing
# TOAST chunks, so the decoded change reaches the pgrepack change callback
# carrying a VR datum.  Flatten-on-capture must turn it into the ordinary
# logical value (no crash, no stale locator in the spill file) and the
# REPACK must complete with the value intact.
#
# The concurrent change must commit inside the repack window.  This build
# has no injection points, so the test is timing-based: a wide table keeps
# the window open for seconds while a background psql loops the same-body
# UPDATE via \watch.  If a run misses the window the test still passes (it
# then only proves non-regression); the unit-level capture semantics are
# covered deterministically in the regress script.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use IPC::Run qw(start);
use Time::HiRes qw(usleep);
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('capture');
$node->init(allows_streaming => 1);    # wal_level=replica
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres', 'CREATE TABLE jb (id int PRIMARY KEY, j jsonb)');

# Pre-existing VR rows; arm and insert in one session (the selector arm is
# backend-local).  The wide table widens the repack copy window.
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb SELECT g, $payload FROM generate_series(1,1500) g;");

like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1500\b/,
	'all rows stored as VR before REPACK');

# Background same-body updater (deliberately NOT armed: the update reuses the
# existing body, writes no TOAST chunks, and keeps the VR datum in the new
# tuple - the no-chunk capture slice).
my $updater_in  = "UPDATE jb SET j = j WHERE id = 400;\n\\watch 0.05\n";
my $updater_out = '';
my $updater_err = '';
my $updater     = start [ 'psql', '-X', '-qAt', '-d',
	$node->connstr('postgres') ],
  \$updater_in, \$updater_out, \$updater_err;

# Make sure the updater is alive before opening the window.  IPC::Run only
# delivers the child's stdin during pump calls, so pump (non-blocking) while
# waiting for the first update to land in the stats.
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

# The window: decode + flatten-on-capture happen behind this statement.
$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) jb;');
pass('REPACK CONCURRENTLY completed with a concurrent same-body VR update');

$updater->kill_kill;

# The decoding worker must not have crashed (the pre-fix failure mode was a
# TRAP in repack_store_change followed by a postmaster restart).
my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP|terminated by signal|server process.*exited/,
	'no crash in the server log');

# Value fidelity: the updated row still denotes the same logical value.
is( $node->safe_psql('postgres',
		"SELECT j = $payload FROM jb WHERE id = 400"),
	't',
	'captured value is byte-identical after REPACK');

is( $node->safe_psql('postgres', 'SELECT count(*) FROM jb'),
	'1500', 'row count intact');

# The relation stays fully readable and maintainable.
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE (j ->> 'k1')::int = 1"),
	'1500', 'all values readable after REPACK');
$node->safe_psql('postgres', 'VACUUM jb');

$node->stop;

done_testing();
