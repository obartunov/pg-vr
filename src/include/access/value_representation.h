/*-------------------------------------------------------------------------
 *
 * value_representation.h
 *	  Type-Aware Persistent Value Representation (VR) - core interface.
 *
 * Layering (RFC v3.1.2):
 *	  type owns semantics and read path;
 *	  Value Representation owns the write-side/lifecycle obligations of the
 *	  type-defined persistent representation;
 *	  the TOAST substrate stores and fetches raw body bytes.
 *
 * "jsonb owns meaning.  VR owns lifecycle.  TOAST owns bytes."
 *
 * v1 scope (gated): in-core only; fixed VrKind enum; external-only; ordinary
 * TOAST substrate; lifecycle-only methods; mechanical body access; no
 * semantic read API; no raw write; no relation-level preserve/swap; no inline
 * VR; no non-transactional own-GC substrate.  See docs/VR_BASELINE.md and
 * docs/VALUE_REPRESENTATION_RFC_v3_1_4.md.
 *
 * The on-disk pointer/header struct varatt_vr lives in varatt.h, alongside
 * varatt_external.
 *
 * src/include/access/value_representation.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VALUE_REPRESENTATION_H
#define VALUE_REPRESENTATION_H

#include "access/attnum.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "varatt.h"

/*
 * Kind and version.
 *
 * VrKind selects which in-core representation implementation owns the value.
 * It is stored in the VR pointer/header and indexes a static methods table.
 * Values are on-disk-stable; append only, never reuse a value.  The specific
 * numeric assignments are branch-local and remain subject to review.
 *
 * Version is separate from kind: each kind owns its own physical format
 * version sequence.  Kind chooses the interpreter; version chooses the body
 * format generation inside that interpreter.  Reading an unknown kind is a
 * hard ERROR; a pg_upgrade pre-flight check must reject unknown on-disk kinds
 * before production use.
 */
typedef enum VrKind
{
	VR_KIND_INVALID = 0,
	VR_KIND_JSONB_COLD = 1,		/* production target: jsonb owns semantics */
	VR_KIND_TEST_VECTORS = 2,	/* contrib/vr_test_vectors lifecycle proof */
	VR_KIND_BYTEA_BLOCK = 3,	/* possible later demonstrator */
	VR_KIND__COUNT				/* append only; values are permanent */
} VrKind;

/*
 * Mechanical pointer/header accessor.  Catalog-free and locator-free: it reads
 * only the inline VR pointer/header and returns false for non-VR values.  It
 * does not expose storage_oid/valueid or any other substrate locator, and it
 * does not define type semantics.
 *
 *   VrHeaderInfo.flags:	persistent on-disk VR pointer flags
 *   VrRewriteContext.flags:	per-call rewrite policy flags
 */
typedef struct VrHeaderInfo
{
	VrKind		kind;
	uint8		version;
	uint16		flags;			/* persistent on-disk pointer flags; v1 writers
								 * set 0, readers ERROR on unknown bits */
	Size		logical_size;	/* flattened logical size, not body size */
} VrHeaderInfo;

extern bool vr_header_info(Datum stored_value, VrHeaderInfo *out);

/*
 * Transient in-memory VR body form.
 *
 * Logical decoding may construct a runtime-only VR datum whose body is an
 * in-memory byte buffer captured from the decoded stream.  This is not a
 * persistent vr_flags bit and must never reach heap storage, a TOAST save,
 * rewrite output, WAL as tuple data, or logical output as a stored datum.
 *
 * vr_header_info() and vr_body_read() work on both persistent on-disk VR
 * values and transient in-memory VR values.  make / replace / rewrite / save
 * paths must reject transient VR values.
 */
static inline bool
vr_is_transient(Datum stored_value)
{
	return VARATT_IS_VR_INMEM(DatumGetPointer(stored_value));
}

/*
 * Mechanical representation body access.  Not vtable methods; not a semantic
 * read API.
 *
 *   offset/len address the representation BODY byte stream, not the flattened
 *   logical value; vr_body_size() != VrHeaderInfo.logical_size.
 *
 * The type owns the body layout, computes offsets and interprets bytes; the
 * substrate only returns raw bytes and hides physical chunking.  Reads must
 * lie within [0, vr_body_size(stored_value)).  There is no raw write.
 */
extern Size vr_body_size(Datum stored_value);

extern void vr_body_read(Datum stored_value,
						 Size offset,
						 Size len,
						 void *buf);

/*
 * Lifecycle contexts.  Purpose-specific and small; not a generic
 * storage-provider API.
 */
typedef struct VrMakeContext
{
	MemoryContext mcxt;
	/*
	 * Advisory inline-budget hint from heap/TOAST sizing context.  Not a
	 * semantic threshold and not a hard VR limit: the method may use, clamp
	 * or ignore it.  Values at or below the ordinary inline budget normally
	 * should not be VR-backed, but final policy belongs to the type method.
	 */
	Size		inline_budget;
} VrMakeContext;

