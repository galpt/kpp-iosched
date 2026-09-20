// SPDX-License-Identifier: GPL-2.0
/*
 * The KPP I/O scheduler (kyber++). Controls latency by throttling queue
 * depths using scalable techniques.
 *
 * Derived from block/kyber-iosched.c (Linux 7.3-rc3); see that file for
 * origin. KPP coexists with kyber: all symbols/strings renamed kyber->kpp.
 *
 * Copyright (C) 2017 Facebook
 */

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/sbitmap.h>

#include <trace/events/block.h>

#include "elevator.h"
#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-sched.h"

#define CREATE_TRACE_POINTS
#include <trace/events/kpp.h>

/*
 * Scheduling domains: the device is divided into multiple domains based on the
 * request type.
 */
enum {
	KPP_READ,
	KPP_WRITE,
	KPP_DISCARD,
	KPP_OTHER,
	KPP_NUM_DOMAINS,
};

static const char *kpp_domain_names[] = {
	[KPP_READ] = "READ",
	[KPP_WRITE] = "WRITE",
	[KPP_DISCARD] = "DISCARD",
	[KPP_OTHER] = "OTHER",
};

enum {
	/*
	 * In order to prevent starvation of synchronous requests by a flood of
	 * asynchronous requests, we reserve 25% of requests for synchronous
	 * operations.
	 */
	KPP_DEFAULT_ASYNC_PERCENT = 75,
};
/*
 * Maximum device-wide depth for each scheduling domain.
 *
 * Even for fast devices with lots of tags like NVMe, you can saturate the
 * device with only a fraction of the maximum possible queue depth. So, we cap
 * these to a reasonable value.
 */
static const unsigned int kpp_depth[] = {
	[KPP_READ] = 256,
	[KPP_WRITE] = 128,
	[KPP_DISCARD] = 64,
	[KPP_OTHER] = 16,
};

/*
 * Default latency targets for each scheduling domain.
 */
static const u64 kpp_latency_targets[] = {
	[KPP_READ] = 2ULL * NSEC_PER_MSEC,
	[KPP_WRITE] = 10ULL * NSEC_PER_MSEC,
	[KPP_DISCARD] = 5ULL * NSEC_PER_SEC,
};

/*
 * Batch size (number of requests we'll dispatch in a row) for each scheduling
 * domain.
 */
static const unsigned int kpp_batch_size[] = {
	[KPP_READ] = 16,
	[KPP_WRITE] = 8,
	[KPP_DISCARD] = 1,
	[KPP_OTHER] = 1,
};

/*
 * KPP bounded-reuse deltas (frozen, not tunables):
 * - LIFO period 8: 7 inserts at head (LIFO/locality) + 1 at tail
 *   (FIFO/anti-starvation). Reuses the sbitmap ilog2(8) shift, no new knob.
 * - Flush cap 8 kcqs per dispatch.
 * - Timer shard 8 CPUs per run.
 * All deltas are O(1) bounded per-op, preserve FIFO storage (insert position
 * only, no new lists), and keep existing locking (kcq lock / khd lock).
 */
enum {
	KPP_LIFO_PERIOD = 8,
	KPP_FLUSH_CAP = 8,
	KPP_TIMER_SHARD = 8,
};

/*
 * Requests latencies are recorded in a histogram with buckets defined relative
 * to the target latency:
 *
 * <= 1/4 * target latency
 * <= 1/2 * target latency
 * <= 3/4 * target latency
 * <= target latency
 * <= 1 1/4 * target latency
 * <= 1 1/2 * target latency
 * <= 1 3/4 * target latency
 * > 1 3/4 * target latency
 */
enum {
	/*
	 * The width of the latency histogram buckets is
	 * 1 / (1 << KPP_LATENCY_SHIFT) * target latency.
	 */
	KPP_LATENCY_SHIFT = 2,
	/*
	 * The first (1 << KPP_LATENCY_SHIFT) buckets are <= target latency,
	 * thus, "good".
	 */
	KPP_GOOD_BUCKETS = 1 << KPP_LATENCY_SHIFT,
	/* There are also (1 << KPP_LATENCY_SHIFT) "bad" buckets. */
	KPP_LATENCY_BUCKETS = 2 << KPP_LATENCY_SHIFT,
};

/*
 * We measure both the total latency and the I/O latency (i.e., latency after
 * submitting to the device).
 */
enum {
	KPP_TOTAL_LATENCY,
	KPP_IO_LATENCY,
};

static const char *kpp_latency_type_names[] = {
	[KPP_TOTAL_LATENCY] = "total",
	[KPP_IO_LATENCY] = "I/O",
};

/*
 * Per-cpu latency histograms: total latency and I/O latency for each scheduling
 * domain except for KPP_OTHER.
 */
struct kpp_cpu_latency {
	atomic_t buckets[KPP_OTHER][2][KPP_LATENCY_BUCKETS];
};

/*
 * There is a same mapping between ctx & hctx and kcq & khd,
 * we use request->mq_ctx->index_hw to index the kcq in khd.
 */
struct kpp_ctx_queue {
	/*
	 * Used to ensure operations on rq_list and kcq_map to be an atmoic one.
	 * Also protect the rqs on rq_list when merge.
	 */
	spinlock_t lock;
	struct list_head rq_list[KPP_NUM_DOMAINS];
	/*
	 * Bounded-LIFO sequence per domain (KPP delta, O(1)).
	 * Protected by @lock; insert position only, FIFO storage preserved.
	 */
	u32 lifo_seq[KPP_NUM_DOMAINS];
} ____cacheline_aligned_in_smp;

struct kpp_queue_data {
	struct request_queue *q;
	dev_t dev;

