/*-------------------------------------------------------------------------
 *
 * toast_helper.c
 *	  Helper functions for table AMs implementing compressed or
 *    out-of-line storage of varlena attributes.
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/table/toast_helper.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/toast_helper.h"
#include "access/toast_internals.h"
#include "access/vr_toast.h"
#include "catalog/pg_type_d.h"
#include "varatt.h"


/*
 * Prepare to TOAST a tuple.
 *
 * tupleDesc, toast_values, and toast_isnull are required parameters; they
 * provide the necessary details about the tuple to be toasted.
 *
 * toast_oldvalues and toast_oldisnull should be NULL for a newly-inserted
 * tuple; for an update, they should describe the existing tuple.
 *
 * All of these arrays should have a length equal to tupleDesc->natts.
 *
 * On return, toast_flags and toast_attr will have been initialized.
 * toast_flags is just a single uint8, but toast_attr is a caller-provided
 * array with a length equal to tupleDesc->natts.  The caller need not
 * perform any initialization of the array before calling this function.
 */
void
toast_tuple_init(ToastTupleContext *ttc)
{
	TupleDesc	tupleDesc = ttc->ttc_rel->rd_att;
	int			numAttrs = tupleDesc->natts;
	int			i;

	ttc->ttc_flags = 0;

	for (i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		varlena    *old_value;
		varlena    *new_value;

		ttc->ttc_attr[i].tai_colflags = 0;
		ttc->ttc_attr[i].tai_oldexternal = NULL;
		ttc->ttc_attr[i].tai_compression = att->attcompression;

		if (ttc->ttc_oldvalues != NULL)
		{
			/*
			 * For UPDATE get the old and new values of this attribute
			 */
			old_value =
				(varlena *) DatumGetPointer(ttc->ttc_oldvalues[i]);
			new_value =
				(varlena *) DatumGetPointer(ttc->ttc_values[i]);

			/*
			 * If the old value is stored out-of-line (ordinary on-disk TOAST,
			 * or a persistent Value Representation), check whether it has
			 * changed so we have to delete it later.  A persistent VR is
			 * external but not ONDISK, so it must be admitted here explicitly;
			 * otherwise its out-of-line body would be orphaned on UPDATE.
			 */
			if (att->attlen == -1 && !ttc->ttc_oldisnull[i] &&
				(VARATT_IS_EXTERNAL_ONDISK(old_value) ||
				 VARATT_IS_EXTERNAL_VR(old_value)))
			{
				bool		delete_old;
				bool		old_is_inline_vr;

				/*
				 * A VR with no out-of-line body (an inline self-contained VR)
				 * has nothing to reclaim.  Ask the VR-owned predicate instead
				 * of inspecting VR_FLAG_INLINE here.  vr_has_external_body() is
				 * also false for the ONDISK case, so the EXTERNAL_VR guard keeps
				 * an ordinary on-disk value out of the inline branch.
				 */
				old_is_inline_vr = VARATT_IS_EXTERNAL_VR(old_value) &&
					!vr_has_external_body(PointerGetDatum(old_value));

				if (old_is_inline_vr)
				{
					/*
					 * A self-contained inline VR has no out-of-line body to
					 * reclaim, so it is never marked for deletion.  Reuse the old
					 * reference only if the new value is the byte-identical
					 * inline descriptor (column effectively unchanged); otherwise
					 * fall through to process the new value, which - if itself an
					 * inline VR - is kept verbatim by the new-value path below.
					 */
					if (!ttc->ttc_isnull[i] &&
						VARATT_IS_EXTERNAL_VR(new_value) &&
						VARSIZE_EXTERNAL(old_value) == VARSIZE_EXTERNAL(new_value) &&
						memcmp(old_value, new_value,
							   VARSIZE_EXTERNAL(old_value)) == 0)
					{
						ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;
						continue;
					}
					/* changed: no old body to delete; process the new value */
				}
				else
				{
					if (VARATT_IS_EXTERNAL_ONDISK(old_value))
						delete_old = ttc->ttc_isnull[i] ||
							!VARATT_IS_EXTERNAL_ONDISK(new_value) ||
							memcmp(old_value, new_value,
								   VARSIZE_EXTERNAL(old_value)) != 0;
					else
						/*
						 * Persistent VR: the old body is still needed only if the
						 * new value is a persistent VR denoting the SAME substrate
						 * body (same storage_oid + valueid).  Otherwise the old
						 * body is no longer referenced and must be reclaimed.
						 */
						delete_old = ttc->ttc_isnull[i] ||
							!VARATT_IS_EXTERNAL_VR(new_value) ||
							!vr_toast_same_body(ttc->ttc_oldvalues[i],
												ttc->ttc_values[i]);

					if (delete_old)
					{
						/*
						 * The old external stored value isn't needed any more
						 * after the update
						 */
						ttc->ttc_attr[i].tai_colflags |= TOASTCOL_NEEDS_DELETE_OLD;
						ttc->ttc_flags |= TOAST_NEEDS_DELETE_OLD;
					}
					else
					{
						/*
						 * This attribute isn't changed by this update so we reuse
						 * the original reference to the old value in the new
						 * tuple.
						 */
						ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;
						continue;
					}
				}
			}
		}
		else
		{
			/*
			 * For INSERT simply get the new value
			 */
			new_value = (varlena *) DatumGetPointer(ttc->ttc_values[i]);
		}

		/*
		 * Handle NULL attributes
		 */
		if (ttc->ttc_isnull[i])
		{
			ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;
			ttc->ttc_flags |= TOAST_HAS_NULLS;
			continue;
		}

		/*
		 * Now look at varlena attributes
		 */
		if (att->attlen == -1)
		{
			bool		need_detoast = true;


			/*
			 * If the table's attribute says PLAIN always, force it so.
			 */
			if (att->attstorage == TYPSTORAGE_PLAIN)
				ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;

			/*
			 * Value Representation safe relocate (INSERT / rewrite / cross-
			 * relation copy path).
			 *
			 * A persistent VR (VARTAG_VR) owns its body in a specific TOAST
			 * relation named by storage_oid.  A heap rewrite (VACUUM FULL,
			 * CLUSTER, REPACK, ALTER rewrite) or a cross-relation copy carries
			 * such a value into a relation whose reltoastrelid differs; a raw
			 * pointer copy would dangle once the source storage is dropped.
			 * Instead of flattening or refusing, copy the body into THIS
			 * relation's TOAST and rewrite the locator, preserving the VR
			 * representation.  The copy does NOT delete the source body: the old
			 * relfilenode/relation keeps ownership and reclaims it (on a rewrite
			 * the old relfilenode and its TOAST are dropped at the swap).  The
			 * body is written into this relation's physical TOAST; during a
			 * rewrite toast_save_datum stamps the new locator with rd_toastoid
			 * (the pre-swap OID), so the result is homed here and must not be
			 * flattened afterwards, so clear need_detoast.  PLAIN storage must
			 * stay inline and is left to the flatten path below.  The body writer
			 * inherits the heap's TOAST options (e.g. HEAP_INSERT_NO_LOGICAL
			 * during a rewrite) via ttc_options.
			 *
			 * Seam precedent: jsonb_toaster copy_toast in toast_tuple_init
			 * (postgrespro/postgres jsonb_toaster branch).
			 */
			if (att->attstorage != TYPSTORAGE_PLAIN &&
				VARATT_IS_EXTERNAL_VR(new_value))
			{
				VrRewriteAction act;

				/*
				 * Ask the VR-owned policy how to place this value into THIS
				 * relation's physical TOAST; reltoastrelid is the write target.
				 *
				 *   VR_RW_KEEP    - inline self-contained VR: keep verbatim on
				 *                   INSERT, UPDATE and heap rewrite alike,
				 *                   preserving the value and the inline form.
				 *   VR_RW_REHOME  - body lives in a different TOAST: on INSERT /
				 *                   rewrite / cross-relation copy (ttc_oldvalues
				 *                   == NULL) copy it into this relation and
				 *                   rewrite the locator (toast_save_datum stamps
				 *                   it with rd_toastoid during a rewrite, so the
				 *                   result is homed here); on UPDATE leave it to
				 *                   the generic flatten path below.
				 *   VR_RW_FLATTEN - body already homed here: leave it to the
				 *                   generic flatten path so the value is
				 *                   materialised independently (cross-row body
				 *                   sharing is unsupported - the substrate has no
				 *                   refcount, so a shared body would dangle when
				 *                   one referencing row is deleted).
				 *
				 * Only VR_RW_KEEP and a completed rehome clear need_detoast;
				 * every other case keeps need_detoast and is handled by the
				 * external-value path below, byte-for-byte as before.
				 */
				act = vr_rewrite_action(ttc->ttc_values[i],
										ttc->ttc_rel->rd_rel->reltoastrelid);

				if (act == VR_RW_KEEP)
				{
					need_detoast = false;
				}
				else if (act == VR_RW_REHOME && ttc->ttc_oldvalues == NULL)
				{
					VrRewriteContext rc;
					varlena    *new_vr;

					/* Validate the target before writing anything. */
					if (!OidIsValid(ttc->ttc_rel->rd_rel->reltoastrelid))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot relocate a value representation: target relation has no TOAST storage")));

					rc.old_rel = NULL;
					rc.new_rel = ttc->ttc_rel;
					rc.attnum = (AttrNumber) (i + 1);
					rc.flags = 0;
					rc.toast_options = ttc->ttc_options;
					rc.mcxt = CurrentMemoryContext;
					new_vr = (varlena *)
						DatumGetPointer(vr_toast_body_copy_to_relation(ttc->ttc_values[i],
																	   ttc->ttc_rel,
																	   (AttrNumber) (i + 1),
																	   &rc));

					/* Free a prior palloc'd copy of this column value, if any. */
					if (ttc->ttc_attr[i].tai_colflags & TOASTCOL_NEEDS_FREE)
						pfree(DatumGetPointer(ttc->ttc_values[i]));

					ttc->ttc_attr[i].tai_oldexternal = new_value;
					ttc->ttc_values[i] = PointerGetDatum(new_vr);
					ttc->ttc_attr[i].tai_colflags |= TOASTCOL_NEEDS_FREE;
					ttc->ttc_flags |= (TOAST_NEEDS_CHANGE | TOAST_NEEDS_FREE);
					new_value = new_vr;
					need_detoast = false;
				}
			}

			/*
			 * We took care of UPDATE above, so any external value we find
			 * still in the tuple must be someone else's that we cannot reuse
			 * (this includes the case of an out-of-line in-memory datum).
			 * Fetch it back (without decompression, unless we are forcing
			 * PLAIN storage).  If necessary, we'll push it out as a new
			 * external value below.  A VR relocated just above is already homed
			 * in this relation's TOAST and must not be flattened, so it is
			 * excluded by need_detoast.
			 */
			if (VARATT_IS_EXTERNAL(new_value) && need_detoast)
			{
				ttc->ttc_attr[i].tai_oldexternal = new_value;
				if (att->attstorage == TYPSTORAGE_PLAIN)
					new_value = detoast_attr(new_value);
				else
					new_value = detoast_external_attr(new_value);
				ttc->ttc_values[i] = PointerGetDatum(new_value);
				ttc->ttc_attr[i].tai_colflags |= TOASTCOL_NEEDS_FREE;
				ttc->ttc_flags |= (TOAST_NEEDS_CHANGE | TOAST_NEEDS_FREE);
			}

			/*
			 * Remember the size of this attribute
			 */
			ttc->ttc_attr[i].tai_size = VARSIZE_ANY(new_value);
		}
		else
		{
			/*
			 * Not a varlena attribute, plain storage always
			 */
			ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;
		}
	}
}

