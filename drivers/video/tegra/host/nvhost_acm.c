/*
 * drivers/video/tegra/host/nvhost_acm.c
 *
 * Tegra Graphics Host Automatic Clock Management
 *
 * Copyright (c) 2010-2011, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "dev.h"
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/device.h>
#include <mach/powergate.h>
#include <mach/clk.h>
#include <mach/hardware.h>

#define ACM_POWERDOWN_HANDLER_DELAY_MSEC  25
#define ACM_SUSPEND_WAIT_FOR_IDLE_TIMEOUT (2 * HZ)

void nvhost_module_busy(struct nvhost_module *mod)
{
	mutex_lock(&mod->lock);
	cancel_delayed_work(&mod->powerdown);
	if ((atomic_inc_return(&mod->refcount) == 1) && !mod->powered) {
		int i = 0;
		if (mod->parent)
			nvhost_module_busy(mod->parent);
		if (mod->powergate_id != -1)
			tegra_unpowergate_partition(mod->powergate_id);
		if (mod->powergate_id2 != -1)
			tegra_unpowergate_partition(mod->powergate_id2);
		while (i < mod->num_clks)
			clk_enable(mod->clk[i++]);
		if (mod->func)
			mod->func(mod, NVHOST_POWER_ACTION_ON);
		mod->powered = true;
	}
	mutex_unlock(&mod->lock);
}

static void powerdown_handler(struct work_struct *work)
{
	struct nvhost_module *mod;
	mod = container_of(to_delayed_work(work), struct nvhost_module, powerdown);
	mutex_lock(&mod->lock);
	if ((atomic_read(&mod->refcount) == 0) && mod->powered) {
		int i;
		if (mod->func)
			mod->func(mod, NVHOST_POWER_ACTION_OFF);
		for (i = 0; i < mod->num_clks; i++)
			clk_disable(mod->clk[i]);
		if (mod->powergate_id != -1)
			tegra_powergate_partition(mod->powergate_id);

		if (mod->powergate_id2 != -1)
			tegra_powergate_partition(mod->powergate_id2);

		mod->powered = false;
		if (mod->parent)
			nvhost_module_idle(mod->parent);
	}
	mutex_unlock(&mod->lock);
}

void nvhost_module_idle_mult(struct nvhost_module *mod, int refs)
{
	bool kick = false;

	mutex_lock(&mod->lock);
	if (atomic_sub_return(refs, &mod->refcount) == 0) {
		BUG_ON(!mod->powered);
		schedule_delayed_work(&mod->powerdown,
			msecs_to_jiffies(mod->powerdown_delay));
		kick = true;
	}
	mutex_unlock(&mod->lock);

	if (kick)
		wake_up(&mod->idle);
}

static const char *get_module_clk_id(const char *module, int index)
{
	if (index == 0)
		return module;
	if (strcmp(module, "gr2d") == 0) {
		if (index == 1)
			return "epp";
		if (index == 2)
			return "emc";
	}
	if (strcmp(module, "gr3d") == 0) {
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
		if (index == 1)
			return "emc";
#else
		if (index == 1)
			return "gr3d2";
		if (index == 2)
			return "emc";
#endif
	}
	if (strcmp(module, "mpe") == 0) {
		if (index == 1)
			return "emc";
	}
	return NULL;
}

/* Not all hardware revisions support power gating */
static bool _3d_powergating_disabled(void)
{
	int chipid = tegra_get_chipid();

	return chipid < TEGRA_CHIPID_TEGRA3
		|| (chipid == TEGRA_CHIPID_TEGRA3
			&& tegra_get_revision() == TEGRA_REVISION_A01);
}

int nvhost_module_init(struct nvhost_module *mod, const char *name,
		nvhost_modulef func, struct nvhost_module *parent,
		struct device *dev)
{
	int i = 0;

	mod->name = name;