	/*
	 * Each scheduling domain has a limited number of in-flight requests
	 * device-wide, limited by these tokens.
	 */
	struct sbitmap_queue domain_tokens[KPP_NUM_DOMAINS];

	struct kpp_cpu_latency __percpu *cpu_latency;

	/* Timer for stats aggregation and adjusting domain tokens. */
	struct timer_list timer;
	/*
	 * Timer shard cursor (KPP delta, O(1)): next possible CPU to process.
	 * Accessed only from timer_fn (serialized per-queue timer); re-armed
	 * via mod_timer while uncovered CPUs remain in this cycle, else rely
	 * on timer_reduce() from completion path (kyber rhythm).
	 * Backport note (6.18/7.1/7.2): keep for_each_possible_cpu on 7.3;
	 * on older trees without for_each_possible_cpu_wrap use manual
	 * (start + n) % num_possible_cpus() with cpu_possible() guard (stub).
	 */
	unsigned int timer_cursor;
	/*
	 * Timer covered count (KPP delta, O(1)): CPUs aggregated this cycle.
	 * Accessed only from timer_fn; reset on full-cycle completion before
	 * percentile evaluation. kzalloc-zeroed, no locking change.
	 */
	unsigned int timer_covered;

	unsigned int latency_buckets[KPP_OTHER][2][KPP_LATENCY_BUCKETS];

	unsigned long latency_timeout[KPP_OTHER];

	int domain_p99[KPP_OTHER];

	/* Target latencies in nanoseconds. */
	u64 latency_targets[KPP_OTHER];
};

struct kpp_hctx_data {
	spinlock_t lock;
	struct list_head rqs[KPP_NUM_DOMAINS];
	unsigned int cur_domain;
	unsigned int batching;
	struct kpp_ctx_queue *kcqs;
	struct sbitmap kcq_map[KPP_NUM_DOMAINS];
	struct sbq_wait domain_wait[KPP_NUM_DOMAINS];
	struct sbq_wait_state *domain_ws[KPP_NUM_DOMAINS];
	atomic_t wait_index[KPP_NUM_DOMAINS];
	/*
	 * Flush cursor per domain (KPP delta, O(1)): resume hint for
	 * __sbitmap_for_each_set(). Protected by @lock (held by dispatch).
	 * Wrap handled by sbitmap (start >= depth restarts at 0).
	 */
	unsigned int flush_cursor[KPP_NUM_DOMAINS];
	/*
	 * Drain sequence per domain (KPP delta, O(1)): bounded-LIFO drain
	 * cadence 7 tail + 1 head per KPP_LIFO_PERIOD. Protected by @lock
	 * (held by dispatch); advance only on successful take, wrap forces
	 * head. AT_HEAD-marked takes bypass without advancing.
	 */
	u32 drain_seq[KPP_NUM_DOMAINS];
};

static int kpp_domain_wake(wait_queue_entry_t *wait, unsigned mode, int flags,
			     void *key);

static unsigned int kpp_sched_domain(blk_opf_t opf)
{
	switch (opf & REQ_OP_MASK) {
	case REQ_OP_READ:
		return KPP_READ;
	case REQ_OP_WRITE:
		return KPP_WRITE;
	case REQ_OP_DISCARD:
		return KPP_DISCARD;
	default:
		return KPP_OTHER;
	}
}

/*
 * Bounded-LIFO helper (KPP delta, O(1)): true means insert at head.
 * 7 head + 1 tail per KPP_LIFO_PERIOD (mask test, no loop, no alloc, no
 * sleep). Called with kcq->lock held; insert position only, FIFO storage
 * (rq_list) preserved, locking unchanged. BLK_MQ_INSERT_AT_HEAD callers are
 * exempt and must not call this helper (no sequence advance). Requeue
 * (token clear via kpp_finish_request) and flush (REQ_OP_FLUSH bypasses the
 * scheduler via blk_mq_request_bypass_insert) never invoke this helper, so
 * they also do not advance the sequence. Counter wrap forces tail.
 */
static bool kpp_take_head(struct kpp_ctx_queue *kcq, unsigned int sched_domain)
{
	u32 seq = kcq->lifo_seq[sched_domain]++;

	if (seq == (u32)-1)
		return false;
	return (seq & (KPP_LIFO_PERIOD - 1)) != (KPP_LIFO_PERIOD - 1);
}

/*
 * Bounded-LIFO drain helper (KPP delta, O(1)): true means take from tail.
 * 7 tail + 1 head per KPP_LIFO_PERIOD (mask test, no loop, no alloc, no
 * sleep). Called with khd->lock held at both dispatch sites; call only
 * after token acquired and list confirmed non-empty so the increment
 * counts only successful takes (throttled/empty peeks do not advance).
 * Counter wrap forces head. AT_HEAD-marked takes bypass this helper
 * without advancing.
 */
static bool kpp_take_tail_for_drain(struct kpp_hctx_data *khd,
				    unsigned int sched_domain)
{
	u32 seq = khd->drain_seq[sched_domain]++;

	if (seq == (u32)-1)
		return false;
	return (seq & (KPP_LIFO_PERIOD - 1)) != (KPP_LIFO_PERIOD - 1);
}

static void flush_latency_buckets(struct kpp_queue_data *kqd,
				  struct kpp_cpu_latency *cpu_latency,
				  unsigned int sched_domain, unsigned int type)
{
	unsigned int *buckets = kqd->latency_buckets[sched_domain][type];
	atomic_t *cpu_buckets = cpu_latency->buckets[sched_domain][type];
	unsigned int bucket;

	for (bucket = 0; bucket < KPP_LATENCY_BUCKETS; bucket++)
		buckets[bucket] += atomic_xchg(&cpu_buckets[bucket], 0);
}

