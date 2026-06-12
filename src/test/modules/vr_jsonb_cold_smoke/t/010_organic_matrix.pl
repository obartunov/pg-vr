# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# M3 organic logical-replication matrix.  Unlike TAP 008/009 (forged class-S
# exposures), every VR here is constructed ORGANICALLY on a logically logged
# relation under the explicit administrator opt-in vr_logical_construction.
#
# Legs:
#   0. opt-in gate: default off refuses organic construction (C1); after
#      SIGHUP enablement the same INSERT constructs a real VR.
#   1. INSERT with VR  = organic class S: chunks in the decoded stream,
#      captured to ordinary bytes, subscriber applies the logical value
#      (unarmed subscriber: visible degradation, vr=0).
#   2. UPDATE replacing the VR body = organic class S again.
#   3. UPDATE of another column = class U: marker, subscriber keeps value.
#   4. mixed: ordinary-TOAST column changes (class-S chunks) while the VR
#      column is unchanged (class-U marker) - no interference.
#   5. REPLICA IDENTITY FULL: old images carry the VR as a marker; UPDATE
#      and DELETE apply correctly on the subscriber.
#   6. spill/restart: logical_decoding_work_mem=64kB forces serialization;
#      class-S capture remains sound across spill (slot stats prove spill).
#   7. B4: vr_logical_capture_limit=0 wedges the walsender deterministically;
#      SIGHUP raise + reconnect drains the backlog (retry through walsender,
#      not REPACK).
#   8. B3: an incomplete class-S chunk set (stray-chunk forge) is refused
#      with ERRCODE_DATA_CORRUPTED (XX001) by the N16 reassembly
#      (test_decoding consumer; run after the subscription is dropped, since
#      ToastReplace fires before publication filtering and would poison the
#      pgoutput slot as well).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};
my $payload2 =
  q{(SELECT jsonb_object_agg('m'||g, g*2) FROM generate_series(1,2000) g)};

my $pub = PostgreSQL::Test::Cluster->new('pub');
$pub->init(allows_streaming => 'logical');
$pub->append_conf('postgresql.conf',
	"logical_decoding_work_mem = 64kB\n");
$pub->start;
my $sub = PostgreSQL::Test::Cluster->new('sub');
$sub->init;
$sub->start;

for my $node ($pub, $sub)
{
	$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
	$node->safe_psql('postgres',
		    'CREATE TABLE jb (id int PRIMARY KEY, j jsonb, t text);'
		  . 'ALTER TABLE jb ALTER COLUMN t SET STORAGE EXTERNAL;');
}

# ---- leg 0: the opt-in gate itself ----------------------------------------

my ($ret, $out, $err) = $pub->psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb');"
	  . "INSERT INTO jb VALUES (1, $payload, 'small');");
like(
	$err,
	qr/persistent value representation is not supported on logically logged relations/,
	'leg0: default-off C1 refuses organic construction');

$pub->append_conf('postgresql.conf', "vr_logical_construction = on\n");
$pub->reload;

# SIGHUP applied at next command; the armed INSERT must now construct VR.
$pub->poll_query_until('postgres',
	"SELECT current_setting('vr_logical_construction')::bool");
$pub->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb');"
	  . "INSERT INTO jb VALUES (1, $payload, 'small');");
like(
	$pub->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1\b.*\bvr_storage_ok=1\b/,
	'leg0: with opt-in on, organic VR constructed');

# ---- subscription ----------------------------------------------------------

$pub->safe_psql('postgres', 'CREATE PUBLICATION pub_jb FOR TABLE jb');
my $pub_connstr = $pub->connstr . ' dbname=postgres';
$sub->safe_psql('postgres',
	    "CREATE SUBSCRIPTION sub_jb CONNECTION '$pub_connstr' "
	  . "PUBLICATION pub_jb WITH (copy_data = true)");
$pub->wait_for_catchup('sub_jb');
$sub->poll_query_until('postgres',
	'SELECT count(*) = 1 FROM jb WHERE id = 1');

# initial sync used COPY (D1 boundary), organic stream starts now

# ---- leg 1: organic INSERT = class S --------------------------------------

$pub->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb');"
	  . "INSERT INTO jb VALUES (2, $payload2, 'two');");
like(
	$pub->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=2\b/,
	'leg1: publisher holds organic VR after INSERT');
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id = 2 AND j = $payload2"),
	'1',
	'leg1: subscriber applied the logical value of an organic class-S INSERT');
like(
	$sub->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=0\b/,
	'leg1: unarmed subscriber degrades visibly (vr=0, ordinary representation)');

# ---- leg 2: UPDATE replacing the VR body = class S ------------------------

$pub->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb');"
	  . "UPDATE jb SET j = $payload WHERE id = 2;");
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id = 2 AND j = $payload"),
	'1',
	'leg2: body-replacing UPDATE applied (organic class S)');

# ---- leg 3: other-column UPDATE = class U marker ---------------------------

$pub->safe_psql('postgres',
	"UPDATE jb SET t = 'three' WHERE id = 2;");
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT count(*) FROM jb "
	  . "WHERE id = 2 AND t = 'three' AND j = $payload"),
	'1',
	'leg3: class-U marker preserved the subscriber value, txn applied');

# ---- leg 4: mixed ordinary-TOAST class S + VR class U ----------------------

