// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Rockchip SoCs
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *		http://www.rock-chips.com
 *
 * Author: Simon Xue <xxm@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/resource.h>
#include <linux/rfkill-wlan.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/pci-epf.h>

#include "pcie-designware.h"
#include "../../pci.h"
#include "../rockchip-pcie-dma.h"
#include "pcie-dw-dmatest.h"

enum rk_pcie_device_mode {
	RK_PCIE_EP_TYPE,
	RK_PCIE_RC_TYPE,
};

struct reset_bulk_data	{
	const char *id;
	struct reset_control *rst;
};

#define RK_PCIE_DBG			0

#define PCIE_DMA_OFFSET			0x380000

#define PCIE_DMA_WR_ENB			0xc
#define PCIE_DMA_WR_CTRL_LO		0x200
#define PCIE_DMA_WR_CTRL_HI		0x204
#define PCIE_DMA_WR_XFERSIZE		0x208
#define PCIE_DMA_WR_SAR_PTR_LO		0x20c
#define PCIE_DMA_WR_SAR_PTR_HI		0x210
#define PCIE_DMA_WR_DAR_PTR_LO		0x214
#define PCIE_DMA_WR_DAR_PTR_HI		0x218
#define PCIE_DMA_WR_WEILO		0x18
#define PCIE_DMA_WR_WEIHI		0x1c
#define PCIE_DMA_WR_DOORBELL		0x10
#define PCIE_DMA_WR_INT_STATUS		0x4c
#define PCIE_DMA_WR_INT_MASK		0x54
#define PCIE_DMA_WR_INT_CLEAR		0x58

#define PCIE_DMA_RD_ENB			0x2c
#define PCIE_DMA_RD_CTRL_LO		0x300
#define PCIE_DMA_RD_CTRL_HI		0x304
#define PCIE_DMA_RD_XFERSIZE		0x308
#define PCIE_DMA_RD_SAR_PTR_LO		0x30c
#define PCIE_DMA_RD_SAR_PTR_HI		0x310
#define PCIE_DMA_RD_DAR_PTR_LO		0x314
#define PCIE_DMA_RD_DAR_PTR_HI		0x318
#define PCIE_DMA_RD_WEILO		0x38
#define PCIE_DMA_RD_WEIHI		0x3c
#define PCIE_DMA_RD_DOORBELL		0x30
#define PCIE_DMA_RD_INT_STATUS		0xa0
#define PCIE_DMA_RD_INT_MASK		0xa8
#define PCIE_DMA_RD_INT_CLEAR		0xac

#define PCIE_DMA_CHANEL_MAX_NUM		2

/* Parameters for the waiting for iATU enabled routine */
#define LINK_WAIT_IATU_MIN		9000
#define LINK_WAIT_IATU_MAX		10000

#define PCIE_DIRECT_SPEED_CHANGE	(0x1 << 17)

#define PCIE_TYPE0_STATUS_COMMAND_REG	0x4
#define PCIE_TYPE0_BAR0_REG		0x10

#define PCIE_CAP_LINK_CONTROL2_LINK_STATUS	0xa0

#define PCIE_CLIENT_INTR_STATUS_LEGACY	0x08
#define PCIE_CLIENT_INTR_STATUS_MISC	0x10
#define PCIE_CLIENT_INTR_MASK_LEGACY	0x1c
#define UNMASK_ALL_LEGACY_INT		0xffff0000
#define MASK_LEGACY_INT(x)		(0x00110011 << x)
#define UNMASK_LEGACY_INT(x)		(0x00110000 << x)
#define PCIE_CLIENT_INTR_MASK		0x24
#define PCIE_CLIENT_GENERAL_DEBUG	0x104
#define PCIE_CLIENT_HOT_RESET_CTRL	0x180
#define PCIE_LTSSM_ENABLE_ENHANCE	BIT(4)
#define PCIE_CLIENT_LTSSM_STATUS	0x300
#define SMLH_LINKUP			BIT(16)
#define RDLH_LINKUP			BIT(17)
#define PCIE_CLIENT_DBG_FIFO_MODE_CON	0x310
#define PCIE_CLIENT_DBG_FIFO_PTN_HIT_D0 0x320
#define PCIE_CLIENT_DBG_FIFO_PTN_HIT_D1 0x324
#define PCIE_CLIENT_DBG_FIFO_TRN_HIT_D0 0x328
#define PCIE_CLIENT_DBG_FIFO_TRN_HIT_D1 0x32c
#define PCIE_CLIENT_DBG_FIFO_STATUS	0x350
#define PCIE_CLIENT_DBG_TRANSITION_DATA	0xffff0000
#define PCIE_CLIENT_DBF_EN		0xffff0003

#define PCIE_PHY_LINKUP			BIT(0)
#define PCIE_DATA_LINKUP		BIT(1)

#define PCIE_RESBAR_CTRL_REG0_REG	0x2a8
#define PCIE_SB_BAR0_MASK_REG		0x100010

#define PCIE_PL_ORDER_RULE_CTRL_OFF	0x8B4

struct rk_pcie {
	struct dw_pcie			*pci;
	enum rk_pcie_device_mode	mode;
	enum phy_mode			phy_mode;
	unsigned char			bar_to_atu[6];
	phys_addr_t			*outbound_addr;
	unsigned long			*ib_window_map;
	unsigned long			*ob_window_map;
	unsigned int			num_ib_windows;
	unsigned int			num_ob_windows;
	void __iomem			*dbi_base;
	void __iomem			*apb_base;
	struct phy			*phy;
	struct clk_bulk_data		*clks;
	unsigned int			clk_cnt;
	struct reset_bulk_data		*rsts;
	struct gpio_desc		*rst_gpio;
	struct gpio_desc		*prsnt_gpio;
	phys_addr_t			mem_start;
	size_t				mem_size;
	struct pcie_port		pp;
	struct regmap			*usb_pcie_grf;
	struct regmap			*pmu_grf;
	struct dma_trx_obj		*dma_obj;
	bool				in_suspend;
	bool                            skip_scan_in_resume;
	bool				is_rk1808;
	bool				is_signal_test;
	bool				bifurcation;
	struct regulator		*vpcie3v3;
	struct irq_domain		*irq_domain;
	raw_spinlock_t			intx_lock;
	struct dentry			*debugfs;
	u32				msi_vector_num;
};

struct rk_pcie_of_data {
	enum rk_pcie_device_mode	mode;
	u32 msi_vector_num;
};

#define to_rk_pcie(x)	dev_get_drvdata((x)->dev)
static const struct dev_pm_ops rockchip_dw_pcie_pm_ops;

