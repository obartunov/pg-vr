# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# VR_STORAGE_POLICY_INCORE_SELECTOR_V0: the first in-core VR kind selector.
# The producer path (toast_helper.c) consults the built-in selector FIRST and
# the extension hook only as a fallback, so the durable per-column policy
# (vr_jsonb_cold) drives VR construction in core with no extension loaded, and
# a test/extension hook cannot mask the core policy.
#
# Eligibility: jsonb only, column carries vr_jsonb_cold = on, value >=
# VR_JSONB_COLD_MIN (about two ordinary TOAST chunks); make() remains final. Reads never consult policy; existing data is
# never re-represented; vr_logical_construction stays an independent veto.
#
# The probe/arm helpers live in the test extension, so we CREATE EXTENSION to
# read accounting - but the VR is built by core before/independent of any hook.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $big   = q{(SELECT jsonb_object_agg('k'||g, g) FROM generate_series(1,2000) x(g))};
my $small = q{'{"a":1}'::jsonb};

# ---------------------------------------------------------------------------
# Node A: non-logical. Core selector, no preload, no hook.
# ---------------------------------------------------------------------------
my $a = PostgreSQL::Test::Cluster->new('vrselA');
$a->init;
$a->start;
# extension only for the probe; do NOT preload the module, so the extension
# hook is not installed in these backends - core must do the work alone.
$a->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');

# 1. marked jsonb column produces VR without backend-local arm and without
#    the extension hook being installed (no preload, fresh backend).
$a->safe_psql('postgres', 'CREATE TABLE t (id int PRIMARY KEY, j jsonb, k jsonb)');
$a->safe_psql('postgres', 'ALTER TABLE t ALTER COLUMN j SET (vr_jsonb_cold = on)');
$a->safe_psql('postgres', "INSERT INTO t SELECT 1, $big, $big");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('t')"),
	'rows=1 vr=1 ondisk=0 inline=0 vr_storage_ok=1 logical_size=40900 comp=pglz',
	'1: marked jsonb column produced VR via the in-core selector (no arm, no hook)');

# 2. unmarked jsonb column (k) stays ordinary -> proven by per-column probe:
#    j is VR, but if the selector ignored the policy, k would also be VR.
#    vr=1 (only one column became VR) already shows k stayed ordinary.
is( $a->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE id=1 AND j = $big AND k = $big"),
	'1', '2: unmarked sibling column k stays ordinary, logical values intact');

# 3. marked but SMALL jsonb stays ordinary (below the floor).
$a->safe_psql('postgres', 'CREATE TABLE s (id int PRIMARY KEY, j jsonb)');
$a->safe_psql('postgres', 'ALTER TABLE s ALTER COLUMN j SET (vr_jsonb_cold = on)');
$a->safe_psql('postgres', "INSERT INTO s SELECT 1, $small");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('s') ~ 'vr=0'"),
	't', '3: marked but small jsonb stays ordinary (size floor)');

# 3b. LOWER-BOUND regression (mandatory): a marked jsonb column whose value is
#     large enough to be EXTERNALIZED (ordinary out-of-line TOAST) but still
#     SMALLER than VR_JSONB_COLD_MIN must stay ordinary external, NOT VR. This
#     guards against the bad interpretation VR_JSONB_COLD_MIN = 0, which would
#     make every externalized jsonb on a marked column become VR. SET STORAGE
#     EXTERNAL forces out-of-line and disables compression so the size is
#     deterministic; an md5-keyed object is not-too-compressible and sized in
#     the window (TOAST threshold ~2 kB) < value < (VR floor ~4 kB).
my $borderline =
  q{(SELECT jsonb_object_agg('k'||g, md5(g::text)) FROM generate_series(1,80) x(g))};
$a->safe_psql('postgres', 'CREATE TABLE bl (id int PRIMARY KEY, j jsonb)');
$a->safe_psql('postgres', 'ALTER TABLE bl ALTER COLUMN j SET STORAGE EXTERNAL');
$a->safe_psql('postgres', 'ALTER TABLE bl ALTER COLUMN j SET (vr_jsonb_cold = on)');
$a->safe_psql('postgres', "INSERT INTO bl SELECT 1, $borderline");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('bl')"),
	'rows=1 vr=0 ondisk=1 inline=0 vr_storage_ok=1 logical_size=-1 comp=none',
	'3b: below-floor jsonb on a marked column is externalized but NOT VR (real lower bound; guards MIN=0)');
is( $a->safe_psql('postgres',
		"SELECT count(*) FROM bl WHERE id=1 AND j = $borderline"),
	'1', '3b: below-floor externalized value preserves the logical jsonb');
