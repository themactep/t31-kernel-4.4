/*
 * drivers/video/fbdev/ingenic/x2000_v12/ingenicfb.c
 *
 * Copyright (c) 2012 Ingenic Semiconductor Co., Ltd.
 *              http://www.ingenic.com/
 *
 * Core file for Ingenic Display Controller driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/ctype.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/memory.h>
#include <linux/suspend.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/gpio.h>
#include <asm/cacheflush.h>
#include <linux/of_address.h>
#include "dpu_reg.h"
#include "ingenicfb.h"
#include <libdmmu.h>

#define dpu_debug 1
#define print_dbg(f, arg...) if(dpu_debug) printk(KERN_INFO "dpu: %s, %d: " f "\n", __func__, __LINE__, ## arg)

#define COMPOSER_DIRECT_OUT_EN

#ifdef CONFIG_FB_INGENIC_MIPI_DSI
#include "./jz_mipi_dsi/jz_mipi_dsih_hal.h"
#include "./jz_mipi_dsi/jz_mipi_dsi_regs.h"
#include "./jz_mipi_dsi/jz_mipi_dsi_lowlevel.h"
extern struct dsi_device * jzdsi_init(struct jzdsi_data *pdata);
extern void jzdsi_remove(struct dsi_device *dsi);
extern void dump_dsi_reg(struct dsi_device *dsi);
int jz_mipi_dsi_set_client(struct dsi_device *dsi, int power);
#endif

#ifdef CONFIG_TRUE_COLOR_LOGO
extern void show_logo(struct fb_info *info);
#endif

struct lcd_panel *fbdev_panel = NULL;
struct platform_device *fbdev_pdev = NULL;
static unsigned int cmp_gen_sop = 0;
static int uboot_inited;
static int showFPS = 0;
static int over_cnt = 0;
static struct ingenicfb_device *fbdev;

#ifdef CONFIG_FB_VSYNC_SKIP_DISABLE
static unsigned int old_desc_addr = 0;
#endif

/* #define TEST_IRQ */

static const struct fb_fix_screeninfo ingenicfb_fix  = {
	.id = "ingenicfb",
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR,
	.xpanstep = 0,
	.ypanstep = 1,
	.ywrapstep = 0,
	.accel = FB_ACCEL_NONE,
};

struct ingenicfb_colormode {
	uint32_t mode;
	const char *name;
	uint32_t color;
	uint32_t bits_per_pixel;
	uint32_t nonstd;
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;
};

static struct ingenicfb_colormode ingenicfb_colormodes[] = {
	{
		.mode = LAYER_CFG_FORMAT_RGB888,
		.name = "rgb888",
		.color = LAYER_CFG_COLOR_RGB,
		.bits_per_pixel = 32,
		.nonstd = 0,
#ifdef CONFIG_ANDROID
		.color = LAYER_CFG_COLOR_BGR,
		.red	= { .length = 8, .offset = 0, .msb_right = 0 },
		.green	= { .length = 8, .offset = 8, .msb_right = 0 },
		.blue	= { .length = 8, .offset = 16, .msb_right = 0 },
#else
		.color = LAYER_CFG_COLOR_RGB,
		.red	= { .length = 8, .offset = 16, .msb_right = 0 },
		.green	= { .length = 8, .offset = 8, .msb_right = 0 },
		.blue	= { .length = 8, .offset = 0, .msb_right = 0 },
#endif
		.transp	= { .length = 0, .offset = 0, .msb_right = 0 },
	}, {
		.mode = LAYER_CFG_FORMAT_ARGB8888,
		.name = "argb888",
		.bits_per_pixel = 32,
		.nonstd = 0,
#ifdef CONFIG_ANDROID
		.color = LAYER_CFG_COLOR_BGR,
		.red	= { .length = 8, .offset = 0, .msb_right = 0 },
		.green	= { .length = 8, .offset = 8, .msb_right = 0 },
		.blue	= { .length = 8, .offset = 16, .msb_right = 0 },
#else
		.color = LAYER_CFG_COLOR_RGB,
		.red	= { .length = 8, .offset = 16, .msb_right = 0 },
		.green	= { .length = 8, .offset = 8, .msb_right = 0 },
		.blue	= { .length = 8, .offset = 0, .msb_right = 0 },
#endif
		.transp	= { .length = 8, .offset = 24, .msb_right = 0 },
	}, {
		.mode = LAYER_CFG_FORMAT_RGB555,
		.name = "rgb555",
		.color = LAYER_CFG_COLOR_RGB,
		.bits_per_pixel = 16,
		.nonstd = 0,
		.red	= { .length = 5, .offset = 10, .msb_right = 0 },
		.green	= { .length = 5, .offset = 5, .msb_right = 0 },
		.blue	= { .length = 5, .offset = 0, .msb_right = 0 },
		.transp	= { .length = 0, .offset = 0, .msb_right = 0 },
	}, {
		.mode = LAYER_CFG_FORMAT_ARGB1555,
		.name = "argb1555",
		.color = LAYER_CFG_COLOR_RGB,
		.bits_per_pixel = 16,
		.nonstd = 0,
		.red	= { .length = 5, .offset = 10, .msb_right = 0 },
		.green	= { .length = 5, .offset = 5, .msb_right = 0 },
		.blue	= { .length = 5, .offset = 0, .msb_right = 0 },
		.transp	= { .length = 1, .offset = 15, .msb_right = 0 },
	}, {
		.mode = LAYER_CFG_FORMAT_RGB565,
		.name = "rgb565",
		.color = LAYER_CFG_COLOR_RGB,
		.bits_per_pixel = 16,
		.nonstd = 0,
		.red	= { .length = 5, .offset = 11, .msb_right = 0 },
		.green	= { .length = 6, .offset = 5, .msb_right = 0 },
		.blue	= { .length = 5, .offset = 0, .msb_right = 0 },
		.transp	= { .length = 0, .offset = 0, .msb_right = 0 },
	}, {
		.mode = LAYER_CFG_FORMAT_YUV422,
		.name = "yuv422",
		.bits_per_pixel = 16,
		.nonstd = LAYER_CFG_FORMAT_YUV422,
	}, {
		.mode = LAYER_CFG_FORMAT_NV12,
		.name = "nv12",
		.bits_per_pixel = 16,
		.nonstd = LAYER_CFG_FORMAT_NV12,
	}, {
		.mode = LAYER_CFG_FORMAT_NV21,
		.name = "nv21",
		.bits_per_pixel = 16,
		.nonstd = LAYER_CFG_FORMAT_NV21,
	},
};

static void dump_dc_reg(void)
{
	printk("-----------------dc_reg------------------\n");
	printk("DC_FRM_CFG_ADDR(0x%04x):    0x%08lx\n", DC_FRM_CFG_ADDR,reg_read(fbdev, DC_FRM_CFG_ADDR));
	printk("DC_FRM_CFG_CTRL(0x%04x):    0x%08lx\n", DC_FRM_CFG_CTRL,reg_read(fbdev, DC_FRM_CFG_CTRL));
	printk("DC_RDMA_CHAIN_ADDR(0x%04x):    0x%08lx\n", DC_RDMA_CHAIN_ADDR,reg_read(fbdev, DC_RDMA_CHAIN_ADDR));
	printk("DC_RDMA_CHAIN_CTRL(0x%04x):    0x%08lx\n", DC_RDMA_CHAIN_CTRL,reg_read(fbdev, DC_RDMA_CHAIN_CTRL));
	printk("DC_CTRL(0x%04x):            0x%08lx\n", DC_CTRL, reg_read(fbdev, DC_CTRL));
	printk("DC_CSC_MULT_YRV(0x%04x):    0x%08lx\n", DC_CSC_MULT_YRV,reg_read(fbdev, DC_CSC_MULT_YRV));
	printk("DC_CSC_MULT_GUGV(0x%04x):   0x%08lx\n", DC_CSC_MULT_GUGV,reg_read(fbdev, DC_CSC_MULT_GUGV));
	printk("DC_CSC_MULT_BU(0x%04x):     0x%08lx\n", DC_CSC_MULT_BU,reg_read(fbdev, DC_CSC_MULT_BU));
	printk("DC_CSC_SUB_YUV(0x%04x):     0x%08lx\n", DC_CSC_SUB_YUV,reg_read(fbdev, DC_CSC_SUB_YUV));
	printk("DC_ST(0x%04x):              0x%08lx\n", DC_ST, reg_read(fbdev, DC_ST));
	printk("DC_INTC(0x%04x):            0x%08lx\n", DC_INTC, reg_read(fbdev, DC_INTC));
	printk("DC_INT_FLAG(0x%04x):	   0x%08lx\n", DC_INT_FLAG, reg_read(fbdev, DC_INT_FLAG));
	printk("DC_COM_CONFIG(0x%04x):      0x%08lx\n", DC_COM_CONFIG ,reg_read(fbdev, DC_COM_CONFIG));
	printk("DC_TLB_GLBC(0x%04x):        0x%08lx\n", DC_TLB_GLBC,reg_read(fbdev, DC_TLB_GLBC));
	printk("DC_TLB_TLBA(0x%04x):        0x%08lx\n", DC_TLB_TLBA,reg_read(fbdev, DC_TLB_TLBA));
	printk("DC_TLB_TLBC(0x%04x):        0x%08lx\n", DC_TLB_TLBC,reg_read(fbdev, DC_TLB_TLBC));
	printk("DC_TLB0_VPN(0x%04x):        0x%08lx\n", DC_TLB0_VPN,reg_read(fbdev, DC_TLB0_VPN));
	printk("DC_TLB1_VPN(0x%04x):        0x%08lx\n", DC_TLB1_VPN,reg_read(fbdev, DC_TLB1_VPN));
	printk("DC_TLB2_VPN(0x%04x):        0x%08lx\n", DC_TLB2_VPN,reg_read(fbdev, DC_TLB2_VPN));
	printk("DC_TLB3_VPN(0x%04x):        0x%08lx\n", DC_TLB3_VPN,reg_read(fbdev, DC_TLB3_VPN));
	printk("DC_TLB_TLBV(0x%04x):        0x%08lx\n", DC_TLB_TLBV,reg_read(fbdev, DC_TLB_TLBV));
	printk("DC_TLB_STAT(0x%04x):        0x%08lx\n", DC_TLB_STAT,reg_read(fbdev, DC_TLB_STAT));
	printk("DC_PCFG_RD_CTRL(0x%04x):    0x%08lx\n", DC_PCFG_RD_CTRL,reg_read(fbdev, DC_PCFG_RD_CTRL));
	printk("DC_PCFG_WR_CTRL(0x%04x):    0x%08lx\n", DC_PCFG_WR_CTRL,reg_read(fbdev, DC_PCFG_WR_CTRL));
	printk("DC_OFIFO_PCFG(0x%04x):	   0x%08lx\n", DC_OFIFO_PCFG,reg_read(fbdev, DC_OFIFO_PCFG));
	printk("DC_WDMA_PCFG(0x%04x):	   0x%08lx\n", DC_WDMA_PCFG,reg_read(fbdev, DC_WDMA_PCFG));
	printk("DC_CMPW_PCFG_CTRL(0x%04x): 0x%08lx\n", DC_CMPW_PCFG_CTRL,reg_read(fbdev, DC_CMPW_PCFG_CTRL));
	printk("DC_CMPW_PCFG0(0x%04x):	   0x%08lx\n", DC_CMPW_PCFG0,reg_read(fbdev, DC_CMPW_PCFG0));
	printk("DC_CMPW_PCFG1(0x%04x):	   0x%08lx\n", DC_CMPW_PCFG1,reg_read(fbdev, DC_CMPW_PCFG1));
	printk("DC_CMPW_PCFG2(0x%04x):	   0x%08lx\n", DC_CMPW_PCFG2,reg_read(fbdev, DC_CMPW_PCFG2));
	printk("DC_PCFG_RD_CTRL(0x%04x):    0x%08lx\n", DC_PCFG_RD_CTRL,reg_read(fbdev, DC_PCFG_RD_CTRL));
	printk("DC_OFIFO_PCFG(0x%04x):	   0x%08lx\n", DC_OFIFO_PCFG,reg_read(fbdev, DC_OFIFO_PCFG));
	printk("DC_DISP_COM(0x%04x):        0x%08lx\n", DC_DISP_COM,reg_read(fbdev, DC_DISP_COM));
	printk("-----------------dc_reg------------------\n");
}

static void dump_tft_reg(void)
{
	printk("----------------tft_reg------------------\n");
	printk("TFT_TIMING_HSYNC(0x%04x):   0x%08lx\n", DC_TFT_HSYNC, reg_read(fbdev, DC_TFT_HSYNC));
	printk("TFT_TIMING_VSYNC(0x%04x):   0x%08lx\n", DC_TFT_VSYNC, reg_read(fbdev, DC_TFT_VSYNC));
	printk("TFT_TIMING_HDE(0x%04x):     0x%08lx\n", DC_TFT_HDE, reg_read(fbdev, DC_TFT_HDE));
	printk("TFT_TIMING_VDE(0x%04x):     0x%08lx\n", DC_TFT_VDE, reg_read(fbdev, DC_TFT_VDE));
	printk("TFT_TRAN_CFG(0x%04x):       0x%08lx\n", DC_TFT_CFG, reg_read(fbdev, DC_TFT_CFG));
	printk("TFT_ST(0x%04x):             0x%08lx\n", DC_TFT_ST, reg_read(fbdev, DC_TFT_ST));
	printk("----------------tft_reg------------------\n");
}

static void dump_slcd_reg(void)
{
	printk("---------------slcd_reg------------------\n");
	printk("SLCD_CFG(0x%04x):           0x%08lx\n", DC_SLCD_CFG, reg_read(fbdev, DC_SLCD_CFG));
	printk("SLCD_WR_DUTY(0x%04x):       0x%08lx\n", DC_SLCD_WR_DUTY, reg_read(fbdev, DC_SLCD_WR_DUTY));
	printk("SLCD_TIMING(0x%04x):        0x%08lx\n", DC_SLCD_TIMING, reg_read(fbdev, DC_SLCD_TIMING));
	printk("SLCD_FRM_SIZE(0x%04x):      0x%08lx\n", DC_SLCD_FRM_SIZE, reg_read(fbdev, DC_SLCD_FRM_SIZE));
	printk("SLCD_SLOW_TIME(0x%04x):     0x%08lx\n", DC_SLCD_SLOW_TIME, reg_read(fbdev, DC_SLCD_SLOW_TIME));
	printk("SLCD_REG_IF(0x%04x):	    0x%08lx\n", DC_SLCD_REG_IF, reg_read(fbdev, DC_SLCD_REG_IF));
	printk("SLCD_ST(0x%04x):            0x%08lx\n", DC_SLCD_ST, reg_read(fbdev, DC_SLCD_ST));
	printk("---------------slcd_reg------------------\n");
}

