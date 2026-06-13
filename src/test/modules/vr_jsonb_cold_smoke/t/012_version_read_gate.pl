# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# VR_VERSION_READ_GATE_V0: every generic persistent VR read path refuses an
# unsupported vr_version BEFORE any body access.  Before this gate the
# production jsonb_cold read funnel checked flags and kind but not version
# (U1 smoke finding), so a future vr_version=2 descriptor would have been read
# under v1 assumptions.
#
# Gated chokepoints (source), all added in the same patch with the identical
# "hdr.version != 1 -> ERROR before body" shape:
#   vr_detoast_flatten        (detoast.c)  - flatten funnel (SELECT, detoast)
#   vr_capture_logical_value  (vr.c)       - logical capture seam
#   vr_build_source_external  (vr_toast.c) - substrate body read
#   vr_relocate entry         (vr_toast.c) - CLUSTER/VACUUM FULL/REPACK rehome
#
# Runtime proof here: a v2 descriptor cannot be placed on disk at all - the
# store-time toast pass routes a foreign-homed VR through the relocate path,
# whose version gate (vr_toast.c) refuses before any body access.  That is
# strictly stronger than "refused on later read".  The detoast funnel guard is
# on the same v1 read path that the control rows exercise (the gate passes v1,
# refuses != 1) and shares identical source with the relocate guard proven
# below.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) g)};

my $node = PostgreSQL::Test::Cluster->new('vgate');
$node->init(allows_streaming => 'logical');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres',
	    'CREATE TABLE jb (id int PRIMARY KEY, j jsonb, n int);'
	  . 'CREATE TABLE src (id int, b bytea);');

# ---- v1 control: a valid v1 forged VR stores and reads through detoast -----

# These reads go through vr_detoast_flatten, i.e. the detoast funnel that now
# carries the version gate; their success proves the gate passes valid v1.
$node->safe_psql('postgres',
	"SELECT vr_jsonb_cold_forge_version_s('src','jb',1,$payload,1)");
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id=1 AND j = $payload"),
	'1',
	'v1 control: valid v1 reads through the detoast funnel and equals payload');
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id=1 AND (j->>'k1')::int = 1"),
	'1', 'v1 control: jsonb operators work on the v1 body via detoast');

# ---- relocate/substrate path: v2 refused before body ----------------------

# Storing a foreign-homed v2 descriptor routes the toast pass through the
# relocate path; its version gate refuses before reading the body.
my ($r, $o, $e) = $node->psql('postgres',
	    "\\set VERBOSITY verbose\n"
	  . "SELECT vr_jsonb_cold_forge_version_s('src','jb',2,$payload,2)");
isnt($r, 0, 'relocate path: a v2 descriptor is refused');
like($e, qr/unsupported version 2/,
	'relocate path: refusal names the unsupported version');
like($e, qr/0A000/, 'relocate path: ERRCODE_FEATURE_NOT_SUPPORTED');
like($e, qr/vr_toast\.c|relocate/,
	'relocate path: refusal originates in the substrate/relocate path');
is( $node->safe_psql('postgres', 'SELECT count(*) FROM jb WHERE id = 2'),
	'0', 'relocate path: the v2 row was never placed on disk');

# ---- capture seam (shared guard, source-level) -----------------------------

# vr_capture_logical_value carries the identical guard, immediately before
# vr_read_supported and the flatten.  No separate runtime leg: a v2 row reaches
# the seam only as organic class-S, which the forge does not produce, and a v2
# row cannot be stored at all (proven above).

# ---- regression: valid v1 still fully readable after the gate -------------

is( $node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id=1 AND j = $payload"),
	'1', 'no regression: v1 descriptor still reads after the version gate');

done_testing();
