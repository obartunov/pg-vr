/*-------------------------------------------------------------------------
 *
 * vr_jsonb_cold_smoke.c
 *	  Focused smoke for the first real in-core VR user: VR_KIND_JSONB_COLD.
 *
 * The VR_KIND_JSONB_COLD lifecycle methods are in core (vr.c, compiled into
 * vr_methods_table); this module does NOT register them.  It only installs a
 * selector that opts a large jsonb attribute into VR_KIND_JSONB_COLD, then
 * proves the substrate round-trips a real jsonb byte-identically: the value is
 * born as a persistent VARTAG_VR through the externalize path, flatten() feeds
 * ordinary jsonb operators, and the value survives a heap rewrite.
 *
 * Selection is scoped to an armed relation (vr_jsonb_cold_arm) so the global
 * selector hook does not affect unrelated jsonb columns: the broader "which
 * jsonb becomes cold" policy is deferred and is not decided here.
 *
 * IDENTIFICATION
 *	  src/test/modules/vr_jsonb_cold_smoke/vr_jsonb_cold_smoke.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"		/* VARATT_EXTERNAL_GET_POINTER */
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/vr_toast.h"
#include "executor/spi.h"
#include "utils/lsyscache.h"
#include "access/tableam.h"
#include "access/value_representation.h"
#include "catalog/pg_type_d.h"	/* JSONBOID */
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/* jsonb values at least this large (logical bytes) are opted into VR. */
#define VR_JSONB_COLD_MIN 4096

/* Relation armed by vr_jsonb_cold_arm(); InvalidOid = nothing armed. */
static Oid	vr_jsonb_cold_armed_relid = InvalidOid;

/*
 * selector(): opt a large jsonb attribute of the armed relation into the
 * in-core VR_KIND_JSONB_COLD kind.  Type-aware (atttypid == JSONBOID) and
 * size-gated; scoped to the armed relation.
 */
static VrKind
vr_jsonb_cold_selector(Relation rel, AttrNumber attnum, Datum flat_value,
					   const VrMakeContext *ctx)
{
	Form_pg_attribute att;

	if (!OidIsValid(vr_jsonb_cold_armed_relid) ||
		RelationGetRelid(rel) != vr_jsonb_cold_armed_relid)
		return VR_KIND_INVALID;

	att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
	if (att->atttypid != JSONBOID)
		return VR_KIND_INVALID;

	if (VARSIZE_ANY_EXHDR(DatumGetPointer(flat_value)) < VR_JSONB_COLD_MIN)
		return VR_KIND_INVALID;

	return VR_KIND_JSONB_COLD;
}

void
_PG_init(void)
{
	/* The kind itself is in core; we only drive selection. */
	vr_kind_selector_hook = vr_jsonb_cold_selector;
}

/* vr_jsonb_cold_arm(rel regclass): opt the relation into jsonb-cold VR. */
PG_FUNCTION_INFO_V1(vr_jsonb_cold_arm);
Datum
vr_jsonb_cold_arm(PG_FUNCTION_ARGS)
{
	vr_jsonb_cold_armed_relid = PG_GETARG_OID(0);
	PG_RETURN_VOID();
}

/* vr_jsonb_cold_disarm(): stop opting any relation into VR. */
PG_FUNCTION_INFO_V1(vr_jsonb_cold_disarm);
Datum
vr_jsonb_cold_disarm(PG_FUNCTION_ARGS)
{
	vr_jsonb_cold_armed_relid = InvalidOid;
	PG_RETURN_VOID();
}

/*
 * vr_jsonb_cold_probe(rel regclass) RETURNS text
 *
 * Classify how each row's second attribute (the jsonb column) is physically
 * stored, reading the RAW datum (no detoast).  For VR rows, also report whether
 * storage_oid equals the relation's reltoastrelid.
 */
/* Name of the VR body compression method recorded in vr_flags. */
static const char *
vr_comp_name(uint16 flags)
{
	switch (flags & VR_FLAG_COMPRESSION_MASK)
	{
		case VR_COMPRESSION_NONE: return "none";
		case VR_COMPRESSION_PGLZ: return "pglz";
		case VR_COMPRESSION_LZ4:  return "lz4";
		default: return "unknown";
	}
}

