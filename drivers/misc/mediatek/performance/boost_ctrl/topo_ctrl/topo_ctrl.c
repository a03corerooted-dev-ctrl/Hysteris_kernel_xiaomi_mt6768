// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#define pr_fmt(fmt) "[topo_ctrl] " fmt

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <sched/sched.h>

#include "topo_ctrl.h"
#include "mtk_perfmgr_internal.h"

struct topo_plat_cluster_info {
	unsigned int core_num;
	struct cpumask cpus;
};

int topo_cluster_num;

static int *calc_cpu_cap;
static int *calc_cpu_num;
static int s_clstr_core = -1;
static int b_clstr_core = -1;
static struct topo_plat_cluster_info *topo_cluster_info;

int topo_ctrl_get_nr_clusters(void)
{
	return topo_cluster_num;
}
EXPORT_SYMBOL(topo_ctrl_get_nr_clusters);

static void topo_ctrl_free_data(void)
{
	kfree(topo_cluster_info);
	kfree(calc_cpu_cap);
	kfree(calc_cpu_num);

	topo_cluster_info = NULL;
	calc_cpu_cap = NULL;
	calc_cpu_num = NULL;
	topo_cluster_num = 0;
	s_clstr_core = -1;
	b_clstr_core = -1;
}

/*
 * Build cluster masks from the actual CPU IDs reported by the architecture.
 * Do not assume that CPU IDs are contiguous or ordered by cluster.
 */
static int topo_platform_init(void)
{
	int cpu;
	int cluster;

	topo_cluster_num = arch_nr_clusters();
	if (topo_cluster_num <= 0)
		return -ENODEV;

	topo_cluster_info = kcalloc(topo_cluster_num,
				     sizeof(*topo_cluster_info), GFP_KERNEL);
	if (!topo_cluster_info)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		cluster = arch_cpu_cluster_id(cpu);
		if (cluster < 0 || cluster >= topo_cluster_num) {
			pr_warn("ignoring CPU%d with invalid cluster id %d\n",
				cpu, cluster);
			continue;
		}

		cpumask_set_cpu(cpu, &topo_cluster_info[cluster].cpus);
		topo_cluster_info[cluster].core_num++;
	}

	for (cluster = 0; cluster < topo_cluster_num; cluster++) {
		if (!topo_cluster_info[cluster].core_num)
			pr_warn("cluster%d has no possible CPUs\n", cluster);
	}

	return 0;
}

/*
 * Capacity is static for a given topology, while online CPU counts are
 * collected at read time so hotplug state is never presented as stale data.
 */
static int calc_min_cpucap(void)
{
	unsigned long min_cap = ULONG_MAX;
	unsigned long max_cap = 0;
	unsigned long cap;
	int cpu;
	int cluster;
	bool found = false;

	for (cluster = 0; cluster < topo_cluster_num; cluster++) {
		calc_cpu_num[cluster] = topo_cluster_info[cluster].core_num;
		calc_cpu_cap[cluster] = 0;

		for_each_cpu(cpu, &topo_cluster_info[cluster].cpus) {
			cap = capacity_orig_of(cpu);
			if (cap > (unsigned long)calc_cpu_cap[cluster])
				calc_cpu_cap[cluster] = (int)cap;
		}

		if (!topo_cluster_info[cluster].core_num)
			continue;

		found = true;
		cap = (unsigned long)calc_cpu_cap[cluster];
		if (cap < min_cap) {
			min_cap = cap;
			s_clstr_core = cluster;
		}
		if (cap > max_cap) {
			max_cap = cap;
			b_clstr_core = cluster;
		}
	}

	return found ? 0 : -ENODEV;
}

int get_min_clstr_cap(void)
{
	return s_clstr_core;
}

int get_max_clstr_cap(void)
{
	return b_clstr_core;
}

