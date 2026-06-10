/*-------------------------------------------------------------------------
 *
 * vr_test_vectors_smoke.c
 *	  Smoke for the substrate-free simple VR kind: VR_KIND_TEST_VECTORS.
 *
 * The VR_KIND_TEST_VECTORS lifecycle methods are in core (vr.c, compiled into
 * vr_methods_table); this module does NOT register them.  It provides:
 *
 *   - vr_tv_make(rel, payload):  build a self-contained inline VR datum via the
 *     sanctioned core seam vr_make_inline().  A tiny inline value is never
 *     chosen by size-driven TOAST externalization, so this is how a small
 *     inline value is injected for the round-trip / heap-rewrite proof.
 *   - a selector (armed bytea attribute) + vr_tv_arm/disarm: proves the
 *     selector -> make() wiring and the make() decline path (a value larger
 *     than the inline capacity falls back to ordinary TOAST).
 *   - vr_tv_probe(rel): classify each row's bytea by physical representation.
 *   - vr_tv_toast_chunks(rel): count rows in the relation's TOAST table, to
 *     prove an inline-only table has no external body.
 *
 * IDENTIFICATION
 *	  src/test/modules/vr_test_vectors_smoke/vr_test_vectors_smoke.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"		/* VARATT_EXTERNAL_GET_POINTER */
#include "access/table.h"
#include "access/tableam.h"
#include "access/value_representation.h"
#include "catalog/pg_type_d.h"	/* BYTEAOID */
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/* Relation armed by vr_tv_arm(); InvalidOid = nothing armed. */
static Oid	vr_tv_armed_relid = InvalidOid;

/*
 * selector(): opt a bytea attribute of the armed relation into the in-core
 * VR_KIND_TEST_VECTORS kind.  No size gate here: make() decides, declining
 * values that do not fit the inline descriptor.
 */
static VrKind
vr_tv_selector(Relation rel, AttrNumber attnum, Datum flat_value,
			   const VrMakeContext *ctx)
{
	Form_pg_attribute att;

	if (!OidIsValid(vr_tv_armed_relid) ||
		RelationGetRelid(rel) != vr_tv_armed_relid)
		return VR_KIND_INVALID;

	att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
	if (att->atttypid != BYTEAOID)
		return VR_KIND_INVALID;

	return VR_KIND_TEST_VECTORS;
}

void
_PG_init(void)
{
	/* The kind itself is in core; we only drive selection. */
	vr_kind_selector_hook = vr_tv_selector;
}

PG_FUNCTION_INFO_V1(vr_tv_arm);
Datum
vr_tv_arm(PG_FUNCTION_ARGS)
{
	vr_tv_armed_relid = PG_GETARG_OID(0);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(vr_tv_disarm);
Datum
vr_tv_disarm(PG_FUNCTION_ARGS)
{
	vr_tv_armed_relid = InvalidOid;
	PG_RETURN_VOID();
}

/*
 * vr_tv_make(rel regclass, payload bytea) RETURNS bytea
 *
 * Build a self-contained inline VR datum through the sanctioned core seam.
 * Returns it as a bytea value; INSERTing the result stores the inline VR datum
 * verbatim (the relocation trigger skips VR_FLAG_INLINE), and SELECTing it
 * flattens back to the payload.  Oversized payloads raise a deterministic ERROR
 * inside vr_make_inline.
 */
PG_FUNCTION_INFO_V1(vr_tv_make);
Datum
vr_tv_make(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bytea	   *payload = PG_GETARG_BYTEA_PP(1);
	Relation	rel = table_open(relid, AccessShareLock);
	Size		len = VARSIZE_ANY_EXHDR(payload);
	Datum		d;

	d = vr_make_inline(rel, VR_KIND_TEST_VECTORS, 1,
					   VARDATA_ANY(payload), len, CurrentMemoryContext);
	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(d);
}

/*
 * vr_tv_probe(rel regclass) RETURNS text
 *
 * Classify how each row's second attribute (the bytea column) is physically
 * stored, reading the RAW datum (no detoast).
 */
PG_FUNCTION_INFO_V1(vr_tv_probe);
Datum
vr_tv_probe(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = table_open(relid, AccessShareLock);
	TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	int64		rows = 0,
				vr = 0,
				inline_vr = 0,
				ondisk = 0,
				plain = 0;
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
			if (hdr.vr_flags & VR_FLAG_INLINE)
				inline_vr++;
		}
		else if (VARATT_IS_EXTERNAL_ONDISK(v))
			ondisk++;
		else
			plain++;
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	initStringInfo(&s);
	appendStringInfo(&s,
					 "rows=%lld vr=%lld inline_vr=%lld ondisk=%lld plain=%lld",
					 (long long) rows, (long long) vr, (long long) inline_vr,
					 (long long) ondisk, (long long) plain);

	PG_RETURN_TEXT_P(cstring_to_text(s.data));
}

/*
 * vr_tv_toast_chunks(rel regclass) RETURNS bigint
 *
 * Count rows in the relation's TOAST table (0 = no external body exists).
 */
PG_FUNCTION_INFO_V1(vr_tv_toast_chunks);
Datum
vr_tv_toast_chunks(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = table_open(relid, AccessShareLock);
	Oid			toastoid = rel->rd_rel->reltoastrelid;
	int64		n = 0;

	table_close(rel, AccessShareLock);

	if (OidIsValid(toastoid))
	{
		Relation	t = table_open(toastoid, AccessShareLock);
		TableScanDesc scan = table_beginscan(t, GetActiveSnapshot(), 0, NULL, 0);
		TupleTableSlot *slot = table_slot_create(t, NULL);

		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
			n++;

		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		table_close(t, AccessShareLock);
	}

	PG_RETURN_INT64(n);
}
