/* contrib/vr_admin/vr_admin--1.0.sql */
\echo Use "CREATE EXTENSION vr_admin" to load this file. \quit

CREATE FUNCTION vr_inspect(rel regclass,
    OUT attname            name,
    OUT marked             bool,
    OUT rows_total         bigint,
    OUT rows_vr            bigint,
    OUT rows_ordinary      bigint,
    OUT rows_below_floor   bigint,
    OUT rows_above_floor   bigint,
    OUT rows_would_convert bigint,
    OUT rows_null          bigint,
    OUT rows_storage_bad   bigint,
    OUT storage_ok         bool,
    OUT logical_size_min   bigint,
    OUT logical_size_max   bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vr_inspect' LANGUAGE C STRICT;

-- INTERNAL primitive (not an admin concept): fresh non-external copy of a
-- jsonb value so a re-store re-enters the externalize path.
CREATE FUNCTION vr_force_fresh(jsonb) RETURNS jsonb
AS 'MODULE_PATHNAME', 'vr_force_fresh' LANGUAGE C STRICT;
COMMENT ON FUNCTION vr_force_fresh(jsonb) IS
  'internal VR primitive; not a supported admin interface';

CREATE FUNCTION vr_rerepresent_column(rel regclass, attname name) RETURNS text
AS 'MODULE_PATHNAME', 'vr_rerepresent_column' LANGUAGE C STRICT;
