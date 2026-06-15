# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# VR_JSONB_CONSUMER_AUDIT_V0 -- jsonb consumer boundary regression matrix.
#
# Source audit conclusion (no mechanism gap): every jsonb value consumer
# obtains its Jsonb through PG_GETARG_JSONB_P / DatumGetJsonbP, both of which are
#   (Jsonb *) PG_DETOAST_DATUM(d)   (jsonb.h:401,418)
# and PG_DETOAST_DATUM routes through detoast_attr / detoast_external_attr, which
# are VR-aware: vr_detoast_flatten (detoast.c:55) recognises the inline VR header,
# rejects unknown persistent flag bits, and flattens to an ordinary logical Jsonb
# before any layout access.  No consumer casts a raw datum/pointer to Jsonb
# without detoast (verified: no `(Jsonb *) DatumGetPointer/PG_GETARG_POINTER`
# anywhere).  jsonb_util.c machinery (compareJsonbContainers, iterators,
# JsonbValueToJsonb) only ever sees already-flattened Jsonb*/JsonbContainer*.
#
# This test closes the *coverage* gap: it proves, at runtime, that a VR-jsonb
# value and an ordinary-jsonb value carrying the SAME logical value are
# indistinguishable to every important consumer entry point, and that read-side
# consumption does not flatten / leak / orphan the stored VR.
#
# Method: two tables with identical per-id payloads --
#   co (ordinary jsonb)   vs   cv (VR jsonb: column armed vr_jsonb_cold=on).
# For each consumer the co-result must equal the cv-result.  GIN tests also
# assert (EXPLAIN) that the index path is actually used.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $N = 60;    # enough rows that GIN is chosen with seqscan off

# Payload per id g: 2000 keys (drives the VR size floor) + structural keys
# 'a' (nested object), 'arr' (array), 'tag' (string) for the operator matrix.
my $payload = q{(
    (SELECT jsonb_object_agg('k'||k, k) FROM generate_series(1,2000) k)
    || jsonb_build_object(
         'a',   jsonb_build_object('b', g, 'c', g*2),
         'arr', to_jsonb(ARRAY[g, g+1, g+2]),
         'tag', 'row'||g)
)};

my $node = PostgreSQL::Test::Cluster->new('consumer');
$node->init;    # default wal_level = replica -> selector builds VR on insert
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'vr_jsonb_cold_smoke'");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');