static int rk_pcie_read(void __iomem *addr, int size, u32 *val)
{
	if ((uintptr_t)addr & (size - 1)) {
		*val = 0;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	if (size == 4) {
		*val = readl(addr);
	} else if (size == 2) {
		*val = readw(addr);
	} else if (size == 1) {
		*val = readb(addr);
	} else {
		*val = 0;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	return PCIBIOS_SUCCESSFUL;
}

static int rk_pcie_write(void __iomem *addr, int size, u32 val)
{
	if ((uintptr_t)addr & (size - 1))
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (size == 4)
		writel(val, addr);
	else if (size == 2)
		writew(val, addr);
	else if (size == 1)
		writeb(val, addr);
	else
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}

static u32 __rk_pcie_read_apb(struct rk_pcie *rk_pcie, void __iomem *base,
			u32 reg, size_t size)
{
	int ret;
	u32 val;

	ret = rk_pcie_read(base + reg, size, &val);
	if (ret)
		dev_err(rk_pcie->pci->dev, "Read APB address failed\n");

	return val;
}

static void __rk_pcie_write_apb(struct rk_pcie *rk_pcie, void __iomem *base,
			u32 reg, size_t size, u32 val)
{
	int ret;

	ret = rk_pcie_write(base + reg, size, val);
	if (ret)
		dev_err(rk_pcie->pci->dev, "Write APB address failed\n");
}

static inline u32 rk_pcie_readl_apb(struct rk_pcie *rk_pcie, u32 reg)
{
	return __rk_pcie_read_apb(rk_pcie, rk_pcie->apb_base, reg, 0x4);
}

static inline void rk_pcie_writel_apb(struct rk_pcie *rk_pcie, u32 reg,
					u32 val)
{
	__rk_pcie_write_apb(rk_pcie, rk_pcie->apb_base, reg, 0x4, val);
}

static u8 rk_pcie_iatu_unroll_enabled(struct dw_pcie *pci)
{
	u32 val;

	val = dw_pcie_readl_dbi(pci, PCIE_ATU_VIEWPORT);
	if (val == 0xffffffff)
		return 1;

	return 0;
}

static int rk_pcie_ep_inbound_atu(struct rk_pcie *rk_pcie,
				enum pci_barno bar, dma_addr_t cpu_addr,
				enum dw_pcie_as_type as_type)
{
	int ret;
	u32 free_win;

	if (rk_pcie->in_suspend) {
		free_win = rk_pcie->bar_to_atu[bar];
	} else {
		free_win = find_first_zero_bit(rk_pcie->ib_window_map,
					       rk_pcie->num_ib_windows);
		if (free_win >= rk_pcie->num_ib_windows) {
			dev_err(rk_pcie->pci->dev, "No free inbound window\n");
			return -EINVAL;
		}
	}

	ret = dw_pcie_prog_inbound_atu(rk_pcie->pci, free_win, bar, cpu_addr,
				       as_type);
	if (ret < 0) {
		dev_err(rk_pcie->pci->dev, "Failed to program IB window\n");
		return ret;
	}

	if (rk_pcie->in_suspend)
		return 0;

	rk_pcie->bar_to_atu[bar] = free_win;
	set_bit(free_win, rk_pcie->ib_window_map);

	return 0;
}

static int rk_pcie_ep_outbound_atu(struct rk_pcie *rk_pcie,
					phys_addr_t phys_addr, u64 pci_addr,
					size_t size)
{
	u32 free_win;

	if (rk_pcie->in_suspend) {
		free_win = find_first_bit(rk_pcie->ob_window_map,
					  rk_pcie->num_ob_windows);
	} else {
		free_win = find_first_zero_bit(rk_pcie->ob_window_map,
					       rk_pcie->num_ob_windows);
		if (free_win >= rk_pcie->num_ob_windows) {
			dev_err(rk_pcie->pci->dev, "No free outbound window\n");
			return -EINVAL;
		}
	}

	dw_pcie_prog_outbound_atu(rk_pcie->pci, free_win, PCIE_ATU_TYPE_MEM,
				  phys_addr, pci_addr, size);

	if (rk_pcie->in_suspend)
		return 0;

	set_bit(free_win, rk_pcie->ob_window_map);
	rk_pcie->outbound_addr[free_win] = phys_addr;

	return 0;
}

static void __rk_pcie_ep_reset_bar(struct rk_pcie *rk_pcie,
					     enum pci_barno bar, int flags)
{
	u32 reg;

	reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	dw_pcie_writel_dbi(rk_pcie->pci, reg, 0x0);
	if (flags & PCI_BASE_ADDRESS_MEM_TYPE_64)
		dw_pcie_writel_dbi(rk_pcie->pci, reg + 4, 0x0);
}

static void rk_pcie_ep_reset_bar(struct rk_pcie *rk_pcie, enum pci_barno bar)
{
	__rk_pcie_ep_reset_bar(rk_pcie, bar, 0);
}

static int rk_pcie_ep_atu_init(struct rk_pcie *rk_pcie)
{
	int ret;
	enum pci_barno bar;
	enum dw_pcie_as_type as_type;
	dma_addr_t cpu_addr;
	phys_addr_t phys_addr;
	u64 pci_addr;
	size_t size;

	for (bar = BAR_0; bar <= BAR_5; bar++)
		rk_pcie_ep_reset_bar(rk_pcie, bar);

	cpu_addr = rk_pcie->mem_start;
	as_type = DW_PCIE_AS_MEM;
	ret = rk_pcie_ep_inbound_atu(rk_pcie, BAR_0, cpu_addr, as_type);
	if (ret)
		return ret;

	phys_addr = 0x0;
	pci_addr = 0x0;
	size = SZ_2G;
	ret = rk_pcie_ep_outbound_atu(rk_pcie, phys_addr, pci_addr, size);
	if (ret)
		return ret;

	return 0;
}

static inline void rk_pcie_set_mode(struct rk_pcie *rk_pcie)
{
	switch (rk_pcie->mode) {
	case RK_PCIE_EP_TYPE:
		rk_pcie_writel_apb(rk_pcie, 0x0, 0xf00000);
		break;
	case RK_PCIE_RC_TYPE:
		rk_pcie_writel_apb(rk_pcie, 0x0, 0xf00040);
		/*
		 * Disable order rule for CPL can't pass halted P queue.
		 * Need to check producer-consumer model.
		 * Just for RK1808 platform.
		 */
		if (rk_pcie->is_rk1808)
			dw_pcie_writel_dbi(rk_pcie->pci,
					   PCIE_PL_ORDER_RULE_CTRL_OFF,
					   0xff00);
		break;
	}
}

static inline void rk_pcie_link_status_clear(struct rk_pcie *rk_pcie)
{
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_GENERAL_DEBUG, 0x0);
}

static inline void rk_pcie_disable_ltssm(struct rk_pcie *rk_pcie)
{
	rk_pcie_writel_apb(rk_pcie, 0x0, 0xc0008);
}

static inline void rk_pcie_enable_ltssm(struct rk_pcie *rk_pcie)
{
	rk_pcie_writel_apb(rk_pcie, 0x0, 0xC000C);
}

static int rk_pcie_link_up(struct dw_pcie *pci)
{
	struct rk_pcie *rk_pcie = to_rk_pcie(pci);
	u32 val;

	if (rk_pcie->is_rk1808) {
		val = rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_GENERAL_DEBUG);
		if ((val & (PCIE_PHY_LINKUP | PCIE_DATA_LINKUP)) == 0x3 &&
		    ((val & GENMASK(15, 10)) >> 10) == 0x11)
			return 1;
	} else {
		val = rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_LTSSM_STATUS);
		if ((val & (RDLH_LINKUP | SMLH_LINKUP)) == 0x30000)
			return 1;
	}

	return 0;
}

static void rk_pcie_enable_debug(struct rk_pcie *rk_pcie)
{
#if RK_PCIE_DBG
	if (rk_pcie->is_rk1808 == true)
		return;

	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_DBG_FIFO_PTN_HIT_D0,
			   PCIE_CLIENT_DBG_TRANSITION_DATA);
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_DBG_FIFO_PTN_HIT_D1,
			   PCIE_CLIENT_DBG_TRANSITION_DATA);
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_DBG_FIFO_TRN_HIT_D0,
			   PCIE_CLIENT_DBG_TRANSITION_DATA);
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_DBG_FIFO_TRN_HIT_D1,
			   PCIE_CLIENT_DBG_TRANSITION_DATA);
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_DBG_FIFO_MODE_CON,
			   PCIE_CLIENT_DBF_EN);
#endif
}

static void rk_pcie_debug_dump(struct rk_pcie *rk_pcie)
{
#if RK_PCIE_DBG
	u32 loop;
	struct dw_pcie *pci = rk_pcie->pci;

	dev_info(pci->dev, "ltssm = 0x%x\n",
		 rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_LTSSM_STATUS));
	for (loop = 0; loop < 64; loop++)
		dev_info(pci->dev, "fifo_status = 0x%x\n",
			 rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_DBG_FIFO_STATUS));
#endif
}

