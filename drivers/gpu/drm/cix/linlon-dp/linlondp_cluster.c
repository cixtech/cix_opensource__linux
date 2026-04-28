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
 * ACPI (cluster device _HID CIXH50C0; last 4 chars must be hex per ACPI EISA ID):
 *   Either list full ACPI paths of each DPU device in _DSD property
 *   "cix,dpu-acpi-paths" (comma-separated, e.g. "\\_SB.DPU0,\\_SB.DPU1"), or
 *   declare each DPU as a child ACPI device with _HID CIXH5010 under the
 *   cluster scope (enumeration order follows AML child order).
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
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/aperture.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/acpi.h>

#if IS_ENABLED(CONFIG_ACPI)
#include <acpi/acpi_bus.h>
#endif

#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_module.h>
#include <drm/drm_modeset_helper.h>

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

	/* Remove existing drivers that may own the framebuffer memory. */
	err = aperture_remove_all_conflicting_devices("linlondp");
	if (err) {
		DRM_DEV_ERROR(dev, "Failed to remove existing framebuffers - %d.\n",err);
		return err;
	}

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

static const struct acpi_device_id linlondp_cluster_acpi_match[] = {
	{ "CIXH50C0", 0 },
	{},
};
MODULE_DEVICE_TABLE(acpi, linlondp_cluster_acpi_match);

#if IS_ENABLED(CONFIG_ACPI)
struct linlondp_cluster_acpi_ctx {
	struct device *cluster_dev;
	struct linlondp_cluster *cl;
	struct component_match **match;
	unsigned int idx;
};

static int linlondp_cluster_acpi_child_cb(struct acpi_device *child, void *data)
{
	struct linlondp_cluster_acpi_ctx *ctx = data;
	struct linlondp_dev *mdev;
	struct device *phys;
	int err;

	if (!acpi_dev_hid_uid_match(child, "CIXH5010", NULL))
		return 0;

	if (ctx->idx >= LINLONDP_MAX_CLUSTER_DPUS)
		return -EINVAL;

	phys = acpi_get_first_physical_node(child);
	if (!phys)
		return -EPROBE_DEFER;

	if (!dev_is_platform(phys)) {
		put_device(phys);
		return -EINVAL;
	}

	mdev = platform_get_drvdata(to_platform_device(phys));
	put_device(phys);
	if (!mdev)
		return -EPROBE_DEFER;

	ctx->cl->mdevs[ctx->idx] = mdev;
	err = linlondp_dpu_add_graph_slaves(ctx->cluster_dev, ctx->match, mdev->dev);
	if (err)
		return err;
	ctx->idx++;
	return 0;
}

/* Returns DPU count, -ENOENT if property absent, else error. */
static int linlondp_cluster_collect_dpus_acpi_paths(struct device *dev,
						    struct linlondp_cluster *cl,
						    struct component_match **match)
{
	const char *buf;
	char *kbuf, *p, *token;
	int n = 0, err;
	acpi_handle h;
	struct acpi_device *adev;
	struct device *phys;

	if (!device_property_present(dev, "cix,dpu-acpi-paths"))
		return -ENOENT;

	if (device_property_read_string(dev, "cix,dpu-acpi-paths", &buf))
		return -EINVAL;

	kbuf = kstrdup(buf, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	p = kbuf;
	while ((token = strsep(&p, ","))) {
		while (*token == ' ' || *token == '\t')
			token++;
		if (!*token)
			continue;

		if (n >= LINLONDP_MAX_CLUSTER_DPUS) {
			kfree(kbuf);
			return -EINVAL;
		}

		if (ACPI_FAILURE(acpi_get_handle(ACPI_ROOT_OBJECT, token, &h))) {
			dev_err(dev, "linlon-cluster: bad ACPI path \"%s\"\n", token);
			kfree(kbuf);
			return -EINVAL;
		}

		adev = acpi_fetch_acpi_dev(h);
		if (!adev) {
			kfree(kbuf);
			return -EINVAL;
		}

		phys = acpi_get_first_physical_node(adev);
		if (!phys) {
			kfree(kbuf);
			return -EPROBE_DEFER;
		}

		if (!dev_is_platform(phys)) {
			put_device(phys);
			kfree(kbuf);
			return -EINVAL;
		}

		cl->mdevs[n] = platform_get_drvdata(to_platform_device(phys));
		put_device(phys);
		if (!cl->mdevs[n]) {
			kfree(kbuf);
			return -EPROBE_DEFER;
		}

		err = linlondp_dpu_add_graph_slaves(dev, match, cl->mdevs[n]->dev);
		if (err) {
			kfree(kbuf);
			return err;
		}
		n++;
	}

	kfree(kbuf);
	if (!n)
		return -EINVAL;
	return n;
}

static int linlondp_cluster_collect_dpus_acpi_children(struct acpi_device *parent,
						       struct device *cluster_dev,
						       struct linlondp_cluster *cl,
						       struct component_match **match)
{
	struct linlondp_cluster_acpi_ctx ctx = {
		.cluster_dev = cluster_dev,
		.cl = cl,
		.match = match,
		.idx = 0,
	};
	int ret;

