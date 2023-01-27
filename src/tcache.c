#define JEMALLOC_TCACHE_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"

/******************************************************************************/
/* Data. */

#if !defined(__BIONIC__) || defined(ANDROID_ENABLE_TCACHE)
bool	opt_tcache = true;
#else
bool	opt_tcache = false;
#endif
ssize_t	opt_lg_tcache_max = LG_TCACHE_MAXCLASS_DEFAULT;

cache_bin_info_t	*tcache_bin_info;

/* Total stack size required (per tcache).  Include the padding above. */
static size_t tcache_bin_alloc_size;
static size_t tcache_bin_alloc_alignment;

unsigned		nhbins;
size_t			tcache_maxclass;

tcaches_t		*tcaches;

/* Index of first element within tcaches that has never been used. */
static unsigned		tcaches_past;

/* Head of singly linked list tracking available tcaches elements. */
static tcaches_t	*tcaches_avail;

/* Protects tcaches{,_past,_avail}. */
static malloc_mutex_t	tcaches_mtx;

/******************************************************************************/

size_t
tcache_salloc(tsdn_t *tsdn, const void *ptr) {
	return arena_salloc(tsdn, ptr);
}

void
tcache_event_hard(tsd_t *tsd, tcache_t *tcache) {
	szind_t binind = tcache->next_gc_bin;
	cache_bin_t *tbin;
	bool is_small;
	if (binind < SC_NBINS) {
		tbin = tcache_small_bin_get(tcache, binind);
		is_small = true;
	} else {
		tbin = tcache_large_bin_get(tcache, binind);
		is_small = false;
	}

	cache_bin_sz_t low_water = cache_bin_low_water_get(tbin,
	    &tcache_bin_info[binind]);
	cache_bin_sz_t ncached = cache_bin_ncached_get(tbin,
	    &tcache_bin_info[binind]);
	if (low_water > 0) {
		/*
		 * Flush (ceiling) 3/4 of the objects below the low water mark.
		 */
		if (is_small) {
			assert(!tcache->bin_refilled[binind]);
			tcache_bin_flush_small(tsd, tcache, tbin, binind,
			    ncached - low_water + (low_water >> 2));
			/*
			 * Reduce fill count by 2X.  Limit lg_fill_div such that
			 * the fill count is always at least 1.
			 */
			if ((cache_bin_info_ncached_max(
			    &tcache_bin_info[binind]) >>
			    (tcache->lg_fill_div[binind] + 1)) >= 1) {
				tcache->lg_fill_div[binind]++;
			}
		} else {
			tcache_bin_flush_large(tsd, tcache, tbin, binind,
			     ncached - low_water + (low_water >> 2));
		}
	} else if (is_small && tcache->bin_refilled[binind]) {
		assert(low_water == 0);
		/*
		 * Increase fill count by 2X for small bins.  Make sure
		 * lg_fill_div stays greater than 0.
		 */
		if (tcache->lg_fill_div[binind] > 1) {
			tcache->lg_fill_div[binind]--;
		}
		tcache->bin_refilled[binind] = false;
	}
	tbin->low_water_position = tbin->cur_ptr.lowbits;

	tcache->next_gc_bin++;
	if (tcache->next_gc_bin == nhbins) {
		tcache->next_gc_bin = 0;
	}
}

void *
tcache_alloc_small_hard(tsdn_t *tsdn, arena_t *arena, tcache_t *tcache,
    cache_bin_t *tbin, szind_t binind, bool *tcache_success) {
	void *ret;

	assert(tcache->arena != NULL);
	arena_tcache_fill_small(tsdn, arena, tcache, tbin, binind);
	ret = cache_bin_alloc_easy(tbin, &tcache_bin_info[binind],
	    tcache_success);

	return ret;
}