static int rk_pcie_establish_link(struct dw_pcie *pci)
{
	int retries, power;
	struct rk_pcie *rk_pcie = to_rk_pcie(pci);
	bool std_rc = rk_pcie->mode == RK_PCIE_RC_TYPE && !rk_pcie->dma_obj;

	/*
	 * For standard RC, even if the link has been setup by firmware,
	 * we still need to reset link as we need to remove all resource info
	 * from devices, for instance BAR, as it wasn't assigned by kernel.
	 */
	if (dw_pcie_link_up(pci) && !std_rc) {
		dev_err(pci->dev, "link is already up\n");
		return 0;
	}

	rk_pcie_disable_ltssm(rk_pcie);
	rk_pcie_link_status_clear(rk_pcie);
	rk_pcie_enable_debug(rk_pcie);

	/* Enable client reset or link down interrupt */
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_INTR_MASK, 0x40000);

	/* Enable LTSSM */
	rk_pcie_enable_ltssm(rk_pcie);

	/*
	 * In resume routine, function devices' resume function must be late after
	 * controllers'. Some devices, such as Wi-Fi, need special IO setting before
	 * finishing training. So there must be timeout here. These kinds of devices
	 * need rescan devices by its driver when used. So no need to waste time waiting
	 * for training pass.
	 */
	if (rk_pcie->in_suspend && rk_pcie->skip_scan_in_resume) {
		rfkill_get_wifi_power_state(&power);
		if (!power) {
			gpiod_set_value_cansleep(rk_pcie->rst_gpio, 1);
			return 0;
		}
	}

	/*
	 * PCIe requires the refclk to be stable for 100µs prior to releasing
	 * PERST and T_PVPERL (Power stable to PERST# inactive) should be a
	 * minimum of 100ms.  See table 2-4 in section 2.6.2 AC, the PCI Express
	 * Card Electromechanical Specification 3.0. So 100ms in total is the min
	 * requuirement here. We add a 1s for sake of hoping everthings work fine.
	 */
	msleep(1000);
	gpiod_set_value_cansleep(rk_pcie->rst_gpio, 1);

	/*
	 * Add this 1ms delay because we observe link is always up stably after it and
	 * could help us save 20ms for scanning devices.
	 */
	 usleep_range(1000, 1100);

	for (retries = 0; retries < 10; retries++) {
		if (dw_pcie_link_up(pci)) {
			/*
			 * We may be here in case of L0 in Gen1. But if EP is capable
			 * of Gen2 or Gen3, Gen switch may happen just in this time, but
			 * we keep on accessing devices in unstable link status. Given
			 * that LTSSM max timeout is 24ms per period, we can wait a bit
			 * more for Gen switch.
			 */
			msleep(100);
			dev_info(pci->dev, "PCIe Link up, LTSSM is 0x%x\n",
				 rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_LTSSM_STATUS));
			rk_pcie_debug_dump(rk_pcie);
			return 0;
		}

		dev_info_ratelimited(pci->dev, "PCIe Linking... LTSSM is 0x%x\n",
				     rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_LTSSM_STATUS));
		rk_pcie_debug_dump(rk_pcie);
		msleep(1000);
	}

	dev_err(pci->dev, "PCIe Link Fail\n");

	return rk_pcie->is_signal_test == true ? 0 : -EINVAL;
}

static int rk_pcie_init_dma_trx(struct rk_pcie *rk_pcie)
{
	rk_pcie->dma_obj = rk_pcie_dma_obj_probe(rk_pcie->pci->dev);
	if (IS_ERR(rk_pcie->dma_obj)) {
		dev_err(rk_pcie->pci->dev, "failed to prepare dma object\n");
		return -EINVAL;
	}

	rk_pcie->dma_obj = pcie_dw_dmatest_register(rk_pcie->pci->dev, true);
	if (IS_ERR(rk_pcie->dma_obj)) {
		dev_err(rk_pcie->pci->dev, "failed to prepare dmatest\n");
		return -EINVAL;
	}

	/* Enable client write and read interrupt */
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_INTR_MASK, 0xc000000);

	/* Enable core write interrupt */
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_WR_INT_MASK,
			   0x0);
	/* Enable core read interrupt */
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_RD_INT_MASK,
			   0x0);
	return 0;
}

static void rk_pcie_ep_setup(struct rk_pcie *rk_pcie)
{
	int ret;
	u32 val;
	u32 lanes;
	struct device *dev = rk_pcie->pci->dev;
	struct device_node *np = dev->of_node;

	/* Enable client write and read interrupt */
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_INTR_MASK, 0xc000000);

	/* Enable core write interrupt */
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_WR_INT_MASK,
			   0x0);
	/* Enable core read interrupt */
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_RD_INT_MASK,
			   0x0);

	ret = of_property_read_u32(np, "num-lanes", &lanes);
	if (ret)
		lanes = 0;

	/* Set the number of lanes */
	val = dw_pcie_readl_dbi(rk_pcie->pci, PCIE_PORT_LINK_CONTROL);
	val &= ~PORT_LINK_MODE_MASK;
	switch (lanes) {
	case 1:
		val |= PORT_LINK_MODE_1_LANES;
		break;
	case 2:
		val |= PORT_LINK_MODE_2_LANES;
		break;
	case 4:
		val |= PORT_LINK_MODE_4_LANES;
		break;
	case 8:
		val |= PORT_LINK_MODE_8_LANES;
		break;
	default:
		dev_err(dev, "num-lanes %u: invalid value\n", lanes);
		return;
	}

	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_PORT_LINK_CONTROL, val);

	/* Set link width speed control register */
	val = dw_pcie_readl_dbi(rk_pcie->pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
	val &= ~PORT_LOGIC_LINK_WIDTH_MASK;
	switch (lanes) {
	case 1:
		val |= PORT_LOGIC_LINK_WIDTH_1_LANES;
		break;
	case 2:
		val |= PORT_LOGIC_LINK_WIDTH_2_LANES;
		break;
	case 4:
		val |= PORT_LOGIC_LINK_WIDTH_4_LANES;
		break;
	case 8:
		val |= PORT_LOGIC_LINK_WIDTH_8_LANES;
		break;
	}

	val |= PCIE_DIRECT_SPEED_CHANGE;

	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);

	/* Enable bus master and memory space */
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_TYPE0_STATUS_COMMAND_REG, 0x6);

	/* Resize BAR0 to 4GB */
	/* bit13-8 set to 6 means 64MB */
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_RESBAR_CTRL_REG0_REG, 0x600);

	/* Set shadow BAR0 according 64MB */
	val = rk_pcie->mem_size - 1;
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_SB_BAR0_MASK_REG, val);

	/* Set reserved memory address to BAR0 */
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_TYPE0_BAR0_REG,
			   rk_pcie->mem_start);
}

static int rk_pcie_ep_win_parse(struct rk_pcie *rk_pcie)
{
	int ret;
	void *addr;
	struct device *dev = rk_pcie->pci->dev;
	struct device_node *np = dev->of_node;

	ret = of_property_read_u32(np, "num-ib-windows",
				   &rk_pcie->num_ib_windows);
	if (ret < 0) {
		dev_err(dev, "unable to read *num-ib-windows* property\n");
		return ret;
	}

	if (rk_pcie->num_ib_windows > MAX_IATU_IN) {
		dev_err(dev, "Invalid *num-ib-windows*\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "num-ob-windows",
				   &rk_pcie->num_ob_windows);
	if (ret < 0) {
		dev_err(dev, "Unable to read *num-ob-windows* property\n");
		return ret;
	}

	if (rk_pcie->num_ob_windows > MAX_IATU_OUT) {
		dev_err(dev, "Invalid *num-ob-windows*\n");
		return -EINVAL;
	}

	rk_pcie->ib_window_map = devm_kcalloc(dev,
					BITS_TO_LONGS(rk_pcie->num_ib_windows),
					sizeof(long), GFP_KERNEL);
	if (!rk_pcie->ib_window_map)
		return -ENOMEM;

	rk_pcie->ob_window_map = devm_kcalloc(dev,
					BITS_TO_LONGS(rk_pcie->num_ob_windows),
					sizeof(long), GFP_KERNEL);
	if (!rk_pcie->ob_window_map)
		return -ENOMEM;

	addr = devm_kcalloc(dev, rk_pcie->num_ob_windows, sizeof(phys_addr_t),
			    GFP_KERNEL);
	if (!addr)
		return -ENOMEM;

	rk_pcie->outbound_addr = addr;

	return 0;
}

static int rk_pcie_msi_host_init(struct pcie_port *pp)
{
	return 0;
}

static void rk_pcie_msi_set_num_vectors(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct rk_pcie *rk_pcie = to_rk_pcie(pci);

	pp->num_vectors = rk_pcie->msi_vector_num;
}

static int rk_pcie_host_init(struct pcie_port *pp)
{
	int ret;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);

	dw_pcie_setup_rc(pp);

	ret = rk_pcie_establish_link(pci);

	if (pp->msi_irq > 0)
		dw_pcie_msi_init(pp);

	return ret;
}

static struct dw_pcie_host_ops rk_pcie_host_ops = {
	.host_init = rk_pcie_host_init,
};

