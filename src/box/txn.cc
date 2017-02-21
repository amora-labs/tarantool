/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "txn.h"
#include "engine.h"
#include "box.h" /* global recovery */
#include "tuple.h"
#include "recovery.h"
#include "wal.h"
#include <fiber.h>
#include "xrow.h"
#include "iproto_constants.h"

enum {
	/**
	 * Maximum recursion depth for on_replace triggers.
	 * Large numbers may corrupt C stack.
	 */
	TXN_SUB_STMT_MAX = 3
};

double too_long_threshold;
/** Pool of transaction objects. */
static struct mempool txn_pool;

static void
txn_add_redo(struct txn_stmt *stmt, struct request *request)
{
	stmt->row = request->header;
	if (request->header != NULL)
		return;

	/* Create a redo log row for Lua requests */
	struct xrow_header *row;
	row = region_alloc_object_xc(&in_txn()->region, struct xrow_header);
	/* Initialize members explicitly to save time on memset() */
	row->type = request->type;
	row->replica_id = 0;
	row->lsn = 0;
	row->sync = 0;
	row->tm = 0;
	row->tx_id = 0;
	row->coordinator_id = 0;
	row->bodycnt = request_encode_xc(request, row->body, &in_txn()->region);
	stmt->row = row;
}

/** Initialize a new stmt object within txn. */
static struct txn_stmt *
txn_stmt_new(struct txn *txn)
{
	struct txn_stmt *stmt;
	stmt = region_alloc_object_xc(&txn->region, struct txn_stmt);

	/* Initialize members explicitly to save time on memset() */
	stmt->space = NULL;
	stmt->old_tuple = NULL;
	stmt->new_tuple = NULL;
	stmt->engine_savepoint = NULL;
	stmt->row = NULL;

	stailq_add_tail_entry(&txn->stmts, stmt, next);
	++txn->in_sub_stmt;
	return stmt;
}

struct txn *
txn_begin(bool is_autocommit)
{
	assert(! in_txn());
	struct txn *txn = (struct txn *) mempool_alloc_xc(&txn_pool);
	/* Initialize members explicitly to save time on memset() */
	stailq_create(&txn->stmts);
	txn->is_two_phase = false;
	txn->in_prepare = false;
	txn->tx_id = UINT64_MAX;
	txn->coordinator_id = UINT32_MAX;
	txn->n_rows = 0;
	txn->is_autocommit = is_autocommit;
	txn->has_triggers  = false;
	txn->in_sub_stmt = 0;
	txn->engine = NULL;
	txn->engine_tx = NULL;
	region_create(&txn->region, cord_slab_cache());
	txn->fiber_on_yield.run = NULL;
	txn->fiber_on_stop.run = NULL;
	/* fiber_on_yield/fiber_on_stop initialized by engine on demand */
	fiber_set_txn(fiber(), txn);
	return txn;
}

struct txn *
txn_begin_two_phase(uint64_t tx_id, uint32_t coordinator_id)
{
	struct txn *txn = txn_begin(false);
	txn->tx_id = tx_id;
	txn->coordinator_id = coordinator_id;
	txn->is_two_phase = true;
	return txn;
}

void
txn_prepare_two_phase(struct txn *txn, struct xrow_header *header)
{
	if (txn->in_prepare)
		tnt_raise(ClientError, ER_ALREADY_PREPARED);
	if (! txn->is_two_phase)
		tnt_raise(ClientError, ER_ILLEGAL_PARAMS,
			  "can't prepare not two-phase transaction");
	assert(txn == in_txn());
	assert(! txn->in_prepare);
	assert(txn->is_two_phase);
	assert(header->tx_id == txn->tx_id);
	assert(header->coordinator_id == txn->coordinator_id);
	(void) header;
	txn->in_prepare = true;
	if (txn->engine) {
		txn->engine->prepare_two_phase(txn);
	}
}

void
txn_begin_in_engine(Engine *engine, struct txn *txn)
{
	if (txn->engine == NULL) {
		assert(stailq_empty(&txn->stmts));
		txn->engine = engine;
		engine->begin(txn);
	} else if (txn->engine != engine) {
		/**
		 * Only one engine can be used in
		 * a multi-statement transaction currently.
		 */
		tnt_raise(ClientError, ER_CROSS_ENGINE_TRANSACTION);
	}
}

