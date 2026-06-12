# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# Dump/COPY boundary for VR values (D1/D2 edges of the lifecycle graph).
#
# Claim under test: COPY OUT (text and binary) and pg_dump observe LOGICAL
# values only, because the per-attribute output/send functions detoast the
# raw datum through detoast_attr, whose VR arm (detoast.c) flattens via the
# kind's flatten method before any bytes are produced.  A physical VR
# descriptor cannot leak: in text output it would break the JSON syntax the
# round trip parses back; in binary output it would fail the versioned recv
# format - so round-trip logical equality IS the leak proof, plus a direct
# non-printable-byte tripwire on the text file.
#
# Restore semantics under test (accepted v0): COPY FROM / restore writes
# ordinary logical values; representation degrades VISIBLY (probe vr=0)
# unless the selector is explicitly armed in the restoring session, in
# which case the representation is reconstructed (probe vr=N).  Logical
# value is preserved in every case.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g*x) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('dumpcopy');
$node->init;
$node->start;

my $tmp = PostgreSQL::Test::Utils::tempdir();

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres', 'CREATE TABLE jc (id int PRIMARY KEY, j jsonb)');
# Distinct payload per row (g*x) so cross-row mixups cannot pass equality.
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jc'::regclass);\n"
	  . "INSERT INTO jc SELECT x, $payload FROM generate_series(1,3) x;");
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jc')"),
	qr/\bvr=3\b/,
	'source table stores VR');

my $eq_against_jc =
    'SELECT count(*) = 3 AND bool_and(t.j = jc.j) '
  . 'FROM %s t JOIN jc USING (id)';

# ---------- 1. COPY OUT text -> COPY FROM (unarmed) ----------
$node->safe_psql('postgres', "COPY jc TO '$tmp/jc.txt'");

my $txt = slurp_file("$tmp/jc.txt");
unlike($txt, qr/[^[:print:]\t\n]/,
	'text COPY output contains no non-printable descriptor bytes');

$node->safe_psql('postgres',
	    "CREATE TABLE jc_text (id int PRIMARY KEY, j jsonb);\n"
	  . "COPY jc_text FROM '$tmp/jc.txt';");
is( $node->safe_psql('postgres', sprintf($eq_against_jc, 'jc_text')),
	't', 'text COPY round trip preserves the logical values');
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jc_text')"),
	qr/\bvr=0\b/,
	'unarmed text restore degrades representation visibly (vr=0)');

# ---------- 2. COPY OUT binary -> COPY FROM binary (unarmed) ----------
$node->safe_psql('postgres', "COPY jc TO '$tmp/jc.bin' WITH (FORMAT binary)");
$node->safe_psql('postgres',
	    "CREATE TABLE jc_bin (id int PRIMARY KEY, j jsonb);\n"
	  . "COPY jc_bin FROM '$tmp/jc.bin' WITH (FORMAT binary);");
is( $node->safe_psql('postgres', sprintf($eq_against_jc, 'jc_bin')),
	't', 'binary COPY round trip preserves the logical values');
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jc_bin')"),
	qr/\bvr=0\b/,
	'unarmed binary restore degrades representation visibly (vr=0)');

# ---------- 3. armed COPY FROM reconstructs the representation ----------
# Arm and COPY in ONE session: the selector arm is backend-local.
$node->safe_psql('postgres',
	    "CREATE TABLE jc_armed (id int PRIMARY KEY, j jsonb);\n");
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jc_armed'::regclass);\n"
	  . "COPY jc_armed FROM '$tmp/jc.txt';");
is( $node->safe_psql('postgres', sprintf($eq_against_jc, 'jc_armed')),
	't', 'armed restore preserves the logical values');
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jc_armed')"),
	qr/\bvr=3\b/,
	'armed restore reconstructs VR (degradation is policy, not loss)');

# ---------- 4. pg_dump plain -> psql restore ----------
command_ok(
	[
		'pg_dump',     '--no-sync',
		'--table',     'jc',
		'--file',      "$tmp/dump_plain.sql",
		'--dbname',    $node->connstr('postgres')
	],
	'pg_dump plain format');
my $dump = slurp_file("$tmp/dump_plain.sql");
unlike($dump, qr/[^[:print:]\t\n]/,
	'plain dump contains no non-printable descriptor bytes');

$node->safe_psql('postgres', 'CREATE DATABASE restdb_plain');
# The dump carries data only; the extension provides the probe.
$node->safe_psql('restdb_plain', 'CREATE EXTENSION vr_jsonb_cold_smoke');
command_ok(
	[ 'psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1',
		'-d', $node->connstr('restdb_plain'), '-f', "$tmp/dump_plain.sql" ],
	'plain dump restores');
is( $node->safe_psql('restdb_plain',
		    'SELECT count(*) = 3 FROM jc'),
	't', 'plain restore row count');
is( $node->safe_psql(
		'restdb_plain',
		"SELECT bool_and(j = $payload) FROM jc, LATERAL (SELECT id AS x) s"),
	't',
	'plain restore preserves the logical values');
like(
	$node->safe_psql('restdb_plain', "SELECT vr_jsonb_cold_probe('jc')"),
	qr/\bvr=0\b/,
	'plain restore degrades representation visibly (vr=0)');

# ---------- 5. pg_dump custom -> pg_restore ----------
command_ok(
	[
		'pg_dump', '--no-sync', '--format', 'custom',
		'--table', 'jc',
		'--file',  "$tmp/dump_custom.pgdump",
		'--dbname', $node->connstr('postgres')
	],
	'pg_dump custom format');
$node->safe_psql('postgres', 'CREATE DATABASE restdb_custom');
$node->safe_psql('restdb_custom', 'CREATE EXTENSION vr_jsonb_cold_smoke');
command_ok(
	[
		'pg_restore', '--no-owner',
		'--dbname',   $node->connstr('restdb_custom'),
		"$tmp/dump_custom.pgdump"
	],
	'custom dump restores');
is( $node->safe_psql(
		'restdb_custom',
		"SELECT bool_and(j = $payload) FROM jc, LATERAL (SELECT id AS x) s"),
	't',
	'custom restore preserves the logical values');
like(
	$node->safe_psql('restdb_custom', "SELECT vr_jsonb_cold_probe('jc')"),
	qr/\bvr=0\b/,
	'custom restore degrades representation visibly (vr=0)');

# ---------- 6. boundary hygiene ----------
my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP|terminated by signal|server process.*exited/,
	'no crash in the server log');
unlike(
	$log,
	qr/value representation is not supported/,
	'no decode-side refusal was ever involved in dump/COPY');

$node->stop;

done_testing();
