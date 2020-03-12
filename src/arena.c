#define JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/decay.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/util.h"

JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS

/******************************************************************************/
/* Data. */

/*
 * Define names for both unininitialized and initialized phases, so that
 * options and mallctl processing are straightforward.
 */
const char *percpu_arena_mode_names[] = {
	"percpu",
	"phycpu",
	"disabled",
	"percpu",
	"phycpu"
};
percpu_arena_mode_t opt_percpu_arena = PERCPU_ARENA_DEFAULT;

ssize_t opt_dirty_decay_ms = DIRTY_DECAY_MS_DEFAULT;
ssize_t opt_muzzy_decay_ms = MUZZY_DECAY_MS_DEFAULT;

static atomic_zd_t dirty_decay_ms_default;
static atomic_zd_t muzzy_decay_ms_default;

const uint64_t h_steps[SMOOTHSTEP_NSTEPS] = {
#define STEP(step, h, x, y)			\
		h,
		SMOOTHSTEP
#undef STEP
};

static div_info_t arena_binind_div_info[SC_NBINS];

size_t opt_oversize_threshold = OVERSIZE_THRESHOLD_DEFAULT;
size_t oversize_threshold = OVERSIZE_THRESHOLD_DEFAULT;
static unsigned huge_arena_ind;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static bool arena_decay_dirty(tsdn_t *tsdn, arena_t *arena,
    bool is_background_thread, bool all);
static void arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, edata_t *slab,
    bin_t *bin);

/******************************************************************************/

void
arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy) {
	*nthreads += arena_nthreads_get(arena, false);
	*dss = dss_prec_names[arena_dss_prec_get(arena)];
	*dirty_decay_ms = arena_dirty_decay_ms_get(arena);
	*muzzy_decay_ms = arena_muzzy_decay_ms_get(arena);
	pa_shard_basic_stats_merge(&arena->pa_shard, nactive, ndirty, nmuzzy);
}

void
arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy, arena_stats_t *astats,
    bin_stats_data_t *bstats, arena_stats_large_t *lstats,
    pa_extent_stats_t *estats) {
	cassert(config_stats);

	arena_basic_stats_merge(tsdn, arena, nthreads, dss, dirty_decay_ms,
	    muzzy_decay_ms, nactive, ndirty, nmuzzy);

	size_t base_allocated, base_resident, base_mapped, metadata_thp;
	base_stats_get(tsdn, arena->base, &base_allocated, &base_resident,
	    &base_mapped, &metadata_thp);
	size_t pa_mapped = atomic_load_zu(&arena->pa_shard.stats->pa_mapped,
	    ATOMIC_RELAXED);
	astats->mapped += base_mapped + pa_mapped;

	LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);

	astats->pa_shard_stats.retained +=
	    ecache_npages_get(&arena->pa_shard.ecache_retained) << LG_PAGE;
	astats->pa_shard_stats.edata_avail +=  atomic_load_zu(
	    &arena->pa_shard.edata_cache.count, ATOMIC_RELAXED);

	/* Dirty decay stats */
	locked_inc_u64_unsynchronized(
	    &astats->pa_shard_stats.decay_dirty.npurge,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->pa_shard.stats->decay_dirty.npurge));
	locked_inc_u64_unsynchronized(
	    &astats->pa_shard_stats.decay_dirty.nmadvise,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->pa_shard.stats->decay_dirty.nmadvise));
	locked_inc_u64_unsynchronized(
	    &astats->pa_shard_stats.decay_dirty.purged,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->pa_shard.stats->decay_dirty.purged));

	/* Decay stats */
	locked_inc_u64_unsynchronized(
	    &astats->pa_shard_stats.decay_muzzy.npurge,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->pa_shard.stats->decay_muzzy.npurge));
	locked_inc_u64_unsynchronized(
	    &astats->pa_shard_stats.decay_muzzy.nmadvise,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->pa_shard.stats->decay_muzzy.nmadvise));
	locked_inc_u64_unsynchronized(
	    &astats->pa_shard_stats.decay_muzzy.purged,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->pa_shard.stats->decay_muzzy.purged));

	astats->base += base_allocated;
	atomic_load_add_store_zu(&astats->internal, arena_internal_get(arena));
	astats->metadata_thp += metadata_thp;

	size_t pa_resident_pgs = 0;
	pa_resident_pgs
	    += atomic_load_zu(&arena->pa_shard.nactive, ATOMIC_RELAXED);
	pa_resident_pgs
	    += ecache_npages_get(&arena->pa_shard.ecache_dirty);
	astats->resident += base_resident + (pa_resident_pgs << LG_PAGE);

	atomic_load_add_store_zu(&astats->pa_shard_stats.abandoned_vm,
	    atomic_load_zu(&arena->stats.pa_shard_stats.abandoned_vm,
	    ATOMIC_RELAXED));

	for (szind_t i = 0; i < SC_NSIZES - SC_NBINS; i++) {
		uint64_t nmalloc = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].nmalloc);
		locked_inc_u64_unsynchronized(&lstats[i].nmalloc, nmalloc);
		astats->nmalloc_large += nmalloc;

		uint64_t ndalloc = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].ndalloc);
		locked_inc_u64_unsynchronized(&lstats[i].ndalloc, ndalloc);
		astats->ndalloc_large += ndalloc;

		uint64_t nrequests = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].nrequests);
		locked_inc_u64_unsynchronized(&lstats[i].nrequests,
		    nmalloc + nrequests);
		astats->nrequests_large += nmalloc + nrequests;

		/* nfill == nmalloc for large currently. */
		locked_inc_u64_unsynchronized(&lstats[i].nfills, nmalloc);
		astats->nfills_large += nmalloc;

		uint64_t nflush = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].nflushes);
		locked_inc_u64_unsynchronized(&lstats[i].nflushes, nflush);
		astats->nflushes_large += nflush;

		assert(nmalloc >= ndalloc);
		assert(nmalloc - ndalloc <= SIZE_T_MAX);
		size_t curlextents = (size_t)(nmalloc - ndalloc);
		lstats[i].curlextents += curlextents;
		astats->allocated_large +=
		    curlextents * sz_index2size(SC_NBINS + i);
	}

	for (pszind_t i = 0; i < SC_NPSIZES; i++) {
		size_t dirty, muzzy, retained, dirty_bytes, muzzy_bytes,
		    retained_bytes;
		dirty = ecache_nextents_get(&arena->pa_shard.ecache_dirty, i);
		muzzy = ecache_nextents_get(&arena->pa_shard.ecache_muzzy, i);
		retained = ecache_nextents_get(&arena->pa_shard.ecache_retained,
		    i);
		dirty_bytes = ecache_nbytes_get(&arena->pa_shard.ecache_dirty,
		    i);
		muzzy_bytes = ecache_nbytes_get(&arena->pa_shard.ecache_muzzy,
		    i);
		retained_bytes = ecache_nbytes_get(
		    &arena->pa_shard.ecache_retained, i);

		estats[i].ndirty = dirty;
		estats[i].nmuzzy = muzzy;
		estats[i].nretained = retained;
		estats[i].dirty_bytes = dirty_bytes;
		estats[i].muzzy_bytes = muzzy_bytes;
		estats[i].retained_bytes = retained_bytes;
	}

	LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);

	/* tcache_bytes counts currently cached bytes. */
	astats->tcache_bytes = 0;
	malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
	cache_bin_array_descriptor_t *descriptor;
	ql_foreach(descriptor, &arena->cache_bin_array_descriptor_ql, link) {
		for (szind_t i = 0; i < SC_NBINS; i++) {
			cache_bin_t *tbin = &descriptor->bins_small[i];
			astats->tcache_bytes +=
			    cache_bin_ncached_get(tbin,
				&tcache_bin_info[i]) * sz_index2size(i);
		}
		for (szind_t i = 0; i < nhbins - SC_NBINS; i++) {
			cache_bin_t *tbin = &descriptor->bins_large[i];
			astats->tcache_bytes +=
			    cache_bin_ncached_get(tbin,
			    &tcache_bin_info[i + SC_NBINS])
			    * sz_index2size(i + SC_NBINS);
		}
	}
	malloc_mutex_prof_read(tsdn,
	    &astats->mutex_prof_data[arena_prof_mutex_tcache_list],
	    &arena->tcache_ql_mtx);
	malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);

