/*-------------------------------------------------------------------------
 *
 * vr_core_smoke.c
 *	  Storage-independent smoke for the value representation (VR) contract.
 *
 * Proves VR as a representation layer without any TOAST persistence: a
 * transient in-memory VR (VARTAG_VR_INMEM) is constructed from a flat bytea
 * payload, recognized as a VR, its header read (kind / version / logical_size),
 * its body sized and read back, and flattened through the real detoast/VR
 * funnel to the original bytes.  Nothing is stored; the VARTAG_VR_INMEM datum
 * never leaves C (returning it through SQL would force premature detoast and
 * risks the never-store invariant), so the driver does construct + flatten +
 * compare entirely in one C call and returns only a text summary.
 *
 * VR_KIND_TEST_VECTORS uses a trivial body == logical-bytes representation: the
 * body byte stream is the bytea payload verbatim, so flatten just wraps the
 * body back into a bytea.
 *
 * IDENTIFICATION
 *	  src/test/modules/vr_core_smoke/vr_core_smoke.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/value_representation.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/*
 * flatten: VR_INMEM-backed value -> ordinary bytea.  Reads the body straight
 * from the varatt_vr_inmem header, the same convention core uses in
 * toast_datum_size(); no separate body-accessor core API is involved.
 */
static Datum
vr_test_flatten(Datum stored_value, MemoryContext cxt)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(stored_value);
	varatt_vr_inmem v;
	bytea	   *result;
	MemoryContext old;

	/* this test kind constructs only the transient in-memory form */
	Assert(VARATT_IS_VR_INMEM(attr));
	VARATT_EXTERNAL_GET_POINTER(v, attr);

	old = MemoryContextSwitchTo(cxt);
	result = (bytea *) palloc(VARHDRSZ + v.vr_body_size);
	SET_VARSIZE(result, VARHDRSZ + v.vr_body_size);
	MemoryContextSwitchTo(old);

	if (v.vr_body_size > 0)
		memcpy(VARDATA(result), v.vr_body, v.vr_body_size);

	return PointerGetDatum(result);
}

static const ValueRepresentationMethods vr_test_methods = {
	.kind = VR_KIND_TEST_VECTORS,
	.write_version = 1,
	.flatten = vr_test_flatten,
};

void
_PG_init(void)
{
	vr_register_methods(&vr_test_methods);
}

/*
 * Construct a transient VARTAG_VR_INMEM datum wrapping body[0..body_size) for
 * the given kind/version.  The body buffer is referenced, not owned (the
 * VR_INMEM contract): it must outlive the datum, which here it always does
 * because the datum is consumed in the same call.
 */
static struct varlena *
vr_make_inmem(const char *body, int32 body_size, VrKind kind, uint8 version,
			  uint16 flags)
{
	varatt_vr_inmem v;
	Size		total = VARHDRSZ_EXTERNAL + sizeof(varatt_vr_inmem);
	struct varlena *result = (struct varlena *) palloc(total);

	v.vr_kind = (uint8) kind;
	v.vr_version = version;
	v.vr_flags = flags;
	v.vr_logical_size = (int32) (VARHDRSZ + body_size);
	v.vr_body_size = body_size;
	v.vr_body = body;
	SET_VARTAG_EXTERNAL(result, VARTAG_VR_INMEM);
	memcpy(VARDATA_EXTERNAL(result), &v, sizeof(v));
	return result;
}

/*
 * Forge a PERSISTENT VARTAG_VR datum with a chosen flags/size header and an
 * invalid locator.  Used only to drive the substrate reader's validation: the
 * reader must reject a bad header before it ever touches the (invalid) locator.
 */
static struct varlena *
vr_make_persistent_forged(VrKind kind, uint16 flags,
						  int32 logical_size, int32 body_size)
{
	Size		total = VARHDRSZ_EXTERNAL + sizeof(varatt_vr);
	struct varlena *result = (struct varlena *) palloc0(total);
	varatt_vr  *h;

	SET_VARTAG_EXTERNAL(result, VARTAG_VR);
	h = (varatt_vr *) VARDATA_EXTERNAL(result);
	h->vr_kind = (uint8) kind;
	h->vr_version = 1;
	h->vr_flags = flags;
	h->vr_logical_size = logical_size;
	h->vr_body_size = body_size;
	h->vr_storage_oid = InvalidOid;
	h->vr_valueid = InvalidOid;
	return result;
}

/*
 * vr_core_roundtrip(payload bytea) RETURNS text
 *
 * Build a transient VR over payload, probe the representation contract, flatten
 * through detoast, and report the observed invariants and whether the flattened
 * bytes equal the original.
 */
PG_FUNCTION_INFO_V1(vr_core_roundtrip);

