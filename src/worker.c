/*
 * worker.c — event queue, curation triggers, worker thread / asper_tick.
 *
 * Owns the background scheduling policy: the worker loop,
 * the access-boost batch, and the shared cycle-slot helpers. The cycle-slot
 * protocol itself is specified in asper_internal.h.
 */

#include <stdlib.h>
#include <string.h>

#include "asper_internal.h"

/* Shared with api.c (asper_flush / asper_run_due_work callers declare these
 * `extern`): blocking acquisition of the curator cycle slot. asper_flush(1)
 * borrows the recall_waiting counter while waiting so the worker defers new
 * cycles to it, exactly as it does for recall. No-ops in no-thread contexts,
 * where all curator work is already serialized on the caller. */
void asper_cycle_slot_acquire(asper_ctx *c);
void asper_cycle_slot_release(asper_ctx *c);

void asper_cycle_slot_acquire(asper_ctx *c) {
  if (c->no_threads) return;
  os_mutex_lock(&c->ev_mu);
  c->recall_waiting++;
  while (c->cycle_busy)
    os_cond_wait(&c->done_cv, &c->ev_mu);
  c->cycle_busy = true;
  c->recall_waiting--;
  os_mutex_unlock(&c->ev_mu);
}

void asper_cycle_slot_release(asper_ctx *c) {
  if (c->no_threads) return;
  os_mutex_lock(&c->ev_mu);
  c->cycle_busy = false;
  os_cond_broadcast(&c->done_cv);
  os_cond_signal(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
}

/* ---- access batch ------------------------------------------------------- */

void asper_access_note(asper_ctx *c, const char *id) {
  bool dropped = false;
  if (!c || !id || strlen(id) != 36) return;
  os_mutex_lock(&c->ev_mu);
  if (c->access_n == c->access_cap) {
    size_t ncap = c->access_cap ? c->access_cap * 2 : 32;
    char(*nb)[37] = realloc(c->access_batch, ncap * sizeof *nb);
    if (!nb) {
      dropped = true;
    } else {
      c->access_batch = nb;
      c->access_cap = ncap;
    }
  }
  if (!dropped) {
    memcpy(c->access_batch[c->access_n], id, 37);
    c->access_n++;
  }
  os_mutex_unlock(&c->ev_mu);
  if (dropped)
    asper_log(c, ASPER_LOG_WARN, "worker",
              "access note dropped: out of memory");
}

asper_err asper_access_flush(asper_ctx *c) {
  char(*batch)[37];
  size_t n;
  asper_op op;
  asper_err e;

  if (!c) return ASPER_ERR_INVALID;

  os_mutex_lock(&c->ev_mu);
  batch = c->access_batch;
  n = c->access_n;
  c->access_batch = NULL;
  c->access_n = 0;
  c->access_cap = 0;
  os_mutex_unlock(&c->ev_mu);

  if (n == 0) {
    free(batch);
    return ASPER_OK;
  }

  memset(&op, 0, sizeof op);
  op.kind = ASPER_OP_ACCESS;
  op.at = asper_clock_now(&c->clock);
  op.ids = batch;
  op.ids_n = n;
  /* from_curator=true: the ACCESS op is batch-level bookkeeping — under the
   * "batch" sync policy the fsync happens at the surrounding flush point
   * (cycle end / asper_flush / close), not per access batch. */
  e = asper_apply_op(c, &op, true);
  asper_op_free(&op);
  if (e != ASPER_OK)
    asper_log(c, ASPER_LOG_WARN, "worker", "access flush failed: %s",
              asper_err_name(e));
  return e;
}

/* ---- shared due-work driver -------------------------------------------- */

asper_err asper_run_due_work(asper_ctx *c, bool force_cycle, bool full) {
  asper_err first = ASPER_OK, e;
  size_t tn;
  asper_time lta, last_maintenance, now;
  bool batch_trig, idle_trig, run_cycle, maint_due;
  size_t journal_ops;

  if (!c) return ASPER_ERR_INVALID;
  now = asper_clock_now(&c->clock);

  os_mutex_lock(&c->ev_mu);
  tn = c->turns_n;
  lta = c->last_turn_at;
  last_maintenance = c->last_maintenance;
  os_mutex_unlock(&c->ev_mu);

  batch_trig = c->cfg.turn_batch > 0 && tn >= (size_t)c->cfg.turn_batch;
  idle_trig = tn > 0 && c->cfg.idle_flush_s > 0 &&
              now - lta >= c->cfg.idle_flush_s;
  run_cycle = tn > 0 && (force_cycle || batch_trig || idle_trig);
  maint_due = c->cfg.maintenance_interval_s > 0 &&
              now - last_maintenance >= c->cfg.maintenance_interval_s;

  if (c->has_curator && (run_cycle || full || maint_due)) {
    bool use_slot = !c->no_threads;
    if (use_slot) asper_cycle_slot_acquire(c);
    if (run_cycle) {
      e = asper_curation_cycle(c, force_cycle || (idle_trig && !batch_trig));
      if (first == ASPER_OK) first = e;
    }
    if (full || maint_due) {
      e = asper_maintenance_review(c, full);
      if (first == ASPER_OK) first = e;
    }
    if (use_slot) asper_cycle_slot_release(c);
  }

  e = asper_access_flush(c);
  if (first == ASPER_OK) first = e;
  e = asper_journal_sync(c);
  if (first == ASPER_OK) first = e;

  os_mutex_lock(&c->journal_mu);
  journal_ops = c->store.journal_ops;
  os_mutex_unlock(&c->journal_mu);
  if (full || journal_ops > (size_t)c->cfg.journal_max_ops) {
    e = asper_store_compact(c);
    if (first == ASPER_OK) first = e;
  }
  if (c->has_embedder && asper_cache_needs_save(c)) {
    e = asper_cache_save(c);
    if (e != ASPER_OK && first == ASPER_OK) first = e;
  }
  return first;
}

asper_err asper_tick(asper_ctx *c) {
  if (!c) return ASPER_ERR_INVALID;
  if (!c->no_threads) return ASPER_OK;
  return asper_run_due_work(c, false, false);
}

/* ---- worker thread ------------------------------------------------------ */

#ifndef ASPER_NO_THREADS

/* Event-driven compaction check, run after each completed curator cycle.
 * Called with ev_mu held; releases it around the store work (compaction
 * takes the table/journal locks itself). */
static void worker_maybe_compact(asper_ctx *c) {
  size_t journal_ops;
  os_mutex_lock(&c->journal_mu);
  journal_ops = c->store.journal_ops;
  os_mutex_unlock(&c->journal_mu);
  if (journal_ops <= (size_t)c->cfg.journal_max_ops) return;
  os_mutex_unlock(&c->ev_mu);
  (void)asper_access_flush(c);
  (void)asper_store_compact(c);
  if (c->has_embedder && asper_cache_needs_save(c))
    (void)asper_cache_save(c);
  os_mutex_lock(&c->ev_mu);
}

static void *asper_worker_main(void *arg) {
  asper_ctx *c = (asper_ctx *)arg;
  int64_t cycle_retry_ms = 1000;
  /* Last time this thread *attempted* a maintenance review. Guards against
   * a due-loop if a review declines to advance c->last_maintenance (e.g. no
   * candidates); curator.c still applies its own interval gate. */
  asper_time last_review_try = 0;

  os_mutex_lock(&c->ev_mu);
  while (!c->stop_worker) {
    asper_time now = asper_clock_now(&c->clock);
    size_t tn = c->turns_n;
    bool batch_trig = c->cfg.turn_batch > 0 &&
                      tn >= (size_t)c->cfg.turn_batch;
    bool idle_trig = tn > 0 && c->cfg.idle_flush_s > 0 &&
                     now - c->last_turn_at >= c->cfg.idle_flush_s;
    asper_time mbase = c->last_maintenance > last_review_try
                           ? c->last_maintenance
                           : last_review_try;
    bool maint_due = c->cfg.maintenance_interval_s > 0 &&
                     now - mbase >= c->cfg.maintenance_interval_s;
    /* Defer new cycles while a recall is waiting (recall takes priority). */
    bool can_work = c->has_curator && !c->cycle_busy &&
                    c->recall_waiting == 0;
    int64_t wait_ms;

    if (can_work && (batch_trig || idle_trig)) {
      /* Idle flush with fewer than turn_batch turns forces a partial
       * batch through the cycle. */
      bool force = idle_trig && !batch_trig;
      asper_err cycle_rc;
      c->cycle_busy = true;
      os_mutex_unlock(&c->ev_mu);
      cycle_rc = asper_curation_cycle(c, force);
      os_mutex_lock(&c->ev_mu);
      c->cycle_busy = false;
      os_cond_broadcast(&c->done_cv);
      worker_maybe_compact(c);
      if (cycle_rc != ASPER_OK && !c->stop_worker) {
        (void)os_cond_timedwait(&c->ev_cv, &c->ev_mu, cycle_retry_ms);
        if (cycle_retry_ms < 60000)
          cycle_retry_ms = cycle_retry_ms > 30000
                               ? 60000 : cycle_retry_ms * 2;
      } else {
        cycle_retry_ms = 1000;
      }
      continue;
    }
    if (can_work && maint_due) {
      last_review_try = now;
      c->cycle_busy = true;
      os_mutex_unlock(&c->ev_mu);
      (void)asper_maintenance_review(c, false);
      os_mutex_lock(&c->ev_mu);
      c->cycle_busy = false;
      os_cond_broadcast(&c->done_cv);
      worker_maybe_compact(c);
      continue;
    }

    wait_ms = -1;
    if (tn > 0 && c->cfg.idle_flush_s > 0) {
      int64_t rem = ((c->last_turn_at + c->cfg.idle_flush_s) - now) * 1000;
      if (rem < 0) rem = 0;
      wait_ms = rem;
    }
    if (c->has_curator && c->cfg.maintenance_interval_s > 0) {
      int64_t rem = ((mbase + c->cfg.maintenance_interval_s) - now) * 1000;
      if (rem < 0) rem = 0;
      if (wait_ms < 0 || rem < wait_ms) wait_ms = rem;
    }
    /* Deadline passed but the work could not start (slot busy, recall
     * pending, degraded without curator): block on the condition instead of
     * spinning — every slot release and trigger signals ev_cv. */
    if (wait_ms == 0) wait_ms = -1;

    if (wait_ms < 0)
      os_cond_wait(&c->ev_cv, &c->ev_mu);
    else
      (void)os_cond_timedwait(&c->ev_cv, &c->ev_mu, wait_ms);
  }
  os_mutex_unlock(&c->ev_mu);
  return NULL;
}

#endif /* !ASPER_NO_THREADS */

asper_err asper_worker_start(asper_ctx *c) {
  if (!c) return ASPER_ERR_INVALID;
#ifdef ASPER_NO_THREADS
  c->no_threads = true;
  c->worker_running = false;
  return ASPER_OK;
#else
  {
    asper_err e;
    c->no_threads = false;
    c->stop_worker = false;
    e = os_thread_start(&c->worker, asper_worker_main, c);
    if (e != ASPER_OK)
      return asper_seterr(c, e, "worker thread start failed");
    c->worker_running = true;
    return ASPER_OK;
  }
#endif
}

void asper_worker_stop(asper_ctx *c) {
  if (!c || !c->worker_running) return;
  os_mutex_lock(&c->ev_mu);
  c->stop_worker = true;
  os_cond_broadcast(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
  os_thread_join(&c->worker);
  c->worker_running = false;
  c->stop_worker = false;
}

void asper_worker_kick(asper_ctx *c) {
  if (!c || !c->worker_running) return;
  os_mutex_lock(&c->ev_mu);
  os_cond_signal(&c->ev_cv);
  os_mutex_unlock(&c->ev_mu);
}