#define READ_ARENA_MUTEX_PROF_DATA(mtx, ind)				\
    malloc_mutex_lock(tsdn, &arena->mtx);				\
    malloc_mutex_prof_read(tsdn, &astats->mutex_prof_data[ind],		\
        &arena->mtx);							\
    malloc_mutex_unlock(tsdn, &arena->mtx);

	/* Gather per arena mutex profiling data. */
	READ_ARENA_MUTEX_PROF_DATA(large_mtx, arena_prof_mutex_large);
	READ_ARENA_MUTEX_PROF_DATA(pa_shard.edata_cache.mtx,
	    arena_prof_mutex_extent_avail)
	READ_ARENA_MUTEX_PROF_DATA(pa_shard.ecache_dirty.mtx,
	    arena_prof_mutex_extents_dirty)
	READ_ARENA_MUTEX_PROF_DATA(pa_shard.ecache_muzzy.mtx,
	    arena_prof_mutex_extents_muzzy)
	READ_ARENA_MUTEX_PROF_DATA(pa_shard.ecache_retained.mtx,
	    arena_prof_mutex_extents_retained)
	READ_ARENA_MUTEX_PROF_DATA(pa_shard.decay_dirty.mtx,
	    arena_prof_mutex_decay_dirty)
	READ_ARENA_MUTEX_PROF_DATA(pa_shard.decay_muzzy.mtx,
	    arena_prof_mutex_decay_muzzy)
	READ_ARENA_MUTEX_PROF_DATA(base->mtx,
	    arena_prof_mutex_base)
#undef READ_ARENA_MUTEX_PROF_DATA

	nstime_copy(&astats->uptime, &arena->create_time);
	nstime_update(&astats->uptime);
	nstime_subtract(&astats->uptime, &arena->create_time);

	for (szind_t i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_stats_merge(tsdn, &bstats[i],
			    &arena->bins[i].bin_shards[j]);
		}
	}
}

void arena_handle_new_dirty_pages(tsdn_t *tsdn, arena_t *arena) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (arena_dirty_decay_ms_get(arena) == 0) {
		arena_decay_dirty(tsdn, arena, false, true);
	} else {
		arena_background_thread_inactivity_check(tsdn, arena, false);
	}
}

static void *
arena_slab_reg_alloc(edata_t *slab, const bin_info_t *bin_info) {
	void *ret;
	slab_data_t *slab_data = edata_slab_data_get(slab);
	size_t regind;

	assert(edata_nfree_get(slab) > 0);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(slab_data->bitmap, &bin_info->bitmap_info);
	ret = (void *)((uintptr_t)edata_addr_get(slab) +
	    (uintptr_t)(bin_info->reg_size * regind));
	edata_nfree_dec(slab);
	return ret;
}

static void
arena_slab_reg_alloc_batch(edata_t *slab, const bin_info_t *bin_info,
			   unsigned cnt, void** ptrs) {
	slab_data_t *slab_data = edata_slab_data_get(slab);

	assert(edata_nfree_get(slab) >= cnt);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

#if (! defined JEMALLOC_INTERNAL_POPCOUNTL) || (defined BITMAP_USE_TREE)
	for (unsigned i = 0; i < cnt; i++) {
		size_t regind = bitmap_sfu(slab_data->bitmap,
					   &bin_info->bitmap_info);
		*(ptrs + i) = (void *)((uintptr_t)edata_addr_get(slab) +
		    (uintptr_t)(bin_info->reg_size * regind));
	}
#else
	unsigned group = 0;
	bitmap_t g = slab_data->bitmap[group];
	unsigned i = 0;
	while (i < cnt) {
		while (g == 0) {
			g = slab_data->bitmap[++group];
		}
		size_t shift = group << LG_BITMAP_GROUP_NBITS;
		size_t pop = popcount_lu(g);
		if (pop > (cnt - i)) {
			pop = cnt - i;
		}

		/*
		 * Load from memory locations only once, outside the
		 * hot loop below.
		 */
		uintptr_t base = (uintptr_t)edata_addr_get(slab);
		uintptr_t regsize = (uintptr_t)bin_info->reg_size;
		while (pop--) {
			size_t bit = cfs_lu(&g);
			size_t regind = shift + bit;
			*(ptrs + i) = (void *)(base + regsize * regind);

			i++;
		}
		slab_data->bitmap[group] = g;
	}
#endif
	edata_nfree_sub(slab, cnt);
}

#ifndef JEMALLOC_JET
static
#endif
size_t
arena_slab_regind(edata_t *slab, szind_t binind, const void *ptr) {
	size_t diff, regind;

	/* Freeing a pointer outside the slab can cause assertion failure. */
	assert((uintptr_t)ptr >= (uintptr_t)edata_addr_get(slab));
	assert((uintptr_t)ptr < (uintptr_t)edata_past_get(slab));
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab)) %
	    (uintptr_t)bin_infos[binind].reg_size == 0);

	diff = (size_t)((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab));

	/* Avoid doing division with a variable divisor. */
	regind = div_compute(&arena_binind_div_info[binind], diff);

	assert(regind < bin_infos[binind].nregs);

	return regind;
}

static void
arena_slab_reg_dalloc(edata_t *slab, slab_data_t *slab_data, void *ptr) {
	szind_t binind = edata_szind_get(slab);
	const bin_info_t *bin_info = &bin_infos[binind];
	size_t regind = arena_slab_regind(slab, binind, ptr);

	assert(edata_nfree_get(slab) < bin_info->nregs);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(slab_data->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(slab_data->bitmap, &bin_info->bitmap_info, regind);
	edata_nfree_inc(slab);
}

static void
arena_large_malloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < SC_LARGE_MINCLASS) {
		usize = SC_LARGE_MINCLASS;
	}
	index = sz_size2index(usize);
	hindex = (index >= SC_NBINS) ? index - SC_NBINS : 0;

	locked_inc_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->stats.lstats[hindex].nmalloc, 1);
}

static void
arena_large_dalloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < SC_LARGE_MINCLASS) {
		usize = SC_LARGE_MINCLASS;
	}
	index = sz_size2index(usize);
	hindex = (index >= SC_NBINS) ? index - SC_NBINS : 0;

	locked_inc_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->stats.lstats[hindex].ndalloc, 1);
}

static void
arena_large_ralloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t oldusize,
    size_t usize) {
	arena_large_malloc_stats_update(tsdn, arena, usize);
	arena_large_dalloc_stats_update(tsdn, arena, oldusize);
}

edata_t *
arena_extent_alloc_large(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool *zero) {
	szind_t szind = sz_size2index(usize);
	size_t esize = usize + sz_large_pad;

	edata_t *edata = pa_alloc(tsdn, &arena->pa_shard, esize, alignment,
	    /* slab */ false, szind, zero);

	if (edata != NULL) {
		if (config_stats) {
			LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
			arena_large_malloc_stats_update(tsdn, arena, usize);
			LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
		}
	}

	if (edata != NULL && sz_large_pad != 0) {
		arena_cache_oblivious_randomize(tsdn, arena, edata, alignment);
	}

	return edata;
}

