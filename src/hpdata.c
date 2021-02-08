#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpdata.h"

static int
hpdata_age_comp(const hpdata_t *a, const hpdata_t *b) {
	uint64_t a_age = hpdata_age_get(a);
	uint64_t b_age = hpdata_age_get(b);
	/*
	 * hpdata ages are operation counts in the psset; no two should be the
	 * same.
	 */
	assert(a_age != b_age);
	return (a_age > b_age) - (a_age < b_age);
}

ph_gen(, hpdata_age_heap_, hpdata_age_heap_t, hpdata_t, ph_link, hpdata_age_comp)

void
hpdata_init(hpdata_t *hpdata, void *addr, uint64_t age) {
	hpdata_addr_set(hpdata, addr);
	hpdata_age_set(hpdata, age);
	hpdata->h_huge = false;
	hpdata->h_alloc_allowed = true;
	hpdata->h_in_psset_alloc_container = false;
	hpdata->h_purge_allowed = false;
	hpdata->h_hugify_allowed = false;
	hpdata->h_in_psset_hugify_container = false;
	hpdata->h_mid_purge = false;
	hpdata->h_mid_hugify = false;
	hpdata->h_updating = false;
	hpdata->h_in_psset = false;
	hpdata_longest_free_range_set(hpdata, HUGEPAGE_PAGES);
	hpdata->h_nactive = 0;
	fb_init(hpdata->active_pages, HUGEPAGE_PAGES);
	hpdata->h_ntouched = 0;
	fb_init(hpdata->touched_pages, HUGEPAGE_PAGES);

	hpdata_assert_consistent(hpdata);
}