sub q1 { return $node->safe_psql('postgres', $_[0]); }
sub probe { return $node->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('$_[0]')"); }

sub body_count
{
	my $tbl = shift;
	my $toast = q1("SELECT reltoastrelid::regclass::text FROM pg_class WHERE oid='$tbl'::regclass");
	return '0' if (!defined $toast || $toast eq '' || $toast eq '-');
	return q1("SELECT count(DISTINCT chunk_id) FROM $toast");
}

# co = ordinary jsonb; cv = VR jsonb (armed). Identical logical payload per id.
q1('CREATE TABLE co (id int PRIMARY KEY, j jsonb)');
q1('CREATE TABLE cv (id int PRIMARY KEY, j jsonb)');
q1('ALTER TABLE cv ALTER COLUMN j SET (vr_jsonb_cold = on)');
q1("INSERT INTO co SELECT g, $payload FROM generate_series(1,$N) g");
q1("INSERT INTO cv SELECT g, $payload FROM generate_series(1,$N) g");

# Preconditions: cv really is VR (all rows), co is not.
like(probe('cv'), qr/\bvr=$N\b.*\bvr_storage_ok=1\b/, "cv: $N local VR rows, vr_storage_ok=1");
like(probe('co'), qr/\bvr=0\b/, "co: ordinary jsonb (vr=0)");
is(body_count('cv'), "$N", "cv: $N physical VR bodies");

# agree($label, $expr) : the scalar/aggregate $expr (using table alias T) must
# return the same value over co and cv.  Compared as a single aggregate string
# over all rows to keep it one round-trip and order-stable.
sub agree
{
	my ($label, $expr) = @_;
	my $o = q1("SELECT md5(string_agg(($expr)::text, '|' ORDER BY id)) FROM co T");
	my $v = q1("SELECT md5(string_agg(($expr)::text, '|' ORDER BY id)) FROM cv T");
	is($v, $o, "consumer agrees co==cv: $label");
}

# count_agree($label, $where) : COUNT(*) under a predicate must match.
sub count_agree
{
	my ($label, $where) = @_;
	my $o = q1("SELECT count(*) FROM co T WHERE $where");
	my $v = q1("SELECT count(*) FROM cv T WHERE $where");
	is($v, $o, "consumer agrees co==cv (count=$o): $label");
}

# --- 1. extraction operators ---------------------------------------------
agree('-> object',   q{T.j -> 'a'});
agree('->> text',    q{T.j ->> 'tag'});
agree('-> array',    q{T.j -> 'arr'});
agree('#> path',     q{T.j #> '{a,b}'});
agree('#>> path',    q{T.j #>> '{a,b}'});
# subscripting fetch (jsonbsubs.c jsonb_subscript_fetch -> DatumGetJsonbP):
agree('subscript [text key]', q{T.j['tag']});
agree('subscript [key][idx]', q{T.j['arr'][0]});

# --- 2. existence operators ----------------------------------------------
count_agree('? existing key',   q{T.j ? 'k1'});
count_agree('? structural key', q{T.j ? 'a'});
count_agree('? missing key',    q{T.j ? 'nope'});
count_agree('?| any',           q{T.j ?| array['nope','tag']});
count_agree('?& all',           q{T.j ?& array['a','arr','tag']});
count_agree('?& missing',       q{T.j ?& array['a','nope']});

# --- 3. containment operators --------------------------------------------
count_agree('@> scalar',  q{T.j @> '{"tag":"row5"}'});
count_agree('@> nested',  q{T.j @> jsonb_build_object('a', jsonb_build_object('b', 1))});
count_agree('@> none',    q{T.j @> '{"tag":"nope"}'});
count_agree('<@ superset',q{T.j <@ (T.j || '{"extra":1}')});

# --- 4. equality / order / hash ------------------------------------------
# logical equality across representations: every cv row equals its co twin.
is(q1("SELECT count(*) FROM co JOIN cv USING (id) WHERE co.j = cv.j"),
	"$N", "equality: VR row = ordinary twin for all $N rows");
is(q1("SELECT count(*) FROM co JOIN cv USING (id) WHERE co.j IS DISTINCT FROM cv.j"),
	'0', "equality: no VR/ordinary twin differs");
# ordering (jsonb_cmp): id order produced by ORDER BY j must match.
agree('order by j (jsonb_cmp)', q{T.id});    # md5 of ids in id-order; see below
{
	my $o = q1("SELECT md5(string_agg(id::text, '|' ORDER BY j, id)) FROM co");
	my $v = q1("SELECT md5(string_agg(id::text, '|' ORDER BY j, id)) FROM cv");
	is($v, $o, "consumer agrees co==cv: ORDER BY j (jsonb_cmp ordering)");
}
# hash (jsonb_hash via GROUP BY): distinct-group count matches.
is(q1("SELECT count(*) FROM (SELECT j FROM cv GROUP BY j) x"),
	q1("SELECT count(*) FROM (SELECT j FROM co GROUP BY j) x"),
	"hash: GROUP BY j distinct-group count matches");

# --- 5. modification / build paths (result must equal ordinary result) ----
agree('jsonb_set',    q{jsonb_set(T.j, '{a,b}', '999')});
agree('jsonb_insert', q{jsonb_insert(T.j, '{newk}', '42')});
agree('- delete key', q{T.j - 'tag'});
agree('concat ||',    q{T.j || '{"z":1}'});
agree('jsonb_build',  q{jsonb_build_object('w', T.j -> 'a')});
# jsonb_agg / jsonb_object_agg consume jsonb values:
is(q1("SELECT md5((jsonb_agg(j ORDER BY id))::text) FROM cv"),
	q1("SELECT md5((jsonb_agg(j ORDER BY id))::text) FROM co"),
	"jsonb_agg over VR == over ordinary");

# --- 6. jsonpath ----------------------------------------------------------
count_agree('jsonb_path_exists', q{jsonb_path_exists(T.j, '$.a.b ? (@ > 0)')});
agree('jsonb_path_query', q{(SELECT string_agg(v::text,',' ORDER BY v::text)
	FROM jsonb_path_query(T.j, '$.arr[*]') v)});
count_agree('jsonb_path_match', q{jsonb_path_match(T.j, '$.a.b >= 1')});

# --- 7. GIN index paths (jsonb_ops and jsonb_path_ops) --------------------
# explain_uses_index($idx, $query) : assert the plan uses a bitmap index scan
# on $idx (seqscan disabled), and return the co/cv row counts for equality.
sub gin_check
{
	my ($label, $idxname, $opclass, $query_where) = @_;

	q1("DROP INDEX IF EXISTS $idxname");
	q1("CREATE INDEX $idxname ON cv USING gin (j $opclass)");
	q1('ANALYZE cv');

	# correctness: indexed cv result == ordinary co result
	my $o = q1("SELECT count(*) FROM co T WHERE $query_where");
	my $v = q1("SET enable_seqscan=off; SELECT count(*) FROM cv T WHERE $query_where");
	is($v, $o, "GIN $label: indexed VR result == ordinary (count=$o)");

	# index actually used (not a seqscan fallback)
	my $plan = q1("SET enable_seqscan=off; EXPLAIN (COSTS OFF) "
		. "SELECT count(*) FROM cv T WHERE $query_where");
	like($plan, qr/Bitmap Index Scan on \Q$idxname\E/,
		"GIN $label: plan uses Bitmap Index Scan on $idxname");
	unlike($plan, qr/Seq Scan on cv/,
		"GIN $label: plan is not a seq scan");
}

gin_check('jsonb_ops @>',        'gin_cv',  '',              q{T.j @> '{"tag":"row5"}'});
gin_check('jsonb_ops ?',         'gin_cv2', '',              q{T.j ? 'a'});
gin_check('jsonb_path_ops @>',   'ginp_cv', 'jsonb_path_ops',q{T.j @> '{"tag":"row7"}'});

# --- 8. I/O surfaces (consumer-boundary only) -----------------------------
# output (jsonb_out) and binary send (jsonb_send): result must match ordinary.
agree('jsonb_out (::text)', q{T.j::text});
# jsonb_send content equality per row (not just length): binary out path detoasts.
is(q1("SELECT count(*) FROM co JOIN cv USING (id) WHERE jsonb_send(co.j) = jsonb_send(cv.j)"),
	"$N", "jsonb_send (binary out): VR bytes == ordinary for all $N rows");

# --- 9. invariants after all consumption ----------------------------------
# Read-side consumption (incl. index build/scan) must NOT flatten the stored VR,
# must NOT leak a locator, must NOT orphan a body.
like(probe('cv'), qr/\bvr=$N\b.*\bvr_storage_ok=1\b/,
	"invariant: cv still $N VR rows, vr_storage_ok=1 after all consumers");
is(body_count('cv'), "$N",
	"invariant: cv still exactly $N physical bodies (no orphan, no leak)");
# logical value still intact and still equal to the ordinary twin.
is(q1("SELECT count(*) FROM co JOIN cv USING (id) WHERE co.j = cv.j"),
	"$N", "invariant: logical value preserved across all consumers");

$node->stop;
done_testing();