void
arena_extent_dalloc_large_prep(tsdn_t *tsdn, arena_t *arena, edata_t *edata) {
	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
		arena_large_dalloc_stats_update(tsdn, arena,
		    edata_usize_get(edata));
		LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
	}
}

void
arena_extent_ralloc_large_shrink(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    size_t oldusize) {
	size_t usize = edata_usize_get(edata);

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
	}
}

void
arena_extent_ralloc_large_expand(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    size_t oldusize) {
	size_t usize = edata_usize_get(edata);

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
	}
}

ssize_t
arena_dirty_decay_ms_get(arena_t *arena) {
	return pa_shard_dirty_decay_ms_get(&arena->pa_shard);
}

ssize_t
arena_muzzy_decay_ms_get(arena_t *arena) {
	return pa_shard_muzzy_decay_ms_get(&arena->pa_shard);
}

/*
 * In situations where we're not forcing a decay (i.e. because the user
 * specifically requested it), should we purge ourselves, or wait for the
 * background thread to get to it.
 */
static pa_decay_purge_setting_t
arena_decide_unforced_decay_purge_setting(bool is_background_thread) {
	if (is_background_thread) {
		return PA_DECAY_PURGE_ALWAYS;
	} else if (!is_background_thread && background_thread_enabled()) {
		return PA_DECAY_PURGE_NEVER;
	} else {
		return PA_DECAY_PURGE_ON_EPOCH_ADVANCE;
	}
}

static bool
arena_decay_ms_set(tsdn_t *tsdn, arena_t *arena, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache, ssize_t decay_ms) {
	if (!decay_ms_valid(decay_ms)) {
		return true;
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	/*
	 * Restart decay backlog from scratch, which may cause many dirty pages
	 * to be immediately purged.  It would conceptually be possible to map
	 * the old backlog onto the new backlog, but there is no justification
	 * for such complexity since decay_ms changes are intended to be
	 * infrequent, either between the {-1, 0, >0} states, or a one-time
	 * arbitrary change during initial arena configuration.
	 */
	nstime_t cur_time;
	nstime_init_update(&cur_time);
	decay_reinit(decay, &cur_time, decay_ms);
	pa_decay_purge_setting_t decay_purge =
	    arena_decide_unforced_decay_purge_setting(
		/* is_background_thread */ false);
	pa_maybe_decay_purge(tsdn, &arena->pa_shard, decay, decay_stats, ecache,
	    decay_purge);
	malloc_mutex_unlock(tsdn, &decay->mtx);

	return false;
}

bool
arena_dirty_decay_ms_set(tsdn_t *tsdn, arena_t *arena,
    ssize_t decay_ms) {
	return arena_decay_ms_set(tsdn, arena, &arena->pa_shard.decay_dirty,
	    &arena->pa_shard.stats->decay_dirty, &arena->pa_shard.ecache_dirty,
	    decay_ms);
}

bool
arena_muzzy_decay_ms_set(tsdn_t *tsdn, arena_t *arena,
    ssize_t decay_ms) {
	return arena_decay_ms_set(tsdn, arena, &arena->pa_shard.decay_muzzy,
	    &arena->pa_shard.stats->decay_muzzy, &arena->pa_shard.ecache_muzzy,
	    decay_ms);
}

static bool
arena_decay_impl(tsdn_t *tsdn, arena_t *arena, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache,
    bool is_background_thread, bool all) {
	if (all) {
		malloc_mutex_lock(tsdn, &decay->mtx);
		pa_decay_all(tsdn, &arena->pa_shard, decay, decay_stats, ecache,
		    /* fully_decay */ all);
		malloc_mutex_unlock(tsdn, &decay->mtx);
		/*
		 * The previous pa_decay_all call may not have actually decayed
		 * all pages, if new pages were added concurrently with the
		 * purge.
		 *
		 * I don't think we need an activity check for that case (some
		 * other thread must be deallocating, and they should do one),
		 * but we do one anyways.  This line comes out of a refactoring
		 * diff in which the check was pulled out of the callee, and so
		 * an extra redundant check minimizes the change.  We should
		 * reevaluate.
		 */
		assert(!is_background_thread);
		arena_background_thread_inactivity_check(tsdn, arena,
		    /* is_background_thread */ false);
		return false;
	}

	if (malloc_mutex_trylock(tsdn, &decay->mtx)) {
		/* No need to wait if another thread is in progress. */
		return true;
	}
	pa_decay_purge_setting_t decay_purge =
	    arena_decide_unforced_decay_purge_setting(is_background_thread);
	bool epoch_advanced = pa_maybe_decay_purge(tsdn, &arena->pa_shard,
	    decay, decay_stats, ecache, decay_purge);
	size_t npages_new;
	if (epoch_advanced) {
		/* Backlog is updated on epoch advance. */
		npages_new = decay_epoch_npages_delta(decay);
	}
	malloc_mutex_unlock(tsdn, &decay->mtx);

	if (have_background_thread && background_thread_enabled() &&
	    epoch_advanced && !is_background_thread) {
		background_thread_interval_check(tsdn, arena, decay,
		    npages_new);
	}

	return false;
}

static bool
arena_decay_dirty(tsdn_t *tsdn, arena_t *arena, bool is_background_thread,
    bool all) {
	return arena_decay_impl(tsdn, arena, &arena->pa_shard.decay_dirty,
	    &arena->pa_shard.stats->decay_dirty, &arena->pa_shard.ecache_dirty,
	    is_background_thread, all);
}

static bool
arena_decay_muzzy(tsdn_t *tsdn, arena_t *arena, bool is_background_thread,
    bool all) {
	if (ecache_npages_get(&arena->pa_shard.ecache_muzzy) == 0 &&
	    arena_muzzy_decay_ms_get(arena) <= 0) {
		return false;
	}
	return arena_decay_impl(tsdn, arena, &arena->pa_shard.decay_muzzy,
	    &arena->pa_shard.stats->decay_muzzy, &arena->pa_shard.ecache_muzzy,
	    is_background_thread, all);
}

void
arena_decay(tsdn_t *tsdn, arena_t *arena, bool is_background_thread, bool all) {
	if (arena_decay_dirty(tsdn, arena, is_background_thread, all)) {
		return;
	}
	arena_decay_muzzy(tsdn, arena, is_background_thread, all);
}

void
arena_slab_dalloc(tsdn_t *tsdn, arena_t *arena, edata_t *slab) {
	bool generated_dirty;
	pa_dalloc(tsdn, &arena->pa_shard, slab, &generated_dirty);
	if (generated_dirty) {
		arena_handle_new_dirty_pages(tsdn, arena);
	}
}

static void
arena_bin_slabs_nonfull_insert(bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) > 0);
	edata_heap_insert(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs++;
	}
}

static void
arena_bin_slabs_nonfull_remove(bin_t *bin, edata_t *slab) {
	edata_heap_remove(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs--;
	}
}

static edata_t *
arena_bin_slabs_nonfull_tryget(bin_t *bin) {
	edata_t *slab = edata_heap_remove_first(&bin->slabs_nonfull);
	if (slab == NULL) {
		return NULL;
	}
	if (config_stats) {
		bin->stats.reslabs++;
		bin->stats.nonfull_slabs--;
	}
	return slab;
}

static void
arena_bin_slabs_full_insert(arena_t *arena, bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) == 0);
	/*
	 *  Tracking extents is required by arena_reset, which is not allowed
	 *  for auto arenas.  Bypass this step to avoid touching the edata
	 *  linkage (often results in cache misses) for auto arenas.
	 */
	if (arena_is_auto(arena)) {
		return;
	}
	edata_list_append(&bin->slabs_full, slab);
}

static void
arena_bin_slabs_full_remove(arena_t *arena, bin_t *bin, edata_t *slab) {
	if (arena_is_auto(arena)) {
		return;
	}
	edata_list_remove(&bin->slabs_full, slab);
}

