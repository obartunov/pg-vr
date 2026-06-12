# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# M2 class-U over real logical replication: an UPDATE that does not touch
# the VR column ships LOGICALREP_COLUMN_UNCHANGED for it (the same protocol
# shape as unchanged TOAST), so the subscriber retains its current LOCAL
# value - including a local VR representation if it has one.  No physical
# descriptor crosses the wire (the residual N17 backstop would hard-ERROR),
# no live read happens (E21 stands), and the publisher's representation is
# not imposed on the subscriber (no cross-wire representation identity).
#
# Both nodes pre-seed the same logical row; the subscriber's copy is forged
# as a LOCAL VR, the strongest retention assert: after two class-U applies
# the subscriber must still hold vr=1.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $pub = PostgreSQL::Test::Cluster->new('pub');
$pub->init(allows_streaming => 'logical');
$pub->start;
my $sub = PostgreSQL::Test::Cluster->new('sub');
$sub->init;
$sub->start;

for my $node ($pub, $sub)
{
	$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
	$node->safe_psql('postgres',
		    'CREATE TABLE jb (id int PRIMARY KEY, j jsonb, t text, n int);'
		  . 'ALTER TABLE jb ALTER COLUMN t SET STORAGE EXTERNAL;'
		  . 'CREATE TABLE src (id int, b bytea);');
	# Same logical row on both sides; both copies forged as local VR.
	$node->safe_psql('postgres',
		"SELECT vr_jsonb_cold_forge_class_s('src', 'jb', 1, $payload)");
	like(
		$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
		qr/\bvr=1\b/,
		$node->name . ': local VR seeded');
}

$pub->safe_psql('postgres', 'CREATE PUBLICATION pub_jb FOR TABLE jb');
my $pub_connstr = $pub->connstr . ' dbname=postgres';
$sub->safe_psql('postgres',
	    "CREATE SUBSCRIPTION sub_jb CONNECTION '$pub_connstr' "
	  . "PUBLICATION pub_jb WITH (copy_data = false)");

# Wait until the apply worker is up.
$pub->wait_for_catchup('sub_jb');

# --- pure class-U: only the plain int column changes --------------------
$pub->safe_psql('postgres', 'UPDATE jb SET n = 2 WHERE id = 1');
$pub->wait_for_catchup('sub_jb');
is($sub->safe_psql('postgres', 'SELECT n FROM jb WHERE id = 1'),
	'2', 'pure class-U change applied');
is( $sub->safe_psql('postgres',
		"SELECT j = $payload FROM jb WHERE id = 1"),
	't', 'subscriber retains the logical value');
like(
	$sub->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1\b/,
	'subscriber retains its LOCAL VR representation');

# --- mixed class-U: another column toasts; VR rides the marker ----------
$pub->safe_psql('postgres',
	"UPDATE jb SET t = repeat('y', 8000), n = 3 WHERE id = 1");
$pub->wait_for_catchup('sub_jb');
is($sub->safe_psql('postgres', 'SELECT n FROM jb WHERE id = 1'),
	'3', 'mixed class-U change applied');
is( $sub->safe_psql('postgres',
		"SELECT t = repeat('y', 8000) FROM jb WHERE id = 1"),
	't', 'toasted ordinary column replicated');
is( $sub->safe_psql('postgres',
		"SELECT j = $payload FROM jb WHERE id = 1"),
	't', 'logical value still intact after the mixed change');
like(
	$sub->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1\b/,
	'local VR survives the mixed class-U apply');

# --- datum-printing consumer on the same exposure still refuses ----------
$pub->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('s_td', 'test_decoding')");
$pub->safe_psql('postgres',
	"UPDATE jb SET t = repeat('z', 8000) WHERE id = 1");
my ($ret, $out, $err) = $pub->psql('postgres',
	"SELECT count(*) FROM pg_logical_slot_get_changes('s_td', NULL, NULL)");
isnt($ret, 0, 'datum-printing consumer still fails on mixed class-U');
like(
	$err,
	qr/logical decoding of a value representation is not supported/,
	'N16 refusal unchanged for non-marking consumers');
$pub->safe_psql('postgres', "SELECT pg_drop_replication_slot('s_td')");

# the marker-driven replication kept flowing meanwhile
$pub->wait_for_catchup('sub_jb');
is( $sub->safe_psql('postgres',
		"SELECT t = repeat('z', 8000) FROM jb WHERE id = 1"),
	't', 'replication stream unaffected by the refused side consumer');

for my $node ($pub, $sub)
{
	my $log = slurp_file($node->logfile);
	unlike($log, qr/TRAP|terminated by signal/,
		$node->name . ': no crash in the log');
}

$sub->safe_psql('postgres', 'DROP SUBSCRIPTION sub_jb');
$sub->stop;
$pub->stop;

done_testing();
