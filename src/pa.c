#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

static void
pa_nactive_add(pa_shard_t *shard, size_t add_pages) {
	atomic_fetch_add_zu(&shard->nactive, add_pages, ATOMIC_RELAXED);
}

static void
pa_nactive_sub(pa_shard_t *shard, size_t sub_pages) {
	assert(atomic_load_zu(&shard->nactive, ATOMIC_RELAXED) >= sub_pages);
	atomic_fetch_sub_zu(&shard->nactive, sub_pages, ATOMIC_RELAXED);
}

bool
pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, emap_t *emap, base_t *base,
    unsigned ind, pa_shard_stats_t *stats, malloc_mutex_t *stats_mtx,
    nstime_t *cur_time, ssize_t dirty_decay_ms, ssize_t muzzy_decay_ms) {
	/* This will change eventually, but for now it should hold. */
	assert(base_ind_get(base) == ind);
	/*
	 * Delay coalescing for dirty extents despite the disruptive effect on
	 * memory layout for best-fit extent allocation, since cached extents
	 * are likely to be reused soon after deallocation, and the cost of
	 * merging/splitting extents is non-trivial.
	 */
	if (ecache_init(tsdn, &shard->ecache_dirty, extent_state_dirty, ind,
	    /* delay_coalesce */ true)) {
		return true;
	}
	/*
	 * Coalesce muzzy extents immediately, because operations on them are in
	 * the critical path much less often than for dirty extents.
	 */
	if (ecache_init(tsdn, &shard->ecache_muzzy, extent_state_muzzy, ind,
	    /* delay_coalesce */ false)) {
		return true;
	}
	/*
	 * Coalesce retained extents immediately, in part because they will
	 * never be evicted (and therefore there's no opportunity for delayed
	 * coalescing), but also because operations on retained extents are not
	 * in the critical path.
	 */
	if (ecache_init(tsdn, &shard->ecache_retained, extent_state_retained,
	    ind, /* delay_coalesce */ false)) {
		return true;
	}
	if (edata_cache_init(&shard->edata_cache, base)) {
		return true;
	}

	if (ecache_grow_init(tsdn, &shard->ecache_grow)) {
		return true;
	}

	if (decay_init(&shard->decay_dirty, cur_time, dirty_decay_ms)) {
		return true;
	}
	if (decay_init(&shard->decay_muzzy, cur_time, muzzy_decay_ms)) {
		return true;
	}

	atomic_store_zu(&shard->extent_sn_next, 0, ATOMIC_RELAXED);
	atomic_store_zu(&shard->nactive, 0, ATOMIC_RELAXED);

	shard->stats_mtx = stats_mtx;
	shard->stats = stats;
	memset(shard->stats, 0, sizeof(*shard->stats));

	shard->emap = emap;
	shard->base = base;

	return false;
}

void
pa_shard_reset(pa_shard_t *shard) {
	atomic_store_zu(&shard->nactive, 0, ATOMIC_RELAXED);
}

void
pa_shard_destroy_retained(tsdn_t *tsdn, pa_shard_t *shard) {
	assert(ecache_npages_get(&shard->ecache_dirty) == 0);
	assert(ecache_npages_get(&shard->ecache_muzzy) == 0);
	/*
	 * Iterate over the retained extents and destroy them.  This gives the
	 * extent allocator underlying the extent hooks an opportunity to unmap
	 * all retained memory without having to keep its own metadata
	 * structures.  In practice, virtual memory for dss-allocated extents is
	 * leaked here, so best practice is to avoid dss for arenas to be
	 * destroyed, or provide custom extent hooks that track retained
	 * dss-based extents for later reuse.
	 */
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	edata_t *edata;
	while ((edata = ecache_evict(tsdn, shard, ehooks,
	    &shard->ecache_retained, 0)) != NULL) {
		extent_destroy_wrapper(tsdn, shard, ehooks, edata);
	}
}

size_t
pa_shard_extent_sn_next(pa_shard_t *shard) {
	return atomic_fetch_add_zu(&shard->extent_sn_next, 1, ATOMIC_RELAXED);
}

static bool
pa_shard_may_have_muzzy(pa_shard_t *shard) {
	return pa_shard_muzzy_decay_ms_get(shard) != 0;
}

edata_t *
pa_alloc(tsdn_t *tsdn, pa_shard_t *shard, size_t size, size_t alignment,
    bool slab, szind_t szind, bool zero) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	size_t mapped_add = 0;

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	edata_t *edata = ecache_alloc(tsdn, shard, ehooks,
	    &shard->ecache_dirty, NULL, size, alignment, zero);

	if (edata == NULL && pa_shard_may_have_muzzy(shard)) {
		edata = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_muzzy,
		    NULL, size, alignment, zero);
	}
	if (edata == NULL) {
		edata = ecache_alloc_grow(tsdn, shard, ehooks,
		    &shard->ecache_retained, NULL, size, alignment, zero);
		mapped_add = size;
	}
	if (edata != NULL) {
		pa_nactive_add(shard, size >> LG_PAGE);
		if (config_stats && mapped_add > 0) {
			atomic_fetch_add_zu(&shard->stats->pa_mapped,
			    mapped_add, ATOMIC_RELAXED);
		}
		emap_remap(tsdn, shard->emap, edata, szind, slab);
		edata_szind_set(edata, szind);
		edata_slab_set(edata, slab);
		if (slab) {
			emap_register_interior(tsdn, shard->emap, edata, szind);
		}
	}
	return edata;
}