/*
 * Calculate the histogram bucket with the given percentile rank, or -1 if there
 * aren't enough samples yet.
 */
static int calculate_percentile(struct kpp_queue_data *kqd,
				unsigned int sched_domain, unsigned int type,
				unsigned int percentile)
{
	unsigned int *buckets = kqd->latency_buckets[sched_domain][type];
	unsigned int bucket, samples = 0, percentile_samples;

	for (bucket = 0; bucket < KPP_LATENCY_BUCKETS; bucket++)
		samples += buckets[bucket];

	if (!samples)
		return -1;

	/*
	 * We do the calculation once we have 500 samples or one second passes
	 * since the first sample was recorded, whichever comes first.
	 */
	if (!kqd->latency_timeout[sched_domain])
		kqd->latency_timeout[sched_domain] = max(jiffies + HZ, 1UL);
	if (samples < 500 &&
	    time_is_after_jiffies(kqd->latency_timeout[sched_domain])) {
		return -1;
	}
	kqd->latency_timeout[sched_domain] = 0;

	percentile_samples = DIV_ROUND_UP(samples * percentile, 100);
	for (bucket = 0; bucket < KPP_LATENCY_BUCKETS - 1; bucket++) {
		if (buckets[bucket] >= percentile_samples)
			break;
		percentile_samples -= buckets[bucket];
	}
	memset(buckets, 0, sizeof(kqd->latency_buckets[sched_domain][type]));

	trace_kpp_latency(kqd->dev, kpp_domain_names[sched_domain],
			    kpp_latency_type_names[type], percentile,
			    bucket + 1, 1 << KPP_LATENCY_SHIFT, samples);

	return bucket;
}

static void kpp_resize_domain(struct kpp_queue_data *kqd,
				unsigned int sched_domain, unsigned int depth)
{
	depth = clamp(depth, 1U, kpp_depth[sched_domain]);
	if (depth != kqd->domain_tokens[sched_domain].sb.depth) {
		sbitmap_queue_resize(&kqd->domain_tokens[sched_domain], depth);
		trace_kpp_adjust(kqd->dev, kpp_domain_names[sched_domain],
				   depth);
	}
}

static void kpp_timer_fn(struct timer_list *t)
{
	struct kpp_queue_data *kqd = timer_container_of(kqd, t, timer);
	unsigned int sched_domain;
	unsigned int done = 0;
	unsigned int start = kqd->timer_cursor;
	int cpu;
	bool bad = false;

	/*
	 * Shard-8 (KPP delta, O(1)): sum at most KPP_TIMER_SHARD possible
	 * CPUs per run, resuming via timer_cursor with wrap. Evaluate only
	 * on full possible-CPU cycle completion (covered vs num_possible);
	 * re-arm only while uncovered remain, else rely on timer_reduce()
	 * from completion path (kyber rhythm). No alloc, no sleep, no loop
	 * over queue length.
	 */
	for_each_possible_cpu_wrap(cpu, start) {
		struct kpp_cpu_latency *cpu_latency;

		if (done >= KPP_TIMER_SHARD)
			break;
		cpu_latency = per_cpu_ptr(kqd->cpu_latency, cpu);
		for (sched_domain = 0; sched_domain < KPP_OTHER; sched_domain++) {
			flush_latency_buckets(kqd, cpu_latency, sched_domain,
					      KPP_TOTAL_LATENCY);
			flush_latency_buckets(kqd, cpu_latency, sched_domain,
					      KPP_IO_LATENCY);
		}
		done++;
		kqd->timer_cursor = (unsigned int)cpu + 1;
	}
	kqd->timer_covered += done;
	if (kqd->timer_covered < num_possible_cpus()) {
		mod_timer(&kqd->timer, jiffies + 1);
		return;
	}
	kqd->timer_covered = 0;

	/*
	 * Check if any domains have a high I/O latency, which might indicate
	 * congestion in the device. Note that we use the p90; we don't want to
	 * be too sensitive to outliers here.
	 */
	for (sched_domain = 0; sched_domain < KPP_OTHER; sched_domain++) {
		int p90;

		p90 = calculate_percentile(kqd, sched_domain, KPP_IO_LATENCY,
					   90);
		if (p90 >= KPP_GOOD_BUCKETS)
			bad = true;
	}

	/*
	 * Adjust the scheduling domain depths. If we determined that there was
	 * congestion, we throttle all domains with good latencies. Either way,
	 * we ease up on throttling domains with bad latencies.
	 */
	for (sched_domain = 0; sched_domain < KPP_OTHER; sched_domain++) {
		unsigned int orig_depth, depth;
		int p99;

		p99 = calculate_percentile(kqd, sched_domain,
					   KPP_TOTAL_LATENCY, 99);
		/*
		 * This is kind of subtle: different domains will not
		 * necessarily have enough samples to calculate the latency
		 * percentiles during the same window, so we have to remember
		 * the p99 for the next time we observe congestion; once we do,
		 * we don't want to throttle again until we get more data, so we
		 * reset it to -1.
		 */
		if (bad) {
			if (p99 < 0)
				p99 = kqd->domain_p99[sched_domain];
			kqd->domain_p99[sched_domain] = -1;
		} else if (p99 >= 0) {
			kqd->domain_p99[sched_domain] = p99;
		}
		if (p99 < 0)
			continue;

		/*
		 * If this domain has bad latency, throttle less. Otherwise,
		 * throttle more iff we determined that there is congestion.
		 *
		 * The new depth is scaled linearly with the p99 latency vs the
		 * latency target. E.g., if the p99 is 3/4 of the target, then
		 * we throttle down to 3/4 of the current depth, and if the p99
		 * is 2x the target, then we double the depth.
		 */
		if (bad || p99 >= KPP_GOOD_BUCKETS) {
			orig_depth = kqd->domain_tokens[sched_domain].sb.depth;
			depth = (orig_depth * (p99 + 1)) >> KPP_LATENCY_SHIFT;
			kpp_resize_domain(kqd, sched_domain, depth);
		}
	}
}