Datum
vr_core_roundtrip(PG_FUNCTION_ARGS)
{
	bytea	   *payload = PG_GETARG_BYTEA_PP(0);
	int32		body_size = (int32) VARSIZE_ANY_EXHDR(payload);
	const char *body = VARDATA_ANY(payload);
	struct varlena *vr = vr_make_inmem(body, body_size, VR_KIND_TEST_VECTORS, 1, 0);
	Datum		vrd = PointerGetDatum(vr);
	VrHeaderInfo hdr;
	bool		is_vr = VARATT_IS_VR(vr);
	bool		is_inmem = VARATT_IS_VR_INMEM(vr);
	bool		hdr_ok = vr_header_info(vrd, &hdr);
	Size		tds = toast_datum_size(vrd);	/* expect body_size */
	Size		trds = toast_raw_datum_size(vrd);	/* expect logical_size */
	struct varlena *flat = detoast_attr(vr);
	Size		flat_len = VARSIZE_ANY_EXHDR(flat);
	bool		match = (flat_len == (Size) body_size) &&
		(body_size == 0 || memcmp(VARDATA_ANY(flat), body, body_size) == 0);
	bool		tds_ok = (tds == (Size) body_size);
	bool		trds_ok = (trds == (Size) (VARHDRSZ + body_size));
	StringInfoData s;

	initStringInfo(&s);
	appendStringInfo(&s,
					 "is_vr=%d is_inmem=%d hdr=%d kind=%d ver=%u lsize=%zu tds=%zu trds=%zu tds_ok=%d trds_ok=%d flat_len=%zu match=%d",
					 is_vr, is_inmem, hdr_ok, (int) hdr.kind, hdr.version,
					 hdr.logical_size, tds, trds, tds_ok, trds_ok, flat_len, match);

	PG_RETURN_TEXT_P(cstring_to_text(s.data));
}

/*
 * vr_core_badkind() RETURNS void
 *
 * Build a transient VR for a kind that has no registered methods and flatten
 * it; the VR recognition path must hard-ERROR rather than misread it.
 */
PG_FUNCTION_INFO_V1(vr_core_badkind);

Datum
vr_core_badkind(PG_FUNCTION_ARGS)
{
	char		dummy[4] = {0, 0, 0, 0};
	struct varlena *vr = vr_make_inmem(dummy, (int32) sizeof(dummy),
									   VR_KIND_BYTEA_BLOCK, 1, 0);

	(void) detoast_attr(vr);	/* expect ERROR: unsupported VR kind */

	PG_RETURN_VOID();
}

/*
 * vr_core_badflags() RETURNS void
 *
 * Build a transient VR with an unknown persistent flag bit set and flatten it;
 * a v1 reader must hard-ERROR on unknown flags rather than proceed.
 */
PG_FUNCTION_INFO_V1(vr_core_badflags);

Datum
vr_core_badflags(PG_FUNCTION_ARGS)
{
	char		dummy[4] = {0, 0, 0, 0};
	struct varlena *vr = vr_make_inmem(dummy, (int32) sizeof(dummy),
									   VR_KIND_TEST_VECTORS, 1, 0x0004);

	(void) detoast_attr(vr);	/* expect ERROR: unsupported VR flags 0x4 */

	PG_RETURN_VOID();
}

/*
 * vr_core_badmethod() RETURNS void
 *
 * The reserved compression method 0x0003 passes the known-flag-bit mask
 * (VR_FLAG_KNOWN_MASK) yet names no defined method.  The substrate body reader
 * must hard-ERROR on it rather than misread the stream.  Drive vr_body_read
 * directly with a forged persistent header; the reader must reject the method
 * before it ever dereferences the (invalid) locator.
 */
PG_FUNCTION_INFO_V1(vr_core_badmethod);

Datum
vr_core_badmethod(PG_FUNCTION_ARGS)
{
	struct varlena *vr = vr_make_persistent_forged(VR_KIND_TEST_VECTORS,
												   0x0003,
												   (int32) (VARHDRSZ + 4), 4);
	char		buf[4];

	/* expect ERROR: VR body read: unknown compression method 3 */
	vr_body_read(PointerGetDatum(vr), 0, sizeof(buf), buf);

	PG_RETURN_VOID();
}

/*
 * vr_core_badregister() RETURNS void
 *
 * The registration seam must reject an invalid kind.  Try to register methods
 * declared for VR_KIND_INVALID; vr_register_methods() ERRORs before touching
 * any slot.
 */
PG_FUNCTION_INFO_V1(vr_core_badregister);

Datum
vr_core_badregister(PG_FUNCTION_ARGS)
{
	static const ValueRepresentationMethods wrong = {
		.kind = VR_KIND_INVALID,
		.write_version = 1,
		.flatten = vr_test_flatten,
	};

	vr_register_methods(&wrong);	/* expect ERROR: invalid VR kind */

	PG_RETURN_VOID();
}

/*
 * vr_core_dupregister() RETURNS void
 *
 * The registration seam must reject a second registration for a kind that is
 * already registered.  VR_KIND_TEST_VECTORS was registered at module load, so
 * re-registering it ERRORs (duplicate), again before touching the slot.
 */
PG_FUNCTION_INFO_V1(vr_core_dupregister);

Datum
vr_core_dupregister(PG_FUNCTION_ARGS)
{
	vr_register_methods(&vr_test_methods);	/* expect ERROR: already registered */

	PG_RETURN_VOID();
}

/*
 * vr_core_isvr(payload bytea) RETURNS bool
 *
 * Recognition must NOT false-positive: an ordinary varlena is not a VR.
 */
PG_FUNCTION_INFO_V1(vr_core_isvr);

Datum
vr_core_isvr(PG_FUNCTION_ARGS)
{
	bytea	   *payload = PG_GETARG_BYTEA_PP(0);

	PG_RETURN_BOOL(VARATT_IS_VR(payload));
}
