# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# VR_INSPECTION_SURFACE_IMPL_V0: typed admin inspection surface vr_inspect(rel)
# plus the explicit re-representation API (vr_rerepresent_column), both in the
# sanctioned contrib extension vr_admin. The smoke extension is loaded only to
# set up VR states (arm/probe) and force inline-compressed / below-floor cases.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $big   = q{(SELECT jsonb_object_agg('k'||g, md5(g::text)) FROM generate_series(1,200) x(g))};
my $small = q{'{"a":1}'::jsonb};

my $node = PostgreSQL::Test::Cluster->new('vradmin');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION vr_admin');
$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');

# helper: fetch one column's vr_inspect row as a pipe-joined string
my $cols = 'attname,marked,rows_total,rows_vr,rows_ordinary,rows_below_floor,'
         . 'rows_above_floor,rows_would_convert,rows_null,rows_storage_bad,'
         . 'storage_ok,logical_size_min,logical_size_max';

# --- empty relation ---------------------------------------------------------
$node->safe_psql('postgres', 'CREATE TABLE e (id int, j jsonb)');
is( $node->safe_psql('postgres',
		"SELECT rows_total||'|'||rows_vr||'|'||storage_ok FROM vr_inspect('e')"),
	'0|0|true', 'empty relation: zero counts, storage_ok true');

# --- all VR -----------------------------------------------------------------
$node->safe_psql('postgres', 'CREATE TABLE av (id int primary key, j jsonb)');
$node->safe_psql('postgres', 'ALTER TABLE av ALTER COLUMN j SET (vr_jsonb_cold = on)');
$node->safe_psql('postgres', "INSERT INTO av SELECT g, $big FROM generate_series(1,5) g");
is( $node->safe_psql('postgres',
		"SELECT rows_total||'|'||rows_vr||'|'||rows_ordinary||'|'||rows_above_floor||'|'||rows_would_convert FROM vr_inspect('av')"),
	'5|5|0|0|0', 'all-VR: rows_vr==total, no ordinary/above_floor/would_convert');
is( $node->safe_psql('postgres',
		"SELECT (logical_size_min IS NOT NULL) AND (logical_size_max IS NOT NULL) FROM vr_inspect('av')"),
	't', 'all-VR: logical_size_min/max present');

# --- all ordinary above floor (marked) -> would_convert == total ------------
$node->safe_psql('postgres', 'CREATE TABLE ao (id int primary key, j jsonb)');
$node->safe_psql('postgres', "INSERT INTO ao SELECT g, $big FROM generate_series(1,4) g");
$node->safe_psql('postgres', 'ALTER TABLE ao ALTER COLUMN j SET (vr_jsonb_cold = on)');  # mark after insert -> ordinary
is( $node->safe_psql('postgres',
		"SELECT rows_vr||'|'||rows_above_floor||'|'||rows_would_convert||'|'||COALESCE(logical_size_min::text,'NULL') FROM vr_inspect('ao')"),
	'0|4|4|NULL', 'all-ordinary-above-floor marked: above_floor=would_convert=4, no VR, logical_size_min NULL');

# --- unmarked jsonb above floor: above_floor counted, would_convert 0 -------
$node->safe_psql('postgres', 'CREATE TABLE un (id int primary key, j jsonb)');
$node->safe_psql('postgres', "INSERT INTO un SELECT g, $big FROM generate_series(1,3) g");  # never marked
is( $node->safe_psql('postgres',
		"SELECT marked||'|'||rows_above_floor||'|'||rows_would_convert FROM vr_inspect('un')"),
	'false|3|0', 'unmarked jsonb: marked=false, above_floor=3, would_convert=0');