$pub->safe_psql('postgres',
	"UPDATE jb SET t = repeat('x', 8000) WHERE id = 2;");
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id = 2 "
	  . "AND t = repeat('x', 8000) AND j = $payload"),
	'1',
	'leg4: ordinary-TOAST class S and VR class U do not interfere');

# ---- leg 5: REPLICA IDENTITY FULL old images -------------------------------

$pub->safe_psql('postgres', 'ALTER TABLE jb REPLICA IDENTITY FULL');
$pub->safe_psql('postgres',
	"UPDATE jb SET t = 'rif' WHERE id = 2;");
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id = 2 AND t = 'rif' AND j = $payload"),
	'1',
	'leg5: RI FULL UPDATE with VR marker in old image applies');
$pub->safe_psql('postgres', 'DELETE FROM jb WHERE id = 2');
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres', 'SELECT count(*) FROM jb WHERE id = 2'),
	'0',
	'leg5: RI FULL DELETE with VR in old image applies');
$pub->safe_psql('postgres', 'ALTER TABLE jb REPLICA IDENTITY DEFAULT');

# ---- leg 6: spill/restart --------------------------------------------------

$pub->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb');"
	  . "BEGIN;"
	  . "INSERT INTO jb SELECT g, $payload, repeat('y', 1000) "
	  . "  FROM generate_series(100, 199) g;"
	  . "COMMIT;");
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id BETWEEN 100 AND 199 "
	  . "AND j = $payload"),
	'100',
	'leg6: large txn with organic class-S VR applied across spill');
$pub->poll_query_until('postgres',
	    "SELECT spill_txns > 0 FROM pg_stat_replication_slots "
	  . "WHERE slot_name = 'sub_jb'");
ok(1, 'leg6: slot statistics confirm the txn actually spilled');

# ---- leg 7 (B4): threshold wedge + SIGHUP retry through walsender ----------

my $log_offset = -s $pub->logfile;
$pub->append_conf('postgresql.conf', "vr_logical_capture_limit = 0\n");
$pub->reload;
$pub->poll_query_until('postgres',
	"SELECT current_setting('vr_logical_capture_limit') = '0'");

$pub->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jb');"
	  . "INSERT INTO jb VALUES (300, $payload, 'wedged');");

# the walsender must hit the deterministic threshold refusal
$pub->wait_for_log(
	qr/exceeds "vr_logical_capture_limit"/, $log_offset);
ok(1, 'leg7: walsender wedged deterministically at the capture threshold');
is( $sub->safe_psql('postgres', 'SELECT count(*) FROM jb WHERE id = 300'),
	'0',
	'leg7: wedged change did not reach the subscriber');

$pub->append_conf('postgresql.conf', "vr_logical_capture_limit = 1024\n");
$pub->reload;
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id = 300 AND j = $payload"),
	'1',
	'leg7: SIGHUP raise + walsender restart drained the backlog (B4)');

# ---- leg 8 (B3): incomplete class-S chunk set => ERRCODE_DATA_CORRUPTED ----

# Drop the subscription first: ReorderBufferToastReplace runs before
# publication filtering, so the poisoned txn would wedge the pgoutput slot
# permanently as well.
$sub->safe_psql('postgres', 'DROP SUBSCRIPTION sub_jb');

$pub->safe_psql('postgres',
	    "CREATE TABLE jx (id int PRIMARY KEY, j jsonb, t text);"
	  . "SELECT vr_jsonb_cold_arm('jx');"
	  . "INSERT INTO jx VALUES (1, $payload, 'a');");
like(
	$pub->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jx')"),
	qr/\bvr=1\b/,
	'leg8: organic VR seeded on jx');

my $loc = $pub->safe_psql('postgres',
	    "SELECT (SELECT reltoastrelid FROM pg_class "
	  . "        WHERE oid = 'jx'::regclass)::regclass::text");
my ($valueid, $maxseq) = split /\|/,
  $pub->safe_psql('postgres',
	"SELECT chunk_id, max(chunk_seq) FROM $loc GROUP BY chunk_id");

$pub->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('b3slot', 'test_decoding')");

# poisoned txn: a stray chunk under the live valueid creates the toast_hash
# entry (class-S claim); the byte-identical-VR UPDATE puts the descriptor in
# the same decoded txn.  The chunk set is incomplete by construction.
# The stray chunk must start at seq 0: a non-zero first seq is rejected by
# the upstream sequence check in ReorderBufferToastAppendChunk, whose
# mid-iteration ERROR trips an upstream txn->size accounting Assert on
# cassert builds (M3 finding); the VR reassembly check is what B3 targets.
$pub->safe_psql('postgres',
	    "BEGIN;"
	  . "SELECT vr_jsonb_cold_forge_truncate_chunks('jx', $valueid);"
	  . "SELECT vr_jsonb_cold_forge_stray_chunk('jx', $valueid, 0, 16);"
	  . "UPDATE jx SET t = 'poison' WHERE id = 1;"
	  . "COMMIT;");

($ret, $out, $err) = $pub->psql('postgres',
	    "\\set VERBOSITY verbose\n"
	  . "SELECT data FROM pg_logical_slot_get_changes('b3slot', NULL, NULL)");
isnt($ret, 0, 'leg8: decoding the poisoned txn fails');
like(
	$err,
	qr/incomplete: \d+ of \d+ bytes/,
	'leg8: N16 reassembly refused the incomplete class-S chunk set');
like($err, qr/XX001/,
	'leg8: refusal carries ERRCODE_DATA_CORRUPTED');

$pub->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('b3slot')");

done_testing();