static void
arena_bin_reset(tsd_t *tsd, arena_t *arena, bin_t *bin) {
	edata_t *slab;

	malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	if (bin->slabcur != NULL) {
		slab = bin->slabcur;
		bin->slabcur = NULL;
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	while ((slab = edata_heap_remove_first(&bin->slabs_nonfull)) != NULL) {
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	for (slab = edata_list_first(&bin->slabs_full); slab != NULL;
	     slab = edata_list_first(&bin->slabs_full)) {
		arena_bin_slabs_full_remove(arena, bin, slab);
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	if (config_stats) {
		bin->stats.curregs = 0;
		bin->stats.curslabs = 0;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
}

void
arena_reset(tsd_t *tsd, arena_t *arena) {
	/*
	 * Locking in this function is unintuitive.  The caller guarantees that
	 * no concurrent operations are happening in this arena, but there are
	 * still reasons that some locking is necessary:
	 *
	 * - Some of the functions in the transitive closure of calls assume
	 *   appropriate locks are held, and in some cases these locks are
	 *   temporarily dropped to avoid lock order reversal or deadlock due to
	 *   reentry.
	 * - mallctl("epoch", ...) may concurrently refresh stats.  While
	 *   strictly speaking this is a "concurrent operation", disallowing
	 *   stats refreshes would impose an inconvenient burden.
	 */

	/* Large allocations. */
	malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);

	for (edata_t *edata = edata_list_first(&arena->large); edata !=
	    NULL; edata = edata_list_first(&arena->large)) {
		void *ptr = edata_base_get(edata);
		size_t usize;

		malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);
		emap_alloc_ctx_t alloc_ctx;
		emap_alloc_ctx_lookup(tsd_tsdn(tsd), &emap_global, ptr,
		    &alloc_ctx);
		assert(alloc_ctx.szind != SC_NSIZES);

		if (config_stats || (config_prof && opt_prof)) {
			usize = sz_index2size(alloc_ctx.szind);
			assert(usize == isalloc(tsd_tsdn(tsd), ptr));
		}
		/* Remove large allocation from prof sample set. */
		if (config_prof && opt_prof) {
			prof_free(tsd, ptr, usize, &alloc_ctx);
		}
		large_dalloc(tsd_tsdn(tsd), edata);
		malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);

	/* Bins. */
	for (unsigned i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			arena_bin_reset(tsd, arena,
			    &arena->bins[i].bin_shards[j]);
		}
	}

	atomic_store_zu(&arena->pa_shard.nactive, 0, ATOMIC_RELAXED);
}

static void
arena_destroy_retained(tsdn_t *tsdn, arena_t *arena) {
	/*
	 * Iterate over the retained extents and destroy them.  This gives the
	 * extent allocator underlying the extent hooks an opportunity to unmap
	 * all retained memory without having to keep its own metadata
	 * structures.  In practice, virtual memory for dss-allocated extents is
	 * leaked here, so best practice is to avoid dss for arenas to be
	 * destroyed, or provide custom extent hooks that track retained
	 * dss-based extents for later reuse.
	 */
	ehooks_t *ehooks = arena_get_ehooks(arena);
	edata_t *edata;
	while ((edata = ecache_evict(tsdn, &arena->pa_shard, ehooks,
	    &arena->pa_shard.ecache_retained, 0)) != NULL) {
		extent_destroy_wrapper(tsdn, &arena->pa_shard, ehooks, edata);
	}
}

void
arena_destroy(tsd_t *tsd, arena_t *arena) {
	assert(base_ind_get(arena->base) >= narenas_auto);
	assert(arena_nthreads_get(arena, false) == 0);
	assert(arena_nthreads_get(arena, true) == 0);

	/*
	 * No allocations have occurred since arena_reset() was called.
	 * Furthermore, the caller (arena_i_destroy_ctl()) purged all cached
	 * extents, so only retained extents may remain.
	 */
	assert(ecache_npages_get(&arena->pa_shard.ecache_dirty) == 0);
	assert(ecache_npages_get(&arena->pa_shard.ecache_muzzy) == 0);

	/* Deallocate retained memory. */
	arena_destroy_retained(tsd_tsdn(tsd), arena);

	/*
	 * Remove the arena pointer from the arenas array.  We rely on the fact
	 * that there is no way for the application to get a dirty read from the
	 * arenas array unless there is an inherent race in the application
	 * involving access of an arena being concurrently destroyed.  The
	 * application must synchronize knowledge of the arena's validity, so as
	 * long as we use an atomic write to update the arenas array, the
	 * application will get a clean read any time after it synchronizes
	 * knowledge that the arena is no longer valid.
	 */
	arena_set(base_ind_get(arena->base), NULL);

	/*
	 * Destroy the base allocator, which manages all metadata ever mapped by
	 * this arena.
	 */
	base_delete(tsd_tsdn(tsd), arena->base);
}

static edata_t *
arena_slab_alloc(tsdn_t *tsdn, arena_t *arena, szind_t binind, unsigned binshard,
    const bin_info_t *bin_info) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	bool zero = false;

	edata_t *slab = pa_alloc(tsdn, &arena->pa_shard, bin_info->slab_size,
	    PAGE, /* slab */ true, /* szind */ binind, &zero);

	if (slab == NULL) {
		return NULL;
	}
	assert(edata_slab_get(slab));

	/* Initialize slab internals. */
	slab_data_t *slab_data = edata_slab_data_get(slab);
	edata_nfree_binshard_set(slab, bin_info->nregs, binshard);
	bitmap_init(slab_data->bitmap, &bin_info->bitmap_info, false);

	return slab;
}

/*
 * Before attempting the _with_fresh_slab approaches below, the _no_fresh_slab
 * variants (i.e. through slabcur and nonfull) must be tried first.
 */
static void
arena_bin_refill_slabcur_with_fresh_slab(tsdn_t *tsdn, arena_t *arena,
    bin_t *bin, szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after slabcur and nonfull both failed. */
	assert(bin->slabcur == NULL);
	assert(edata_heap_first(&bin->slabs_nonfull) == NULL);
	assert(fresh_slab != NULL);

	/* A new slab from arena_slab_alloc() */
	assert(edata_nfree_get(fresh_slab) == bin_infos[binind].nregs);
	if (config_stats) {
		bin->stats.nslabs++;
		bin->stats.curslabs++;
	}
	bin->slabcur = fresh_slab;
}

/* Refill slabcur and then alloc using the fresh slab */
static void *
arena_bin_malloc_with_fresh_slab(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	arena_bin_refill_slabcur_with_fresh_slab(tsdn, arena, bin, binind,
	    fresh_slab);

	return arena_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

static bool
arena_bin_refill_slabcur_no_fresh_slab(tsdn_t *tsdn, arena_t *arena,
    bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after arena_slab_reg_alloc[_batch] failed. */
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0);

	if (bin->slabcur != NULL) {
		arena_bin_slabs_full_insert(arena, bin, bin->slabcur);
	}

	/* Look for a usable slab. */
	bin->slabcur = arena_bin_slabs_nonfull_tryget(bin);
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) > 0);

	return (bin->slabcur == NULL);
}

/* Choose a bin shard and return the locked bin. */
bin_t *
arena_bin_choose_lock(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    unsigned *binshard) {
	bin_t *bin;
	if (tsdn_null(tsdn) || tsd_arena_get(tsdn_tsd(tsdn)) == NULL) {
		*binshard = 0;
	} else {
		*binshard = tsd_binshardsp_get(tsdn_tsd(tsdn))->binshard[binind];
	}
	assert(*binshard < bin_infos[binind].n_shards);
	bin = &arena->bins[binind].bin_shards[*binshard];
	malloc_mutex_lock(tsdn, &bin->lock);

	return bin;
}

