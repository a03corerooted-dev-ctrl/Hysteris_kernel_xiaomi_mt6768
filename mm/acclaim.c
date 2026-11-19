// SPDX-License-Identifier: GPL-2.0
/*
 * Acclaim lightweight prediction-based reclaim (LWP).
 *
 * Keep one sampled allocation size per 10ms slot in a lock-free ring.
 * kswapd compares two adjacent 100ms windows and scales its scan target
 * according to the observed allocation trend.
 */

#include <linux/acclaim.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/limits.h>

#define ACCLAIM_SAMPLE_NS		10000000ULL
#define ACCLAIM_SAMPLE_SLOTS		10
#define ACCLAIM_HISTORY_SLOTS		(ACCLAIM_SAMPLE_SLOTS * 2)
#define ACCLAIM_PAGE_BITS		16
#define ACCLAIM_PAGE_MASK		((1ULL << ACCLAIM_PAGE_BITS) - 1)

static atomic64_t acclaim_samples[ACCLAIM_HISTORY_SLOTS];

/*
 * Sample the first successful allocation in each 10ms slot. The timestamp
 * and allocation size are packed into one atomic value so readers never
 * observe a new timestamp with an old size. Racing samples may be dropped;
 * the predictor is advisory and does not participate in memory correctness.
 */
void acclaim_record_allocation(unsigned int order)
{
	u64 sample = ktime_get_ns() / ACCLAIM_SAMPLE_NS;
	u64 pages;
	u64 value;
	atomic64_t *slot;
	int attempt;

	if (order >= ACCLAIM_PAGE_BITS)
		pages = ACCLAIM_PAGE_MASK;
	else
		pages = 1ULL << order;

	value = (sample << ACCLAIM_PAGE_BITS) | pages;
	slot = &acclaim_samples[sample % ACCLAIM_HISTORY_SLOTS];

	/* A bounded retry keeps contention off the allocator's hot path. */
	for (attempt = 0; attempt < 2; attempt++) {
		u64 old = (u64)atomic64_read(slot);

		if ((old >> ACCLAIM_PAGE_BITS) >= sample)
			return;

		if ((u64)atomic64_cmpxchg(slot, (s64)old, (s64)value) == old)
			return;
	}
}

/*
 * Scale the default kswapd scan budget by the ratio between the latest
 * 100ms allocation window and the preceding 100ms window. Bound the result
 * by the aggregate low watermark and by one additional watermark gap.
 *
 * A missing previous window means that there is not enough history yet;
 * in that case leave the kernel's normal reclaim target unchanged.
 */
unsigned long acclaim_adjust_kswapd_reclaim(unsigned long high_target,
					    unsigned long low_target)
{
	u64 sample = ktime_get_ns() / ACCLAIM_SAMPLE_NS;
	u64 current_sum = 0;
	u64 previous_sum = 0;
	u64 scaled_target;
	unsigned long max_target;
	unsigned int i;

	if (low_target >= high_target ||
	    sample < ACCLAIM_HISTORY_SLOTS)
		return high_target;

	for (i = 0; i < ACCLAIM_HISTORY_SLOTS; i++) {
		u64 expected = sample - i;
		u64 entry = (u64)atomic64_read(
			&acclaim_samples[expected % ACCLAIM_HISTORY_SLOTS]);
		u64 entry_sample = entry >> ACCLAIM_PAGE_BITS;
		u64 pages = entry & ACCLAIM_PAGE_MASK;

		if (entry_sample != expected)
			continue;

		if (i < ACCLAIM_SAMPLE_SLOTS)
			current_sum += pages;
		else
			previous_sum += pages;
	}

	if (!previous_sum)
		return high_target;

	if (!current_sum)
		return low_target;

	if (high_target > ULONG_MAX - (high_target - low_target))
		max_target = ULONG_MAX;
	else
		max_target = high_target + (high_target - low_target);

	scaled_target = div64_u64((u64)high_target * current_sum,
			   previous_sum);

	return clamp_t(u64, scaled_target, low_target, max_target);
}
