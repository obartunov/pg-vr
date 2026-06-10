/*-------------------------------------------------------------------------
 *
 * vr.c
 *	  Type-Aware Persistent Value Representation (VR) - dispatch and header.
 *
 * Recognition and static dispatch for VR datums.  No representation kind is
 * implemented yet, and nothing here constructs a persistent VR datum: this is
 * the recognition foundation that lets audit paths fail loudly (a hard ERROR
 * via a NULL methods lookup) rather than silently misread a VR datum as an
 * ordinary TOAST pointer.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/vr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"		/* VARATT_EXTERNAL_GET_POINTER */
#include "access/value_representation.h"
#include "fmgr.h"				/* pg_detoast_datum */
#include "utils/rel.h"			/* RelationIsLogicallyLogged */

/*
 * Static methods table, indexed by VrKind.
 *
 * All entries are NULL until a representation kind is implemented by a later
 * patch (together with the code that can first construct a persistent VR
 * datum).  Until then vr_lookup_methods() returns NULL for every kind, so any
 * VR datum reaching a recognition path resolves to a hard ERROR in the caller
 * rather than a silent misread.
 */
/*
 * In-core VR_KIND_JSONB_COLD: the first real in-core VR user.
 *
 * Scope is deliberately narrow: store a large jsonb value's whole body as a
 * persistent VR and return it byte-identical on read.  There is NO jsonb
 * structural relocation and NO update optimization here; jsonb is treated as an
 * opaque varlena.  "Byte-identical flatten" is well defined as the fully
 * detoasted (decompressed, de-externalized) canonical jsonb binary: that is
 * what an ordinary read of the column would yield, and what flatten() returns.
 *
 * Which jsonb values become cold is a selection-policy decision and is NOT made
 * here: a selector (test harness today; a future in-core policy) opts a value
 * into this kind, exactly as for any other kind.
 */
static Datum
vr_jsonb_cold_make(Relation rel, AttrNumber attnum,
				   Datum logical_value, const VrMakeContext *ctx)
{
	struct varlena *raw = (struct varlena *) DatumGetPointer(logical_value);
	struct varlena *flat = pg_detoast_datum(raw);
	Size		body_size = VARSIZE_ANY_EXHDR(flat);
	Datum		result;

	/*
	 * Store the canonical (detoasted) jsonb body verbatim.  The selector is
	 * responsible for the size policy; make() does not second-guess it.
	 */
	result = vr_make_save_body(rel, attnum, ctx,
							   VR_KIND_JSONB_COLD, 1, 0,
							   VARHDRSZ + body_size,
							   VARDATA_ANY(flat), body_size);

	if (flat != raw)
		pfree(flat);

	return result;
}

static Datum
vr_jsonb_cold_flatten(Datum stored_value, MemoryContext cxt)
{
	Size		body_size = vr_body_size(stored_value);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	struct varlena *result = (struct varlena *) palloc(VARHDRSZ + body_size);

	SET_VARSIZE(result, VARHDRSZ + body_size);
	MemoryContextSwitchTo(old);

	if (body_size > 0)
		vr_body_read(stored_value, 0, body_size, VARDATA(result));

	return PointerGetDatum(result);
}

static const ValueRepresentationMethods vr_jsonb_cold_methods = {
	.kind = VR_KIND_JSONB_COLD,
	.write_version = 1,
	.flatten = vr_jsonb_cold_flatten,
	.make = vr_jsonb_cold_make,
};

/*
 * VR_KIND_TEST_VECTORS - the substrate-free simple kind.
 *
 * Proves the VR layer independent of TOAST-backed body storage: a small logical
 * value is carried INLINE inside a self-contained persistent VR descriptor
 * (VR_FLAG_INLINE).  There is no external/TOAST body, no home OID, no body
 * ownership, and therefore no orphan/dangling surface.  make() builds the
 * inline descriptor (declining values that do not fit), flatten() reads the
 * inline payload back through the ordinary vr_body_read() seam, and the
 * lifecycle methods are descriptor-local (no substrate work).
 */

