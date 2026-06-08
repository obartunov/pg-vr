# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# B-repl gate: a persistent value representation cannot be represented by
# logical decoding, so it must be refused at construction on a logically logged
# relation.  Verify that the refusal fires under wal_level=logical (before any
# body is written) and that VR-cold construction still succeeds otherwise.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# 2000 distinct keys -> a poorly compressible jsonb that is externalized, so the
# selector reaches the construction primitive.
my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

# --- wal_level=logical: construction is refused ---
my $log = PostgreSQL::Test::Cluster->new('logical');
$log->init(allows_streaming => 'logical');
$log->start;
$log->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$log->safe_psql('postgres', 'CREATE TABLE jr(id int, j jsonb)');

# arm and insert in one session (the selector arm is backend-local).
my ($ret, $out, $err) = $log->psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jr'::regclass);\n"
	  . "INSERT INTO jr VALUES (1, $payload);");
isnt($ret, 0, 'persistent VR construction fails under logical logging');
like(
	$err,
	qr/persistent value representation is not supported on logically logged relations/,
	'gate reports the expected error');
is( $log->safe_psql('postgres', 'SELECT count(*) FROM jr'),
	'0', 'refusal precedes body write: no row inserted');
$log->stop;

# --- default (wal_level=replica): construction still succeeds ---
my $rep = PostgreSQL::Test::Cluster->new('replica');
$rep->init;
$rep->start;
$rep->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$rep->safe_psql('postgres', 'CREATE TABLE jr(id int, j jsonb)');
my $probe = $rep->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jr'::regclass);\n"
	  . "INSERT INTO jr VALUES (1, $payload);\n"
	  . "SELECT vr_jsonb_cold_probe('jr'::regclass);");
like($probe, qr/\bvr=1\b/,
	'VR-cold construction succeeds when not logically logged');
$rep->stop;

done_testing();