/* Enabled with --enable-extra-size-check. */
static void
tbin_edatas_lookup_size_check(tsd_t *tsd, cache_bin_ptr_array_t *arr,
    szind_t binind, size_t nflush, edata_t **edatas) {
	/* Avoids null-checking tsdn in the loop below. */
	util_assume(tsd != NULL);

	/*
	 * Verify that the items in the tcache all have the correct size; this
	 * is useful for catching sized deallocation bugs, also to fail early
	 * instead of corrupting metadata.  Since this can be turned on for opt
	 * builds, avoid the branch in the loop.
	 */
	size_t szind_sum = binind * nflush;
	for (unsigned i = 0; i < nflush; i++) {
		emap_full_alloc_ctx_t full_alloc_ctx;
		emap_full_alloc_ctx_lookup(tsd_tsdn(tsd), &emap_global,
		    cache_bin_ptr_array_get(arr, i), &full_alloc_ctx);
		edatas[i] = full_alloc_ctx.edata;
		szind_sum -= full_alloc_ctx.szind;
	}

	if (szind_sum != 0) {
		safety_check_fail_sized_dealloc(false);
	}
}

JEMALLOC_ALWAYS_INLINE bool
tcache_bin_flush_match(edata_t *edata, unsigned cur_arena_ind,
    unsigned cur_binshard, bool small) {
	if (small) {
		return edata_arena_ind_get(edata) == cur_arena_ind
		    && edata_binshard_get(edata) == cur_binshard;
	} else {
		return edata_arena_ind_get(edata) == cur_arena_ind;
	}
}