/*
 * The inline region (vr_storage_oid, vr_valueid) must be contiguous and exactly
 * VR_INLINE_CAPACITY bytes, so it can hold a packed inline payload.
 */
StaticAssertDecl(offsetof(varatt_vr, vr_valueid) ==
				 offsetof(varatt_vr, vr_storage_oid) + sizeof(Oid),
				 "varatt_vr inline region must be contiguous");
StaticAssertDecl(VR_INLINE_CAPACITY == 2 * sizeof(Oid),
				 "VR_INLINE_CAPACITY must match the inline region size");

Datum
vr_make_inline(Relation rel, VrKind kind, uint8 version,
			   const void *payload, Size payload_len, MemoryContext mcxt)
{
	struct varlena *result;
	varatt_vr  *vr;
	MemoryContext old;

	/*
	 * Same construction boundary as vr_make_save_body: a persistent VR datum
	 * (inline or substrate-backed) cannot be represented by logical decoding,
	 * which refuses any external VR, so it would wedge a logical slot.  This is
	 * a VR representation boundary, not a TOAST rule.
	 */
	if (RelationIsLogicallyLogged(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("persistent value representation is not supported on logically logged relations")));

	if (payload_len > VR_INLINE_CAPACITY)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("value representation inline payload too large: %zu bytes (max %zu)",
						payload_len, (Size) VR_INLINE_CAPACITY)));

	old = MemoryContextSwitchTo(mcxt);
	result = (struct varlena *) palloc0(VARHDRSZ_EXTERNAL + sizeof(varatt_vr));
	SET_VARTAG_EXTERNAL(result, VARTAG_VR);
	vr = (varatt_vr *) VARDATA_EXTERNAL(result);
	vr->vr_kind = (uint8) kind;
	vr->vr_version = version;
	vr->vr_flags = VR_FLAG_INLINE;	/* self-contained; compression bits 0 */
	vr->vr_logical_size = (int32) (VARHDRSZ + payload_len);
	vr->vr_body_size = (int32) payload_len;

	/*
	 * The inline payload occupies the (vr_storage_oid, vr_valueid) region, which
	 * palloc0 already zeroed.  VR_FLAG_INLINE marks these bytes as payload, never
	 * a TOAST locator.
	 */
	if (payload_len > 0)
		memcpy(&vr->vr_storage_oid, payload, payload_len);
	MemoryContextSwitchTo(old);

	return PointerGetDatum(result);
}

/* --- VR_KIND_TEST_VECTORS lifecycle methods --- */

static Datum
vr_test_vectors_make(Relation rel, AttrNumber attnum,
					 Datum logical_value, const VrMakeContext *ctx)
{
	struct varlena *raw = (struct varlena *) DatumGetPointer(logical_value);
	struct varlena *flat = pg_detoast_datum(raw);
	Size		len = VARSIZE_ANY_EXHDR(flat);
	Datum		result;

	/*
	 * Decline values that do not fit the inline descriptor; the externalizer
	 * then falls back to the ordinary representation.
	 */
	if (len > VR_INLINE_CAPACITY)
	{
		if (flat != raw)
			pfree(flat);
		return logical_value;
	}

	result = vr_make_inline(rel, VR_KIND_TEST_VECTORS, 1,
							VARDATA_ANY(flat), len, ctx->mcxt);
	if (flat != raw)
		pfree(flat);
	return result;
}

static Datum
vr_test_vectors_flatten(Datum stored_value, MemoryContext cxt)
{
	Size		body_size = vr_body_size(stored_value); /* logical bytes */
	MemoryContext old = MemoryContextSwitchTo(cxt);
	struct varlena *result = (struct varlena *) palloc(VARHDRSZ + body_size);

	SET_VARSIZE(result, VARHDRSZ + body_size);
	MemoryContextSwitchTo(old);

	if (body_size > 0)
		vr_body_read(stored_value, 0, body_size, VARDATA(result));

	return PointerGetDatum(result);
}