static int rk_add_pcie_port(struct rk_pcie *rk_pcie, struct platform_device *pdev)
{
	int ret;
	struct dw_pcie *pci = rk_pcie->pci;
	struct pcie_port *pp = &pci->pp;
	struct device *dev = pci->dev;

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq_byname(pdev, "msi");
		/* If msi_irq is invalid, use outband msi routine */
		if (pp->msi_irq < 0) {
			dev_info(dev, "use outband MSI support");
			rk_pcie_host_ops.msi_host_init = rk_pcie_msi_host_init;
		} else {
			dev_info(dev, "max MSI vector is %d\n", rk_pcie->msi_vector_num);
			rk_pcie_host_ops.set_num_vectors = rk_pcie_msi_set_num_vectors;
		}
	}

	pp->ops = &rk_pcie_host_ops;

	if (device_property_read_bool(dev, "msi-map"))
		pp->msi_ext = 1;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static int rk_pcie_add_ep(struct rk_pcie *rk_pcie)
{
	int ret;
	struct device *dev = rk_pcie->pci->dev;
	struct device_node *np = dev->of_node;
	struct device_node *mem;
	struct resource reg;

	mem = of_parse_phandle(np, "memory-region", 0);
	if (!mem) {
		dev_err(dev, "missing \"memory-region\" property\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(mem, 0, &reg);
	if (ret < 0) {
		dev_err(dev, "missing \"reg\" property\n");
		return ret;
	}

	rk_pcie->mem_start = reg.start;
	rk_pcie->mem_size = resource_size(&reg);

	ret = rk_pcie_ep_win_parse(rk_pcie);
	if (ret) {
		dev_err(dev, "failed to parse ep dts\n");
		return ret;
	}

	rk_pcie->pci->iatu_unroll_enabled = rk_pcie_iatu_unroll_enabled(rk_pcie->pci);

	ret = rk_pcie_ep_atu_init(rk_pcie);
	if (ret) {
		dev_err(dev, "failed to init ep device\n");
		return ret;
	}

	rk_pcie_ep_setup(rk_pcie);

	ret = rk_pcie_establish_link(rk_pcie->pci);
	if (ret) {
		dev_err(dev, "failed to establish pcie link\n");
		return ret;
	}

	return 0;
}

static void rk_pcie_clk_deinit(struct rk_pcie *rk_pcie)
{
	clk_bulk_disable(rk_pcie->clk_cnt, rk_pcie->clks);
	clk_bulk_unprepare(rk_pcie->clk_cnt, rk_pcie->clks);
}

static int rk_pcie_clk_init(struct rk_pcie *rk_pcie)
{
	struct device *dev = rk_pcie->pci->dev;
	struct property *prop;
	const char *name;
	int i = 0, ret, count;

	count = of_property_count_strings(dev->of_node, "clock-names");
	if (count < 1)
		return -ENODEV;

	rk_pcie->clks = devm_kcalloc(dev, count,
				     sizeof(struct clk_bulk_data),
				     GFP_KERNEL);
	if (!rk_pcie->clks)
		return -ENOMEM;

	rk_pcie->clk_cnt = count;

	of_property_for_each_string(dev->of_node, "clock-names", prop, name) {
		rk_pcie->clks[i].id = name;
		if (!rk_pcie->clks[i].id)
			return -ENOMEM;
		i++;
	}

	ret = devm_clk_bulk_get(dev, count, rk_pcie->clks);
	if (ret)
		return ret;

	ret = clk_bulk_prepare(count, rk_pcie->clks);
	if (ret)
		return ret;

	ret = clk_bulk_enable(count, rk_pcie->clks);
	if (ret) {
		clk_bulk_unprepare(count, rk_pcie->clks);
		return ret;
	}

	return 0;
}

static int rk_pcie_resource_get(struct platform_device *pdev,
					 struct rk_pcie *rk_pcie)
{
	struct resource *dbi_base;
	struct resource *apb_base;

	dbi_base = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"pcie-dbi");
	if (!dbi_base) {
		dev_err(&pdev->dev, "get pcie-dbi failed\n");
		return -ENODEV;
	}

	rk_pcie->dbi_base = devm_ioremap_resource(&pdev->dev, dbi_base);
	if (IS_ERR(rk_pcie->dbi_base))
		return PTR_ERR(rk_pcie->dbi_base);

	rk_pcie->pci->dbi_base = rk_pcie->dbi_base;

	apb_base = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"pcie-apb");
	if (!apb_base) {
		dev_err(&pdev->dev, "get pcie-apb failed\n");
		return -ENODEV;
	}
	rk_pcie->apb_base = devm_ioremap_resource(&pdev->dev, apb_base);
	if (IS_ERR(rk_pcie->apb_base))
		return PTR_ERR(rk_pcie->apb_base);

	/*
	 * Rest the device before enabling power because some of the
	 * platforms may use external refclk input with the some power
	 * rail connect to 100MHz OSC chip. So once the power is up for
	 * the slot and the refclk is available, which isn't quite follow
	 * the spec. We should make sure it is in reset state before
	 * everthing's ready.
	 */
	rk_pcie->rst_gpio = devm_gpiod_get_optional(&pdev->dev, "reset",
						    GPIOD_OUT_LOW);
	if (IS_ERR(rk_pcie->rst_gpio)) {
		dev_err(&pdev->dev, "invalid reset-gpios property in node\n");
		return PTR_ERR(rk_pcie->rst_gpio);
	}

	rk_pcie->prsnt_gpio = devm_gpiod_get_optional(&pdev->dev, "prsnt", GPIOD_IN);
	if (IS_ERR_OR_NULL(rk_pcie->prsnt_gpio))
		dev_info(&pdev->dev, "invalid prsnt-gpios property in node\n");

	return 0;
}

static int rk_pcie_phy_init(struct rk_pcie *rk_pcie)
{
	int ret;
	struct device *dev = rk_pcie->pci->dev;

	rk_pcie->phy = devm_phy_get(dev, "pcie-phy");
	if (IS_ERR(rk_pcie->phy)) {
		if (PTR_ERR(rk_pcie->phy) != -EPROBE_DEFER)
			dev_info(dev, "missing phy\n");
		return PTR_ERR(rk_pcie->phy);
	}

	switch (rk_pcie->mode) {
	case RK_PCIE_RC_TYPE:
		rk_pcie->phy_mode = PHY_MODE_PCIE_RC;
		break;
	case RK_PCIE_EP_TYPE:
		rk_pcie->phy_mode = PHY_MODE_PCIE_EP;
		break;
	}

	ret = phy_set_mode(rk_pcie->phy, rk_pcie->phy_mode);
	if (ret) {
		dev_err(dev, "fail to set phy to  mode %s, err %d\n",
			(rk_pcie->phy_mode == PHY_MODE_PCIE_RC) ? "RC" : "EP",
			ret);
		return ret;
	}

	if (rk_pcie->bifurcation)
		ret = phy_set_mode(rk_pcie->phy, PHY_MODE_PCIE_BIFURCATION);

	ret = phy_init(rk_pcie->phy);
	if (ret < 0) {
		dev_err(dev, "fail to init phy, err %d\n", ret);
		return ret;
	}

	phy_power_on(rk_pcie->phy);

	return 0;
}

static int rk_pcie_reset_control_release(struct rk_pcie *rk_pcie)
{
	struct device *dev = rk_pcie->pci->dev;
	struct property *prop;
	const char *name;
	int ret, count, i = 0;

	count = of_property_count_strings(dev->of_node, "reset-names");
	if (count < 1)
		return -ENODEV;

	rk_pcie->rsts = devm_kcalloc(dev, count,
				     sizeof(struct reset_bulk_data),
				     GFP_KERNEL);
	if (!rk_pcie->rsts)
		return -ENOMEM;

	of_property_for_each_string(dev->of_node, "reset-names",
				    prop, name) {
		rk_pcie->rsts[i].id = name;
		if (!rk_pcie->rsts[i].id)
			return -ENOMEM;
		i++;
	}

	for (i = 0; i < count; i++) {
		rk_pcie->rsts[i].rst = devm_reset_control_get_exclusive(dev,
						rk_pcie->rsts[i].id);
		if (IS_ERR_OR_NULL(rk_pcie->rsts[i].rst)) {
			dev_err(dev, "failed to get %s\n",
				rk_pcie->clks[i].id);
			return -PTR_ERR(rk_pcie->rsts[i].rst);
		}
	}

	for (i = 0; i < count; i++) {
		ret = reset_control_deassert(rk_pcie->rsts[i].rst);
		if (ret) {
			dev_err(dev, "failed to release %s\n",
				rk_pcie->rsts[i].id);
			return ret;
		}
	}

	return 0;
}