PG_FUNCTION_INFO_V1(vr_jsonb_cold_probe);
Datum
vr_jsonb_cold_probe(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = table_open(relid, AccessShareLock);
	Oid			reltoastrelid = rel->rd_rel->reltoastrelid;
	TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	int64		rows = 0,
				vr = 0,
				ondisk = 0,
				inlined = 0;
	bool		vr_storage_ok = true;
	int64		logical_size_seen = -1;
	uint16		comp_seen = VR_COMPRESSION_NONE;
	StringInfoData s;

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, 2, &isnull);
		struct varlena *v;

		if (isnull)
			continue;
		rows++;
		v = (struct varlena *) DatumGetPointer(d);

		if (VARATT_IS_EXTERNAL_VR(v))
		{
			varatt_vr	hdr;

			VARATT_EXTERNAL_GET_POINTER(hdr, v);
			vr++;
			logical_size_seen = (int64) hdr.vr_logical_size - VARHDRSZ;
			comp_seen = hdr.vr_flags & VR_FLAG_COMPRESSION_MASK;
			if (hdr.vr_storage_oid != reltoastrelid)
				vr_storage_ok = false;
		}
		else if (VARATT_IS_EXTERNAL_ONDISK(v))
			ondisk++;
		else
			inlined++;
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	initStringInfo(&s);
	appendStringInfo(&s,
					 "rows=%lld vr=%lld ondisk=%lld inline=%lld vr_storage_ok=%d logical_size=%lld comp=%s",
					 (long long) rows, (long long) vr, (long long) ondisk,
					 (long long) inlined, vr_storage_ok ? 1 : 0,
					 (long long) logical_size_seen, vr_comp_name(comp_seen));

	PG_RETURN_TEXT_P(cstring_to_text(s.data));
}

/*
 * vr_jsonb_cold_capture(jsonb) RETURNS jsonb
 *
 * Test wrapper over the shared logical-value capture seam
 * (vr_capture_logical_value).  When the argument arrives as the stored VR
 * datum (a plain Var of an armed column), it is captured through the seam,
 * exercising the threshold and the flatten dispatch exactly as a logical
 * capture consumer would.  The executor may instead hand us an
 * already-materialized ordinary copy (e.g. through a projection); that is
 * returned unchanged, so pair assertions with vr_jsonb_cold_probe() to know
 * the stored form really is VR.
 */
PG_FUNCTION_INFO_V1(vr_jsonb_cold_capture);
Datum
vr_jsonb_cold_capture(PG_FUNCTION_ARGS)
{
	Datum		raw = PG_GETARG_DATUM(0);

	if (VARATT_IS_VR(DatumGetPointer(raw)))
		PG_RETURN_DATUM(vr_capture_logical_value(raw, CurrentMemoryContext));

	PG_RETURN_DATUM(raw);
}

/*
 * TEST-ONLY class-S forge (milestone M1): hand-build a persistent VR
 * descriptor whose body is homed in a FOREIGN relation (src), then INSERT it
 * into the target through the ordinary write path.  vr_rewrite_action says
 * REHOME for a foreign home on INSERT, so the write path itself copies the
 * body into the target's toast relation - writing real chunks in THIS
 * transaction, which is exactly what makes the decoded transaction class S -
 * and stores a properly homed VR descriptor.  No production policy is
 * bypassed: this exercises the same REHOME edge REPACK relocation uses; only
 * the descriptor fabrication (in place of the C1 construction seam) is
 * test-only.
 */
