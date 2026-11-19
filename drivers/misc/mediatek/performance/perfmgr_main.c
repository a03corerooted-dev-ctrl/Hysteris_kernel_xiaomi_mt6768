// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "boost_ctrl.h"
#include "mtk_perfmgr_internal.h"
#include "topo_ctrl.h"

static int perfmgr_probe(struct platform_device *pdev)
{
	return 0;
}

static struct platform_device *perfmgr_device;

static int perfmgr_suspend(struct device *dev)
{
#ifdef CONFIG_MTK_PERFMGR_TOUCH_BOOST
	ktch_suspend();
#endif
	return 0;
}

static int perfmgr_resume(struct device *dev)
{
	return 0;
}

static int perfmgr_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver perfmgr_driver = {
	.probe = perfmgr_probe,
	.remove = perfmgr_remove,
	.driver = {
		.name = "perfmgr",
		.pm = &(const struct dev_pm_ops) {
			.suspend = perfmgr_suspend,
			.resume = perfmgr_resume,
		},
	},
};

static int __init init_perfmgr(void)
{
	struct proc_dir_entry *perfmgr_root;
	int ret;

	ret = platform_driver_register(&perfmgr_driver);
	if (ret)
		return ret;

	perfmgr_device = platform_device_register_simple("perfmgr", -1,
						 NULL, 0);
	if (IS_ERR(perfmgr_device)) {
		ret = PTR_ERR(perfmgr_device);
		goto err_driver;
	}

	perfmgr_root = proc_mkdir("perfmgr", NULL);
	if (!perfmgr_root) {
		ret = -ENOMEM;
		goto err_driver;
	}

	ret = init_boostctrl(perfmgr_root);
	if (ret) {
		remove_proc_subtree("perfmgr", NULL);
		goto err_driver;
	}

	pr_info("MediaTek perfmgr initialized\n");
	return 0;

err_device:
	platform_device_unregister(perfmgr_device);
err_driver:
	platform_driver_unregister(&perfmgr_driver);
	return ret;
}
device_initcall(init_perfmgr);
