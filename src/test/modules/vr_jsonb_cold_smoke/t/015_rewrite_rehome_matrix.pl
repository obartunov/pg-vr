# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# VR_REWRITE_REHOME_GREEN_V0 -- dedicated rewrite/rehome regression matrix.
#
# Closes the "dedicated test inventory" gap behind the partial status of
#   E6  rewrite REHOME via VACUUM FULL / CLUSTER (and ALTER rewrite / REPACK)
#   E7  rewrite FLATTEN / foreign-home disposition
# in VR_LIFECYCLE_GRAPH_V1.  Prior coverage was a single VACUUM FULL case in
# sql/vr_jsonb_cold_smoke.sql; the logical-replication matrix (010/011) exercises
# the *capture* path (REPACK CONCURRENTLY, N21), not the rewrite/rehome seam.
#
# Single seam under test (source-audited):
#   toast_helper.c:243  vr_rewrite_action(value, reltoastrelid)  -- the
#       KEEP/REHOME/FLATTEN mechanism; REHOME copies the body into the rewrite
#       target's TOAST and rewrites the locator (vr_toast_body_copy_to_relation).
#   heaptoast.c:199     final safety net: any VR still REHOME against the
#       effective home (rd_toastoid ?: reltoastrelid) at store time -> hard ERROR.
# Rewrite commands all funnel through this seam:
#   VACUUM FULL / CLUSTER / non-concurrent REPACK -> commands/repack.c
#       cluster_rel -> rebuild_relation -> table_relation_copy_for_cluster
#       -> rewriteheap.c raw_heap_insert -> heap_toast_insert_or_update.
#   ALTER TABLE rewrite -> ATRewriteTable -> table_tuple_insert(newrel)
#       -> heap_toast_insert_or_update.
#
# Invariants asserted per rewrite, for the required matrix:
#   * logical value preserved   (j = original payload)
#   * home keeps/rehomes VR     (vr=N, vr_storage_ok=1: locator points at the
#                                relation's live reltoastrelid)
#   * no orphan body            (distinct chunk_id groups in the live TOAST rel
#                                == number of VR rows; pre-rewrite bodies gone)
#   * no physical VR leak        (vr_storage_ok=1 -> no descriptor references a
#                                dropped / foreign TOAST)
# Plus an explicit E25-negative leg: a rewrite must PRESERVE representation and
# must NOT auto-convert ordinary -> VR (E25 stays not-implemented for v0).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('rewrite');
$node->init;    # default wal_level = replica; not logically logged
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'vr_jsonb_cold_smoke'");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');

# probe() -> the raw probe string for a relation.
sub probe { return $_[0]->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('$_[1]')"); }

# body_count() -> number of distinct TOAST chunk groups (= distinct valueids =
# physical VR bodies) currently stored in a relation's own TOAST relation.
# Resolves the dynamic pg_toast relation name first, then counts.
sub body_count
{
	my ($n, $tbl) = @_;
	my $toast = $n->safe_psql('postgres',
		"SELECT reltoastrelid::regclass::text FROM pg_class WHERE oid='$tbl'::regclass");
	return '0' if (!defined $toast || $toast eq '' || $toast eq '-');
	return $n->safe_psql('postgres',
		"SELECT count(DISTINCT chunk_id) FROM $toast");
}

# preserved() -> count of rows whose jsonb still equals the original payload.
sub preserved
{
	my ($n, $tbl, $expect) = @_;
	return $n->safe_psql('postgres',
		"SELECT count(*) FROM $tbl WHERE j = $payload") eq "$expect";
}

# rewrite_matrix_leg: run one rewrite command on a 3-row local-VR table and
# assert all four invariants.
sub rewrite_matrix_leg
{
	my ($edge, $cmd) = @_;

	$node->safe_psql('postgres', 'DROP TABLE IF EXISTS r');
	$node->safe_psql('postgres', 'CREATE TABLE r (id int PRIMARY KEY, j jsonb)');
	$node->safe_psql('postgres',
		'ALTER TABLE r ALTER COLUMN j SET (vr_jsonb_cold = on)');
	$node->safe_psql('postgres',
		"INSERT INTO r SELECT g, $payload FROM generate_series(1,3) g");

	# precondition: 3 VR rows, locally homed, 3 physical bodies
	like(probe($node, 'r'), qr/\bvr=3\b.*\bvr_storage_ok=1\b/,
		"$edge: precondition 3 local VR rows, vr_storage_ok=1");
	is(body_count($node, 'r'), '3', "$edge: precondition 3 physical bodies");

	# relfilenode before the command: proves the command really rewrites the
	# heap (defeats a vacuous pass if a command ever silently no-ops).
	my $rfn_before = $node->safe_psql('postgres',
		"SELECT relfilenode FROM pg_class WHERE oid='r'::regclass");

	$node->safe_psql('postgres', $cmd);

	isnt($node->safe_psql('postgres',
			"SELECT relfilenode FROM pg_class WHERE oid='r'::regclass"),
		$rfn_before,
		"$edge: [$cmd] actually rewrote the heap (relfilenode changed) -- non-vacuous");
	like(probe($node, 'r'), qr/\bvr=3\b.*\bvr_storage_ok=1\b/,
		"$edge: after [$cmd] still 3 VR rows, rehomed (vr_storage_ok=1, no leak)");
	ok(preserved($node, 'r', 3),
		"$edge: after [$cmd] logical value preserved for all rows");
	is(body_count($node, 'r'), '3',
		"$edge: after [$cmd] exactly 3 physical bodies (no orphan, old bodies reclaimed)");
}

# ---------------------------------------------------------------------------
# E6: local-home VR rehome across the full rewrite command matrix.
# ---------------------------------------------------------------------------
rewrite_matrix_leg('E6 VACUUM FULL', 'VACUUM FULL r');
rewrite_matrix_leg('E6 CLUSTER',     'CLUSTER r USING r_pkey');
# ALTER rewrite creates a NEW toast OID: vr_storage_ok=1 afterwards proves the
# locator's storage_oid was rewritten to the new home (strong rehome evidence).
rewrite_matrix_leg('E6 ALTER rewrite', 'ALTER TABLE r ALTER COLUMN id TYPE bigint');
rewrite_matrix_leg('E6 REPACK',      'REPACK r');

# ---------------------------------------------------------------------------
# E7: foreign-home disposition.  A VR whose body lives in a DIFFERENT
# relation's TOAST is never persisted with a foreign locator: the write path
# classifies REHOME and copies the body into the target's own TOAST
# (heaptoast.c:199 net is the backstop).  forge_class_s builds exactly that
# foreign-home descriptor and routes it through the ordinary write path.
# ---------------------------------------------------------------------------
$node->safe_psql('postgres', 'CREATE TABLE src (id int PRIMARY KEY, j jsonb)');
$node->safe_psql('postgres', 'CREATE TABLE fh (id int PRIMARY KEY, j jsonb, n int)');
# forge: body chunks written into src's TOAST; foreign-home VR INSERTed into fh.
$node->safe_psql('postgres',
	"SELECT vr_jsonb_cold_forge_class_s('src','fh',1,$payload)");
like(probe($node, 'fh'), qr/\bvr=1\b.*\bvr_storage_ok=1\b/,
	'E7: foreign-home VR rehomed into target on write (locally homed, no foreign locator)');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM fh WHERE id=1 AND j = $payload"),
	'1', 'E7: foreign-home write preserved the logical value');