bool
pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool zero) {
	assert(new_size > old_size);
	assert(edata_size_get(edata) == old_size);
	assert((new_size & PAGE_MASK) == 0);

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	void *trail_begin = edata_past_get(edata);
	size_t expand_amount = new_size - old_size;

	size_t mapped_add = 0;

	if (ehooks_merge_will_fail(ehooks)) {
		return true;
	}
	edata_t *trail = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_dirty,
	    trail_begin, expand_amount, PAGE, zero);
	if (trail == NULL) {
		trail = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_muzzy,
		    trail_begin, expand_amount, PAGE, zero);
	}
	if (trail == NULL) {
		trail = ecache_alloc_grow(tsdn, shard, ehooks,
		    &shard->ecache_retained, trail_begin, expand_amount, PAGE,
		    zero);
		mapped_add = expand_amount;
	}
	if (trail == NULL) {
		return true;
	}
	if (extent_merge_wrapper(tsdn, shard, ehooks, edata, trail)) {
		extent_dalloc_wrapper(tsdn, shard, ehooks, trail);
		return true;
	}
	if (config_stats && mapped_add > 0) {
		atomic_fetch_add_zu(&shard->stats->pa_mapped, mapped_add,
		    ATOMIC_RELAXED);
	}
	pa_nactive_add(shard, expand_amount >> LG_PAGE);
	edata_szind_set(edata, szind);
	emap_remap(tsdn, shard->emap, edata, szind, /* slab */ false);
	return false;
}

bool
pa_shrink(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool *generated_dirty) {
	assert(new_size < old_size);
	assert(edata_size_get(edata) == old_size);
	assert((new_size & PAGE_MASK) == 0);
	size_t shrink_amount = old_size - new_size;

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	*generated_dirty = false;

	if (ehooks_split_will_fail(ehooks)) {
		return true;
	}

	edata_t *trail = extent_split_wrapper(tsdn, shard, ehooks, edata,
	    new_size, shrink_amount);
	if (trail == NULL) {
		return true;
	}
	pa_nactive_sub(shard, shrink_amount >> LG_PAGE);

	ecache_dalloc(tsdn, shard, ehooks, &shard->ecache_dirty, trail);
	*generated_dirty = true;

	edata_szind_set(edata, szind);
	emap_remap(tsdn, shard->emap, edata, szind, /* slab */ false);
	return false;
}

void
pa_dalloc(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata,
    bool *generated_dirty) {
	emap_remap(tsdn, shard->emap, edata, SC_NSIZES, /* slab */ false);
	if (edata_slab_get(edata)) {
		emap_deregister_interior(tsdn, shard->emap, edata);
		edata_slab_set(edata, false);
	}
	edata_szind_set(edata, SC_NSIZES);
	pa_nactive_sub(shard, edata_size_get(edata) >> LG_PAGE);
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	ecache_dalloc(tsdn, shard, ehooks, &shard->ecache_dirty, edata);
	*generated_dirty = true;
}

static size_t
pa_stash_decayed(tsdn_t *tsdn, pa_shard_t *shard, ecache_t *ecache,
    size_t npages_limit, size_t npages_decay_max, edata_list_t *result) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);

	/* Stash extents according to npages_limit. */
	size_t nstashed = 0;
	while (nstashed < npages_decay_max) {
		edata_t *edata = ecache_evict(tsdn, shard, ehooks, ecache,
		    npages_limit);
		if (edata == NULL) {
			break;
		}
		edata_list_append(result, edata);
		nstashed += edata_size_get(edata) >> LG_PAGE;
	}
	return nstashed;
}

