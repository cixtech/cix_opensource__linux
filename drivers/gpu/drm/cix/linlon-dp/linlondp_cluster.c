// SPDX-License-Identifier: GPL-2.0
/*
 * CIX linlon display cluster — one DRM card for multiple DPU platform devices.
 *
 * Device tree:
 *   dpu_display_cluster {
 *     compatible = "cix,linlon-display-cluster";
 *     dpus = <&dpu0 &dpu1 ...>;
 *   };
 *   &dpu0 { cix,linlon-dpu-slave; };
 *   (repeat for each DPU in the cluster)
 *
 * SMMU: Each DPU node keeps its own `iommus = <&smmu_mmhub stream-id>, ...`.
 * GEM is still allocated on the first DPU's `struct device` (drm dev parent).
 * `linlondp_kms.c` mirrors each buffer's IOVA→PA mapping into every other
 * cluster DPU's IOMMU domain so scanout on dpu1+ does not fault (AXIE).
 * Per-DPU TBU `connect_iommu` still runs on resume.
 *
 * `device-id` in DT is the driver's logical id (DSM/debugfs), not the SMMU SID.
 */
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_module.h>

#include "linlondp_dev.h"
#include "linlondp_kms.h"

extern bool enable_fb;

struct linlondp_cluster {
	struct linlondp_kms_dev *kms;
	struct linlondp_dev *mdevs[LINLONDP_MAX_CLUSTER_DPUS];
	unsigned int n_mdevs;
};

static int linlondp_cluster_bind(struct device *dev)
{
	struct linlondp_cluster *cl = dev_get_drvdata(dev);
	struct device *d0 = cl->mdevs[0]->dev;
	unsigned int i;
	int err;

	/*
	 * Root cause (cluster, multi-DPU):
	 * - Each CRTC pipeline sits on one linlondp_dev; linlondp_crtc_atomic_enable()
	 *   uses that mdev’s struct device for pm_runtime_get_sync() and (when CONFIG_PM)
	 *   requires pm_runtime_enabled(mdev->dev) before proceeding.
	 * - Original code only pm_runtime_enable(d0). For dpu1+ the flag never became
	 *   true, so atomic_enable aborted before get_sync/enable_irq/vblank_on → no
	 *   vsync / flip completion (often misread as “IRQ broken on dpu1”).
	 * - Fix: enable runtime on every cluster member here. Do not patch this in
	 *   atomic_enable (unbalanced vs bind). After replacing linlon-dp.ko, run
	 *   update-initramfs on the target or the initrd may still load an old module.
	 */
	for (i = 0; i < cl->n_mdevs; i++) {
		pm_runtime_enable(cl->mdevs[i]->dev);
		dev_notice(dev,
			   "linlon-cluster: pm_runtime_enable DPU%u %s\n", i,
			   dev_name(cl->mdevs[i]->dev));
	}

	cl->kms = linlondp_kms_attach_cluster(dev, cl->mdevs, cl->n_mdevs);
	if (IS_ERR(cl->kms)) {
		err = PTR_ERR(cl->kms);
		cl->kms = NULL;
		for (i = 0; i < cl->n_mdevs; i++) {
			if (pm_runtime_enabled(cl->mdevs[i]->dev))
				pm_runtime_disable(cl->mdevs[i]->dev);
		}
		return err;
	}

	if (enable_fb)
		drm_fbdev_generic_setup(&cl->kms->base, 32);

	if (cl->mdevs[0]->enabled_by_gop)
		pm_runtime_set_active(d0);

	return 0;
}

static void linlondp_cluster_unbind(struct device *dev)
{
	struct linlondp_cluster *cl = dev_get_drvdata(dev);
	unsigned int i;

	if (!cl || !cl->kms)
		return;

	linlondp_kms_detach(cl->kms);
	cl->kms = NULL;

	for (i = 0; i < cl->n_mdevs; i++) {
		if (pm_runtime_enabled(cl->mdevs[i]->dev))
			pm_runtime_disable(cl->mdevs[i]->dev);
	}
}

static const struct component_master_ops linlondp_cluster_master_ops = {
	.bind = linlondp_cluster_bind,
	.unbind = linlondp_cluster_unbind,
};

static int linlondp_cluster_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct linlondp_cluster *cl;
	struct component_match *match = NULL;
	struct device_node *np = dev->of_node;
	int n, i, err;

	if (!np)
		return -ENODEV;

	n = of_count_phandle_with_args(np, "dpus", NULL);
	if (n <= 0)
		return dev_err_probe(dev, -EINVAL, "missing or empty dpus\n");

	if ((unsigned int)n > LINLONDP_MAX_CLUSTER_DPUS)
		return dev_err_probe(dev, -EINVAL, "too many dpus (%d)\n", n);

	cl = devm_kzalloc(dev, sizeof(*cl), GFP_KERNEL);
	if (!cl)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		struct device_node *dpu_np = of_parse_phandle(np, "dpus", i);
		struct platform_device *dpu_pdev;
		struct linlondp_dev *mdev;

		if (!dpu_np)
			return -EINVAL;

		dpu_pdev = of_find_device_by_node(dpu_np);
		of_node_put(dpu_np);
		if (!dpu_pdev)
			return dev_err_probe(dev, -EPROBE_DEFER, "dpu %d not ready\n", i);

		mdev = platform_get_drvdata(dpu_pdev);
		put_device(&dpu_pdev->dev);
		if (!mdev)
			return dev_err_probe(dev, -EPROBE_DEFER, "dpu %d has no mdev\n", i);

		cl->mdevs[i] = mdev;
		err = linlondp_dpu_add_graph_slaves(dev, &match, mdev->dev);
		if (err)
			return err;
	}

	cl->n_mdevs = n;
	dev_set_drvdata(dev, cl);

	return component_master_add_with_match(dev, &linlondp_cluster_master_ops,
					       match);
}

static int linlondp_cluster_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &linlondp_cluster_master_ops);
	return 0;
}

static const struct of_device_id linlondp_cluster_of_match[] = {
	{ .compatible = "cix,linlon-display-cluster" },
	{},
};
MODULE_DEVICE_TABLE(of, linlondp_cluster_of_match);

struct platform_driver linlondp_cluster_driver = {
	.probe = linlondp_cluster_probe,
	.remove = linlondp_cluster_remove,
	.driver = {
		.name = "linlondp-cluster",
		.of_match_table = linlondp_cluster_of_match,
	},
};
