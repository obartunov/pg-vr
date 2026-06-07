/*-------------------------------------------------------------------------
 *
 * vr_toast.h
 *	  VR substrate over ordinary TOAST (v1).
 *
 * INTERNAL: included by the VR core and the substrate implementation only.
 * Type code MUST NOT include this header and MUST NOT call toast_save_datum;
 * types see only access/value_representation.h.
 *
 * The substrate stores the type's body as an opaque, contiguous byte stream
 * and never interprets it.  The body is stored TOAST-UNCOMPRESSED so the
 * decorated VR pointer round-trips; any per-block compression is the type's
 * own concern inside the body.
 *
 * src/include/access/vr_toast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VR_TOAST_H
#define VR_TOAST_H

#include "access/value_representation.h"
#include "utils/relcache.h"

/*
 * Body save request.  Type code fills kind/version/flags/logical_size and the
 * body bytes; the substrate writes the body and assembles the persistent
 * varatt_vr (adding storage_oid/valueid).
 */
typedef struct VrBodySaveRequest
{
	Relation	rel;
	AttrNumber	attnum;

	VrKind		kind;
	uint8		version;
	uint16		flags;

	Size		logical_size;

	const void *body;
	Size		body_size;

	int			toast_options;	/* options for toast_save_datum: 0 for an
								 * ordinary insert; the surrounding heap insert
								 * options (e.g. HEAP_INSERT_NO_LOGICAL) during a
								 * heap rewrite so the body chunks honour the same
								 * WAL/logical-decoding policy as the heap */
	MemoryContext mcxt;
} VrBodySaveRequest;

/* Build a persistent VR datum (VARTAG_VR): store body uncompressed, decorate. */
extern Datum vr_toast_body_save(const VrBodySaveRequest *req);

/*
 * Copy a body into the target relation's physical home for a per-tuple
 * rewrite.  Copy, not relocate: the OLD body remains governed by the OLD
 * lifecycle (reclaimed when the old relfilenode is dropped); this does not
 * delete the old body.
 */
extern Datum vr_toast_body_copy_to_relation(Datum old_stored,
											 Relation new_rel,
											 AttrNumber attnum,
											 const VrRewriteContext *ctx);

/* Substrate raw body access (the public vr_body_size/vr_body_read delegate). */
extern Size vr_toast_body_size(Datum stored_value);
extern void vr_toast_body_read(Datum stored_value,
							   Size offset, Size len, void *buf);

/* Ownership release: delete body chunks by locator.  is_speculative selects
 * speculative super-deletion (heap_abort_speculative) vs. an ordinary delete,
 * exactly as for an ordinary external value. */
extern void vr_toast_body_delete(Oid storage_oid, Oid valueid,
								 bool is_speculative);

/*
 * Substrate-private locator.  Not exposed to type code via vr_header_info().
 * Used for update-time ownership/share detection.
 */
typedef struct VrToastLocator
{
	Oid			storage_oid;
	Oid			valueid;
} VrToastLocator;

extern bool vr_toast_get_locator(Datum stored_value, VrToastLocator *out);

/*
 * True iff a and b are persistent VR datums denoting the same substrate body
 * (same storage_oid + valueid).  Used so an unchanged shared VR body on UPDATE
 * is neither orphaned nor leaked.
 */
extern bool vr_toast_same_body(Datum a, Datum b);

/*
 * Wrap an already-reassembled in-memory body as a TRANSIENT VR datum
 * (VARTAG_VR_INMEM) for the logical-decoding path.  Never stored; the save
 * paths reject it.  body_size is passed explicitly (VrHeaderInfo carries the
 * logical size, not the body size).  The buffer must outlive the datum.
 */
extern Datum vr_make_inmemory(const VrHeaderInfo *hdr,
							  const char *body, Size body_size,
							  MemoryContext mcxt);

#endif							/* VR_TOAST_H */
