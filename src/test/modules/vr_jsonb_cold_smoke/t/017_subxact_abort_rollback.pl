# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# Transaction / subtransaction abort lifetime of a persistent VR body.
#
# Claim under test: a VR body is written through the core TOAST writer
# (vr_toast_body_save -> toast_save_datum), carries the inserting
# (sub)transaction's xid like any ordinary TOAST chunk, and registers no
# VR-private xact/subxact callback.  Therefore abort, ROLLBACK TO SAVEPOINT,
# and rolled-back COPY/localize are handled by the SAME MVCC + VACUUM rules
# as ordinary TOAST: a rolled-back body becomes a dead, invisible toast tuple
# and is reclaimed by VACUUM; a committed body stays live.  No orphan body in
# live storage, no stale locator reachable from a live row, no visible broken
# VR row.
#
# Persistent per-column policy (vr_jsonb_cold = on) chooses VR for new writes
# in any backend (E24), so the abort/savepoint sequences run as ordinary
# sessions.  Default wal_level=replica: not logically logged, so construction
# proceeds without the C1 logical gate.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $payload  = q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) x(g))};
my $payload2 = q{(SELECT jsonb_object_agg('m'||g, g) FROM generate_series(1,2000) x(g))};
my $payload3 = q{(SELECT jsonb_object_agg('z'||g, g) FROM generate_series(1,2000) x(g))};

my $tmp = PostgreSQL::Test::Utils::tempdir();

my $node = PostgreSQL::Test::Cluster->new('vrabort');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'vr_jsonb_cold_smoke'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$node->safe_psql('postgres', 'CREATE TABLE jb (id int PRIMARY KEY, j jsonb)');
$node->safe_psql('postgres',
	'ALTER TABLE jb ALTER COLUMN j SET (vr_jsonb_cold = on)');

# sanity: the policy actually builds a VR for a committed control row
$node->safe_psql('postgres', "INSERT INTO jb VALUES (100, $payload)");
like($node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=1\b.*\bvr_storage_ok=1\b/,
	'control: per-column policy constructs a VR body');

# ---------- 1. INSERT VR row then ROLLBACK ----------
$node->safe_psql('postgres',
	"BEGIN; INSERT INTO jb VALUES (1, $payload); ROLLBACK;");
is($node->safe_psql('postgres', 'SELECT count(*) FROM jb WHERE id = 1'),
	'0', 'INSERT VR + ROLLBACK leaves no visible row');

# ---------- 2. INSERT in SAVEPOINT, ROLLBACK TO SAVEPOINT, outer COMMIT ----
$node->safe_psql('postgres',
	    "BEGIN;\n"
	  . "  INSERT INTO jb VALUES (10, $payload);\n"
	  . "  SAVEPOINT sp;\n"
	  . "  INSERT INTO jb VALUES (11, $payload);\n"
	  . "  ROLLBACK TO SAVEPOINT sp;\n"
	  . "COMMIT;");
is($node->safe_psql('postgres',
		"SELECT count(*) FROM jb WHERE id = 10 AND j = $payload"),
	'1', 'subxact: outer-committed VR row survives and is readable');
is($node->safe_psql('postgres', 'SELECT count(*) FROM jb WHERE id = 11'),
	'0', 'subxact: row rolled back to savepoint is invisible');

# ---------- 3. UPDATE to a new VR body then ROLLBACK ----------
$node->safe_psql('postgres', "INSERT INTO jb VALUES (20, $payload)");
$node->safe_psql('postgres',
	"BEGIN; UPDATE jb SET j = $payload2 WHERE id = 20; ROLLBACK;");
is($node->safe_psql('postgres',
		"SELECT j = $payload FROM jb WHERE id = 20"),
	't', 'UPDATE VR body + ROLLBACK restores the original body');

# ---------- 4. UPDATE inside SAVEPOINT, ROLLBACK TO SAVEPOINT, outer COMMIT -
$node->safe_psql('postgres',
	    "BEGIN;\n"
	  . "  UPDATE jb SET j = $payload2 WHERE id = 20;\n"
	  . "  SAVEPOINT sp;\n"
	  . "  UPDATE jb SET j = $payload3 WHERE id = 20;\n"
	  . "  ROLLBACK TO SAVEPOINT sp;\n"
	  . "COMMIT;");
is($node->safe_psql('postgres',
		"SELECT j = $payload2 FROM jb WHERE id = 20"),
	't', 'subxact UPDATE: outer-committed body kept, inner rolled back');

# ---------- 5. COPY FROM producing VR rows then ROLLBACK ----------
my $f = "$tmp/vr_copy.dat";
$node->safe_psql('postgres',
	"COPY (SELECT g, $payload FROM generate_series(40,41) g) TO '$f'");
$node->safe_psql('postgres', "BEGIN; COPY jb FROM '$f'; ROLLBACK;");
is($node->safe_psql('postgres', 'SELECT count(*) FROM jb WHERE id IN (40,41)'),
	'0', 'COPY FROM VR rows + ROLLBACK leaves no visible row');

# ---------- 7. foreign localize-on-write inside an aborted subxact ----------
$node->safe_psql('postgres',
	    'CREATE TABLE jb2 (id int PRIMARY KEY, j jsonb);'
	  . 'ALTER TABLE jb2 ALTER COLUMN j SET (vr_jsonb_cold = on);');
$node->safe_psql('postgres',
	    "BEGIN;\n"
	  . "  SAVEPOINT sp;\n"
	  . "  INSERT INTO jb2 SELECT id, j FROM jb;\n"   # reads VR, localizes new local VR
	  . "  ROLLBACK TO SAVEPOINT sp;\n"
	  . "COMMIT;");
is($node->safe_psql('postgres', 'SELECT count(*) FROM jb2'),
	'0', 'localize-on-write in aborted subxact leaves no visible row');

# ---------- 6. mixed survivors + orphan-body accounting ----------
$node->safe_psql('postgres', 'VACUUM jb; VACUUM jb2;');

# survivors: control id=100 (payload), id=10 (payload), id=20 (payload2) = 3 VR rows
is($node->safe_psql('postgres', 'SELECT count(*) FROM jb'),
	'3', 'only committed rows survive (100,10,20)');
like($node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('jb')"),
	qr/\bvr=3\b.*\bvr_storage_ok=1\b/,
	'survivors are VR, storage ok, no broken VR row');
is($node->safe_psql('postgres',
		"SELECT bool_and(ok) FROM (VALUES "
	  . "(100, ($payload)), (10, ($payload)), (20, ($payload2))) v(id,p), "
	  . "LATERAL (SELECT jb.j = v.p AS ok FROM jb WHERE jb.id = v.id) s"),
	't', 'every surviving row carries its expected logical value');

my $oid = $node->safe_psql('postgres', "SELECT 'jb'::regclass::oid");
is($node->safe_psql('postgres',
		"SELECT count(DISTINCT chunk_id) FROM pg_toast.pg_toast_$oid"),
	'3',
	'orphan-body accounting jb: live bodies (3) == committed VR rows; aborted bodies reclaimed');

my $oid2 = $node->safe_psql('postgres', "SELECT 'jb2'::regclass::oid");
is($node->safe_psql('postgres',
		"SELECT count(DISTINCT chunk_id) FROM pg_toast.pg_toast_$oid2"),
	'0',
	'orphan-body accounting jb2: no body left by the aborted localize');

# ---------- boundary hygiene ----------
my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP|terminated by signal|server process.*exited/,
	'no crash in the server log');

$node->stop;
done_testing();
