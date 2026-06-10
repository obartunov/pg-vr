/*-------------------------------------------------------------------------
 *
 * pgrepack.c
 *		Logical Replication output plugin for REPACK command
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/replication/pgrepack/pgrepack.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/value_representation.h"
#include "commands/repack_internal.h"
#include "replication/snapbuild.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

static void repack_startup(LogicalDecodingContext *ctx,
						   OutputPluginOptions *opt, bool is_init);
static void repack_shutdown(LogicalDecodingContext *ctx);
static void repack_begin_txn(LogicalDecodingContext *ctx,
							 ReorderBufferTXN *txn);
static void repack_commit_txn(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void repack_process_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
								  Relation relation, ReorderBufferChange *change);
static void repack_store_change(LogicalDecodingContext *ctx, Relation relation,
								ConcurrentChangeKind kind, HeapTuple tuple);

void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	cb->startup_cb = repack_startup;
	cb->begin_cb = repack_begin_txn;
	cb->change_cb = repack_process_change;
	cb->commit_cb = repack_commit_txn;
	cb->shutdown_cb = repack_shutdown;
}


/* initialize this plugin */
static void
repack_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
			   bool is_init)
{
	ctx->output_plugin_private = NULL;

	/* Probably unnecessary, as we don't use the SQL interface ... */
	opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;

	if (ctx->output_plugin_options != NIL)
	{
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("this plugin does not expect any options"));
	}
}

static void
repack_shutdown(LogicalDecodingContext *ctx)
{
}

/*
 * As we don't release the slot during processing of particular table, there's
 * no room for SQL interface, even for debugging purposes. Therefore we need
 * neither OutputPluginPrepareWrite() nor OutputPluginWrite() in the plugin
 * callbacks. (Although we might want to write custom callbacks, this API
 * seems to be unnecessarily generic for our purposes.)
 */

/* BEGIN callback */
static void
repack_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
}

/* COMMIT callback */
static void
repack_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				  XLogRecPtr commit_lsn)
{
}

/*
 * Callback for individual changed tuples
 */
static void
repack_process_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					  Relation relation, ReorderBufferChange *change)
{
	RepackDecodingState *private PG_USED_FOR_ASSERTS_ONLY =
		(RepackDecodingState *) ctx->output_writer_private;

	/* Changes of other relation should not have been decoded. */
	Assert(RelationGetRelid(relation) == private->relid);

	/* Decode entry depending on its type */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			{
				HeapTuple	newtuple;

				newtuple = change->data.tp.newtuple;

				/*
				 * Identity checks in the main function should have made this
				 * impossible.
				 */
				if (newtuple == NULL)
					elog(ERROR, "incomplete insert info");

				repack_store_change(ctx, relation, CHANGE_INSERT, newtuple);
			}
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			{
				HeapTuple	oldtuple,
							newtuple;

				oldtuple = change->data.tp.oldtuple;
				newtuple = change->data.tp.newtuple;

				if (newtuple == NULL)
					elog(ERROR, "incomplete update info");

				if (oldtuple != NULL)
					repack_store_change(ctx, relation, CHANGE_UPDATE_OLD, oldtuple);

				repack_store_change(ctx, relation, CHANGE_UPDATE_NEW, newtuple);
			}
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			{
				HeapTuple	oldtuple;

				oldtuple = change->data.tp.oldtuple;

				if (oldtuple == NULL)
					elog(ERROR, "incomplete delete info");

				repack_store_change(ctx, relation, CHANGE_DELETE, oldtuple);
			}
			break;
		default:

			/*
			 * Should not come here. This includes TRUNCATE of the table being
			 * processed. heap_decode() cannot check the file locator easily,
			 * but we assume that TRUNCATE uses AccessExclusiveLock on the
			 * table so it should not occur during REPACK (CONCURRENTLY).
			 */
			Assert(false);
			break;
	}
}

/*
 * Write the given tuple, with the given change kind, to the repack spill
 * file.  Later, the repack decoding worker can read these and replay
 * the operations on the new copy of the table.
 *
 * For each change affecting the table being repacked, we store enough
 * information about each tuple in it, so that it can be replayed in the
 * new copy of the table.
 */
