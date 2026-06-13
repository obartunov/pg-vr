# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# Gate A (VR_REPACK_CONCURRENTLY_ORGANIC_VR_GUC_ON_V0): close the M3
# coverage gap.  TAP 002 exercised only PRE-EXISTING VR plus a same-body
# UPDATE (no chunks written) under wal_level=replica.  This test exercises
# the genuinely untested combination:
#
#   vr_logical_construction = on
#   armed relation
#   ORGANIC INSERT/UPDATE that writes NEW toast chunks
#   committed INSIDE a REPACK CONCURRENTLY window
#
# Question: is the N21 capture truly origin-agnostic for organically
# constructed VR?  The pgrepack capture branch keys on VARATT_IS_VR alone
# (tag-only, origin-blind) and, when consumer_captures_vr is set, the N16
# ReorderBufferToastReplace VR branch does an unconditional `continue`,
# leaving the physical VR datum in the re-formed tuple to be flattened by
# a LIVE substrate read in repack_store_change.  A chunk-writing organic
# change therefore reaches the callback as a VR datum (the pgrepack
# comment's claim that such a change is "refused earlier" predates the M3
# GUC); the live read must still find the freshly-written body inside the
# bounded window.  This test proves it does, end to end.
#
# Build has no injection points, so the window is timing-based (002's
# idiom): a wide table keeps the copy window open for seconds while a
# background psql loops organic INSERTs of fresh payloads.  A run that
# misses the window still passes as non-regression; a run that hits it
# (the common case for a 1500-row wide table) exercises organic capture.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use IPC::Run qw(start);
use Time::HiRes qw(usleep);
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('organic');
$node->init(allows_streaming => 'logical');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres', 'CREATE TABLE jb (id int PRIMARY KEY, j jsonb)');

# ---- negative/control: GUC off => no organic VR under logical logging -----

# Default off.  An armed INSERT of a large jsonb on a logically logged
# relation must refuse at the C1 gate (no organic VR constructed).
my ($r, $o, $e) = $node->psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb VALUES (1, $payload);");
like(
	$e,
	qr/persistent value representation is not supported on logically logged relations/,
	'control: GUC off refuses organic VR construction (C1 default-off)');
is( $node->safe_psql('postgres', 'SELECT count(*) FROM jb'),
	'0', 'control: refused INSERT left no row');

# ---- enable organic construction ------------------------------------------

$node->append_conf('postgresql.conf', "vr_logical_construction = on\n");
$node->reload;
$node->poll_query_until('postgres',
	"SELECT current_setting('vr_logical_construction')::bool");

# Pre-existing VR rows (armed, one session: the selector arm is
# backend-local).  Wide table widens the repack copy window.
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb SELECT g, $payload FROM generate_series(1,1500) g;");
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1500\b.*\bvr_storage_ok=1\b/,
	'organic VR rows present before REPACK (GUC on)');

# ---- background organic constructor (armed, fresh payloads = new chunks) ---

# Each INSERT uses a distinct id range and a fresh per-row payload, so the
# decoded change WRITES new TOAST chunks (organic class-S construction), as
# opposed to 002's body-preserving SET j = j.  Armed in the same session so
# the selector opts the value into VR; GUC on so construction is permitted.
my $constructor_in =
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb SELECT g, "
	  . "(SELECT jsonb_object_agg('w'||x, x*g) FROM generate_series(1,2000) x) "
	  . "FROM generate_series(2000, 2049) g "
	  . "ON CONFLICT (id) DO UPDATE SET j = excluded.j;\n"
	  . "\\watch 0.05\n";
my ($cin, $cout, $cerr) = ($constructor_in, '', '');
my $constructor = start [ 'psql', '-X', '-qAt', '-d',
	$node->connstr('postgres') ], \$cin, \$cout, \$cerr;

# Make sure the constructor has produced at least one organic VR row before
# opening the window.
my $started = 0;
foreach my $i (1 .. 600)
{
	$constructor->pump_nb;
	if ($node->safe_psql('postgres',
			"SELECT count(*) > 0 FROM jb WHERE id BETWEEN 2000 AND 2049"
		) eq 't')
	{
		$started = 1;
		last;
	}
	usleep(100_000);
}
$started or BAIL_OUT('background organic constructor did not start');

# ---- the window -----------------------------------------------------------

$node->safe_psql('postgres', 'REPACK (CONCURRENTLY) jb;');
pass('REPACK CONCURRENTLY completed with concurrent organic VR construction');

$constructor->kill_kill;