JEMALLOC_ALWAYS_INLINE void
tcache_bin_flush_impl(tsd_t *tsd, tcache_t *tcache, cache_bin_t *tbin,
    szind_t binind, unsigned rem, bool small) {
	/*
	 * A couple lookup calls take tsdn; declare it once for convenience
	 * instead of calling tsd_tsdn(tsd) all the time.
	 */
	tsdn_t *tsdn = tsd_tsdn(tsd);

	if (small) {
		assert(binind < SC_NBINS);
	} else {
		assert(binind < nhbins);
	}
	cache_bin_sz_t ncached = cache_bin_ncached_get(tbin,
	    &tcache_bin_info[binind]);
	assert((cache_bin_sz_t)rem <= ncached);
	arena_t *tcache_arena = tcache->arena;
	assert(tcache_arena != NULL);

	unsigned nflush = ncached - rem;
	/*
	 * Variable length array must have > 0 length; the last element is never
	 * touched (it's just included to satisfy the no-zero-length rule).
	 */
	VARIABLE_ARRAY(edata_t *, item_edata, nflush + 1);
	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nflush);

	cache_bin_init_ptr_array_for_flush(tbin, &tcache_bin_info[binind],
	    &ptrs, nflush);

	/* Look up edata once per item. */
	if (config_opt_safety_checks) {
		tbin_edatas_lookup_size_check(tsd, &ptrs, binind, nflush,
		    item_edata);
	} else {
		for (unsigned i = 0 ; i < nflush; i++) {
			item_edata[i] = emap_edata_lookup(tsd_tsdn(tsd),
			    &emap_global, cache_bin_ptr_array_get(&ptrs, i));
		}
	}

	/*
	 * The slabs where we freed the last remaining object in the slab (and
	 * so need to free the slab itself).
	 * Used only if small == true.
	 */
	unsigned dalloc_count = 0;
	VARIABLE_ARRAY(edata_t *, dalloc_slabs, nflush + 1);

	/*
	 * We're about to grab a bunch of locks.  If one of them happens to be
	 * the one guarding the arena-level stats counters we flush our
	 * thread-local ones to, we do so under one critical section.
	 */
	bool merged_stats = false;
	while (nflush > 0) {
		/* Lock the arena, or bin, associated with the first object. */
		edata_t *edata = item_edata[0];
		unsigned cur_arena_ind = edata_arena_ind_get(edata);
		arena_t *cur_arena = arena_get(tsdn, cur_arena_ind, false);

		/*
		 * These assignments are always overwritten when small is true,
		 * and their values are always ignored when small is false, but
		 * to avoid the technical UB when we pass them as parameters, we
		 * need to intialize them.
		 */
		unsigned cur_binshard = 0;
		bin_t *cur_bin = NULL;
		if (small) {
			cur_binshard = edata_binshard_get(edata);
			cur_bin = &cur_arena->bins[binind].bin_shards[
			    cur_binshard];
			assert(cur_binshard < bin_infos[binind].n_shards);
		}

		if (small) {
			malloc_mutex_lock(tsdn, &cur_bin->lock);
		}
		if (!small && !arena_is_auto(cur_arena)) {
			malloc_mutex_lock(tsdn, &cur_arena->large_mtx);
		}

		/*
		 * If we acquired the right lock and have some stats to flush,
		 * flush them.
		 */
#if defined(ANDROID_ENABLE_TCACHE_STATS)
		if (config_stats && tcache_arena == cur_arena
		    && !merged_stats) {
			merged_stats = true;
			if (small) {
				cur_bin->stats.nflushes++;
				cur_bin->stats.nrequests +=
				    tbin->tstats.nrequests;
				tbin->tstats.nrequests = 0;
			} else {
				arena_stats_large_flush_nrequests_add(tsdn,
				    &tcache_arena->stats, binind,
				    tbin->tstats.nrequests);
				tbin->tstats.nrequests = 0;
			}
		}
#endif
		/*
		 * Large allocations need special prep done.  Afterwards, we can
		 * drop the large lock.
		 */
		if (!small) {
			for (unsigned i = 0; i < nflush; i++) {
				void *ptr = cache_bin_ptr_array_get(&ptrs, i);
				edata = item_edata[i];
				assert(ptr != NULL && edata != NULL);

				if (tcache_bin_flush_match(edata, cur_arena_ind,
				    cur_binshard, small)) {
					large_dalloc_prep_locked(tsdn,
					    edata);
				}
			}
		}
		if (!small && !arena_is_auto(cur_arena)) {
			malloc_mutex_unlock(tsdn, &cur_arena->large_mtx);
		}

		/* Deallocate whatever we can. */
		unsigned ndeferred = 0;
		for (unsigned i = 0; i < nflush; i++) {
			void *ptr = cache_bin_ptr_array_get(&ptrs, i);
			edata = item_edata[i];
			assert(ptr != NULL && edata != NULL);
			if (!tcache_bin_flush_match(edata, cur_arena_ind,
			    cur_binshard, small)) {
				/*
				 * The object was allocated either via a
				 * different arena, or a different bin in this
				 * arena.  Either way, stash the object so that
				 * it can be handled in a future pass.
				 */
				cache_bin_ptr_array_set(&ptrs, ndeferred, ptr);
				item_edata[ndeferred] = edata;
				ndeferred++;
				continue;
			}
			if (small) {
				if (arena_dalloc_bin_locked(tsdn, cur_arena,
				    cur_bin, binind, edata, ptr)) {
					dalloc_slabs[dalloc_count] = edata;
					dalloc_count++;
				}
			} else {
				large_dalloc_finish(tsdn, edata);
			}
		}

		if (small) {
			malloc_mutex_unlock(tsdn, &cur_bin->lock);
		}
		arena_decay_ticks(tsdn, cur_arena, nflush - ndeferred);
		nflush = ndeferred;
	}

	/* Handle all deferred slab dalloc. */
	assert(small || dalloc_count == 0);
	for (unsigned i = 0; i < dalloc_count; i++) {
		edata_t *slab = dalloc_slabs[i];
		arena_slab_dalloc(tsdn, arena_get_from_edata(slab), slab);

	}

#if defined(ANDROID_ENABLE_TCACHE_STATS)
	if (config_stats && !merged_stats) {
		if (small) {
			/*
			 * The flush loop didn't happen to flush to this
			 * thread's arena, so the stats didn't get merged.
			 * Manually do so now.
			 */
			unsigned binshard;
			bin_t *bin = arena_bin_choose_lock(tsdn, tcache_arena,
			    binind, &binshard);
			bin->stats.nflushes++;
			bin->stats.nrequests += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
			malloc_mutex_unlock(tsdn, &bin->lock);
		} else {
			arena_stats_large_flush_nrequests_add(tsdn,
			    &tcache_arena->stats, binind,
			    tbin->tstats.nrequests);
			tbin->tstats.nrequests = 0;
		}
	}
#endif

	cache_bin_finish_flush(tbin, &tcache_bin_info[binind], &ptrs,
	    ncached - rem);
}