static int perfmgr_glb_info_proc_show(struct seq_file *m, void *v)
{
	int cluster;
	struct cpumask online_cpus;

	seq_printf(m, "perfmgr_clusters: %d\n", topo_cluster_num);
	if (!topo_cluster_info || !calc_cpu_cap || !calc_cpu_num)
		return 0;

	for (cluster = 0; cluster < topo_cluster_num; cluster++) {
		cpumask_and(&online_cpus,
			    &topo_cluster_info[cluster].cpus,
			    cpu_online_mask);
		seq_printf(m,
			   "cluster%d_capacity: %d\n"
			   "cluster%d_possible_cpus: %d\n"
			   "cluster%d_online_cpus: %d\n",
			   cluster, calc_cpu_cap[cluster],
			   cluster, calc_cpu_num[cluster],
			   cluster, cpumask_weight(&online_cpus));
	}

	seq_printf(m, "small_cluster: %d\n", s_clstr_core);
	seq_printf(m, "big_cluster: %d\n", b_clstr_core);
	return 0;
}

static int perfmgr_topo_ctrl_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", topo_cluster_num);
	return 0;
}

static int perfmgr_is_big_little_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", topo_cluster_num > 1 ? 1 : 0);
	return 0;
}

static int perfmgr_nr_clusters_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", topo_cluster_num);
	return 0;
}

static int perfmgr_cpus_per_cluster_proc_show(struct seq_file *m, void *v)
{
	int cluster;

	if (!topo_cluster_info)
		return -ENODEV;

	for (cluster = 0; cluster < topo_cluster_num; cluster++)
		seq_printf(m, "cluster%d: %*pbl\n", cluster,
			   cpumask_pr_args(&topo_cluster_info[cluster].cpus));

	return 0;
}

static int perfmgr_online_cpus_per_cluster_proc_show(struct seq_file *m,
						     void *v)
{
	int cluster;
	struct cpumask online_cpus;

	if (!topo_cluster_info)
		return -ENODEV;

	for (cluster = 0; cluster < topo_cluster_num; cluster++) {
		cpumask_and(&online_cpus,
			    &topo_cluster_info[cluster].cpus,
			    cpu_online_mask);
		seq_printf(m, "cluster%d: %*pbl\n", cluster,
			   cpumask_pr_args(&online_cpus));
	}

	return 0;
}

PROC_FOPS_RO(topo_ctrl);
PROC_FOPS_RO(glb_info);
PROC_FOPS_RO(is_big_little);
PROC_FOPS_RO(nr_clusters);
PROC_FOPS_RO(cpus_per_cluster);
PROC_FOPS_RO(online_cpus_per_cluster);

int topo_ctrl_init(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *topo_dir;
	int ret;
	size_t i;

	struct pentry {
		const char *name;
		const struct file_operations *fops;
	};

	const struct pentry entries[] = {
		PROC_ENTRY(topo_ctrl),
		PROC_ENTRY(glb_info),
		PROC_ENTRY(is_big_little),
		PROC_ENTRY(nr_clusters),
		PROC_ENTRY(cpus_per_cluster),
		PROC_ENTRY(online_cpus_per_cluster),
	};

	if (!parent)
		return -EINVAL;

	topo_dir = proc_mkdir("topo_ctrl", parent);
	if (!topo_dir)
		return -ENOMEM;

	ret = topo_platform_init();
	if (ret)
		goto err;

	calc_cpu_cap = kcalloc(topo_cluster_num, sizeof(*calc_cpu_cap),
			       GFP_KERNEL);
	calc_cpu_num = kcalloc(topo_cluster_num, sizeof(*calc_cpu_num),
			       GFP_KERNEL);
	if (!calc_cpu_cap || !calc_cpu_num) {
		ret = -ENOMEM;
		goto err;
	}

	ret = calc_min_cpucap();
	if (ret)
		goto err;

	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		if (!proc_create(entries[i].name, 0444, topo_dir,
				 entries[i].fops)) {
			pr_err("failed to create /proc/perfmgr/boost_ctrl/topo_ctrl/%s\n",
			       entries[i].name);
			ret = -ENOMEM;
			goto err;
		}
	}

	return 0;

err:
	remove_proc_subtree("topo_ctrl", parent);
	topo_ctrl_free_data();
	return ret;
}

void topo_ctrl_exit(void)
{
	topo_ctrl_free_data();
}