	ret = acpi_dev_for_each_child(parent, linlondp_cluster_acpi_child_cb, &ctx);
	if (ret)
		return ret;
	if (!ctx.idx)
		return -ENOENT;
	return ctx.idx;
}

static int linlondp_cluster_probe_acpi(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct acpi_device *cluster_adev = ACPI_COMPANION(dev);
	struct linlondp_cluster *cl;
	struct component_match *match = NULL;
	const char *hid;
	int n, err;

	if (!cluster_adev)
		return -ENODEV;

	cl = devm_kzalloc(dev, sizeof(*cl), GFP_KERNEL);
	if (!cl)
		return -ENOMEM;

	hid = acpi_device_hid(cluster_adev);
	dev_info(dev,
		 "linlon-cluster: ACPI cluster HID %s (%s) — match CIXH50C0 / cix,dpu-acpi-paths or child CIXH5010\n",
		 hid ? hid : "?", dev_name(dev));

	n = linlondp_cluster_collect_dpus_acpi_paths(dev, cl, &match);
	if (n == -ENOENT)
		n = linlondp_cluster_collect_dpus_acpi_children(cluster_adev, dev,
								cl, &match);
	if (n < 0) {
		if (n == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		return dev_err_probe(dev, n == -ENOENT ? -EINVAL : n,
				     "ACPI cluster: no DPUs (paths or CIXH5010 children)\n");
	}

	cl->n_mdevs = n;
	dev_set_drvdata(dev, cl);

	dev_info(dev,
		 "linlon-cluster: registered %u DPU(s) for ACPI cluster %s, component master\n",
		 cl->n_mdevs, hid ? hid : "?");

	err = component_master_add_with_match(dev, &linlondp_cluster_master_ops,
					      match);
	if (err)
		dev_set_drvdata(dev, NULL);
	return err;
}
#endif /* CONFIG_ACPI */

static int linlondp_cluster_probe_of(struct platform_device *pdev)
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

static int linlondp_cluster_probe(struct platform_device *pdev)
{
#if IS_ENABLED(CONFIG_ACPI)
	const struct acpi_device_id *aid;

	aid = acpi_match_device(linlondp_cluster_acpi_match, &pdev->dev);
	if (aid)
		return linlondp_cluster_probe_acpi(pdev);
#endif
	if (pdev->dev.of_node)
		return linlondp_cluster_probe_of(pdev);
	return -ENODEV;
}

static int linlondp_cluster_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &linlondp_cluster_master_ops);
	return 0;
}

#ifdef CONFIG_PM
/* Cluster owns KMS; per-DPU PM uses direct_mdev and skips modeset suspend. */
static int linlondp_cluster_pm_prepare(struct device *dev)
{
	struct linlondp_cluster *cl = dev_get_drvdata(dev);

	if (!cl || !cl->kms)
		return 0;

	return drm_mode_config_helper_suspend(&cl->kms->base);
}

static int linlondp_cluster_resume_modeset(struct device *dev)
{
	struct linlondp_cluster *cl = dev_get_drvdata(dev);

	if (!cl || !cl->kms)
		return 0;

	return drm_mode_config_helper_resume(&cl->kms->base);
}

static void linlondp_cluster_pm_complete(struct device *dev)
{
	linlondp_cluster_resume_modeset(dev);
}

static const struct dev_pm_ops linlondp_cluster_pm_ops = {
	.prepare = linlondp_cluster_pm_prepare,
	.complete = linlondp_cluster_pm_complete,
	.freeze = linlondp_cluster_pm_prepare,
	.thaw = linlondp_cluster_resume_modeset,
	.poweroff = linlondp_cluster_pm_prepare,
	.restore = linlondp_cluster_resume_modeset,
};
#endif

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
		.acpi_match_table = ACPI_PTR(linlondp_cluster_acpi_match),
#ifdef CONFIG_PM
		.pm = &linlondp_cluster_pm_ops,
#endif
	},
};