void
tcache_bin_flush_small(tsd_t *tsd, tcache_t *tcache, cache_bin_t *tbin,
    szind_t binind, unsigned rem) {
	tcache_bin_flush_impl(tsd, tcache, tbin, binind, rem, true);
}

void
tcache_bin_flush_large(tsd_t *tsd, tcache_t *tcache, cache_bin_t *tbin,
    szind_t binind, unsigned rem) {
	tcache_bin_flush_impl(tsd, tcache, tbin, binind, rem, false);
}

void
tcache_arena_associate(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	assert(tcache->arena == NULL);
	tcache->arena = arena;

	if (config_stats) {
		/* Link into list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);

		ql_elm_new(tcache, link);
		ql_tail_insert(&arena->tcache_ql, tcache, link);
		cache_bin_array_descriptor_init(
		    &tcache->cache_bin_array_descriptor, tcache->bins_small,
		    tcache->bins_large);
		ql_tail_insert(&arena->cache_bin_array_descriptor_ql,
		    &tcache->cache_bin_array_descriptor, link);

		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
}

static void
tcache_arena_dissociate(tsdn_t *tsdn, tcache_t *tcache) {
	arena_t *arena = tcache->arena;
	assert(arena != NULL);
	if (config_stats) {
		/* Unlink from list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
		if (config_debug) {
			bool in_ql = false;
			tcache_t *iter;
			ql_foreach(iter, &arena->tcache_ql, link) {
				if (iter == tcache) {
					in_ql = true;
					break;
				}
			}
			assert(in_ql);
		}
		ql_remove(&arena->tcache_ql, tcache, link);
		ql_remove(&arena->cache_bin_array_descriptor_ql,
		    &tcache->cache_bin_array_descriptor, link);
		tcache_stats_merge(tsdn, tcache, arena);
		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
	tcache->arena = NULL;
}

void
tcache_arena_reassociate(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	tcache_arena_dissociate(tsdn, tcache);
	tcache_arena_associate(tsdn, tcache, arena);
}

bool
tsd_tcache_enabled_data_init(tsd_t *tsd) {
	/* Called upon tsd initialization. */
	tsd_tcache_enabled_set(tsd, opt_tcache);
	tsd_slow_update(tsd);

	if (opt_tcache) {
		/* Trigger tcache init. */
		tsd_tcache_data_init(tsd);
	}

	return false;
}

static void
tcache_init(tsd_t *tsd, tcache_t *tcache, void *avail_stack) {
	memset(&tcache->link, 0, sizeof(ql_elm(tcache_t)));
	tcache->next_gc_bin = 0;
	tcache->arena = NULL;

	assert((TCACHE_NSLOTS_SMALL_MAX & 1U) == 0);
	memset(tcache->bins_small, 0, sizeof(cache_bin_t) * SC_NBINS);
	memset(tcache->bins_large, 0, sizeof(cache_bin_t) * (nhbins - SC_NBINS));

	unsigned i = 0;
	size_t cur_offset = 0;
	cache_bin_preincrement(tcache_bin_info, nhbins, avail_stack,
	    &cur_offset);
	for (; i < SC_NBINS; i++) {
		tcache->lg_fill_div[i] = 1;
		tcache->bin_refilled[i] = false;
		cache_bin_t *bin = tcache_small_bin_get(tcache, i);
		cache_bin_init(bin, &tcache_bin_info[i], avail_stack,
		    &cur_offset);
	}
	for (; i < nhbins; i++) {
		cache_bin_t *bin = tcache_large_bin_get(tcache, i);
		cache_bin_init(bin, &tcache_bin_info[i], avail_stack,
		    &cur_offset);
	}
	cache_bin_postincrement(tcache_bin_info, nhbins, avail_stack,
	    &cur_offset);
	/* Sanity check that the whole stack is used. */
	assert(cur_offset == tcache_bin_alloc_size);
}

/* Initialize auto tcache (embedded in TSD). */
bool
tsd_tcache_data_init(tsd_t *tsd) {
	tcache_t *tcache = tsd_tcachep_get_unsafe(tsd);
	assert(tcache_small_bin_get(tcache, 0)->cur_ptr.ptr == NULL);
	size_t alignment = tcache_bin_alloc_alignment;
	size_t size = sz_sa2u(tcache_bin_alloc_size, alignment);

	void *avail_array = ipallocztm(tsd_tsdn(tsd), size, alignment, true,
	    NULL, true, arena_get(TSDN_NULL, 0, true));
	if (avail_array == NULL) {
		return true;
	}

	tcache_init(tsd, tcache, avail_array);
	/*
	 * Initialization is a bit tricky here.  After malloc init is done, all
	 * threads can rely on arena_choose and associate tcache accordingly.
	 * However, the thread that does actual malloc bootstrapping relies on
	 * functional tsd, and it can only rely on a0.  In that case, we
	 * associate its tcache to a0 temporarily, and later on
	 * arena_choose_hard() will re-associate properly.
	 */
	tcache->arena = NULL;
	arena_t *arena;
	if (!malloc_initialized()) {
		/* If in initialization, assign to a0. */
		arena = arena_get(tsd_tsdn(tsd), 0, false);
		tcache_arena_associate(tsd_tsdn(tsd), tcache, arena);
	} else {
		arena = arena_choose(tsd, NULL);
		/* This may happen if thread.tcache.enabled is used. */
		if (tcache->arena == NULL) {
			tcache_arena_associate(tsd_tsdn(tsd), tcache, arena);
		}
	}
	assert(arena == tcache->arena);

	return false;
}

/* Created manual tcache for tcache.create mallctl. */
tcache_t *
tcache_create_explicit(tsd_t *tsd) {
	/*
	 * We place the cache bin stacks, then the tcache_t, then a pointer to
	 * the beginning of the whole allocation (for freeing).  The makes sure
	 * the cache bins have the requested alignment.
	 */
	size_t size = tcache_bin_alloc_size + sizeof(tcache_t) + sizeof(void *);
	/* Naturally align the pointer stacks. */
	size = PTR_CEILING(size);
	size = sz_sa2u(size, tcache_bin_alloc_alignment);

	void *mem = ipallocztm(tsd_tsdn(tsd), size, tcache_bin_alloc_alignment,
	    true, NULL, true, arena_get(TSDN_NULL, 0, true));
	if (mem == NULL) {
		return NULL;
	}
	void *avail_array = mem;
	tcache_t *tcache = (void *)((uintptr_t)avail_array
	    + tcache_bin_alloc_size);
	void **head_ptr = (void *)((uintptr_t)avail_array
	    + tcache_bin_alloc_size + sizeof(tcache_t));
	tcache_init(tsd, tcache, avail_array);
	*head_ptr = mem;

	tcache_init(tsd, tcache, avail_array);
	tcache_arena_associate(tsd_tsdn(tsd), tcache, arena_ichoose(tsd, NULL));

	return tcache;
}

static void
tcache_flush_cache(tsd_t *tsd, tcache_t *tcache) {
	assert(tcache->arena != NULL);

	for (unsigned i = 0; i < SC_NBINS; i++) {
		cache_bin_t *tbin = tcache_small_bin_get(tcache, i);
		tcache_bin_flush_small(tsd, tcache, tbin, i, 0);

#if defined(ANDROID_ENABLE_TCACHE_STATS)
		if (config_stats) {
			assert(tbin->tstats.nrequests == 0);
		}
#endif
	}
	for (unsigned i = SC_NBINS; i < nhbins; i++) {
		cache_bin_t *tbin = tcache_large_bin_get(tcache, i);
		tcache_bin_flush_large(tsd, tcache, tbin, i, 0);

#if defined(ANDROID_ENABLE_TCACHE_STATS)
		if (config_stats) {
			assert(tbin->tstats.nrequests == 0);
		}
#endif
	}
}

void
tcache_flush(tsd_t *tsd) {
	assert(tcache_available(tsd));
	tcache_flush_cache(tsd, tsd_tcachep_get(tsd));
}

static void
tcache_destroy(tsd_t *tsd, tcache_t *tcache, bool tsd_tcache) {
	tcache_flush_cache(tsd, tcache);
	arena_t *arena = tcache->arena;
	tcache_arena_dissociate(tsd_tsdn(tsd), tcache);

	if (tsd_tcache) {
		/* Release the avail array for the TSD embedded auto tcache. */
		cache_bin_t *bin = tcache_small_bin_get(tcache, 0);
		assert(cache_bin_ncached_get(bin, &tcache_bin_info[0]) == 0);
		assert(cache_bin_empty_position_get(bin, &tcache_bin_info[0]) ==
		    bin->cur_ptr.ptr);
		void *avail_array = (void *)((uintptr_t)bin->cur_ptr.ptr -
		    tcache_bin_info[0].stack_size);
		idalloctm(tsd_tsdn(tsd), avail_array, NULL, NULL, true, true);
	} else {
		/* See the comment at the top of tcache_create_explicit. */
		void **mem_begin = (void **)((uintptr_t)tcache + sizeof(tcache_t));
		/* Release both the tcache struct and avail array. */
		idalloctm(tsd_tsdn(tsd), *mem_begin, NULL, NULL, true, true);
	}

	/*
	 * The deallocation and tcache flush above may not trigger decay since
	 * we are on the tcache shutdown path (potentially with non-nominal
	 * tsd).  Manually trigger decay to avoid pathological cases.  Also
	 * include arena 0 because the tcache array is allocated from it.
	 */
	arena_decay(tsd_tsdn(tsd), arena_get(tsd_tsdn(tsd), 0, false),
	    false, false);

	if (arena_nthreads_get(arena, false) == 0 &&
	    !background_thread_enabled()) {
		/* Force purging when no threads assigned to the arena anymore. */
		arena_decay(tsd_tsdn(tsd), arena, false, true);
	} else {
		arena_decay(tsd_tsdn(tsd), arena, false, false);
	}
}

/* For auto tcache (embedded in TSD) only. */
void
tcache_cleanup(tsd_t *tsd) {
	tcache_t *tcache = tsd_tcachep_get(tsd);
	if (!tcache_available(tsd)) {
		assert(tsd_tcache_enabled_get(tsd) == false);
		if (config_debug) {
			assert(tcache_small_bin_get(tcache, 0)->cur_ptr.ptr
			    == NULL);
		}
		return;
	}
	assert(tsd_tcache_enabled_get(tsd));
	assert(tcache_small_bin_get(tcache, 0)->cur_ptr.ptr != NULL);

	tcache_destroy(tsd, tcache, true);
	if (config_debug) {
		tcache_small_bin_get(tcache, 0)->cur_ptr.ptr = NULL;
	}
}

void
tcache_stats_merge(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
#if defined(ANDROID_ENABLE_TCACHE_STATS)
	unsigned i;

	cassert(config_stats);

	/* Merge and reset tcache stats. */
	for (i = 0; i < SC_NBINS; i++) {
		cache_bin_t *tbin = tcache_small_bin_get(tcache, i);
		unsigned binshard;
		bin_t *bin = arena_bin_choose_lock(tsdn, arena, i, &binshard);
		bin->stats.nrequests += tbin->tstats.nrequests;
		malloc_mutex_unlock(tsdn, &bin->lock);
		tbin->tstats.nrequests = 0;
	}

	for (; i < nhbins; i++) {
		cache_bin_t *tbin = tcache_large_bin_get(tcache, i);
		arena_stats_large_flush_nrequests_add(tsdn, &arena->stats, i,
		    tbin->tstats.nrequests);
		tbin->tstats.nrequests = 0;
	}
#endif
}

static bool
tcaches_create_prep(tsd_t *tsd, base_t *base) {
	bool err;

	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);

	if (tcaches == NULL) {
		tcaches = base_alloc(tsd_tsdn(tsd), base,
		    sizeof(tcache_t *) * (MALLOCX_TCACHE_MAX+1), CACHELINE);
		if (tcaches == NULL) {
			err = true;
			goto label_return;
		}
	}

	if (tcaches_avail == NULL && tcaches_past > MALLOCX_TCACHE_MAX) {
		err = true;
		goto label_return;
	}

	err = false;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	return err;
}

