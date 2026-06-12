# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# Decode flatten guard (P-2c closed): a NO-CHUNK change carrying a
# pre-existing VR datum passes ReorderBufferToastReplace untouched (the
# transaction wrote no TOAST chunks) and reaches the output plugin.  A
# datum-printing consumer (test_decoding) then detoasts the attribute,
# which funnels into vr_detoast_flatten - whose historic-snapshot guard
# must now refuse DETERMINISTICALLY, before any live body access, instead
# of live-reading the body with consumer-dependent soundness.
#
# Also exercises N17 directly on the same change: a pgoutput slot must
# refuse at the wire boundary with its own (different) message.
#
# Deterministic throughout: slots decode already-committed WAL; no timing
# window is involved.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('flattenguard');
$node->init(allows_streaming => 1);    # wal_level=replica
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres',
	'CREATE TABLE jb (id int PRIMARY KEY, j jsonb, n int)');
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb VALUES (1, $payload, 0);");
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1\b/,
	'value stored as VR at wal_level=replica');

# Switch to logical logging for the slot phase.
$node->append_conf('postgresql.conf', "wal_level = logical\n");
$node->restart;

$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('s_td', 'test_decoding')");
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('s_po', 'pgoutput')");
$node->safe_psql('postgres', 'CREATE PUBLICATION pub FOR TABLE jb');

# NO-CHUNK change: update the plain int column; the VR column rides along
# unchanged and nothing else toasts, so reorderbuffer reassembly never runs.
$node->safe_psql('postgres', 'UPDATE jb SET n = 1 WHERE id = 1');

# --- datum-printing consumer: the N8 guard must refuse, before body access
my ($ret, $out, $err) = $node->psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('s_td', NULL, NULL)");
isnt($ret, 0, 'datum-printing consumer fails on the no-chunk VR change');
like(
	$err,
	qr/logical decoding of a value representation is not supported/,
	'guard refusal message');
like(
	$err,
	qr/cannot be flattened from live storage inside a logical decoding context/,
	'errdetail discriminates the flatten guard from the N16 refusal');

# Deterministic: the slot is stuck on the change, the refusal repeats.
($ret, $out, $err) = $node->psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('s_td', NULL, NULL)");
isnt($ret, 0, 'refusal is deterministic on retry');
like(
	$err,
	qr/cannot be flattened from live storage/,
	'same guard site on retry');

# --- pgoutput consumer: N17 still refuses at the wire boundary,
#     with its own message (the guard is never reached: pgoutput does not
#     print datums through output functions).
($ret, $out, $err) = $node->psql('postgres',
	    "SELECT count(*) FROM pg_logical_slot_get_binary_changes("
	  . "'s_po', NULL, NULL, 'proto_version', '1', 'publication_names', 'pub')"
);
isnt($ret, 0, 'pgoutput consumer fails on the same change');
like(
	$err,
	qr/logical replication of a value representation is not supported/,
	'N17 refusal unchanged, distinct message');

$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('s_td')");
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('s_po')");
$node->stop;

done_testing();
