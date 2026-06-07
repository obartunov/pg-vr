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
#include "utils/rel.h"			/* RelationData->rd_rel->reltoastrelid */
#include "varatt.h"

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

/*
 * vr_toast_body_save
 *
 * Write the body as a plain uncompressed varlena via the core writer
 * toast_save_datum, which allocates a value id and writes the chunks (with WAL
 * and the toast index insert) into rel->rd_rel->reltoastrelid.  Copy the
 * (toastrelid, valueid) locator from the returned varatt_external into a fresh
 * persistent varatt_vr.
 *
 * Safety / ownership: the body lives in the OWNING relation's TOAST relation,
 * so it has the ordinary-TOAST lifetime - reclaimed when the owning heap tuple
 * is deleted (toast_delete_datum -> vr_toast_body_delete) and when the
 * relfilenode is dropped.  storage_oid is therefore the target reltoastrelid.
 * The body is stored UNCOMPRESSED so vr_toast_body_read reads it back verbatim.
 * oldexternal is NULL: a fresh body, not an OID-preserving rewrite (rewrite of
 * a VR value is refused by the heap_toast_insert_or_update guard).
 */
Datum
vr_toast_body_save(const VrBodySaveRequest *req)
{
	struct varlena *tmp;
	Size		tmp_size;
	Datum		ext_datum;
	struct varlena *ext;
	varatt_external ve;
	struct varlena *result;
	varatt_vr  *vr;
	MemoryContext oldcxt;

	Assert(req != NULL);
	Assert(req->rel != NULL);
	Assert(req->body != NULL || req->body_size == 0);
	Assert(req->body_size <= VARLENA_EXTSIZE_MASK);

	/* Wrap the body as a plain (uncompressed, 4-byte-header) varlena. */
	tmp_size = VARHDRSZ + req->body_size;
	tmp = (struct varlena *) palloc(tmp_size);
	SET_VARSIZE(tmp, tmp_size);
	if (req->body_size > 0)
		memcpy(VARDATA(tmp), req->body, req->body_size);

	/* Core writer: allocates value id, writes chunks into rel's TOAST rel. */
	ext_datum = toast_save_datum(req->rel, PointerGetDatum(tmp), NULL,
								 req->toast_options);
	ext = (struct varlena *) DatumGetPointer(ext_datum);
	Assert(VARATT_IS_EXTERNAL_ONDISK(ext));
	VARATT_EXTERNAL_GET_POINTER(ve, ext);

	/* Assemble the persistent VR pointer in the caller's context. */
	oldcxt = MemoryContextSwitchTo(req->mcxt);
	result = (struct varlena *) palloc0(VARHDRSZ_EXTERNAL + sizeof(varatt_vr));
	SET_VARTAG_EXTERNAL(result, VARTAG_VR);
	vr = (varatt_vr *) VARDATA_EXTERNAL(result);
	vr->vr_kind = (uint8) req->kind;
	vr->vr_version = req->version;
	vr->vr_flags = req->flags;
	vr->vr_logical_size = (int32) req->logical_size;
	vr->vr_body_size = (int32) req->body_size;
	vr->vr_storage_oid = ve.va_toastrelid;
	vr->vr_valueid = ve.va_valueid;
	MemoryContextSwitchTo(oldcxt);

	pfree(tmp);
	pfree(ext);

	return PointerGetDatum(result);
}

/*
 * vr_make_save_body
 *
 * Public make()-time body writer (declared in value_representation.h).  A
 * kind's make() builds the body in memory and calls this to persist it without
 * touching the private VrBodySaveRequest substrate struct: assemble the request
 * here from the make context and the supplied header fields, and return the
 * persistent VR datum produced by the substrate writer.  toast_options come
 * from the make context so a VR born during a heap rewrite inherits the heap's
 * HEAP_INSERT_NO_LOGICAL policy.
 */
Datum
vr_make_save_body(Relation rel, AttrNumber attnum,
				  const VrMakeContext *ctx,
				  VrKind kind, uint8 version, uint16 flags,
				  Size logical_size,
				  const void *body, Size body_size)
{
	VrBodySaveRequest req;

	Assert(ctx != NULL);

	req.rel = rel;
	req.attnum = attnum;
	req.kind = kind;
	req.version = version;
	req.flags = flags;
	req.logical_size = logical_size;
	req.body = body;
	req.body_size = body_size;
	req.toast_options = ctx->toast_options;
	req.mcxt = ctx->mcxt;

	return vr_toast_body_save(&req);
}

/*
 * vr_toast_body_copy_to_relation
 *
 * Per-tuple safe relocate for a heap rewrite (VACUUM FULL, CLUSTER, REPACK) or
 * a cross-relation movement: read the old VR body verbatim from its current
 * storage and save a fresh copy into new_rel's TOAST relation, returning a new
 * persistent VARTAG_VR whose locator names new_rel's reltoastrelid.  The
 * kind/version/flags/logical_size/body_size header fields are preserved.
 *
 * Copy, not move: the OLD body is NOT deleted here.  Its lifetime stays with
 * its original owner - on a heap rewrite the old relfilenode and its TOAST
 * relation are dropped at the swap, reclaiming the old body; on a cross-
 * relation copy the source row still references and owns it.  This never
 * produces a silent pointer copy into incompatible TOAST storage: the body is
 * physically re-written into new_rel.
 *
 * ctx->toast_options carries the surrounding heap-insert options (notably
 * HEAP_INSERT_NO_LOGICAL during a rewrite) so the copied body chunks honour the
 * same WAL/logical-decoding policy as the heap tuple they belong to.
 */