/*
 * Find the largest varlena attribute that satisfies certain criteria.
 *
 * The relevant column must not be marked TOASTCOL_IGNORE, and if the
 * for_compression flag is passed as true, it must also not be marked
 * TOASTCOL_INCOMPRESSIBLE.
 *
 * The column must have attstorage EXTERNAL or EXTENDED if check_main is
 * false, and must have attstorage MAIN if check_main is true.
 *
 * The column must have a minimum size of MAXALIGN(TOAST_POINTER_SIZE);
 * if not, no benefit is to be expected by compressing it.
 *
 * The return value is the index of the biggest suitable column, or
 * -1 if there is none.
 */
int
toast_tuple_find_biggest_attribute(ToastTupleContext *ttc,
								   bool for_compression, bool check_main)
{
	TupleDesc	tupleDesc = ttc->ttc_rel->rd_att;
	int			numAttrs = tupleDesc->natts;
	int			biggest_attno = -1;
	int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
	int32		skip_colflags = TOASTCOL_IGNORE;
	int			i;

	if (for_compression)
		skip_colflags |= TOASTCOL_INCOMPRESSIBLE;

	for (i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);

		if ((ttc->ttc_attr[i].tai_colflags & skip_colflags) != 0)
			continue;
		if (VARATT_IS_EXTERNAL(DatumGetPointer(ttc->ttc_values[i])))
			continue;			/* can't happen, toast_action would be PLAIN */
		if (for_compression &&
			VARATT_IS_COMPRESSED(DatumGetPointer(ttc->ttc_values[i])))
			continue;
		if (check_main && att->attstorage != TYPSTORAGE_MAIN)
			continue;
		if (!check_main && att->attstorage != TYPSTORAGE_EXTENDED &&
			att->attstorage != TYPSTORAGE_EXTERNAL)
			continue;

		if (ttc->ttc_attr[i].tai_size > biggest_size)
		{
			biggest_attno = i;
			biggest_size = ttc->ttc_attr[i].tai_size;
		}
	}

	return biggest_attno;
}

