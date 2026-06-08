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
#include "access/toast_compression.h"	/* toast compression methods/ids */
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
 * Persist the logical body into rel's TOAST storage and return a fresh
 * persistent varatt_vr naming it.
 *
 * Compression is a property of the VR body substrate, not of any kind: the body
 * is compressed with the attribute's compression method using the same decision
 * and on-disk representation as ordinary TOAST (toast_compress_datum keeps the
 * compressed form only when it actually saves space).  Whatever is stored -
 * compressed or not - goes through the core writer toast_save_datum, which
 * allocates a value id and writes the chunks (with WAL and the toast index
 * insert) into rel->rd_rel->reltoastrelid.  The descriptor records:
 *	 vr_logical_size = the logical (decompressed) VARSIZE incl VARHDRSZ;
 *	 vr_body_size    = the SAVED PHYSICAL stream size (compressed when compressed);
 *	 vr_flags low bits = the compression method actually used (VR_COMPRESSION_*).
 * vr_toast_body_read reconstructs the external pointer from these and lets
 * detoast_external_attr decompress, so it returns the logical body verbatim.
 *
 * Safety / ownership: the body lives in the OWNING relation's TOAST relation,
 * so it has the ordinary-TOAST lifetime - reclaimed when the owning heap tuple
 * is deleted (toast_delete_datum -> vr_toast_body_delete) and when the
 * relfilenode is dropped.  storage_oid is therefore the target reltoastrelid.
 * oldexternal is NULL: a fresh body, not an OID-preserving rewrite (rewrite of
 * a VR value is refused by the heap_toast_insert_or_update guard).
 */