struct txn *
txn_begin_stmt(struct space *space)
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		txn = txn_begin(true);
	else if (txn->in_sub_stmt > TXN_SUB_STMT_MAX)
		tnt_raise(ClientError, ER_SUB_STMT_MAX);
	else if (txn->in_prepare) {
		assert(txn->is_two_phase);
		tnt_raise(ClientError, ER_CHANGE_PREPARED);
	}

	assert(! txn->in_prepare);
	Engine *engine = space->handler->engine;
	txn_begin_in_engine(engine, txn);
	struct txn_stmt *stmt = txn_stmt_new(txn);
	stmt->space = space;

	engine->beginStatement(txn);
	return txn;
}

/**
 * End a statement. In autocommit mode, end
 * the current transaction as well.
 */
void
txn_commit_stmt(struct txn *txn, struct request *request)
{
	assert(txn->in_sub_stmt > 0);
	assert(! txn->in_prepare);
	/*
	 * Run on_replace triggers. For now, disallow mutation
	 * of tuples in the trigger.
	 */
	struct txn_stmt *stmt = stailq_last_entry(&txn->stmts,
						  struct txn_stmt, next);

	/* Create WAL record for the write requests in non-temporary spaces */
	if (!space_is_temporary(stmt->space)) {
		txn_add_redo(stmt, request);
		++txn->n_rows;
	}
	/*
	 * If there are triggers, and they are not disabled, and
	 * the statement found any rows, run triggers.
	 * XXX:
	 * - vinyl doesn't set old/new tuple, so triggers don't
	 *   work for it
	 * - perhaps we should run triggers even for deletes which
	 *   doesn't find any rows
	 */
	if (!rlist_empty(&stmt->space->on_replace) &&
	    stmt->space->run_triggers && (stmt->old_tuple || stmt->new_tuple)) {
		trigger_run(&stmt->space->on_replace, txn);
	}
	--txn->in_sub_stmt;
	if (txn->is_autocommit && txn->in_sub_stmt == 0)
		txn_commit(txn);
}


static int64_t
txn_write_to_wal(struct txn *txn)
{
	assert(txn->n_rows > 0);

	struct wal_request *req;
	req = (struct wal_request *)region_aligned_alloc_xc(
		&fiber()->gc,
		sizeof(struct wal_request) +
		         sizeof(req->rows[0]) * txn->n_rows,
		alignof(struct wal_request));
	/*
	 * Note: offsetof(struct wal_request, rows) is more appropriate,
	 * but compiler warns.
	 */
	req->n_rows = 0;

	struct txn_stmt *stmt;
	stailq_foreach_entry(stmt, &txn->stmts, next) {
		if (stmt->row == NULL)
			continue; /* A read (e.g. select) request */
		/*
		 * Bump current LSN even if wal_mode = NONE, so that
		 * snapshots still works with WAL turned off.
		 */
		recovery_fill_lsn(recovery, stmt->row);
		stmt->row->tm = ev_now(loop());
		req->rows[req->n_rows++] = stmt->row;
	}
	assert(req->n_rows == txn->n_rows);

	ev_tstamp start = ev_now(loop()), stop;
	int64_t res;
	if (wal == NULL) {
		/** wal_mode = NONE or initial recovery. */
		res = vclock_sum(&recovery->vclock);
	} else {
		res = wal_write(req);
	}

	stop = ev_now(loop());
	if (stop - start > too_long_threshold)
		say_warn("too long WAL write: %.3f sec", stop - start);
	if (res < 0) {
		/* Cascading rollback. */
		txn_rollback(); /* Perform our part of cascading rollback. */
		/*
		 * Move fiber to end of event loop to avoid
		 * execution of any new requests before all
		 * pending rollbacks are processed.
		 */
		fiber_reschedule();
		tnt_raise(LoggedError, ER_WAL_IO);
	}
	/*
	 * Use vclock_sum() from WAL writer as transaction signature.
	 */
	return res;
}

void
txn_commit(struct txn *txn)
{
	assert(txn == in_txn());
	assert(stailq_empty(&txn->stmts) || txn->engine);
	assert(! txn->is_two_phase || txn->in_prepare);

	/* Do transaction conflict resolving */
	if (txn->engine) {
		int64_t signature = -1;
		if (! txn->is_two_phase)
			txn->engine->prepare(txn);

		if (txn->n_rows > 0)
			signature = txn_write_to_wal(txn);
		/*
		 * The transaction is in the binary log. No action below
		 * may throw. In case an error has happened, there is
		 * no other option but terminate.
		 */
		if (txn->has_triggers)
			trigger_run(&txn->on_commit, txn);

		txn->engine->commit(txn, signature);
	}
	region_destroy(&txn->region);
	mempool_free(&txn_pool, txn);
	/** Free volatile txn memory. */
	fiber_gc();
	fiber_set_txn(fiber(), NULL);
}