static struct kpp_queue_data *kpp_queue_data_alloc(struct request_queue *q)
{
	struct kpp_queue_data *kqd;
	int ret = -ENOMEM;
	int i;

	kqd = kzalloc_node(sizeof(*kqd), GFP_KERNEL, q->node);
	if (!kqd)
		goto err;

	kqd->q = q;
	kqd->dev = disk_devt(q->disk);

	kqd->cpu_latency = alloc_percpu_gfp(struct kpp_cpu_latency,
					    GFP_KERNEL | __GFP_ZERO);
	if (!kqd->cpu_latency)
		goto err_kqd;

	timer_setup(&kqd->timer, kpp_timer_fn, 0);
	/* kzalloc zeroes cursor/covered: first cycle starts at CPU 0. */

	for (i = 0; i < KPP_NUM_DOMAINS; i++) {
		WARN_ON(!kpp_depth[i]);
		WARN_ON(!kpp_batch_size[i]);
		ret = sbitmap_queue_init_node(&kqd->domain_tokens[i],
					      kpp_depth[i], -1, false,
					      GFP_KERNEL, q->node);
		if (ret) {
			while (--i >= 0)
				sbitmap_queue_free(&kqd->domain_tokens[i]);
			goto err_buckets;
		}
	}

	for (i = 0; i < KPP_OTHER; i++) {
		kqd->domain_p99[i] = -1;
		kqd->latency_targets[i] = kpp_latency_targets[i];
	}

	return kqd;

err_buckets:
	free_percpu(kqd->cpu_latency);
err_kqd:
	kfree(kqd);
err:
	return ERR_PTR(ret);
}

static void kpp_depth_updated(struct request_queue *q)
{
	blk_mq_set_min_shallow_depth(q, q->async_depth);
}

static int kpp_init_sched(struct request_queue *q, struct elevator_queue *eq)
{
	blk_stat_enable_accounting(q);

	blk_queue_flag_clear(QUEUE_FLAG_SQ_SCHED, q);

	q->elevator = eq;
	q->async_depth = q->nr_requests * KPP_DEFAULT_ASYNC_PERCENT / 100;
	kpp_depth_updated(q);

	return 0;
}

static void *kpp_alloc_sched_data(struct request_queue *q)
{
	struct kpp_queue_data *kqd;

	kqd = kpp_queue_data_alloc(q);
	if (IS_ERR(kqd))
		return NULL;

	return kqd;
}

static void kpp_exit_sched(struct elevator_queue *e)
{
	struct kpp_queue_data *kqd = e->elevator_data;

	timer_shutdown_sync(&kqd->timer);
	blk_stat_disable_accounting(kqd->q);
}

static void kpp_free_sched_data(void *elv_data)
{
	struct kpp_queue_data *kqd = elv_data;
	int i;

	if (!kqd)
		return;

	for (i = 0; i < KPP_NUM_DOMAINS; i++)
		sbitmap_queue_free(&kqd->domain_tokens[i]);
	free_percpu(kqd->cpu_latency);
	kfree(kqd);
}

static void kpp_ctx_queue_init(struct kpp_ctx_queue *kcq)
{
	unsigned int i;

	spin_lock_init(&kcq->lock);
	for (i = 0; i < KPP_NUM_DOMAINS; i++) {
		INIT_LIST_HEAD(&kcq->rq_list[i]);
		kcq->lifo_seq[i] = 0;
	}
}

static int kpp_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct kpp_hctx_data *khd;
	int i;

	khd = kmalloc_node(sizeof(*khd), GFP_KERNEL, hctx->numa_node);
	if (!khd)
		return -ENOMEM;

	khd->kcqs = kmalloc_array_node(hctx->nr_ctx,
				       sizeof(struct kpp_ctx_queue),
				       GFP_KERNEL, hctx->numa_node);
	if (!khd->kcqs)
		goto err_khd;

	for (i = 0; i < hctx->nr_ctx; i++)
		kpp_ctx_queue_init(&khd->kcqs[i]);

	for (i = 0; i < KPP_NUM_DOMAINS; i++) {
		if (sbitmap_init_node(&khd->kcq_map[i], hctx->nr_ctx,
				      ilog2(8), GFP_KERNEL, hctx->numa_node,
				      false, false)) {
			while (--i >= 0)
				sbitmap_free(&khd->kcq_map[i]);
			goto err_kcqs;
		}
	}

	spin_lock_init(&khd->lock);

	for (i = 0; i < KPP_NUM_DOMAINS; i++) {
		INIT_LIST_HEAD(&khd->rqs[i]);
		khd->domain_wait[i].sbq = NULL;
		init_waitqueue_func_entry(&khd->domain_wait[i].wait,
					  kpp_domain_wake);
		khd->domain_wait[i].wait.private = hctx;
		INIT_LIST_HEAD(&khd->domain_wait[i].wait.entry);
		atomic_set(&khd->wait_index[i], 0);
		khd->flush_cursor[i] = 0;
		khd->drain_seq[i] = 0;
	}

	khd->cur_domain = 0;
	khd->batching = 0;

	hctx->sched_data = khd;

	return 0;