PG_FUNCTION_INFO_V1(vr_jsonb_cold_forge_class_s);
Datum
vr_jsonb_cold_forge_class_s(PG_FUNCTION_ARGS)
{
	Oid			srcid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);
	int32		id = PG_GETARG_INT32(2);
	struct varlena *payload = PG_GETARG_VARLENA_P(3);
	Relation	src;
	Relation	rel;
	Size		body_len;
	Datum		saved;
	varatt_external ve;
	varatt_vr	v;
	char	   *d;
	char	   *query;
	Oid			argtypes[2] = {INT4OID, JSONBOID};
	Datum		values[2];

	src = table_open(srcid, RowExclusiveLock);
	rel = table_open(relid, RowExclusiveLock);
	if (!OidIsValid(src->rd_rel->reltoastrelid) ||
		!OidIsValid(rel->rd_rel->reltoastrelid))
		elog(ERROR, "forge: relation has no toast relation");

	body_len = VARSIZE(payload) - VARHDRSZ;

	/* body chunks into the FOREIGN (src) toast relation */
	saved = toast_save_datum(src, PointerGetDatum(payload), NULL, 0);
	VARATT_EXTERNAL_GET_POINTER(ve, DatumGetPointer(saved));

	memset(&v, 0, sizeof(v));
	v.vr_kind = (uint8) VR_KIND_JSONB_COLD;
	v.vr_version = 1;
	v.vr_flags = 0;				/* uncompressed body stream */
	v.vr_logical_size = (int32) (VARHDRSZ + body_len);
	v.vr_body_size = (int32) body_len;
	v.vr_storage_oid = src->rd_rel->reltoastrelid;	/* foreign home */
	v.vr_valueid = ve.va_valueid;

	d = palloc(VARHDRSZ_EXTERNAL + sizeof(varatt_vr));
	SET_VARTAG_EXTERNAL(d, VARTAG_VR);
	memcpy(VARDATA_EXTERNAL(d), &v, sizeof(v));

	query = psprintf("INSERT INTO %s (id, j, n) VALUES ($1, $2, 0)",
					 quote_qualified_identifier(
						 get_namespace_name(RelationGetNamespace(rel)),
						 RelationGetRelationName(rel)));
	table_close(rel, NoLock);	/* keep locks until commit */
	table_close(src, NoLock);

	values[0] = Int32GetDatum(id);
	values[1] = PointerGetDatum(d);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * A one-shot SPI execution folds supplied parameter values into planner
	 * Consts, and makeConst flattens external datums (a Const must not carry
	 * a pointer that can outlive a snapshot) - which would flatten the VR
	 * before the write path ever sees it.  Force a GENERIC plan so the VR
	 * datum stays a runtime Param all the way to fill_val and the toast pass.
	 */
	{
		SPIPlanPtr	plan = SPI_prepare_cursor(query, 2, argtypes,
											  CURSOR_OPT_GENERIC_PLAN);

		if (plan == NULL)
			elog(ERROR, "SPI_prepare_cursor failed: %s",
				 SPI_result_code_string(SPI_result));
		if (SPI_execute_plan(plan, values, NULL, false, 0) != SPI_OK_INSERT ||
			SPI_processed != 1)
			elog(ERROR, "forge: insert stored %llu rows, expected 1",
				 (unsigned long long) SPI_processed);
	}
	SPI_finish();

	PG_RETURN_OID(ve.va_valueid);
}

/*
 * TEST-ONLY: attempt to STORE a transient VARTAG_VR_INMEM datum.  Must fail
 * in fill_val ("cannot store a transient in-memory value representation");
 * proves the producer's output cannot leak into heap storage.
 */
PG_FUNCTION_INFO_V1(vr_jsonb_cold_forge_inmem_store);
Datum
vr_jsonb_cold_forge_inmem_store(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		id = PG_GETARG_INT32(1);
	Relation	rel;
	static const char body[] = "\"x\"";
	VrHeaderInfo hdr;
	Datum		inmem;
	char	   *query;
	Oid			argtypes[2] = {JSONBOID, INT4OID};
	Datum		values[2];

	rel = table_open(relid, RowExclusiveLock);
	query = psprintf("UPDATE %s SET j = $1 WHERE id = $2",
					 quote_qualified_identifier(
						 get_namespace_name(RelationGetNamespace(rel)),
						 RelationGetRelationName(rel)));
	table_close(rel, NoLock);

	hdr.kind = VR_KIND_JSONB_COLD;
	hdr.version = 1;
	hdr.flags = 0;
	hdr.logical_size = VARHDRSZ + sizeof(body) - 1;
	inmem = vr_make_inmemory(&hdr, body, sizeof(body) - 1,
							 CurrentMemoryContext);

	values[0] = inmem;
	values[1] = Int32GetDatum(id);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	{
		SPIPlanPtr	plan = SPI_prepare_cursor(query, 2, argtypes,
											  CURSOR_OPT_GENERIC_PLAN);

		if (plan == NULL)
			elog(ERROR, "SPI_prepare_cursor failed: %s",
				 SPI_result_code_string(SPI_result));
		(void) SPI_execute_plan(plan, values, NULL, false, 0);
	}
	SPI_finish();

	PG_RETURN_VOID();			/* unreachable if fill_val refuses */
}