static int rk_pcie_reset_grant_ctrl(struct rk_pcie *rk_pcie,
						bool enable)
{
	int ret;
	u32 val = (0x1 << 18); /* Write mask bit */

	if (enable)
		val |= (0x1 << 2);

	ret = regmap_write(rk_pcie->usb_pcie_grf, 0x0, val);
	return ret;
}

static void rk_pcie_start_dma_rd(struct dma_trx_obj *obj, struct dma_table *cur, int ctr_off)
{
	struct rk_pcie *rk_pcie = dev_get_drvdata(obj->dev);

	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_RD_ENB,
			   cur->enb.asdword);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_RD_CTRL_LO,
			   cur->ctx_reg.ctrllo.asdword);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_RD_CTRL_HI,
			   cur->ctx_reg.ctrlhi.asdword);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_RD_XFERSIZE,
			   cur->ctx_reg.xfersize);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_RD_SAR_PTR_LO,
			   cur->ctx_reg.sarptrlo);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_RD_SAR_PTR_HI,
			   cur->ctx_reg.sarptrhi);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_RD_DAR_PTR_LO,
			   cur->ctx_reg.darptrlo);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_RD_DAR_PTR_HI,
			   cur->ctx_reg.darptrhi);
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_RD_DOORBELL,
			   cur->start.asdword);
}

static void rk_pcie_start_dma_wr(struct dma_trx_obj *obj, struct dma_table *cur, int ctr_off)
{
	struct rk_pcie *rk_pcie = dev_get_drvdata(obj->dev);

	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_WR_ENB,
			   cur->enb.asdword);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_CTRL_LO,
			   cur->ctx_reg.ctrllo.asdword);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_CTRL_HI,
			   cur->ctx_reg.ctrlhi.asdword);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_XFERSIZE,
			   cur->ctx_reg.xfersize);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_SAR_PTR_LO,
			   cur->ctx_reg.sarptrlo);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_SAR_PTR_HI,
			   cur->ctx_reg.sarptrhi);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_DAR_PTR_LO,
			   cur->ctx_reg.darptrlo);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_DAR_PTR_HI,
			   cur->ctx_reg.darptrhi);
	dw_pcie_writel_dbi(rk_pcie->pci, ctr_off + PCIE_DMA_WR_WEILO,
			   cur->weilo.asdword);
	dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET + PCIE_DMA_WR_DOORBELL,
			   cur->start.asdword);
}

static void rk_pcie_start_dma_dwc(struct dma_trx_obj *obj, struct dma_table *table)
{
	int dir = table->dir;
	int chn = table->chn;

	int ctr_off = PCIE_DMA_OFFSET + chn * 0x200;

	if (dir == DMA_FROM_BUS)
		rk_pcie_start_dma_rd(obj, table, ctr_off);
	else if (dir == DMA_TO_BUS)
		rk_pcie_start_dma_wr(obj, table, ctr_off);
}

static void rk_pcie_config_dma_dwc(struct dma_table *table)
{
	table->enb.enb = 0x1;
	table->ctx_reg.ctrllo.lie = 0x1;
	table->ctx_reg.ctrllo.rie = 0x0;
	table->ctx_reg.ctrllo.td = 0x1;
	table->ctx_reg.ctrlhi.asdword = 0x0;
	table->ctx_reg.xfersize = table->buf_size;
	if (table->dir == DMA_FROM_BUS) {
		table->ctx_reg.sarptrlo = (u32)(table->bus & 0xffffffff);
		table->ctx_reg.sarptrhi = (u32)(table->bus >> 32);
		table->ctx_reg.darptrlo = (u32)(table->local & 0xffffffff);
		table->ctx_reg.darptrhi = (u32)(table->local >> 32);
	} else if (table->dir == DMA_TO_BUS) {
		table->ctx_reg.sarptrlo = (u32)(table->local & 0xffffffff);
		table->ctx_reg.sarptrhi = (u32)(table->local >> 32);
		table->ctx_reg.darptrlo = (u32)(table->bus & 0xffffffff);
		table->ctx_reg.darptrhi = (u32)(table->bus >> 32);
	}
	table->weilo.weight0 = 0x0;
	table->start.stop = 0x0;
	table->start.chnl = table->chn;
}

static irqreturn_t rk_pcie_sys_irq_handler(int irq, void *arg)
{
	struct rk_pcie *rk_pcie = arg;
	u32 chn;
	union int_status status;
	union int_clear clears;
	u32 reg, val;

	status.asdword = dw_pcie_readl_dbi(rk_pcie->pci, PCIE_DMA_OFFSET +
					   PCIE_DMA_WR_INT_STATUS);
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++) {
		if (status.donesta & BIT(chn)) {
			clears.doneclr = 0x1 << chn;
			dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET +
					PCIE_DMA_WR_INT_CLEAR, clears.asdword);
			if (rk_pcie->dma_obj && rk_pcie->dma_obj->cb)
				rk_pcie->dma_obj->cb(rk_pcie->dma_obj, chn, DMA_TO_BUS);
		}

		if (status.abortsta & BIT(chn)) {
			dev_err(rk_pcie->pci->dev, "%s, abort\n", __func__);
			clears.abortclr = 0x1 << chn;
			dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET +
					PCIE_DMA_WR_INT_CLEAR, clears.asdword);
		}
	}

	status.asdword = dw_pcie_readl_dbi(rk_pcie->pci, PCIE_DMA_OFFSET +
					   PCIE_DMA_RD_INT_STATUS);
	for (chn = 0; chn < PCIE_DMA_CHANEL_MAX_NUM; chn++) {
		if (status.donesta & BIT(chn)) {
			clears.doneclr = 0x1 << chn;
			dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET +
					PCIE_DMA_RD_INT_CLEAR, clears.asdword);
			if (rk_pcie->dma_obj && rk_pcie->dma_obj->cb)
				rk_pcie->dma_obj->cb(rk_pcie->dma_obj, chn, DMA_FROM_BUS);
		}

		if (status.abortsta & BIT(chn)) {
			dev_err(rk_pcie->pci->dev, "%s, abort\n", __func__);
			clears.abortclr = 0x1 << chn;
			dw_pcie_writel_dbi(rk_pcie->pci, PCIE_DMA_OFFSET +
					PCIE_DMA_RD_INT_CLEAR, clears.asdword);
		}
	}

	reg = rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_INTR_STATUS_MISC);
	if (reg & BIT(2)) {
		/* Setup command register */
		val = dw_pcie_readl_dbi(rk_pcie->pci, PCI_COMMAND);
		val &= 0xffff0000;
		val |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		       PCI_COMMAND_MASTER | PCI_COMMAND_SERR;
		dw_pcie_writel_dbi(rk_pcie->pci, PCI_COMMAND, val);
	}

	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_INTR_STATUS_MISC, reg);

	return IRQ_HANDLED;
}

static int rk_pcie_request_sys_irq(struct rk_pcie *rk_pcie,
					struct platform_device *pdev)
{
	int irq;
	int ret;

	irq = platform_get_irq_byname(pdev, "sys");
	if (irq < 0) {
		dev_err(rk_pcie->pci->dev, "missing sys IRQ resource\n");
		return -EINVAL;
	}

	ret = devm_request_irq(rk_pcie->pci->dev, irq, rk_pcie_sys_irq_handler,
			       IRQF_SHARED, "pcie-sys", rk_pcie);
	if (ret) {
		dev_err(rk_pcie->pci->dev, "failed to request PCIe subsystem IRQ\n");
		return ret;
	}

	return 0;
}

static const struct rk_pcie_of_data rk_pcie_rc_of_data = {
	.mode = RK_PCIE_RC_TYPE,
};

static const struct rk_pcie_of_data rk_pcie_ep_of_data = {
	.mode = RK_PCIE_EP_TYPE,
};

static const struct rk_pcie_of_data rk3528_pcie_rc_of_data = {
	.mode = RK_PCIE_RC_TYPE,
	.msi_vector_num = 8,
};

static const struct of_device_id rk_pcie_of_match[] = {
	{
		.compatible = "rockchip,rk1808-pcie",
		.data = &rk_pcie_rc_of_data,
	},
	{
		.compatible = "rockchip,rk1808-pcie-ep",
		.data = &rk_pcie_ep_of_data,
	},
	{
		.compatible = "rockchip,rk3528-pcie",
		.data = &rk3528_pcie_rc_of_data,
	},
	{
		.compatible = "rockchip,rk3568-pcie",
		.data = &rk_pcie_rc_of_data,
	},
	{
		.compatible = "rockchip,rk3568-pcie-ep",
		.data = &rk_pcie_ep_of_data,
	},
	{},
};

