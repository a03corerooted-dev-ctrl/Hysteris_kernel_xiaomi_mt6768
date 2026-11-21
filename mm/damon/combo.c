// SPDX-License-Identifier: GPL-2.0
/*
 * Combined DAMON reclaim and split-LRU policy for Hysteris 4.19.
 *
 * This policy runs pageout, hot-page promotion, and cold-page deactivation
 * as schemes of one DAMON context.  A shared PADDR monitor avoids competing
 * access-bit sampling by independent DAMON_RECLAIM and DAMON_LRU_SORT
 * workers.
 *
 * The reclaim and LRU policy logic is based on DAMON_RECLAIM and
 * DAMON_LRU_SORT by SeongJae Park <sj@kernel.org>.
 */

#define pr_fmt(fmt) "damon-combo: " fmt

#include <linux/damon.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "modules-common.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_combo."

static bool enabled __read_mostly;
static bool commit_inputs __read_mostly;
module_param(commit_inputs, bool, 0600);

static unsigned long min_age __read_mostly = 120000000;
module_param(min_age, ulong, 0600);
static unsigned long cold_min_age __read_mostly = 120000000;
module_param(cold_min_age, ulong, 0600);
static unsigned long hot_thres_access_freq __read_mostly = 500;
module_param(hot_thres_access_freq, ulong, 0600);
static bool skip_anon __read_mostly;
module_param(skip_anon, bool, 0600);

static unsigned long monitor_region_start __read_mostly;
module_param(monitor_region_start, ulong, 0600);
static unsigned long monitor_region_end __read_mostly;
module_param(monitor_region_end, ulong, 0600);

static unsigned long quota_mem_pressure_us __read_mostly;
module_param(quota_mem_pressure_us, ulong, 0600);
static unsigned long quota_autotune_feedback __read_mostly;
module_param(quota_autotune_feedback, ulong, 0600);

static struct damon_attrs damon_combo_attrs = {
	.sample_interval = 5000,
	.aggr_interval = 100000,
	.ops_update_interval = 0,
	.min_nr_regions = 10,
	.max_nr_regions = 1000,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_combo_attrs);

/*
 * Keep the aggregate policy time budget near the 10 ms/s used by the
 * individual upstream policies: 3 ms for each of the three schemes.
 */
static struct damos_quota reclaim_quota = {
	.ms = 3,
	.sz = 128 * 1024 * 1024,
	.reset_interval = 1000,
	.weight_age = 1,
};
static struct damos_quota lru_quota = {
	.ms = 3,
	.reset_interval = 1000,
	.weight_nr_accesses = 1,
};
static struct damos_watermarks reclaim_wmarks = {
	.metric = DAMOS_WMARK_FREE_MEM_RATE,
	.interval = 5000000,
	.high = 500,
	.mid = 400,
	.low = 200,
};
static struct damos_watermarks lru_wmarks = {
	.metric = DAMOS_WMARK_FREE_MEM_RATE,
	.interval = 5000000,
	.high = 200,
	.mid = 150,
	.low = 50,
};

static struct damos_stat reclaim_stat;
static struct damos_stat hot_stat;
static struct damos_stat cold_stat;
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(reclaim_stat,
		combo_reclaim_tried, combo_reclaimed, combo_reclaim_quota_exceeds);
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(hot_stat,
		combo_hot_tried, combo_hot_sorted, combo_hot_quota_exceeds);
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(cold_stat,
		combo_cold_tried, combo_cold_sorted, combo_cold_quota_exceeds);

static struct damon_ctx *ctx;
static struct damon_target *target;
static int kdamond_pid __read_mostly = -1;
module_param(kdamond_pid, int, 0400);

static struct damos *damon_combo_new_scheme(struct damos_access_pattern *p,
		enum damos_action action, struct damos_quota *quota,
		struct damos_watermarks *wmarks)
{
	return damon_new_scheme(p, action, 0, quota, wmarks, NUMA_NO_NODE);
}