void
arena_tcache_fill_small(tsdn_t *tsdn, arena_t *arena, tcache_t *tcache,
    cache_bin_t *tbin, szind_t binind) {
	assert(cache_bin_ncached_get(tbin, &tcache_bin_info[binind]) == 0);
	tcache->bin_refilled[binind] = true;

	const bin_info_t *bin_info = &bin_infos[binind];
	const unsigned nfill = cache_bin_info_ncached_max(
	    &tcache_bin_info[binind]) >> tcache->lg_fill_div[binind];

	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nfill);
	cache_bin_init_ptr_array_for_fill(tbin, &tcache_bin_info[binind], &ptrs,
	    nfill);
	/*
	 * Bin-local resources are used first: 1) bin->slabcur, and 2) nonfull
	 * slabs.  After both are exhausted, new slabs will be allocated through
	 * arena_slab_alloc().
	 *
	 * Bin lock is only taken / released right before / after the while(...)
	 * refill loop, with new slab allocation (which has its own locking)
	 * kept outside of the loop.  This setup facilitates flat combining, at
	 * the cost of the nested loop (through goto label_refill).
	 *
	 * To optimize for cases with contention and limited resources
	 * (e.g. hugepage-backed or non-overcommit arenas), each fill-iteration
	 * gets one chance of slab_alloc, and a retry of bin local resources
	 * after the slab allocation (regardless if slab_alloc failed, because
	 * the bin lock is dropped during the slab allocation).
	 *
	 * In other words, new slab allocation is allowed, as long as there was
	 * progress since the previous slab_alloc.  This is tracked with
	 * made_progress below, initialized to true to jump start the first
	 * iteration.
	 *
	 * In other words (again), the loop will only terminate early (i.e. stop
	 * with filled < nfill) after going through the three steps: a) bin
	 * local exhausted, b) unlock and slab_alloc returns null, c) re-lock
	 * and bin local fails again.
	 */
	bool made_progress = true;
	edata_t *fresh_slab = NULL;
	bool alloc_and_retry = false;
	unsigned filled = 0;

	bin_t *bin;
	unsigned binshard;
label_refill:
	bin = arena_bin_choose_lock(tsdn, arena, binind, &binshard);
	while (filled < nfill) {
		/* Try batch-fill from slabcur first. */
		edata_t *slabcur = bin->slabcur;
		if (slabcur != NULL && edata_nfree_get(slabcur) > 0) {
			unsigned tofill = nfill - filled;
			unsigned nfree = edata_nfree_get(slabcur);
			unsigned cnt = tofill < nfree ? tofill : nfree;

			arena_slab_reg_alloc_batch(slabcur, bin_info, cnt,
			    &ptrs.ptr[filled]);
			made_progress = true;
			filled += cnt;
			continue;
		}
		/* Next try refilling slabcur from nonfull slabs. */
		if (!arena_bin_refill_slabcur_no_fresh_slab(tsdn, arena, bin)) {
			assert(bin->slabcur != NULL);
			continue;
		}

		/* Then see if a new slab was reserved already. */
		if (fresh_slab != NULL) {
			arena_bin_refill_slabcur_with_fresh_slab(tsdn, arena,
			    bin, binind, fresh_slab);
			assert(bin->slabcur != NULL);
			fresh_slab = NULL;
			continue;
		}

		/* Try slab_alloc if made progress (or never did slab_alloc). */
		if (made_progress) {
			assert(bin->slabcur == NULL);
			assert(fresh_slab == NULL);
			alloc_and_retry = true;
			/* Alloc a new slab then come back. */
			break;
		}

		/* OOM. */

		assert(fresh_slab == NULL);
		assert(!alloc_and_retry);
		break;
	} /* while (filled < nfill) loop. */

	if (config_stats && !alloc_and_retry) {
		bin->stats.nmalloc += filled;
#if defined(ANDROID_ENABLE_TCACHE_STATS)
		bin->stats.nrequests += tbin->tstats.nrequests;
#endif
		bin->stats.curregs += filled;
		bin->stats.nfills++;
#if defined(ANDROID_ENABLE_TCACHE_STATS)
		tbin->tstats.nrequests = 0;
#endif
	}
	malloc_mutex_unlock(tsdn, &bin->lock);

	if (alloc_and_retry) {
		assert(fresh_slab == NULL);
		assert(filled < nfill);
		assert(made_progress);

		fresh_slab = arena_slab_alloc(tsdn, arena, binind, binshard,
		    bin_info);
		/* fresh_slab NULL case handled in the for loop. */

		alloc_and_retry = false;
		made_progress = false;
		goto label_refill;
	}
	assert(filled == nfill || (fresh_slab == NULL && !made_progress));

	/* Release if allocated but not used. */
	if (fresh_slab != NULL) {
		assert(edata_nfree_get(fresh_slab) == bin_info->nregs);
		arena_slab_dalloc(tsdn, arena, fresh_slab);
		fresh_slab = NULL;
	}

	cache_bin_finish_fill(tbin, &tcache_bin_info[binind], &ptrs, filled);
	arena_decay_tick(tsdn, arena);
}

/*
 * Without allocating a new slab, try arena_slab_reg_alloc() and re-fill
 * bin->slabcur if necessary.
 */
static void *
arena_bin_malloc_no_fresh_slab(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	if (bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0) {
		if (arena_bin_refill_slabcur_no_fresh_slab(tsdn, arena, bin)) {
			return NULL;
		}
	}

	assert(bin->slabcur != NULL && edata_nfree_get(bin->slabcur) > 0);
	return arena_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

static void *
arena_malloc_small(tsdn_t *tsdn, arena_t *arena, szind_t binind, bool zero) {
	assert(binind < SC_NBINS);
	const bin_info_t *bin_info = &bin_infos[binind];
	size_t usize = sz_index2size(binind);
	unsigned binshard;
	bin_t *bin = arena_bin_choose_lock(tsdn, arena, binind, &binshard);

	edata_t *fresh_slab = NULL;
	void *ret = arena_bin_malloc_no_fresh_slab(tsdn, arena, bin, binind);
	if (ret == NULL) {
		malloc_mutex_unlock(tsdn, &bin->lock);
		/******************************/
		fresh_slab = arena_slab_alloc(tsdn, arena, binind, binshard,
		    bin_info);
		/********************************/
		malloc_mutex_lock(tsdn, &bin->lock);
		/* Retry since the lock was dropped. */
		ret = arena_bin_malloc_no_fresh_slab(tsdn, arena, bin, binind);
		if (ret == NULL) {
			if (fresh_slab == NULL) {
				/* OOM */
				malloc_mutex_unlock(tsdn, &bin->lock);
				return NULL;
			}
			ret = arena_bin_malloc_with_fresh_slab(tsdn, arena, bin,
			    binind, fresh_slab);
			fresh_slab = NULL;
		}
	}
	if (config_stats) {
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
		bin->stats.curregs++;
	}
	malloc_mutex_unlock(tsdn, &bin->lock);

	if (fresh_slab != NULL) {
		arena_slab_dalloc(tsdn, arena, fresh_slab);
	}
	if (zero) {
		memset(ret, 0, usize);
	}
	arena_decay_tick(tsdn, arena);

	return ret;
}

void *
arena_malloc_hard(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind,
    bool zero) {
	assert(!tsdn_null(tsdn) || arena != NULL);

	if (likely(!tsdn_null(tsdn))) {
		arena = arena_choose_maybe_huge(tsdn_tsd(tsdn), arena, size);
	}
	if (unlikely(arena == NULL)) {
		return NULL;
	}

	if (likely(size <= SC_SMALL_MAXCLASS)) {
		return arena_malloc_small(tsdn, arena, ind, zero);
	}
	return large_malloc(tsdn, arena, sz_index2size(ind), zero);
}

void *
arena_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero, tcache_t *tcache) {
	void *ret;

	if (usize <= SC_SMALL_MAXCLASS
	    && (alignment < PAGE
	    || (alignment == PAGE && (usize & PAGE_MASK) == 0))) {
		/* Small; alignment doesn't require special slab placement. */
		ret = arena_malloc(tsdn, arena, usize, sz_size2index(usize),
		    zero, tcache, true);
	} else {
		if (likely(alignment <= CACHELINE)) {
			ret = large_malloc(tsdn, arena, usize, zero);
		} else {
			ret = large_palloc(tsdn, arena, usize, alignment, zero);
		}
	}
	return ret;
}