MODULE_DEVICE_TABLE(of, rk_pcie_of_match);

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = rk_pcie_establish_link,
	.link_up = rk_pcie_link_up,
};

static int rk1808_pcie_fixup(struct rk_pcie *rk_pcie, struct device_node *np)
{
	int ret;
	struct device *dev = rk_pcie->pci->dev;

	rk_pcie->usb_pcie_grf = syscon_regmap_lookup_by_phandle(np,
						"rockchip,usbpciegrf");
	if (IS_ERR(rk_pcie->usb_pcie_grf)) {
		dev_err(dev, "failed to find usb_pcie_grf regmap\n");
		return PTR_ERR(rk_pcie->usb_pcie_grf);
	}

	rk_pcie->pmu_grf = syscon_regmap_lookup_by_phandle(np,
							 "rockchip,pmugrf");
	if (IS_ERR(rk_pcie->pmu_grf)) {
		dev_err(dev, "failed to find pmugrf regmap\n");
		return PTR_ERR(rk_pcie->pmu_grf);
	}

	/* Workaround for pcie, switch to PCIe_PRSTNm0 */
	ret = regmap_write(rk_pcie->pmu_grf, 0x100, 0x01000100);
	if (ret)
		return ret;

	ret = regmap_write(rk_pcie->pmu_grf, 0x0, 0x0c000000);
	if (ret)
		return ret;

	/* release link reset grant */
	ret = rk_pcie_reset_grant_ctrl(rk_pcie, true);
	return ret;
}

static void rk_pcie_fast_link_setup(struct rk_pcie *rk_pcie)
{
	u32 val;

	/* LTSSM EN ctrl mode */
	val = rk_pcie_readl_apb(rk_pcie, PCIE_CLIENT_HOT_RESET_CTRL);
	val |= PCIE_LTSSM_ENABLE_ENHANCE | (PCIE_LTSSM_ENABLE_ENHANCE << 16);
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_HOT_RESET_CTRL, val);
}

static void rk_pcie_legacy_irq_mask(struct irq_data *d)
{
	struct rk_pcie *rk_pcie = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&rk_pcie->intx_lock, flags);
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_INTR_MASK_LEGACY,
			   MASK_LEGACY_INT(d->hwirq));
	raw_spin_unlock_irqrestore(&rk_pcie->intx_lock, flags);
}

static void rk_pcie_legacy_irq_unmask(struct irq_data *d)
{
	struct rk_pcie *rk_pcie = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&rk_pcie->intx_lock, flags);
	rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_INTR_MASK_LEGACY,
			   UNMASK_LEGACY_INT(d->hwirq));
	raw_spin_unlock_irqrestore(&rk_pcie->intx_lock, flags);
}

static struct irq_chip rk_pcie_legacy_irq_chip = {
	.name		= "rk-pcie-legacy-int",
	.irq_enable	= rk_pcie_legacy_irq_unmask,
	.irq_disable	= rk_pcie_legacy_irq_mask,
	.irq_mask	= rk_pcie_legacy_irq_mask,
	.irq_unmask	= rk_pcie_legacy_irq_unmask,
	.flags		= IRQCHIP_SKIP_SET_WAKE | IRQCHIP_MASK_ON_SUSPEND,
};

static int rk_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			    irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &rk_pcie_legacy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = rk_pcie_intx_map,
};

static void rk_pcie_legacy_int_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct rk_pcie *rockchip = irq_desc_get_handler_data(desc);
	struct device *dev = rockchip->pci->dev;
	u32 reg;
	u32 hwirq;
	u32 virq;

	chained_irq_enter(chip, desc);

	reg = rk_pcie_readl_apb(rockchip, PCIE_CLIENT_INTR_STATUS_LEGACY);
	reg = reg & 0xf;

	while (reg) {
		hwirq = ffs(reg) - 1;
		reg &= ~BIT(hwirq);

		virq = irq_find_mapping(rockchip->irq_domain, hwirq);
		if (virq)
			generic_handle_irq(virq);
		else
			dev_err(dev, "unexpected IRQ, INT%d\n", hwirq);
	}

	chained_irq_exit(chip, desc);
}

static int rk_pcie_init_irq_domain(struct rk_pcie *rockchip)
{
	struct device *dev = rockchip->pci->dev;
	struct device_node *intc = of_get_next_child(dev->of_node, NULL);

	if (!intc) {
		dev_err(dev, "missing child interrupt-controller node\n");
		return -EINVAL;
	}

	raw_spin_lock_init(&rockchip->intx_lock);
	rockchip->irq_domain = irq_domain_add_linear(intc, PCI_NUM_INTX,
						     &intx_domain_ops, rockchip);
	if (!rockchip->irq_domain) {
		dev_err(dev, "failed to get a INTx IRQ domain\n");
		return -EINVAL;
	}

	return 0;
}

static int rk_pcie_enable_power(struct rk_pcie *rk_pcie)
{
	int ret = 0;
	struct device *dev = rk_pcie->pci->dev;

	if (IS_ERR(rk_pcie->vpcie3v3))
		return ret;

	ret = regulator_enable(rk_pcie->vpcie3v3);
	if (ret)
		dev_err(dev, "fail to enable vpcie3v3 regulator\n");

	return ret;
}

static int rk_pcie_disable_power(struct rk_pcie *rk_pcie)
{
	int ret = 0;
	struct device *dev = rk_pcie->pci->dev;

	if (IS_ERR(rk_pcie->vpcie3v3))
		return ret;

	ret = regulator_disable(rk_pcie->vpcie3v3);
	if (ret)
		dev_err(dev, "fail to disable vpcie3v3 regulator\n");

	return ret;
}

static int rk_pci_find_capability(struct rk_pcie *rk_pcie, int cap)
{
	u32 header;
	int ttl;
	int start = 0;
	int pos = PCI_CFG_SPACE_SIZE;

	/* minimum 8 bytes per capability */
	ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;

	header = dw_pcie_readl_dbi(rk_pcie->pci, pos);

	/*
	 * If we have no capabilities, this is indicated by cap ID,
	 * cap version and next pointer all being 0.
	 */
	if (header == 0)
		return 0;

	while (ttl-- > 0) {
		if (PCI_EXT_CAP_ID(header) == cap && pos != start)
			return pos;

		pos = PCI_EXT_CAP_NEXT(header);
		if (pos < PCI_CFG_SPACE_SIZE)
			break;

		header = dw_pcie_readl_dbi(rk_pcie->pci, pos);
		if (!header)
			break;
	}

	return 0;
}

#define RAS_DES_EVENT(ss, v) \
do { \
	dw_pcie_writel_dbi(pcie->pci, cap_base + 8, v); \
	seq_printf(s, ss "0x%x\n", dw_pcie_readl_dbi(pcie->pci, cap_base + 0xc)); \
} while (0)

static int rockchip_pcie_rasdes_show(struct seq_file *s, void *unused)
{
	struct rk_pcie *pcie = s->private;
	int cap_base;

	cap_base = rk_pci_find_capability(pcie, PCI_EXT_CAP_ID_VNDR);
	if (!cap_base) {
		dev_err(pcie->pci->dev, "Not able to find RASDES CAP!\n");
		return 0;
	}

	RAS_DES_EVENT("EBUF Overflow: ", 0);
	RAS_DES_EVENT("EBUF Under-run: ", 0x0010000);
	RAS_DES_EVENT("Decode Error: ", 0x0020000);
	RAS_DES_EVENT("Running Disparity Error: ", 0x0030000);
	RAS_DES_EVENT("SKP OS Parity Error: ", 0x0040000);
	RAS_DES_EVENT("SYNC Header Error: ", 0x0050000);
	RAS_DES_EVENT("CTL SKP OS Parity Error: ", 0x0060000);
	RAS_DES_EVENT("Detect EI Infer: ", 0x1050000);
	RAS_DES_EVENT("Receiver Error: ", 0x1060000);
	RAS_DES_EVENT("Rx Recovery Request: ", 0x1070000);
	RAS_DES_EVENT("N_FTS Timeout: ", 0x1080000);
	RAS_DES_EVENT("Framing Error: ", 0x1090000);
	RAS_DES_EVENT("Deskew Error: ", 0x10a0000);
	RAS_DES_EVENT("BAD TLP: ", 0x2000000);
	RAS_DES_EVENT("LCRC Error: ", 0x2010000);
	RAS_DES_EVENT("BAD DLLP: ", 0x2020000);
	RAS_DES_EVENT("Replay Number Rollover: ", 0x2030000);
	RAS_DES_EVENT("Replay Timeout: ", 0x2040000);
	RAS_DES_EVENT("Rx Nak DLLP: ", 0x2050000);
	RAS_DES_EVENT("Tx Nak DLLP: ", 0x2060000);
	RAS_DES_EVENT("Retry TLP: ", 0x2070000);
	RAS_DES_EVENT("FC Timeout: ", 0x3000000);
	RAS_DES_EVENT("Poisoned TLP: ", 0x3010000);
	RAS_DES_EVENT("ECRC Error: ", 0x3020000);
	RAS_DES_EVENT("Unsupported Request: ", 0x3030000);
	RAS_DES_EVENT("Completer Abort: ", 0x3040000);
	RAS_DES_EVENT("Completion Timeout: ", 0x3050000);

	return 0;
}