typedef struct VrReplaceContext
{
	MemoryContext mcxt;
	Size		inline_budget;	/* advisory; see VrMakeContext */
} VrReplaceContext;

/*
 * Per-tuple rewrite policy flags (not persistent on-disk flags).  No
 * VR_REWRITE_ALLOW_PRESERVE in v1: per-tuple rewrite can copy, rebuild or
 * explicitly degrade, but cannot do relation-level physical preserve/swap
 * (a future separate lifecycle hook).
 */
#define VR_REWRITE_ALLOW_REBUILD	0x0001	/* may flatten -> make */
#define VR_REWRITE_ALLOW_DEGRADE	0x0002	/* may return ordinary flat */
#define VR_REWRITE_VALIDATE			0x0004	/* validate during rewrite */

typedef struct VrRewriteContext
{
	Relation	old_rel;		/* source relation being rewritten */
	Relation	new_rel;		/* target relation / new physical home */
	AttrNumber	attnum;
	uint32		flags;			/* per-call VR_REWRITE_* policy flags */
	MemoryContext mcxt;
} VrRewriteContext;

typedef enum VrCleanupReason
{
	VR_CLEANUP_DELETE,			/* owning tuple deleted / vacuumed */
	VR_CLEANUP_ABORT			/* creating/updating transaction aborted */
} VrCleanupReason;

typedef struct VrCleanupContext
{
	MemoryContext mcxt;
} VrCleanupContext;

/*
 * amcheck-style reporting: validate() returns the overall verdict and calls
 * report() once per problem; the caller (which knows rel/attnum/tid) supplies
 * the sink.
 */
typedef struct VrValidateContext
{
	int			elevel;
	void		(*report) (void *arg, const char *detail);
	void	   *report_arg;
	MemoryContext mcxt;
} VrValidateContext;

/*
 * ValueRepresentationMethods - lifecycle-only vtable.
 *
 * The type -> core lifecycle boundary for persistent physical representation.
 * Not a generic read interface.  Type-owned read paths may use
 * vr_header_info() / vr_body_size() / vr_body_read() to access their own body,
 * but all semantic interpretation stays in the type implementation.
 */
typedef struct ValueRepresentationMethods
{
	VrKind		kind;

	/*
	 * Physical format version written by this implementation.  Not "max
	 * supported version": older supported read versions are internal to
	 * flatten/validate/rewrite for this kind.
	 */
	uint8		write_version;

	/* VR-backed stored value -> ordinary flat logical datum.  MUST be pure /
	 * catalog-free: also runs in the logical-decoding context over the
	 * transient form. */
	Datum		(*flatten) (Datum stored_value, MemoryContext cxt);

	/* Build initial persistent representation from a logical value.  May
	 * decline and return the ordinary value unchanged (e.g. small values). */
	Datum		(*make) (Relation rel, AttrNumber attnum,
						 Datum logical_value, const VrMakeContext *ctx);

	/* Build the new stored representation from old stored + full new logical
	 * value.  old_stored_value is the current stored value, possibly VR-backed
	 * and possibly an ordinary flat value; it is provided for optional limited
	 * reuse and an implementation must not be required to flatten(old). */
	Datum		(*replace) (Relation rel, AttrNumber attnum,
							Datum old_stored_value, Datum new_logical_value,
							const VrReplaceContext *ctx);

	/* Build a stored datum for the rewritten tuple in ctx->new_rel: copy,
	 * rebuild, explicit degrade, or ERROR.  Per-tuple; no relation-level
	 * preserve/swap in v1. */
	Datum		(*rewrite) (Datum old_stored_value, const VrRewriteContext *ctx);

	/* Per-value cleanup / ownership release.  v1 over ordinary TOAST may rely
	 * on TOAST MVCC and relation cleanup, but the obligation is explicit. */
	void		(*cleanup) (Datum stored_value, VrCleanupReason reason,
							const VrCleanupContext *ctx);

	/* Explicit check/maintenance validation; not a read hot-path method. */
	bool		(*validate) (Relation rel, AttrNumber attnum,
							 Datum stored_value, const VrValidateContext *ctx);
} ValueRepresentationMethods;

/*
 * Static dispatch.  No handler, no DDL, no catalog object, no extension
 * registration.
 */
extern const ValueRepresentationMethods *vr_lookup_methods(VrKind kind);

/*
 * Test-only registration seam.  Installs (or with NULL clears) the lifecycle
 * methods for the single fixed kind VR_KIND_TEST_VECTORS, so storage-independent
 * tests can exercise the VR representation contract without a catalog, DDL, a
 * provider framework, or dynamic kind allocation.  Rejects methods declared for
 * any other kind.
 */
extern void vr_register_test_methods(const ValueRepresentationMethods *methods);

#endif							/* VALUE_REPRESENTATION_H */