void
arena_prof_promote(tsdn_t *tsdn, void *ptr, size_t usize) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(isalloc(tsdn, ptr) == SC_LARGE_MINCLASS);
	assert(usize <= SC_SMALL_MAXCLASS);

	if (config_opt_safety_checks) {
		safety_check_set_redzone(ptr, usize, SC_LARGE_MINCLASS);
	}

	edata_t *edata = emap_edata_lookup(tsdn, &emap_global, ptr);

	szind_t szind = sz_size2index(usize);
	emap_remap(tsdn, &emap_global, edata, szind, false);

	prof_idump_rollback(tsdn, usize);

	assert(isalloc(tsdn, ptr) == usize);
}

static size_t
arena_prof_demote(tsdn_t *tsdn, edata_t *edata, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	emap_remap(tsdn, &emap_global, edata, SC_NBINS, false);

	assert(isalloc(tsdn, ptr) == SC_LARGE_MINCLASS);

	return SC_LARGE_MINCLASS;
}

void
arena_dalloc_promoted(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    bool slow_path) {
	cassert(config_prof);
	assert(opt_prof);

	edata_t *edata = emap_edata_lookup(tsdn, &emap_global, ptr);
	size_t usize = edata_usize_get(edata);
	size_t bumped_usize = arena_prof_demote(tsdn, edata, ptr);
	if (config_opt_safety_checks && usize < SC_LARGE_MINCLASS) {
		/*
		 * Currently, we only do redzoning for small sampled
		 * allocations.
		 */
		assert(bumped_usize == SC_LARGE_MINCLASS);
		safety_check_verify_redzone(ptr, usize, bumped_usize);
	}
	if (bumped_usize <= tcache_maxclass && tcache != NULL) {
		tcache_dalloc_large(tsdn_tsd(tsdn), tcache, ptr,
		    sz_size2index(bumped_usize), slow_path);
	} else {
		large_dalloc(tsdn, edata);
	}
}

static void
arena_dissociate_bin_slab(arena_t *arena, edata_t *slab, bin_t *bin) {
	/* Dissociate slab from bin. */
	if (slab == bin->slabcur) {
		bin->slabcur = NULL;
	} else {
		szind_t binind = edata_szind_get(slab);
		const bin_info_t *bin_info = &bin_infos[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * slab only contains one region, then it never gets inserted
		 * into the non-full slabs heap.
		 */
		if (bin_info->nregs == 1) {
			arena_bin_slabs_full_remove(arena, bin, slab);
		} else {
			arena_bin_slabs_nonfull_remove(bin, slab);
		}
	}
}

static void
arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, edata_t *slab,
    bin_t *bin) {
	assert(edata_nfree_get(slab) > 0);

	/*
	 * Make sure that if bin->slabcur is non-NULL, it refers to the
	 * oldest/lowest non-full slab.  It is okay to NULL slabcur out rather
	 * than proactively keeping it pointing at the oldest/lowest non-full
	 * slab.
	 */
	if (bin->slabcur != NULL && edata_snad_comp(bin->slabcur, slab) > 0) {
		/* Switch slabcur. */
		if (edata_nfree_get(bin->slabcur) > 0) {
			arena_bin_slabs_nonfull_insert(bin, bin->slabcur);
		} else {
			arena_bin_slabs_full_insert(arena, bin, bin->slabcur);
		}
		bin->slabcur = slab;
		if (config_stats) {
			bin->stats.reslabs++;
		}
	} else {
		arena_bin_slabs_nonfull_insert(bin, slab);
	}
}

static void
arena_dalloc_bin_slab_prepare(tsdn_t *tsdn, edata_t *slab, bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);

	assert(slab != bin->slabcur);
	if (config_stats) {
		bin->stats.curslabs--;
	}
}

/* Returns true if arena_slab_dalloc must be called on slab */
static bool
arena_dalloc_bin_locked_impl(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind, edata_t *slab, void *ptr) {
	const bin_info_t *bin_info = &bin_infos[binind];
	arena_slab_reg_dalloc(slab, edata_slab_data_get(slab), ptr);

	bool ret = false;
	unsigned nfree = edata_nfree_get(slab);
	if (nfree == bin_info->nregs) {
		arena_dissociate_bin_slab(arena, slab, bin);
		arena_dalloc_bin_slab_prepare(tsdn, slab, bin);
		ret = true;
	} else if (nfree == 1 && slab != bin->slabcur) {
		arena_bin_slabs_full_remove(arena, bin, slab);
		arena_bin_lower_slab(tsdn, arena, slab, bin);
	}

	if (config_stats) {
		bin->stats.ndalloc++;
		bin->stats.curregs--;
	}

	return ret;
}

bool
arena_dalloc_bin_locked(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
szind_t binind, edata_t *edata, void *ptr) {
	return arena_dalloc_bin_locked_impl(tsdn, arena, bin, binind, edata,
	    ptr);
}

static void
arena_dalloc_bin(tsdn_t *tsdn, arena_t *arena, edata_t *edata, void *ptr) {
	szind_t binind = edata_szind_get(edata);
	unsigned binshard = edata_binshard_get(edata);
	bin_t *bin = &arena->bins[binind].bin_shards[binshard];

	malloc_mutex_lock(tsdn, &bin->lock);
	bool ret = arena_dalloc_bin_locked_impl(tsdn, arena, bin, binind, edata,
	    ptr);
	malloc_mutex_unlock(tsdn, &bin->lock);

	if (ret) {
		arena_slab_dalloc(tsdn, arena, edata);
	}
}

void
arena_dalloc_small(tsdn_t *tsdn, void *ptr) {
	edata_t *edata = emap_edata_lookup(tsdn, &emap_global, ptr);
	arena_t *arena = arena_get_from_edata(edata);

	arena_dalloc_bin(tsdn, arena, edata, ptr);
	arena_decay_tick(tsdn, arena);
}

bool
arena_ralloc_no_move(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size,
    size_t extra, bool zero, size_t *newsize) {
	bool ret;
	/* Calls with non-zero extra had to clamp extra. */
	assert(extra == 0 || size + extra <= SC_LARGE_MAXCLASS);

	edata_t *edata = emap_edata_lookup(tsdn, &emap_global, ptr);
	if (unlikely(size > SC_LARGE_MAXCLASS)) {
		ret = true;
		goto done;
	}

	size_t usize_min = sz_s2u(size);
	size_t usize_max = sz_s2u(size + extra);
	if (likely(oldsize <= SC_SMALL_MAXCLASS && usize_min
	    <= SC_SMALL_MAXCLASS)) {
		/*
		 * Avoid moving the allocation if the size class can be left the
		 * same.
		 */
		assert(bin_infos[sz_size2index(oldsize)].reg_size ==
		    oldsize);
		if ((usize_max > SC_SMALL_MAXCLASS
		    || sz_size2index(usize_max) != sz_size2index(oldsize))
		    && (size > oldsize || usize_max < oldsize)) {
			ret = true;
			goto done;
		}

		arena_t *arena = arena_get_from_edata(edata);
		arena_decay_tick(tsdn, arena);
		ret = false;
	} else if (oldsize >= SC_LARGE_MINCLASS
	    && usize_max >= SC_LARGE_MINCLASS) {
		ret = large_ralloc_no_move(tsdn, edata, usize_min, usize_max,
		    zero);
	} else {
		ret = true;
	}
done:
	assert(edata == emap_edata_lookup(tsdn, &emap_global, ptr));
	*newsize = edata_usize_get(edata);

	return ret;
}