static int damon_combo_apply_parameters(void)
{
	struct damon_ctx *param_ctx;
	struct damon_target *param_target;
	struct damos_access_pattern reclaim_pattern = {
		.min_sz_region = PAGE_SIZE,
		.max_sz_region = ULONG_MAX,
		.min_nr_accesses = 0,
		.max_nr_accesses = 0,
		.min_age_region = 0,
		.max_age_region = UINT_MAX,
	};
	struct damos_access_pattern hot_pattern = {
		.min_sz_region = PAGE_SIZE,
		.max_sz_region = ULONG_MAX,
		.min_nr_accesses = 0,
		.max_nr_accesses = UINT_MAX,
		.min_age_region = 0,
		.max_age_region = UINT_MAX,
	};
	struct damos_access_pattern cold_pattern = {
		.min_sz_region = PAGE_SIZE,
		.max_sz_region = ULONG_MAX,
		.min_nr_accesses = 0,
		.max_nr_accesses = 0,
		.min_age_region = 0,
		.max_age_region = UINT_MAX,
	};
	struct damos *schemes[3];
	struct damos_quota_goal *goal;
	struct damos_filter *filter;
	int err;

	err = damon_modules_new_paddr_ctx_target(&param_ctx, &param_target);
	if (err)
		return err;

	if (!damon_combo_attrs.sample_interval ||
			!damon_combo_attrs.aggr_interval ||
			hot_thres_access_freq > 1000) {
		err = -EINVAL;
		goto out;
	}
	err = damon_set_attrs(param_ctx, &damon_combo_attrs);
	if (err)
		goto out;

	reclaim_pattern.min_age_region = min_age /
		damon_combo_attrs.aggr_interval;
	hot_pattern.min_nr_accesses = damon_max_nr_accesses(
			&damon_combo_attrs) * hot_thres_access_freq / 1000;
	cold_pattern.min_age_region = cold_min_age /
		damon_combo_attrs.aggr_interval;

	schemes[0] = damon_combo_new_scheme(&reclaim_pattern, DAMOS_PAGEOUT,
			&reclaim_quota, &reclaim_wmarks);
	if (!schemes[0]) {
		err = -ENOMEM;
		goto out;
	}
	schemes[1] = damon_combo_new_scheme(&hot_pattern, DAMOS_LRU_PRIO,
			&lru_quota, &lru_wmarks);
	if (!schemes[1]) {
		damon_destroy_scheme(schemes[0]);
		err = -ENOMEM;
		goto out;
	}
	schemes[2] = damon_combo_new_scheme(&cold_pattern, DAMOS_LRU_DEPRIO,
			&lru_quota, &lru_wmarks);
	if (!schemes[2]) {
		damon_destroy_scheme(schemes[0]);
		damon_destroy_scheme(schemes[1]);
		err = -ENOMEM;
		goto out;
	}

	if (quota_mem_pressure_us) {
		goal = damos_new_quota_goal(DAMOS_QUOTA_SOME_MEM_PSI_US,
				quota_mem_pressure_us);
		if (!goal) {
			err = -ENOMEM;
			goto destroy_schemes;
		}
		damos_add_quota_goal(&schemes[0]->quota, goal);
	}
	if (quota_autotune_feedback) {
		goal = damos_new_quota_goal(DAMOS_QUOTA_USER_INPUT, 10000);
		if (!goal) {
			err = -ENOMEM;
			goto destroy_schemes;
		}
		goal->current_value = quota_autotune_feedback;
		damos_add_quota_goal(&schemes[0]->quota, goal);
	}
	if (skip_anon) {
		filter = damos_new_filter(DAMOS_FILTER_TYPE_ANON, true, false);
		if (!filter) {
			err = -ENOMEM;
			goto destroy_schemes;
		}
		damos_add_filter(schemes[0], filter);
	}

	damon_set_schemes(param_ctx, schemes, ARRAY_SIZE(schemes));
	err = damon_set_region_biggest_system_ram_default(param_target,
			&monitor_region_start, &monitor_region_end);
	if (err)
		goto out;
	err = damon_commit_ctx(ctx, param_ctx);
	goto out;

destroy_schemes:
	damon_destroy_scheme(schemes[0]);
	damon_destroy_scheme(schemes[1]);
	damon_destroy_scheme(schemes[2]);
out:
	damon_destroy_ctx(param_ctx);
	return err;
}

static int damon_combo_turn(bool on)
{
	int err;

	if (!on) {
		err = damon_stop(&ctx, 1);
		if (!err)
			kdamond_pid = -1;
		return err;
	}
	err = damon_combo_apply_parameters();
	if (err)
		return err;
	err = damon_start(&ctx, 1, true);
	if (!err)
		kdamond_pid = ctx->kdamond->pid;
	return err;
}

static int damon_combo_enabled_store(const char *val,
		const struct kernel_param *kp)
{
	bool enable;
	int err = kstrtobool(val, &enable);

	if (err || enabled == enable)
		return err;
	if (!ctx) {
		enabled = enable;
		return 0;
	}
	err = damon_combo_turn(enable);
	if (!err)
		enabled = enable;
	return err;
}

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_combo_enabled_store,
	.get = param_get_bool,
};
module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled, "Enable combined DAMON reclaim/LRU policy");

static int damon_combo_handle_commit_inputs(void)
{
	int err;

	if (!commit_inputs)
		return 0;
	err = damon_combo_apply_parameters();
	commit_inputs = false;
	return err;
}

static int damon_combo_after_aggregation(struct damon_ctx *c)
{
	struct damos *s;

	damon_for_each_scheme(s, c) {
		if (s->action == DAMOS_PAGEOUT)
			reclaim_stat = s->stat;
		else if (s->action == DAMOS_LRU_PRIO)
			hot_stat = s->stat;
		else if (s->action == DAMOS_LRU_DEPRIO)
			cold_stat = s->stat;
	}
	return damon_combo_handle_commit_inputs();
}

static int damon_combo_after_wmarks_check(struct damon_ctx *c)
{
	return damon_combo_handle_commit_inputs();
}

static int __init damon_combo_init(void)
{
	int err = damon_modules_new_paddr_ctx_target(&ctx, &target);

	if (err)
		return err;
	ctx->callback.after_wmarks_check = damon_combo_after_wmarks_check;
	ctx->callback.after_aggregation = damon_combo_after_aggregation;
	if (enabled)
		err = damon_combo_turn(true);
	return err;
}

module_init(damon_combo_init);

MODULE_DESCRIPTION("Combined DAMON reclaim and LRU policy");
MODULE_LICENSE("GPL");
