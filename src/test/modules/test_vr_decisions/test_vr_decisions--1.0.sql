/* src/test/modules/test_vr_decisions/test_vr_decisions--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_vr_decisions" to load this file. \quit

CREATE FUNCTION vr_decision_kind_count()
	RETURNS pg_catalog.int4 STRICT
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION vr_decision_kind_implemented(kind pg_catalog.int4)
	RETURNS pg_catalog.bool STRICT
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION vr_decision_recognize(val pg_catalog.bytea)
	RETURNS pg_catalog.text STRICT
	AS 'MODULE_PATHNAME' LANGUAGE C;