/**
 * Void all effects of the statement, but
 * keep it in the list - to maintain
 * limit on the number of statements in a
 * transaction.
 */
void
txn_rollback_stmt()
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		return;
	if (txn->is_autocommit)
		return txn_rollback();
	if (txn->in_sub_stmt == 0)
		return;
	struct txn_stmt *stmt = stailq_last_entry(&txn->stmts, struct txn_stmt,
						  next);
	txn->engine->rollbackStatement(txn, stmt);
	if (stmt->row != NULL) {
		stmt->row = NULL;
		--txn->n_rows;
		assert(txn->n_rows >= 0);
	}
	--txn->in_sub_stmt;
}

void
txn_rollback()
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		return;
	if (txn->has_triggers)
		trigger_run(&txn->on_rollback, txn); /* must not throw. */
	if (txn->engine)
		txn->engine->rollback(txn);
	region_destroy(&txn->region);
	mempool_free(&txn_pool, txn);
	/** Free volatile txn memory. */
	fiber_gc();
	fiber_set_txn(fiber(), NULL);
}

void
txn_check_autocommit(struct txn *txn, const char *where)
{
	if (txn->is_autocommit == false) {
		tnt_raise(ClientError, ER_UNSUPPORTED,
			  where, "multi-statement transactions");
	}
}

void
txn_init()
{
	mempool_create(&txn_pool, cord_slab_cache(), sizeof(struct txn));
}

extern "C" {

bool
box_txn()
{
	return in_txn() != NULL;
}

int
box_txn_begin()
{
	try {
		if (in_txn())
			tnt_raise(ClientError, ER_ACTIVE_TRANSACTION);
		(void) txn_begin(false);
	} catch (Exception  *e) {
		return -1; /* pass exception  through FFI */
	}
	return 0;
}

int
box_txn_begin_two_phase(uint64_t tx_id, uint32_t coordinator_id)
{
	try {
		if (in_txn())
			tnt_raise(ClientError, ER_ACTIVE_TRANSACTION);
		(void) txn_begin_two_phase(tx_id, coordinator_id);
	} catch (Exception  *e) {
		return -1; /* pass exception  through FFI */
	}
	return 0;
}

int
box_txn_prepare_two_phase()
{
	struct txn *txn = in_txn();
	if (! txn) {
		diag_set(ClientError, ER_NO_ACTIVE_TRANSACTION);
		return -1;
	}
	try {
		struct xrow_header row;
		memset(&row, 0, sizeof(row));
		row.type = IPROTO_PREPARE;
		row.replica_id = recovery->replica_id;
		row.tx_id = txn->tx_id;
		row.coordinator_id = txn->coordinator_id;
		txn_prepare_two_phase(txn, &row);
	} catch (Exception *e) {
		return -1;
	}
	return 0;
}

int
box_txn_commit()
{
	struct txn *txn = in_txn();
	/**
	 * COMMIT is like BEGIN or ROLLBACK
	 * a "transaction-initiating statement".
	 * Do nothing if transaction is not started,
	 * it's the same as BEGIN + COMMIT.
	*/
	if (! txn)
		return 0;
	if (txn->in_sub_stmt) {
		diag_set(ClientError, ER_COMMIT_IN_SUB_STMT);
		return -1;
	}
	if (txn->is_two_phase && !txn->in_prepare) {
		diag_set(ClientError, ER_COMMIT_BEFORE_PREPARE);
		return -1;
	}
	try {
		txn_commit(txn);
	} catch (Exception *e) {
		txn_rollback();
		return -1;
	}
	return 0;
}

int
box_txn_rollback()
{
	struct txn *txn = in_txn();
	if (txn && txn->in_sub_stmt) {
		diag_set(ClientError, ER_ROLLBACK_IN_SUB_STMT);
		return -1;
	}
	txn_rollback(); /* doesn't throw */
	return 0;
}

void *
box_txn_alloc(size_t size)
{
	union natural_align {
		void *p;
		double lf;
		long l;
	};
	return region_aligned_alloc(txn_region(), size,
	                            alignof(union natural_align));
}

} /* extern "C" */
