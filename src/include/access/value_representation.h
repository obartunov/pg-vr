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
#include "access/heaptoast.h"	/* TOAST_MAX_CHUNK_SIZE */
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
 * Generic read-validity policy: true iff the generic VR layer understands every
 * persistent flag bit set in the header (compression bits and VR_FLAG_INLINE).
 * The per-kind dispatch (vr_lookup_methods) is a separate, already-centralized
 * step.  Backend read paths ask this instead of testing the flag mask locally.
 */
extern bool vr_read_supported(const VrHeaderInfo *hdr);

/*
 * Logical-value capture (VR-owned policy).
 *
 * Materialize the logical value a VR datum denotes into an ordinary flat
 * varlena Datum allocated in the caller-provided MemoryContext.  This is the
 * single seam through which a physical VR datum may leave VR-aware code into
 * a logical capture path (logical decoding, pgoutput, pgrepack change
 * capture): the consumer receives only the logical value; the substrate
 * locator is consumed inside VR code and never crosses the seam.
 *
 * Works on both the persistent (VARTAG_VR) and the transient in-memory
 * (VARTAG_VR_INMEM) form.  Fail-closed: a value whose logical size exceeds
 * vr_logical_capture_limit is refused with a deterministic
 * ERRCODE_FEATURE_NOT_SUPPORTED error before any body access, so capture-side
 * worker memory stays bounded by policy.  ERROR on a non-VR datum.
 */
extern Datum vr_capture_logical_value(Datum value, MemoryContext cxt);

/* Per-value capture bound in kilobytes; 0 refuses all VR capture. */
extern PGDLLIMPORT int vr_logical_capture_limit;

/*
 * Explicit administrator opt-in (default off): permit constructing persistent
 * external-body VR on logically logged relations (relaxes the C1 construction
 * gate in vr_make_save_body only; vr_make_inline stays refused).  With this
 * on, logical consumers without VR capture support (e.g. test_decoding) will
 * refuse such relations' changes until the data is removed - a deliberate,
 * admin-accepted fail-closed wedge.
 */
extern PGDLLIMPORT bool vr_logical_construction;

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

	/*
	 * TOAST/heap-insert option bits for the externalize that triggered this
	 * make(); passed through to the body writer so a VR born during a heap
	 * rewrite inherits HEAP_INSERT_NO_LOGICAL.  0 for an ordinary insert.
	 */
	int			toast_options;
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
	int			toast_options;	/* TOAST/heap-insert option bits passed through
								 * to the body writer; carries
								 * HEAP_INSERT_NO_LOGICAL during a heap rewrite */
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
 * Narrow fixed-kind method registration.
 *
 * Installs the lifecycle methods for methods->kind, which must be one of the
 * fixed VrKind enum values.  No catalog, no DDL, no dynamic kind allocation: it
 * only fills a static per-kind slot.  ERRORs on a NULL methods pointer, an
 * invalid/INVALID kind, or a kind that already has methods (compiled in or
 * previously registered).  vr_lookup_methods() remains the only lookup path.
 * A production kind registers through exactly this seam (e.g. from its _PG_init
 * or in-core init); storage-independent tests register VR_KIND_TEST_VECTORS the
 * same way.
 */
extern void vr_register_methods(const ValueRepresentationMethods *methods);

/*
 * make()-time body writer.
 *
 * A kind's make() builds the representation body in memory and calls this to
 * persist it: the substrate writes the body into rel's TOAST and returns the
 * assembled persistent VR datum (VARTAG_VR) carrying the given
 * kind/version/flags/logical_size plus the substrate locator.  This is the only
 * body-save entry a method needs, so a method implementation does not include
 * the private substrate header.  ctx supplies the memory context and the
 * pass-through TOAST options (HEAP_INSERT_NO_LOGICAL during a rewrite).
 *
 * The matching read side is already public above: vr_body_size() /
 * vr_body_read().
 */
extern Datum vr_make_save_body(Relation rel, AttrNumber attnum,
							   const VrMakeContext *ctx,
							   VrKind kind, uint8 version, uint16 flags,
							   Size logical_size,
							   const void *body, Size body_size);

/*
 * vr_make_inline
 *
 * Substrate-free construction: build a self-contained persistent VR descriptor
 * whose body bytes are carried INLINE in the descriptor (VR_FLAG_INLINE), with
 * no TOAST body, no substrate locator, and no body ownership.  payload_len must
 * be <= VR_INLINE_CAPACITY.  Applies the same construction boundary as
 * vr_make_save_body (refuses on a logically logged relation), because an inline
 * value is still a persistent VR datum that logical decoding cannot represent.
 * The matching read side is vr_body_size() / vr_body_read().
 */
extern Datum vr_make_inline(Relation rel, VrKind kind, uint8 version,
							const void *payload, Size payload_len,
							MemoryContext mcxt);

/*
 * VR kind selector hook.
 *
 * Consulted by the TOAST externalization path when a flat logical value is
 * about to be pushed out of line.  It returns a candidate VrKind for the
 * (relation, attribute, value) context, or VR_KIND_INVALID for "no VR" (the
 * default with no hook installed).  The chosen kind's vtable make() then
 * performs the actual construction and may still decline; on decline or an
 * unknown kind/method the externalizer falls back to ordinary on-disk TOAST.
 *
 * The selector receives the flat value so the decision can be value-aware
 * (e.g. size-gated), not keyed on (relation, attribute) alone.
 */
typedef VrKind (*vr_kind_selector_hook_type) (Relation rel, AttrNumber attnum,
											   Datum flat_value,
											   const VrMakeContext *ctx);
extern PGDLLIMPORT vr_kind_selector_hook_type vr_kind_selector_hook;

/*
 * Per-column VR storage policy reader.  Returns true if the attribute carries
 * the durable VR storage policy (attribute reloption vr_jsonb_cold = on),
 * read from the relcache attribute options.  This is policy STORAGE only: it
 * is consulted on the write path by a selector that decides representation; it
 * is never consulted on read (reads are self-describing).  Core ships no
 * autonomous selector that calls this - the decision of which columns to mark
 * is the user's, via ALTER TABLE ... ALTER COLUMN ... SET (vr_jsonb_cold = on).
 */
extern bool vr_attribute_storage_policy(Relation rel, AttrNumber attnum);

/*
 * Minimum flat (detoasted, header-excluded) jsonb size for the built-in
 * selector to choose VR_KIND_JSONB_COLD: at least about two ordinary TOAST
 * chunks.  Expressed in TOAST geometry rather than a magic constant, so it
 * tracks non-default BLCKSZ builds: TOAST_MAX_CHUNK_SIZE already accounts for
 * page/header/tuple/chunk_id/chunk_seq overhead, and on the default 8 kB page
 * this is ~4 kB.  The rule means "do not use VR for values smaller than about
 * two ordinary TOAST chunks".  Not a GUC, not a reloption knob.
 */
#define VR_JSONB_COLD_MIN	(2 * TOAST_MAX_CHUNK_SIZE)

/*
 * Built-in VR kind selector.  Applies the strict in-core eligibility rule
 * (jsonb only, column carries the durable vr_jsonb_cold policy, value at or
 * above VR_JSONB_COLD_MIN) and returns VR_KIND_JSONB_COLD, else
 * VR_KIND_INVALID.  Consulted first on the externalize/producer path; the
 * extension hook is the fallback.  Write path only; never consulted on read.
 */
extern VrKind vr_builtin_kind_selector(Relation rel, AttrNumber attnum,
									   Datum flat_value,
									   const VrMakeContext *ctx);

#endif							/* VALUE_REPRESENTATION_H */
