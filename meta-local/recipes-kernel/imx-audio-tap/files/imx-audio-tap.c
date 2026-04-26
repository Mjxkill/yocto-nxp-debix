// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * imx-audio-tap — kernel module exposing NPU tap shared memory to userspace.
 *
 * V3.2.2 NPU tap (i.MX8MP) :
 *   - DSP firmware SOF écrit le buffer audio post-effets dans une zone
 *     reserved-memory à NPU_TAP_PHYS_ADDR (0x942B0000, 256 KB no-map).
 *   - Ce module miscdevice expose la zone via /dev/imx-audio-tap (mmap).
 *   - Userspace mmap PROT_READ + write-combine, lit le ring buffer pour
 *     pousser les samples au NPU.
 *
 * A7 : runtime DT check — refuse le probe si l'adresse/taille du
 *      reserved-memory ne correspond pas exactement à la spec.
 * R2/R5 : NPU_TAP_PHYS_ADDR partagé via imx-audio-tap-uapi.h, validé au
 *        build par Yocto recipe + au runtime par ce module.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "imx-audio-tap-uapi.h"

#define DRV_NAME "imx-audio-tap"

struct imx_audio_tap {
	struct device *dev;
	struct miscdevice misc;
	phys_addr_t phys_addr;
	size_t size;
};

static int imx_audio_tap_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct imx_audio_tap *priv =
		container_of(misc, struct imx_audio_tap, misc);

	file->private_data = priv;
	return 0;
}

static int imx_audio_tap_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct imx_audio_tap *priv = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > priv->size) {
		dev_err(priv->dev, "mmap size %lu exceeds region size %zu\n",
			size, priv->size);
		return -EINVAL;
	}
	if (vma->vm_pgoff != 0) {
		dev_err(priv->dev, "mmap offset must be 0\n");
		return -EINVAL;
	}

	/*
	 * pgprot_writecombine : Normal Non-Cacheable on ARM64, ordering relaxed
	 * but guaranteed visibility from DSP writes (DSP cacheattr WT).
	 * R4 — userspace must use atomic_load_acquire + memory_order_acquire
	 * pattern when reading hdr->{magic,epoch,write_idx}.
	 */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	if (remap_pfn_range(vma, vma->vm_start,
			    priv->phys_addr >> PAGE_SHIFT,
			    size, vma->vm_page_prot)) {
		dev_err(priv->dev, "remap_pfn_range failed\n");
		return -EAGAIN;
	}
	return 0;
}

static const struct file_operations imx_audio_tap_fops = {
	.owner = THIS_MODULE,
	.open = imx_audio_tap_open,
	.mmap = imx_audio_tap_mmap,
	.llseek = no_llseek,
};

/* sysfs read-only attributes — readable from userspace for sanity checks */
static ssize_t phys_addr_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct imx_audio_tap *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%pa\n", &priv->phys_addr);
}
static DEVICE_ATTR_RO(phys_addr);

static ssize_t size_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct imx_audio_tap *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%zu\n", priv->size);
}
static DEVICE_ATTR_RO(size);

static struct attribute *imx_audio_tap_attrs[] = {
	&dev_attr_phys_addr.attr,
	&dev_attr_size.attr,
	NULL,
};
ATTRIBUTE_GROUPS(imx_audio_tap);

static int imx_audio_tap_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *mem_np;
	struct reserved_mem *rmem;
	struct imx_audio_tap *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	mem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_np) {
		dev_err(dev, "missing 'memory-region' phandle in DT\n");
		return -ENODEV;
	}
	rmem = of_reserved_mem_lookup(mem_np);
	of_node_put(mem_np);
	if (!rmem) {
		dev_err(dev, "of_reserved_mem_lookup failed\n");
		return -EINVAL;
	}

	/*
	 * A7 — runtime DT sanity check.
	 * R2/R5 — verify DT base/size match the values shared with SOF firmware
	 * (NPU_TAP_PHYS_ADDR / NPU_TAP_RING_SIZE in imx-audio-tap-uapi.h).
	 * Refuse to probe if mismatch — would corrupt audio or NPU data.
	 */
	if (rmem->base != NPU_TAP_PHYS_ADDR || rmem->size != NPU_TAP_RING_SIZE) {
		dev_err(dev,
			"DT mismatch: expected base=0x%x size=0x%x, got base=%pa size=%pa\n",
			NPU_TAP_PHYS_ADDR, NPU_TAP_RING_SIZE,
			&rmem->base, &rmem->size);
		return -EINVAL;
	}
	priv->phys_addr = rmem->base;
	priv->size = rmem->size;

	priv->misc.minor = MISC_DYNAMIC_MINOR;
	priv->misc.name = DRV_NAME;
	priv->misc.fops = &imx_audio_tap_fops;
	priv->misc.groups = imx_audio_tap_groups;

	ret = misc_register(&priv->misc);
	if (ret) {
		dev_err(dev, "misc_register failed: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);
	dev_info(dev,
		 "registered: /dev/%s phys=%pa size=%zu (NPU_TAP V3.2.2)\n",
		 priv->misc.name, &priv->phys_addr, priv->size);
	return 0;
}

static int imx_audio_tap_remove(struct platform_device *pdev)
{
	struct imx_audio_tap *priv = platform_get_drvdata(pdev);

	misc_deregister(&priv->misc);
	return 0;
}

static const struct of_device_id imx_audio_tap_of_match[] = {
	{ .compatible = "electrosens,imx-audio-tap" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx_audio_tap_of_match);

static struct platform_driver imx_audio_tap_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = imx_audio_tap_of_match,
	},
	.probe = imx_audio_tap_probe,
	.remove = imx_audio_tap_remove,
};
module_platform_driver(imx_audio_tap_driver);

MODULE_AUTHOR("Electrosens / Michael Jouannigot");
MODULE_DESCRIPTION("i.MX8MP NPU audio tap — expose SOF DSP shared mem to userspace");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("3.2.2");
