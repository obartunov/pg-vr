# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# A-narrow non-regression smoke for REPACK CONCURRENTLY x VR.
#
# This is deliberately deterministic: it does NOT try to trigger the A-narrow
# refusal, which requires a concurrent change committed inside the repack copy
# window (a timing race with no deterministic injection point).  It verifies the
# paths that must stay green:
#   - a pre-existing VR value survives VACUUM FULL / CLUSTER / non-concurrent
#     REPACK (relocate path, home OID intact);
#   - REPACK CONCURRENTLY with no concurrent change leaves the VR value intact;
#   - REPACK CONCURRENTLY of an ordinary-TOAST relation is not falsely refused.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# 2000 distinct keys -> a poorly compressible jsonb that is externalized as a
# persistent VR value.
my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('repack_vr');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
wal_level = replica
max_wal_senders = 4
max_replication_slots = 4
max_worker_processes = 16
autovacuum = off
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');

# Build a pre-existing VR value.  Arm + insert in one session (the selector arm
# is backend-local).
$node->safe_psql(
	'postgres',
	    "CREATE TABLE jr(id int PRIMARY KEY, j jsonb);\n"
	  . "SELECT vr_jsonb_cold_arm('jr'::regclass);\n"
	  . "INSERT INTO jr VALUES (1, $payload);");

like($node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jr'::regclass)"),
	qr/\bvr=1\b.*\bvr_storage_ok=1\b/,
	'pre-existing VR value built (vr=1, home OID ok)');

# Each rewrite driver must leave the VR value intact and home-correct.
my %drivers = (
	'VACUUM FULL'            => 'VACUUM FULL jr',
	'CLUSTER'                => 'CLUSTER jr USING jr_pkey',
	'non-concurrent REPACK'  => 'REPACK jr',
	'REPACK CONCURRENTLY'    => 'REPACK (CONCURRENTLY) jr',
);
for my $name (sort keys %drivers)
{
	my ($rc, $out, $err) = $node->psql('postgres', $drivers{$name});
	is($rc, 0, "$name succeeds on a VR relation");
	like(
		$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jr'::regclass)"),
		qr/\bvr=1\b.*\bvr_storage_ok=1\b/,
		"VR value intact after $name");
}

# The value must still read back correctly after all rewrites.
is( $node->safe_psql('postgres', "SELECT (j->>'k1') FROM jr WHERE id=1"),
	'1', 'VR value readable after rewrites');

# An ordinary-TOAST relation must not be falsely refused by REPACK CONCURRENTLY.
$node->safe_psql(
	'postgres',
	    "CREATE TABLE ot(id int PRIMARY KEY, t text);\n"
	  . "INSERT INTO ot SELECT g, repeat('x'||g, 4000) FROM generate_series(1,50) g;");
my ($rc2, $out2, $err2) = $node->psql('postgres', 'REPACK (CONCURRENTLY) ot');
is($rc2, 0, 'REPACK CONCURRENTLY on an ordinary-TOAST relation is not falsely refused');

$node->stop;
done_testing();
