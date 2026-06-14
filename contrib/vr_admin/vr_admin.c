/*-------------------------------------------------------------------------
 *
 * vr_admin.c
 *	  Sanctioned admin surface for the in-core Value Representation (VR)
 *	  jsonb-cold capability: typed inspection plus explicit, admin-triggered
 *	  re-representation.  This is the production-facing home (contrib), as
 *	  opposed to the test/smoke module; it adds no policy and no second
 *	  selector, drives only the ordinary write path, and never consults the
 *	  storage policy on read.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"		/* pg_detoast_datum_copy */
#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/value_representation.h"
#include "access/vr_toast.h"
#include "catalog/pg_type_d.h"	/* JSONBOID */
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/*
 * Exact flat (header-excluded) logical size of an ordinary (non-VR) jsonb
 * value, without a full detoast where avoidable:
 *   - external on-disk: va_rawsize - VARHDRSZ from the TOAST pointer;
 *   - inline uncompressed: VARSIZE_ANY_EXHDR directly;
 *   - inline compressed: detoast ONLY this value to get the exact size.
 * (External values are never inline-compressed in a way that hides rawsize:
 * va_rawsize is the original size for both compressed and uncompressed
 * external values.)
 */
static int64
ordinary_flat_size(struct varlena *v)
{
	if (VARATT_IS_EXTERNAL_ONDISK(v))
	{
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, v);
		return (int64) toast_pointer.va_rawsize - VARHDRSZ;
	}
	if (VARATT_IS_COMPRESSED(v))
	{
		/* inline compressed: detoast just this value for an exact size */
		struct varlena *flat = pg_detoast_datum_copy(v);
		int64		sz = (int64) VARSIZE_ANY_EXHDR(flat);

		pfree(flat);
		return sz;
	}
	/* inline uncompressed (or other external forms): direct */
	return (int64) VARSIZE_ANY_EXHDR(v);
}

/*
 * vr_inspect(rel regclass)
 *	  One typed row per jsonb column of the relation, with VR accounting.
 */
