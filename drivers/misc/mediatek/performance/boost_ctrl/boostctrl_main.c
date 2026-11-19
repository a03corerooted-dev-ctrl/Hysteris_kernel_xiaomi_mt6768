// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/errno.h>
#include <linux/proc_fs.h>

#include "boost_ctrl.h"

int init_boostctrl(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *boost_root;
	int ret;

	if (!parent)
		return -EINVAL;

	boost_root = proc_mkdir("boost_ctrl", parent);
	if (!boost_root)
		return -ENOMEM;

	ret = topo_ctrl_init(boost_root);
	if (ret)
		goto err;

	ret = dram_ctrl_init(boost_root);
	if (ret)
		goto err_topology;

	return 0;

err_topology:
	topo_ctrl_exit();
err:
	remove_proc_subtree("boost_ctrl", parent);
	return ret;
}