static void dump_frm_desc_reg(void)
{
	unsigned int ctrl;
	ctrl = reg_read(fbdev, DC_CTRL);
	ctrl |= DC_DES_CNT_RST;
	reg_write(fbdev, DC_CTRL, ctrl);

	printk("--------Frame Descriptor register--------\n");
	printk("FrameNextCfgAddr:   %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("FrameSize:          %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("FrameCtrl:          %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("WritebackAddr:      %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("WritebackStride:    %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("Layer0CfgAddr:      %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("Layer1CfgAddr:      %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("Layer2CfgAddr:      %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("Layer3CfgAddr:      %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("LayCfgEn:	    %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("InterruptControl:   %lx\n",reg_read(fbdev, DC_FRM_DES));
	printk("--------Frame Descriptor register--------\n");
}

static void dump_layer_desc_reg(void)
{
	unsigned int ctrl;
	ctrl = reg_read(fbdev, DC_CTRL);
	ctrl |= DC_DES_CNT_RST;
	reg_write(fbdev, DC_CTRL, ctrl);

	printk("--------layer0 Descriptor register-------\n");
	printk("LayerSize:          %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerCfg:           %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerBufferAddr:    %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerScale:         %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerRotation:      %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerScratch:       %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerPos:           %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerResizeCoef_X:  %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerResizeCoef_Y:  %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("LayerStride:        %lx\n",reg_read(fbdev, DC_LAY0_DES));
//	printk("Layer_UV_addr:	    %lx\n",reg_read(fbdev, DC_LAY0_DES));
//	printk("Layer_UV_stride:    %lx\n",reg_read(fbdev, DC_LAY0_DES));
	printk("--------layer0 Descriptor register-------\n");

	printk("--------layer1 Descriptor register-------\n");
	printk("LayerSize:          %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerCfg:           %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerBufferAddr:    %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerScale:         %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerRotation:      %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerScratch:       %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerPos:           %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerResizeCoef_X:  %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerResizeCoef_Y:  %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("LayerStride:        %lx\n",reg_read(fbdev, DC_LAY1_DES));
//	printk("Layer_UV_addr:	    %lx\n",reg_read(fbdev, DC_LAY1_DES));
//	printk("Layer_UV_stride:    %lx\n",reg_read(fbdev, DC_LAY1_DES));
	printk("--------layer1 Descriptor register-------\n");

	printk("--------layer2 Descriptor register-------\n");
	printk("LayerSize:          %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerCfg:           %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerBufferAddr:    %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerScale:         %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerRotation:      %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerScratch:       %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerPos:           %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerResizeCoef_X:  %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerResizeCoef_Y:  %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("LayerStride:        %lx\n",reg_read(fbdev, DC_LAY2_DES));
//	printk("Layer_UV_addr:	    %lx\n",reg_read(fbdev, DC_LAY2_DES));
//	printk("Layer_UV_stride:    %lx\n",reg_read(fbdev, DC_LAY2_DES));
	printk("--------layer2 Descriptor register-------\n");

	printk("--------layer3 Descriptor register-------\n");
	printk("LayerSize:          %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerCfg:           %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerBufferAddr:    %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerScale:         %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerRotation:      %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerScratch:       %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerPos:           %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerResizeCoef_X:  %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerResizeCoef_Y:  %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("LayerStride:        %lx\n",reg_read(fbdev, DC_LAY3_DES));
//	printk("Layer_UV_addr:	    %lx\n",reg_read(fbdev, DC_LAY3_DES));
//	printk("Layer_UV_stride:    %lx\n",reg_read(fbdev, DC_LAY3_DES));
	printk("--------layer3 Descriptor register-------\n");
}

static void dump_rdma_desc_reg(void)
{
	unsigned int ctrl;
	  ctrl = reg_read(fbdev, DC_CTRL);
	  ctrl |= (1 << 2);
	  reg_write(fbdev, DC_CTRL, ctrl);
	printk("====================rdma Descriptor register======================\n");
	printk("RdmaNextCfgAddr:    %lx\n",reg_read(fbdev, DC_RDMA_DES));
	printk("FrameBufferAddr:    %lx\n",reg_read(fbdev, DC_RDMA_DES));
	printk("Stride:             %lx\n",reg_read(fbdev, DC_RDMA_DES));
	printk("ChainCfg:           %lx\n",reg_read(fbdev, DC_RDMA_DES));
	printk("InterruptControl:   %lx\n",reg_read(fbdev, DC_RDMA_DES));
	printk("==================rdma Descriptor register end======================\n");
}


static void dump_frm_desc(struct ingenicfb_framedesc *framedesc, int index)
{
	printk("-------User Frame Descriptor index[%d]-----\n", index);
	printk("FramedescAddr:	    0x%x\n",(uint32_t)framedesc);
	printk("FrameNextCfgAddr:   0x%x\n",framedesc->FrameNextCfgAddr);
	printk("FrameSize:          0x%x\n",framedesc->FrameSize.d32);
	printk("FrameCtrl:          0x%x\n",framedesc->FrameCtrl.d32);
	printk("Layer0CfgAddr:      0x%x\n",framedesc->Layer0CfgAddr);
	printk("Layer1CfgAddr:      0x%x\n",framedesc->Layer1CfgAddr);
	printk("LayerCfgEn:	    0x%x\n",framedesc->LayCfgEn.d32);
	printk("InterruptControl:   0x%x\n",framedesc->InterruptControl.d32);
	printk("-------User Frame Descriptor index[%d]-----\n", index);
}

static void dump_layer_desc(struct ingenicfb_layerdesc *layerdesc, int row, int col)
{
	printk("------User layer Descriptor index[%d][%d]------\n", row, col);
	printk("LayerdescAddr:	    0x%x\n",(uint32_t)layerdesc);
	printk("LayerSize:          0x%x\n",layerdesc->LayerSize.d32);
	printk("LayerCfg:           0x%x\n",layerdesc->LayerCfg.d32);
	printk("LayerBufferAddr:    0x%x\n",layerdesc->LayerBufferAddr);
	printk("LayerScale:         0x%x\n",layerdesc->LayerScale.d32);
	printk("LayerResizeCoef_X:  0x%x\n",layerdesc->layerresizecoef_x);
	printk("LayerResizeCoef_Y:  0x%x\n",layerdesc->layerresizecoef_y);
	printk("LayerPos:           0x%x\n",layerdesc->LayerPos.d32);
	printk("LayerStride:        0x%x\n",layerdesc->LayerStride);
//	printk("Layer_UV_addr:	    0x%x\n",layer_config->BufferAddr_UV);
//	printk("Layer_UV_stride:    0x%x\n",layer_config->stride_UV);
	printk("------User layer Descriptor index[%d][%d]------\n", row, col);
}

void dump_lay_cfg(struct ingenicfb_lay_cfg * lay_cfg, int index)
{
	printk("------User disp set index[%d]------\n", index);
	printk("lay_en:		   0x%x\n",lay_cfg->lay_en);
	printk("tlb_en:		   0x%x\n",lay_cfg->tlb_en);
	printk("lay_scale_en:	   0x%x\n",lay_cfg->lay_scale_en);
	printk("lay_z_order:	   0x%x\n",lay_cfg->lay_z_order);
	printk("source_w:	   0x%x\n",lay_cfg->source_w);
	printk("source_h:	   0x%x\n",lay_cfg->source_h);
	printk("disp_pos_x:	   0x%x\n",lay_cfg->disp_pos_x);
	printk("disp_pos_y:	   0x%x\n",lay_cfg->disp_pos_y);
	printk("scale_w:	   0x%x\n",lay_cfg->scale_w);
	printk("scale_h:	   0x%x\n",lay_cfg->scale_h);
	printk("g_alpha_en:	   0x%x\n",lay_cfg->g_alpha_en);
	printk("g_alpha_val:	   0x%x\n",lay_cfg->g_alpha_val);
	printk("color:		   0x%x\n",lay_cfg->color);
	printk("format:		   0x%x\n",lay_cfg->format);
	printk("stride:		   0x%x\n",lay_cfg->stride);
	printk("addr[0]:	   0x%x\n",lay_cfg->addr[0]);
	printk("addr[1]:	   0x%x\n",lay_cfg->addr[1]);
	printk("addr[2]:	   0x%x\n",lay_cfg->addr[2]);

	printk("------User disp set index[%d]------\n", index);
}

static void dump_lcdc_registers(void)
{
	dump_dc_reg();
	dump_tft_reg();
	dump_slcd_reg();
	dump_frm_desc_reg();
	dump_layer_desc_reg();
	dump_rdma_desc_reg();
}

static void dump_desc(struct ingenicfb_device *fbdev)
{
	int i, j;
	for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES*2; i++) {
		for(j = 0; j < DPU_SUPPORT_MAX_LAYERS; j++) {
			dump_layer_desc(fbdev->layerdesc[i][j], i, j);
		}
		dump_frm_desc(fbdev->framedesc[i], i);
	}
}

	static ssize_t
dump_lcd(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);

	printk("\nDisp_end_num = %d\n\n",fbdev->irq_cnt);
	printk("\nTFT_UNDR_num = %d\n\n",fbdev->tft_undr_cnt);
	printk("\nFrm_start_num = %d\n\n", fbdev->frm_start);
	printk(" Pand display count=%d\n",fbdev->pan_display_count);
	printk("timestamp.wp = %d , timestamp.rp = %d\n\n", fbdev->timestamp.wp, fbdev->timestamp.rp);
	dump_lcdc_registers();
	dump_desc(fbdev);
	return 0;
}

static void dump_all(struct ingenicfb_device *fbdev)
{
	printk("\ndisp_end_num = %d\n\n", fbdev->irq_cnt);
	printk("\nTFT_UNDR_num = %d\n\n", fbdev->tft_undr_cnt);
	printk("\nFrm_start_num = %d\n\n", fbdev->frm_start);
	dump_lcdc_registers();
	dump_desc(fbdev);
}

#ifdef CONFIG_DEBUG_DPU_IRQCNT
	static ssize_t
dump_irqcnts(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dbg_irqcnt *dbg = &fbdev->dbg_irqcnt;
	char *p = buf;
	p += sprintf(p, "irqcnt		: %lld\n", dbg->irqcnt);
	p += sprintf(p, "cmp_start	: %lld\n", dbg->cmp_start);
	p += sprintf(p, "stop_disp_ack	: %lld\n", dbg->stop_disp_ack);
	p += sprintf(p, "disp_end	: %lld\n", dbg->disp_end);
	p += sprintf(p, "tft_under	: %lld\n", dbg->tft_under);
	p += sprintf(p, "wdma_over	: %lld\n", dbg->wdma_over);
	p += sprintf(p, "wdma_end	: %lld\n", dbg->wdma_end);
	p += sprintf(p, "layer3_end	: %lld\n", dbg->layer3_end);
	p += sprintf(p, "layer2_end	: %lld\n", dbg->layer2_end);
	p += sprintf(p, "layer1_end	: %lld\n", dbg->layer1_end);
	p += sprintf(p, "layer0_end	: %lld\n", dbg->layer0_end);
	p += sprintf(p, "clr_cmp_end	: %lld\n", dbg->clr_cmp_end);
	p += sprintf(p, "stop_wrbk_ack	: %lld\n", dbg->stop_wrbk_ack);
	p += sprintf(p, "srd_start	: %lld\n", dbg->srd_start);
	p += sprintf(p, "srd_end	: %lld\n", dbg->srd_end);
	p += sprintf(p, "cmp_w_slow	: %lld\n", dbg->cmp_w_slow);

	return p - buf;
}
#endif

void ingenicfb_clk_enable(struct ingenicfb_device *fbdev)
{
	if(fbdev->is_clk_en){
		return;
	}
	clk_prepare_enable(fbdev->pclk);
	clk_prepare_enable(fbdev->clk);
	fbdev->is_clk_en = 1;
}

void ingenicfb_clk_disable(struct ingenicfb_device *fbdev)
{
	if(!fbdev->is_clk_en){
		return;
	}
	fbdev->is_clk_en = 0;
	clk_disable_unprepare(fbdev->clk);
	clk_disable_unprepare(fbdev->pclk);
}

	static void
ingenicfb_videomode_to_var(struct fb_var_screeninfo *var,
		const struct fb_videomode *mode, int lcd_type)
{
	var->xres = mode->xres;
	var->yres = mode->yres;
	var->xres_virtual = mode->xres;
	var->yres_virtual = mode->yres * CONFIG_FB_INGENIC_NR_FRAMES * CONFIG_FB_INGENIC_NR_LAYERS;
	var->xoffset = 0;
	var->yoffset = 0;
	var->left_margin = mode->left_margin;
	var->right_margin = mode->right_margin;
	var->upper_margin = mode->upper_margin;
	var->lower_margin = mode->lower_margin;
	var->hsync_len = mode->hsync_len;
	var->vsync_len = mode->vsync_len;
	var->sync = mode->sync;
	var->vmode = mode->vmode & FB_VMODE_MASK;
	var->pixclock = mode->pixclock;
}

static struct fb_videomode *ingenicfb_get_mode(struct fb_var_screeninfo *var,
		struct fb_info *info)
{
	size_t i;
	struct ingenicfb_device *fbdev = info->par;
	struct fb_videomode *mode = fbdev->panel->modes;

	for (i = 0; i < fbdev->panel->num_modes; ++i, ++mode) {
		if (mode->xres == var->xres && mode->yres == var->yres
				&& mode->vmode == var->vmode
				&& mode->right_margin == var->right_margin) {
			if (fbdev->panel->lcd_type != LCD_TYPE_SLCD) {
				if (mode->pixclock == var->pixclock)
					return mode;
			} else {
				return mode;
			}
		}
	}

	return NULL;
}

static int ingenicfb_check_frm_cfg(struct fb_info *info, struct ingenicfb_frm_cfg *frm_cfg)
{
	struct fb_var_screeninfo *var = &info->var;
	struct ingenicfb_lay_cfg *lay_cfg;
	struct fb_videomode *mode;
	int scale_num = 0;
	int i;

	mode = ingenicfb_get_mode(var, info);
	if (mode == NULL) {
		dev_err(info->dev, "%s get video mode failed\n", __func__);
		return -EINVAL;
	}

	lay_cfg = frm_cfg->lay_cfg;

	if((!(lay_cfg[0].lay_en || lay_cfg[1].lay_en || lay_cfg[2].lay_en || lay_cfg[3].lay_en)) ||
	   (lay_cfg[0].lay_z_order == lay_cfg[1].lay_z_order) ||
	   (lay_cfg[1].lay_z_order == lay_cfg[2].lay_z_order) ||
	   (lay_cfg[2].lay_z_order == lay_cfg[3].lay_z_order)) {
		dev_err(info->dev,"%s %d frame[0] cfg value is err!\n",__func__,__LINE__);
		return -EINVAL;
	}

	switch (lay_cfg[0].format) {
	case LAYER_CFG_FORMAT_RGB555:
	case LAYER_CFG_FORMAT_ARGB1555:
	case LAYER_CFG_FORMAT_RGB565:
	case LAYER_CFG_FORMAT_RGB888:
	case LAYER_CFG_FORMAT_ARGB8888:
	case LAYER_CFG_FORMAT_YUV422:
	case LAYER_CFG_FORMAT_NV12:
	case LAYER_CFG_FORMAT_NV21:
		break;
	default:
		dev_err(info->dev,"%s %d frame[0] cfg value is err!\n",__func__,__LINE__);
		return -EINVAL;
	}

	for(i = 0; i < DPU_SUPPORT_MAX_LAYERS; i++) {
		if(lay_cfg[i].lay_en) {
			if((lay_cfg[i].source_w > DPU_MAX_SIZE) ||
			   (lay_cfg[i].source_w < DPU_MIN_SIZE) ||
			   (lay_cfg[i].source_h > DPU_MAX_SIZE) ||
			   (lay_cfg[i].source_h < DPU_MIN_SIZE) ||
			   (lay_cfg[i].disp_pos_x > mode->xres) ||
			   (lay_cfg[i].disp_pos_y > mode->yres) ||
			   (lay_cfg[i].color > LAYER_CFG_COLOR_BGR) ||
			   (lay_cfg[i].stride > DPU_STRIDE_SIZE)) {
				dev_err(info->dev,"%s %d frame[0] cfg value is err!\n",__func__,__LINE__);
				return -EINVAL;
			}
			switch (lay_cfg[i].format) {
			case LAYER_CFG_FORMAT_RGB555:
			case LAYER_CFG_FORMAT_ARGB1555:
			case LAYER_CFG_FORMAT_RGB565:
			case LAYER_CFG_FORMAT_RGB888:
			case LAYER_CFG_FORMAT_ARGB8888:
			case LAYER_CFG_FORMAT_YUV422:
			case LAYER_CFG_FORMAT_NV12:
			case LAYER_CFG_FORMAT_NV21:
				break;
			default:
				dev_err(info->dev,"%s %d frame[%d] cfg value is err!\n",__func__,__LINE__, i);
				return -EINVAL;
			}
			if(lay_cfg[i].lay_scale_en) {
				scale_num++;
				if((lay_cfg[i].scale_w > mode->xres) ||
					(lay_cfg[i].scale_w < DPU_SCALE_MIN_SIZE) ||
					(lay_cfg[i].scale_h > mode->yres) ||
					(lay_cfg[i].scale_h < DPU_SCALE_MIN_SIZE) ||
					(lay_cfg[i].scale_w + lay_cfg[i].disp_pos_x > mode->xres) ||
					(lay_cfg[i].scale_h + lay_cfg[i].disp_pos_y > mode->yres) ||
					(scale_num > 2)) {
					dev_err(info->dev,"%s %d frame[%d] cfg value is err!\n",
							__func__, __LINE__,i);
					return -EINVAL;
				}
			} else {
				if((lay_cfg[i].disp_pos_x + lay_cfg[i].source_w > mode->xres) ||
				(lay_cfg[i].disp_pos_y + lay_cfg[i].source_h > mode->yres)) {
					dev_err(info->dev,"%s %d frame[%d] cfg value is err!\n",
							__func__, __LINE__,i);
					return -EINVAL;
				}
			}
		}
	}

	return 0;
}

static void ingenicfb_colormode_to_var(struct fb_var_screeninfo *var,
		struct ingenicfb_colormode *color)
{
	var->bits_per_pixel = color->bits_per_pixel;
	var->nonstd = color->nonstd;
	var->red = color->red;
	var->green = color->green;
	var->blue = color->blue;
	var->transp = color->transp;
}

static bool cmp_var_to_colormode(struct fb_var_screeninfo *var,
		struct ingenicfb_colormode *color)
{
	bool cmp_component(struct fb_bitfield *f1, struct fb_bitfield *f2)
	{
		return f1->length == f2->length &&
			f1->offset == f2->offset &&
			f1->msb_right == f2->msb_right;
	}

	if (var->bits_per_pixel == 0 ||
			var->red.length == 0 ||
			var->blue.length == 0 ||
			var->green.length == 0)
		return 0;

	return var->bits_per_pixel == color->bits_per_pixel &&
		cmp_component(&var->red, &color->red) &&
		cmp_component(&var->green, &color->green) &&
		cmp_component(&var->blue, &color->blue) &&
		cmp_component(&var->transp, &color->transp);
}

static int ingenicfb_check_colormode(struct fb_var_screeninfo *var, uint32_t *mode)
{
	int i;

	if (var->nonstd) {
		for (i = 0; i < ARRAY_SIZE(ingenicfb_colormodes); ++i) {
			struct ingenicfb_colormode *m = &ingenicfb_colormodes[i];
			if (var->nonstd == m->nonstd) {
				ingenicfb_colormode_to_var(var, m);
				*mode = m->mode;
				return 0;
			}
		}

		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(ingenicfb_colormodes); ++i) {
		struct ingenicfb_colormode *m = &ingenicfb_colormodes[i];
		if (cmp_var_to_colormode(var, m)) {
			ingenicfb_colormode_to_var(var, m);
			*mode = m->mode;
			return 0;
		}
	}
	/* To support user libraries that only support RGB format */
	for (i = 0; i < ARRAY_SIZE(ingenicfb_colormodes); ++i) {
		struct ingenicfb_colormode *m = &ingenicfb_colormodes[i];
		if (var->bits_per_pixel == m->bits_per_pixel) {
			ingenicfb_colormode_to_var(var, m);
			*mode = m->mode;
			return 0;
		}
	}

	return -EINVAL;
}

static int ingenicfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct fb_videomode *mode;
	uint32_t colormode;
	int ret;

	mode = ingenicfb_get_mode(var, info);
	if (mode == NULL) {
		dev_err(info->dev, "%s get video mode failed\n", __func__);
		return -EINVAL;
	}

	ingenicfb_videomode_to_var(var, mode, fbdev->panel->lcd_type);

	ret = ingenicfb_check_colormode(var, &colormode);
	if(ret) {
		dev_err(info->dev,"Check colormode failed!\n");
		return  ret;
	}

	return 0;
}

static void slcd_send_mcu_command(struct ingenicfb_device *fbdev, unsigned long cmd)
{
	int count = 10000;
	uint32_t slcd_cfg;

	while ((reg_read(fbdev, DC_SLCD_ST) & DC_SLCD_ST_BUSY) && count--) {
		udelay(10);
	}
	if (count < 0) {
		dev_err(fbdev->dev, "SLCDC wait busy state wrong");
	}

	slcd_cfg = reg_read(fbdev, DC_SLCD_CFG);
	reg_write(fbdev, DC_SLCD_CFG, (slcd_cfg & ~DC_FMT_EN));
	reg_write(fbdev, DC_SLCD_REG_IF, DC_SLCD_REG_IF_FLAG_CMD | (cmd & ~DC_SLCD_REG_IF_FLAG_MASK));
}

static void slcd_send_mcu_data(struct ingenicfb_device *fbdev, unsigned long data)
{
	int count = 10000;
	uint32_t slcd_cfg;

	while ((reg_read(fbdev, DC_SLCD_ST) & DC_SLCD_ST_BUSY) && count--) {
		udelay(10);
	}
	if (count < 0) {
		dev_err(fbdev->dev, "SLCDC wait busy state wrong");
	}

	slcd_cfg = reg_read(fbdev, DC_SLCD_CFG);
	reg_write(fbdev, DC_SLCD_CFG, (slcd_cfg | DC_FMT_EN));
	reg_write(fbdev, DC_SLCD_REG_IF, DC_SLCD_REG_IF_FLAG_DATA | (data & ~DC_SLCD_REG_IF_FLAG_MASK));
}

static void slcd_send_mcu_prm(struct ingenicfb_device *fbdev, unsigned long data)
{
	int count = 10000;
	uint32_t slcd_cfg;

	while ((reg_read(fbdev, DC_SLCD_ST) & DC_SLCD_ST_BUSY) && count--) {
		udelay(10);
	}
	if (count < 0) {
		dev_err(fbdev->dev, "SLCDC wait busy state wrong");
	}

	slcd_cfg = reg_read(fbdev, DC_SLCD_CFG);
	reg_write(fbdev, DC_SLCD_CFG, (slcd_cfg & ~DC_FMT_EN));
	reg_write(fbdev, DC_SLCD_REG_IF, DC_SLCD_REG_IF_FLAG_PRM | (data & ~DC_SLCD_REG_IF_FLAG_MASK));
}

static void wait_slcd_busy(void)
{
	int count = 100000;
	while ((reg_read(fbdev, DC_SLCD_ST) & DC_SLCD_ST_BUSY)
			&& count--) {
		udelay(10);
	}
	if (count < 0) {
		dev_err(fbdev->dev,"SLCDC wait busy state wrong");
	}
}

static int wait_dc_state(uint32_t state, uint32_t flag)
{
	unsigned long timeout = 20000;
	while(((!(reg_read(fbdev, DC_ST) & state)) == flag) && timeout) {
		timeout--;
		udelay(10);
	}
	if(timeout <= 0) {
		printk("LCD wait state timeout! state = %d, DC_ST = 0x%x\n", state, DC_ST);
		return -1;
	}
	return 0;
}

static void ingenicfb_slcd_mcu_init(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;
	struct smart_config *smart_config;
	struct smart_lcd_data_table *data_table;
	uint32_t length_data_table;
	uint32_t i;

	smart_config = panel->smart_config;
	if (panel->lcd_type != LCD_TYPE_SLCD || smart_config == NULL)
		return;

	data_table = smart_config->data_table;
	length_data_table = smart_config->length_data_table;
	if(length_data_table && data_table) {
		for(i = 0; i < length_data_table; i++) {
			switch (data_table[i].type) {
			case SMART_CONFIG_DATA:
				slcd_send_mcu_data(fbdev, data_table[i].value);
				break;
			case SMART_CONFIG_PRM:
				slcd_send_mcu_prm(fbdev, data_table[i].value);
				break;
			case SMART_CONFIG_CMD:
				slcd_send_mcu_command(fbdev, data_table[i].value);
				break;
			case SMART_CONFIG_UDELAY:
				udelay(data_table[i].value);
				break;
			default:
				printk("Unknow SLCD data type\n");
				break;
			}
		}
	}
}

static void ingenicfb_cmp_start(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;

	reg_write(fbdev, DC_FRM_CFG_CTRL, DC_FRM_START);
}

static void ingenicfb_rdma_start(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;

	reg_write(fbdev, DC_RDMA_CHAIN_CTRL, DC_RDMA_START);
}

static void tft_timing_init(struct fb_videomode *modes) {
	uint32_t hps;
	uint32_t hpe;
	uint32_t vps;
	uint32_t vpe;
	uint32_t hds;
	uint32_t hde;
	uint32_t vds;
	uint32_t vde;

	hps = modes->hsync_len;
	hpe = hps + modes->left_margin + modes->xres + modes->right_margin;
	vps = modes->vsync_len;
	vpe = vps + modes->upper_margin + modes->yres + modes->lower_margin;

	hds = modes->hsync_len + modes->left_margin;
	hde = hds + modes->xres;
	vds = modes->vsync_len + modes->upper_margin;
	vde = vds + modes->yres;

	reg_write(fbdev, DC_TFT_HSYNC,
		  (hps << DC_HPS_LBIT) |
		  (hpe << DC_HPE_LBIT));
	reg_write(fbdev, DC_TFT_VSYNC,
		  (vps << DC_VPS_LBIT) |
		  (vpe << DC_VPE_LBIT));
	reg_write(fbdev, DC_TFT_HDE,
		  (hds << DC_HDS_LBIT) |
		  (hde << DC_HDE_LBIT));
	reg_write(fbdev, DC_TFT_VDE,
		  (vds << DC_VDS_LBIT) |
		  (vde << DC_VDE_LBIT));
}

void tft_cfg_init(struct tft_config *tft_config) {
	uint32_t tft_cfg;
	uint32_t lcd_cgu;

	lcd_cgu = *(volatile unsigned int *)(0xb0000064);
	if(tft_config->pix_clk_inv) {
		lcd_cgu |= (0x1 << 26);
	} else {
		lcd_cgu &= ~(0x1 << 26);
	}
	*(volatile unsigned int *)(0xb0000064) = lcd_cgu;

	tft_cfg = reg_read(fbdev, DC_TFT_CFG);
	if(tft_config->de_dl) {
		tft_cfg |= DC_DE_DL;
	} else {
		tft_cfg &= ~DC_DE_DL;
	}

	if(tft_config->sync_dl) {
		tft_cfg |= DC_SYNC_DL;
	} else {
		tft_cfg &= ~DC_SYNC_DL;
	}

	tft_cfg &= ~DC_COLOR_EVEN_MASK;
	switch(tft_config->color_even) {
	case TFT_LCD_COLOR_EVEN_RGB:
		tft_cfg |= DC_EVEN_RGB;
		break;
	case TFT_LCD_COLOR_EVEN_RBG:
		tft_cfg |= DC_EVEN_RBG;
		break;
	case TFT_LCD_COLOR_EVEN_BGR:
		tft_cfg |= DC_EVEN_BGR;
		break;
	case TFT_LCD_COLOR_EVEN_BRG:
		tft_cfg |= DC_EVEN_BRG;
		break;
	case TFT_LCD_COLOR_EVEN_GBR:
		tft_cfg |= DC_EVEN_GBR;
		break;
	case TFT_LCD_COLOR_EVEN_GRB:
		tft_cfg |= DC_EVEN_GRB;
		break;
	default:
		printk("err!\n");
		break;
	}

	tft_cfg &= ~DC_COLOR_ODD_MASK;
	switch(tft_config->color_odd) {
	case TFT_LCD_COLOR_ODD_RGB:
		tft_cfg |= DC_ODD_RGB;
		break;
	case TFT_LCD_COLOR_ODD_RBG:
		tft_cfg |= DC_ODD_RBG;
		break;
	case TFT_LCD_COLOR_ODD_BGR:
		tft_cfg |= DC_ODD_BGR;
		break;
	case TFT_LCD_COLOR_ODD_BRG:
		tft_cfg |= DC_ODD_BRG;
		break;
	case TFT_LCD_COLOR_ODD_GBR:
		tft_cfg |= DC_ODD_GBR;
		break;
	case TFT_LCD_COLOR_ODD_GRB:
		tft_cfg |= DC_ODD_GRB;
		break;
	default:
		printk("err!\n");
		break;
	}

	tft_cfg &= ~DC_MODE_MASK;
	switch(tft_config->mode) {
	case TFT_LCD_MODE_PARALLEL_888:
		tft_cfg |= DC_MODE_PARALLEL_888;
		break;
	case TFT_LCD_MODE_PARALLEL_666:
		tft_cfg |= DC_MODE_PARALLEL_666;
		break;
	case TFT_LCD_MODE_PARALLEL_565:
		tft_cfg |= DC_MODE_PARALLEL_565;
		break;
	case TFT_LCD_MODE_SERIAL_RGB:
		tft_cfg |= DC_MODE_SERIAL_8BIT_RGB;
		break;
	case TFT_LCD_MODE_SERIAL_RGBD:
		tft_cfg |= DC_MODE_SERIAL_8BIT_RGBD;
		break;
	default:
		printk("err!\n");
		break;
	}
	reg_write(fbdev, DC_TFT_CFG, tft_cfg);
}

static int ingenicfb_tft_set_par(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;
	struct lcd_panel_ops *panel_ops;
	struct fb_videomode *mode;

	panel_ops = panel->ops;

	mode = ingenicfb_get_mode(&info->var, info);
	if (mode == NULL) {
		dev_err(info->dev, "%s get video mode failed\n", __func__);
		return -EINVAL;
	}

	tft_timing_init(mode);
	tft_cfg_init(panel->tft_config);
	if(panel_ops && panel_ops->enable)
		panel_ops->enable(panel);

	return 0;
}

static void slcd_cfg_init(struct fb_info *info,
		struct smart_config *smart_config) {
	uint32_t slcd_cfg;

	if (smart_config == NULL) {
		dev_info(info->dev, "SLCD use default config\n");
		return;
	}

	slcd_cfg = reg_read(fbdev, DC_SLCD_CFG);
	slcd_cfg &= ~DC_RDY_SWITCH;
	slcd_cfg &= ~DC_CS_EN;

	if(smart_config->te_switch) {
		slcd_cfg |= DC_TE_SWITCH;

		if(smart_config->te_dp) {
			slcd_cfg |= DC_TE_DP;
		} else {
			slcd_cfg &= ~DC_TE_DP;
		}
		if(smart_config->te_md) {
			slcd_cfg |= DC_TE_MD;
		} else {
			slcd_cfg &= ~DC_TE_MD;
		}
		if(smart_config->te_anti_jit) {
			slcd_cfg |= DC_TE_ANTI_JIT;
		} else {
			slcd_cfg &= ~DC_TE_ANTI_JIT;
		}
	} else {
		slcd_cfg &= ~DC_TE_SWITCH;
	}

	if(smart_config->te_mipi_switch) {
		slcd_cfg |= DC_TE_MIPI_SWITCH;
	} else {
		slcd_cfg &= ~DC_TE_MIPI_SWITCH;
	}

	if(smart_config->dc_md) {
		slcd_cfg |= DC_DC_MD;
	} else {
		slcd_cfg &= ~DC_DC_MD;
	}

	if(smart_config->wr_md) {
		slcd_cfg |= DC_WR_DP;
	} else {
		slcd_cfg &= ~DC_WR_DP;
	}

	slcd_cfg &= ~DC_DBI_TYPE_MASK;
	switch(smart_config->smart_type){
	case SMART_LCD_TYPE_8080:
		slcd_cfg |= DC_DBI_TYPE_B_8080;
		break;
	case SMART_LCD_TYPE_6800:
		slcd_cfg |= DC_DBI_TYPE_A_6800;
		break;
	case SMART_LCD_TYPE_SPI_3:
		slcd_cfg |= DC_DBI_TYPE_C_SPI_3;
		break;
	case SMART_LCD_TYPE_SPI_4:
		slcd_cfg |= DC_DBI_TYPE_C_SPI_4;
		break;
	default:
		printk("err!\n");
		break;
	}

	slcd_cfg &= ~DC_DATA_FMT_MASK;
	switch(smart_config->pix_fmt) {
	case SMART_LCD_FORMAT_888:
		slcd_cfg |= DC_DATA_FMT_888;
		break;
	case SMART_LCD_FORMAT_666:
		slcd_cfg |= DC_DATA_FMT_666;
		break;
	case SMART_LCD_FORMAT_565:
		slcd_cfg |= DC_DATA_FMT_565;
		break;
	default:
		printk("err!\n");
		break;
	}

	slcd_cfg &= ~DC_DWIDTH_MASK;
	switch(smart_config->dwidth) {
	case SMART_LCD_DWIDTH_8_BIT:
		slcd_cfg |= DC_DWIDTH_8BITS;
		break;
	case SMART_LCD_DWIDTH_9_BIT:
		slcd_cfg |= DC_DWIDTH_9BITS;
		break;
	case SMART_LCD_DWIDTH_16_BIT:
		slcd_cfg |= DC_DWIDTH_16BITS;
		break;
	case SMART_LCD_DWIDTH_18_BIT:
		slcd_cfg |= DC_DWIDTH_18BITS;
		break;
	case SMART_LCD_DWIDTH_24_BIT:
		slcd_cfg |= DC_DWIDTH_24BITS;
		break;
	default:
		printk("err!\n");
		break;
	}

	slcd_cfg &= ~DC_CWIDTH_MASK;
	switch(smart_config->cwidth) {
	case SMART_LCD_CWIDTH_8_BIT:
		slcd_cfg |= DC_CWIDTH_8BITS;
		break;
	case SMART_LCD_CWIDTH_9_BIT:
		slcd_cfg |= DC_CWIDTH_9BITS;
		break;
	case SMART_LCD_CWIDTH_16_BIT:
		slcd_cfg |= DC_CWIDTH_16BITS;
		break;
	case SMART_LCD_CWIDTH_18_BIT:
		slcd_cfg |= DC_CWIDTH_18BITS;
		break;
	case SMART_LCD_CWIDTH_24_BIT:
		slcd_cfg |= DC_CWIDTH_24BITS;
		break;
	default:
		printk("err!\n");
		break;
	}

	reg_write(fbdev, DC_SLCD_CFG, slcd_cfg);

	return;
}

static int slcd_timing_init(struct lcd_panel *ingenicfb_panel,
		struct fb_videomode *mode)
{
	uint32_t width = mode->xres;
	uint32_t height = mode->yres;
	uint32_t dhtime = 0;
	uint32_t dltime = 0;
	uint32_t chtime = 0;
	uint32_t cltime = 0;
	uint32_t tah = 0;
	uint32_t tas = 0;
	uint32_t slowtime = 0;

	/*frm_size*/
	reg_write(fbdev, DC_SLCD_FRM_SIZE,
		  ((width << DC_SLCD_FRM_H_SIZE_LBIT) |
		   (height << DC_SLCD_FRM_V_SIZE_LBIT)));

	/* wr duty */
	reg_write(fbdev, DC_SLCD_WR_DUTY,
		  ((dhtime << DC_DSTIME_LBIT) |
		   (dltime << DC_DDTIME_LBIT) |
		   (chtime << DC_CSTIME_LBIT) |
		   (cltime << DC_CDTIME_LBIT)));

	/* slcd timing */
	reg_write(fbdev, DC_SLCD_TIMING,
		  ((tah << DC_TAH_LBIT) |
		  (tas << DC_TAS_LBIT)));

	/* slow time */
	reg_write(fbdev, DC_SLCD_SLOW_TIME, slowtime);

	return 0;
}

static int ingenicfb_slcd_set_par(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;
	struct lcd_panel_ops *panel_ops;
	struct fb_videomode *mode;

	mode = ingenicfb_get_mode(&info->var, info);
	panel_ops = panel->ops;

	slcd_cfg_init(info, panel->smart_config);
	slcd_timing_init(panel, mode);

	if(panel_ops && panel_ops->enable)
		    panel_ops->enable(panel);

	ingenicfb_slcd_mcu_init(info);

	return 0;
}

static void csc_mode_set(struct ingenicfb_device *fbdev, csc_mode_t mode) {
	switch(mode) {
	case CSC_MODE_0:
		reg_write(fbdev, DC_CSC_MULT_YRV, DC_CSC_MULT_Y_MD0 | DC_CSC_MULT_RV_MD0);
		reg_write(fbdev, DC_CSC_MULT_GUGV, DC_CSC_MULT_GU_MD0 | DC_CSC_MULT_GV_MD0);
		reg_write(fbdev, DC_CSC_MULT_BU, DC_CSC_MULT_BU_MD0);
		reg_write(fbdev, DC_CSC_SUB_YUV, DC_CSC_SUB_Y_MD0 | DC_CSC_SUB_UV_MD0);
		break;
	case CSC_MODE_1:
		reg_write(fbdev, DC_CSC_MULT_YRV, DC_CSC_MULT_Y_MD1 | DC_CSC_MULT_RV_MD1);
		reg_write(fbdev, DC_CSC_MULT_GUGV, DC_CSC_MULT_GU_MD1 | DC_CSC_MULT_GV_MD1);
		reg_write(fbdev, DC_CSC_MULT_BU, DC_CSC_MULT_BU_MD1);
		reg_write(fbdev, DC_CSC_SUB_YUV, DC_CSC_SUB_Y_MD1 | DC_CSC_SUB_UV_MD1);
		break;
	case CSC_MODE_2:
		reg_write(fbdev, DC_CSC_MULT_YRV, DC_CSC_MULT_Y_MD2 | DC_CSC_MULT_RV_MD2);
		reg_write(fbdev, DC_CSC_MULT_GUGV, DC_CSC_MULT_GU_MD2 | DC_CSC_MULT_GV_MD2);
		reg_write(fbdev, DC_CSC_MULT_BU, DC_CSC_MULT_BU_MD2);
		reg_write(fbdev, DC_CSC_SUB_YUV, DC_CSC_SUB_Y_MD2 | DC_CSC_SUB_UV_MD2);
		break;
	case CSC_MODE_3:
		reg_write(fbdev, DC_CSC_MULT_YRV, DC_CSC_MULT_Y_MD3 | DC_CSC_MULT_RV_MD3);
		reg_write(fbdev, DC_CSC_MULT_GUGV, DC_CSC_MULT_GU_MD3 | DC_CSC_MULT_GV_MD3);
		reg_write(fbdev, DC_CSC_MULT_BU, DC_CSC_MULT_BU_MD3);
		reg_write(fbdev, DC_CSC_SUB_YUV, DC_CSC_SUB_Y_MD3 | DC_CSC_SUB_UV_MD3);
		break;
	default:
		dev_err(fbdev->dev, "Set csc mode err!\n");
		break;
	}
}

static int ingenicfb_alloc_devmem(struct ingenicfb_device *fbdev)
{
	struct fb_videomode *mode;
	struct fb_info *fb = fbdev->fb;
	uint32_t buff_size;
	void *page;
	uint8_t *addr;
	dma_addr_t addr_phy;
	int i, j;

	mode = fbdev->panel->modes;
	if (!mode) {
		dev_err(fbdev->dev, "Checkout video mode fail\n");
		return -EINVAL;
	}


	/* 1. Alloc Framedesc Memory */
	buff_size = sizeof(struct ingenicfb_framedesc);
	buff_size = ALIGN(buff_size, DESC_ALIGN);
	addr = dma_alloc_coherent(fbdev->dev, buff_size * CONFIG_FB_INGENIC_NR_FRAMES * 2,
				  &addr_phy, GFP_KERNEL);
	if(addr == NULL) {
		return -ENOMEM;
	}
	for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES * 2; i++) {
		fbdev->framedesc[i] =
			(struct ingenicfb_framedesc *)(addr + i * buff_size);
		fbdev->framedesc_phys[i] = addr_phy + i * buff_size;
	}

	/* 2. Alloc Simple Read Dma Desc Memory. */
	buff_size = sizeof(struct ingenicfb_sreadesc);
	buff_size = ALIGN(buff_size, DESC_ALIGN);
	addr = dma_alloc_coherent(fbdev->dev, buff_size * CONFIG_FB_INGENIC_NR_FRAMES,
				  &addr_phy, GFP_KERNEL);
	if(addr == NULL) {
		return -ENOMEM;
	}
	for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES; i++) {
		fbdev->sreadesc[i] =
			(struct ingenicfb_sreadesc *)(addr + i * buff_size);
		fbdev->sreadesc_phys[i] = addr_phy + i * buff_size;
	}


	/* 3. Alloc Frame Buffer.*/
	buff_size = mode->xres * mode->yres;
	fbdev->frm_size = buff_size * fb->var.bits_per_pixel >> 3;
	buff_size *= MAX_BITS_PER_PIX >> 3;

	fbdev->vidmem_size = buff_size * CONFIG_FB_INGENIC_NR_FRAMES * CONFIG_FB_INGENIC_NR_LAYERS;
#ifdef CONFIG_INGENIC_FB_SIMPLE_RDMA
	fbdev->sread_vidmem_size = buff_size * CONFIG_FB_INGENIC_NR_FRAMES;
#else
	fbdev->sread_vidmem_size = 0;

#endif
	buff_size = fbdev->vidmem_size + fbdev->sread_vidmem_size;
	buff_size = PAGE_ALIGN(buff_size);
	fbdev->vidmem[0][0] = dma_alloc_coherent(fbdev->dev, buff_size,
					  &fbdev->vidmem_phys[0][0], GFP_KERNEL);
	if(fbdev->vidmem[0][0] == NULL) {
		return -ENOMEM;
	}
	for (page = fbdev->vidmem[0][0];
	     page < fbdev->vidmem[0][0] + PAGE_ALIGN(fbdev->vidmem_size);
	     page += PAGE_SIZE) {
		SetPageReserved(virt_to_page(page));
	}

	if(!fbdev->sread_vidmem_size) {
		fbdev->sread_vidmem[0] = fbdev->vidmem[0][0] + fbdev->vidmem_size;
		fbdev->sread_vidmem_phys[0] = fbdev->vidmem_phys[0][0] + fbdev->vidmem_size;
		for (page = fbdev->sread_vidmem[0];
				page < fbdev->sread_vidmem[0] + PAGE_ALIGN(fbdev->sread_vidmem_size);
				page += PAGE_SIZE) {
			SetPageReserved(virt_to_page(page));
		}
	}

	/*4. ALloc Layerdesc Memory.*/
	buff_size = sizeof(struct ingenicfb_layerdesc);
	buff_size = ALIGN(buff_size, DESC_ALIGN);
	addr = dma_alloc_coherent(fbdev->dev, buff_size * CONFIG_FB_INGENIC_NR_FRAMES * DPU_SUPPORT_MAX_LAYERS * 2,
				  &addr_phy, GFP_KERNEL);
	if(addr == NULL) {
		return -ENOMEM;
	}
	for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES * 2; i++) {
		for(j = 0; j < DPU_SUPPORT_MAX_LAYERS; j++) {
			fbdev->layerdesc[i][j] = (struct ingenicfb_layerdesc *)
				(addr + j * buff_size + i * buff_size * DPU_SUPPORT_MAX_LAYERS);
			fbdev->layerdesc_phys[i][j] =
				addr_phy + j * buff_size + i * buff_size * DPU_SUPPORT_MAX_LAYERS;
		}
	}
	for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES; i++) {
		for(j = 0; j < CONFIG_FB_INGENIC_NR_LAYERS; j++) {
			/*

			Memory plane:
				Layer0:
				[Frame0], [Frame1], [Frame2]
				Layer1:
				[Frame0], [Frame1], [Frame2]
				Layer2:
				[Frame0], [Frame1], [Frame2]
				Layer3:
				[Frame0], [Frame1], [Frame2]
			 */
			fbdev->vidmem_phys[j][i] = fbdev->vidmem_phys[0][0] +
				j * fbdev->frm_size +
				i * fbdev->frm_size * CONFIG_FB_INGENIC_NR_LAYERS;
			fbdev->vidmem[j][i] = fbdev->vidmem[0][0] +
				j * fbdev->frm_size +
				i * fbdev->frm_size * CONFIG_FB_INGENIC_NR_LAYERS;
		}
	}

	return 0;
}

static void ingenicfb_tlb_enable(struct ingenicfb_device *fbdev)
{
	struct ingenicfb_frm_cfg *frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	unsigned int glbc, val;
	int i;

	frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	lay_cfg = frm_cfg->lay_cfg;
	val = reg_read(fbdev, DC_TLB_GLBC);
	glbc = val;

	for(i = 0; i < DPU_SUPPORT_MAX_LAYERS; i++) {
		if(lay_cfg[i].lay_en) {
			if(lay_cfg[i].tlb_en != (glbc>>i & 0x1)) {
				glbc = lay_cfg[i].tlb_en ?
					(glbc | (0x1 << i)) : (glbc & ~(0x1 << i));
			}
		} else {
			glbc &= ~(0x1 << i);
		}
	}
	if(val != glbc) {
		reg_write(fbdev, DC_CTRL, DC_GEN_STP_CMP);
		wait_dc_state(DC_WORKING, 0);
		reg_write(fbdev, DC_TLB_GLBC, glbc);
	}
}

static void ingenicfb_tlb_configure(struct ingenicfb_device *fbdev)
{
	unsigned int tlbv = 0;

	tlbv |= (1 << DC_CNM_LBIT);
	tlbv |= (1 << DC_GCN_LBIT);

	/*mutex_lock(fbdev->lock);*/
	reg_write(fbdev, DC_TLB_TLBV, tlbv);
	reg_write(fbdev, DC_TLB_TLBA, fbdev->tlba);
	reg_write(fbdev, DC_TLB_TLBC, DC_CH0_INVLD | DC_CH1_INVLD |
			DC_CH2_INVLD | DC_CH3_INVLD);
	/*mutex_unlock(fbdev->lock);*/
}

#define DPU_WAIT_IRQ_TIME 8000
static int ingenicfb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct ingenicfb_device *fbdev = info->par;
	int ret;
	csc_mode_t csc_mode;
	int value;
	int tmp;
	int i;

	switch (cmd) {
		case JZFB_SET_VSYNCINT:
			if (unlikely(copy_from_user(&value, argp, sizeof(int))))
				return -EFAULT;
			tmp = reg_read(fbdev, DC_INTC);
			if (value) {
				reg_write(fbdev, DC_CLR_ST, DC_CLR_CMP_START);
				reg_write(fbdev, DC_INTC, tmp | DC_SOC_MSK);
			} else {
				reg_write(fbdev, DC_INTC, tmp & ~DC_SOC_MSK);
			}
			break;
		case FBIO_WAITFORVSYNC:
#ifndef CONFIG_FB_VSYNC_SKIP_DISABLE
			unlock_fb_info(info);
			ret = wait_event_interruptible_timeout(fbdev->vsync_wq,
					fbdev->timestamp.wp != fbdev->timestamp.rp,
					msecs_to_jiffies(DPU_WAIT_IRQ_TIME));
			lock_fb_info(info);
			if(ret == 0) {
				dev_err(info->dev, "DPU wait vsync timeout!\n");
				return -EFAULT;
			}

			ret = copy_to_user(argp, fbdev->timestamp.value + fbdev->timestamp.rp,
					sizeof(u64));
			fbdev->timestamp.rp = (fbdev->timestamp.rp + 1) % TIMESTAMP_CAP;

			if (unlikely(ret))
				return -EFAULT;
#else
			{
				int retry = 3;
				while(retry--) {
					unlock_fb_info(info);
					fbdev->timestamp.rp = fbdev->timestamp.wp;
					ret = wait_event_interruptible_timeout(fbdev->vsync_wq,
							fbdev->timestamp.wp != fbdev->timestamp.rp,
							msecs_to_jiffies(DPU_WAIT_IRQ_TIME));
					lock_fb_info(info);
					if(ret == 0) {
						dev_err(info->dev, "DPU wait vsync timeout!\n");
						return -EFAULT;
					}
					if (old_desc_addr != reg_read(fbdev, DC_FRM_DES)) break;
				}
			}
#endif
			break;
		case JZFB_PUT_FRM_CFG:
			ret = ingenicfb_check_frm_cfg(info, (struct ingenicfb_frm_cfg *)argp);
			if(ret) {
				return ret;
			}
			copy_from_user(&fbdev->current_frm_mode.frm_cfg,
				       (void *)argp,
				       sizeof(struct ingenicfb_frm_cfg));

			for (i = 0; i < ARRAY_SIZE(ingenicfb_colormodes); ++i) {
				struct ingenicfb_colormode *m = &ingenicfb_colormodes[i];
				if (m->mode == fbdev->current_frm_mode.frm_cfg.lay_cfg[0].format) {
					ingenicfb_colormode_to_var(&info->var, m);
					break;
				}
			}

			for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES; i++) {
				fbdev->current_frm_mode.update_st[i] = FRAME_CFG_NO_UPDATE;
			}
			break;
		case JZFB_GET_FRM_CFG:
			copy_to_user((void *)argp,
				      &fbdev->current_frm_mode.frm_cfg,
				      sizeof(struct ingenicfb_frm_cfg));
			break;
		case JZFB_GET_LAYERS_NUM:
			{
				unsigned int layers_num = CONFIG_FB_INGENIC_NR_LAYERS;
				copy_to_user((void *)argp,
					      &layers_num,
					      sizeof(unsigned int));
			}
			break;
		case JZFB_SET_CSC_MODE:
			if (unlikely(copy_from_user(&csc_mode, argp, sizeof(csc_mode_t))))
				return -EFAULT;
			csc_mode_set(fbdev, csc_mode);
			break;
		case JZFB_USE_TLB:
			if((unsigned int)arg != 0){
				fbdev->tlba = (unsigned int)arg;
				ingenicfb_tlb_configure(fbdev);
			} else
				printk("tlb err!!!\n");
			break;
		case JZFB_DMMU_MEM_SMASH:
			{
				struct smash_mode sm;
				if (copy_from_user(&sm, (void *)arg, sizeof(sm))) {
					ret = -EFAULT;
					break;
				}
				return dmmu_memory_smash(sm.vaddr, sm.mode);
				break;
			}
		case JZFB_DMMU_DUMP_MAP:
			{
				unsigned long vaddr;
				if (copy_from_user(&vaddr, (void *)arg, sizeof(unsigned long))) {
					ret = -EFAULT;
					break;
				}
				return dmmu_dump_map(vaddr);
				break;
			}
		case JZFB_DMMU_MAP:
			{
				struct dpu_dmmu_map_info di;
				if (copy_from_user(&di, (void *)arg, sizeof(di))) {
					ret = -EFAULT;
					break;
				}
				fbdev->user_addr = di.addr;
				return dmmu_map(info->dev,di.addr,di.len);
				break;
			}
		case JZFB_DMMU_UNMAP:
			{
				struct dpu_dmmu_map_info di;
				if (copy_from_user(&di, (void *)arg, sizeof(di))) {
					ret = -EFAULT;
					break;
				}
				return dmmu_unmap(info->dev,di.addr,di.len);
				break;
			}
		case JZFB_DMMU_UNMAP_ALL:
			dmmu_unmap_all(info->dev);
			break;
		case JZFB_DMMU_FLUSH_CACHE:
			{
				struct dpu_dmmu_map_info di;
				if (copy_from_user(&di, (void *)arg, sizeof(di))) {
					ret = -EFAULT;
					break;
				}
				return dmmu_flush_cache(di.addr,di.len);
				break;
			}
		default:
			printk("Command:%x Error!\n",cmd);
			break;
	}
	return 0;
}