static Datum
vr_test_vectors_replace(Relation rel, AttrNumber attnum,
						Datum old_stored_value, Datum new_logical_value,
						const VrReplaceContext *ctx)
{
	struct varlena *raw = (struct varlena *) DatumGetPointer(new_logical_value);
	struct varlena *flat = pg_detoast_datum(raw);
	Size		len = VARSIZE_ANY_EXHDR(flat);
	Datum		result;

	/* Self-contained: rebuild from the new logical value; do not reuse old. */
	if (len > VR_INLINE_CAPACITY)
	{
		if (flat != raw)
			pfree(flat);
		return new_logical_value;
	}
	result = vr_make_inline(rel, VR_KIND_TEST_VECTORS, 1,
							VARDATA_ANY(flat), len, ctx->mcxt);
	if (flat != raw)
		pfree(flat);
	return result;
}

static Datum
vr_test_vectors_rewrite(Datum old_stored_value, const VrRewriteContext *ctx)
{
	/*
	 * An inline VR is self-contained tuple content with no external body and no
	 * home OID, so a heap rewrite needs no relocation: the relocation trigger
	 * (toast_helper.c) skips VR_FLAG_INLINE and copies the datum verbatim, so
	 * this method is not on the heap-rewrite hot path.  If ever invoked, return
	 * the value unchanged, which is correct for a self-contained datum.
	 */
	return old_stored_value;
}

static void
vr_test_vectors_cleanup(Datum stored_value, VrCleanupReason reason,
						const VrCleanupContext *ctx)
{
	/*
	 * No-op: an inline VR owns no external body, no TOAST chunks, and no home
	 * storage, so there is nothing to release on delete or abort.  It is freed
	 * with its owning tuple/memory context like any inline content.
	 */
}

static bool
vr_test_vectors_validate(Relation rel, AttrNumber attnum,
						 Datum stored_value, const VrValidateContext *ctx)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);
	varatt_vr	v;
	bool		ok = true;

#define VR_TV_REPORT(msg) \
	do { if (ctx->report) ctx->report(ctx->report_arg, (msg)); ok = false; } while (0)

	if (!VARATT_IS_EXTERNAL_VR(attr))
	{
		if (ctx->report)
			ctx->report(ctx->report_arg, "not a persistent VR datum");
		return false;
	}
	VARATT_EXTERNAL_GET_POINTER(v, attr);

	if (v.vr_kind != (uint8) VR_KIND_TEST_VECTORS)
		VR_TV_REPORT("wrong VR kind");
	if (v.vr_version != 1)
		VR_TV_REPORT("unsupported VR version");
	if ((v.vr_flags & VR_FLAG_INLINE) == 0)
		VR_TV_REPORT("missing VR_FLAG_INLINE");
	if ((v.vr_flags & VR_FLAG_COMPRESSION_MASK) != 0)
		VR_TV_REPORT("inline VR must be uncompressed");
	if ((v.vr_flags & ~VR_FLAG_GENERIC_KNOWN_MASK) != 0)
		VR_TV_REPORT("unknown VR flags");
	if (v.vr_body_size < 0 || (Size) v.vr_body_size > VR_INLINE_CAPACITY)
		VR_TV_REPORT("inline body size out of range");
	if (v.vr_logical_size != (int32) (VARHDRSZ + v.vr_body_size))
		VR_TV_REPORT("logical size inconsistent with inline body");

#undef VR_TV_REPORT
	return ok;
}

static const ValueRepresentationMethods vr_test_vectors_methods = {
	.kind = VR_KIND_TEST_VECTORS,
	.write_version = 1,
	.flatten = vr_test_vectors_flatten,
	.make = vr_test_vectors_make,
	.replace = vr_test_vectors_replace,
	.rewrite = vr_test_vectors_rewrite,
	.cleanup = vr_test_vectors_cleanup,
	.validate = vr_test_vectors_validate,
};