err_kcqs:
	kfree(khd->kcqs);
err_khd:
	kfree(khd);
	return -ENOMEM;
}

static void kpp_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct kpp_hctx_data *khd = hctx->sched_data;
	int i;

	for (i = 0; i < KPP_NUM_DOMAINS; i++)
		sbitmap_free(&khd->kcq_map[i]);
	kfree(khd->kcqs);
	kfree(hctx->sched_data);
}

static int rq_get_domain_token(struct request *rq)
{
	return (long)rq->elv.priv[0];
}

static void rq_set_domain_token(struct request *rq, int token)
{
	rq->elv.priv[0] = (void *)(long)token;
}

/*
 * AT_HEAD marker via rq->elv.priv[1] (KPP delta, O(1)): struct request
 * carries elv.priv[2] (include/linux/blk-mq.h); KPP uses priv[0] for the
 * domain token like kyber, so priv[1] is free while KPP is active
 * (mq-deadline uses only priv[0], bfq uses both but never shares a
 * request with KPP). No side map, no extra list, no alloc, no sleep.
 * Set under kcq->lock at insert, peeked/cleared under khd->lock at
 * dispatch; handoff is safe (insert touches kcq list, dispatch touches
 * khd list, flush moves under khd->lock then kcq->lock). Cleared in
 * prepare to avoid stale reuse of static_rqs tags.
 */
static bool rq_is_at_head(struct request *rq)
{
	return rq->elv.priv[1] != NULL;
}

static void rq_set_at_head(struct request *rq)
{
	rq->elv.priv[1] = (void *)1;
}

static void rq_clear_at_head(struct request *rq)
{
	rq->elv.priv[1] = NULL;
}

static void rq_clear_domain_token(struct kpp_queue_data *kqd,
				  struct request *rq)
{
	unsigned int sched_domain;
	int nr;

	nr = rq_get_domain_token(rq);
	if (nr != -1) {
		sched_domain = kpp_sched_domain(rq->cmd_flags);
		sbitmap_queue_clear(&kqd->domain_tokens[sched_domain], nr,
				    rq->mq_ctx->cpu);
	}
}

static void kpp_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	if (!blk_mq_is_sync_read(opf))
		data->shallow_depth = data->q->async_depth;
}

static bool kpp_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	struct blk_mq_ctx *ctx = blk_mq_get_ctx(q);
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(bio->bi_opf, ctx);
	struct kpp_hctx_data *khd = hctx->sched_data;
	struct kpp_ctx_queue *kcq = &khd->kcqs[ctx->index_hw[hctx->type]];
	unsigned int sched_domain = kpp_sched_domain(bio->bi_opf);
	struct list_head *rq_list = &kcq->rq_list[sched_domain];
	bool merged;

	spin_lock(&kcq->lock);
	merged = blk_bio_list_merge(hctx->queue, rq_list, bio, nr_segs);
	spin_unlock(&kcq->lock);

	return merged;
}

static void kpp_prepare_request(struct request *rq)
{
	rq_set_domain_token(rq, -1);
	rq_clear_at_head(rq);
}

static void kpp_insert_requests(struct blk_mq_hw_ctx *hctx,
				  struct list_head *rq_list,
				  blk_insert_t flags)
{
	struct kpp_hctx_data *khd = hctx->sched_data;
	struct request *rq, *next;

	list_for_each_entry_safe(rq, next, rq_list, queuelist) {
		unsigned int sched_domain = kpp_sched_domain(rq->cmd_flags);
		struct kpp_ctx_queue *kcq = &khd->kcqs[rq->mq_ctx->index_hw[hctx->type]];
		struct list_head *head = &kcq->rq_list[sched_domain];

		spin_lock(&kcq->lock);
		trace_block_rq_insert(rq);
		if (flags & BLK_MQ_INSERT_AT_HEAD) {
			/* Exempt: AT_HEAD keeps head position, no advance. */
			list_move(&rq->queuelist, head);
			rq_set_at_head(rq);
		} else if (kpp_take_head(kcq, sched_domain)) {
			list_move(&rq->queuelist, head);
			rq_clear_at_head(rq);
		} else {
			list_move_tail(&rq->queuelist, head);
			rq_clear_at_head(rq);
		}
		sbitmap_set_bit(&khd->kcq_map[sched_domain],
				rq->mq_ctx->index_hw[hctx->type]);
		spin_unlock(&kcq->lock);
	}
}

static void kpp_finish_request(struct request *rq)
{
	struct kpp_queue_data *kqd = rq->q->elevator->elevator_data;

	rq_clear_domain_token(kqd, rq);
	rq_clear_at_head(rq);
}

static void add_latency_sample(struct kpp_cpu_latency *cpu_latency,
			       unsigned int sched_domain, unsigned int type,
			       u64 target, u64 latency)
{
	unsigned int bucket;
	u64 divisor;

	if (latency > 0) {
		divisor = max_t(u64, target >> KPP_LATENCY_SHIFT, 1);
		bucket = min_t(unsigned int, div64_u64(latency - 1, divisor),
			       KPP_LATENCY_BUCKETS - 1);
	} else {
		bucket = 0;
	}

	atomic_inc(&cpu_latency->buckets[sched_domain][type][bucket]);
}

