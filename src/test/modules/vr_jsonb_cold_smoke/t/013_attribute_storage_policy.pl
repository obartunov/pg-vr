# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# VR_STORAGE_POLICY_ATTRIBUTE_OPTION_V0 (E24, interpretation B):
# durable per-column VR storage policy plumbing. A jsonb column carries the
# policy as an attribute reloption (vr_jsonb_cold = on) stored in
# pg_attribute.attoptions; the sanctioned reader vr_attribute_storage_policy()
# exposes it from the relcache; the test selector consumes it so that NEW
# writes under the policy choose VR_KIND_JSONB_COLD. Core installs no
# autonomous selector here (later gate); the test module supplies the selector
# and is preloaded so every backend sees it.
#
# Node A (default wal_level=replica): policy plumbing legs.
# Node B (wal_level=logical): vr_logical_construction independent-veto leg.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload =
  q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) x(g))};

# ---------------------------------------------------------------------------
# Node A: non-logically-logged. Policy plumbing.
# ---------------------------------------------------------------------------
my $a = PostgreSQL::Test::Cluster->new('vrpolA');
$a->init;
$a->append_conf('postgresql.conf',
	"shared_preload_libraries = 'vr_jsonb_cold_smoke'");
$a->start;
$a->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$a->safe_psql('postgres',
	'CREATE TABLE t (id int PRIMARY KEY, j jsonb, k jsonb);');

# set per-column policy on j only
$a->safe_psql('postgres',
	'ALTER TABLE t ALTER COLUMN j SET (vr_jsonb_cold = on)');
is( $a->safe_psql('postgres',
		"SELECT attoptions::text FROM pg_attribute WHERE attrelid='t'::regclass AND attname='j'"),
	'{vr_jsonb_cold=on}',
	'policy stored in pg_attribute.attoptions on column j');
is( $a->safe_psql('postgres',
		"SELECT coalesce(attoptions::text,'<null>') FROM pg_attribute WHERE attrelid='t'::regclass AND attname='k'"),
	'<null>',
	'sibling column k carries no policy (per-column, not per-table)');

# new write under policy becomes VR (fresh session, NO backend-local arm)
$a->safe_psql('postgres', "INSERT INTO t SELECT 1, $payload, $payload");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('t')"),
	'rows=1 vr=1 ondisk=0 inline=0 vr_storage_ok=1 logical_size=40900 comp=pglz',
	'new write under per-column policy chose VR for column j (relcache lookup, no arm)');
is( $a->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE id=1 AND j = $payload AND k = $payload"),
	'1', 'logical values preserved for both columns');

# reads do not consult the option: reset policy, the existing VR row still reads
$a->safe_psql('postgres', 'ALTER TABLE t ALTER COLUMN j RESET (vr_jsonb_cold)');
is( $a->safe_psql('postgres', "SELECT count(*) FROM t WHERE id=1 AND j = $payload"),
	'1', 'existing VR row still readable after policy reset (reads ignore the option)');
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('t') ~ 'vr=1'"),
	't', 'policy reset did not re-represent the existing VR row');

# existing ordinary rows are not re-represented when policy turns on
$a->safe_psql('postgres', 'CREATE TABLE u (id int PRIMARY KEY, j jsonb)');
$a->safe_psql('postgres', "INSERT INTO u SELECT 1, $payload");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('u') ~ 'vr=0'"),
	't', 'pre-policy row is ordinary');
$a->safe_psql('postgres', 'ALTER TABLE u ALTER COLUMN j SET (vr_jsonb_cold = on)');
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('u') ~ 'vr=0'"),
	't', 'turning policy on does NOT re-represent the existing ordinary row (new-writes-only)');
# but a fresh write under the now-active policy IS VR
$a->safe_psql('postgres', "INSERT INTO u SELECT 2, $payload");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('u') ~ 'vr=1'"),
	't', 'a new write under the active policy becomes VR');

$a->stop;

# ---------------------------------------------------------------------------
# Node B: logically logged. Policy must NOT override the M3 gate.
# ---------------------------------------------------------------------------
my $b = PostgreSQL::Test::Cluster->new('vrpolB');
$b->init(allows_streaming => 'logical');
$b->append_conf('postgresql.conf',
	"shared_preload_libraries = 'vr_jsonb_cold_smoke'");
$b->start;
$b->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$b->safe_psql('postgres', 'CREATE TABLE g (id int PRIMARY KEY, j jsonb)');
$b->safe_psql('postgres', 'ALTER TABLE g ALTER COLUMN j SET (vr_jsonb_cold = on)');

# gate off (default): policy + logically logged -> construction refused
my ($r, $o, $e) = $b->psql('postgres',
	    "\\set VERBOSITY verbose\n"
	  . "INSERT INTO g SELECT 1, $payload");
isnt($r, 0, 'policy + logically logged + gate off: construction refused (independent veto)');
like($e, qr/logically logged|vr_logical_construction/,
	'refusal is the vr_logical_construction gate, not a policy decision');

# gate on: same policy now constructs VR (gate composes with, does not replace, policy)
$b->safe_psql('postgres', 'ALTER SYSTEM SET vr_logical_construction = on');
$b->safe_psql('postgres', 'SELECT pg_reload_conf()');
$b->safe_psql('postgres', "INSERT INTO g SELECT 2, $payload");
is( $b->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('g') ~ 'vr=1'"),
	't', 'with the gate on, the same per-column policy constructs VR');

$b->stop;
done_testing();
