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

/*
 * Static methods table, indexed by VrKind.
 *
 * All entries are NULL until a representation kind is implemented by a later
 * patch (together with the code that can first construct a persistent VR
 * datum).  Until then vr_lookup_methods() returns NULL for every kind, so any
 * VR datum reaching a recognition path resolves to a hard ERROR in the caller
 * rather than a silent misread.
 */
static const ValueRepresentationMethods *const vr_methods_table[VR_KIND__COUNT] =
{
	[VR_KIND_INVALID] = NULL,
	[VR_KIND_JSONB_COLD] = NULL,
	[VR_KIND_TEST_VECTORS] = NULL,
	[VR_KIND_BYTEA_BLOCK] = NULL,
};

/*
 * Test-only registration slot for VR_KIND_TEST_VECTORS.  Separate from the
 * const methods table above so production kinds stay immutable.  Installed at
 * module load by a test, cleared with NULL.  No dynamic kind, no catalog.
 */
static const ValueRepresentationMethods *vr_test_vectors_methods = NULL;

void
vr_register_test_methods(const ValueRepresentationMethods *methods)
{
	if (methods != NULL && methods->kind != VR_KIND_TEST_VECTORS)
		elog(ERROR,
			 "vr_register_test_methods: methods declared for kind %d, expected VR_KIND_TEST_VECTORS",
			 (int) methods->kind);
	vr_test_vectors_methods = methods;
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
	if (kind == VR_KIND_TEST_VECTORS && vr_test_vectors_methods != NULL)
		return vr_test_vectors_methods;
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