static void kpp_completed_request(struct request *rq, u64 now)
{
	struct kpp_queue_data *kqd = rq->q->elevator->elevator_data;
	struct kpp_cpu_latency *cpu_latency;
	unsigned int sched_domain;
	u64 target;

	sched_domain = kpp_sched_domain(rq->cmd_flags);
	if (sched_domain == KPP_OTHER)
		return;

	cpu_latency = get_cpu_ptr(kqd->cpu_latency);
	target = kqd->latency_targets[sched_domain];
	add_latency_sample(cpu_latency, sched_domain, KPP_TOTAL_LATENCY,
			   target, now - rq->start_time_ns);
	add_latency_sample(cpu_latency, sched_domain, KPP_IO_LATENCY, target,
			   now - rq->io_start_time_ns);
	put_cpu_ptr(kqd->cpu_latency);

	timer_reduce(&kqd->timer, jiffies + HZ / 10);
}

struct flush_kcq_data {
	struct kpp_hctx_data *khd;
	unsigned int sched_domain;
	struct list_head *list;
	unsigned int flushed;
};

static bool flush_busy_kcq(struct sbitmap *sb, unsigned int bitnr, void *data)
{
	struct flush_kcq_data *flush_data = data;
	struct kpp_ctx_queue *kcq = &flush_data->khd->kcqs[bitnr];

	/* Cap-8 (KPP delta, O(1)): stop after KPP_FLUSH_CAP splices. */
	if (flush_data->flushed >= KPP_FLUSH_CAP)
		return false;
	spin_lock(&kcq->lock);
	list_splice_tail_init(&kcq->rq_list[flush_data->sched_domain],
			      flush_data->list);
	sbitmap_clear_bit(sb, bitnr);
	spin_unlock(&kcq->lock);

	flush_data->flushed++;
	/* Resume hint for next flush; wrap handled by sbitmap start check. */
	flush_data->khd->flush_cursor[flush_data->sched_domain] = bitnr + 1;
	return true;
}

static void kpp_flush_busy_kcqs(struct kpp_hctx_data *khd,
				  unsigned int sched_domain,
				  struct list_head *list)
{
	struct flush_kcq_data data = {
		.khd = khd,
		.sched_domain = sched_domain,
		.list = list,
		.flushed = 0,
	};
	unsigned int start = khd->flush_cursor[sched_domain];

	/*
	 * Cap-8 cursor (KPP delta, O(1) bounded): resume at cursor with wrap,
	 * stop after KPP_FLUSH_CAP kcqs. Called with khd->lock held (same as
	 * kyber); per-kcq locking unchanged; FIFO storage preserved (splice
	 * only, no new lists, no alloc, no sleep under lock).
	 */
	__sbitmap_for_each_set(&khd->kcq_map[sched_domain], start,
			       flush_busy_kcq, &data);
}

static int kpp_domain_wake(wait_queue_entry_t *wqe, unsigned mode, int flags,
			     void *key)
{
	struct blk_mq_hw_ctx *hctx = READ_ONCE(wqe->private);
	struct sbq_wait *wait = container_of(wqe, struct sbq_wait, wait);

	sbitmap_del_wait_queue(wait);
	blk_mq_run_hw_queue(hctx, true);
	return 1;
}

static int kpp_get_domain_token(struct kpp_queue_data *kqd,
				  struct kpp_hctx_data *khd,
				  struct blk_mq_hw_ctx *hctx)
{
	unsigned int sched_domain = khd->cur_domain;
	struct sbitmap_queue *domain_tokens = &kqd->domain_tokens[sched_domain];
	struct sbq_wait *wait = &khd->domain_wait[sched_domain];
	struct sbq_wait_state *ws;
	int nr;

	nr = __sbitmap_queue_get(domain_tokens);

	/*
	 * If we failed to get a domain token, make sure the hardware queue is
	 * run when one becomes available. Note that this is serialized on
	 * khd->lock, but we still need to be careful about the waker.
	 */
	if (nr < 0 && list_empty_careful(&wait->wait.entry)) {
		ws = sbq_wait_ptr(domain_tokens,
				  &khd->wait_index[sched_domain]);
		khd->domain_ws[sched_domain] = ws;
		sbitmap_add_wait_queue(domain_tokens, ws, wait);

		/*
		 * Try again in case a token was freed before we got on the wait
		 * queue.
		 */
		nr = __sbitmap_queue_get(domain_tokens);
	}

	/*
	 * If we got a token while we were on the wait queue, remove ourselves
	 * from the wait queue to ensure that all wake ups make forward
	 * progress. It's possible that the waker already deleted the entry
	 * between the !list_empty_careful() check and us grabbing the lock, but
	 * list_del_init() is okay with that.
	 */
	if (nr >= 0 && !list_empty_careful(&wait->wait.entry)) {
		ws = khd->domain_ws[sched_domain];
		spin_lock_irq(&ws->wait.lock);
		sbitmap_del_wait_queue(wait);
		spin_unlock_irq(&ws->wait.lock);
	}

	return nr;
}

static struct request *
kpp_dispatch_cur_domain(struct kpp_queue_data *kqd,
			  struct kpp_hctx_data *khd,
			  struct blk_mq_hw_ctx *hctx)
{
	struct list_head *rqs;
	struct request *rq;
	int nr;

	rqs = &khd->rqs[khd->cur_domain];