static int ingenicfb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	unsigned long start;
	unsigned long off;
	u32 len;

	off = vma->vm_pgoff << PAGE_SHIFT;

	start = info->fix.smem_start;
	len = PAGE_ALIGN((start & ~PAGE_MASK) + info->fix.smem_len);
	start &= PAGE_MASK;

	if ((vma->vm_end - vma->vm_start + off) > len)
		return -EINVAL;
	off += start;

	vma->vm_pgoff = off >> PAGE_SHIFT;
	vma->vm_flags |= VM_IO;

	pgprot_val(vma->vm_page_prot) &= ~_CACHE_MASK;
	/* Write-Acceleration */
	pgprot_val(vma->vm_page_prot) |= _CACHE_CACHABLE_WA;

	if (io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
				vma->vm_end - vma->vm_start, vma->vm_page_prot))
	{
		return -EAGAIN;
	}

	return 0;
}

static void ingenicfb_set_vsync_value(struct ingenicfb_device *fbdev)
{
	fbdev->vsync_skip_map = (fbdev->vsync_skip_map >> 1 |
				fbdev->vsync_skip_map << 9) & 0x3ff;
	if(likely(fbdev->vsync_skip_map & 0x1)) {
		fbdev->timestamp.value[fbdev->timestamp.wp] =
			ktime_to_ns(ktime_get());
		fbdev->timestamp.wp = (fbdev->timestamp.wp + 1) % TIMESTAMP_CAP;
		wake_up_interruptible(&fbdev->vsync_wq);
#ifdef CONFIG_FB_VSYNC_SKIP_DISABLE
	} else {
		fbdev->timestamp.wp = fbdev->timestamp.rp + 1;
		wake_up_interruptible(&fbdev->vsync_wq);
#endif
	}
}

