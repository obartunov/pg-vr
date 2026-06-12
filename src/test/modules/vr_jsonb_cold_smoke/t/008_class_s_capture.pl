# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# M1 class-S: a decoded transaction that carries BOTH a VR datum and its
# body chunks (forged by the test module inside one transaction, bypassing
# the C1 construction seam without relaxing it) must be captured by
# ReorderBufferToastReplace: chunks reassembled from the DECODED STREAM
# ONLY, packaged as transient VARTAG_VR_INMEM, flattened through the
# capture seam into ordinary logical bytes, and handed to the plugin via
# the INDIRECT discipline - so a datum-printing consumer prints the
# flattened logical value where it previously hit a refusal.
#
# Also proves: the capture limit refuses the class-S path cleanly and is
# retryable; a class-U exposure (no chunks) still refuses (E21 regression);
# the transient INMEM form cannot be stored (fill_val).
#
# Deterministic throughout: slots decode committed WAL.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('classs');
$node->init(allows_streaming => 'logical');    # wal_level=logical
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres',
	'CREATE TABLE jb (id int PRIMARY KEY, j jsonb, n int)');
$node->safe_psql('postgres',
	'CREATE TABLE src (id int, b bytea)');
$node->safe_psql('postgres', 'INSERT INTO jb VALUES (3, NULL, 0)');
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('s_cls', 'test_decoding')");

# --- class-S green path -------------------------------------------------
# One transaction: chunks into the toast rel + the forged VR into row 1.
$node->safe_psql('postgres',
	"SELECT vr_jsonb_cold_forge_class_s('src', 'jb', 1, $payload)");
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1\b/,
	'forged persistent VR is stored');

my ($ret, $out, $err) = $node->psql('postgres',
	"SELECT data FROM pg_logical_slot_get_changes('s_cls', NULL, NULL)");
is($ret, 0, 'class-S transaction decodes successfully');
like($out, qr/"k1": 1.*"k2": 2/s,
	'datum-printing consumer received the flattened logical value');
unlike($err, qr/value representation/,
	'no VR refusal anywhere in the class-S decode');

like($out, qr/\Q"k2000": 2000\E/,
	'flattened output extends to the last key');

# --- threshold refusal in the class-S path, retryable --------------------
$node->safe_psql('postgres',
	"ALTER SYSTEM SET vr_logical_capture_limit = 0");
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
$node->poll_query_until('postgres',
	"SELECT current_setting('vr_logical_capture_limit') = '0'")
  or BAIL_OUT('reload did not apply');

$node->safe_psql('postgres',
	"SELECT vr_jsonb_cold_forge_class_s('src', 'jb', 2, $payload)");
($ret, $out, $err) = $node->psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('s_cls', NULL, NULL)");
isnt($ret, 0, 'class-S capture above the limit fails');
like(
	$err,
	qr/exceeds "vr_logical_capture_limit"/,
	'failure is the seam threshold, before flatten');

$node->safe_psql('postgres',
	"ALTER SYSTEM SET vr_logical_capture_limit = '1MB'");
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
$node->poll_query_until('postgres',
	"SELECT current_setting('vr_logical_capture_limit') = '1MB'")
  or BAIL_OUT('reload did not apply');

($ret, $out, $err) = $node->psql('postgres',
	"SELECT data FROM pg_logical_slot_get_changes('s_cls', NULL, NULL)");
is($ret, 0, 'retry after limit restore succeeds (wedge is retryable)');
like($out, qr/"k1": 1/, 'retried decode delivers the logical value');

# --- class-U regression (E21): no chunks -> refusal unchanged -------------
$node->safe_psql('postgres', 'UPDATE jb SET n = 1 WHERE id = 1');
($ret, $out, $err) = $node->psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('s_cls', NULL, NULL)");
isnt($ret, 0, 'class-U (no-chunk) exposure still fails');
like(
	$err,
	qr/cannot be flattened from live storage/,
	'class-U refusal is the E21 flatten guard, unchanged');

# --- transient INMEM is not storable --------------------------------------
($ret, $out, $err) = $node->psql('postgres',
	"SELECT vr_jsonb_cold_forge_inmem_store('jb', 3)");
isnt($ret, 0, 'storing a transient INMEM datum fails');
like(
	$err,
	qr/cannot store a transient in-memory value representation/,
	'fill_val hard-refuses VARTAG_VR_INMEM');

my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP|terminated by signal|server process.*exited/,
	'no crash in the server log');

$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('s_cls')");
$node->stop;

done_testing();