	/*
	 * If we already have a flushed request, then we just need to get a
	 * token for it. Otherwise, if there are pending requests in the kcqs,
	 * flush the kcqs, but only if we can get a token. If not, we should
	 * leave the requests in the kcqs so that they can be merged. Note that
	 * khd->lock serializes the flushes, so if we observed any bit set in
	 * the kcq_map, we will always get a request.
	 *
	 * Bounded-LIFO drain (KPP delta, O(1)): AT_HEAD-marked head bypasses
	 * without advancing drain_seq; otherwise 7 tail + 1 head per
	 * KPP_LIFO_PERIOD via kpp_take_tail_for_drain (wrap forces head).
	 * Advance only on successful take (after token, list non-empty).
	 * No alloc, no sleep, lock order khd->kcq unchanged.
	 */
	rq = list_first_entry_or_null(rqs, struct request, queuelist);
	if (rq) {
		nr = kpp_get_domain_token(kqd, khd, hctx);
		if (nr >= 0) {
			if (rq_is_at_head(rq)) {
				rq_clear_at_head(rq);
			} else if (kpp_take_tail_for_drain(khd,
							   khd->cur_domain)) {
				rq = list_last_entry(rqs, struct request,
						     queuelist);
				rq_clear_at_head(rq);
			} else {
				rq_clear_at_head(rq);
			}
			khd->batching++;
			rq_set_domain_token(rq, nr);
			list_del_init(&rq->queuelist);
			return rq;
		} else {
			trace_kpp_throttled(kqd->dev,
					      kpp_domain_names[khd->cur_domain]);
		}
	} else if (sbitmap_any_bit_set(&khd->kcq_map[khd->cur_domain])) {
		nr = kpp_get_domain_token(kqd, khd, hctx);
		if (nr >= 0) {
			kpp_flush_busy_kcqs(khd, khd->cur_domain, rqs);
			rq = list_first_entry(rqs, struct request, queuelist);
			if (rq_is_at_head(rq)) {
				rq_clear_at_head(rq);
			} else if (kpp_take_tail_for_drain(khd,
							   khd->cur_domain)) {
				rq = list_last_entry(rqs, struct request,
						     queuelist);
				rq_clear_at_head(rq);
			} else {
				rq_clear_at_head(rq);
			}
			khd->batching++;
			rq_set_domain_token(rq, nr);
			list_del_init(&rq->queuelist);
			return rq;
		} else {
			trace_kpp_throttled(kqd->dev,
					      kpp_domain_names[khd->cur_domain]);
		}
	}

	/* There were either no pending requests or no tokens. */
	return NULL;
}

static struct request *kpp_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct kpp_queue_data *kqd = hctx->queue->elevator->elevator_data;
	struct kpp_hctx_data *khd = hctx->sched_data;
	struct request *rq;
	int i;

	spin_lock(&khd->lock);

	/*
	 * First, if we are still entitled to batch, try to dispatch a request
	 * from the batch.
	 */
	if (khd->batching < kpp_batch_size[khd->cur_domain]) {
		rq = kpp_dispatch_cur_domain(kqd, khd, hctx);
		if (rq)
			goto out;
	}

	/*
	 * Either,
	 * 1. We were no longer entitled to a batch.
	 * 2. The domain we were batching didn't have any requests.
	 * 3. The domain we were batching was out of tokens.
	 *
	 * Start another batch. Note that this wraps back around to the original
	 * domain if no other domains have requests or tokens.
	 */
	khd->batching = 0;
	for (i = 0; i < KPP_NUM_DOMAINS; i++) {
		if (khd->cur_domain == KPP_NUM_DOMAINS - 1)
			khd->cur_domain = 0;
		else
			khd->cur_domain++;

		rq = kpp_dispatch_cur_domain(kqd, khd, hctx);
		if (rq)
			goto out;
	}

	rq = NULL;
out:
	spin_unlock(&khd->lock);
	return rq;
}

static bool kpp_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct kpp_hctx_data *khd = hctx->sched_data;
	int i;

	for (i = 0; i < KPP_NUM_DOMAINS; i++) {
		if (!list_empty_careful(&khd->rqs[i]) ||
		    sbitmap_any_bit_set(&khd->kcq_map[i]))
			return true;
	}

	return false;
}

#define KPP_LAT_SHOW_STORE(domain, name)				\
static ssize_t kpp_##name##_lat_show(struct elevator_queue *e,	\
				       char *page)			\
{									\
	struct kpp_queue_data *kqd = e->elevator_data;		\
									\
	return sprintf(page, "%llu\n", kqd->latency_targets[domain]);	\
}									\
									\
static ssize_t kpp_##name##_lat_store(struct elevator_queue *e,	\
					const char *page, size_t count)	\
{									\
	struct kpp_queue_data *kqd = e->elevator_data;		\
	unsigned long long nsec;					\
	int ret;							\
									\
	ret = kstrtoull(page, 10, &nsec);				\
	if (ret)							\
		return ret;						\
									\
	kqd->latency_targets[domain] = nsec;				\
									\
	return count;							\
}
KPP_LAT_SHOW_STORE(KPP_READ, read);
KPP_LAT_SHOW_STORE(KPP_WRITE, write);
#undef KPP_LAT_SHOW_STORE

#define KPP_LAT_ATTR(op) __ATTR(op##_lat_nsec, 0644, kpp_##op##_lat_show, kpp_##op##_lat_store)
static const struct elv_fs_entry kpp_sched_attrs[] = {
	KPP_LAT_ATTR(read),
	KPP_LAT_ATTR(write),
	__ATTR_NULL
};
#undef KPP_LAT_ATTR

#define HCTX_FROM_SEQ_FILE(m) ((struct blk_mq_hw_ctx *)(m)->private)
#define KPP_HCTX_DATA(hctx) ((struct kpp_hctx_data *)(hctx)->sched_data)

#ifdef CONFIG_BLK_DEBUG_FS
#define KPP_DEBUGFS_DOMAIN_ATTRS(domain, name)			\
static int kpp_##name##_tokens_show(void *data, struct seq_file *m)	\
{									\
	struct request_queue *q = data;					\
	struct kpp_queue_data *kqd = q->elevator->elevator_data;	\
									\
	sbitmap_queue_show(&kqd->domain_tokens[domain], m);		\
	return 0;							\
}									\
									\
