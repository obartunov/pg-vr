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
