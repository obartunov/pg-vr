/*-------------------------------------------------------------------------
 *
 * vr_persist_smoke.c
 *	  System smoke for VR persistence backend v0.
 *
 * Proves a flat logical value can be born as a persistent VARTAG_VR through the
 * owning heap/TOAST externalization path, read back through the VR method
 * contract, and cleaned up through the accepted delete/reuse paths - with no
 * pre-built external VR ever inserted from SQL.
 *
 * Mechanism (T1): a kind selector opts a (relation, attribute, value) into
 * VR_KIND_TEST_VECTORS; the vtable make() then constructs the persistent VR,
 * saving the body into the owning relation's TOAST storage via the substrate.
 * make() is value-aware and declines small values, which fall back to ordinary
 * on-disk TOAST.
 *
 * This is a TEST module and is the sanctioned exception to the rule that type
 * code must not include access/vr_toast.h: it stands in for a not-yet-existing
 * production type's make() and drives the substrate body writer directly.
 *
 * IDENTIFICATION
 *	  src/test/modules/vr_persist_smoke/vr_persist_smoke.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/value_representation.h"
#include "access/vr_toast.h"	/* test-only: substrate body writer */
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/* Values whose body is at least this large are VR-backed; smaller decline. */
#define VR_PERSIST_MIN_BODY 4096

/* Relation armed for VR by vr_persist_arm(); InvalidOid = nothing armed. */
static Oid	vr_persist_armed_relid = InvalidOid;

/*
 * make(): flat logical value -> persistent VARTAG_VR, body saved into rel's own
 * TOAST storage.  Value-aware: declines small values (returns the value
 * unchanged) so they fall back to ordinary on-disk TOAST.
 */
static Datum
vr_persist_make(Relation rel, AttrNumber attnum, Datum logical_value,
				const VrMakeContext *ctx)
{
	struct varlena *flat = (struct varlena *) DatumGetPointer(logical_value);
	Size		body_size = VARSIZE_ANY_EXHDR(flat);
	VrBodySaveRequest req;

	if (body_size < VR_PERSIST_MIN_BODY)
		return logical_value;	/* decline -> ordinary TOAST */

	req.rel = rel;
	req.attnum = attnum;
	req.kind = VR_KIND_TEST_VECTORS;
	req.version = 1;
	req.flags = 0;
	req.logical_size = VARHDRSZ + body_size;
	req.body = VARDATA_ANY(flat);
	req.body_size = body_size;
	req.toast_options = 0;
	req.mcxt = ctx->mcxt;

	return vr_toast_body_save(&req);
}

/*
 * flatten(): persistent VR -> ordinary bytea, via the public body accessor.
 */
static Datum
vr_persist_flatten(Datum stored_value, MemoryContext cxt)
{
	Size		body_size = vr_body_size(stored_value);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	bytea	   *result = (bytea *) palloc(VARHDRSZ + body_size);

	SET_VARSIZE(result, VARHDRSZ + body_size);
	MemoryContextSwitchTo(old);

	if (body_size > 0)
		vr_body_read(stored_value, 0, body_size, VARDATA(result));

	return PointerGetDatum(result);
}

static const ValueRepresentationMethods vr_persist_methods = {
	.kind = VR_KIND_TEST_VECTORS,
	.write_version = 1,
	.flatten = vr_persist_flatten,
	.make = vr_persist_make,
};

/*
 * selector(): value-aware (size-gated) and scoped to the armed relation.  Not
 * keyed on (relation, attribute) alone.
 */
static VrKind
vr_persist_selector(Relation rel, AttrNumber attnum, Datum flat_value,
					const VrMakeContext *ctx)
{
	if (OidIsValid(vr_persist_armed_relid) &&
		RelationGetRelid(rel) == vr_persist_armed_relid &&
		VARSIZE_ANY_EXHDR(DatumGetPointer(flat_value)) >= VR_PERSIST_MIN_BODY)
		return VR_KIND_TEST_VECTORS;
	return VR_KIND_INVALID;
}

void
_PG_init(void)
{
	vr_register_test_methods(&vr_persist_methods);
	vr_kind_selector_hook = vr_persist_selector;
}

/* vr_persist_arm(rel regclass): opt the relation into VR for large values. */
PG_FUNCTION_INFO_V1(vr_persist_arm);
Datum
vr_persist_arm(PG_FUNCTION_ARGS)
{
	vr_persist_armed_relid = PG_GETARG_OID(0);
	PG_RETURN_VOID();
}

/* vr_persist_disarm(): stop opting any relation into VR. */
PG_FUNCTION_INFO_V1(vr_persist_disarm);
Datum
vr_persist_disarm(PG_FUNCTION_ARGS)
{
	vr_persist_armed_relid = InvalidOid;
	PG_RETURN_VOID();
}

/*
 * vr_persist_probe(rel regclass) RETURNS text
 *
 * Classify how each row's second attribute (the bytea column) is physically
 * stored, reading the RAW datum (no detoast).  Reports counts and, for VR rows,
 * whether storage_oid equals the relation's reltoastrelid.
 */
PG_FUNCTION_INFO_V1(vr_persist_probe);
Datum
vr_persist_probe(PG_FUNCTION_ARGS)
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
	int64		vr_body_size_seen = -1;
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
			vr_body_size_seen = hdr.vr_body_size;
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
					 "rows=%lld vr=%lld ondisk=%lld inline=%lld vr_storage_ok=%d vr_body_size=%lld",
					 (long long) rows, (long long) vr, (long long) ondisk,
					 (long long) inlined, vr_storage_ok ? 1 : 0,
					 (long long) vr_body_size_seen);

	PG_RETURN_TEXT_P(cstring_to_text(s.data));
}

/*
 * vr_persist_toast_count(rel regclass) RETURNS bigint
 *
 * Number of chunk rows currently in the relation's TOAST relation.  Used to
 * show that DELETE + VACUUM leaves no orphaned VR body.
 */
PG_FUNCTION_INFO_V1(vr_persist_toast_count);
Datum
vr_persist_toast_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = table_open(relid, AccessShareLock);
	Oid			toastrelid = rel->rd_rel->reltoastrelid;
	int64		cnt = 0;

	table_close(rel, AccessShareLock);

	if (OidIsValid(toastrelid))
	{
		Relation	toastrel = table_open(toastrelid, AccessShareLock);
		TableScanDesc scan = table_beginscan(toastrel, GetActiveSnapshot(), 0, NULL, 0);
		TupleTableSlot *slot = table_slot_create(toastrel, NULL);

		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
			cnt++;

		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		table_close(toastrel, AccessShareLock);
	}

	PG_RETURN_INT64(cnt);
}