static void *kpp_##name##_rqs_start(struct seq_file *m, loff_t *pos)	\
	__acquires(&KPP_HCTX_DATA(HCTX_FROM_SEQ_FILE(m))->lock)	\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;			\
	struct kpp_hctx_data *khd = hctx->sched_data;			\
									\
	spin_lock(&khd->lock);						\
	return seq_list_start(&khd->rqs[domain], *pos);			\
}									\
									\
static void *kpp_##name##_rqs_next(struct seq_file *m, void *v,	\
				     loff_t *pos)			\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;			\
	struct kpp_hctx_data *khd = hctx->sched_data;			\
									\
	return seq_list_next(v, &khd->rqs[domain], pos);		\
}									\
									\
static void kpp_##name##_rqs_stop(struct seq_file *m, void *v)	\
	__releases(&KPP_HCTX_DATA(HCTX_FROM_SEQ_FILE(m))->lock)	\
{									\
	struct blk_mq_hw_ctx *hctx = m->private;			\
	struct kpp_hctx_data *khd = hctx->sched_data;			\
									\
	spin_unlock(&khd->lock);					\
}									\
									\
static const struct seq_operations kpp_##name##_rqs_seq_ops = {	\
	.start	= kpp_##name##_rqs_start,				\
	.next	= kpp_##name##_rqs_next,				\
	.stop	= kpp_##name##_rqs_stop,				\
	.show	= blk_mq_debugfs_rq_show,				\
};									\
									\
static int kpp_##name##_waiting_show(void *data, struct seq_file *m)	\
{									\
	struct blk_mq_hw_ctx *hctx = data;				\
	struct kpp_hctx_data *khd = hctx->sched_data;			\
	wait_queue_entry_t *wait = &khd->domain_wait[domain].wait;	\
									\
	seq_printf(m, "%d\n", !list_empty_careful(&wait->entry));	\
	return 0;							\
}
KPP_DEBUGFS_DOMAIN_ATTRS(KPP_READ, read)
KPP_DEBUGFS_DOMAIN_ATTRS(KPP_WRITE, write)
KPP_DEBUGFS_DOMAIN_ATTRS(KPP_DISCARD, discard)
KPP_DEBUGFS_DOMAIN_ATTRS(KPP_OTHER, other)
#undef KPP_DEBUGFS_DOMAIN_ATTRS

static int kpp_cur_domain_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;
	struct kpp_hctx_data *khd = hctx->sched_data;

	seq_printf(m, "%s\n", kpp_domain_names[khd->cur_domain]);
	return 0;
}

static int kpp_batching_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;
	struct kpp_hctx_data *khd = hctx->sched_data;

	seq_printf(m, "%u\n", khd->batching);
	return 0;
}

#define KPP_QUEUE_DOMAIN_ATTRS(name)	\
	{#name "_tokens", 0400, kpp_##name##_tokens_show}
static const struct blk_mq_debugfs_attr kpp_queue_debugfs_attrs[] = {
	KPP_QUEUE_DOMAIN_ATTRS(read),
	KPP_QUEUE_DOMAIN_ATTRS(write),
	KPP_QUEUE_DOMAIN_ATTRS(discard),
	KPP_QUEUE_DOMAIN_ATTRS(other),
	{},
};
#undef KPP_QUEUE_DOMAIN_ATTRS

#define KPP_HCTX_DOMAIN_ATTRS(name)					\
	{#name "_rqs", 0400, .seq_ops = &kpp_##name##_rqs_seq_ops},	\
	{#name "_waiting", 0400, kpp_##name##_waiting_show}
static const struct blk_mq_debugfs_attr kpp_hctx_debugfs_attrs[] = {
	KPP_HCTX_DOMAIN_ATTRS(read),
	KPP_HCTX_DOMAIN_ATTRS(write),
	KPP_HCTX_DOMAIN_ATTRS(discard),
	KPP_HCTX_DOMAIN_ATTRS(other),
	{"cur_domain", 0400, kpp_cur_domain_show},
	{"batching", 0400, kpp_batching_show},
	{},
};
#undef KPP_HCTX_DOMAIN_ATTRS
#endif

static struct elevator_type kpp_sched = {
	.ops = {
		.init_sched = kpp_init_sched,
		.exit_sched = kpp_exit_sched,
		.init_hctx = kpp_init_hctx,
		.exit_hctx = kpp_exit_hctx,
		.alloc_sched_data = kpp_alloc_sched_data,
		.free_sched_data = kpp_free_sched_data,
		.limit_depth = kpp_limit_depth,
		.bio_merge = kpp_bio_merge,
		.prepare_request = kpp_prepare_request,
		.insert_requests = kpp_insert_requests,
		.finish_request = kpp_finish_request,
		.requeue_request = kpp_finish_request,
		.completed_request = kpp_completed_request,
		.dispatch_request = kpp_dispatch_request,
		.has_work = kpp_has_work,
		.depth_updated = kpp_depth_updated,
	},
#ifdef CONFIG_BLK_DEBUG_FS
	.queue_debugfs_attrs = kpp_queue_debugfs_attrs,
	.hctx_debugfs_attrs = kpp_hctx_debugfs_attrs,
#endif
	.elevator_attrs = kpp_sched_attrs,
	.elevator_name = "kpp",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("mq-kpp-iosched");

static int __init kpp_init(void)
{
	return elv_register(&kpp_sched);
}

static void __exit kpp_exit(void)
{
	elv_unregister(&kpp_sched);
}

module_init(kpp_init);
module_exit(kpp_exit);

MODULE_AUTHOR("Omar Sandoval");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KPP I/O scheduler");