static int rockchip_pcie_rasdes_open(struct inode *inode, struct file *file)
{
	return single_open(file, rockchip_pcie_rasdes_show,
			   inode->i_private);
}

static ssize_t rockchip_pcie_rasdes_write(struct file *file,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct rk_pcie *pcie = s->private;
	char buf[32];
	int cap_base;

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count)))
		return -EFAULT;

	cap_base = rk_pci_find_capability(pcie, PCI_EXT_CAP_ID_VNDR);
	if (!cap_base) {
		dev_err(pcie->pci->dev, "Not able to find RASDES CAP!\n");
		return 0;
	}

	if (!strncmp(buf, "enable", 6))	{
		dev_info(pcie->pci->dev, "RAS DES Event: Enable ALL!\n");
		dw_pcie_writel_dbi(pcie->pci, cap_base + 8, 0x1c);
		dw_pcie_writel_dbi(pcie->pci, cap_base + 8, 0x3);
	} else if (!strncmp(buf, "disable", 7)) {
		dev_info(pcie->pci->dev, "RAS DES Event: disable ALL!\n");
		dw_pcie_writel_dbi(pcie->pci, cap_base + 8, 0x14);
	} else if (!strncmp(buf, "clear", 5)) {
		dev_info(pcie->pci->dev, "RAS DES Event: Clear ALL!\n");
		dw_pcie_writel_dbi(pcie->pci, cap_base + 8, 0x3);
	} else {
		dev_info(pcie->pci->dev, "Not support command!\n");
	}

	return count;
}

static const struct file_operations rockchip_pcie_rasdes_ops = {
	.owner = THIS_MODULE,
	.open = rockchip_pcie_rasdes_open,
	.read = seq_read,
	.write = rockchip_pcie_rasdes_write,
};

static int rockchip_pcie_fifo_show(struct seq_file *s, void *data)
{
	struct rk_pcie *pcie = (struct rk_pcie *)dev_get_drvdata(s->private);
	u32 loop;

	seq_printf(s, "ltssm = 0x%x\n",
		   rk_pcie_readl_apb(pcie, PCIE_CLIENT_LTSSM_STATUS));
	for (loop = 0; loop < 64; loop++)
		seq_printf(s, "fifo_status = 0x%x\n",
			   rk_pcie_readl_apb(pcie, PCIE_CLIENT_DBG_FIFO_STATUS));

	return 0;
}

static void rockchip_pcie_debugfs_exit(struct rk_pcie *pcie)
{
	debugfs_remove_recursive(pcie->debugfs);
	pcie->debugfs = NULL;
}

static int rockchip_pcie_debugfs_init(struct rk_pcie *pcie)
{
	struct dentry *file;

	pcie->debugfs = debugfs_create_dir(dev_name(pcie->pci->dev), NULL);
	if (!pcie->debugfs)
		return -ENOMEM;

	debugfs_create_devm_seqfile(pcie->pci->dev, "dumpfifo",
				    pcie->debugfs,
				    rockchip_pcie_fifo_show);
	file = debugfs_create_file("err_event", 0644, pcie->debugfs,
				   pcie, &rockchip_pcie_rasdes_ops);
	if (!file)
		goto remove;

	return 0;

remove:
	rockchip_pcie_debugfs_exit(pcie);

	return -ENOMEM;
}

static int rk_pcie_really_probe(void *p)
{
	struct platform_device *pdev = p;
	struct device *dev = &pdev->dev;
	struct rk_pcie *rk_pcie;
	struct dw_pcie *pci;
	int ret;
	const struct of_device_id *match;
	const struct rk_pcie_of_data *data;
	enum rk_pcie_device_mode mode;
	struct device_node *np = pdev->dev.of_node;
	struct platform_driver *drv = to_platform_driver(dev->driver);
	u32 val = 0;
	int irq;

	match = of_match_device(rk_pcie_of_match, dev);
	if (!match) {
		ret = -EINVAL;
		goto release_driver;
	}

	data = (struct rk_pcie_of_data *)match->data;
	mode = (enum rk_pcie_device_mode)data->mode;

	rk_pcie = devm_kzalloc(dev, sizeof(*rk_pcie), GFP_KERNEL);
	if (!rk_pcie) {
		ret = -ENOMEM;
		goto release_driver;
	}

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci) {
		ret = -ENOMEM;
		goto release_driver;
	}

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	rk_pcie->mode = mode;
	rk_pcie->msi_vector_num = data->msi_vector_num;
	rk_pcie->pci = pci;

	if (of_device_is_compatible(np, "rockchip,rk1808-pcie") ||
	    of_device_is_compatible(np, "rockchip,rk1808-pcie-ep"))
		rk_pcie->is_rk1808 = true;
	else
		rk_pcie->is_rk1808 = false;

	if (device_property_read_bool(dev, "rockchip,bifurcation"))
		rk_pcie->bifurcation = true;

	ret = rk_pcie_resource_get(pdev, rk_pcie);
	if (ret) {
		dev_err(dev, "resource init failed\n");
		goto release_driver;
	}

	if (!IS_ERR_OR_NULL(rk_pcie->prsnt_gpio)) {
		if (!gpiod_get_value(rk_pcie->prsnt_gpio)) {
			ret = -ENODEV;
			goto release_driver;
		}
	}

