/*-------------------------------------------------------------------------
 *
 * test_vr_decisions.c
 *	  Decision probes for the type-aware persistent Value Representation (VR).
 *
 * Exposes the pure, catalog-free DECISIONS the VR recognition/dispatch layer
 * makes, so a regression test can lock the v1 invariants without constructing
 * a persistent VR datum:
 *
 *	 - dispatch: vr_lookup_methods() resolves no kind in v1 (every kind, and
 *	   every out-of-range value, returns NULL methods);
 *	 - recognition: an ordinary datum is never recognized as a VR.
 *
 * It constructs no VR value, references no relation or TOAST value id, and
 * performs no body save/delete, so there is no storage side effect that could
 * be faked as success.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/test_vr_decisions/test_vr_decisions.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/value_representation.h"
#include "fmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/*
 * vr_decision_kind_count() -> int4
 *
 * Number of defined VrKind values (VR_KIND__COUNT).  Lets a test iterate the
 * kind space without hardcoding its size.
 */
PG_FUNCTION_INFO_V1(vr_decision_kind_count);
Datum
vr_decision_kind_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) VR_KIND__COUNT);
}

/*
 * vr_decision_kind_implemented(kind int4) -> bool
 *
 * Pure dispatch decision: does vr_lookup_methods() resolve this kind to a
 * lifecycle methods table?  No datum, no storage, no OID.  In v1 every kind --
 * and every out-of-range value -- resolves to NULL, i.e. false.  A regression
 * over all kinds therefore locks the invariant that no kind is implemented and
 * nothing can yet construct a persistent VR datum.
 */
PG_FUNCTION_INFO_V1(vr_decision_kind_implemented);
Datum
vr_decision_kind_implemented(PG_FUNCTION_ARGS)
{
	int32		kind = PG_GETARG_INT32(0);

	PG_RETURN_BOOL(vr_lookup_methods((VrKind) kind) != NULL);
}

/*
 * vr_decision_recognize(val bytea) -> text
 *
 * Recognition decision on the SUPPLIED datum, inspected raw (deliberately not
 * detoasted): 'external_vr' for a persistent VR pointer, 'vr_inmem' for a
 * transient in-memory VR, otherwise 'none'.  Nothing constructs a VR, so from
 * SQL this is a negative probe: an ordinary value (short, long, or an ordinary
 * TOAST pointer) must never be recognized as a VR.
 */
PG_FUNCTION_INFO_V1(vr_decision_recognize);
Datum
vr_decision_recognize(PG_FUNCTION_ARGS)
{
	struct varlena *raw = (struct varlena *) PG_GETARG_POINTER(0);
	const char *result;

	if (VARATT_IS_EXTERNAL_VR(raw))
		result = "external_vr";
	else if (VARATT_IS_VR_INMEM(raw))
		result = "vr_inmem";
	else
		result = "none";

	PG_RETURN_TEXT_P(cstring_to_text(result));
}