Datum
vr_toast_body_save(const VrBodySaveRequest *req)
{
	struct varlena *tmp;
	Size		tmp_size;
	Datum		cvalue;
	struct varlena *compressed;
	struct varlena *to_store;
	Datum		ext_datum;
	struct varlena *ext;
	varatt_external ve;
	struct varlena *result;
	varatt_vr  *vr;
	uint16		vrcomp;
	char		cmethod;
	MemoryContext oldcxt;

	Assert(req != NULL);
	Assert(req->rel != NULL);
	Assert(req->body != NULL || req->body_size == 0);
	Assert(req->body_size <= VARLENA_EXTSIZE_MASK);

	/*
	 * Substrate contract: the body, wrapped as a varlena and decompressed on
	 * read, IS the logical value, so logical_size must be exactly
	 * VARHDRSZ + body_size.  A kind that violates this would make the reader
	 * reconstruct a wrong raw size and decompress to the wrong length.  Enforce
	 * it in production, before any storage is written, instead of relying only
	 * on the debug-only Assert on va_rawsize below.
	 */
	if (req->logical_size != (Size) VARHDRSZ + req->body_size)
		elog(ERROR,
			 "vr_toast_body_save: logical_size %zu does not match VARHDRSZ + body_size %zu",
			 req->logical_size, req->body_size);

	/* Wrap the logical body as a plain (uncompressed, 4-byte-header) varlena. */
	tmp_size = VARHDRSZ + req->body_size;
	tmp = (struct varlena *) palloc(tmp_size);
	SET_VARSIZE(tmp, tmp_size);
	if (req->body_size > 0)
		memcpy(VARDATA(tmp), req->body, req->body_size);

	/*
	 * Compress with the attribute's method, same rule and representation as
	 * ordinary TOAST.  toast_compress_datum returns NULL when compression does
	 * not pay, in which case the body is stored uncompressed.
	 */
	cmethod = TupleDescAttr(RelationGetDescr(req->rel),
							req->attnum - 1)->attcompression;
	cvalue = toast_compress_datum(PointerGetDatum(tmp), cmethod);
	compressed = (struct varlena *) DatumGetPointer(cvalue);
	to_store = (compressed != NULL) ? compressed : tmp;

	/* Core writer: allocates value id, writes chunks into rel's TOAST rel. */
	ext_datum = toast_save_datum(req->rel, PointerGetDatum(to_store), NULL,
								 req->toast_options);
	ext = (struct varlena *) DatumGetPointer(ext_datum);
	Assert(VARATT_IS_EXTERNAL_ONDISK(ext));
	VARATT_EXTERNAL_GET_POINTER(ve, ext);

	/* The logical raw size must round-trip through the external pointer. */
	Assert(ve.va_rawsize == (int32) req->logical_size);

	/* Record the method actually used, derived from what was stored. */
	if (VARATT_EXTERNAL_IS_COMPRESSED(ve))
	{
		ToastCompressionId cmid = VARATT_EXTERNAL_GET_COMPRESS_METHOD(ve);

		if (cmid == TOAST_PGLZ_COMPRESSION_ID)
			vrcomp = VR_COMPRESSION_PGLZ;
		else if (cmid == TOAST_LZ4_COMPRESSION_ID)
			vrcomp = VR_COMPRESSION_LZ4;
		else
			elog(ERROR, "vr_toast_body_save: unexpected compression id %d",
				 (int) cmid);
	}
	else
		vrcomp = VR_COMPRESSION_NONE;

	/* Assemble the persistent VR pointer in the caller's context. */
	oldcxt = MemoryContextSwitchTo(req->mcxt);
	result = (struct varlena *) palloc0(VARHDRSZ_EXTERNAL + sizeof(varatt_vr));
	SET_VARTAG_EXTERNAL(result, VARTAG_VR);
	vr = (varatt_vr *) VARDATA_EXTERNAL(result);
	vr->vr_kind = (uint8) req->kind;
	vr->vr_version = req->version;
	/* the substrate owns the compression bits; preserve other caller flags */
	vr->vr_flags = (uint16) ((req->flags & ~VR_FLAG_COMPRESSION_MASK) | vrcomp);
	vr->vr_logical_size = (int32) req->logical_size;
	vr->vr_body_size = (int32) VARATT_EXTERNAL_GET_EXTSIZE(ve);
	vr->vr_storage_oid = ve.va_toastrelid;
	vr->vr_valueid = ve.va_valueid;
	MemoryContextSwitchTo(oldcxt);

	if (compressed != NULL)
		pfree(compressed);
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
 * vr_build_source_external
 *
 * Reconstruct the ordinary on-disk TOAST pointer naming a persistent VR body's
 * saved stream, carrying its recorded compression method and logical raw size.
 * Shared by the logical reader and the physical-verbatim relocate; it rejects
 * unknown flag bits and an unknown method before the pointer is ever used.
 */
static void
vr_build_source_external(const varatt_vr *v, varatt_external *ve)
{
	uint16		vrcomp;

	if ((v->vr_flags & ~VR_FLAG_KNOWN_MASK) != 0)
		elog(ERROR, "VR body read: unsupported flags 0x%04x",
			 (unsigned) v->vr_flags);
	vrcomp = v->vr_flags & VR_FLAG_COMPRESSION_MASK;

	memset(ve, 0, sizeof(*ve));
	ve->va_rawsize = (int32) v->vr_logical_size;	/* logical size incl VARHDRSZ */
	ve->va_valueid = v->vr_valueid;
	ve->va_toastrelid = v->vr_storage_oid;

	switch (vrcomp)
	{
		case VR_COMPRESSION_NONE:
			/* extsize == rawsize - VARHDRSZ => stored uncompressed */
			ve->va_extinfo = (uint32) v->vr_body_size;
			break;
		case VR_COMPRESSION_PGLZ:
			VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(*ve, (uint32) v->vr_body_size,
														 TOAST_PGLZ_COMPRESSION_ID);
			break;
		case VR_COMPRESSION_LZ4:
			VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(*ve, (uint32) v->vr_body_size,
														 TOAST_LZ4_COMPRESSION_ID);
			break;
		default:
			elog(ERROR, "VR body read: unknown compression method %u",
				 (unsigned) vrcomp);
	}
}

/*
 * vr_toast_body_copy_to_relation
 *
 * Per-tuple safe relocate for a heap rewrite (VACUUM FULL, CLUSTER, REPACK) or
 * a cross-relation movement: copy the old VR body's SAVED PHYSICAL stream
 * verbatim into new_rel's TOAST relation - no decompress and no recompress -
 * returning a new persistent VARTAG_VR whose locator names new_rel's
 * reltoastrelid.  kind/version/logical_size and the compression method are
 * preserved exactly; only the (storage_oid, valueid) locator changes.
 *
 * The body is fetched as-stored (detoast_external_attr does not decompress) and
 * handed to toast_save_datum together with the reconstructed source ON-DISK
 * pointer as oldexternal.  During a heap rewrite (new_rel->rd_toastoid set) this
 * makes toast_save_datum reuse the old body's value OID
 * (toast_internals.c:259-262) and short-circuit a second physical write when the
 * same body was already copied for an earlier version of the same row
 * (toast_internals.c:281-286) - the same path ordinary TOAST takes.  So when a
 * rewrite copies several heap versions (live + recently-dead) that share one VR
 * body, the body is written once and all versions reference one value OID,
 * instead of N duplicate bodies whose dead-version copies VACUUM would never
 * reclaim (B0/F1).  Outside a rewrite (rd_toastoid unset) oldexternal is ignored
 * and a fresh value OID is assigned, as before.  WAL/space stay identical to
 * ordinary TOAST while the relocate avoids the read+decompress+save+recompress
 * CPU of the logical path.
 *
 * Copy, not move: the OLD body is NOT deleted here.  Its lifetime stays with its
 * original owner - on a heap rewrite the old relfilenode and its TOAST relation
 * are dropped at the swap, reclaiming the old body; on a cross-relation copy the
 * source row still references and owns it.
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
	varatt_external ve_src;
	struct varlena *src_extptr;
	struct varlena *stored;
	Datum		ext_datum;
	struct varlena *ext;
	varatt_external ve_new;
	struct varlena *result;
	varatt_vr  *out;
	uint16		vrcomp;
	MemoryContext oldcxt;

	Assert(VARATT_IS_EXTERNAL_VR(old));
	Assert(new_rel != NULL);
	Assert(ctx != NULL);

	if (!OidIsValid(new_rel->rd_rel->reltoastrelid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot relocate a value representation: target relation has no TOAST storage")));

	VARATT_EXTERNAL_GET_POINTER(v, old);

	/* Fetch the saved stream as-stored (still compressed if compressed). */
	vr_build_source_external(&v, &ve_src);
	src_extptr = (struct varlena *) palloc(VARHDRSZ_EXTERNAL + sizeof(varatt_external));
	SET_VARTAG_EXTERNAL(src_extptr, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(src_extptr), &ve_src, sizeof(ve_src));
	stored = detoast_external_attr(src_extptr);

	/*
	 * Store the (possibly compressed) varlena verbatim into the target TOAST,
	 * passing the reconstructed source ON-DISK pointer as oldexternal.  During a
	 * heap rewrite (new_rel->rd_toastoid set) this makes toast_save_datum reuse
	 * the old body's value OID and short-circuit a duplicate write - see the
	 * header comment.  Outside a rewrite, oldexternal is ignored.
	 */
	ext_datum = toast_save_datum(new_rel, PointerGetDatum(stored), src_extptr,
								 ctx->toast_options);
	ext = (struct varlena *) DatumGetPointer(ext_datum);
	Assert(VARATT_IS_EXTERNAL_ONDISK(ext));
	VARATT_EXTERNAL_GET_POINTER(ve_new, ext);
	Assert(ve_new.va_rawsize == (int32) v.vr_logical_size);

	/* The method round-trips verbatim through toast_save_datum. */
	if (VARATT_EXTERNAL_IS_COMPRESSED(ve_new))
	{
		ToastCompressionId cmid = VARATT_EXTERNAL_GET_COMPRESS_METHOD(ve_new);

		if (cmid == TOAST_PGLZ_COMPRESSION_ID)
			vrcomp = VR_COMPRESSION_PGLZ;
		else if (cmid == TOAST_LZ4_COMPRESSION_ID)
			vrcomp = VR_COMPRESSION_LZ4;
		else
			elog(ERROR, "vr relocate: unexpected compression id %d", (int) cmid);
	}
	else
		vrcomp = VR_COMPRESSION_NONE;

	/* Assemble the new persistent VR pointer in the caller's context. */
	oldcxt = MemoryContextSwitchTo(ctx->mcxt);
	result = (struct varlena *) palloc0(VARHDRSZ_EXTERNAL + sizeof(varatt_vr));
	SET_VARTAG_EXTERNAL(result, VARTAG_VR);
	out = (varatt_vr *) VARDATA_EXTERNAL(result);
	out->vr_kind = v.vr_kind;
	out->vr_version = v.vr_version;
	out->vr_flags = (uint16) ((v.vr_flags & ~VR_FLAG_COMPRESSION_MASK) | vrcomp);
	out->vr_logical_size = v.vr_logical_size;
	out->vr_body_size = (int32) VARATT_EXTERNAL_GET_EXTSIZE(ve_new);
	out->vr_storage_oid = ve_new.va_toastrelid;
	out->vr_valueid = ve_new.va_valueid;
	MemoryContextSwitchTo(oldcxt);

	pfree(stored);
	pfree(src_extptr);
	pfree(ext);

	return PointerGetDatum(result);
}