/*
 * Try compression for an attribute.
 *
 * If we find that the attribute is not compressible, mark it so.
 */
void
toast_tuple_try_compression(ToastTupleContext *ttc, int attribute)
{
	Datum	   *value = &ttc->ttc_values[attribute];
	Datum		new_value;
	ToastAttrInfo *attr = &ttc->ttc_attr[attribute];

	new_value = toast_compress_datum(*value, attr->tai_compression);

	if (DatumGetPointer(new_value) != NULL)
	{
		/* successful compression */
		if ((attr->tai_colflags & TOASTCOL_NEEDS_FREE) != 0)
			pfree(DatumGetPointer(*value));
		*value = new_value;
		attr->tai_colflags |= TOASTCOL_NEEDS_FREE;
		attr->tai_size = VARSIZE(DatumGetPointer(*value));
		ttc->ttc_flags |= (TOAST_NEEDS_CHANGE | TOAST_NEEDS_FREE);
	}
	else
	{
		/* incompressible, ignore on subsequent compression passes */
		attr->tai_colflags |= TOASTCOL_INCOMPRESSIBLE;
	}
}

/*
 * Move an attribute to external storage.
 */
void
toast_tuple_externalize(ToastTupleContext *ttc, int attribute, uint32 options)
{
	Datum	   *value = &ttc->ttc_values[attribute];
	Datum		old_value = *value;
	ToastAttrInfo *attr = &ttc->ttc_attr[attribute];
	Datum		new_value = (Datum) 0;
	bool		made = false;

	attr->tai_colflags |= TOASTCOL_IGNORE;

	/*
	 * Value Representation producer.  If a selector opts this flat value into a
	 * VR kind whose vtable make() accepts it, the value is born here as a
	 * persistent VARTAG_VR - its body saved into this relation's own TOAST
	 * storage, so storage_oid == reltoastrelid - instead of an ordinary on-disk
	 * pointer.  make() may decline; on decline or an unknown kind/method we fall
	 * back to ordinary TOAST.
	 */
	{
		VrMakeContext mctx;
		VrKind		kind;

		mctx.mcxt = CurrentMemoryContext;
		mctx.inline_budget = 0;
		mctx.toast_options = options;

		/*
		 * Built-in selector first: it applies the durable per-column VR
		 * storage policy (vr_jsonb_cold), the only production control
		 * surface.  The extension hook is consulted only as a fallback when
		 * the built-in declines, so a test/extension hook cannot mask a
		 * durable core policy.  Unmarked columns make the built-in return
		 * VR_KIND_INVALID, so default behavior is exactly stock.
		 */
		kind = vr_builtin_kind_selector(ttc->ttc_rel,
										(AttrNumber) (attribute + 1),
										old_value, &mctx);
		if (kind == VR_KIND_INVALID && vr_kind_selector_hook != NULL)
			kind = vr_kind_selector_hook(ttc->ttc_rel,
										 (AttrNumber) (attribute + 1),
										 old_value, &mctx);

		if (kind != VR_KIND_INVALID)
		{
			const ValueRepresentationMethods *m = vr_lookup_methods(kind);

			if (m != NULL && m->make != NULL)
			{
				Datum		d = m->make(ttc->ttc_rel,
										(AttrNumber) (attribute + 1),
										old_value, &mctx);

				if (VARATT_IS_EXTERNAL_VR(DatumGetPointer(d)))
				{
					new_value = d;
					made = true;
				}
				/* else: make() declined -> fall back to ordinary TOAST */
			}
		}
	}

	if (!made)
		new_value = toast_save_datum(ttc->ttc_rel, old_value,
									 attr->tai_oldexternal, options);

	*value = new_value;
	if ((attr->tai_colflags & TOASTCOL_NEEDS_FREE) != 0)
		pfree(DatumGetPointer(old_value));
	attr->tai_colflags |= TOASTCOL_NEEDS_FREE;
	ttc->ttc_flags |= (TOAST_NEEDS_CHANGE | TOAST_NEEDS_FREE);
}

