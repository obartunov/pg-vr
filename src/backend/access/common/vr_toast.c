/*-------------------------------------------------------------------------
 *
 * vr_toast.c
 *	  VR substrate over ordinary TOAST (v1) - locator and ownership/delete.
 *
 * This is the substrate side of Type-Aware Persistent Value Representation
 * (VR): it stores and reclaims the opaque body byte stream of a persistent VR
 * value in an ordinary TOAST relation, and answers substrate-level ownership
 * questions.  It never interprets the body, and it does not construct a
 * persistent VR datum.
 *
 * This increment provides only the locator / ownership / delete trio:
 *
 *	  vr_toast_body_delete  - reclaim a body by (storage_oid, valueid)
 *	  vr_toast_get_locator  - read the substrate locator of a persistent VR
 *	  vr_toast_same_body    - whether two persistent VR datums share one body
 *
 * The body save/copy/read helpers and the transient in-memory wrapper are
 * declared in access/vr_toast.h but are implemented by a later increment,
 * together with the first code able to construct a persistent VR datum.  Until
 * then nothing in core produces a VARTAG_VR datum, so these readers simply
 * report "not a VR body" for every value they are handed.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/vr_toast.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"			/* VARATT_EXTERNAL_GET_POINTER */
#include "access/toast_internals.h" /* toast_delete_chunks_by_id */
#include "access/vr_toast.h"

/*
 * vr_toast_body_delete
 *
 * Release ownership of a persistent VR body identified by its substrate
 * locator.  In v1 the body is stored as ordinary TOAST chunks, so reclamation
 * is exactly the chunk deletion that toast_delete_datum performs for an
 * ordinary external value - here driven from the locator rather than from a
 * varatt_external pointer.
 *
 * is_speculative is threaded through unchanged: a VR body that was written as
 * part of a speculative insertion (INSERT ... ON CONFLICT) and is being
 * aborted must be super-deleted with heap_abort_speculative, exactly like the
 * TOAST chunks of an ordinary speculatively-inserted value; an ordinary
 * lifecycle delete (owning tuple deleted/vacuumed, creating transaction
 * aborted) uses a plain delete.  The body lives in an ordinary TOAST relation,
 * so heap_abort_speculative applies to its chunks (it accepts toast
 * relations).  toast_delete_chunks_by_id makes that choice from is_speculative.
 */
void
vr_toast_body_delete(Oid storage_oid, Oid valueid, bool is_speculative)
{
	toast_delete_chunks_by_id(storage_oid, valueid, is_speculative);
}

/*
 * vr_toast_get_locator
 *
 * Read the substrate locator (storage_oid, valueid) of a PERSISTENT VR datum.
 * Returns false for any non-VR datum and for the transient in-memory VR form
 * (VARTAG_VR_INMEM), which carries an in-memory body and has no locator.
 *
 * This is substrate-private: deliberately NOT reachable through
 * vr_header_info(), so type code can neither read nor fabricate locators.
 */
bool
vr_toast_get_locator(Datum stored_value, VrToastLocator *out)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);
	varatt_vr	v;

	if (!VARATT_IS_EXTERNAL_VR(attr))
		return false;

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(v, attr);
	out->storage_oid = v.vr_storage_oid;
	out->valueid = v.vr_valueid;
	return true;
}

/*
 * vr_toast_same_body
 *
 * True iff a and b are both persistent VR datums denoting the same substrate
 * body (identical storage_oid and valueid).  Used on UPDATE so that an
 * unchanged, shared VR body is neither orphaned nor double-freed.
 *
 * A non-VR or transient operand yields false: there is no shared persistent
 * body to speak of.  The locators of constructed VR datums are always valid;
 * comparing values that were never persisted is out of contract.
 */
bool
vr_toast_same_body(Datum a, Datum b)
{
	VrToastLocator la;
	VrToastLocator lb;

	if (!vr_toast_get_locator(a, &la) || !vr_toast_get_locator(b, &lb))
		return false;

	return la.storage_oid == lb.storage_oid && la.valueid == lb.valueid;
}