	while (i < NVHOST_MODULE_MAX_CLOCKS) {
		long rate;
		mod->clk[i] = clk_get(dev, get_module_clk_id(name, i));
		if (IS_ERR_OR_NULL(mod->clk[i]))
			break;
		if (strcmp(name, "gr2d") == 0)
			rate = clk_round_rate(mod->clk[i], 0);
		else
			rate = clk_round_rate(mod->clk[i], UINT_MAX);
		if (rate < 0) {
			pr_err("%s: can't get maximum rate for %s\n",
				__func__, name);
			break;
		}
		clk_enable(mod->clk[i]);
		clk_set_rate(mod->clk[i], rate);
		clk_disable(mod->clk[i]);
		i++;
	}

	mod->num_clks = i;
	mod->func = func;
	mod->parent = parent;
	mod->powered = false;
	mod->powergate_id = -1;
	mod->powergate_id2 = -1;
	if (strcmp(name, "gr2d") == 0)
		mod->powerdown_delay = 0;
	else
		mod->powerdown_delay = ACM_POWERDOWN_HANDLER_DELAY_MSEC;

	if (strcmp(name, "gr3d") == 0) {
		mod->powergate_id = TEGRA_POWERGATE_3D;
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
		mod->powergate_id2 = TEGRA_POWERGATE_3D1;
#endif
	}

	if (mod->powergate_id == TEGRA_POWERGATE_3D
		&& _3d_powergating_disabled()) {
		tegra_unpowergate_partition(mod->powergate_id);
		mod->powergate_id = -1;

#ifdef CONFIG_ARCH_TEGRA_3x_SOC
	if (mod->powergate_id2 == TEGRA_POWERGATE_3D1) {
		tegra_unpowergate_partition(mod->powergate_id2);
		mod->powergate_id2 = -1;
	}
#endif
	}

	mutex_init(&mod->lock);
	init_waitqueue_head(&mod->idle);
	INIT_DELAYED_WORK(&mod->powerdown, powerdown_handler);

	return 0;
}

static int is_module_idle(struct nvhost_module *mod)
{
	int count;
	mutex_lock(&mod->lock);
	count = atomic_read(&mod->refcount);
	mutex_unlock(&mod->lock);
	return (count == 0);
}

static void debug_not_idle(struct nvhost_master *dev)
{
	int i;
	bool lock_released = true;

	for (i = 0; i < dev->nb_channels; i++) {
		struct nvhost_module *m = &dev->channels[i].mod;
		if (m->name)
			printk("tegra_grhost: %s: refcnt %d\n",
				m->name, atomic_read(&m->refcount));
	}

	for (i = 0; i < dev->nb_mlocks; i++) {
		int c = atomic_read(&dev->cpuaccess.lock_counts[i]);
		if (c) {
			printk("tegra_grhost: lock id %d: refcnt %d\n", i, c);
			lock_released = false;
		}
	}
	if (lock_released)
		printk("tegra_grhost: all locks released\n");
}

void nvhost_module_suspend(struct nvhost_module *mod, bool system_suspend)
{
	int ret;
	struct nvhost_master *dev;

	if (system_suspend) {
		dev = container_of(mod, struct nvhost_master, mod);
		if (!is_module_idle(mod))
			debug_not_idle(dev);
	} else {
		dev = container_of(mod, struct nvhost_channel, mod)->dev;
	}

	ret = wait_event_timeout(mod->idle, is_module_idle(mod),
			ACM_SUSPEND_WAIT_FOR_IDLE_TIMEOUT);
	if (ret == 0)
		nvhost_debug_dump(dev);

	if (system_suspend)
		printk("tegra_grhost: entered idle\n");

	flush_delayed_work(&mod->powerdown);
	if (system_suspend)
		printk("tegra_grhost: flushed delayed work\n");
	BUG_ON(mod->powered);
}

void nvhost_module_deinit(struct nvhost_module *mod)
{
	int i;
	nvhost_module_suspend(mod, false);
	for (i = 0; i < mod->num_clks; i++)
		clk_put(mod->clk[i]);
}