/*
 * arch/arm/mach-tegra/board-smba9701.c
 *
 * Copyright (C) 2011 Eduardo Jos� Tagle <ejtagle@tutopia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/console.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>
#include <linux/clk.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/dma-mapping.h>
#include <linux/fsl_devices.h>
#include <linux/platform_data/tegra_usb.h>
#include <linux/pda_power.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/i2c-tegra.h>
#include <linux/memblock.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/setup.h>

#include <mach/io.h>
#include <mach/w1.h>
#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/nand.h>
#include <mach/iomap.h>
#include <mach/sdhci.h>
#include <mach/gpio.h>
#include <mach/clk.h>
#include <mach/usb_phy.h>
#include <mach/i2s.h>
#include <mach/system.h>
#include <linux/nvmap.h>
#include <linux/antares_dock.h>

#include "board.h"
#include "board-smba9701.h"
#include "clock.h"
#include "gpio-names.h"
#include "devices.h"
#include "pm.h"
#include "wakeups-t2.h"
#include "wdt-recovery.h"

#include <linux/rfkill-gpio.h>



#define PMC_CTRL 0x0
#define PMC_CTRL_INTR_LOW (1 << 17)

/* NVidia bootloader tags */
#define ATAG_NVIDIA 0x41000801
#define MAX_MEMHDL 8

struct tag_tegra {
	__u32 bootarg_len;
	__u32 bootarg_key;
	__u32 bootarg_nvkey;
	__u32 bootarg[];
};

struct memhdl {
	__u32 id;
	__u32 start;
	__u32 size;
};

enum {
	RM = 1,
	DISPLAY,
	FRAMEBUFFER,
	CHIPSHMOO,
	CHIPSHMOO_PHYS,
	CARVEOUT,
	WARMBOOT,
};

static int num_memhdl = 0;

static struct memhdl nv_memhdl[MAX_MEMHDL];

static const char atag_ids[][16] = {
	"RM ",
	"DISPLAY ",
	"FRAMEBUFFER ",
	"CHIPSHMOO ",
	"CHIPSHMOO_PHYS ",
	"CARVEOUT ",
	"WARMBOOT ",
};

static int __init parse_tag_nvidia(const struct tag *tag)
{
        int i;
        struct tag_tegra *nvtag = (struct tag_tegra *)tag;
        __u32 id;

        switch (nvtag->bootarg_nvkey) {
        case FRAMEBUFFER:
                id = nvtag->bootarg[1];
                for (i=0; i<num_memhdl; i++)
              if (nv_memhdl[i].id == id) {
                tegra_bootloader_fb_start = nv_memhdl[i].start;
                tegra_bootloader_fb_size = nv_memhdl[i].size;
              }
                break;
        case WARMBOOT:
                id = nvtag->bootarg[1];
                for (i=0; i<num_memhdl; i++) {
                        if (nv_memhdl[i].id == id) {
                                tegra_lp0_vec_start = nv_memhdl[i].start;
                                tegra_lp0_vec_size = nv_memhdl[i].size;
                        }
                }
                break;
        }

        if (nvtag->bootarg_nvkey & 0x10000) {
                char pmh[] = " PreMemHdl ";
                id = nvtag->bootarg_nvkey;
                if (num_memhdl < MAX_MEMHDL) {
                        nv_memhdl[num_memhdl].id = id;
                        nv_memhdl[num_memhdl].start = nvtag->bootarg[1];
                        nv_memhdl[num_memhdl].size = nvtag->bootarg[2];
                        num_memhdl++;
                }
                pmh[11] = '0' + id;
                print_hex_dump(KERN_INFO, pmh, DUMP_PREFIX_NONE,
                                32, 4, &nvtag->bootarg[0], 4*(tag->hdr.size-2), false);
        }
        else if (nvtag->bootarg_nvkey <= ARRAY_SIZE(atag_ids))
                print_hex_dump(KERN_INFO, atag_ids[nvtag->bootarg_nvkey-1], DUMP_PREFIX_NONE,
                                32, 4, &nvtag->bootarg[0], 4*(tag->hdr.size-2), false);
        else
                pr_warning("unknown ATAG key %d\n", nvtag->bootarg_nvkey);

        return 0;
}
__tagtable(ATAG_NVIDIA, parse_tag_nvidia);

static struct rfkill_gpio_platform_data bluetooth_rfkill = {
	.name		= "bluetooth_rfkill",
	.reset_gpio	= SMBA9701_BT_RESET,
	.shutdown_gpio	= -1,
	.power_clk_name	= "bcm4329_32k_clk",
	.type		= RFKILL_TYPE_BLUETOOTH,
};

static struct platform_device bluetooth_rfkill_device = {
	.name	= "rfkill_gpio",
	.id	= -1,
	.dev	= {
		.platform_data = &bluetooth_rfkill,
	},
};