static irqreturn_t ingenicfb_irq_handler(int irq, void *data)
{
	unsigned int irq_flag;
	struct ingenicfb_device *fbdev = (struct ingenicfb_device *)data;

	spin_lock(&fbdev->irq_lock);

	dbg_irqcnt_inc(fbdev->dbg_irqcnt, irqcnt);

	irq_flag = reg_read(fbdev, DC_INT_FLAG);
	if(likely(irq_flag & DC_CMP_START)) {
		reg_write(fbdev, DC_CLR_ST, DC_CLR_CMP_START);
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, cmp_start);

		fbdev->frm_start++;
		ingenicfb_set_vsync_value(fbdev);
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
#define IRQ_P_N 50

	if(unlikely(irq_flag & DC_STOP_DISP_ACK)) {
		reg_write(fbdev, DC_CLR_ST, DC_CLR_STOP_DISP_ACK);
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, stop_disp_ack);
		cmp_gen_sop = 1;
		wake_up_interruptible(&fbdev->gen_stop_wq);
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}

	if(unlikely(irq_flag & DC_DISP_END)) {
		reg_write(fbdev, DC_CLR_ST, DC_CLR_DISP_END);
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, disp_end);
		//print_dbg("DC_DISP_END");
		//fbdev->dsi->master_ops->query_te(fbdev->dsi);
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}


	if(unlikely(irq_flag & DC_TFT_UNDR)) {
		reg_write(fbdev, DC_CLR_ST, DC_CLR_TFT_UNDR);
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, tft_under);
		fbdev->tft_undr_cnt++;
#ifdef CONFIG_FPGA_TEST
		if (!(fbdev->tft_undr_cnt % 100000)) //FIXME:
#endif
		printk("\nTFT_UNDR_num = %d\n\n", fbdev->tft_undr_cnt);
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}

#if 1
	if(likely(irq_flag & DC_WDMA_OVER)) {
		over_cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, wdma_over);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_WDMA_OVER);
#ifdef CONFIG_FPGA_TEST
		if (!(over_cnt % 1000)) //FIXME:
#endif
		print_dbg("\nDC_WDMA_OVER irq came here!!!!!!!!!!!!!!over_cnt = %d\n",over_cnt);
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_WDMA_END)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, wdma_end);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_WDMA_END);
		if(cnt < IRQ_P_N)
		print_dbg("DC_WDMA_END irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_LAY3_END)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, layer3_end);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_LAY3_END);
		if(cnt < IRQ_P_N)
		print_dbg("DC_LAY3_END irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_LAY2_END)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, layer2_end);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_LAY2_END);
		if(cnt < IRQ_P_N)
		print_dbg("DC_LAY2_END irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_LAY1_END)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, layer1_end);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_LAY1_END);
		if(cnt < IRQ_P_N)
		print_dbg("DC_LAY1_END irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_LAY0_END)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, layer0_end);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_LAY0_END);
		if(cnt < IRQ_P_N)
		print_dbg("DC_LAY0_END irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_CMP_END)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, clr_cmp_end);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_CMP_END);
		if(cnt < IRQ_P_N)
		print_dbg("DC_CMP_END irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_STOP_WRBK_ACK)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, stop_wrbk_ack);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_STOP_WRBK_ACK);
		if(cnt < IRQ_P_N)
		print_dbg("DC_STOP_SRD irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_SRD_START)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, srd_start);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_SRD_START);
		if(cnt < IRQ_P_N)
		print_dbg("DC_SRD_START irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_SRD_END)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, srd_end);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_SRD_END);
		if(cnt < IRQ_P_N)
		print_dbg("DC_SRD_END irq came here!!!!!!!!!!!!!!");
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
	if(likely(irq_flag & DC_CMP_W_SLOW)) {
		static int cnt = 0;
		cnt++;
		dbg_irqcnt_inc(fbdev->dbg_irqcnt, cmp_w_slow);
		reg_write(fbdev, DC_CLR_ST, DC_CLR_CMP_W_SLOW);
		if(cnt < IRQ_P_N)
		print_dbg("DC_CMP_W_SLOW came here!!!!!!!!!!!!!! DC_ST = 0x%lx cnt = %d",reg_read(fbdev,DC_ST),cnt);
		if(cnt > 10)
		reg_write(fbdev, DC_CMPW_PCFG_CTRL, 0 << 10 | 30);
		spin_unlock(&fbdev->irq_lock);
		return IRQ_HANDLED;
	}
#endif

	dev_err(fbdev->dev, "DPU irq nothing do, please check!!! DC_ST = 0x%lx\n",reg_read(fbdev,DC_ST));
	spin_unlock(&fbdev->irq_lock);
	return IRQ_HANDLED;
}

static inline uint32_t convert_color_to_hw(unsigned val, struct fb_bitfield *bf)
{
	return (((val << bf->length) + 0x7FFF - val) >> 16) << bf->offset;
}

static int ingenicfb_setcolreg(unsigned regno, unsigned red, unsigned green,
		unsigned blue, unsigned transp, struct fb_info *fb)
{
	if (regno >= 16)
		return -EINVAL;

	((uint32_t *)(fb->pseudo_palette))[regno] =
		convert_color_to_hw(red, &fb->var.red) |
		convert_color_to_hw(green, &fb->var.green) |
		convert_color_to_hw(blue, &fb->var.blue) |
		convert_color_to_hw(transp, &fb->var.transp);

	return 0;
}

static void __maybe_unused ingenicfb_display_sread_v_color_bar(struct fb_info *info)
{
	int i, j;
	int w, h;
	int bpp;
	unsigned short *p16;
	unsigned int *p32;
	struct ingenicfb_device *fbdev = info->par;
	struct fb_videomode *mode = fbdev->panel->modes;


	if (!mode) {
		dev_err(fbdev->dev, "%s, video mode is NULL\n", __func__);
		return;
	}

	p16 = (unsigned short *)fbdev->sread_vidmem[0];
	p32 = (unsigned int *)fbdev->sread_vidmem[0];
	w = mode->xres;
	h = mode->yres;
	bpp = info->var.bits_per_pixel;


	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++) {
			short c16;
			int c32 = 0;
			switch ((j / 10) % 4) {
				case 0:
					c16 = 0xF800;
					c32 = 0xFFFF0000;
					break;
				case 1:
					c16 = 0x07C0;
					c32 = 0xFF00FF00;
					break;
				case 2:
					c16 = 0x001F;
					c32 = 0xFF0000FF;
					break;
				default:
					c16 = 0xFFFF;
					c32 = 0xFFFFFFFF;
					break;
			}
			switch (bpp) {
				case 18:
				case 24:
				case 32:
					*p32++ = c32;
					break;
				default:
					*p16++ = c16;
			}
		}
		if (w % PIXEL_ALIGN) {
			switch (bpp) {
				case 18:
				case 24:
				case 32:
					p32 += (ALIGN(mode->xres, PIXEL_ALIGN) - w);
					break;
				default:
					p16 += (ALIGN(mode->xres, PIXEL_ALIGN) - w);
					break;
			}
		}
	}
}

static void ingenicfb_display_v_color_bar(struct fb_info *info)
{
	int i, j;
	int w, h;
	int bpp;
	unsigned short *p16;
	unsigned int *p32;
	struct ingenicfb_device *fbdev = info->par;
	struct fb_videomode *mode = fbdev->panel->modes;

	if (!mode) {
		dev_err(fbdev->dev, "%s, video mode is NULL\n", __func__);
		return;
	}
	if (!fbdev->vidmem_phys[fbdev->current_frm_desc][0]) {
		dev_err(fbdev->dev, "Not allocate frame buffer yet\n");
		return;
	}
	if (!fbdev->vidmem[fbdev->current_frm_desc][0])
		fbdev->vidmem[fbdev->current_frm_desc][0] =
			(void *)phys_to_virt(fbdev->vidmem_phys[fbdev->current_frm_desc][0]);
	p16 = (unsigned short *)fbdev->vidmem[fbdev->current_frm_desc][0];
	p32 = (unsigned int *)fbdev->vidmem[fbdev->current_frm_desc][0];
	w = mode->xres;
	h = mode->yres;
	bpp = info->var.bits_per_pixel;

	dev_info(info->dev,
			"LCD V COLOR BAR w,h,bpp(%d,%d,%d) fbdev->vidmem[0]=%p\n", w, h,
			bpp, fbdev->vidmem[fbdev->current_frm_desc][0]);


	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++) {
			short c16;
			int c32 = 0;
			switch ((j / 10) % 4) {
				case 0:
					c16 = 0xF800;
					c32 = 0xFFFF0000;
					break;
				case 1:
					c16 = 0x07C0;
					c32 = 0xFF00FF00;
					break;
				case 2:
					c16 = 0x001F;
					c32 = 0xFF0000FF;
					break;
				default:
					c16 = 0xFFFF;
					c32 = 0xFFFFFFFF;
					break;
			}
			switch (bpp) {
				case 18:
				case 24:
				case 32:
					*p32++ = c32;
					break;
				default:
					*p16++ = c16;
			}
		}
		if (w % PIXEL_ALIGN) {
			switch (bpp) {
				case 18:
				case 24:
				case 32:
					p32 += (ALIGN(mode->xres, PIXEL_ALIGN) - w);
					break;
				default:
					p16 += (ALIGN(mode->xres, PIXEL_ALIGN) - w);
					break;
			}
		}
	}
}

static void ingenicfb_display_h_color_bar(struct fb_info *info)
{
	int i, j;
	int w, h;
	int bpp;
	unsigned short *p16;
	unsigned int *p32;
	struct ingenicfb_device *fbdev = info->par;
	struct fb_videomode *mode = fbdev->panel->modes;

	if (!mode) {
		dev_err(fbdev->dev, "%s, video mode is NULL\n", __func__);
		return;
	}
	if (!fbdev->vidmem_phys[fbdev->current_frm_desc][0]) {
		dev_err(fbdev->dev, "Not allocate frame buffer yet\n");
		return;
	}
	if (!fbdev->vidmem[fbdev->current_frm_desc][0])
		fbdev->vidmem[fbdev->current_frm_desc][0] =
			(void *)phys_to_virt(fbdev->vidmem_phys[fbdev->current_frm_desc][0]);
	p16 = (unsigned short *)fbdev->vidmem[fbdev->current_frm_desc][0];
	p32 = (unsigned int *)fbdev->vidmem[fbdev->current_frm_desc][0];
	w = mode->xres;
	h = mode->yres;
	bpp = info->var.bits_per_pixel;

	dev_info(info->dev,
			"LCD H COLOR BAR w,h,bpp(%d,%d,%d), fbdev->vidmem[0]=%p\n", w, h,
			bpp, fbdev->vidmem[fbdev->current_frm_desc][0]);

	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++) {
			short c16;
			int c32;
			switch ((i / 10) % 4) {
				case 0:
					c16 = 0xF800;
					c32 = 0x00FF0000;
					break;
				case 1:
					c16 = 0x07C0;
					c32 = 0x0000FF00;
					break;
				case 2:
					c16 = 0x001F;
					c32 = 0x000000FF;
					break;
				default:
					c16 = 0xFFFF;
					c32 = 0xFFFFFFFF;
					break;
			}
			switch (bpp) {
				case 18:
				case 24:
				case 32:
					*p32++ = c32;
					break;
				default:
					*p16++ = c16;
			}
		}
		if (w % PIXEL_ALIGN) {
			switch (bpp) {
				case 18:
				case 24:
				case 32:
					p32 += (ALIGN(mode->xres, PIXEL_ALIGN) - w);
					break;
				default:
					p16 += (ALIGN(mode->xres, PIXEL_ALIGN) - w);
					break;
			}
		}
	}
}

int lcd_display_inited_by_uboot( void )
{
	if (*(unsigned int*)(0xb3050000 + DC_ST) & DC_WORKING)
		uboot_inited = 1;
	else
		uboot_inited = 0;
	return uboot_inited;
}

static int slcd_pixel_refresh_times(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;
	struct smart_config *smart_config = panel->smart_config;

	switch(smart_config->smart_type){
	case SMART_LCD_TYPE_8080:
	case SMART_LCD_TYPE_6800:
		break;
	case SMART_LCD_TYPE_SPI_3:
		return 9;
	case SMART_LCD_TYPE_SPI_4:
		return 8;
	default:
		printk("%s %d err!\n",__func__,__LINE__);
		break;
	}

	switch(smart_config->pix_fmt) {
	case SMART_LCD_FORMAT_888:
		if(smart_config->dwidth == SMART_LCD_DWIDTH_8_BIT)
			return 3;
		if(smart_config->dwidth == SMART_LCD_DWIDTH_24_BIT)
			return 1;
	case SMART_LCD_FORMAT_565:
		if(smart_config->dwidth == SMART_LCD_DWIDTH_8_BIT)
			return 2;
		if(smart_config->dwidth == SMART_LCD_DWIDTH_16_BIT)
			return 1;
	default:
		printk("%s %d err!\n",__func__,__LINE__);
		break;
	}

	return 1;
}

static void ingenic_set_pixclk(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	unsigned long rate, prate;
	struct clk *clk;

	rate = PICOS2KHZ(info->var.pixclock) * 1000;
	clk = clk_get_parent(fbdev->pclk);
	prate = clk_get_rate(clk);

	if(prate % rate)
		rate = prate / (prate / rate) + 1;
	clk_set_rate(fbdev->pclk, rate);
}


static int refresh_pixclock_auto_adapt(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;
	struct fb_var_screeninfo *var = &info->var;
	struct fb_videomode *mode;
	uint16_t hds, vds;
	uint16_t hde, vde;
	uint16_t ht, vt;
	unsigned long rate;

	mode = panel->modes;
	if (mode == NULL) {
		dev_err(fbdev->dev, "%s get video mode failed\n", __func__);
		return -EINVAL;
	}

	hds = mode->hsync_len + mode->left_margin;
	hde = hds + mode->xres;
	ht = hde + mode->right_margin;

	vds = mode->vsync_len + mode->upper_margin;
	vde = vds + mode->yres;
	vt = vde + mode->lower_margin;

	if(mode->refresh){
		rate = mode->refresh * vt * ht;
		if(fbdev->panel->lcd_type == LCD_TYPE_SLCD) {
			rate *= slcd_pixel_refresh_times(info);
		}

		mode->pixclock = KHZ2PICOS(round_up(rate, 1000)/1000);
		var->pixclock = mode->pixclock;
	}else if(mode->pixclock){
		rate = PICOS2KHZ(mode->pixclock) * 1000;
		mode->refresh = rate / vt / ht;
		if(fbdev->panel->lcd_type == LCD_TYPE_SLCD)
			mode->refresh /= slcd_pixel_refresh_times(info);
	}else{
		dev_err(fbdev->dev,"%s error:lcd important config info is absenced\n",__func__);
		return -EINVAL;
	}

	return 0;
}

static void ingenicfb_enable(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;

	mutex_lock(&fbdev->lock);
	if (fbdev->is_lcd_en) {
		mutex_unlock(&fbdev->lock);
		return;
	}


	if(panel->lcd_type == LCD_TYPE_SLCD) {
		slcd_send_mcu_command(fbdev, panel->smart_config->write_gram_cmd);
		wait_slcd_busy();
	}
#ifdef CONFIG_INGENIC_FB_SIMPLE_RDMA
	ingenicfb_rdma_start(info);
#else
	ingenicfb_cmp_start(info);
#endif

	fbdev->is_lcd_en = 1;
	mutex_unlock(&fbdev->lock);
	return;
}

static void ingenicfb_disable(struct fb_info *info, stop_mode_t stop_md)
{
	mutex_lock(&fbdev->lock);
	if (!fbdev->is_lcd_en) {
		mutex_unlock(&fbdev->lock);
		return;
	}

	if(stop_md == QCK_STOP) {
		reg_write(fbdev, DC_CTRL, DC_QCK_STP_CMP);
		udelay(20);
		reg_write(fbdev, DC_CTRL, DC_QCK_STP_RDMA);
		wait_dc_state(DC_WORKING, 0);
	} else {
		int ret;
		reg_write(fbdev, DC_CTRL, DC_GEN_STP_CMP);
		reg_write(fbdev, DC_CTRL, DC_GEN_STP_RDMA);
		ret = wait_event_interruptible_timeout(fbdev->gen_stop_wq,
				!(reg_read(fbdev, DC_ST) & DC_WORKING),msecs_to_jiffies(30000));
		if(ret == 0) {
			dev_err(info->dev, "DPU wait gen stop timeout!!!\n");
		}
		print_dbg("spend time = %d\n",30000-jiffies_to_msecs(ret));
	}

	fbdev->is_lcd_en = 0;
	mutex_unlock(&fbdev->lock);
}

static int ingenicfb_desc_init(struct fb_info *info, int frm_num)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;
	struct fb_videomode *mode;
	struct ingenicfb_frm_mode *frm_mode;
	struct ingenicfb_frm_cfg *frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	struct fb_var_screeninfo *var = &info->var;
	struct ingenicfb_framedesc **framedesc;
	struct ingenicfb_sreadesc **sreadesc;
	int frm_num_mi, frm_num_ma;
	int frm_size;
	int i, j;
	int ret = 0;

	mode = ingenicfb_get_mode(var, info);
	if (mode == NULL) {
		dev_err(info->dev, "%s get video mode failed\n", __func__);
		return -EINVAL;
	}

	framedesc = fbdev->framedesc;
	sreadesc = fbdev->sreadesc;
	frm_mode = &fbdev->current_frm_mode;
	frm_cfg = &frm_mode->frm_cfg;
	lay_cfg = frm_cfg->lay_cfg;

	ret = ingenicfb_check_frm_cfg(info, frm_cfg);
	if(ret) {
		dev_err(info->dev, "%s configure framedesc[%d] error!\n", __func__, frm_num);
		return ret;
	}

	if(frm_num == FRAME_CFG_ALL_UPDATE) {
		frm_num_mi = 0;
		frm_num_ma = CONFIG_FB_INGENIC_NR_FRAMES * 2;
	} else {
		if(frm_num < 0 || frm_num > (CONFIG_FB_INGENIC_NR_FRAMES * 2 - 1)) {
			dev_err(info->dev, "framedesc num err!\n");
			return -EINVAL;
		}
		frm_num_mi = frm_num;
		frm_num_ma = frm_num_mi + 1;
	}


	frm_size = mode->xres * mode->yres;
	fbdev->frm_size = frm_size * MAX_BITS_PER_PIX >> 3;
	info->screen_size = fbdev->frm_size;

	for(i = frm_num_mi; i < frm_num_ma; i++) {
		framedesc[i]->FrameNextCfgAddr = fbdev->framedesc_phys[i];
		framedesc[i]->FrameSize.d32 = 0;
		framedesc[i]->FrameSize.b.width = mode->xres;
		framedesc[i]->FrameSize.b.height = mode->yres;
		framedesc[i]->FrameCtrl.d32 = 0;
		framedesc[i]->FrameCtrl.b.stop = 1;
		framedesc[i]->FrameCtrl.b.wb_en = fbdev->sread_vidmem_size > 0 ? fbdev->wback_en : 0;
		framedesc[i]->FrameCtrl.b.direct_en = 1;
		if(fbdev->slcd_continua ||
			(panel->lcd_type == LCD_TYPE_TFT))
			framedesc[i]->FrameCtrl.b.change_2_rdma = 1;
		else
			framedesc[i]->FrameCtrl.b.change_2_rdma = 0;
#ifdef COMPOSER_DIRECT_OUT_EN
		framedesc[i]->FrameCtrl.b.stop = 0;
		framedesc[i]->FrameCtrl.b.change_2_rdma = 0;
#endif
		framedesc[i]->FrameCtrl.b.wb_dither_en = 0;
		framedesc[i]->FrameCtrl.b.wb_dither_auto = 0;
		framedesc[i]->FrameCtrl.b.wb_dither_auto = 0;
		framedesc[i]->FrameCtrl.b.wb_dither_b_dw = 0;
		framedesc[i]->FrameCtrl.b.wb_dither_g_dw = 0;
		framedesc[i]->FrameCtrl.b.wb_dither_r_dw = 0;
//		framedesc[i]->WritebackAddr = (uint32_t)fbdev->sread_vidmem_phys[0] + i*fbdev->frm_size;
		framedesc[i]->WritebackAddr = (uint32_t)fbdev->sread_vidmem_phys[0];
		framedesc[i]->WritebackStride = mode->xres;
		framedesc[i]->FrameCtrl.b.wb_format = DC_WB_FORMAT_888;
		framedesc[i]->Layer0CfgAddr = fbdev->layerdesc_phys[i][0];
		framedesc[i]->Layer1CfgAddr = fbdev->layerdesc_phys[i][1];
		framedesc[i]->Layer2CfgAddr = fbdev->layerdesc_phys[i][2];
		framedesc[i]->Layer3CfgAddr = fbdev->layerdesc_phys[i][3];
		framedesc[i]->LayCfgEn.d32 = 0;
		framedesc[i]->LayCfgEn.b.lay0_scl_en = lay_cfg[0].lay_scale_en;
		framedesc[i]->LayCfgEn.b.lay1_scl_en = lay_cfg[1].lay_scale_en;
		framedesc[i]->LayCfgEn.b.lay2_scl_en = lay_cfg[2].lay_scale_en;
		framedesc[i]->LayCfgEn.b.lay3_scl_en = lay_cfg[3].lay_scale_en;
		framedesc[i]->LayCfgEn.b.lay0_en = lay_cfg[0].lay_en;
		framedesc[i]->LayCfgEn.b.lay1_en = lay_cfg[1].lay_en;
		framedesc[i]->LayCfgEn.b.lay2_en = lay_cfg[2].lay_en;
		framedesc[i]->LayCfgEn.b.lay3_en = lay_cfg[3].lay_en;
		framedesc[i]->LayCfgEn.b.lay0_z_order = lay_cfg[0].lay_z_order;
		framedesc[i]->LayCfgEn.b.lay1_z_order = lay_cfg[1].lay_z_order;
		framedesc[i]->LayCfgEn.b.lay2_z_order = lay_cfg[2].lay_z_order;
		framedesc[i]->LayCfgEn.b.lay3_z_order = lay_cfg[3].lay_z_order;
#ifndef TEST_IRQ
		framedesc[i]->InterruptControl.d32 = DC_SOC_MSK;
#else
		framedesc[i]->InterruptControl.d32 = DC_SOC_MSK | DC_EOD_MSK | DC_EOW_MSK | DC_EOC_MSK;
#endif
	}

	for(i = frm_num_mi; i < frm_num_ma; i++) {
		for(j =0; j < DPU_SUPPORT_MAX_LAYERS; j++) {
			if(!lay_cfg[j].lay_en)
				continue;

			fbdev->layerdesc[i][j]->LayerSize.d32 = 0;
			fbdev->layerdesc[i][j]->LayerSize.b.width = lay_cfg[j].source_w;
			fbdev->layerdesc[i][j]->LayerSize.b.height = lay_cfg[j].source_h;
			fbdev->layerdesc[i][j]->LayerPos.d32 = 0;
			fbdev->layerdesc[i][j]->LayerPos.b.x_pos = lay_cfg[j].disp_pos_x;
			fbdev->layerdesc[i][j]->LayerPos.b.y_pos = lay_cfg[j].disp_pos_y;
			fbdev->layerdesc[i][j]->LayerCfg.d32 = 0;
			fbdev->layerdesc[i][j]->LayerCfg.b.g_alpha_en = lay_cfg[j].g_alpha_en;
			fbdev->layerdesc[i][j]->LayerCfg.b.g_alpha = lay_cfg[j].g_alpha_val;
			fbdev->layerdesc[i][j]->LayerCfg.b.color = lay_cfg[j].color;
			fbdev->layerdesc[i][j]->LayerCfg.b.domain_multi = 1;
			fbdev->layerdesc[i][j]->LayerCfg.b.format = lay_cfg[j].format;
			fbdev->layerdesc[i][j]->LayerCfg.b.sharpl = 0;
			fbdev->layerdesc[i][j]->LayerStride = lay_cfg[j].stride;
			fbdev->layerdesc[i][j]->LayerScale.d32 = 0;
			if(lay_cfg[j].lay_scale_en) {
				fbdev->layerdesc[i][j]->LayerScale.b.target_width = lay_cfg[j].scale_w;
				fbdev->layerdesc[i][j]->LayerScale.b.target_height = lay_cfg[j].scale_h;
				fbdev->layerdesc[i][j]->layerresizecoef_x = (512 * lay_cfg[j].source_w) / lay_cfg[j].scale_w;
				fbdev->layerdesc[i][j]->layerresizecoef_y = (512 * lay_cfg[j].source_h) / lay_cfg[j].scale_h;
			} else {
				fbdev->layerdesc[i][j]->LayerScale.b.target_width = 0;
				fbdev->layerdesc[i][j]->LayerScale.b.target_height = 0;
				fbdev->layerdesc[i][j]->layerresizecoef_x = 0;
				fbdev->layerdesc[i][j]->layerresizecoef_y = 0;
			}
			if(!lay_cfg[j].tlb_en) {
				fbdev->layerdesc[i][j]->LayerBufferAddr =
					fbdev->vidmem_phys[j][i % CONFIG_FB_INGENIC_NR_FRAMES];
			} else {
				fbdev->layerdesc[i][j]->LayerBufferAddr =
					lay_cfg[j].addr[i % CONFIG_FB_INGENIC_NR_FRAMES];
			}
			if(lay_cfg[j].uv_offset[i % CONFIG_FB_INGENIC_NR_FRAMES]) {
				fbdev->layerdesc[i][j]->UVBufferAddr =
					fbdev->layerdesc[i][j]->LayerBufferAddr +
					lay_cfg[j].uv_offset[i % CONFIG_FB_INGENIC_NR_FRAMES];
			} else {
				fbdev->layerdesc[i][j]->UVBufferAddr =
					fbdev->layerdesc[i][j]->LayerBufferAddr +
					lay_cfg[j].stride * lay_cfg[j].source_h;
			}
			fbdev->layerdesc[i][j]->UVStride = lay_cfg[j].stride;
		}
	}

#ifdef CONFIG_INGENIC_FB_SIMPLE_RDMA
	for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES; i++) {
		sreadesc[i]->RdmaNextCfgAddr = fbdev->sreadesc_phys[0] + i*fbdev->frm_size;
		sreadesc[i]->FrameBufferAddr = fbdev->sread_vidmem_phys[0] + i*fbdev->frm_size;
		sreadesc[i]->Stride = mode->xres;
		sreadesc[i]->ChainCfg.d32 = 0;
		sreadesc[i]->ChainCfg.b.format = RDMA_CHAIN_CFG_FORMA_888;
		sreadesc[i]->ChainCfg.b.color = RDMA_CHAIN_CFG_COLOR_RGB;
		sreadesc[i]->ChainCfg.b.change_2_cmp = 0;
		if(fbdev->slcd_continua ||
			(panel->lcd_type == LCD_TYPE_TFT))
			sreadesc[i]->ChainCfg.b.chain_end = 0;
		else
			sreadesc[i]->ChainCfg.b.chain_end = 1;
#ifdef TEST_IRQ
		sreadesc[i]->InterruptControl.d32 = DC_SOS_MSK | DC_EOS_MSK | DC_EOD_MSK;
#else
		sreadesc[i]->InterruptControl.d32 = 0;
#endif
	}
#endif

	for(i = frm_num_mi; i < frm_num_ma; i++) {
		frm_mode->update_st[i] = FRAME_CFG_UPDATE;
	}

	return 0;
}

static int ingenicfb_update_frm_mode(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct fb_videomode *mode;
	struct ingenicfb_frm_mode *frm_mode;
	struct ingenicfb_frm_cfg *frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	struct fb_var_screeninfo *var = &info->var;
	int i;

	mode = ingenicfb_get_mode(var, info);
	if (mode == NULL) {
		dev_err(info->dev, "%s get video mode failed\n", __func__);
		return -EINVAL;
	}

	frm_mode = &fbdev->current_frm_mode;
	frm_cfg = &frm_mode->frm_cfg;
	lay_cfg = frm_cfg->lay_cfg;

	/*Only set layer0 work*/
	lay_cfg[0].lay_en = 1;
	lay_cfg[0].lay_z_order = LAYER_Z_ORDER_3; /* top */
	lay_cfg[1].lay_en = 0;
	lay_cfg[1].lay_z_order = LAYER_Z_ORDER_2;
	lay_cfg[2].lay_en = 0;
	lay_cfg[2].lay_z_order = LAYER_Z_ORDER_1;
	lay_cfg[3].lay_en = 0;
	lay_cfg[3].lay_z_order = LAYER_Z_ORDER_0;

	for(i = 0; i < DPU_SUPPORT_MAX_LAYERS; i++) {
		lay_cfg[i].source_w = mode->xres;
		lay_cfg[i].source_h = mode->yres;
		lay_cfg[i].disp_pos_x = 0;
		lay_cfg[i].disp_pos_y = 0;
		lay_cfg[i].g_alpha_en = 0;
		lay_cfg[i].g_alpha_val = 0xff;
		lay_cfg[i].color = ingenicfb_colormodes[0].color;
		lay_cfg[i].format = ingenicfb_colormodes[0].mode;
		lay_cfg[i].stride = mode->xres;
		lay_cfg[i].scale_w = 0;
		lay_cfg[i].scale_h = 0;
	}

	for(i = 0; i < CONFIG_FB_INGENIC_NR_FRAMES; i++) {
		fbdev->framedesc_array_next[i] = i + CONFIG_FB_INGENIC_NR_FRAMES;
		fbdev->framedesc_array_use[i] = i;
	}
	fbdev->current_frm_desc = 0;

	return 0;
}

static void disp_common_init(struct lcd_panel *ingenicfb_panel) {
	uint32_t disp_com;

	disp_com = reg_read(fbdev, DC_DISP_COM);
	disp_com &= ~DC_DP_IF_SEL_MASK;
	if(ingenicfb_panel->lcd_type == LCD_TYPE_SLCD) {
		disp_com |= DC_DISP_COM_SLCD;
	} else if(ingenicfb_panel->lcd_type == LCD_TYPE_MIPI_SLCD) {
		disp_com |= DC_DISP_COM_MIPI_SLCD;
	} else {
		disp_com |= DC_DISP_COM_TFT;
	}
	if(ingenicfb_panel->dither_enable) {
		disp_com |= DC_DP_DITHER_EN;
		disp_com &= ~DC_DP_DITHER_DW_MASK;
		disp_com |= ingenicfb_panel->dither.dither_red
			     << DC_DP_DITHER_DW_RED_LBIT;
		disp_com |= ingenicfb_panel->dither.dither_green
			    << DC_DP_DITHER_DW_GREEN_LBIT;
		disp_com |= ingenicfb_panel->dither.dither_blue
			    << DC_DP_DITHER_DW_BLUE_LBIT;
	} else {
		disp_com &= ~DC_DP_DITHER_EN;
	}
	reg_write(fbdev, DC_DISP_COM, disp_com);

	/* QOS */
#if 0
	reg_write(fbdev, DC_PCFG_RD_CTRL, 7);
	reg_write(fbdev, DC_PCFG_WR_CTRL, 1);
	reg_write(fbdev, DC_WDMA_PCFG, 0x1ff << 18 | 0x1f0 << 9 | 0x1e0 << 0);
	reg_write(fbdev, DC_OFIFO_PCFG, 0x1ff << 18 | 0x1f0 << 9 | 0x1e0 << 0);
	reg_write(fbdev, DC_CMPW_PCFG_CTRL, 1 << 10);
#endif
}