static void
repack_store_change(LogicalDecodingContext *ctx, Relation relation,
					ConcurrentChangeKind kind, HeapTuple tuple)
{
	RepackDecodingState *dstate;
	MemoryContext oldcxt;
	BufFile    *file;
	List	   *attrs_ext = NIL;
	List	   *vr_attnos = NIL;
	int			natt_ext;

	dstate = (RepackDecodingState *) ctx->output_writer_private;
	file = dstate->file;

	/* Store the change kind. */
	BufFileWrite(file, &kind, 1);

	/* Use a frequently-reset context to avoid dealing with leaks manually */
	oldcxt = MemoryContextSwitchTo(dstate->change_cxt);

	/*
	 * If the tuple contains "external indirect" attributes, we need to write
	 * the contents to the file because we have no control over that memory.
	 */
	if (HeapTupleHasExternal(tuple))
	{
		TupleDesc	desc = RelationGetDescr(relation);
		TupleTableSlot *slot;

		/* Initialize the slot, if not done already */
		if (dstate->slot == NULL)
		{
			ResourceOwner saveResourceOwner;

			MemoryContextSwitchTo(dstate->worker_cxt);
			saveResourceOwner = CurrentResourceOwner;
			CurrentResourceOwner = dstate->worker_resowner;
			dstate->slot = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
			MemoryContextSwitchTo(dstate->change_cxt);
			CurrentResourceOwner = saveResourceOwner;
		}

		slot = dstate->slot;
		ExecStoreHeapTuple(tuple, slot, false);

		/*
		 * Loop over all attributes, and find out which ones we need to spill
		 * separately, to wit: each one that's a non-null varlena and stored
		 * out of line.
		 */
		for (int i = 0; i < desc->natts; i++)
		{
			CompactAttribute *attr = TupleDescCompactAttr(desc, i);
			varlena    *varlen;

			if (attr->attisdropped || attr->attlen != -1 ||
				slot_attisnull(slot, i + 1))
				continue;

			slot_getsomeattrs(slot, i + 1);

			/*
			 * This is a non-null varlena datum, but we only care if it's
			 * out-of-line
			 */
			varlen = (varlena *) DatumGetPointer(slot->tts_values[i]);
			if (!VARATT_IS_EXTERNAL(varlen))
				continue;

			/*
			 * We spill any indirect-external attributes separately from the
			 * heap tuple.  Anything else is written as is.
			 */
			if (VARATT_IS_EXTERNAL_INDIRECT(varlen))
				attrs_ext = lappend(attrs_ext, varlen);
			else if (VARATT_IS_VR(varlen))
			{
				/*
				 * Flatten-on-capture: a value representation (VR) datum must
				 * not cross into the spill file, because its substrate
				 * locator names storage that the relfilenode swap is about
				 * to invalidate (and the apply side must never preserve a
				 * stale locator).  Remember the attribute; after the loop
				 * the tuple is re-formed with the VR datums replaced by
				 * their ordinary logical values, so only logical bytes are
				 * spilled and the normal write path decides the new
				 * representation at apply.  Accepted v0 semantics: if no
				 * selection policy opts the applied value back into a VR,
				 * a captured row's value becomes ordinary TOAST/plain in
				 * the new heap - logical capture preserves the value, not
				 * the representation.
				 *
				 * This is the no-chunk capture slice: a chunk-writing change
				 * (a transaction that wrote TOAST chunks) is refused earlier
				 * by ReorderBufferToastReplace and never reaches this
				 * callback with a VR datum.  Above vr_logical_capture_limit
				 * the flatten below refuses deterministically and the
				 * REPACK fails cleanly.
				 */
				vr_attnos = lappend_int(vr_attnos, i);
			}
			else
			{
				/*
				 * Logical decoding should not produce "external expanded"
				 * attributes (those actually should never appear on disk), so
				 * only TOASTed attribute can be seen here.
				 *
				 * We get here if the table has external values but only
				 * in-line values are being updated now.
				 */
				Assert(VARATT_IS_EXTERNAL_ONDISK(varlen));
			}
		}

		/*
		 * Replace each captured VR datum with its flattened logical value
		 * and re-form the tuple.  The re-formed tuple lives in change_cxt
		 * (reset after the write); indirect-external attributes, if any,
		 * keep their pointers in the re-formed tuple, so the
		 * scan-for-indirect pairing on the restore side is unaffected.
		 */
		if (vr_attnos != NIL)
		{
			Datum	   *values;
			bool	   *isnull;
			HeapTuple	newtup;

			slot_getallattrs(slot);

			values = (Datum *) palloc(desc->natts * sizeof(Datum));
			isnull = (bool *) palloc(desc->natts * sizeof(bool));
			memcpy(values, slot->tts_values, desc->natts * sizeof(Datum));
			memcpy(isnull, slot->tts_isnull, desc->natts * sizeof(bool));

			foreach_int(attno, vr_attnos)
				values[attno] = vr_capture_logical_value(values[attno],
														 dstate->change_cxt);

			newtup = heap_form_tuple(desc, values, isnull);
			newtup->t_self = tuple->t_self;
			newtup->t_tableOid = tuple->t_tableOid;
			tuple = newtup;
		}

		ExecClearTuple(slot);
	}

	/*
	 * First, write the heap tuple (re-formed above if it carried a value
	 * representation), prefixed by its length.  Note
	 * that the external-toast tag for each toasted attribute will be present
	 * in what we write, so that we know where to restore each one later.
	 */
	BufFileWrite(file, &tuple->t_len, sizeof(tuple->t_len));
	BufFileWrite(file, tuple->t_data, tuple->t_len);

	/* Then, write the number of external attributes we found. */
	natt_ext = list_length(attrs_ext);
	BufFileWrite(file, &natt_ext, sizeof(natt_ext));

	/* Finally, the attributes themselves, if any */
	foreach_ptr(varlena, attr_val, attrs_ext)
	{
		attr_val = detoast_external_attr(attr_val);
		BufFileWrite(file, attr_val, VARSIZE_ANY(attr_val));
		/* These attributes could be large, so free them right away */
		pfree(attr_val);
	}

	/* Cleanup. */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(dstate->change_cxt);
}