my $abovefloor =
  q{(SELECT jsonb_object_agg('k'||g, md5(g::text)) FROM generate_series(1,100) x(g))};
$a->safe_psql('postgres', "INSERT INTO bl SELECT 2, $abovefloor");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('bl') ~ 'vr=1'"),
	't', '3b: an above-floor value on the same column becomes VR (floor is the only difference)');

# 4. non-jsonb column with the option must not produce VR. (We do not enforce
#    type at DDL time, so the option can be set on text; the selector's type
#    gate must reject it.)
$a->safe_psql('postgres', 'CREATE TABLE n (id int PRIMARY KEY, t text)');
$a->safe_psql('postgres', 'ALTER TABLE n ALTER COLUMN t SET (vr_jsonb_cold = on)');
$a->safe_psql('postgres',
	"INSERT INTO n SELECT 1, repeat('x', 20000)");
# the text value is large and externalized, but must remain ordinary TOAST
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('n') ~ 'vr=0'"),
	't', '4: non-jsonb column with the option does not produce VR (type gate)');

# 5. RESET affects only future writes.
$a->safe_psql('postgres', 'ALTER TABLE t ALTER COLUMN j RESET (vr_jsonb_cold)');
$a->safe_psql('postgres', "INSERT INTO t SELECT 2, $big, $big");
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('t')"),
	'rows=2 vr=1 ondisk=1 inline=0 vr_storage_ok=1 logical_size=40900 comp=pglz',
	'5: after RESET, the new write is ordinary; the prior VR row is unchanged');

# 6. existing ordinary data is not re-represented when the policy turns on.
$a->safe_psql('postgres', 'CREATE TABLE u (id int PRIMARY KEY, j jsonb)');
$a->safe_psql('postgres', "INSERT INTO u SELECT 1, $big");      # no policy -> ordinary
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('u') ~ 'vr=0'"),
	't', '6a: pre-policy row is ordinary');
$a->safe_psql('postgres', 'ALTER TABLE u ALTER COLUMN j SET (vr_jsonb_cold = on)');
is( $a->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('u') ~ 'vr=0'"),
	't', '6b: turning the policy on does not re-represent existing data');

# 7. existing VR reads after policy reset (reads never consult policy).
#    t row id=1 is VR, policy on j was reset in step 5.
is( $a->safe_psql('postgres', "SELECT count(*) FROM t WHERE id=1 AND j = $big"),
	'1', '7: existing VR row still readable after policy reset');

# 9. extension/test hook still works as a fallback for an UNMARKED column via
#    the legacy backend-local arm. Needs the module loaded in the session.
$a->safe_psql('postgres', 'CREATE TABLE e (id int PRIMARY KEY, j jsonb)');  # unmarked
my $armed = $a->safe_psql('postgres',
	    "LOAD 'vr_jsonb_cold_smoke';"
	  . "SELECT vr_jsonb_cold_arm('e');"
	  . "INSERT INTO e SELECT 1, $big;"
	  . "SELECT vr_jsonb_cold_probe('e')");
like($armed, qr/vr=1/,
	'9: extension hook still drives VR as fallback for an unmarked column (arm)');

$a->stop;

# ---------------------------------------------------------------------------
# Node B: logical. vr_logical_construction independent veto (case 8).
# ---------------------------------------------------------------------------
my $b = PostgreSQL::Test::Cluster->new('vrselB');
$b->init(allows_streaming => 'logical');
$b->start;
$b->safe_psql('postgres', 'CREATE EXTENSION vr_jsonb_cold_smoke');
$b->safe_psql('postgres', 'CREATE TABLE g (id int PRIMARY KEY, j jsonb)');
$b->safe_psql('postgres', 'ALTER TABLE g ALTER COLUMN j SET (vr_jsonb_cold = on)');

# gate off (default): in-core selector selects VR, but the veto refuses.
my ($r, $o, $e) = $b->psql('postgres',
	    "\\set VERBOSITY verbose\n"
	  . "INSERT INTO g SELECT 1, $big");
isnt($r, 0, '8a: logically logged + gate off: construction refused by the veto');
like($e, qr/logically logged|vr_logical_construction/,
	'8b: refusal is the vr_logical_construction veto, independent of the selector');

# gate on: same policy now constructs VR (veto composes with, not replaces).
$b->safe_psql('postgres', 'ALTER SYSTEM SET vr_logical_construction = on');
$b->safe_psql('postgres', 'SELECT pg_reload_conf()');
$b->safe_psql('postgres', "INSERT INTO g SELECT 2, $big");
is( $b->safe_psql('postgres', "SELECT vr_jsonb_cold_probe('g') ~ 'vr=1'"),
	't', '8c: with the gate on, the in-core selector + policy construct VR');

$b->stop;
done_testing();