static void *
arena_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache) {
	if (alignment == 0) {
		return arena_malloc(tsdn, arena, usize, sz_size2index(usize),
		    zero, tcache, true);
	}
	usize = sz_sa2u(usize, alignment);
	if (unlikely(usize == 0 || usize > SC_LARGE_MAXCLASS)) {
		return NULL;
	}
	return ipalloct(tsdn, usize, alignment, zero, tcache, arena);
}

void *
arena_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t oldsize,
    size_t size, size_t alignment, bool zero, tcache_t *tcache,
    hook_ralloc_args_t *hook_args) {
	size_t usize = sz_s2u(size);
	if (unlikely(usize == 0 || size > SC_LARGE_MAXCLASS)) {
		return NULL;
	}

	if (likely(usize <= SC_SMALL_MAXCLASS)) {
		/* Try to avoid moving the allocation. */
		UNUSED size_t newsize;
		if (!arena_ralloc_no_move(tsdn, ptr, oldsize, usize, 0, zero,
		    &newsize)) {
			hook_invoke_expand(hook_args->is_realloc
			    ? hook_expand_realloc : hook_expand_rallocx,
			    ptr, oldsize, usize, (uintptr_t)ptr,
			    hook_args->args);
			return ptr;
		}
	}

	if (oldsize >= SC_LARGE_MINCLASS
	    && usize >= SC_LARGE_MINCLASS) {
		return large_ralloc(tsdn, arena, ptr, usize,
		    alignment, zero, tcache, hook_args);
	}

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and copying.
	 */
	void *ret = arena_ralloc_move_helper(tsdn, arena, usize, alignment,
	    zero, tcache);
	if (ret == NULL) {
		return NULL;
	}

	hook_invoke_alloc(hook_args->is_realloc
	    ? hook_alloc_realloc : hook_alloc_rallocx, ret, (uintptr_t)ret,
	    hook_args->args);
	hook_invoke_dalloc(hook_args->is_realloc
	    ? hook_dalloc_realloc : hook_dalloc_rallocx, ptr, hook_args->args);

	/*
	 * Junk/zero-filling were already done by
	 * ipalloc()/arena_malloc().
	 */
	size_t copysize = (usize < oldsize) ? usize : oldsize;
	memcpy(ret, ptr, copysize);
	isdalloct(tsdn, ptr, oldsize, tcache, NULL, true);
	return ret;
}

ehooks_t *
arena_get_ehooks(arena_t *arena) {
	return base_ehooks_get(arena->base);
}

extent_hooks_t *
arena_set_extent_hooks(tsd_t *tsd, arena_t *arena,
    extent_hooks_t *extent_hooks) {
	background_thread_info_t *info;
	if (have_background_thread) {
		info = arena_background_thread_info_get(arena);
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	}
	extent_hooks_t *ret = base_extent_hooks_set(arena->base, extent_hooks);
	if (have_background_thread) {
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
	}

	return ret;
}


dss_prec_t
arena_dss_prec_get(arena_t *arena) {
	return (dss_prec_t)atomic_load_u(&arena->dss_prec, ATOMIC_ACQUIRE);
}

bool
arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec) {
	if (!have_dss) {
		return (dss_prec != dss_prec_disabled);
	}
	atomic_store_u(&arena->dss_prec, (unsigned)dss_prec, ATOMIC_RELEASE);
	return false;
}

ssize_t
arena_dirty_decay_ms_default_get(void) {
	return atomic_load_zd(&dirty_decay_ms_default, ATOMIC_RELAXED);
}

bool
arena_dirty_decay_ms_default_set(ssize_t decay_ms) {
	if (!decay_ms_valid(decay_ms)) {
		return true;
	}
	atomic_store_zd(&dirty_decay_ms_default, decay_ms, ATOMIC_RELAXED);
	return false;
}

ssize_t
arena_muzzy_decay_ms_default_get(void) {
	return atomic_load_zd(&muzzy_decay_ms_default, ATOMIC_RELAXED);
}

bool
arena_muzzy_decay_ms_default_set(ssize_t decay_ms) {
	if (!decay_ms_valid(decay_ms)) {
		return true;
	}
	atomic_store_zd(&muzzy_decay_ms_default, decay_ms, ATOMIC_RELAXED);
	return false;
}

bool
arena_retain_grow_limit_get_set(tsd_t *tsd, arena_t *arena, size_t *old_limit,
    size_t *new_limit) {
	assert(opt_retain);

	pszind_t new_ind JEMALLOC_CC_SILENCE_INIT(0);
	if (new_limit != NULL) {
		size_t limit = *new_limit;
		/* Grow no more than the new limit. */
		if ((new_ind = sz_psz2ind(limit + 1) - 1) >= SC_NPSIZES) {
			return true;
		}
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &arena->pa_shard.ecache_grow.mtx);
	if (old_limit != NULL) {
		*old_limit = sz_pind2sz(arena->pa_shard.ecache_grow.limit);
	}
	if (new_limit != NULL) {
		arena->pa_shard.ecache_grow.limit = new_ind;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->pa_shard.ecache_grow.mtx);

	return false;
}

unsigned
arena_nthreads_get(arena_t *arena, bool internal) {
	return atomic_load_u(&arena->nthreads[internal], ATOMIC_RELAXED);
}

void
arena_nthreads_inc(arena_t *arena, bool internal) {
	atomic_fetch_add_u(&arena->nthreads[internal], 1, ATOMIC_RELAXED);
}

void
arena_nthreads_dec(arena_t *arena, bool internal) {
	atomic_fetch_sub_u(&arena->nthreads[internal], 1, ATOMIC_RELAXED);
}