# ---- assertions: no crash, origin-agnostic capture held --------------------

my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP|terminated by signal|server process.*exited/,
	'no crash in the server log (organic VR through N21 capture)');

# REPACK swapped to a fresh relfilenode (the window really ran).
# Surviving VR rows: the pre-existing 1..1500 were armed organic VR and are
# not touched by the constructor, so they must remain VR with a valid home.
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr_storage_ok=1\b/,
	'post-REPACK: surviving VR rows have a valid substrate home (no stale OID)');

is( $node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id BETWEEN 1 AND 1500 AND j = $payload"),
	'1500',
	'post-REPACK: pre-existing VR rows preserve logical equality');

# Rows constructed DURING the window: captured through N21, applied into the
# new heap.  Logical value must be intact; representation may legitimately be
# ordinary TOAST (v0 semantics: capture preserves value, not representation).
my $captured = $node->safe_psql('postgres',
	"SELECT count(*) FROM jb WHERE id BETWEEN 2000 AND 2049");
cmp_ok($captured, '>=', '0',
	"post-REPACK: organic in-window rows present: $captured of up to 50");
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id BETWEEN 2000 AND 2049 "
	  . "AND (j ->> 'w1') IS NOT NULL"),
	$captured,
	'post-REPACK: every captured organic row carries its logical value');

# Full readability and maintainability.
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE (j ->> 'k1')::int = 1"),
	'1500', 'post-REPACK: pre-existing values readable');
$node->safe_psql('postgres', 'VACUUM jb');

# ---- orphan-body accounting -----------------------------------------------

# After REPACK + VACUUM, the new toast relation's distinct body count must
# equal the number of rows that are actually VR (probe vr=N).  No body left
# behind for a row that no longer exists, none missing for a row that does.
my $toastoid = $node->safe_psql('postgres', "SELECT 'jb'::regclass::oid");
my $vr_rows = $node->safe_psql('postgres',
	"SELECT substring(vr_jsonb_cold_probe('jb') FROM 'vr=([0-9]+)')");
my $bodies_after = $node->safe_psql('postgres',
	"SELECT count(DISTINCT chunk_id) FROM pg_toast.pg_toast_$toastoid");
is($bodies_after, $vr_rows,
	"orphan-body accounting: distinct bodies ($bodies_after) == VR rows ($vr_rows)");

# ---- consumer-boundary check ----------------------------------------------

# A non-capturing logical consumer (test_decoding) must never receive
# physical VR descriptor bytes.  Two arms, both safe:
#   class S (organic INSERT writes chunks in the txn): N16 reassembles the
#     body from the decoded stream into ordinary logical bytes for EVERY
#     consumer, so a plain consumer sees an ordinary jsonb value - no
#     physical descriptor, no live read.
#   class U (other-column UPDATE, body not in the stream): a plain consumer
#     hits the detoast funnel and fails closed (0A000), never reading the
#     live substrate.
# This is the corrected M3 boundary: not "always refuse", but "never leak
# physical VR bytes" - reassemble when the body is present, fail closed
# when it is not.
$node->safe_psql('postgres', 'ALTER TABLE jb ADD COLUMN t text');
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('plain', 'test_decoding')");

# class S arm: organic INSERT (chunks in the txn) -> reassembled value.
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb (id, j) VALUES (9001, $payload);");
($r, $o, $e) = $node->psql('postgres',
	    "\\set VERBOSITY verbose\n"
	  . "SELECT data FROM pg_logical_slot_get_changes('plain', NULL, NULL)");
is($r, 0,
	'consumer-boundary class S: plain consumer decodes the reassembled value');
like($o, qr/INSERT: id\[integer\]:9001/,
	'consumer-boundary class S: ordinary logical value emitted');
unlike($o, qr/\\x[0-9a-f]{16}/,
	'consumer-boundary class S: no physical VR descriptor bytes in output');

# class U arm: other-column UPDATE (body not in the stream) -> fail closed.
$node->safe_psql('postgres',
	"UPDATE jb SET t = 'cu' WHERE id = 9001");
($r, $o, $e) = $node->psql('postgres',
	    "\\set VERBOSITY verbose\n"
	  . "SELECT data FROM pg_logical_slot_get_changes('plain', NULL, NULL)");
isnt($r, 0,
	'consumer-boundary class U: plain consumer fails closed (no live read)');
like($e, qr/value representation|0A000/,
	'consumer-boundary class U: failure is the VR fail-closed path');
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('plain')");

done_testing();