Datum
vr_toast_body_copy_to_relation(Datum old_stored, Relation new_rel,
							   AttrNumber attnum, const VrRewriteContext *ctx)
{
	struct varlena *old = (struct varlena *) DatumGetPointer(old_stored);
	varatt_vr	v;
	Size		body_size;
	char	   *body;
	VrBodySaveRequest req;
	Datum		result;

	Assert(VARATT_IS_EXTERNAL_VR(old));
	Assert(new_rel != NULL);
	Assert(ctx != NULL);

	if (!OidIsValid(new_rel->rd_rel->reltoastrelid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot relocate a value representation: target relation has no TOAST storage")));

	VARATT_EXTERNAL_GET_POINTER(v, old);
	body_size = (Size) v.vr_body_size;

	/* Read the old body verbatim from its current (source) storage. */
	body = (char *) palloc(body_size > 0 ? body_size : 1);
	vr_toast_body_read(old_stored, 0, body_size, body);

	/* Save a fresh, independent copy into the target relation's TOAST. */
	req.rel = new_rel;
	req.attnum = attnum;
	req.kind = (VrKind) v.vr_kind;
	req.version = v.vr_version;
	req.flags = v.vr_flags;
	req.logical_size = (Size) v.vr_logical_size;
	req.body = body;
	req.body_size = body_size;
	req.toast_options = ctx->toast_options;
	req.mcxt = ctx->mcxt;
	result = vr_toast_body_save(&req);

	pfree(body);

	return result;
}

/*
 * vr_toast_body_size
 *
 * Physical body byte count of a persistent VR value, read from the header.
 */
Size
vr_toast_body_size(Datum stored_value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);
	varatt_vr	v;

	Assert(VARATT_IS_EXTERNAL_VR(attr));
	VARATT_EXTERNAL_GET_POINTER(v, attr);
	return (Size) v.vr_body_size;
}

/*
 * vr_toast_body_read
 *
 * Read [offset, offset+len) of a persistent VR body into buf.  Reconstruct the
 * ordinary on-disk TOAST pointer for the locator and fetch through
 * detoast_external_attr.  The body was stored uncompressed, so va_rawsize =
 * body_size + VARHDRSZ and the external (saved) size equals body_size.
 */
void
vr_toast_body_read(Datum stored_value, Size offset, Size len, void *buf)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);
	varatt_vr	v;
	Size		body_size;
	varatt_external ve;
	struct varlena *extptr;
	struct varlena *full;

	Assert(VARATT_IS_EXTERNAL_VR(attr));
	VARATT_EXTERNAL_GET_POINTER(v, attr);
	body_size = (Size) v.vr_body_size;

	if (offset > body_size || len > body_size - offset)
		elog(ERROR,
			 "VR body read out of range: offset %zu len %zu body %zu",
			 offset, len, body_size);

	memset(&ve, 0, sizeof(ve));
	ve.va_rawsize = (int32) (body_size + VARHDRSZ);
	ve.va_extinfo = (uint32) body_size; /* uncompressed: extsize == rawsize - VARHDRSZ */
	ve.va_valueid = v.vr_valueid;
	ve.va_toastrelid = v.vr_storage_oid;

	extptr = (struct varlena *) palloc(VARHDRSZ_EXTERNAL + sizeof(varatt_external));
	SET_VARTAG_EXTERNAL(extptr, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(extptr), &ve, sizeof(ve));

	full = detoast_external_attr(extptr);
	Assert((Size) VARSIZE_ANY_EXHDR(full) == body_size);
	if (len > 0)
		memcpy(buf, VARDATA_ANY(full) + offset, len);

	pfree(full);
	pfree(extptr);
}

/*
 * vr_body_size / vr_body_read - public, type-facing delegates.  Persistent VR
 * values delegate to the substrate; the transient in-memory body path
 * (VARTAG_VR_INMEM) is not part of this milestone and errors explicitly.
 */
Size
vr_body_size(Datum stored_value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);

	if (VARATT_IS_EXTERNAL_VR(attr))
		return vr_toast_body_size(stored_value);

	elog(ERROR, "vr_body_size: not a persistent value representation");
	return 0;					/* unreachable; keep the compiler quiet */
}

void
vr_body_read(Datum stored_value, Size offset, Size len, void *buf)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);

	if (VARATT_IS_EXTERNAL_VR(attr))
	{
		vr_toast_body_read(stored_value, offset, len, buf);
		return;
	}

	elog(ERROR,
		 "vr_body_read: transient value representation body read is not implemented in this milestone");
}