void *
hpdata_reserve_alloc(hpdata_t *hpdata, size_t sz) {
	hpdata_assert_consistent(hpdata);
	/*
	 * This is a metadata change; the hpdata should therefore either not be
	 * in the psset, or should have explicitly marked itself as being
	 * mid-update.
	 */
	assert(!hpdata->h_in_psset || hpdata->h_updating);
	assert(hpdata->h_alloc_allowed);
	assert((sz & PAGE_MASK) == 0);
	size_t npages = sz >> LG_PAGE;
	assert(npages <= hpdata_longest_free_range_get(hpdata));

	size_t result;

	size_t start = 0;
	/*
	 * These are dead stores, but the compiler will issue warnings on them
	 * since it can't tell statically that found is always true below.
	 */
	size_t begin = 0;
	size_t len = 0;

	size_t largest_unchosen_range = 0;
	while (true) {
		bool found = fb_urange_iter(hpdata->active_pages,
		    HUGEPAGE_PAGES, start, &begin, &len);
		/*
		 * A precondition to this function is that hpdata must be able
		 * to serve the allocation.
		 */
		assert(found);
		assert(len <= hpdata_longest_free_range_get(hpdata));
		if (len >= npages) {
			/*
			 * We use first-fit within the page slabs; this gives
			 * bounded worst-case fragmentation within a slab.  It's
			 * not necessarily right; we could experiment with
			 * various other options.
			 */
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	/* We found a range; remember it. */
	result = begin;
	fb_set_range(hpdata->active_pages, HUGEPAGE_PAGES, begin, npages);
	hpdata->h_nactive += npages;

	/*
	 * We might be about to dirty some memory for the first time; update our
	 * count if so.
	 */
	size_t new_dirty = fb_ucount(hpdata->touched_pages,  HUGEPAGE_PAGES,
	    result, npages);
	fb_set_range(hpdata->touched_pages, HUGEPAGE_PAGES, result, npages);
	hpdata->h_ntouched += new_dirty;

	/*
	 * If we allocated out of a range that was the longest in the hpdata, it
	 * might be the only one of that size and we'll have to adjust the
	 * metadata.
	 */
	if (len == hpdata_longest_free_range_get(hpdata)) {
		start = begin + npages;
		while (start < HUGEPAGE_PAGES) {
			bool found = fb_urange_iter(hpdata->active_pages,
			    HUGEPAGE_PAGES, start, &begin, &len);
			if (!found) {
				break;
			}
			assert(len <= hpdata_longest_free_range_get(hpdata));
			if (len == hpdata_longest_free_range_get(hpdata)) {
				largest_unchosen_range = len;
				break;
			}
			if (len > largest_unchosen_range) {
				largest_unchosen_range = len;
			}
			start = begin + len;
		}
		hpdata_longest_free_range_set(hpdata, largest_unchosen_range);
	}

	hpdata_assert_consistent(hpdata);
	return (void *)(
	    (uintptr_t)hpdata_addr_get(hpdata) + (result << LG_PAGE));
}

void
hpdata_unreserve(hpdata_t *hpdata, void *addr, size_t sz) {
	hpdata_assert_consistent(hpdata);
	/* See the comment in reserve. */
	assert(!hpdata->h_in_psset || hpdata->h_updating);
	assert(((uintptr_t)addr & PAGE_MASK) == 0);
	assert((sz & PAGE_MASK) == 0);
	size_t begin = ((uintptr_t)addr - (uintptr_t)hpdata_addr_get(hpdata))
	    >> LG_PAGE;
	assert(begin < HUGEPAGE_PAGES);
	size_t npages = sz >> LG_PAGE;
	size_t old_longest_range = hpdata_longest_free_range_get(hpdata);

	fb_unset_range(hpdata->active_pages, HUGEPAGE_PAGES, begin, npages);
	/* We might have just created a new, larger range. */
	size_t new_begin = (fb_fls(hpdata->active_pages, HUGEPAGE_PAGES,
	    begin) + 1);
	size_t new_end = fb_ffs(hpdata->active_pages, HUGEPAGE_PAGES,
	    begin + npages - 1);
	size_t new_range_len = new_end - new_begin;

	if (new_range_len > old_longest_range) {
		hpdata_longest_free_range_set(hpdata, new_range_len);
	}

	hpdata->h_nactive -= npages;

	hpdata_assert_consistent(hpdata);
}

size_t
hpdata_purge_begin(hpdata_t *hpdata, hpdata_purge_state_t *purge_state) {
	hpdata_assert_consistent(hpdata);
	/* See the comment in reserve. */

	purge_state->npurged = 0;
	purge_state->next_purge_search_begin = 0;

	/*
	 * Initialize to_purge with everything that's not active but that is
	 * dirty.
	 *
	 * As an optimization, we could note that in practice we never allocate
	 * out of a hugepage while purging within it, and so could try to
	 * combine dirty extents separated by a non-dirty but non-active extent
	 * to avoid purge calls.  This does nontrivially complicate metadata
	 * tracking though, so let's hold off for now.
	 */
	fb_bit_not(purge_state->to_purge, hpdata->active_pages, HUGEPAGE_PAGES);
	fb_bit_and(purge_state->to_purge, purge_state->to_purge,
	    hpdata->touched_pages, HUGEPAGE_PAGES);

	/* We purge everything we can. */
	size_t to_purge = hpdata->h_ntouched - hpdata->h_nactive;
	assert(to_purge == fb_scount(
	    purge_state->to_purge, HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES));

	hpdata_assert_consistent(hpdata);

	return to_purge;
}

bool
hpdata_purge_next(hpdata_t *hpdata, hpdata_purge_state_t *purge_state,
    void **r_purge_addr, size_t *r_purge_size) {
	/*
	 * Note that we don't have a consistency check here; we're accessing
	 * hpdata without synchronization, and therefore have no right to expect
	 * a consistent state.
	 */

	if (purge_state->next_purge_search_begin == HUGEPAGE_PAGES) {
		return false;
	}
	size_t purge_begin;
	size_t purge_len;
	bool found_range = fb_srange_iter(purge_state->to_purge, HUGEPAGE_PAGES,
	    purge_state->next_purge_search_begin, &purge_begin, &purge_len);
	if (!found_range) {
		return false;
	}

	*r_purge_addr = (void *)(
	    (uintptr_t)hpdata_addr_get(hpdata) + purge_begin * PAGE);
	*r_purge_size = purge_len * PAGE;

	purge_state->next_purge_search_begin = purge_begin + purge_len;
	purge_state->npurged += purge_len;
	assert(purge_state->npurged <= HUGEPAGE_PAGES);

	return true;
}

void
hpdata_purge_end(hpdata_t *hpdata, hpdata_purge_state_t *purge_state) {
	hpdata_assert_consistent(hpdata);
	/* See the comment in reserve. */
	assert(!hpdata->h_in_psset || hpdata->h_updating);

	assert(purge_state->npurged == fb_scount(purge_state->to_purge,
	    HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES));

	fb_bit_not(purge_state->to_purge, purge_state->to_purge,
	    HUGEPAGE_PAGES);
	fb_bit_and(hpdata->touched_pages, hpdata->touched_pages,
	    purge_state->to_purge, HUGEPAGE_PAGES);
	assert(hpdata->h_ntouched >= purge_state->npurged);
	hpdata->h_ntouched -= purge_state->npurged;

	hpdata_assert_consistent(hpdata);
}

void
hpdata_hugify(hpdata_t *hpdata) {
	hpdata_assert_consistent(hpdata);
	hpdata->h_huge = true;
	fb_set_range(hpdata->touched_pages, HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES);
	hpdata->h_ntouched = HUGEPAGE_PAGES;
	hpdata_assert_consistent(hpdata);
}

void
hpdata_dehugify(hpdata_t *hpdata) {
	hpdata_assert_consistent(hpdata);
	hpdata->h_huge = false;
	hpdata_assert_consistent(hpdata);
}