/*
 * vr_toast_body_size
 *
 * The SAVED PHYSICAL stream size of a persistent VR value (descriptor
 * vr_body_size), read from the header.  This is the compressed size when the
 * body is compressed; it is NOT the logical body size.  Use the public
 * vr_body_size() for the logical (decompressed) size that flatten consumes.
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
 * Read [offset, offset+len) of the LOGICAL (decompressed) VR body into buf.
 * Reconstruct the ordinary on-disk TOAST pointer for the locator - carrying the
 * recorded compression method and the logical raw size - and fetch through
 * detoast_attr, which reassembles the chunks and decompresses on the method
 * bits.  offset and len are in logical space.  Unsupported flag bits or an
 * unknown method ERROR.
 */
void
vr_toast_body_read(Datum stored_value, Size offset, Size len, void *buf)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);
	varatt_vr	v;
	Size		logical_size;
	varatt_external ve;
	struct varlena *extptr;
	struct varlena *full;

	Assert(VARATT_IS_EXTERNAL_VR(attr));
	VARATT_EXTERNAL_GET_POINTER(v, attr);

	logical_size = (Size) v.vr_logical_size - VARHDRSZ;

	if (offset > logical_size || len > logical_size - offset)
		elog(ERROR,
			 "VR body read out of range: offset %zu len %zu body %zu",
			 offset, len, logical_size);

	/* reconstruct the source pointer (validates flags/method) */
	vr_build_source_external(&v, &ve);

	extptr = (struct varlena *) palloc(VARHDRSZ_EXTERNAL + sizeof(varatt_external));
	SET_VARTAG_EXTERNAL(extptr, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(extptr), &ve, sizeof(ve));

	full = detoast_attr(extptr);
	Assert((Size) VARSIZE_ANY_EXHDR(full) == logical_size);
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

	/*
	 * The LOGICAL (decompressed) body size that flatten consumes: it pairs with
	 * vr_body_read, which returns logical bytes.  This is vr_logical_size minus
	 * VARHDRSZ, independent of how the physical stream is compressed.  (The
	 * physical/saved stream size is vr_toast_body_size.)
	 */
	if (VARATT_IS_EXTERNAL_VR(attr))
	{
		varatt_vr	v;

		VARATT_EXTERNAL_GET_POINTER(v, attr);
		return (Size) v.vr_logical_size - VARHDRSZ;
	}

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