retry_regulator:
	/* DON'T MOVE ME: must be enable before phy init */
	rk_pcie->vpcie3v3 = devm_regulator_get_optional(dev, "vpcie3v3");
	if (IS_ERR(rk_pcie->vpcie3v3)) {
		if (PTR_ERR(rk_pcie->vpcie3v3) != -ENODEV) {
			if (IS_ENABLED(CONFIG_PCIE_RK_THREADED_INIT)) {
				/* Deferred but in threaded context for most 10s */
				msleep(20);
				if (++val < 500)
					goto retry_regulator;
			}

			ret = PTR_ERR(rk_pcie->vpcie3v3);
			goto release_driver;
		}

		dev_info(dev, "no vpcie3v3 regulator found\n");
	}

	ret = rk_pcie_enable_power(rk_pcie);
	if (ret)
		goto release_driver;

	ret = rk_pcie_phy_init(rk_pcie);
	if (ret) {
		dev_err(dev, "phy init failed\n");
		goto disable_vpcie3v3;
	}

	ret = rk_pcie_reset_control_release(rk_pcie);
	if (ret) {
		dev_err(dev, "reset control init failed\n");
		goto disable_phy;
	}

	ret = rk_pcie_request_sys_irq(rk_pcie, pdev);
	if (ret) {
		dev_err(dev, "pcie irq init failed\n");
		goto disable_phy;
	}

	platform_set_drvdata(pdev, rk_pcie);

	ret = rk_pcie_clk_init(rk_pcie);
	if (ret) {
		dev_err(dev, "clock init failed\n");
		goto disable_phy;
	}

	dw_pcie_dbi_ro_wr_en(pci);

	if (rk_pcie->is_rk1808) {
		ret = rk1808_pcie_fixup(rk_pcie, np);
		if (ret)
			goto deinit_clk;
	} else {
		rk_pcie_fast_link_setup(rk_pcie);
	}

	/* Legacy interrupt is optional */
	ret = rk_pcie_init_irq_domain(rk_pcie);
	if (!ret) {
		irq = platform_get_irq_byname(pdev, "legacy");
		if (irq >= 0) {
			irq_set_chained_handler_and_data(irq, rk_pcie_legacy_int_handler,
							 rk_pcie);
			/* Unmask all legacy interrupt from INTA~INTD  */
			rk_pcie_writel_apb(rk_pcie, PCIE_CLIENT_INTR_MASK_LEGACY,
					   UNMASK_ALL_LEGACY_INT);
		} else {
			dev_info(dev, "missing legacy IRQ resource\n");
		}
	}

	/* Set PCIe mode */
	rk_pcie_set_mode(rk_pcie);

	/* Force into loopback master mode */
	if (device_property_read_bool(dev, "rockchip,lpbk-master")) {
		val = dw_pcie_readl_dbi(pci, PCIE_PORT_LINK_CONTROL);
		val |= PORT_LINK_LPBK_ENABLE;
		dw_pcie_writel_dbi(pci, PCIE_PORT_LINK_CONTROL, val);
		rk_pcie->is_signal_test = true;
	}

	/* Force into compliance mode */
	if (device_property_read_bool(dev, "rockchip,compliance-mode")) {
		val = dw_pcie_readl_dbi(pci, PCIE_CAP_LINK_CONTROL2_LINK_STATUS);
		val |= BIT(4);
		dw_pcie_writel_dbi(pci, PCIE_CAP_LINK_CONTROL2_LINK_STATUS, val);
		rk_pcie->is_signal_test = true;
	}

	/* Skip waiting for training to pass in system PM routine */
	if (device_property_read_bool(dev, "rockchip,skip-scan-in-resume"))
		rk_pcie->skip_scan_in_resume = true;

	switch (rk_pcie->mode) {
	case RK_PCIE_RC_TYPE:
		ret = rk_add_pcie_port(rk_pcie, pdev);
		break;
	case RK_PCIE_EP_TYPE:
		ret = rk_pcie_add_ep(rk_pcie);
		break;
	}

	if (rk_pcie->is_signal_test == true)
		return 0;

	if (ret)
		goto remove_irq_domain;

	ret = rk_pcie_init_dma_trx(rk_pcie);
	if (ret) {
		dev_err(dev, "failed to add dma extension\n");
		return ret;
	}

	if (rk_pcie->dma_obj) {
		rk_pcie->dma_obj->start_dma_func = rk_pcie_start_dma_dwc;
		rk_pcie->dma_obj->config_dma_func = rk_pcie_config_dma_dwc;
	}

	if (rk_pcie->is_rk1808) {
		/* hold link reset grant after link-up */
		ret = rk_pcie_reset_grant_ctrl(rk_pcie, false);
		if (ret)
			goto remove_irq_domain;
	}

	dw_pcie_dbi_ro_wr_dis(pci);

	device_init_wakeup(dev, true);
	drv->driver.pm = &rockchip_dw_pcie_pm_ops;

	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		ret = rockchip_pcie_debugfs_init(rk_pcie);
		if (ret < 0)
			dev_err(dev, "failed to setup debugfs: %d\n", ret);

		/* Enable RASDES Error event by default */
		val = rk_pci_find_capability(rk_pcie, PCI_EXT_CAP_ID_VNDR);
		if (!val) {
			dev_err(dev, "Not able to find RASDES CAP!\n");
			return 0;
		}

		dw_pcie_writel_dbi(rk_pcie->pci, val + 8, 0x1c);
		dw_pcie_writel_dbi(rk_pcie->pci, val + 8, 0x3);
	}

	return 0;

remove_irq_domain:
	if (rk_pcie->irq_domain)
		irq_domain_remove(rk_pcie->irq_domain);
disable_phy:
	phy_power_off(rk_pcie->phy);
	phy_exit(rk_pcie->phy);
deinit_clk:
	rk_pcie_clk_deinit(rk_pcie);
disable_vpcie3v3:
	rk_pcie_disable_power(rk_pcie);

release_driver:
	if (IS_ENABLED(CONFIG_PCIE_RK_THREADED_INIT))
		device_release_driver(dev);

	return ret;
}

static int rk_pcie_probe(struct platform_device *pdev)
{
	if (IS_ENABLED(CONFIG_PCIE_RK_THREADED_INIT)) {
		struct task_struct *tsk;

		tsk = kthread_run(rk_pcie_really_probe, pdev, "rk-pcie");
		if (IS_ERR(tsk)) {
			dev_err(&pdev->dev, "start rk-pcie thread failed\n");
			return PTR_ERR(tsk);
		}

		return 0;
	}

	return rk_pcie_really_probe(pdev);
}

static int __maybe_unused rockchip_dw_pcie_suspend(struct device *dev)
{
	struct rk_pcie *rk_pcie = dev_get_drvdata(dev);
	int ret;

	rk_pcie_link_status_clear(rk_pcie);
	rk_pcie_disable_ltssm(rk_pcie);

	/* make sure assert phy success */
	usleep_range(200, 300);

	phy_power_off(rk_pcie->phy);
	phy_exit(rk_pcie->phy);

	clk_bulk_disable(rk_pcie->clk_cnt, rk_pcie->clks);

	rk_pcie->in_suspend = true;

	gpiod_set_value_cansleep(rk_pcie->rst_gpio, 0);
	ret = rk_pcie_disable_power(rk_pcie);

	return ret;
}

static int __maybe_unused rockchip_dw_pcie_resume(struct device *dev)
{
	struct rk_pcie *rk_pcie = dev_get_drvdata(dev);
	bool std_rc = rk_pcie->mode == RK_PCIE_RC_TYPE && !rk_pcie->dma_obj;
	int ret;

	ret = rk_pcie_enable_power(rk_pcie);
	if (ret)
		return ret;

	ret = clk_bulk_enable(rk_pcie->clk_cnt, rk_pcie->clks);
	if (ret) {
		clk_bulk_unprepare(rk_pcie->clk_cnt, rk_pcie->clks);
		return ret;
	}

	ret = phy_set_mode(rk_pcie->phy, rk_pcie->phy_mode);
	if (ret) {
		dev_err(dev, "fail to set phy to mode %s, err %d\n",
			(rk_pcie->phy_mode == PHY_MODE_PCIE_RC) ? "RC" : "EP",
			ret);
		return ret;
	}

	ret = phy_init(rk_pcie->phy);
	if (ret < 0) {
		dev_err(dev, "fail to init phy, err %d\n", ret);
		return ret;
	}

	phy_power_on(rk_pcie->phy);

	dw_pcie_dbi_ro_wr_en(rk_pcie->pci);

	if (rk_pcie->is_rk1808) {
		/* release link reset grant */
		ret = rk_pcie_reset_grant_ctrl(rk_pcie, true);
		if (ret)
			return ret;
	} else {
		rk_pcie_fast_link_setup(rk_pcie);
	}

	/* Set PCIe mode */
	rk_pcie_set_mode(rk_pcie);

	if (std_rc)
		dw_pcie_setup_rc(&rk_pcie->pci->pp);

	ret = rk_pcie_establish_link(rk_pcie->pci);
	if (ret) {
		dev_err(dev, "failed to establish pcie link\n");
		goto err;
	}

	if (std_rc)
		goto std_rc_done;

	ret = rk_pcie_ep_atu_init(rk_pcie);
	if (ret) {
		dev_err(dev, "failed to init ep device\n");
		goto err;
	}

	rk_pcie_ep_setup(rk_pcie);

	rk_pcie->in_suspend = false;

std_rc_done:
	dw_pcie_dbi_ro_wr_dis(rk_pcie->pci);
	/* hold link reset grant after link-up */
	if (rk_pcie->is_rk1808) {
		ret = rk_pcie_reset_grant_ctrl(rk_pcie, false);
		if (ret)
			goto err;
	}

	if (rk_pcie->pci->pp.msi_irq > 0)
		dw_pcie_msi_init(&rk_pcie->pci->pp);

	return 0;
err:
	rk_pcie_disable_power(rk_pcie);

	return ret;
}

static const struct dev_pm_ops rockchip_dw_pcie_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(rockchip_dw_pcie_suspend,
				      rockchip_dw_pcie_resume)
};

static struct platform_driver rk_plat_pcie_driver = {
	.driver = {
		.name	= "rk-pcie",
		.of_match_table = rk_pcie_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver_probe(rk_plat_pcie_driver, rk_pcie_probe);

MODULE_AUTHOR("Simon Xue <xxm@rock-chips.com>");
MODULE_DESCRIPTION("RockChip PCIe Controller driver");
MODULE_LICENSE("GPL v2");