static size_t
pa_decay_stashed(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    edata_list_t *decay_extents) {
	bool err;

	size_t nmadvise = 0;
	size_t nunmapped = 0;
	size_t npurged = 0;

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);

	bool try_muzzy = !fully_decay && pa_shard_may_have_muzzy(shard);

	for (edata_t *edata = edata_list_first(decay_extents); edata !=
	    NULL; edata = edata_list_first(decay_extents)) {
		edata_list_remove(decay_extents, edata);

		size_t size = edata_size_get(edata);
		size_t npages = size >> LG_PAGE;

		nmadvise++;
		npurged += npages;

		switch (ecache->state) {
		case extent_state_active:
			not_reached();
		case extent_state_dirty:
			if (try_muzzy) {
				err = extent_purge_lazy_wrapper(tsdn, ehooks,
				    edata, /* offset */ 0, size);
				if (!err) {
					ecache_dalloc(tsdn, shard, ehooks,
					    &shard->ecache_muzzy, edata);
					break;
				}
			}
			JEMALLOC_FALLTHROUGH;
		case extent_state_muzzy:
			extent_dalloc_wrapper(tsdn, shard, ehooks, edata);
			nunmapped += npages;
			break;
		case extent_state_retained:
		default:
			not_reached();
		}
	}

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, *shard->stats_mtx);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
		    &decay_stats->npurge, 1);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
		    &decay_stats->nmadvise, nmadvise);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
		    &decay_stats->purged, npurged);
		LOCKEDINT_MTX_UNLOCK(tsdn, *shard->stats_mtx);
		atomic_fetch_sub_zu(&shard->stats->pa_mapped,
		    nunmapped << LG_PAGE, ATOMIC_RELAXED);
	}

	return npurged;
}

/*
 * npages_limit: Decay at most npages_decay_max pages without violating the
 * invariant: (ecache_npages_get(ecache) >= npages_limit).  We need an upper
 * bound on number of pages in order to prevent unbounded growth (namely in
 * stashed), otherwise unbounded new pages could be added to extents during the
 * current decay run, so that the purging thread never finishes.
 */
static void
pa_decay_to_limit(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    size_t npages_limit, size_t npages_decay_max) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 1);

	if (decay->purging || npages_decay_max == 0) {
		return;
	}
	decay->purging = true;
	malloc_mutex_unlock(tsdn, &decay->mtx);

	edata_list_t decay_extents;
	edata_list_init(&decay_extents);
	size_t npurge = pa_stash_decayed(tsdn, shard, ecache, npages_limit,
	    npages_decay_max, &decay_extents);
	if (npurge != 0) {
		size_t npurged = pa_decay_stashed(tsdn, shard, decay,
		    decay_stats, ecache, fully_decay, &decay_extents);
		assert(npurged == npurge);
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	decay->purging = false;
}

void
pa_decay_all(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);
	pa_decay_to_limit(tsdn, shard, decay, decay_stats, ecache, fully_decay,
	    /* npages_limit */ 0, ecache_npages_get(ecache));
}

static void
pa_decay_try_purge(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache,
    size_t current_npages, size_t npages_limit) {
	if (current_npages > npages_limit) {
		pa_decay_to_limit(tsdn, shard, decay, decay_stats, ecache,
		    /* fully_decay */ false, npages_limit,
		    current_npages - npages_limit);
	}
}

bool
pa_maybe_decay_purge(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache,
    pa_decay_purge_setting_t decay_purge_setting) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);

	/* Purge all or nothing if the option is disabled. */
	ssize_t decay_ms = decay_ms_read(decay);
	if (decay_ms <= 0) {
		if (decay_ms == 0) {
			pa_decay_to_limit(tsdn, shard, decay, decay_stats,
			    ecache, /* fully_decay */ false,
			    /* npages_limit */ 0, ecache_npages_get(ecache));
		}
		return false;
	}

	/*
	 * If the deadline has been reached, advance to the current epoch and
	 * purge to the new limit if necessary.  Note that dirty pages created
	 * during the current epoch are not subject to purge until a future
	 * epoch, so as a result purging only happens during epoch advances, or
	 * being triggered by background threads (scheduled event).
	 */
	nstime_t time;
	nstime_init_update(&time);
	size_t npages_current = ecache_npages_get(ecache);
	bool epoch_advanced = decay_maybe_advance_epoch(decay, &time,
	    npages_current);
	if (decay_purge_setting == PA_DECAY_PURGE_ALWAYS
	    || (epoch_advanced && decay_purge_setting
	    == PA_DECAY_PURGE_ON_EPOCH_ADVANCE)) {
		size_t npages_limit = decay_npages_limit_get(decay);
		pa_decay_try_purge(tsdn, shard, decay, decay_stats, ecache,
		    npages_current, npages_limit);
	}

	return epoch_advanced;
}

bool
pa_shard_retain_grow_limit_get_set(tsdn_t *tsdn, pa_shard_t *shard,
    size_t *old_limit, size_t *new_limit) {
	pszind_t new_ind JEMALLOC_CC_SILENCE_INIT(0);
	if (new_limit != NULL) {
		size_t limit = *new_limit;
		/* Grow no more than the new limit. */
		if ((new_ind = sz_psz2ind(limit + 1) - 1) >= SC_NPSIZES) {
			return true;
		}
	}

	malloc_mutex_lock(tsdn, &shard->ecache_grow.mtx);
	if (old_limit != NULL) {
		*old_limit = sz_pind2sz(shard->ecache_grow.limit);
	}
	if (new_limit != NULL) {
		shard->ecache_grow.limit = new_ind;
	}
	malloc_mutex_unlock(tsdn, &shard->ecache_grow.mtx);

	return false;
}
