# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# vr_logical_capture_limit is a PGC_SIGHUP bound: it applies in the process
# performing the capture (e.g. the REPACK decoding worker), so a session SET
# would be misleading and is not allowed.  Exercise the threshold through
# configuration reload: refusal at 0 (refuse all) and at a small nonzero
# limit, recovery at the default.  Determinism: each probe below opens a new
# backend after poll_query_until confirmed the reloaded value, and the
# refusal depends only on the value header and the GUC, before any body
# access.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('limit');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres', 'CREATE TABLE jc (id int, j jsonb)');
$node->safe_psql('postgres',
	    "SELECT vr_jsonb_cold_arm('jc'::regclass);\n"
	  . "INSERT INTO jc VALUES (1, $payload);");
like(
	$node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jc')"),
	qr/\bvr=1\b/,
	'value stored as VR');

my $capture_sql = 'SELECT vr_jsonb_cold_capture(j) = j FROM jc WHERE id = 1';

# A session SET must be rejected: the limit is not a per-session knob.
my ($ret, $out, $err) =
  $node->psql('postgres', 'SET vr_logical_capture_limit = 0');
isnt($ret, 0, 'session SET of the capture limit is rejected');

sub set_limit
{
	my ($node, $value) = @_;
	$node->safe_psql('postgres',
		"ALTER SYSTEM SET vr_logical_capture_limit = '$value'");
	$node->reload;
	$node->poll_query_until('postgres',
		"SELECT current_setting('vr_logical_capture_limit') = '$value'")
	  or BAIL_OUT("reload to vr_logical_capture_limit=$value not observed");
	return;
}

# default (1MB): the ~40kB value captures
is($node->safe_psql('postgres', $capture_sql),
	't', 'capture succeeds under the default limit');

# 0 refuses every VR value
set_limit($node, '0');
($ret, $out, $err) = $node->psql('postgres', $capture_sql);
isnt($ret, 0, 'limit 0 refuses capture');
like(
	$err,
	qr/logical capture of a value representation exceeds "vr_logical_capture_limit"/,
	'refusal reports the limit');

# a small nonzero limit still refuses this value (logical size > 16kB)
set_limit($node, '16kB');
($ret, $out, $err) = $node->psql('postgres', $capture_sql);
isnt($ret, 0, 'limit 16kB refuses capture');
like($err, qr/limit is 16384 bytes/, 'refusal reports deterministic sizes');

# recovery at the default
set_limit($node, '1MB');
is($node->safe_psql('postgres', $capture_sql),
	't', 'capture succeeds again after raising the limit');

$node->stop;

done_testing();