bool
tcaches_create(tsd_t *tsd, base_t *base, unsigned *r_ind) {
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);

	bool err;

	if (tcaches_create_prep(tsd, base)) {
		err = true;
		goto label_return;
	}

	tcache_t *tcache = tcache_create_explicit(tsd);
	if (tcache == NULL) {
		err = true;
		goto label_return;
	}

	tcaches_t *elm;
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcaches_avail != NULL) {
		elm = tcaches_avail;
		tcaches_avail = tcaches_avail->next;
		elm->tcache = tcache;
		*r_ind = (unsigned)(elm - tcaches);
	} else {
		elm = &tcaches[tcaches_past];
		elm->tcache = tcache;
		*r_ind = tcaches_past;
		tcaches_past++;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);

	err = false;
label_return:
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);
	return err;
}

static tcache_t *
tcaches_elm_remove(tsd_t *tsd, tcaches_t *elm, bool allow_reinit) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tcaches_mtx);

	if (elm->tcache == NULL) {
		return NULL;
	}
	tcache_t *tcache = elm->tcache;
	if (allow_reinit) {
		elm->tcache = TCACHES_ELM_NEED_REINIT;
	} else {
		elm->tcache = NULL;
	}

	if (tcache == TCACHES_ELM_NEED_REINIT) {
		return NULL;
	}
	return tcache;
}