static void common_cfg_init(void)
{
	unsigned com_cfg = 0;

	com_cfg = reg_read(fbdev, DC_COM_CONFIG);
	/*Keep COM_CONFIG reg first bit 0 */
	com_cfg &= ~DC_OUT_SEL;

	/* set burst length 32*/
	com_cfg &= ~DC_BURST_LEN_BDMA_MASK;
	com_cfg |= DC_BURST_LEN_BDMA_32;
	com_cfg &= ~DC_BURST_LEN_WDMA_MASK;
	com_cfg |= DC_BURST_LEN_WDMA_32;
	com_cfg &= ~DC_BURST_LEN_RDMA_MASK;
	com_cfg |= DC_BURST_LEN_RDMA_32;

#ifdef CONFIG_INGENIC_FB_SIMPLE_RDMA
	com_cfg |= 0x2;
#else
	com_cfg &= ~0x2;
#endif

	reg_write(fbdev, DC_COM_CONFIG, com_cfg);
}

static int ingenicfb_set_fix_par(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct lcd_panel *panel = fbdev->panel;
	unsigned int intc;
	unsigned int disp_com;
	unsigned int is_lcd_en;

	mutex_lock(&fbdev->lock);
	is_lcd_en = fbdev->is_lcd_en;
	mutex_unlock(&fbdev->lock);

	ingenicfb_disable(info, GEN_STOP);

	disp_common_init(panel);

	common_cfg_init();

	reg_write(fbdev, DC_CLR_ST, 0x01FFFFFE);

	disp_com = reg_read(fbdev, DC_DISP_COM);

	switch (panel->lcd_type) {
	case LCD_TYPE_TFT:
		ingenicfb_tft_set_par(info);
#ifdef CONFIG_FB_INGENIC_MIPI_DSI
		fbdev->dsi->master_ops->mode_cfg(fbdev->dsi, 1);
#endif
		break;
	case LCD_TYPE_SLCD:
		ingenicfb_slcd_set_par(info);
		break;
	case LCD_TYPE_MIPI_SLCD:
		ingenicfb_slcd_set_par(info);
#ifdef CONFIG_FB_INGENIC_MIPI_DSI
		fbdev->dsi->master_ops->mode_cfg(fbdev->dsi, 0);
#endif
		break;
	}

	/*       disp end      gen_stop    tft_under    frm_start    frm_end     wback over  GEN_STOP_SRD*/
//	intc = DC_EOD_MSK | DC_SDA_MSK | DC_UOT_MSK | DC_SOC_MSK | DC_EOF_MSK | DC_OOW_MSK | DC_SSA_MSK;
	intc = DC_EOD_MSK | DC_SDA_MSK | DC_UOT_MSK | DC_SOC_MSK | DC_OOW_MSK;
	reg_write(fbdev, DC_INTC, intc);

	ingenicfb_tlb_configure(fbdev);
	ingenicfb_tlb_enable(fbdev);

	if(is_lcd_en) {
		ingenicfb_enable(info);
	}

	return 0;
}

static int ingenicfb_set_par(struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	struct fb_var_screeninfo *var = &info->var;
	struct fb_videomode *mode;
	uint32_t colormode;
	unsigned long flags;
	int ret;

	mode = ingenicfb_get_mode(var, info);
	if (mode == NULL) {
		dev_err(info->dev, "%s get video mode failed\n", __func__);
		return -EINVAL;
	}
	info->mode = mode;

	ret = ingenicfb_check_colormode(var, &colormode);
	if(ret) {
		dev_err(info->dev,"Check colormode failed!\n");
		return  ret;
	}

	fbdev->current_frm_mode.frm_cfg.lay_cfg[0].format = colormode;

	ret = ingenicfb_desc_init(info, FRAME_CFG_ALL_UPDATE);
	if(ret) {
		dev_err(fbdev->dev, "Desc init err!\n");
		return ret;
	}


	spin_lock_irqsave(&fbdev->irq_lock, flags);

	reg_write(fbdev, DC_FRM_CFG_ADDR, fbdev->framedesc_phys[fbdev->framedesc_array_use[fbdev->current_frm_desc]]);

#ifdef CONFIG_INGENIC_FB_SIMPLE_RDMA
	reg_write(fbdev, DC_RDMA_CHAIN_ADDR, fbdev->sreadesc_phys[0]);
#endif
	spin_unlock_irqrestore(&fbdev->irq_lock, flags);

	return 0;
}

int test_pattern(struct ingenicfb_device *fbdev)
{
	int ret;

	ingenicfb_disable(fbdev->fb, QCK_STOP);
#ifdef CONFIG_INGENIC_FB_SIMPLE_RDMA
	ingenicfb_display_sread_v_color_bar(fbdev->fb);
#else
	ingenicfb_display_h_color_bar(fbdev->fb);
#endif
	fbdev->current_frm_desc = 0;
	ingenicfb_set_fix_par(fbdev->fb);
	ret = ingenicfb_set_par(fbdev->fb);
	if(ret) {
		dev_err(fbdev->dev, "Set par failed!\n");
		return ret;
	}
	ingenicfb_enable(fbdev->fb);

	return 0;
}

static inline int timeval_sub_to_us(struct timeval lhs,
						struct timeval rhs)
{
	int sec, usec;
	sec = lhs.tv_sec - rhs.tv_sec;
	usec = lhs.tv_usec - rhs.tv_usec;

	return (sec*1000000 + usec);
}

static inline int time_us2ms(int us)
{
	return (us/1000);
}

static void calculate_frame_rate(void)
{
	static struct timeval time_now, time_last;
	unsigned int interval_in_us;
	unsigned int interval_in_ms;
	static unsigned int fpsCount = 0;

	switch(showFPS){
	case 1:
		fpsCount++;
		do_gettimeofday(&time_now);
		interval_in_us = timeval_sub_to_us(time_now, time_last);
		if ( interval_in_us > (USEC_PER_SEC) ) { /* 1 second = 1000000 us. */
			printk(" Pan display FPS: %d\n",fpsCount);
			fpsCount = 0;
			time_last = time_now;
		}
		break;
	case 2:
		do_gettimeofday(&time_now);
		interval_in_us = timeval_sub_to_us(time_now, time_last);
		interval_in_ms = time_us2ms(interval_in_us);
		printk(" Pan display interval ms: %d\n",interval_in_ms);
		time_last = time_now;
		break;
	default:
		if (showFPS > 3) {
			int d, f;
			fpsCount++;
			do_gettimeofday(&time_now);
			interval_in_us = timeval_sub_to_us(time_now, time_last);
			if (interval_in_us > USEC_PER_SEC * showFPS ) { /* 1 second = 1000000 us. */
				d = fpsCount / showFPS;
				f = (fpsCount * 10) / showFPS - d * 10;
				printk(" Pan display FPS: %d.%01d\n", d, f);
				fpsCount = 0;
				time_last = time_now;
			}
		}
		break;
	}
}

static int ingenicfb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;
	int next_frm;
	int ret = 0;

	if (var->xoffset - info->var.xoffset) {
		dev_err(info->dev, "No support for X panning for now\n");
		return -EINVAL;
	}

	fbdev->pan_display_count++;
	if(showFPS){
		calculate_frame_rate();
	}

	next_frm = var->yoffset / var->yres;

	if(fbdev->current_frm_mode.update_st[next_frm] == FRAME_CFG_NO_UPDATE) {
		if((ret = ingenicfb_desc_init(info, fbdev->framedesc_array_next[next_frm]))) {
			dev_err(info->dev, "%s: desc init err!\n", __func__);
			return ret;
		}
		fbdev->framedesc_array_use[next_frm] = fbdev->framedesc_array_next[next_frm];
		if(fbdev->framedesc_array_next[next_frm] < CONFIG_FB_INGENIC_NR_FRAMES)
			fbdev->framedesc_array_next[next_frm] = next_frm + CONFIG_FB_INGENIC_NR_FRAMES;
		else
			fbdev->framedesc_array_next[next_frm] = next_frm;
	}

#ifdef CONFIG_FB_VSYNC_SKIP_DISABLE
	old_desc_addr = reg_read(fbdev, DC_FRM_DES);
#endif

	ingenicfb_tlb_enable(fbdev);
	reg_write(fbdev, DC_FRM_CFG_ADDR, fbdev->framedesc_phys[fbdev->framedesc_array_use[next_frm]]);
	ingenicfb_cmp_start(info);

	if(!fbdev->is_lcd_en)
		ingenicfb_enable(info);

	fbdev->current_frm_desc = next_frm;

	return 0;
}

static void ingenicfb_do_resume(struct ingenicfb_device *fbdev)
{
	int ret;

	mutex_lock(&fbdev->suspend_lock);
	if(fbdev->is_suspend) {
		fbdev->timestamp.rp = 0;
		fbdev->timestamp.wp = 0;
		ingenicfb_clk_enable(fbdev);
#ifdef CONFIG_FB_INGENIC_MIPI_DSI
		fbdev->dsi->master_ops->set_blank_mode(fbdev->dsi, FB_BLANK_UNBLANK);
#endif
		ingenicfb_set_fix_par(fbdev->fb);
		ret = ingenicfb_set_par(fbdev->fb);
		if(ret) {
			dev_err(fbdev->dev, "Set par failed!\n");
		}
		ingenicfb_enable(fbdev->fb);
		fbdev->is_suspend = 0;
	}
	mutex_unlock(&fbdev->suspend_lock);
}

static void ingenicfb_do_suspend(struct ingenicfb_device *fbdev)
{
	struct lcd_panel *panel = fbdev->panel;
	struct lcd_panel_ops *panel_ops;

	panel_ops = panel->ops;

	mutex_lock(&fbdev->suspend_lock);
	if (!fbdev->is_suspend){
#ifdef CONFIG_FB_INGENIC_MIPI_DSI
		fbdev->dsi->master_ops->set_blank_mode(fbdev->dsi, FB_BLANK_POWERDOWN);
#endif
		ingenicfb_disable(fbdev->fb, QCK_STOP);
		ingenicfb_clk_disable(fbdev);

		fbdev->is_suspend = 1;
	}
	mutex_unlock(&fbdev->suspend_lock);
}

static int ingenicfb_blank(int blank_mode, struct fb_info *info)
{
	struct ingenicfb_device *fbdev = info->par;

	if (blank_mode == FB_BLANK_UNBLANK) {
		ingenicfb_do_resume(fbdev);
	} else {
		ingenicfb_do_suspend(fbdev);
	}

	return 0;
}

static int ingenicfb_open(struct fb_info *info, int user)
{
	struct ingenicfb_device *fbdev = info->par;
	int ret;

	if (!fbdev->is_lcd_en && fbdev->vidmem_phys) {
		fbdev->timestamp.rp = 0;
		fbdev->timestamp.wp = 0;
		ingenicfb_set_fix_par(info);
		ret = ingenicfb_set_par(info);
		if(ret) {
			dev_err(info->dev, "Set par failed!\n");
			return ret;
		}
		memset(fbdev->vidmem[fbdev->current_frm_desc][0], 0, fbdev->frm_size);
		ingenicfb_enable(info);
	}

	dev_dbg(info->dev, "####open count : %d\n", ++fbdev->open_cnt);

	return 0;
}

static int ingenicfb_release(struct fb_info *info, int user)
{
	struct ingenicfb_device *fbdev = info->par;

	dev_dbg(info->dev, "####close count : %d\n", fbdev->open_cnt--);
	if(!fbdev->open_cnt) {
//		fbdev->timestamp.rp = 0;
//		fbdev->timestamp.wp = 0;
	}
	return 0;
}

static ssize_t ingenicfb_write(struct fb_info *info, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct ingenicfb_device *fbdev = info->par;
	u8 *buffer, *src;
	u8 __iomem *dst;
	int c, cnt = 0, err = 0;
	unsigned long total_size;
	unsigned long p = *ppos;
	int next_frm = 0;

	total_size = info->screen_size;

	if (total_size == 0)
		total_size = info->fix.smem_len;

	if (p > total_size)
		return -EFBIG;

	if (count > total_size) {
		err = -EFBIG;
		count = total_size;
	}

	if (count + p > total_size) {
		if (!err)
			err = -ENOSPC;

		count = total_size - p;
	}

	buffer = kmalloc((count > PAGE_SIZE) ? PAGE_SIZE : count,
			 GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	dst = (u8 __iomem *) (info->screen_base + p);

	while (count) {
		c = (count > PAGE_SIZE) ? PAGE_SIZE : count;
		src = buffer;

		if (copy_from_user(src, buf, c)) {
			err = -EFAULT;
			break;
		}

		fb_memcpy_tofb(dst, src, c);
		dst += c;
		src += c;
		*ppos += c;
		buf += c;
		cnt += c;
		count -= c;
	}

	kfree(buffer);



	/* TODO: fix this */
	if (reg_read(fbdev, DC_ST) & DC_WORKING) {
		goto end;
	} else {
		reg_write(fbdev, DC_FRM_CFG_ADDR, fbdev->framedesc_phys[fbdev->framedesc_array_use[fbdev->current_frm_desc]]);
		fbdev->current_frm_desc = next_frm;
		ingenicfb_cmp_start(info);
	}

	/* is_lcd_en will always be 1. there is no chance to call ingenicfb_enable ....*/
	if(!fbdev->is_lcd_en)
		ingenicfb_enable(info);

end:
	return (cnt) ? cnt : err;
}

static struct fb_ops ingenicfb_ops = {
	.owner = THIS_MODULE,
	.fb_open = ingenicfb_open,
	.fb_release = ingenicfb_release,
	.fb_write = ingenicfb_write,
	.fb_check_var = ingenicfb_check_var,
	.fb_set_par = ingenicfb_set_par,
	.fb_setcolreg = ingenicfb_setcolreg,
	.fb_blank = ingenicfb_blank,
	.fb_pan_display = ingenicfb_pan_display,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_ioctl = ingenicfb_ioctl,
	.fb_mmap = ingenicfb_mmap,
};

static int dummy_open(struct fb_info *info, int user)
{
	return 0;
}
static int dummy_release(struct fb_info *info, int user)
{
	return 0;
}
static int dummy_set_par(struct fb_info *info)
{
	return 0;
}

static struct fb_ops ingenicfb_layerx_ops = {
	.owner = THIS_MODULE,
	.fb_open = dummy_open,
	.fb_release = dummy_release,
	.fb_check_var = ingenicfb_check_var,
	.fb_set_par = dummy_set_par,
	.fb_setcolreg = ingenicfb_setcolreg,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_mmap = ingenicfb_mmap,
};

	static ssize_t
dump_h_color_bar(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	ingenicfb_display_h_color_bar(fbdev->fb);
	ingenicfb_cmp_start(fbdev->fb);
	return 0;
}

	static ssize_t
dump_v_color_bar(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	ingenicfb_display_v_color_bar(fbdev->fb);
	ingenicfb_cmp_start(fbdev->fb);
	return 0;
}

	static ssize_t
vsync_skip_r(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	mutex_lock(&fbdev->lock);
	snprintf(buf, 3, "%d\n", fbdev->vsync_skip_ratio);
	printk("vsync_skip_map = 0x%08x\n", fbdev->vsync_skip_map);
	mutex_unlock(&fbdev->lock);
	return 3;		/* sizeof ("%d\n") */
}

static int vsync_skip_set(struct ingenicfb_device *fbdev, int vsync_skip)
{
	unsigned int map_wide10 = 0;
	int rate, i, p, n;
	int fake_float_1k;

	if (vsync_skip < 0 || vsync_skip > 9)
		return -EINVAL;

	rate = vsync_skip + 1;
	fake_float_1k = 10000 / rate;	/* 10.0 / rate */

	p = 1;
	n = (fake_float_1k * p + 500) / 1000;	/* +0.5 to int */

	for (i = 1; i <= 10; i++) {
		map_wide10 = map_wide10 << 1;
		if (i == n) {
			map_wide10++;
			p++;
			n = (fake_float_1k * p + 500) / 1000;
		}
	}
	mutex_lock(&fbdev->lock);
	fbdev->vsync_skip_map = map_wide10;
	fbdev->vsync_skip_ratio = rate - 1;	/* 0 ~ 9 */
	mutex_unlock(&fbdev->lock);

	printk("vsync_skip_ratio = %d\n", fbdev->vsync_skip_ratio);
	printk("vsync_skip_map = 0x%08x\n", fbdev->vsync_skip_map);

	return 0;
}

	static ssize_t
vsync_skip_w(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);

	if ((count != 1) && (count != 2))
		return -EINVAL;
	if ((*buf < '0') && (*buf > '9'))
		return -EINVAL;

	vsync_skip_set(fbdev, *buf - '0');

	return count;
}

static ssize_t fps_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	printk("\n-----you can choice print way:\n");
	printk("Example: echo NUM > show_fps\n");
	printk("NUM = 0: close fps statistics\n");
	printk("NUM = 1: print recently fps\n");
	printk("NUM = 2: print interval between last and this pan_display\n");
	printk("NUM = 3: print pan_display count\n\n");
	return 0;
}

static ssize_t fps_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	int num = 0;
	num = simple_strtoul(buf, NULL, 0);
	if(num < 0){
		printk("\n--please 'cat show_fps' to view using the method\n\n");
		return n;
	}
	showFPS = num;
	if(showFPS == 3)
		printk(KERN_DEBUG " Pand display count=%d\n",fbdev->pan_display_count);
	return n;
}


	static ssize_t
debug_clr_st(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	reg_write(fbdev, DC_CLR_ST, 0xffffffff);
	return 0;
}

static ssize_t test_suspend(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	int num = 0;
	num = simple_strtoul(buf, NULL, 0);
	printk("0 --> resume | 1 --> suspend\nnow input %d\n", num);
	if (num == 0) {
		ingenicfb_do_resume(fbdev);
	} else {
		ingenicfb_do_suspend(fbdev);
	}
	return n;
}

/*************************self test******************************/
static ssize_t test_slcd_send_value(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	int num = 0;
	num = simple_strtoul(buf, NULL, 0);
	if (num == 0) {
		slcd_send_mcu_command(fbdev, 0x0);
		slcd_send_mcu_command(fbdev, 0xffffff);
		slcd_send_mcu_command(fbdev, 0x0);
		slcd_send_mcu_command(fbdev, 0xffffff);
		slcd_send_mcu_command(fbdev, 0x0);
		slcd_send_mcu_command(fbdev, 0xffffff);
		slcd_send_mcu_command(fbdev, 0x0);
		slcd_send_mcu_command(fbdev, 0xffffff);
		slcd_send_mcu_command(fbdev, 0x0);
		slcd_send_mcu_command(fbdev, 0xffffff);
		printk("Send command value 101\n");
	} else if(num == 1){
		slcd_send_mcu_prm(fbdev, 0x0);
		slcd_send_mcu_prm(fbdev, 0xffffffff);
		slcd_send_mcu_prm(fbdev, 0x0);
		slcd_send_mcu_prm(fbdev, 0xffffffff);
		slcd_send_mcu_prm(fbdev, 0x0);
		slcd_send_mcu_prm(fbdev, 0xffffffff);
		slcd_send_mcu_prm(fbdev, 0x0);
		slcd_send_mcu_prm(fbdev, 0xffffffff);
		slcd_send_mcu_prm(fbdev, 0x0);
		slcd_send_mcu_prm(fbdev, 0xffffffff);
		slcd_send_mcu_prm(fbdev, 0x0);
		slcd_send_mcu_prm(fbdev, 0xffffffff);
		printk("Send prm value 0x101\n");
	} else {
		slcd_send_mcu_data(fbdev, 0x0);
		slcd_send_mcu_data(fbdev, 0xffffffff);
		slcd_send_mcu_data(fbdev, 0x0);
		slcd_send_mcu_data(fbdev, 0xffffffff);
		slcd_send_mcu_data(fbdev, 0x0);
		slcd_send_mcu_data(fbdev, 0xffffffff);
		slcd_send_mcu_data(fbdev, 0x0);
		slcd_send_mcu_data(fbdev, 0xffffffff);
		slcd_send_mcu_data(fbdev, 0x0);
		slcd_send_mcu_data(fbdev, 0xffffffff);
		slcd_send_mcu_data(fbdev, 0x0);
		slcd_send_mcu_data(fbdev, 0xffffffff);
		printk("Send data value 0x101\n");
	}
	return n;
}

