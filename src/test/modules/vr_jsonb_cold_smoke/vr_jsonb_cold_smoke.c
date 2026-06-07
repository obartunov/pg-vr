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