PG_FUNCTION_INFO_V1(vr_inspect);
Datum
vr_inspect(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	TupleDesc	reltupdesc;
	TupleDesc	outdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	int			natts;
	int			j;

	/* SRF boilerplate */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));
	if (get_call_result_type(fcinfo, NULL, &outdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = outdesc;
	MemoryContextSwitchTo(oldcontext);

	rel = table_open(relid, AccessShareLock);
	reltupdesc = RelationGetDescr(rel);
	natts = reltupdesc->natts;

	for (j = 0; j < natts; j++)
	{
		Form_pg_attribute att = TupleDescAttr(reltupdesc, j);
		AttrNumber	attnum;
		bool		marked;
		int64		rows_total = 0,
					rows_vr = 0,
					rows_ordinary = 0,
					rows_below_floor = 0,
					rows_above_floor = 0,
					rows_would_convert = 0,
					rows_null = 0,
					rows_storage_bad = 0;
		bool		have_vr = false;
		int64		lsize_min = 0,
					lsize_max = 0;
		Oid			reltoastrelid = rel->rd_rel->reltoastrelid;
		TableScanDesc scan;
		TupleTableSlot *slot;
		Datum		values[13];
		bool		nulls[13];

		if (att->attisdropped || att->atttypid != JSONBOID)
			continue;

		attnum = att->attnum;
		marked = vr_attribute_storage_policy(rel, attnum);

		scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
		slot = table_slot_create(rel, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool		isnull;
			Datum		d;
			struct varlena *v;

			CHECK_FOR_INTERRUPTS();
			rows_total++;
			d = slot_getattr(slot, attnum, &isnull);
			if (isnull)
			{
				rows_null++;
				continue;
			}
			v = (struct varlena *) DatumGetPointer(d);

			if (VARATT_IS_EXTERNAL_VR(v))
			{
				varatt_vr	hdr;
				int64		lsize;

				rows_vr++;
				VARATT_EXTERNAL_GET_POINTER(hdr, v);
				lsize = (int64) hdr.vr_logical_size - VARHDRSZ;
				if (!have_vr)
				{
					lsize_min = lsize_max = lsize;
					have_vr = true;
				}
				else
				{
					if (lsize < lsize_min)
						lsize_min = lsize;
					if (lsize > lsize_max)
						lsize_max = lsize;
				}
				if (hdr.vr_storage_oid != reltoastrelid)
					rows_storage_bad++;
			}
			else
			{
				int64		flat = ordinary_flat_size(v);

				rows_ordinary++;
				if (flat < VR_JSONB_COLD_MIN)
					rows_below_floor++;
				else
				{
					rows_above_floor++;
					if (marked)
						rows_would_convert++;
				}
			}
		}
		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);

		memset(nulls, 0, sizeof(nulls));
		values[0] = DirectFunctionCall1(namein,
										CStringGetDatum(NameStr(att->attname)));
		values[1] = BoolGetDatum(marked);
		values[2] = Int64GetDatum(rows_total);
		values[3] = Int64GetDatum(rows_vr);
		values[4] = Int64GetDatum(rows_ordinary);
		values[5] = Int64GetDatum(rows_below_floor);
		values[6] = Int64GetDatum(rows_above_floor);
		values[7] = Int64GetDatum(rows_would_convert);
		values[8] = Int64GetDatum(rows_null);
		values[9] = Int64GetDatum(rows_storage_bad);
		values[10] = BoolGetDatum(rows_storage_bad == 0);
		if (have_vr)
		{
			values[11] = Int64GetDatum(lsize_min);
			values[12] = Int64GetDatum(lsize_max);
		}
		else
		{
			nulls[11] = true;
			nulls[12] = true;
		}
		tuplestore_putvalues(tupstore, outdesc, values, nulls);
	}

	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * vr_force_fresh(jsonb) -> jsonb  (INTERNAL primitive, not an admin concept)
 *
 * Fresh, fully-detoasted copy of the canonical jsonb (pure byte copy via
 * pg_detoast_datum_copy), value-exact for every shape and non-external, so
 * re-storing it via UPDATE re-enters the externalize path and lets the E24
 * selector run.  Documented as internal; the admin API is
 * vr_rerepresent_column.
 */
PG_FUNCTION_INFO_V1(vr_force_fresh);
Datum
vr_force_fresh(PG_FUNCTION_ARGS)
{
	struct varlena *in = (struct varlena *) PG_GETARG_POINTER(0);
	struct varlena *copy = pg_detoast_datum_copy(in);

	PG_RETURN_POINTER(copy);
}

/*
 * vr_rerepresent_column(rel regclass, attname name) -> text
 *
 * Explicit, admin-triggered re-representation of existing ordinary jsonb
 * rows into VR_KIND_JSONB_COLD via the ordinary write path + E24 selector.
 * (Moved here from the smoke module as its final v0 home.)
 */
PG_FUNCTION_INFO_V1(vr_rerepresent_column);
Datum
vr_rerepresent_column(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Name		attname = PG_GETARG_NAME(1);
	Relation	rel;
	AttrNumber	attnum;
	Form_pg_attribute att;
	TableScanDesc scan;
	TupleTableSlot *slot;
	int64		rows_scanned = 0,
				rows_already_vr = 0,
				rows_ordinary = 0,
				rows_null = 0;
	StringInfoData ctids;
	StringInfoData report;
	bool		first = true;

	rel = table_open(relid, RowExclusiveLock);

	attnum = get_attnum(relid, NameStr(*attname));
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist", NameStr(*attname))));

	att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
	if (att->atttypid != JSONBOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("vr_rerepresent_column supports only jsonb columns")));

	if (RelationIsLogicallyLogged(rel) && !vr_logical_construction)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("persistent value representation is not supported on logically logged relations"),
				 errhint("Set \"vr_logical_construction\" to allow re-representation on this relation.")));

	initStringInfo(&ctids);
	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	slot = table_slot_create(rel, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, attnum, &isnull);
		struct varlena *v;
		ItemPointer tid;

		CHECK_FOR_INTERRUPTS();
		rows_scanned++;
		if (isnull)
		{
			rows_null++;
			continue;
		}
		v = (struct varlena *) DatumGetPointer(d);

		if (VARATT_IS_EXTERNAL_VR(v))
		{
			rows_already_vr++;
			continue;
		}

		rows_ordinary++;
		tid = &slot->tts_tid;
		appendStringInfo(&ctids, "%s'(%u,%u)'::tid",
						 first ? "" : ",",
						 ItemPointerGetBlockNumber(tid),
						 ItemPointerGetOffsetNumber(tid));
		first = false;
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	if (rows_ordinary > 0)
	{
		char	   *relname = quote_qualified_identifier(
						   get_namespace_name(RelationGetNamespace(rel)),
						   RelationGetRelationName(rel));
		const char *colname = quote_identifier(NameStr(*attname));
		StringInfoData q;

		initStringInfo(&q);
		appendStringInfo(&q,
						 "UPDATE %s SET %s = vr_force_fresh(%s) WHERE ctid = ANY(ARRAY[%s])",
						 relname, colname, colname, ctids.data);

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(q.data, false, 0) != SPI_OK_UPDATE)
			elog(ERROR, "vr_rerepresent_column: update failed");
		SPI_finish();
		pfree(q.data);
	}

	{
		int64		converted;
		int64		ordinary_remaining = 0;
		Snapshot	post;

		CommandCounterIncrement();
		post = RegisterSnapshot(GetLatestSnapshot());

		scan = table_beginscan(rel, post, 0, NULL, 0);
		slot = table_slot_create(rel, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, attnum, &isnull);
			struct varlena *v;

			if (isnull)
				continue;
			v = (struct varlena *) DatumGetPointer(d);
			if (VARATT_IS_EXTERNAL_VR(v))
				continue;
			ordinary_remaining++;
		}
		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		UnregisterSnapshot(post);

		converted = rows_ordinary - ordinary_remaining;
		if (converted < 0)
			converted = 0;

		initStringInfo(&report);
		appendStringInfo(&report,
						 "rows_scanned=%lld rows_converted=%lld rows_already_vr=%lld "
						 "rows_declined=%lld rows_null=%lld ordinary_remaining=%lld",
						 (long long) rows_scanned,
						 (long long) converted,
						 (long long) rows_already_vr,
						 (long long) (rows_ordinary - converted),
						 (long long) rows_null,
						 (long long) ordinary_remaining);
	}

	table_close(rel, RowExclusiveLock);
	PG_RETURN_TEXT_P(cstring_to_text(report.data));
}
