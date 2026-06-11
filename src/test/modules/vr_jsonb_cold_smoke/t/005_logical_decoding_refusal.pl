# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# General logical decoding must keep refusing VR: the consumer_captures_vr
# capability is set only in the built-in REPACK decoding worker.  An SQL
# slot (test_decoding) decoding a mixed chunk-writing UPDATE over a table
# with a pre-existing VR column must hit the unchanged N16 refusal.
# Deterministic: the slot decodes already-committed WAL - no timing window.
#
# VR is created at wal_level=replica (construction is gated under logical
# logging), then the same cluster is restarted at wal_level=logical for the
# slot phase; the mixed UPDATE itself constructs nothing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('slotrefusal');
$node->init(allows_streaming => 1);    # wal_level=replica
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres',
	'CREATE TABLE jb (id int PRIMARY KEY, j jsonb, t text)');
$node->safe_psql('postgres',
	'ALTER TABLE jb ALTER COLUMN t SET STORAGE EXTERNAL');
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb'::regclass);\n"
	  . "INSERT INTO jb VALUES (1, $payload, 'x');");
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1\b/,
	'value stored as VR at wal_level=replica');

# Switch to logical logging for the slot phase.
$node->append_conf('postgresql.conf', "wal_level = logical\n");
$node->restart;

$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('s_vr', 'test_decoding')");

# Mixed chunk-writing UPDATE: the ordinary column toasts, the VR column is
# untouched (no construction, so the B-repl gate does not fire).
$node->safe_psql('postgres',
	"UPDATE jb SET t = repeat('y', 8000) WHERE id = 1");

my ($ret, $out, $err) = $node->psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('s_vr', NULL, NULL)");
isnt($ret, 0, 'SQL slot decoding of the mixed change fails');
like(
	$err,
	qr/logical decoding of a value representation is not supported/,
	'N16 refusal unchanged for general logical decoding');

# The refusal is repeatable (the slot is stuck on the change, not consumed).
($ret, $out, $err) = $node->psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('s_vr', NULL, NULL)");
isnt($ret, 0, 'refusal is deterministic on retry');

$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('s_vr')");
$node->stop;

done_testing();
