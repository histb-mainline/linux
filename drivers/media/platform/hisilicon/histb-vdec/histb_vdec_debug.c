// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * debugfs window for the VDH bring-up measurements: the VDM and SCD register
 * windows, the CRG words behind the "clocks" phandle, and the message-pool
 * slots a job assembles.
 *
 * This platform maps /dev/mem only for System RAM, so a driver-side window is
 * the only way to sample these registers, and the vendor-vs-driver A/B tests
 * (SCD global registers, clock/reset bits, doorbell) need to poke them too.
 * Reading the VDM window requires the block clocked, so every dump holds a
 * runtime PM reference.
 */

#include <linux/debugfs.h>
#include <linux/mfd/syscon.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "histb_vdec.h"

#define HISTB_REG_WINDOW	0x1000

static int histb_vdec_regs_show(struct seq_file *s, void *unused)
{
	struct histb_vdec_dev *dev = s->private;
	unsigned int off;
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret)
		return ret;

	if (dev->crg) {
		regmap_read(dev->crg, VDH_CRG30, &val);
		seq_printf(s, "crg30=%08x", val);
		regmap_read(dev->crg, VDH_CRG93, &val);
		seq_printf(s, " crg93=%08x\n", val);
	}
	seq_printf(s, "scd avs_flag=%08x emar_id=%08x ck_gt=%08x\n",
		   readl(dev->scd + SCD_AVS_FLAG),
		   readl(dev->scd + SCD_EMAR_ID),
		   readl(dev->scd + SCD_VDH_CK_GT));
	seq_printf(s, "irq_state=%08x\n", dev->irq_state);
	for (off = 0; off <= VDM_AXI_TEST_STA; off += 0x10) {
		seq_printf(s, "vdm %03x=%08x %03x=%08x %03x=%08x %03x=%08x\n",
			   off, vdm_read(dev, off), off + 4,
			   vdm_read(dev, off + 4), off + 8,
			   vdm_read(dev, off + 8), off + 12,
			   vdm_read(dev, off + 12));
	}
	for (off = VDM_SED_STA; off <= VDM_SED_LAST; off += 0x10) {
		seq_printf(s, "vdm %03x=%08x %03x=%08x %03x=%08x %03x=%08x\n",
			   off, vdm_read(dev, off), off + 4,
			   vdm_read(dev, off + 4), off + 8,
			   vdm_read(dev, off + 8), off + 12,
			   vdm_read(dev, off + 12));
	}

	pm_runtime_put_autosuspend(dev->dev);
	return 0;
}

/* "vdm|scd|crg <offset> <value>" */
static ssize_t histb_vdec_regs_write(struct file *file,
				     const char __user *userbuf,
				     size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct histb_vdec_dev *dev = s->private;
	char buf[40], name[8];
	unsigned int reg, val;
	int ret;

	/*
	 * Debug write format: "<window> <offset> <value>", read into a fixed
	 * buffer, so an over-long line is rejected rather than truncated - a
	 * truncated line would write the value parsed from a prefix of what was
	 * typed. The offset is bounded by the register window that is mapped
	 * for this block: the write goes through that mapping directly, so an
	 * offset past its end would store into whatever lies beyond the block
	 * rather than faulting.
	 */
	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, userbuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, "%7s %x %x", name, &reg, &val) != 3 ||
	    reg >= HISTB_REG_WINDOW)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret)
		return ret;

	if (sysfs_streq(name, "vdm"))
		writel(val, dev->vdm + reg);
	else if (sysfs_streq(name, "scd"))
		writel(val, dev->scd + reg);
	else if (sysfs_streq(name, "crg"))
		ret = dev->crg ? regmap_write(dev->crg, reg, val) : -ENODEV;
	else
		ret = -EINVAL;

	pm_runtime_put_autosuspend(dev->dev);
	return ret ? ret : count;
}

static int histb_vdec_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, histb_vdec_regs_show, inode->i_private);
}

static const struct file_operations histb_vdec_regs_fops = {
	.owner = THIS_MODULE,
	.open = histb_vdec_regs_open,
	.read = seq_read,
	.write = histb_vdec_regs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int histb_vdec_pool_show(struct seq_file *s, void *unused)
{
	struct histb_vdec_dev *dev = s->private;
	unsigned int first = 0;
	unsigned int bad;
	unsigned int i;

	mutex_lock(&dev->hw_lock);
	seq_printf(s, "pool dma=%pad size=%zu pic=%u slice=%u up=%u table=%u\n",
		   &dev->pool.dma, dev->pool.size, dev->pool.pic_off,
		   dev->pool.slice_off, dev->pool.up_off, dev->pool.table_off);
	seq_hex_dump(s, "pic  ", DUMP_PREFIX_OFFSET, 16, 4,
		     histb_vdec_pic_slot(&dev->pool), HISTB_VDEC_SLOT_BYTES,
		     false);
	seq_hex_dump(s, "slc0 ", DUMP_PREFIX_OFFSET, 16, 4,
		     histb_vdec_slice_slot(&dev->pool, 0),
		     HISTB_VDEC_SLOT_BYTES, false);
	seq_hex_dump(s, "up   ", DUMP_PREFIX_OFFSET, 16, 4,
		     histb_vdec_up_msgs(&dev->pool),
		     8 * HISTB_VDEC_UP_MSG_WORDS, false);

	/*
	 * Table-area integrity and scratch usage: the VDM is only supposed to
	 * read the CABAC table and to write the scratch blocks, so a clobbered
	 * table word or an untouched scratch block says a descriptor address
	 * does not point where the driver thinks it does.
	 */
	bad = histb_vdec_hw_check_tables(dev, &first);
	seq_printf(s, "mn264 bad=%u first=%u\n", bad, first);
	for (i = 0; i < HISTB_VDEC_NUM_SCRATCH; i++) {
		unsigned int nz, off;

		nz = histb_vdec_hw_scratch_usage(dev, i, &off);
		seq_printf(s, "scr%u dma=%#llx nz=%u first=%u\n", i,
			   (unsigned long long)(histb_vdec_tables_dma(&dev->pool) +
						HISTB_TABLE_OFF_SCRATCH(i)),
			   nz, off);
	}
	mutex_unlock(&dev->hw_lock);
	return 0;
}

static int histb_vdec_pool_open(struct inode *inode, struct file *file)
{
	return single_open(file, histb_vdec_pool_show, inode->i_private);
}

static const struct file_operations histb_vdec_pool_fops = {
	.owner = THIS_MODULE,
	.open = histb_vdec_pool_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void histb_vdec_debugfs_put(void *dir)
{
	debugfs_remove_recursive(dir);
}

void histb_vdec_debugfs_init(struct histb_vdec_dev *dev)
{
	struct device_node *np;
	struct regmap *crg;
	struct dentry *dir;

	/* The CRG node is claimed by the clock driver, so the regmap lookup has
	 * to go through the syscon registration of the same node.
	 */
	np = of_parse_phandle(dev->dev->of_node, "clocks", 0);
	crg = np ? syscon_node_to_regmap(np) : ERR_PTR(-ENODEV);
	of_node_put(np);
	dev->crg = IS_ERR(crg) ? NULL : crg;

	dir = debugfs_create_dir(dev_name(dev->dev), NULL);
	dev->dbgfs = dir;
	debugfs_create_file("regs", 0600, dir, dev, &histb_vdec_regs_fops);
	debugfs_create_file("pool", 0400, dir, dev, &histb_vdec_pool_fops);
	devm_add_action_or_reset(dev->dev, histb_vdec_debugfs_put, dir);
}
