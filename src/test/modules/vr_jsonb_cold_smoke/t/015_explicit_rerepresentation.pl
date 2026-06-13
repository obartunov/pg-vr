# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# VR_EXPLICIT_REREPRESENTATION_COMMAND_IMPL_V0 (E26): explicit, admin-triggered
# re-representation of existing ordinary jsonb rows into VR_KIND_JSONB_COLD via
# vr_rerepresent_column(rel, attname). It re-stores ordinary values through the
# ordinary write path (UPDATE ... SET col = vr_force_fresh(col) WHERE ctid IN
# ordinary rows), so the in-core E24 selector decides VR; already-VR rows are
# skipped, never rebuilt. No VACUUM FULL, no second policy, composes with the
# vr_logical_construction veto.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $obj = q{(SELECT jsonb_object_agg('z'||(999-x)||'k'||x, md5(x::text)) FROM generate_series(1,200) y(x))};
my $arr = q{(SELECT jsonb_agg(md5(x::text)) FROM generate_series(1,300) y(x))};
my $scal = q{to_jsonb((SELECT string_agg(md5(g::text),'') FROM generate_series(1,300) g))};
my $small = q{'{"a":1}'::jsonb};

# ---------------------------------------------------------------------------
# Node A: non-logical. Conversion + shape + skip + idempotency.
# ---------------------------------------------------------------------------
my $a = PostgreSQL::Test::Cluster->new('vrre_a');
$a->init;
$a->start;
$a->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');

# Insert ordinary (no policy), then enable policy.
$a->safe_psql('postgres', 'CREATE TABLE t (id int PRIMARY KEY, j jsonb)');
$a->safe_psql('postgres', "INSERT INTO t VALUES
  (1, $obj), (2, $arr), (3, $scal), (4, $small)");
$a->safe_psql('postgres', 'ALTER TABLE t ALTER COLUMN j SET (vr_jsonb_cold = on)');

# capture canonical bytes before conversion
$a->safe_psql('postgres', 'CREATE TABLE cap AS SELECT id, j::text::bytea AS b FROM t');

is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('t') ~ 'vr=0'"),
	't', 'baseline: all rows ordinary before re-representation');

# 1 + 2: convert; large object/array/scalar -> VR, below-floor small stays ordinary.
my $rep = $a->safe_psql('postgres', "SELECT vr_rerepresent_column('t','j')");
note "report: $rep";
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('t') ~ 'vr=3'"),
	't', '1: three above-floor rows converted to VR (object, array, scalar)');
# row 4 (small) remained ordinary
is( $a->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE id = 4"),
	'1', '2: below-floor small jsonb row still present');
like($rep, qr/rows_converted=3/, '1: report rows_converted=3');
like($rep, qr/rows_declined=1/, '2: report rows_declined=1 (the small row)');

# 5,6,7,8: logical + byte equality preserved for object, array, scalar, unordered keys.
is( $a->safe_psql('postgres', "SELECT bool_and(t.j::text::bytea = cap.b) FROM t JOIN cap USING (id)"),
	't', '5-8: object/array/scalar incl. unordered keys are byte-identical after conversion');

# 4: existing VR row is skipped, not rebuilt/duplicated.
my $rep2 = $a->safe_psql('postgres', "SELECT vr_rerepresent_column('t','j')");
note "rerun report: $rep2";
like($rep2, qr/rows_already_vr=3/, '4: rerun counts the 3 VR rows as already_vr');
# 9: idempotent — rerun converts nothing new.
like($rep2, qr/rows_converted=0/, '9: idempotent rerun converts nothing new');
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('t') ~ 'vr=3'"),
	't', '9: still exactly 3 VR rows after rerun (no duplication)');

# 3: unmarked column converts nothing (documented behavior: convert nothing, report 0).
$a->safe_psql('postgres', 'CREATE TABLE u (id int PRIMARY KEY, j jsonb)');
$a->safe_psql('postgres', "INSERT INTO u VALUES (1, $obj)");   # no policy
my $repu = $a->safe_psql('postgres', "SELECT vr_rerepresent_column('u','j')");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('u') ~ 'vr=0'"),
	't', '3: unmarked column converts nothing');
like($repu, qr/rows_converted=0/, '3: unmarked column report rows_converted=0');

# 12: VACUUM FULL still does not ordinary->VR convert (E25 absent).
$a->safe_psql('postgres', 'ALTER TABLE u ALTER COLUMN j SET (vr_jsonb_cold = on)');
$a->safe_psql('postgres', 'VACUUM FULL u');
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('u') ~ 'vr=0'"),
	't', '12: VACUUM FULL does not convert ordinary->VR (E25 remains absent)');

$a->stop;

# ---------------------------------------------------------------------------
# Node B: logical. Veto interaction.
# ---------------------------------------------------------------------------
my $b = PostgreSQL::Test::Cluster->new('vrre_b');
$b->init(allows_streaming => 'logical');
$b->start;
$b->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$b->safe_psql('postgres', 'CREATE TABLE g (id int PRIMARY KEY, j jsonb)');
$b->safe_psql('postgres', "INSERT INTO g VALUES (1, $obj)");
$b->safe_psql('postgres', 'ALTER TABLE g ALTER COLUMN j SET (vr_jsonb_cold = on)');

# 10: logically logged + veto off -> fail closed, convert nothing.
my ($r, $o, $e) = $b->psql('postgres',
	    "\\set VERBOSITY verbose\n"
	  . "SELECT vr_rerepresent_column('g','j')");
isnt($r, 0, '10: logically logged + veto off: re-representation fails closed');
like($e, qr/logically logged/, '10: failure cites the logical-logging veto');
is( $b->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('g') ~ 'vr=0'"),
	't', '10: nothing converted while veto off');

# 11: veto on -> converts.
$b->safe_psql('postgres', 'ALTER SYSTEM SET vr_logical_construction = on');
$b->safe_psql('postgres', 'SELECT pg_reload_conf()');
my $repg = $b->safe_psql('postgres', "SELECT vr_rerepresent_column('g','j')");
is( $b->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('g') ~ 'vr=1'"),
	't', '11: with veto on, re-representation converts');
like($repg, qr/rows_converted=1/, '11: report rows_converted=1');

$b->stop;
done_testing();