/*
 * Perform appropriate cleanup after one tuple has been subjected to TOAST.
 */
void
toast_tuple_cleanup(ToastTupleContext *ttc)
{
	TupleDesc	tupleDesc = ttc->ttc_rel->rd_att;
	int			numAttrs = tupleDesc->natts;

	/*
	 * Free allocated temp values
	 */
	if ((ttc->ttc_flags & TOAST_NEEDS_FREE) != 0)
	{
		int			i;

		for (i = 0; i < numAttrs; i++)
		{
			ToastAttrInfo *attr = &ttc->ttc_attr[i];

			if ((attr->tai_colflags & TOASTCOL_NEEDS_FREE) != 0)
				pfree(DatumGetPointer(ttc->ttc_values[i]));
		}
	}

	/*
	 * Delete external values from the old tuple
	 */
	if ((ttc->ttc_flags & TOAST_NEEDS_DELETE_OLD) != 0)
	{
		int			i;

		for (i = 0; i < numAttrs; i++)
		{
			ToastAttrInfo *attr = &ttc->ttc_attr[i];

			if ((attr->tai_colflags & TOASTCOL_NEEDS_DELETE_OLD) != 0)
				toast_delete_datum(ttc->ttc_rel, ttc->ttc_oldvalues[i], false);
		}
	}
}

/*
 * Check for external stored attributes and delete them from the secondary
 * relation.
 */
void
toast_delete_external(Relation rel, const Datum *values, const bool *isnull,
					  bool is_speculative)
{
	TupleDesc	tupleDesc = rel->rd_att;
	int			numAttrs = tupleDesc->natts;
	int			i;

	for (i = 0; i < numAttrs; i++)
	{
		if (TupleDescCompactAttr(tupleDesc, i)->attlen == -1)
		{
			Datum		value = values[i];

			if (isnull[i])
				continue;
			else if (VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(value)) ||
					 VARATT_IS_EXTERNAL_VR(DatumGetPointer(value)))
				toast_delete_datum(rel, value, is_speculative);
		}
	}
}
