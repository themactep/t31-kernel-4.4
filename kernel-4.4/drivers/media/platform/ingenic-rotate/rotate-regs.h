/* * drivers/media/platform/ingenic_rotate/rotate-regs.h
 *
 * Copyright (C) 2016 Ingenic Semiconductor Inc.
 *
 * Author:clwang<chunlei.wang@ingenic.com>
 *
 * This program is free software, you can redistribute it and/or modify it
 *
 * under the terms of the GNU General Public License version 2 as published by
 *
 * the Free Software Foundation.
 */

#ifndef __ROTATER_REG_H__
#define __ROTATER_REG_H__

/*-------------------------------------------------------------------------------
 *			      Rotater Register Offset
 * -----------------------------------------------------------------------------*/


/* RW	32	0x0000_0000	frame descriptor's addres */
#define	ROT_FRM_CFG_ADDR		        (0x0000)
/* RW	32	0x0000_0000	frame's size */
#define	ROT_FRM_SIZE				(0x0004)
/* RW	32	0x0000_0000	global config */
#define	ROT_GLB_CFG				(0x0008)
/* -W	32	0x0000_0000	rotate control */
#define	ROT_CTRL			        (0x000c)
/* R-	32	0x0000_0000	rotate status */
#define	ROT_ST				        (0x0010)
/* R-	32	0x0000_0000	rotarer clear status */
#define	ROT_CLR_ST			        (0x0014)
/* -W	32	0x0000_0000	rotator interrupt mask */
#define	ROT_INT_MASK				(0x0018)
/* RW	32	0x0000_0000	RDMA`s current site */
#define	ROT_RDMA_SITE			        (0x0020)
/* RW	32	0x0000_0000	WDMA`s current site */
#define	ROT_WDMA_SITE			        (0x0024)
/* R-	32	0x0000_0000	Read the frame configure */
#define	ROT_DS_FRM_DES				(0x0028)
/* RW	32	0x0000_0000	Rotator QOS config */
#define	ROT_QOS					(0x0030)


/*-------------------------------------------------------------------------------
 *			DPU Registers Bits Field Define
 * -----------------------------------------------------------------------------*/


/* frame's size(ROT_FRM_SIZE) bit field define */

/* Frame's width */
#define ROT_FRM_WIDTH_LBIT			(0)
#define ROT_FRM_WIDTH_HBIT			(10)
#define ROT_FRM_WIDTH_MASK	      \
	GENMASK(ROT_FRM_WIDTH_HBIT, ROT_FRM_WIDTH_LBIT)
/* Frame's height */
#define ROT_FRM_HEIGHT_LBIT			(16)
#define ROT_FRM_HEIGHT_HBIT			(26)
#define ROT_FRM_HEIGHT_MASK	      \
	GENMASK(ROT_FRM_HEIGHT_HBIT, ROT_FRM_HEIGHT_LBIT)

/* global config(ROT_GLB_CFG) bit field define */

/* RDMA max length of the block DMA's burst. */
#define ROT_RDMA_BURST_LEN_LBIT			(0)
#define ROT_RDMA_BURST_LEN_HBIT			(1)
#define ROT_RDMA_BURST_LEN_MASK	      \
	GENMASK(ROT_RDMA_BURST_LEN_HBIT, ROT_RDMA_BURST_LEN_LBIT)

#define	ROT_RDMA_BURST_4		(0) << ROT_RDMA_BURST_LEN_LBIT
#define	ROT_RDMA_BURST_8		(1) << ROT_RDMA_BURST_LEN_LBIT
#define	ROT_RDMA_BURST_16		(2) << ROT_RDMA_BURST_LEN_LBIT
#define	ROT_RDMA_BURST_32		(3) << ROT_RDMA_BURST_LEN_LBIT
/* WDMA max length of the block DMA's burst. */
#define ROT_WDMA_BURST_LEN_LBIT			(2)
#define ROT_WDMA_BURST_LEN_HBIT			(3)
#define ROT_WDMA_BURST_LEN_MASK	      \
	GENMASK(ROT_WDMA_BURST_LEN_HBIT, ROT_WDMA_BURST_LEN_LBIT)

#define	ROT_WDMA_BURST_4		(0) << ROT_WDMA_BURST_LEN_LBIT
#define	ROT_WDMA_BURST_8		(1) << ROT_WDMA_BURST_LEN_LBIT
#define	ROT_WDMA_BURST_16		(2) << ROT_WDMA_BURST_LEN_LBIT
#define	ROT_WDMA_BURST_32		(3) << ROT_WDMA_BURST_LEN_LBIT
/* Rotater angle. */
#define ROT_ANGLE_LBIT			(4)
#define ROT_ANGLE_HBIT			(5)
#define ROT_ANGLE_MASK		      \
	GENMASK(ROT_ANGLE_HBIT, ROT_ANGLE_LBIT)

#define	ROT_ANGLE_0			(0) << ROT_ANGLE_LBIT
#define	ROT_ANGLE_90			(1) << ROT_ANGLE_LBIT
#define	ROT_ANGLE_180			(2) << ROT_ANGLE_LBIT
#define	ROT_ANGLE_270			(3) << ROT_ANGLE_LBIT
/* Vertical mirror */
#define	ROT_V_MIRROR			BIT(6)
/* Horizontal mirror */
#define	ROT_H_MIRROR			BIT(7)
/* Target format. */
#define ROT_WDMA_FMT_LBIT			(12)
#define ROT_WDMA_FMT_HBIT			(13)
#define ROT_WDMA_FMT_MASK	      \
	GENMASK(ROT_WDMA_FMT_HBIT, ROT_WDMA_FMT_LBIT)