arena_t *
arena_new(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks) {
	arena_t *arena;
	base_t *base;
	unsigned i;

	if (ind == 0) {
		base = b0get();
	} else {
		base = base_new(tsdn, ind, extent_hooks);
		if (base == NULL) {
			return NULL;
		}
	}

	unsigned nbins_total = 0;
	for (i = 0; i < SC_NBINS; i++) {
		nbins_total += bin_infos[i].n_shards;
	}
	size_t arena_size = sizeof(arena_t) + sizeof(bin_t) * nbins_total;
	arena = (arena_t *)base_alloc(tsdn, base, arena_size, CACHELINE);
	if (arena == NULL) {
		goto label_error;
	}

	atomic_store_u(&arena->nthreads[0], 0, ATOMIC_RELAXED);
	atomic_store_u(&arena->nthreads[1], 0, ATOMIC_RELAXED);
	arena->last_thd = NULL;

	if (config_stats) {
		if (arena_stats_init(tsdn, &arena->stats)) {
			goto label_error;
		}

		ql_new(&arena->tcache_ql);
		ql_new(&arena->cache_bin_array_descriptor_ql);
		if (malloc_mutex_init(&arena->tcache_ql_mtx, "tcache_ql",
		    WITNESS_RANK_TCACHE_QL, malloc_mutex_rank_exclusive)) {
			goto label_error;
		}
	}

	if (config_prof) {
		if (prof_accum_init()) {
			goto label_error;
		}
	}

	atomic_store_u(&arena->dss_prec, (unsigned)extent_dss_prec_get(),
	    ATOMIC_RELAXED);

	edata_list_init(&arena->large);
	if (malloc_mutex_init(&arena->large_mtx, "arena_large",
	    WITNESS_RANK_ARENA_LARGE, malloc_mutex_rank_exclusive)) {
		goto label_error;
	}

	if (pa_shard_init(tsdn, &arena->pa_shard, base, ind,
	    &arena->stats.pa_shard_stats, LOCKEDINT_MTX(arena->stats.mtx))) {
		goto label_error;
	}

	nstime_t cur_time;
	nstime_init_update(&cur_time);

	if (decay_init(&arena->pa_shard.decay_dirty, &cur_time,
	    arena_dirty_decay_ms_default_get())) {
		goto label_error;
	}
	if (decay_init(&arena->pa_shard.decay_muzzy, &cur_time,
	    arena_muzzy_decay_ms_default_get())) {
		goto label_error;
	}

	/* Initialize bins. */
	uintptr_t bin_addr = (uintptr_t)arena + sizeof(arena_t);
	atomic_store_u(&arena->binshard_next, 0, ATOMIC_RELEASE);
	for (i = 0; i < SC_NBINS; i++) {
		unsigned nshards = bin_infos[i].n_shards;
		arena->bins[i].bin_shards = (bin_t *)bin_addr;
		bin_addr += nshards * sizeof(bin_t);
		for (unsigned j = 0; j < nshards; j++) {
			bool err = bin_init(&arena->bins[i].bin_shards[j]);
			if (err) {
				goto label_error;
			}
		}
	}
	assert(bin_addr == (uintptr_t)arena + arena_size);

	arena->base = base;
	/* Set arena before creating background threads. */
	arena_set(ind, arena);

	nstime_init_update(&arena->create_time);

	/* We don't support reentrancy for arena 0 bootstrapping. */
	if (ind != 0) {
		/*
		 * If we're here, then arena 0 already exists, so bootstrapping
		 * is done enough that we should have tsd.
		 */
		assert(!tsdn_null(tsdn));
		pre_reentrancy(tsdn_tsd(tsdn), arena);
		if (test_hooks_arena_new_hook) {
			test_hooks_arena_new_hook();
		}
		post_reentrancy(tsdn_tsd(tsdn));
	}

	return arena;
label_error:
	if (ind != 0) {
		base_delete(tsdn, base);
	}
	return NULL;
}

arena_t *
arena_choose_huge(tsd_t *tsd) {
	/* huge_arena_ind can be 0 during init (will use a0). */
	if (huge_arena_ind == 0) {
		assert(!malloc_initialized());
	}

	arena_t *huge_arena = arena_get(tsd_tsdn(tsd), huge_arena_ind, false);
	if (huge_arena == NULL) {
		/* Create the huge arena on demand. */
		assert(huge_arena_ind != 0);
		huge_arena = arena_get(tsd_tsdn(tsd), huge_arena_ind, true);
		if (huge_arena == NULL) {
			return NULL;
		}
		/*
		 * Purge eagerly for huge allocations, because: 1) number of
		 * huge allocations is usually small, which means ticker based
		 * decay is not reliable; and 2) less immediate reuse is
		 * expected for huge allocations.
		 */
		if (arena_dirty_decay_ms_default_get() > 0) {
			arena_dirty_decay_ms_set(tsd_tsdn(tsd), huge_arena, 0);
		}
		if (arena_muzzy_decay_ms_default_get() > 0) {
			arena_muzzy_decay_ms_set(tsd_tsdn(tsd), huge_arena, 0);
		}
	}

	return huge_arena;
}

bool
arena_init_huge(void) {
	bool huge_enabled;

	/* The threshold should be large size class. */
	if (opt_oversize_threshold > SC_LARGE_MAXCLASS ||
	    opt_oversize_threshold < SC_LARGE_MINCLASS) {
		opt_oversize_threshold = 0;
		oversize_threshold = SC_LARGE_MAXCLASS + PAGE;
		huge_enabled = false;
	} else {
		/* Reserve the index for the huge arena. */
		huge_arena_ind = narenas_total_get();
		oversize_threshold = opt_oversize_threshold;
		huge_enabled = true;
	}

	return huge_enabled;
}

bool
arena_is_huge(unsigned arena_ind) {
	if (huge_arena_ind == 0) {
		return false;
	}
	return (arena_ind == huge_arena_ind);
}

void
arena_boot(sc_data_t *sc_data) {
	arena_dirty_decay_ms_default_set(opt_dirty_decay_ms);
	arena_muzzy_decay_ms_default_set(opt_muzzy_decay_ms);
	for (unsigned i = 0; i < SC_NBINS; i++) {
		sc_t *sc = &sc_data->sc[i];
		div_init(&arena_binind_div_info[i],
		    (1U << sc->lg_base) + (sc->ndelta << sc->lg_delta));
	}
}

void
arena_prefork0(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork0(tsdn, &arena->pa_shard);
}

void
arena_prefork1(tsdn_t *tsdn, arena_t *arena) {
	if (config_stats) {
		malloc_mutex_prefork(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_prefork2(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork2(tsdn, &arena->pa_shard);
}

void
arena_prefork3(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork3(tsdn, &arena->pa_shard);
}

void
arena_prefork4(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork4(tsdn, &arena->pa_shard);
}

void
arena_prefork5(tsdn_t *tsdn, arena_t *arena) {
	base_prefork(tsdn, arena->base);
}

void
arena_prefork6(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->large_mtx);
}

void
arena_prefork7(tsdn_t *tsdn, arena_t *arena) {
	for (unsigned i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_prefork(tsdn, &arena->bins[i].bin_shards[j]);
		}
	}
}

void
arena_postfork_parent(tsdn_t *tsdn, arena_t *arena) {
	unsigned i;

	for (i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_postfork_parent(tsdn,
			    &arena->bins[i].bin_shards[j]);
		}
	}
	malloc_mutex_postfork_parent(tsdn, &arena->large_mtx);
	base_postfork_parent(tsdn, arena->base);
	pa_shard_postfork_parent(tsdn, &arena->pa_shard);
	if (config_stats) {
		malloc_mutex_postfork_parent(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_postfork_child(tsdn_t *tsdn, arena_t *arena) {
	unsigned i;

	atomic_store_u(&arena->nthreads[0], 0, ATOMIC_RELAXED);
	atomic_store_u(&arena->nthreads[1], 0, ATOMIC_RELAXED);
	if (tsd_arena_get(tsdn_tsd(tsdn)) == arena) {
		arena_nthreads_inc(arena, false);
	}
	if (tsd_iarena_get(tsdn_tsd(tsdn)) == arena) {
		arena_nthreads_inc(arena, true);
	}
	if (config_stats) {
		ql_new(&arena->tcache_ql);
		ql_new(&arena->cache_bin_array_descriptor_ql);
		tcache_t *tcache = tcache_get(tsdn_tsd(tsdn));
		if (tcache != NULL && tcache->arena == arena) {
			ql_elm_new(tcache, link);
			ql_tail_insert(&arena->tcache_ql, tcache, link);
			cache_bin_array_descriptor_init(
			    &tcache->cache_bin_array_descriptor,
			    tcache->bins_small, tcache->bins_large);
			ql_tail_insert(&arena->cache_bin_array_descriptor_ql,
			    &tcache->cache_bin_array_descriptor, link);
		}
	}

	for (i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_postfork_child(tsdn, &arena->bins[i].bin_shards[j]);
		}
	}
	malloc_mutex_postfork_child(tsdn, &arena->large_mtx);
	base_postfork_child(tsdn, arena->base);
	pa_shard_postfork_child(tsdn, &arena->pa_shard);
	if (config_stats) {
		malloc_mutex_postfork_child(tsdn, &arena->tcache_ql_mtx);
	}
}