static ssize_t test_fifo_threshold(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	int num = 0;
	unsigned int ctrl;
	unsigned int cfg = 0;
	num = simple_strtoul(buf, NULL, 0);
	if (num == 0) {
		ctrl = reg_read(fbdev, DC_PCFG_RD_CTRL);
		ctrl &=  ~DC_ARQOS_CTRL;
		reg_write(fbdev, DC_PCFG_RD_CTRL, ctrl);
		printk("Close software control %d\n", num);
	} else {
		ctrl = reg_read(fbdev, DC_PCFG_RD_CTRL);
		ctrl |=  DC_ARQOS_CTRL;
		ctrl &= ~DC_ARQOS_VAL_MASK;
		switch(num) {
		case 1:
			ctrl |= DC_ARQOS_VAL_0;
			break;
		case 2:
			ctrl |= DC_ARQOS_VAL_1;
			break;
		case 3:
			ctrl |= DC_ARQOS_VAL_2;
			break;
		case 4:
			ctrl |= DC_ARQOS_VAL_3;
			break;
		default:
			printk("Input err value %d\n", num);
			return n;
		}
		reg_write(fbdev, DC_PCFG_RD_CTRL, ctrl);
		cfg = (60 << DC_PCFG0_LBIT) | (120 << DC_PCFG1_LBIT) | (180 << DC_PCFG2_LBIT);
		reg_write(fbdev, DC_OFIFO_PCFG, cfg);
		printk("DC_OFIFO_PCFG = 0x%x  DC_PCFG_RD_CTRL = 0x%x\n", DC_OFIFO_PCFG, DC_PCFG_RD_CTRL);
	}
	return n;
}
static ssize_t test_slcd_time(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int num = 0;
	unsigned int type = 0;
	unsigned int value;
	num = simple_strtoul(buf, NULL, 0);

	type = (num >> 8) & 0xff;
	switch(type) {
	case 0:
		printk("\n0: print set ways\n");
		printk("1: CDTIME\n");
		printk("2: CSTIME\n");
		printk("3: DDTIME\n");
		printk("4: DSTIME\n");
		printk("5: TAS\n");
		printk("6: TAH\n");
		printk("7: TCS\n");
		printk("8: TCH\n");
		printk("9: Slow time\n\n");
		break;
	case 1:
		value = reg_read(fbdev, DC_SLCD_WR_DUTY);
		reg_write(fbdev, DC_SLCD_WR_DUTY, ((num & 0xff) << DC_CDTIME_LBIT) | (value & ~DC_CDTIME_MASK));
		break;
	case 2:
		value = reg_read(fbdev, DC_SLCD_WR_DUTY);
		reg_write(fbdev, DC_SLCD_WR_DUTY, ((num & 0xff) << DC_CSTIME_LBIT) | (value & ~DC_CSTIME_MASK));
		break;
	case 3:
		value = reg_read(fbdev, DC_SLCD_WR_DUTY);
		reg_write(fbdev, DC_SLCD_WR_DUTY, ((num & 0xff) << DC_DDTIME_LBIT) | (value & ~DC_DDTIME_MASK));
		break;
	case 4:
		value = reg_read(fbdev, DC_SLCD_WR_DUTY);
		reg_write(fbdev, DC_SLCD_WR_DUTY, ((num & 0xff) << DC_DSTIME_LBIT) | (value & ~DC_DSTIME_MASK));
		break;
	case 5:
		value = reg_read(fbdev, DC_SLCD_TIMING);
		reg_write(fbdev, DC_SLCD_TIMING, ((num & 0xff) << DC_TAS_LBIT) | (value & ~DC_TAS_MASK));
		break;
	case 6:
		value = reg_read(fbdev, DC_SLCD_TIMING);
		reg_write(fbdev, DC_SLCD_TIMING, ((num & 0xff) << DC_TAH_LBIT) | (value & ~DC_TAH_MASK));
		break;
	case 7:
		value = reg_read(fbdev, DC_SLCD_TIMING);
		reg_write(fbdev, DC_SLCD_TIMING, ((num & 0xff) << DC_TCS_LBIT) | (value & ~DC_TCS_MASK));
		break;
	case 8:
		value = reg_read(fbdev, DC_SLCD_TIMING);
		reg_write(fbdev, DC_SLCD_TIMING, ((num & 0xff) << DC_TCH_LBIT) | (value & ~DC_TCH_MASK));
		break;
	case 9:
		value = reg_read(fbdev, DC_SLCD_SLOW_TIME);
		reg_write(fbdev, DC_SLCD_SLOW_TIME, ((num & 0xff) << DC_SLOW_TIME_LBIT) | (value & ~DC_SLOW_TIME_MASK));
		break;
	default:
		printk("Input err value %d\n", num);
		return n;
	}
	printk("DC_SLCD_WR_DUTY = 0x%lx\n", reg_read(fbdev, DC_SLCD_WR_DUTY));
	printk("DC_SLCD_TIMING = 0x%lx\n", reg_read(fbdev, DC_SLCD_TIMING));
	printk("DC_SLCD_SLOW_TIME = 0x%lx\n", reg_read(fbdev, DC_SLCD_SLOW_TIME));
	return n;
}
static ssize_t show_irq_msg(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	char *p = buf;
	p += printk(p, "0 -> set: all irq\n");
	p += printk(p, "1 -> set: DC_CMP_W_SLOW\n");
	p += printk(p, "2 -> set: DC_DISP_END\n");
	p += printk(p, "3 -> set: DC_WDMA_OVER\n");
	p += printk(p, "4 -> set: DC_WDMA_END\n");
	p += printk(p, "5 -> set: DC_CMP_START\n");
	p += printk(p, "6 -> set: DC_LAY3_END\n");
	p += printk(p, "7 -> set: DC_LAY2_END\n");
	p += printk(p, "8 -> set: DC_LAY1_END\n");
	p += printk(p, "9 -> set: DC_LAY0_END\n");
	p += printk(p, "10 -> set: DC_CMP_END\n");
	p += printk(p, "11 -> set: DC_TFT_UNDR\n");
	p += printk(p, "12 -> set: DC_STOP_WRBK_ACK\n");
	p += printk(p, "13 -> set: DC_STOP_DISP_ACK\n");
	p += printk(p, "14 -> set: DC_SRD_START\n");
	p += printk(p, "15 -> set: DC_SRD_END\n");

	return p - buf;
}
static ssize_t set_irq_bit(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int num = 0;
	unsigned int irq;
	num = simple_strtoul(buf, NULL, 0);
	irq = reg_read(fbdev, DC_INTC);
	switch(num) {
	case 0:
		reg_write(fbdev, DC_INTC, 0x820f46);
		break;
	case 1:
		reg_write(fbdev, DC_INTC, DC_CMP_W_SLOW | irq);
		break;
	case 2:
		reg_write(fbdev, DC_INTC, DC_DISP_END | irq);
		break;
	case 3:
		reg_write(fbdev, DC_INTC, DC_WDMA_OVER | irq);
		break;
	case 4:
		reg_write(fbdev, DC_INTC, DC_WDMA_END | irq);
		break;
	case 5:
		reg_write(fbdev, DC_INTC, DC_CMP_START | irq);
		break;
	case 6:
		reg_write(fbdev, DC_INTC, DC_LAY3_END | irq);
		break;
	case 7:
		reg_write(fbdev, DC_INTC, DC_LAY2_END | irq);
		break;
	case 8:
		reg_write(fbdev, DC_INTC, DC_LAY1_END | irq);
		break;
	case 9:
		reg_write(fbdev, DC_INTC, DC_LAY0_END | irq);
		break;
	case 10:
		reg_write(fbdev, DC_INTC, DC_CMP_END | irq);
		break;
	case 11:
		reg_write(fbdev, DC_INTC, DC_TFT_UNDR | irq);
		break;
	case 12:
		reg_write(fbdev, DC_INTC, DC_STOP_WRBK_ACK | irq);
		break;
	case 13:
		reg_write(fbdev, DC_INTC, DC_STOP_DISP_ACK | irq);
		break;
	case 14:
		reg_write(fbdev, DC_INTC, DC_SRD_START | irq);
		break;
	case 15:
		reg_write(fbdev, DC_INTC, DC_SRD_END | irq);
		break;
	default:
		printk("Err: Please check num = %d", num);
		reg_write(fbdev, DC_INTC, DC_CMP_START);
		break;
	}

	return n;
}

#ifdef CONFIG_FB_INGENIC_MIPI_DSI
static ssize_t
dump_dsi(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	struct dsi_device *dsi = fbdev->dsi;

	mutex_lock(&fbdev->lock);
	dump_dsi_reg(dsi);
	mutex_unlock(&fbdev->lock);
	return 0;
}

static ssize_t
dsi_query_te(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	struct dsi_device *dsi = fbdev->dsi;

	mutex_lock(&fbdev->lock);
	fbdev->dsi->master_ops->query_te(dsi);
	mutex_unlock(&fbdev->lock);
	return 0;
}
#endif


static ssize_t
test_fb_disable(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);

	ingenicfb_disable(fbdev->fb, GEN_STOP);
	return 0;
}

static ssize_t
test_fb_enable(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);

	ingenicfb_enable(fbdev->fb);
	return 0;
}

static ssize_t show_color_modes(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	struct ingenicfb_colormode *current_mode = NULL;
	char *p = buf;
	int i = 0;

	p += sprintf(p, "supported color modes:\n");
	for(i = 0; i < ARRAY_SIZE(ingenicfb_colormodes); i++) {
		struct ingenicfb_colormode * m = &ingenicfb_colormodes[i];
		p += sprintf(p, "[%d]:%s\n", i, m->name);

		if(m->mode == fbdev->current_frm_mode.frm_cfg.lay_cfg[0].format) {
			current_mode = m;
		}
	}
	p += sprintf(p, "Current color mode: [%s]\n", current_mode ? current_mode->name:"none");
	p += sprintf(p, "Tips: echo [%s] to select one of supported color modes\n", current_mode ? current_mode->name:"none");

	return p - buf;

}

static ssize_t store_color_modes(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	struct fb_info *fb = fbdev->fb;
	struct ingenicfb_colormode *m = NULL;
	int index = 0;
	int ret = 0;

	index = simple_strtol(buf, NULL, 10);

	if(index < 0 && index > ARRAY_SIZE(ingenicfb_colormodes))
		return -EINVAL;

	m = &ingenicfb_colormodes[index];

	/* modify fb var. */
	ingenicfb_colormode_to_var(&fb->var, m);

	/* reset params. */
	ret = ingenicfb_set_par(fbdev->fb);
	if(ret < 0)
		return ret;

	return n;
}
static ssize_t show_wback_en(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	char *p = buf;

	p += sprintf(p, "help: write [1/0] to [en/dis] wback\n");
	p += sprintf(p, "wback_en: %d\n", fbdev->wback_en);

	return p - buf;
}

static ssize_t store_wback_en(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int en = 0;
	int ret = 0;

	en = simple_strtol(buf, NULL, 10);

	fbdev->wback_en = !!en;

	/* reset params. */
	ret = ingenicfb_set_par(fbdev->fb);
	if(ret < 0)
		return ret;

	return n;
}

static ssize_t show_csc_mode(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	char *p = buf;

	p += sprintf(p, "current csc_mode: %d\n", fbdev->csc_mode);

	return p - buf;
}

static ssize_t store_csc_mode(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int csc_mode = 0;
	int ret = 0;

	csc_mode = simple_strtol(buf, NULL, 10);

	if(csc_mode < 0 || csc_mode > 3) {
		return -EINVAL;
	}

	csc_mode_set(fbdev, csc_mode);
	fbdev->csc_mode = csc_mode;

	/* reset params. */
	ret = ingenicfb_set_par(fbdev->fb);
	if(ret < 0)
		return ret;

	return n;
}

static ssize_t show_wbackbuf(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);

	char *wbackbuf = fbdev->sread_vidmem[0];	/* TODO: where should wback addr is?*/
	unsigned int wbackbuf_len = fbdev->frm_size;
	int copy_len = wbackbuf_len < 1024 ? wbackbuf_len : 1024; /* only copy first 1024 bytes for quik debug.*/

	if(fbdev->wback_en == 0) {
		return -EINVAL;
	}

	print_hex_dump(KERN_INFO, "wback data: ", DUMP_PREFIX_ADDRESS, 16, 1, wbackbuf, 128, true);

	memcpy(buf, wbackbuf, copy_len);

	return copy_len;
}

/*************************self test******************************/

/**********************lcd_debug***************************/
static DEVICE_ATTR(dump_lcd, S_IRUGO|S_IWUSR, dump_lcd, NULL);
static DEVICE_ATTR(dump_h_color_bar, S_IRUGO|S_IWUSR, dump_h_color_bar, NULL);
static DEVICE_ATTR(dump_v_color_bar, S_IRUGO|S_IWUSR, dump_v_color_bar, NULL);
static DEVICE_ATTR(vsync_skip, S_IRUGO|S_IWUSR, vsync_skip_r, vsync_skip_w);
static DEVICE_ATTR(show_fps, S_IRUGO|S_IWUSR, fps_show, fps_store);
static DEVICE_ATTR(debug_clr_st, S_IRUGO|S_IWUSR, debug_clr_st, NULL);
static DEVICE_ATTR(test_suspend, S_IRUGO|S_IWUSR, NULL, test_suspend);

#ifdef CONFIG_DEBUG_DPU_IRQCNT
static DEVICE_ATTR(dump_irqcnts, (S_IRUGO|S_IWUGO) & (~S_IWOTH), dump_irqcnts, NULL);
#endif

#ifdef CONFIG_FB_INGENIC_MIPI_DSI
static DEVICE_ATTR(dump_dsi, S_IRUGO|S_IWUSR, dump_dsi, NULL);
static DEVICE_ATTR(dsi_query_te, S_IRUGO|S_IWUSR, dsi_query_te, NULL);
#endif
static DEVICE_ATTR(test_fb_disable, (S_IRUGO|S_IWUGO)&(~S_IWOTH), test_fb_disable, NULL);
static DEVICE_ATTR(test_fb_enable, (S_IRUGO|S_IWUGO)&(~S_IWOTH) , test_fb_enable, NULL);

static DEVICE_ATTR(test_irq, S_IRUGO|S_IWUSR, show_irq_msg, set_irq_bit);
static DEVICE_ATTR(test_fifo_threshold, S_IRUGO|S_IWUSR, NULL, test_fifo_threshold);
static DEVICE_ATTR(test_slcd_time, S_IRUGO|S_IWUSR, NULL, test_slcd_time);
static DEVICE_ATTR(test_slcd_send_value, S_IRUGO|S_IWUSR, NULL, test_slcd_send_value);
static DEVICE_ATTR(color_modes, S_IRUGO|S_IWUSR, show_color_modes, store_color_modes);
static DEVICE_ATTR(wback_en, S_IRUGO|S_IWUSR, show_wback_en, store_wback_en);
static DEVICE_ATTR(wbackbuf, S_IRUGO|S_IWUSR, show_wbackbuf, NULL);
static DEVICE_ATTR(csc_mode, S_IRUGO|S_IWUSR, show_csc_mode, store_csc_mode);


static struct attribute *lcd_debug_attrs[] = {
	&dev_attr_dump_lcd.attr,
	&dev_attr_dump_h_color_bar.attr,
	&dev_attr_dump_v_color_bar.attr,
	&dev_attr_vsync_skip.attr,
	&dev_attr_show_fps.attr,
	&dev_attr_debug_clr_st.attr,
	&dev_attr_test_suspend.attr,

#ifdef CONFIG_DEBUG_DPU_IRQCNT
	&dev_attr_dump_irqcnts.attr,
#endif

#ifdef CONFIG_FB_INGENIC_MIPI_DSI
	&dev_attr_dump_dsi.attr,
	&dev_attr_dsi_query_te.attr,
#endif
	&dev_attr_test_fb_disable.attr,
	&dev_attr_test_fb_enable.attr,
	&dev_attr_test_irq.attr,
	&dev_attr_test_fifo_threshold.attr,
	&dev_attr_test_slcd_time.attr,
	&dev_attr_test_slcd_send_value.attr,
	&dev_attr_color_modes.attr,
	&dev_attr_wback_en.attr,
	&dev_attr_wbackbuf.attr,
	&dev_attr_csc_mode.attr,
	NULL,
};

const char lcd_group_name[] = "debug";
static struct attribute_group lcd_debug_attr_group = {
	.name	= lcd_group_name,
	.attrs	= lcd_debug_attrs,
};

struct layer_device_attr {
	struct device_attribute attr;
	unsigned int id;
};

#define to_layer_attr(attr)	\
	container_of(attr, struct layer_device_attr, attr)


static ssize_t show_src_size(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int layer = to_layer_attr(attr)->id;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	char *p = buf;

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}
	lay_cfg = &frm_cfg->lay_cfg[layer];

	p += sprintf(p, "layer: %d, src_w: %d, src_h: %d\n", layer, lay_cfg->source_w, lay_cfg->source_h);

	return p - buf;
}
static ssize_t store_src_size(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{

	return n;
}
static ssize_t show_src_fmt(struct device *dev,
		struct device_attribute *attr, char *buf)
{

	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	struct ingenicfb_colormode *current_mode = NULL;
	int layer = to_layer_attr(attr)->id;
	char *p = buf;
	int i = 0;

	p += sprintf(p, "supported color modes:\n");
	for(i = 0; i < ARRAY_SIZE(ingenicfb_colormodes); i++) {
		struct ingenicfb_colormode * m = &ingenicfb_colormodes[i];
		p += sprintf(p, "[%d]:%s\n", i, m->name);

		if(m->mode == fbdev->current_frm_mode.frm_cfg.lay_cfg[layer].format) {
			current_mode = m;
		}
	}
	p += sprintf(p, "Current color mode: [%s]\n", current_mode ? current_mode->name:"none");
	p += sprintf(p, "Tips: echo [%s] to select one of supported color modes\n", current_mode ? current_mode->name:"none");

	return p - buf;
}
static ssize_t store_src_fmt(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	struct fb_info *fb = fbdev->fb;
	struct ingenicfb_colormode *m = NULL;
	int index = 0;
	int layer = to_layer_attr(attr)->id;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;

	index = simple_strtol(buf, NULL, 10);

	if(index < 0 && index > ARRAY_SIZE(ingenicfb_colormodes))
		return -EINVAL;

	m = &ingenicfb_colormodes[index];

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}

	lay_cfg = &frm_cfg->lay_cfg[layer];
	lay_cfg->format = m->mode;

	/*update fb_info.*/
	fb = fbdev->fbs[layer];
	ingenicfb_colormode_to_var(&fb->var, m);
	fb->mode = ingenicfb_get_mode(&fb->var, fb);

	return n;

}
static ssize_t show_target_size(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int layer = to_layer_attr(attr)->id;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	char *p = buf;

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}

	lay_cfg = &frm_cfg->lay_cfg[layer];

	p += sprintf(p, "%dx%d\n", lay_cfg->scale_w, lay_cfg->scale_h);

	return p - buf;
}
static ssize_t store_target_size(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int layer = to_layer_attr(attr)->id;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	char *p;
	char *s = (char *)buf;

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}

	lay_cfg = &frm_cfg->lay_cfg[layer];

	p = strsep(&s, "x");
	if(!s) {
		return -EINVAL;
	}

	lay_cfg->scale_w = simple_strtoul(p, NULL, 0);
	lay_cfg->scale_h = simple_strtoul(s, NULL, 0);


	printk("scale_w: %d, scale_h: %d\n", lay_cfg->scale_w, lay_cfg->scale_h);

	return n;
}
static ssize_t show_target_pos(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int layer = to_layer_attr(attr)->id;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	char *p = buf;

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}

	lay_cfg = &frm_cfg->lay_cfg[layer];

	p += sprintf(p, "layer:%d, pos_x: %d, pos_y:%d\n", layer, lay_cfg->disp_pos_x, lay_cfg->disp_pos_y);

	return p - buf;
}
static ssize_t store_target_pos(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int layer = to_layer_attr(attr)->id;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	char *p;
	char *s = (char *)buf;

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}

	lay_cfg = &frm_cfg->lay_cfg[layer];

	p = strsep(&s, "x");
	if(!s) {
		return -EINVAL;
	}

	lay_cfg->disp_pos_x = simple_strtoul(p, NULL, 0);
	lay_cfg->disp_pos_y = simple_strtoul(s, NULL, 0);


	printk("-pos_x: %d, pos_y: %d\n", lay_cfg->disp_pos_x, lay_cfg->disp_pos_y);

	return n;
}