static struct resource smba_bluesleep_resources[] = {
	[0] = {
		.name = "gpio_ext_wake",
			.start  = SMBA9701_BT_WAKEUP,
			.end    = SMBA9701_BT_WAKEUP,
			.flags  = IORESOURCE_IO,
	},
	[1] = {
		.name = "host_wake",
			.start  = TEGRA_GPIO_TO_IRQ(SMBA9701_BT_IRQ),
			.end    = TEGRA_GPIO_TO_IRQ(SMBA9701_BT_IRQ),
			.flags  = IORESOURCE_IRQ | IORESOURCE_IRQ_LOWEDGE,
	},
};

static struct platform_device smba_bluesleep_device = {
	.name           = "bluesleep",
	.id             = -1,
	.num_resources  = ARRAY_SIZE(smba_bluesleep_resources),
	.resource       = smba_bluesleep_resources,
};

void __init smba_setup_bluesleep(void)
{
	clk_add_alias("bcm4329_32k_clk", bluetooth_rfkill_device.name, "blink", NULL);
	tegra_gpio_enable(SMBA9701_BT_IRQ);
	tegra_gpio_enable(SMBA9701_BT_RESET);
	return;
}

static struct dock_platform_data dock_on_platform_data = {
        .irq = TEGRA_GPIO_TO_IRQ(SMBA9701_DOCK),
        .gpio_num = SMBA9701_DOCK,
      };

static struct platform_device tegra_dock_device = {
        .name = "tegra_dock",
        .id   = -1,
        .dev = {
            .platform_data = &dock_on_platform_data,
        },
};

static struct platform_device *smba_devices[] __initdata = {
	&tegra_pmu_device,
        &tegra_gart_device,
	&tegra_aes_device,
	&bluetooth_rfkill_device,
	&smba_bluesleep_device,
	&tegra_wdt_device,
        &tegra_avp_device,
        &tegra_dock_device
};

static void __init tegra_smba_init(void)
{
	struct clk *clk;

	/* force consoles to stay enabled across suspend/resume */
	// console_suspend_enabled = 0;

	/* Init the suspend information */
	//	tegra_init_suspend(&smba_suspend);

	/* Set the SDMMC1 (wifi) tap delay to 6.  This value is determined
	 * based on propagation delay on the PCB traces. */
	clk = clk_get_sys("sdhci-tegra.0", NULL);
	if (!IS_ERR(clk)) {
		tegra_sdmmc_tap_delay(clk, 6);
		clk_put(clk);
	} else {
		pr_err("Failed to set wifi sdmmc tap delay\n");
	}

	/* Initialize the pinmux */
	smba_pinmux_init();

	/* Initialize the clocks - clocks require the pinmux to be initialized first */
	smba_clks_init();

	platform_add_devices(smba_devices,ARRAY_SIZE(smba_devices));
	/* Register i2c devices - required for Power management and MUST be done before the power register */
	smba_i2c_register_devices();

	/* Register the power subsystem - Including the poweroff handler - Required by all the others */
	smba_charge_init();
	smba_regulator_init();

	/* Register the USB device */
	smba_usb_register_devices();

	/* Register UART devices */
	smba_uart_register_devices();

        /* Register RAM Console */
        tegra_ram_console_debug_init();

	/* Register GPU devices */
	smba_panel_init();

	/* Register Audio devices */
	smba_audio_register_devices();

	/* Register all the keyboard devices */
	smba_keyboard_register_devices();

	/* Register touchscreen devices */
	smba_touch_register_devices();

	/* Register accelerometer device */
	smba_sensors_register_devices();

	/* Register Camera powermanagement devices */
	smba_camera_register_devices();

	/* Register SDHCI devices */
	smba_sdhci_init();

	/* Register Bluetooth powermanagement devices */
	smba_setup_bluesleep();

	/* Register gsm powermanagement devices */
	smba_gsm_pm_register_devices();

	/* Release the tegra bootloader framebuffer */
	tegra_release_bootloader_fb();
}

static void __init tegra_smba_reserve(void)
{
	if (memblock_reserve(0x0, 4096) < 0)
		pr_warn("Cannot reserve first 4K of memory for safety\n");

	/* Reserve the graphics memory */
	tegra_reserve(SMBA9701_GPU_MEM_SIZE, SMBA9701_FB1_MEM_SIZE, SMBA9701_FB2_MEM_SIZE);
	tegra_ram_console_debug_reserve(SZ_1M);
}

static void __init tegra_smba_fixup(struct machine_desc *desc,
	struct tag *tags, char **cmdline, struct meminfo *mi)
{
	mi->nr_banks = SMBA9701_MEM_BANKS;
	mi->bank[0].start = PHYS_OFFSET;
	mi->bank[0].size  = SMBA9701_MEM_SIZE - SMBA9701_TOTAL_GPU_MEM_SIZE;
        mi->bank[1].start = SZ_512M;
        mi->bank[1].size  = SZ_512M;
}

MACHINE_START(HARMONY, "harmony")
.boot_params	= 0x00000100,
.map_io         = tegra_map_common_io,
.init_early     = tegra_init_early,
.init_irq       = tegra_init_irq,
.timer          = &tegra_timer,
.init_machine	= tegra_smba_init,
.reserve	= tegra_smba_reserve,
.fixup		= tegra_smba_fixup,
MACHINE_END