static const ValueRepresentationMethods *const vr_methods_table[VR_KIND__COUNT] =
{
	[VR_KIND_INVALID] = NULL,
	[VR_KIND_JSONB_COLD] = &vr_jsonb_cold_methods,
	[VR_KIND_TEST_VECTORS] = &vr_test_vectors_methods,
	[VR_KIND_BYTEA_BLOCK] = NULL,
};

/*
 * Runtime registration slots, one per fixed VrKind.  Separate from the const
 * table above so any compile-time production kinds stay immutable.  Filled by
 * vr_register_methods() at module load (a type's init or a test).  No dynamic
 * kind, no catalog, no DDL.
 */
static const ValueRepresentationMethods *vr_registered_methods[VR_KIND__COUNT] =
{
	[VR_KIND_INVALID] = NULL,
};

/*
 * VR kind selector hook.  Default NULL = no value is ever VR-backed; the TOAST
 * externalizer behaves exactly as stock.  A backend (e.g. a type or a test)
 * installs a selector to opt specific values into a VR representation.
 */
vr_kind_selector_hook_type vr_kind_selector_hook = NULL;

/*
 * vr_register_methods
 *
 * Narrow fixed-kind registration: install lifecycle methods for methods->kind.
 * The kind must be a valid fixed VrKind (not VR_KIND_INVALID, in range) and
 * must not already have methods compiled in or previously registered.  This is
 * the single production-shaped registration seam; there is no dynamic kind
 * allocation, catalog, or DDL.
 */
void
vr_register_methods(const ValueRepresentationMethods *methods)
{
	VrKind		kind;

	if (methods == NULL)
		elog(ERROR, "vr_register_methods: NULL methods");

	kind = methods->kind;
	if (kind <= VR_KIND_INVALID || kind >= VR_KIND__COUNT)
		elog(ERROR, "vr_register_methods: invalid VR kind %d", (int) kind);

	if (vr_methods_table[kind] != NULL || vr_registered_methods[kind] != NULL)
		elog(ERROR, "vr_register_methods: methods already registered for VR kind %d",
			 (int) kind);

	vr_registered_methods[kind] = methods;
}

/*
 * vr_lookup_methods
 *
 * Static, catalog-free dispatch from a VrKind to its lifecycle methods.
 * Returns NULL for an out-of-range or not-yet-implemented kind; callers must
 * treat NULL as a hard ERROR ("unknown/unsupported VR kind").
 */
const ValueRepresentationMethods *
vr_lookup_methods(VrKind kind)
{
	if (kind <= VR_KIND_INVALID || kind >= VR_KIND__COUNT)
		return NULL;
	if (vr_registered_methods[kind] != NULL)
		return vr_registered_methods[kind];
	return vr_methods_table[kind];
}

/*
 * vr_header_info
 *
 * Read the inline VR pointer/header.  Catalog-free and locator-free: works on
 * both persistent (VARTAG_VR) and transient in-memory (VARTAG_VR_INMEM) VR
 * datums, reading only the shared header prefix (kind, version, flags,
 * logical_size).  Does not expose the substrate locator.  Returns false for a
 * non-VR datum.
 */
bool
vr_header_info(Datum stored_value, VrHeaderInfo *out)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);

	if (VARATT_IS_EXTERNAL_VR(attr))
	{
		varatt_vr	v;

		VARATT_EXTERNAL_GET_POINTER(v, attr);
		out->kind = (VrKind) v.vr_kind;
		out->version = v.vr_version;
		out->flags = v.vr_flags;
		out->logical_size = (Size) v.vr_logical_size;
		return true;
	}
	else if (VARATT_IS_VR_INMEM(attr))
	{
		varatt_vr_inmem v;

		VARATT_EXTERNAL_GET_POINTER(v, attr);
		out->kind = (VrKind) v.vr_kind;
		out->version = v.vr_version;
		out->flags = v.vr_flags;
		out->logical_size = (Size) v.vr_logical_size;
		return true;
	}

	return false;
}