#define	ROT_WDMA_FMT_ARGB8888		(0) << ROT_WDMA_FMT_LBIT
#define	ROT_WDMA_FMT_RGB565		(1) << ROT_WDMA_FMT_LBIT
#define	ROT_WDMA_FMT_RGB555		(2) << ROT_WDMA_FMT_LBIT
#define	ROT_WDMA_FMT_YUV422		(3) << ROT_WDMA_FMT_LBIT
/* RDMA ORDER. */
#define ROT_RDMA_ORDER_LBIT			(16)
#define ROT_RDMA_ORDER_HBIT			(18)
#define ROT_RDMA_ORDER_MASK	      \
	GENMASK(ROT_RDMA_ORDER_HBIT, ROT_RDMA_ORDER_LBIT)

#define	ROT_RDMA_ORDER_RGB      	(0) << ROT_RDMA_ORDER_LBIT
#define	ROT_RDMA_ORDER_RBG		(1) << ROT_RDMA_ORDER_LBIT
#define	ROT_RDMA_ORDER_GRB		(2) << ROT_RDMA_ORDER_LBIT
#define	ROT_RDMA_ORDER_GBR		(3) << ROT_RDMA_ORDER_LBIT
#define	ROT_RDMA_ORDER_BRG		(4) << ROT_RDMA_ORDER_LBIT
#define	ROT_RDMA_ORDER_BGR		(5) << ROT_RDMA_ORDER_LBIT
/* RDMA format. */
#define ROT_RDMA_FMT_LBIT			(24)
#define ROT_RDMA_FMT_HBIT			(27)
#define ROT_RDMA_FMT_MASK	      \
	GENMASK(ROT_RDMA_FMT_HBIT, ROT_RDMA_FMT_LBIT)

#define	ROT_RDMA_FMT_RGB555		(0) << ROT_RDMA_FMT_LBIT
#define	ROT_RDMA_FMT_RGB1555		(1) << ROT_RDMA_FMT_LBIT
#define	ROT_RDMA_FMT_RGB565		(2) << ROT_RDMA_FMT_LBIT
#define	ROT_RDMA_FMT_RGB888		(4) << ROT_RDMA_FMT_LBIT
#define	ROT_RDMA_FMT_ARGB8888		(5) << ROT_RDMA_FMT_LBIT
#define	ROT_RDMA_FMT_YUV422		(10) << ROT_RDMA_FMT_LBIT

/* rotate control(ROT_CTRL) bit field define */

/* START */
#define	ROT_START			BIT(0)
/* QCK_STOP */
#define	ROT_QCK_STP			BIT(1)
/* GEN_STOP */
#define	ROT_GEN_STP			BIT(2)
/* Reset the counter of FRM_DES */
#define ROT_DES_CNT_RST			BIT(3)

/* rotate status(ROT_SATUS) bit field define */

/* Rotater is working */
#define	ROT_WORKING			BIT(0)
/* Rotater is general stop */
#define	ROT_GEN_STOP_ACK		BIT(1)
/* One frame read end */
#define	ROT_EOF				BIT(2)
/* One frme read start */
#define	ROT_SOF				BIT(3)
/* Mask of ROT_GEN_STOP_ACK */
#define	ROT_GSA_MASK_ST			BIT(17)
/* Mask of FRM_END */
#define	ROT_EOF_MASK_ST			BIT(18)
/* Mask of FRM_START */
#define	ROT_SOF_MASK_ST			BIT(19)

/* rotarer clear status(ROT_CLR_ST) bit field define */

/* Clear general stop acknowledge */
#define	ROT_CLR_GEN_STOP_ACK		BIT(1)
/* Clear FRM_END */
#define	ROT_CLR_EOF			BIT(2)
/* Clear FRM_START */
#define	ROT_CLR_SOF			BIT(3)

/* rotator interrupt mask(ROT_INT_MASK) bit field define */

/* Mask general stop acknowledge */
#define	ROT_GSA_MASK			BIT(1)
/* Mask FRM_END */
#define	ROT_EOF_MASK			BIT(2)
/* Mask FRM_START */
#define	ROT_SOF_MASK			BIT(3)

/* rotator QOS config (ROT_QOS) bit field define */

/* STD_CLK */
#define ROT_STD_CLK_LBIT			(0)
#define ROT_STD_CLK_HBIT			(7)
#define ROT_STD_CLK_MASK	      \
	GENMASK(ROT_STD_CLK_HBIT, ROT_STD_CLK_LBIT)
/* STD_THR0 */
#define ROT_STD_THR0_LBIT			(8)
#define ROT_STD_THR0_HBIT			(15)
#define ROT_STD_THR0_MASK	      \
	GENMASK(ROT_STD_THR0_HBIT, ROT_STD_THR0_LBIT)
/* STD_THR1 */
#define ROT_STD_THR1_LBIT			(16)
#define ROT_STD_THR1_HBIT			(23)
#define ROT_STD_THR1_MASK	      \
	GENMASK(ROT_STD_THR1_HBIT, ROT_STD_THR1_LBIT)
/* STD_THR2 */
#define ROT_STD_THR2_LBIT			(24)
#define ROT_STD_THR2_HBIT			(31)
#define ROT_STD_THR2_MASK	      \
	GENMASK(ROT_STD_THR2_HBIT, ROT_STD_THR2_LBIT)
#endif
