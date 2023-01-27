#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/bit_util.h"

void
cache_bin_info_init(cache_bin_info_t *info,
    cache_bin_sz_t ncached_max) {
	size_t stack_size = (size_t)ncached_max * sizeof(void *);
	assert(stack_size < ((size_t)1 << (sizeof(cache_bin_sz_t) * 8)));
	info->stack_size = (cache_bin_sz_t)stack_size;
}

void
cache_bin_info_compute_alloc(cache_bin_info_t *infos, szind_t ninfos,
    size_t *size, size_t *alignment) {
	/* For the total bin stack region (per tcache), reserve 2 more slots so
	 * that
	 * 1) the empty position can be safely read on the fast path before
	 *    checking "is_empty"; and
	 * 2) the cur_ptr can go beyond the empty position by 1 step safely on
	 * the fast path (i.e. no overflow).
	 */
	*size = sizeof(void *) * 2;
	for (szind_t i = 0; i < ninfos; i++) {
		*size += infos[i].stack_size;
	}

	/*
	 * 1) Align to at least PAGE, to minimize the # of TLBs needed by the
	 * smaller sizes; also helps if the larger sizes don't get used at all.
	 * 2) On 32-bit the pointers won't be compressed; use minimal alignment.
	 */
	if (LG_SIZEOF_PTR < 3 || *size < PAGE) {
		*alignment = PAGE;
	} else {
		/*
		 * Align pow2 to avoid overflow the cache bin compressed
		 * pointers.
		 */
		*alignment = pow2_ceil_zu(*size);
	}
}

void
cache_bin_preincrement(cache_bin_info_t *infos, szind_t ninfos, void *alloc,
    size_t *cur_offset) {
	if (config_debug) {
		size_t computed_size;
		size_t computed_alignment;

		/* Pointer should be as aligned as we asked for. */
		cache_bin_info_compute_alloc(infos, ninfos, &computed_size,
		    &computed_alignment);
		assert(((uintptr_t)alloc & (computed_alignment - 1)) == 0);

		/* And that alignment should disallow overflow. */
		uint32_t lowbits = (uint32_t)((uintptr_t)alloc + computed_size);
		assert((uint32_t)(uintptr_t)alloc < lowbits);
	}
	/*
	 * Leave a noticeable mark pattern on the boundaries, in case a bug
	 * starts leaking those.  Make it look like the junk pattern but be
	 * distinct from it.
	 */
	uintptr_t preceding_ptr_junk = (uintptr_t)0x7a7a7a7a7a7a7a7aULL;
	*(uintptr_t *)((uintptr_t)alloc + *cur_offset) = preceding_ptr_junk;
	*cur_offset += sizeof(void *);
}

void
cache_bin_postincrement(cache_bin_info_t *infos, szind_t ninfos, void *alloc,
    size_t *cur_offset) {
	/* Note: a7 vs. 7a above -- this tells you which pointer leaked. */
	uintptr_t trailing_ptr_junk = (uintptr_t)0xa7a7a7a7a7a7a7a7ULL;
	*(uintptr_t *)((uintptr_t)alloc + *cur_offset) = trailing_ptr_junk;
	*cur_offset += sizeof(void *);
}


void
cache_bin_init(cache_bin_t *bin, cache_bin_info_t *info, void *alloc,
    size_t *cur_offset) {
	assert(sizeof(bin->cur_ptr) == sizeof(void *));
	/*
	 * The full_position points to the lowest available space.  Allocations
	 * will access the slots toward higher addresses (for the benefit of
	 * adjacent prefetch).
	 */
	void *stack_cur = (void *)((uintptr_t)alloc + *cur_offset);
	void *full_position = stack_cur;
	uint32_t bin_stack_size = info->stack_size;

	*cur_offset += bin_stack_size;
	void *empty_position = (void *)((uintptr_t)alloc + *cur_offset);

	/* Init to the empty position. */
	bin->cur_ptr.ptr = empty_position;
	bin->low_water_position = bin->cur_ptr.lowbits;
	bin->full_position = (uint32_t)(uintptr_t)full_position;
	assert(bin->cur_ptr.lowbits - bin->full_position == bin_stack_size);
	assert(cache_bin_ncached_get(bin, info) == 0);
	assert(cache_bin_empty_position_get(bin, info) == empty_position);
}