# --- mixed: VR + ordinary-above + below-floor + NULL -------------------------
$node->safe_psql('postgres', 'CREATE TABLE mx (id int primary key, j jsonb)');
$node->safe_psql('postgres', 'ALTER TABLE mx ALTER COLUMN j SET (vr_jsonb_cold = on)');
$node->safe_psql('postgres', "INSERT INTO mx SELECT 1, $big");          # VR (marked, above floor, new write)
$node->safe_psql('postgres', "INSERT INTO mx SELECT 2, $small");        # below floor -> ordinary
$node->safe_psql('postgres', "INSERT INTO mx SELECT 3, NULL");          # null
# add an ordinary-above-floor by inserting big into an unmarked moment: reset, insert, re-set
$node->safe_psql('postgres', 'ALTER TABLE mx ALTER COLUMN j RESET (vr_jsonb_cold)');
$node->safe_psql('postgres', "INSERT INTO mx SELECT 4, $big");          # ordinary above floor
$node->safe_psql('postgres', 'ALTER TABLE mx ALTER COLUMN j SET (vr_jsonb_cold = on)');
is( $node->safe_psql('postgres',
		"SELECT rows_total||'|'||rows_vr||'|'||rows_below_floor||'|'||rows_above_floor||'|'||rows_would_convert||'|'||rows_null FROM vr_inspect('mx')"),
	'4|1|1|1|1|1', 'mixed: each bucket exact (1 VR, 1 below, 1 above=would_convert, 1 null)');

# --- multiple jsonb columns: one row per column -----------------------------
$node->safe_psql('postgres', 'CREATE TABLE two (id int primary key, a jsonb, b jsonb, t text)');
$node->safe_psql('postgres', 'ALTER TABLE two ALTER COLUMN a SET (vr_jsonb_cold = on)');
$node->safe_psql('postgres', "INSERT INTO two SELECT 1, $big, $big, 'x'");
is( $node->safe_psql('postgres', "SELECT count(*) FROM vr_inspect('two')"),
	'2', 'multiple jsonb columns: one row per jsonb column (text excluded)');
is( $node->safe_psql('postgres',
		"SELECT string_agg(attname||':'||rows_vr, ',' ORDER BY attname) FROM vr_inspect('two')"),
	'a:1,b:0', 'per-column accounting independent: a is VR, b ordinary');

# --- inline-compressed exactness: a compressible jsonb between floor/2 and floor
# build a highly compressible big jsonb that stays inline-compressed and is
# below the 4k flat floor only if measured by compressed size; vr_inspect must
# detoast it and classify by the EXACT flat size.
$node->safe_psql('postgres', 'CREATE TABLE ic (id int primary key, j jsonb)');
# repetitive payload compresses well; flat size well above floor
$node->safe_psql('postgres',
	"INSERT INTO ic SELECT 1, (SELECT jsonb_object_agg('k'||g, 'aaaaaaaaaaaaaaaaaaaa') FROM generate_series(1,400) x(g))");
my $icrow = $node->safe_psql('postgres',
	"SELECT rows_above_floor||'|'||rows_below_floor FROM vr_inspect('ic')");
is($icrow, '1|0',
	'inline-compressed jsonb classified by EXACT flat size (above floor), not compressed size');

# --- rows_storage_bad = 0 on healthy rows -----------------------------------
is( $node->safe_psql('postgres', "SELECT rows_storage_bad FROM vr_inspect('av')"),
	'0', 'healthy VR rows: rows_storage_bad = 0');

# --- after re-representation: above_floor / would_convert decrease ----------
my $before = $node->safe_psql('postgres',
	"SELECT rows_would_convert FROM vr_inspect('ao')");
is($before, '4', 're-representation precondition: would_convert=4');
$node->safe_psql('postgres', "SELECT vr_rerepresent_column('ao','j')");
is( $node->safe_psql('postgres',
		"SELECT rows_vr||'|'||rows_above_floor||'|'||rows_would_convert FROM vr_inspect('ao')"),
	'4|0|0', 'after re-representation: rows_vr=4, above_floor=would_convert=0');

# --- VACUUM FULL / CLUSTER preserve storage_ok ------------------------------
$node->safe_psql('postgres', 'VACUUM FULL av');
is( $node->safe_psql('postgres', "SELECT storage_ok||'|'||rows_vr FROM vr_inspect('av')"),
	'true|5', 'VACUUM FULL preserves storage_ok and VR rows');
$node->safe_psql('postgres', 'CREATE INDEX avi ON av(id)');
$node->safe_psql('postgres', 'CLUSTER av USING avi');
is( $node->safe_psql('postgres', "SELECT storage_ok||'|'||rows_vr FROM vr_inspect('av')"),
	'true|5', 'CLUSTER preserves storage_ok and VR rows');

$node->stop;
done_testing();