static ssize_t show_enable(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	int layer = to_layer_attr(attr)->id;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	char *p = buf;

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}

	lay_cfg = &frm_cfg->lay_cfg[layer];

	p += sprintf(p, "layer: %d, enable: %d\n", layer, lay_cfg->lay_en);

	return p - buf;
}
static ssize_t store_enable(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct ingenicfb_device *fbdev = dev_get_drvdata(dev);
	struct fb_info *fb = fbdev->fb;
	struct ingenicfb_frm_cfg *frm_cfg = &fbdev->current_frm_mode.frm_cfg;
	struct ingenicfb_lay_cfg *lay_cfg;
	int layer = to_layer_attr(attr)->id;
	int enable = 0;

	if(layer > CONFIG_FB_INGENIC_NR_LAYERS) {
		return -EINVAL;
	}

	lay_cfg = &frm_cfg->lay_cfg[layer];

	if(lay_cfg->scale_w == 0 || lay_cfg->scale_h == 0) {
		return -EINVAL;
	}

	enable = simple_strtoul(buf, NULL, 0);
	if(enable) {
		lay_cfg->lay_en = 1;
		if(lay_cfg->scale_w != lay_cfg->source_w || lay_cfg->scale_h != lay_cfg->source_h) {
			lay_cfg->lay_scale_en = 1;
		} else {
			lay_cfg->lay_scale_en = 0;
		}
	} else {
		lay_cfg->lay_en = 0;
		lay_cfg->lay_scale_en = 0;
	}



#if 0
	printk("layer: %d src_w: %d, src_h: %d, scale_w: %d, scale_h: %d, pos_x:%d, pos_y:%d\n",
		layer, lay_cfg->source_w, lay_cfg->source_h, lay_cfg->scale_w, lay_cfg->scale_h, lay_cfg->disp_pos_x, lay_cfg->disp_pos_y);
#endif

	/*update all layers?*/
	if(layer == 0) {
		ingenicfb_disable(fbdev->fb, GEN_STOP);
		ingenicfb_set_par(fb);
		ingenicfb_enable(fbdev->fb);
	}
	return n;
}


#define LAYER_ATTR(layer, _name, _mode, _show, _store)		\
	{							\
	.attr 	= __ATTR(_name, _mode, _show, _store),	\
	.id 	= layer,						\
	}


#define LAYER_DEVICE_ATTR(_name, _mode, _show, _store)							  \
	static struct layer_device_attr dev_attr_##_name##layer0 = LAYER_ATTR(0, _name, _mode, _show, _store); \
	static struct layer_device_attr dev_attr_##_name##layer1 = LAYER_ATTR(1, _name, _mode, _show, _store); \
	static struct layer_device_attr dev_attr_##_name##layer2 = LAYER_ATTR(2, _name, _mode, _show, _store); \
	static struct layer_device_attr dev_attr_##_name##layer3 = LAYER_ATTR(3, _name, _mode, _show, _store)

LAYER_DEVICE_ATTR(src_size, S_IRUGO|S_IWUSR, show_src_size, store_src_size);
LAYER_DEVICE_ATTR(src_fmt, S_IRUGO|S_IWUSR, show_src_fmt, store_src_fmt);
LAYER_DEVICE_ATTR(target_size, S_IRUGO|S_IWUSR, show_target_size, store_target_size);
LAYER_DEVICE_ATTR(target_pos, S_IRUGO|S_IWUSR, show_target_pos, store_target_pos);
LAYER_DEVICE_ATTR(enable, S_IRUGO|S_IWUSR, show_enable, store_enable);

#define LAYER_ATTRIBUTE_GROUP(name)					\
	static struct attribute *lcd_##name##_attrs[] = {		\
		&dev_attr_src_size##name.attr.attr,			\
		&dev_attr_src_fmt##name.attr.attr,                      \
		&dev_attr_target_size##name.attr.attr,                  \
		&dev_attr_target_pos##name.attr.attr,                   \
		&dev_attr_enable##name.attr.attr,                       \
		NULL,							\
	};

LAYER_ATTRIBUTE_GROUP(layer0);
LAYER_ATTRIBUTE_GROUP(layer1);
LAYER_ATTRIBUTE_GROUP(layer2);
LAYER_ATTRIBUTE_GROUP(layer3);

static struct attribute_group lcd_layer0_group = {
		.name = "layer0",
		.attrs = lcd_layer0_attrs,
};
static struct attribute_group lcd_layer1_group = {
		.name = "layer1",
		.attrs = lcd_layer1_attrs,
};
static struct attribute_group lcd_layer2_group = {
		.name = "layer2",
		.attrs = lcd_layer2_attrs,
};
static struct attribute_group lcd_layer3_group = {
		.name = "layer3",
		.attrs = lcd_layer3_attrs,
};

static const struct attribute_group *lcd_layerx_groups[] = {
	&lcd_layer0_group,
	&lcd_layer1_group,
	&lcd_layer2_group,
	&lcd_layer3_group,
	NULL,
};

static void ingenicfb_free_devmem(struct ingenicfb_device *fbdev)
{
	size_t buff_size;

	dma_free_coherent(fbdev->dev,
			  fbdev->vidmem_size + fbdev->sread_vidmem_size,
			  fbdev->vidmem[0][0],
			  fbdev->vidmem_phys[0][0]);

	buff_size = sizeof(struct ingenicfb_layerdesc);
	buff_size = ALIGN(buff_size, DESC_ALIGN);
	dma_free_coherent(fbdev->dev,
			  buff_size * CONFIG_FB_INGENIC_NR_FRAMES * CONFIG_FB_INGENIC_NR_LAYERS * 2,
			  fbdev->layerdesc[0],
			  fbdev->layerdesc_phys[0][0]);

	buff_size = sizeof(struct ingenicfb_framedesc);
	buff_size = ALIGN(buff_size, DESC_ALIGN);
	dma_free_coherent(fbdev->dev,
			  buff_size * CONFIG_FB_INGENIC_NR_FRAMES * 2,
			  fbdev->framedesc[0],
			  fbdev->framedesc_phys[0]);
}

static int ingenicfb_copy_logo(struct fb_info *info)
{
	unsigned long src_addr = 0;	/* u-boot logo buffer address */
	unsigned long dst_addr = 0;	/* kernel frame buffer address */
	struct ingenicfb_device *fbdev = info->par;
	unsigned long size;
	unsigned int ctrl;
	unsigned read_times;
	lay_cfg_en_t lay_cfg_en;

	/* Sure the uboot SLCD using the continuous mode, Close irq */
	if (!(reg_read(fbdev, DC_ST) & DC_WORKING)) {
		dev_err(fbdev->dev, "uboot is not display logo!\n");
#ifdef CONFIG_TRUE_COLOR_LOGO
        fbdev->current_frm_desc = 0;
        ingenicfb_set_fix_par(fbdev->fb);
        ingenicfb_set_par(fbdev->fb);
        ingenicfb_enable(fbdev->fb);
        show_logo(fbdev->fb);
        fb_blank(fbdev->fb, FB_BLANK_UNBLANK);
#endif
		return -ENOEXEC;
	}

	/*fbdev->is_lcd_en = 1;*/

	/* Reading Desc from regisger need reset */
	ctrl = reg_read(fbdev, DC_CTRL);
	ctrl |= DC_DES_CNT_RST;
	reg_write(fbdev, DC_CTRL, ctrl);

	/* For geting LayCfgEn need read  DC_FRM_DES 10 times */
	read_times = 10 - 1;
	while(read_times--) {
		reg_read(fbdev, DC_FRM_DES);
	}
	lay_cfg_en.d32 = reg_read(fbdev, DC_FRM_DES);
	if(!lay_cfg_en.b.lay0_en) {
		dev_err(fbdev->dev, "Uboot initialization is not using layer0!\n");
		return -ENOEXEC;
	}

	/* For geting LayerBufferAddr need read  DC_LAY0_DES 3 times */
	read_times = 3 - 1;
	/* get buffer physical address */
	while(read_times--) {
		reg_read(fbdev, DC_LAY0_DES);
	}
	src_addr = (unsigned long)reg_read(fbdev, DC_LAY0_DES);

	if (src_addr) {
		size = info->fix.line_length * info->var.yres;
		src_addr = (unsigned long)phys_to_virt(src_addr);
		dst_addr = (unsigned long)fbdev->vidmem[0][0];
		memcpy((void *)dst_addr, (void *)src_addr, size);
	}

	return 0;
}

static int ingenicfb_do_probe(struct platform_device *pdev, struct lcd_panel *panel)
{
	struct fb_videomode *video_mode;
	struct fb_info *fb;
	struct fb_info *fb_layer;
	int ret = 0;
	int i;

	fb = framebuffer_alloc(sizeof(struct ingenicfb_device), &pdev->dev);
	if (!fb) {
		dev_err(&pdev->dev, "Failed to allocate framebuffer device\n");
		return -ENOMEM;
	}

	fbdev = fb->par;
	fbdev->fb = fb;
	fbdev->dev = &pdev->dev;
	fbdev->panel = panel;
#ifdef CONFIG_SLCDC_CONTINUA
	if(fbdev->panel->lcd_type == LCD_TYPE_SLCD)
		fbdev->slcd_continua = 1;
#endif


	spin_lock_init(&fbdev->irq_lock);
	mutex_init(&fbdev->lock);
	mutex_init(&fbdev->suspend_lock);
	sprintf(fbdev->clk_name, "gate_lcd");
	sprintf(fbdev->pclk_name, "div_lcd");

	fbdev->clk = devm_clk_get(&pdev->dev, fbdev->clk_name);
	fbdev->pclk = devm_clk_get(&pdev->dev, fbdev->pclk_name);

	if (IS_ERR(fbdev->clk) || IS_ERR(fbdev->pclk)) {
		ret = PTR_ERR(fbdev->clk);
		dev_err(&pdev->dev, "Failed to get lcdc clock: %d\n", ret);
		goto err_framebuffer_release;
	}

	fbdev->base = of_iomap(pdev->dev.of_node, 0);
	if (!fbdev->base) {
		dev_err(&pdev->dev,
				"Failed to ioremap register memory region\n");
		ret = -EBUSY;
		goto err_put_clk;
	}

	ret = refresh_pixclock_auto_adapt(fb);
	if(ret){
		goto err_iounmap;
	}

	video_mode = fbdev->panel->modes;
	if (!video_mode) {
		ret = -ENXIO;
		goto err_iounmap;
	}

	fb_videomode_to_modelist(panel->modes, panel->num_modes, &fb->modelist);

	ingenicfb_videomode_to_var(&fb->var, video_mode, fbdev->panel->lcd_type);
	fb->fbops = &ingenicfb_ops;
	fb->flags = FBINFO_DEFAULT;
	fb->var.width = panel->width;
	fb->var.height = panel->height;

	ingenicfb_colormode_to_var(&fb->var, &ingenicfb_colormodes[0]);

	ret = ingenicfb_check_var(&fb->var, fb);
	if (ret) {
		goto err_iounmap;
	}

	/*
	 * #BUG: if uboot pixclock is different from kernel. this may cause problem.
	 *
	 **/
	ingenic_set_pixclk(fb);

	clk_prepare_enable(fbdev->clk);
	clk_prepare_enable(fbdev->pclk);
	fbdev->is_clk_en = 1;

	ret = ingenicfb_alloc_devmem(fbdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to allocate video memory\n");
		goto err_iounmap;
	}

	fb->fix = ingenicfb_fix;
	fb->fix.line_length = (fb->var.bits_per_pixel * fb->var.xres) >> 3;
	fb->fix.smem_start = fbdev->vidmem_phys[0][0];
	fb->fix.smem_len = fbdev->vidmem_size + fbdev->sread_vidmem_size;
	fb->screen_size = fbdev->frm_size;
	fb->screen_base = fbdev->vidmem[0][0];
	fb->pseudo_palette = fbdev->pseudo_palette;

	vsync_skip_set(fbdev, CONFIG_FB_VSYNC_SKIP);
	init_waitqueue_head(&fbdev->vsync_wq);
	init_waitqueue_head(&fbdev->gen_stop_wq);
	fbdev->open_cnt = 0;
	fbdev->is_lcd_en = 0;
	fbdev->timestamp.rp = 0;
	fbdev->timestamp.wp = 0;

	fbdev->csc_mode = CSC_MODE_1;
	csc_mode_set(fbdev, fbdev->csc_mode);
	ingenicfb_update_frm_mode(fbdev->fb);

	fbdev->irq = platform_get_irq(pdev, 0);
	sprintf(fbdev->irq_name, "lcdc%d", pdev->id);
	if (devm_request_irq(fbdev->dev, fbdev->irq, ingenicfb_irq_handler, 0,
				fbdev->irq_name, fbdev)) {
		dev_err(&pdev->dev, "request irq failed\n");
		ret = -EINVAL;
		goto err_free_devmem;
	}

#ifdef CONFIG_FB_INGENIC_MIPI_DSI
	/*BUG**/
	fbdev->dsi = jzdsi_init(panel->dsi_pdata);
	if (!fbdev->dsi) {
		goto err_iounmap;
	}
#endif

	platform_set_drvdata(pdev, fbdev);

	ret = sysfs_create_group(&fbdev->dev->kobj, &lcd_debug_attr_group);
	if (ret) {
		dev_err(&pdev->dev, "device create sysfs group failed\n");

		ret = -EINVAL;
		goto err_free_irq;
	}

	ret = register_framebuffer(fb);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register framebuffer: %d\n",
				ret);
		goto err_free_file;
	}

	if (fbdev->vidmem_phys) {
		if (!ingenicfb_copy_logo(fbdev->fb)) {
			fbdev->is_lcd_en = 1;
			ret = ingenicfb_desc_init(fb, 0);
			if(ret) {
				dev_err(fbdev->dev, "Desc init err!\n");
				goto err_free_file;
			}
			reg_write(fbdev, DC_FRM_CFG_ADDR, fbdev->framedesc_phys[fbdev->framedesc_array_use[fbdev->current_frm_desc]]);
		}
#ifdef CONFIG_FB_INGENIC_DEBUG
		test_pattern(fbdev);
#endif
	}else{

		ingenicfb_clk_disable(fbdev);
		ret = -ENOMEM;
		goto err_free_file;
	}

	/* From Layer1 to NR_LAYERS. */
	for(i = 1; i < CONFIG_FB_INGENIC_NR_LAYERS; i++) {

		fbdev->fbs[i] = framebuffer_alloc(sizeof(struct ingenicfb_device), &pdev->dev);
		if(fbdev->fbs[i] == NULL) {
			dev_err(&pdev->dev, "Failed to alloc framebuffer for layer: %d\n", i);
			return -ENOMEM;
		}
		fbdev->fbs[i]->par = fbdev;

		fb_layer = fbdev->fbs[i];

		fb_videomode_to_modelist(panel->modes, panel->num_modes, &fb_layer->modelist);
		ingenicfb_videomode_to_var(&fb_layer->var, video_mode, fbdev->panel->lcd_type);
		fb_layer->fbops = &ingenicfb_layerx_ops;
		fb_layer->flags = FBINFO_DEFAULT;
		fb_layer->var.width = panel->width;
		fb_layer->var.height = panel->height;

		ingenicfb_colormode_to_var(&fb_layer->var, &ingenicfb_colormodes[0]);
		fb_layer->fix = ingenicfb_fix;
		fb_layer->fix.line_length = (fb_layer->var.bits_per_pixel * fb_layer->var.xres) >> 3;
		fb_layer->fix.smem_start = fbdev->vidmem_phys[i][0];
		fb_layer->fix.smem_len = fbdev->vidmem_size / CONFIG_FB_INGENIC_NR_FRAMES;
		fb_layer->screen_size = fbdev->frm_size;
		fb_layer->screen_base = fbdev->vidmem[i][0];
		fb_layer->pseudo_palette = fbdev->pseudo_palette;
		ret = register_framebuffer(fb_layer);
		if(ret) {
			dev_err(&pdev->dev, "Failed to register framebuffer layer : %d\n", i);
		}
	}
	fbdev->fbs[0] = fb;

	ret = sysfs_create_groups(&fbdev->dev->kobj, lcd_layerx_groups);
	if(ret) {
		dev_err(&pdev->dev, "Failed to create sysfs groups\n");
	}


	return 0;

err_free_file:
	sysfs_remove_group(&fbdev->dev->kobj, &lcd_debug_attr_group);
err_free_irq:
	free_irq(fbdev->irq, fbdev);
err_free_devmem:
	ingenicfb_free_devmem(fbdev);
err_iounmap:
	iounmap(fbdev->base);
err_put_clk:

   if (fbdev->clk)
	clk_put(fbdev->clk);
   if (fbdev->pclk)
	clk_put(fbdev->pclk);

err_framebuffer_release:
	framebuffer_release(fb);
	return ret;
}

int ingenicfb_register_panel(struct lcd_panel *panel)
{
	WARN_ON(fbdev_panel != NULL);

	fbdev_panel = panel;

	if(fbdev_pdev != NULL) {
		return ingenicfb_do_probe(fbdev_pdev, fbdev_panel);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ingenicfb_register_panel);

static int ingenicfb_probe(struct platform_device *pdev)
{
	WARN_ON(fbdev_pdev != NULL);

	fbdev_pdev = pdev;

	if(fbdev_panel != NULL) {
		return ingenicfb_do_probe(fbdev_pdev, fbdev_panel);
	}

	return 0;
}

static int ingenicfb_remove(struct platform_device *pdev)
{
	struct ingenicfb_device *fbdev = platform_get_drvdata(pdev);

	ingenicfb_free_devmem(fbdev);
	platform_set_drvdata(pdev, NULL);

#ifdef CONFIG_FB_INGENIC_MIPI_DSI
	jzdsi_remove(fbdev->dsi);
#endif

	devm_clk_put(fbdev->dev, fbdev->pclk);
	devm_clk_put(fbdev->dev, fbdev->clk);

	sysfs_remove_group(&fbdev->dev->kobj, &lcd_debug_attr_group);

	iounmap(fbdev->base);

	framebuffer_release(fbdev->fb);

	return 0;
}

static void ingenicfb_shutdown(struct platform_device *pdev)
{
	struct ingenicfb_device *fbdev = platform_get_drvdata(pdev);
	int is_fb_blank;
	mutex_lock(&fbdev->suspend_lock);
	is_fb_blank = (fbdev->is_suspend != 1);
	fbdev->is_suspend = 1;
	mutex_unlock(&fbdev->suspend_lock);
	if (is_fb_blank)
		fb_blank(fbdev->fb, FB_BLANK_POWERDOWN);
};

#ifdef CONFIG_PM

static int ingenicfb_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ingenicfb_device *fbdev = platform_get_drvdata(pdev);

	fb_blank(fbdev->fb, FB_BLANK_POWERDOWN);

	return 0;
}

static int ingenicfb_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ingenicfb_device *fbdev = platform_get_drvdata(pdev);

	fb_blank(fbdev->fb, FB_BLANK_UNBLANK);

	return 0;
}

static const struct dev_pm_ops ingenicfb_pm_ops = {
	.suspend = ingenicfb_suspend,
	.resume = ingenicfb_resume,
};
#endif
static const struct of_device_id ingenicfb_of_match[] = {
	{ .compatible = "ingenic,x2000-dpu"},
	{ .compatible = "ingenic,t40-dpu"},
	{ .compatible = "ingenic,m300-dpu"},
	{},
};

static struct platform_driver ingenicfb_driver = {
	.probe = ingenicfb_probe,
	.remove = ingenicfb_remove,
	.shutdown = ingenicfb_shutdown,
	.driver = {
		.name = "ingenic-fb",
		.of_match_table = ingenicfb_of_match,
#ifdef CONFIG_PM
		.pm = &ingenicfb_pm_ops,
#endif

	},
};

static int __init ingenicfb_init(void)
{
	platform_driver_register(&ingenicfb_driver);
	return 0;
}

static void __exit ingenicfb_cleanup(void)
{
	platform_driver_unregister(&ingenicfb_driver);
}


#ifdef CONFIG_EARLY_INIT_RUN
rootfs_initcall(ingenicfb_init);
#else
module_init(ingenicfb_init);
#endif

module_exit(ingenicfb_cleanup);

MODULE_DESCRIPTION("JZ LCD Controller driver");
MODULE_AUTHOR("Sean Tang <ctang@ingenic.cn>");
MODULE_LICENSE("GPL");