void
tcaches_flush(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcache_t *tcache = tcaches_elm_remove(tsd, &tcaches[ind], true);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		/* Destroy the tcache; recreate in tcaches_get() if needed. */
		tcache_destroy(tsd, tcache, false);
	}
}

void
tcaches_destroy(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcaches_t *elm = &tcaches[ind];
	tcache_t *tcache = tcaches_elm_remove(tsd, elm, false);
	elm->next = tcaches_avail;
	tcaches_avail = elm;
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		tcache_destroy(tsd, tcache, false);
	}
}

bool
tcache_boot(tsdn_t *tsdn, base_t *base) {
	/* If necessary, clamp opt_lg_tcache_max. */
	if (opt_lg_tcache_max < 0 || (ZU(1) << opt_lg_tcache_max) <
	    SC_SMALL_MAXCLASS) {
		tcache_maxclass = SC_SMALL_MAXCLASS;
	} else {
		tcache_maxclass = (ZU(1) << opt_lg_tcache_max);
	}

	if (malloc_mutex_init(&tcaches_mtx, "tcaches", WITNESS_RANK_TCACHES,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}

	nhbins = sz_size2index(tcache_maxclass) + 1;

	/* Initialize tcache_bin_info. */
	tcache_bin_info = (cache_bin_info_t *)base_alloc(tsdn, base,
	    nhbins * sizeof(cache_bin_info_t), CACHELINE);
	if (tcache_bin_info == NULL) {
		return true;
	}
	unsigned i, ncached_max;
	for (i = 0; i < SC_NBINS; i++) {
		if ((bin_infos[i].nregs << 1) <= TCACHE_NSLOTS_SMALL_MIN) {
			ncached_max = TCACHE_NSLOTS_SMALL_MIN;
		} else if ((bin_infos[i].nregs << 1) <=
		    TCACHE_NSLOTS_SMALL_MAX) {
			ncached_max = bin_infos[i].nregs << 1;
		} else {
			ncached_max = TCACHE_NSLOTS_SMALL_MAX;
		}
		cache_bin_info_init(&tcache_bin_info[i], ncached_max);
	}
	for (; i < nhbins; i++) {
		cache_bin_info_init(&tcache_bin_info[i], TCACHE_NSLOTS_LARGE);
	}
	cache_bin_info_compute_alloc(tcache_bin_info, i, &tcache_bin_alloc_size,
	    &tcache_bin_alloc_alignment);

	return false;
}

void
tcache_prefork(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &tcaches_mtx);
}

void
tcache_postfork_parent(tsdn_t *tsdn) {
	malloc_mutex_postfork_parent(tsdn, &tcaches_mtx);
}

void
tcache_postfork_child(tsdn_t *tsdn) {
	malloc_mutex_postfork_child(tsdn, &tcaches_mtx);
}