# and it survives a subsequent rewrite of the target like any local VR
$node->safe_psql('postgres', 'VACUUM FULL fh');
like(probe($node, 'fh'), qr/\bvr=1\b.*\bvr_storage_ok=1\b/,
	'E7: rehomed foreign-origin VR survives VACUUM FULL, still locally homed');
ok(preserved($node, 'fh', 1),
	'E7: logical value preserved across rewrite of foreign-origin VR');

# ---------------------------------------------------------------------------
# E25-NEGATIVE: a rewrite must PRESERVE representation, never auto-convert
# ordinary -> VR.  This proves E25 is NOT greened by this work.
# ---------------------------------------------------------------------------
$node->safe_psql('postgres', 'CREATE TABLE ord (id int PRIMARY KEY, j jsonb)');
# no vr_jsonb_cold policy: the large value is ordinary on-disk TOAST.
$node->safe_psql('postgres',
	"INSERT INTO ord SELECT g, $payload FROM generate_series(1,3) g");
like(probe($node, 'ord'), qr/\bvr=0\b/,
	'E25-neg: ordinary rows are not VR before rewrite');
for my $cmd ('VACUUM FULL ord', 'CLUSTER ord USING ord_pkey',
	'ALTER TABLE ord ALTER COLUMN id TYPE bigint', 'REPACK ord')
{
	$node->safe_psql('postgres', $cmd);
	like(probe($node, 'ord'), qr/\bvr=0\b/,
		"E25-neg: [$cmd] preserved ordinary representation (no auto ordinary->VR)");
	ok(preserved($node, 'ord', 3),
		"E25-neg: [$cmd] logical value preserved");
}

# ---------------------------------------------------------------------------
# Mixed table: a VR row and an ordinary row in the same relation; one rewrite
# must keep each row's representation independently.
# ---------------------------------------------------------------------------
$node->safe_psql('postgres', 'CREATE TABLE mix (id int PRIMARY KEY, j jsonb)');
$node->safe_psql('postgres',
	'ALTER TABLE mix ALTER COLUMN j SET (vr_jsonb_cold = on)');
$node->safe_psql('postgres', "INSERT INTO mix SELECT 1, $payload");      # VR
$node->safe_psql('postgres', 'ALTER TABLE mix ALTER COLUMN j RESET (vr_jsonb_cold)');
$node->safe_psql('postgres', "INSERT INTO mix SELECT 2, $payload");      # ordinary
like(probe($node, 'mix'), qr/\bvr=1\b.*\bondisk=1\b/,
	'mixed: one VR row + one ordinary row before rewrite');
$node->safe_psql('postgres', 'VACUUM FULL mix');
like(probe($node, 'mix'), qr/\bvr=1\b.*\bondisk=1\b.*\bvr_storage_ok=1\b/,
	'mixed: VACUUM FULL kept VR row VR (rehomed) and ordinary row ordinary');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM mix WHERE j = $payload"),
	'2', 'mixed: both logical values preserved');

$node->stop;
done_testing();
