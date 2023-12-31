/*
 * SFC controller for SPI protocol, use FIFO and DMA;
 *
 * Copyright (c) 2015 Ingenic
 * Author: <xiaoyang.fu@ingenic.com>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
*/

#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#include <asm/uaccess.h>
#include <asm/irq.h>
#include <asm/io.h>

#include "ingenic_sfc_common.h"


//#define SFC_DEBUG

#define GET_PHYADDR(a)  \
({						\
	unsigned int v;        \
	if (unlikely((unsigned int)(a) & 0x40000000)) {    \
	v = page_to_phys(vmalloc_to_page((const void *)(a))) | ((unsigned int)(a) & ~PAGE_MASK); \
	} else     \
	v = ((unsigned int)(a) & 0x1fffffff);                   \
	v;                                             \
 })
static inline void sfc_writel(struct sfc *sfc, unsigned short offset, u32 value)
{
	writel(value, sfc->iomem + offset);
}

static inline unsigned int sfc_readl(struct sfc *sfc, unsigned short offset)
{
	return readl(sfc->iomem + offset);
}

#ifdef SFC_DEBUG
void dump_sfc_reg(struct sfc *sfc)
{
	int i = 0;
	printk("SFC_GLB0			:%08x\n", sfc_readl(sfc, SFC_GLB0 ));
	printk("SFC_DEV_CONF	:%08x\n", sfc_readl(sfc, SFC_DEV_CONF ));
	printk("SFC_DEV_STA_EXP	:%08x\n", sfc_readl(sfc, SFC_DEV_STA_EXP));
	printk("SFC_DEV0_STA_RT	:%08x\n", sfc_readl(sfc, SFC_DEV0_STA_RT ));
	printk("SFC_DEV_STA_MSK	:%08x\n", sfc_readl(sfc, SFC_DEV_STA_MSK ));
	printk("SFC_TRAN_LEN		:%08x\n", sfc_readl(sfc, SFC_TRAN_LEN ));

	for(i = 0; i < 6; i++)
		printk("SFC_TRAN_CONF0(%d)	:%08x\n", i,sfc_readl(sfc, SFC_TRAN_CONF0(i)));
	for(i = 0; i < 6; i++)
		printk("SFC_TRAN_CONF1(%d)	:%08x\n", i,sfc_readl(sfc, SFC_TRAN_CONF1(i)));

	for(i = 0; i < 6; i++)
		printk("SFC_DEV_ADDR(%d)	:%08x\n", i,sfc_readl(sfc, SFC_DEV_ADDR(i)));

	printk("SFC_MEM_ADDR :%08x\n", sfc_readl(sfc, SFC_MEM_ADDR ));
	printk("SFC_TRIG	 :%08x\n", sfc_readl(sfc, SFC_TRIG));
	printk("SFC_SR		 :%08x\n", sfc_readl(sfc, SFC_SR));
	printk("SFC_SCR		 :%08x\n", sfc_readl(sfc, SFC_SCR));
	printk("SFC_INTC	 :%08x\n", sfc_readl(sfc, SFC_INTC));
	printk("SFC_FSM		 :%08x\n", sfc_readl(sfc, SFC_FSM ));
	printk("SFC_CGE		 :%08x\n", sfc_readl(sfc, SFC_CGE ));
//	printk("SFC_RM_DR 	 :%08x\n", sfc_readl(spi, SFC_RM_DR));
}
static void dump_reg(struct sfc *sfc)
{
	printk("SFC_GLB0 = %08x\n",sfc_readl(sfc,0x0000));
	printk("SFC_DEV_CONF = %08x\n",sfc_readl(sfc,0x0004));
	printk("SFC_DEV_STA_EXP = %08x\n",sfc_readl(sfc,0x0008));
	printk("SFC_DEV0_STA_RT	 = %08x\n",sfc_readl(sfc,0x000c));
	printk("SFC_DEV_STA_MASK = %08x\n",sfc_readl(sfc,0x0010));
	printk("SFC_TRAN_CONF0 = %08x\n",sfc_readl(sfc,0x0014));
	printk("SFC_TRAN_LEN = %08x\n",sfc_readl(sfc,0x002c));
	printk("SFC_DEV_ADDR0 = %08x\n",sfc_readl(sfc,0x0030));
	printk("SFC_DEV_ADDR_PLUS0 = %08x\n",sfc_readl(sfc,0x0048));
	printk("SFC_MEM_ADDR = %08x\n",sfc_readl(sfc,0x0060));
	printk("SFC_TRIG = %08x\n",sfc_readl(sfc,0x0064));
	printk("SFC_SR = %08x\n",sfc_readl(sfc,0x0068));
	printk("SFC_SCR = %08x\n",sfc_readl(sfc,0x006c));
	printk("SFC_INTC = %08x\n",sfc_readl(sfc,0x0070));
	printk("SFC_FSM = %08x\n",sfc_readl(sfc,0x0074));
	printk("SFC_CGE = %08x\n",sfc_readl(sfc,0x0078));
	printk("SFC_CMD_IDX = %08x\n",sfc_readl(sfc,0x007c));
	printk("SFC_COL_ADDR = %08x\n", sfc_readl(sfc, 0x80));
	printk("SFC_ROW_ADDR = %08x\n", sfc_readl(sfc, 0x84));
	printk("SFC_STA_ADDR0 = %08x\n", sfc_readl(sfc, 0x88));
	printk("SFC_DES_ADDR = %08x\n", sfc_readl(sfc, 0x90));
	printk("SFC_GLB1 = %08x\n", sfc_readl(sfc, 0x94));
	printk("SFC_DEV1_STA_RT = %08x\n", sfc_readl(sfc, 0x98));
	printk("SFC_TRAN_CONF1 = %08x\n", sfc_readl(sfc, 0x9c));
	printk("SFC_CDT = %08x\n", sfc_readl(sfc, 0x800));
//	printk("SFC_DR = %08x\n",sfc_readl(sfc,0x1000));
}

void dump_cdt(struct sfc *sfc)
{
	struct sfc_cdt *cdt;
	int i;

	if(sfc->iomem == NULL){
		printk("%s error: sfc res not init !\n", __func__);
		return;
	}

	cdt = sfc->iomem + 0x800;

	for(i = 0; i < 32; i++){
		printk("\nnum------->%d\n", i);
		printk("link:%02x, ENDIAN:%02x, WORD_UINT:%02x, TRAN_MODE:%02x, ADDR_KIND:%02x\n",
				(cdt[i].link >> 31) & 0x1, (cdt[i].link >> 18) & 0x1,
				(cdt[i].link >> 16) & 0x3, (cdt[i].link >> 4) & 0xf,
				(cdt[i].link >> 0) & 0x3
				);
		printk("CLK_MODE:%02x, ADDR_WIDTH:%02x, POLL_EN:%02x, CMD_EN:%02x,PHASE_FORMAT:%02x, DMY_BITS:%02x, DATA_EN:%02x, TRAN_CMD:%04x\n",
				(cdt[i].xfer >> 29) & 0x7, (cdt[i].xfer >> 26) & 0x7,
				(cdt[i].xfer >> 25) & 0x1, (cdt[i].xfer >> 24) & 0x1,
				(cdt[i].xfer >> 23) & 0x1, (cdt[i].xfer >> 17) & 0x3f,
				(cdt[i].xfer >> 16) & 0x1, (cdt[i].xfer >> 0) & 0xffff
				);
		printk("DEV_STA_EXP:%08x\n", cdt[i].staExp);
		printk("DEV_STA_MSK:%08x\n", cdt[i].staMsk);
	}
}

void dump_desc(struct sfc *sfc, uint32_t desc_num)
{
	struct sfc_desc *desc = sfc->desc;
	int i = 0;

	for(; i <= desc_num; i++){
		printk("\nDMA Descriptor ---->num: %d, addr: 0x%08x\n", i, (unsigned int)virt_to_phys(&desc[i]));
		printk("next_desc_addr: 0x%08x\n", desc[i].next_des_addr);
		printk("mem_addr: 0x%08x\n", desc[i].mem_addr);
		printk("tran_len: %d\n", desc[i].tran_len);
		printk("link: %d\n\n", desc[i].link);
	}
}

#endif

#if 0
static int32_t sfc_stop(struct sfc *sfc)
{
	int32_t timeout = 0xffff;
	sfc_writel(sfc, SFC_TRIG, TRIG_STOP);

	while((sfc_readl(sfc, SFC_SR) & SFC_BUSY) && timeout--);
	if(timeout < 0)
		return -EIO;
	return 0;
}
#endif

static inline void sfc_init(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_TRIG, TRIG_STOP);
	sfc_writel(sfc, SFC_DEV_CONF, 0);

	/* X1000 need set to 0,but X2000 can be set to 1*/
	sfc_writel(sfc, SFC_CGE, 0);

}

static inline void sfc_start(struct sfc *sfc)
{
	uint32_t tmp;
	tmp = sfc_readl(sfc, SFC_TRIG);
	tmp |= TRIG_START;
	sfc_writel(sfc, SFC_TRIG, tmp);
}

static inline void sfc_flush_fifo(struct sfc *sfc)
{
	unsigned int tmp;
	tmp = sfc_readl(sfc, SFC_TRIG);
	tmp |= TRIG_FLUSH;
	sfc_writel(sfc, SFC_TRIG, tmp);
}
static inline void  sfc_clear_end_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_SCR, CLR_END);
}

static inline void sfc_clear_treq_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_SCR, CLR_TREQ);
}

static inline void sfc_clear_rreq_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_SCR, CLR_RREQ);
}

static inline void sfc_clear_over_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_SCR, CLR_OVER);
}

static inline void sfc_clear_under_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_SCR, CLR_UNDER);
}

static inline void sfc_clear_all_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_SCR, 0x1f);
}

static inline void sfc_mask_all_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_INTC, 0x1f);
}

static void sfc_dev_hw_init(struct sfc *sfc)
{
	uint32_t tmp;
	tmp = sfc_readl(sfc, SFC_DEV_CONF);

	/*cpha bit:0 , cpol bit:0 */
	tmp &= ~(DEV_CONF_CPHA | DEV_CONF_CPOL);
	/*ce_dl bit:1, hold bit:1,wp bit:1*/
	tmp |= (DEV_CONF_CEDL | DEV_CONF_HOLDDL | DEV_CONF_WPDL);
	sfc_writel(sfc, SFC_DEV_CONF, tmp);

	/* use CDT mode */
	printk("Enter 'CDT' mode.\n");
	tmp = sfc_readl(sfc, SFC_GLB0);
	tmp |= GLB0_CDT_EN;
	sfc_writel(sfc, SFC_GLB0, tmp);

	/* use DMA Descriptor chain mode */
	printk("Enter 'DMA Descriptor chain' mode.\n");
	tmp = sfc_readl(sfc, SFC_GLB0);
	tmp |= GLB0_DES_EN;
	sfc_writel(sfc, SFC_GLB0, tmp);
}

static void sfc_threshold(struct sfc *sfc, uint32_t value)
{
	uint32_t tmp = sfc_readl(sfc, SFC_GLB0);
	tmp &= ~GLB0_THRESHOLD_MSK;
	tmp |= value << GLB0_THRESHOLD_OFFSET;
	sfc_writel(sfc, SFC_GLB0, tmp);
}

static void sfc_smp_delay(struct sfc *sfc, uint32_t value)
{
	uint32_t tmp;
	tmp = sfc_readl(sfc, SFC_DEV_CONF);
	tmp &= ~DEV_CONF_SMP_DELAY_MSK;
	tmp |= value << DEV_CONF_SMP_DELAY_OFFSET;
	sfc_writel(sfc, SFC_DEV_CONF, tmp);
}

int32_t set_flash_timing(struct sfc *sfc, uint32_t t_hold, uint32_t t_setup, uint32_t t_shslrd, uint32_t t_shslwr)
{
	uint32_t c_hold = 0;
	uint32_t c_setup = 0;
	uint32_t t_in = 0, c_in = 0;
	uint32_t tmp;
	unsigned long cycle;
	unsigned long long ns;

	ns = 1000000000ULL;
	cycle = do_div(ns, sfc->src_clk);
	cycle = ns;

	tmp = sfc_readl(sfc, SFC_DEV_CONF);
	tmp &= ~(DEV_CONF_THOLD_MSK | DEV_CONF_TSETUP_MSK | DEV_CONF_TSH_MSK);

	c_hold = t_hold / cycle;
	if(c_hold > 0)
		c_hold -= 1;

	c_setup = t_setup / cycle;
	if(c_setup > 0)
		c_setup -= 1;

	t_in = max(t_shslrd, t_shslwr);
	c_in = t_in / cycle;
	if(c_in > 0)
		c_in -= 1;

	tmp |= (c_hold << DEV_CONF_THOLD_OFFSET) | \
		  (c_setup << DEV_CONF_TSETUP_OFFSET) | \
		  (c_in << DEV_CONF_TSH_OFFSET);

	sfc_writel(sfc, SFC_DEV_CONF, tmp);
	return 0;
}

static void sfc_set_length(struct sfc *sfc, uint32_t value)
{
	sfc_writel(sfc, SFC_TRAN_LEN, value);
}

static inline void sfc_transfer_mode(struct sfc *sfc, uint32_t value)
{
	uint32_t tmp;
	tmp = sfc_readl(sfc, SFC_GLB0);
	if(value == 0)
		tmp &= ~GLB0_OP_MODE;
	else
		tmp |= GLB0_OP_MODE;
	sfc_writel(sfc, SFC_GLB0, tmp);
}

static void sfc_read_data(struct sfc *sfc, uint32_t *value)
{
	*value = sfc_readl(sfc, SFC_RM_DR);
}

static void sfc_write_data(struct sfc *sfc, uint32_t value)
{
	sfc_writel(sfc, SFC_RM_DR, value);
}

static void cpu_read_rxfifo(struct sfc *sfc, struct sfc_cdt_xfer *xfer)
{
	int32_t i = 0;
	uint32_t align_len = 0;
	uint32_t fifo_num = 0;
	uint32_t last_word = 0;
	uint32_t unalign_data;
	uint8_t *c;

	align_len = ALIGN(xfer->config.datalen, 4);

	if(((align_len - xfer->config.cur_len) / 4) > sfc->threshold) {
		fifo_num = sfc->threshold;
		last_word = 0;
	} else {
		/* last aligned THRESHOLD data*/
		if(xfer->config.datalen % 4) {
			fifo_num = (align_len - xfer->config.cur_len) / 4 - 1;
			last_word = 1;
		} else {
			fifo_num = (align_len - xfer->config.cur_len) / 4;
			last_word = 0;
		}
	}

	if ((uint32_t)xfer->config.buf & 0x3) {
		/* addr not align */
		for (i = 0; i < fifo_num; i++) {
			sfc_read_data(sfc, &unalign_data);
			c = xfer->config.buf;
			c[0] = (unalign_data >> 0) & 0xff;
			c[1] = (unalign_data >> 8) & 0xff;
			c[2] = (unalign_data >> 16) & 0xff;
			c[3] = (unalign_data >> 24) & 0xff;

			xfer->config.buf += 4;
			xfer->config.cur_len += 4;
		}
	} else {
		/* addr align */
		for (i = 0; i < fifo_num; i++) {
			sfc_read_data(sfc, (uint32_t *)xfer->config.buf);
			xfer->config.buf += 4;
			xfer->config.cur_len += 4;
		}
	}

	/* last word */
	if(last_word == 1) {
		sfc_read_data(sfc, &unalign_data);
		c = (uint8_t *)xfer->config.buf;

		for(i = 0; i < xfer->config.datalen % 4; i++) {
			c[i] = (unalign_data >> (i * 8)) & 0xff;
		}

		xfer->config.buf += xfer->config.datalen % 4;
		xfer->config.cur_len += xfer->config.datalen % 4;
	}

}

static void cpu_write_txfifo(struct sfc *sfc, struct sfc_cdt_xfer *xfer)
{
	uint32_t align_len = 0;
	uint32_t fifo_num = 0;
	uint32_t data = 0;
	uint32_t i;
	uint32_t nbytes = xfer->config.datalen % 4;

	align_len = xfer->config.datalen / 4 * 4;

	if (((align_len - xfer->config.cur_len) / 4) >= sfc->threshold) {
		fifo_num = sfc->threshold;
		nbytes = 0;
	} else {
		fifo_num = (align_len - xfer->config.cur_len) / 4;
	}

	if ((uint32_t)xfer->config.buf & 0x3) {
		/* addr not align */
		for(i = 0; i < fifo_num; i++) {
			data = xfer->config.buf[3] << 24 | xfer->config.buf[2] << 16 | xfer->config.buf[1] << 8 | xfer->config.buf[0];
			sfc_write_data(sfc, data);
			xfer->config.buf += 4;
			xfer->config.cur_len += 4;
		}
	} else {
		/* addr align */
		for(i = 0; i < fifo_num; i++) {
			sfc_write_data(sfc, *(uint32_t *)xfer->config.buf);
			xfer->config.buf += 4;
			xfer->config.cur_len += 4;
		}
	}

	if(nbytes) {
		data = 0;
		for(i = 0; i < nbytes; i++)
			data |= xfer->config.buf[i] << i * 8;
		sfc_write_data(sfc, data);
		xfer->config.cur_len += nbytes;
	}

}

uint32_t sfc_get_sta_rt0(struct sfc *sfc)
{
	return sfc_readl(sfc, SFC_DEV0_STA_RT);
}


static void sfc_enable_all_intc(struct sfc *sfc)
{
	sfc_writel(sfc, SFC_INTC, 0);
}

static void sfc_set_mem_addr(struct sfc *sfc, unsigned int addr)
{
	sfc_writel(sfc, SFC_MEM_ADDR, addr);
}

static void sfc_set_desc_addr(struct sfc *sfc, unsigned int addr)
{
	sfc_writel(sfc, SFC_DES_ADDR, addr);
}

void *sfc_get_paddr(void *vaddr)
{
	unsigned long paddr;
	unsigned int pfn = 0;
	unsigned int page_offset = 0;

	if (is_vmalloc_addr(vaddr)) {
		pfn = vmalloc_to_pfn(vaddr);
		page_offset = (unsigned int)vaddr & (PAGE_SIZE - 1);
		paddr = (pfn << 12) + page_offset;
	} else {
		paddr = virt_to_phys(vaddr);
	}

	return (void *)paddr;
}

int32_t create_sfc_desc(struct sfc_flash *flash, unsigned char *vaddr, size_t len)
{
	struct sfc *sfc = flash->sfc;
	struct sfc_desc *desc = sfc->desc;
	uint32_t ualign_size, off_len, last_len, step_len, page_num;
	int current_pfn = 0, next_pfn = 0;
	uint32_t i = 0;

	ualign_size = (unsigned int)vaddr & (PAGE_SIZE - 1);
	off_len = PAGE_SIZE - ualign_size;

	if(is_vmalloc_addr(vaddr) && (len > off_len)){
		page_num = (len - off_len) >> (ffs(PAGE_SIZE) - 1);
		last_len = (len - off_len) & (PAGE_SIZE - 1);
		current_pfn = vmalloc_to_pfn(vaddr);

		desc[i].next_des_addr = 0;
		desc[i].mem_addr = (unsigned int)sfc_get_paddr((void *)vaddr);
		desc[i].tran_len = off_len;
		desc[i].link = 1;

		vaddr += off_len;
		step_len = PAGE_SIZE;

		/* case 1. Handle physical address discontinuity */
		do{
			if(!page_num){
				if(last_len)
					step_len = last_len;
				else{
					break;
				}
			}

			next_pfn = vmalloc_to_pfn(vaddr);
			if((current_pfn + 1) != next_pfn){
				if(++i > (sfc->desc_max_num - 1)){
					dev_err(flash->dev, "%s The number of descriptors exceeds the maximum limit.\n", __func__);
					return -ENOMEM;
				}

				desc[i-1].next_des_addr = (unsigned int)sfc_get_paddr((void *)&desc[i]);

				desc[i].next_des_addr = 0;
				desc[i].mem_addr = (unsigned int)sfc_get_paddr((void *)vaddr);
				desc[i].tran_len = step_len;
				desc[i].link = 1;
			}else{
				desc[i].tran_len += step_len;
			}


			if(page_num){
				current_pfn = next_pfn;
				vaddr += step_len;
			}
		}while(page_num--);
	}else{
		/* case 2. Physical Address Continuity and only need one descriptor */
		desc[i].next_des_addr = 0;
		desc[i].mem_addr = (unsigned int)sfc_get_paddr((void *)vaddr);
		desc[i].tran_len = len;
	}

	/* last descriptor is not link */
	desc[i].link = 0;

	return i;
}

#define SFC_TRANSFER_TIMEOUT	3000	//3000ms for timeout
static int32_t sfc_start_transfer(struct sfc *sfc)
{
	int32_t err;
	sfc_clear_all_intc(sfc);
	sfc_enable_all_intc(sfc);
	sfc_start(sfc);
	err = wait_for_completion_timeout(&sfc->done, msecs_to_jiffies(SFC_TRANSFER_TIMEOUT));
	if (!err) {
		sfc_mask_all_intc(sfc);
		sfc_clear_all_intc(sfc);
		printk("line:%d Timeout for ACK from SFC device\n",__LINE__);
		return -ETIMEDOUT;
	}
	return 0;
}

void write_cdt(struct sfc *sfc, struct sfc_cdt *cdt, uint16_t start_index, uint16_t end_index)
{
	uint32_t cdt_num, cdt_size;

	cdt_num = end_index - start_index + 1;
	cdt_size = sizeof(struct sfc_cdt);

	memcpy((void *)sfc->iomem + SFC_CDT + (start_index * cdt_size), (void *)cdt + (start_index * cdt_size), cdt_num * cdt_size);
	//printk("create CDT index: %d ~ %d,  index number:%d.\n", start_index, end_index, cdt_num);
}

static void sfc_set_index(struct sfc *sfc, unsigned short index)
{

	uint32_t tmp = sfc_readl(sfc, SFC_CMD_IDX);
	tmp &= ~CMD_IDX_MSK;
	tmp |= index;
	sfc_writel(sfc, SFC_CMD_IDX, tmp);
}

static void sfc_set_dataen(struct sfc *sfc, uint8_t dataen)
{

	uint32_t tmp = sfc_readl(sfc, SFC_CMD_IDX);
	tmp &= ~CDT_DATAEN_MSK;
	tmp |= (dataen << CDT_DATAEN_OFF);
	sfc_writel(sfc, SFC_CMD_IDX, tmp);
}

static void sfc_set_datadir(struct sfc *sfc, uint8_t datadir)
{

	uint32_t tmp = sfc_readl(sfc, SFC_CMD_IDX);
	tmp &= ~CDT_DIR_MSK;
	tmp |= (datadir << CDT_DIR_OFF);
	sfc_writel(sfc, SFC_CMD_IDX, tmp);
}


int sfc_sync_cdt(struct sfc *sfc, struct sfc_cdt_xfer *xfer)
{
	/*0.reset transfer length*/
	sfc_set_length(sfc, 0);

	/*1. set index*/
	sfc_set_index(sfc, xfer->cmd_index);

	/*2. set addr*/
	sfc_writel(sfc, SFC_COL_ADDR, xfer->columnaddr);
	sfc_writel(sfc, SFC_ROW_ADDR, xfer->rowaddr);
	sfc_writel(sfc, SFC_STA_ADDR0, xfer->staaddr0);
	sfc_writel(sfc, SFC_STA_ADDR1, xfer->staaddr1);

	/*3. config data*/
	sfc_set_dataen(sfc, xfer->dataen);
	if(xfer->dataen){
		sfc_set_datadir(sfc, xfer->config.data_dir);
		sfc_transfer_mode(sfc, xfer->config.ops_mode);
		sfc_set_length(sfc, xfer->config.datalen);

		/* Memory address for DMA when do not use DMA descriptor */
		sfc_set_mem_addr(sfc, 0);

		if(xfer->config.ops_mode == DMA_OPS){
			if(xfer->config.data_dir == GLB0_TRAN_DIR_READ){
				dma_cache_sync(NULL, (void *)xfer->config.buf, xfer->config.datalen, DMA_FROM_DEVICE);
			}else{
				dma_cache_sync(NULL, (void *)xfer->config.buf, xfer->config.datalen, DMA_TO_DEVICE);
			}
			/* Set Descriptor address for DMA */
			sfc_set_desc_addr(sfc, virt_to_phys(sfc->desc));
		}
		sfc->xfer = xfer;
	}

	return sfc_start_transfer(sfc);

}

static irqreturn_t ingenic_sfc_pio_irq_callback(int32_t irq, void *dev)
{
	struct sfc *sfc = dev;
	uint32_t val;

	val = sfc_readl(sfc, SFC_SR) & 0x1f;

	if(val & CLR_RREQ) {
		sfc_clear_rreq_intc(sfc);
		cpu_read_rxfifo(sfc, sfc->xfer);
	} else if(val & CLR_TREQ) {
		sfc_clear_treq_intc(sfc);
		cpu_write_txfifo(sfc, sfc->xfer);
	} else if(val & CLR_OVER) {
		sfc_clear_over_intc(sfc);
		pr_err("sfc OVER !\n");
		complete(&sfc->done);
	} else if(val & CLR_UNDER) {
		sfc_clear_under_intc(sfc);
		pr_err("sfc UNDR !\n");
		complete(&sfc->done);
	} else if(val & CLR_END) {
		sfc_mask_all_intc(sfc);
		sfc_clear_end_intc(sfc);
		complete(&sfc->done);
	}
	return IRQ_HANDLED;
}

static void ingenic_sfc_init_setup(struct sfc *sfc)
{
	sfc_init(sfc);
	sfc_threshold(sfc, sfc->threshold);
	sfc_dev_hw_init(sfc);

	sfc_transfer_mode(sfc, SLAVE_MODE);
	if(sfc->src_clk >= 100000000){
		sfc_smp_delay(sfc, DEV_CONF_SMP_DELAY_180);
	}
}

struct sfc *sfc_res_init(struct platform_device *pdev)
{
	struct device_node* np = pdev->dev.of_node;
	struct ingenic_sfc_info *board_info;
	struct sfc *sfc;
	struct resource *res;
	int32_t err = 0;

	board_info = devm_kzalloc(&pdev->dev, sizeof(struct ingenic_sfc_info), GFP_KERNEL);
	if(!board_info){
		printk("ERROR: %s %d devm_kzalloc() error !\n",__func__,__LINE__);
		return ERR_PTR(-ENOMEM);
	}

	sfc = devm_kzalloc(&pdev->dev, sizeof(struct sfc), GFP_KERNEL);
	if (!sfc) {
		printk("ERROR: %s %d devm_kzalloc() error !\n",__func__,__LINE__);
		return ERR_PTR(-ENOMEM);
	}

	err = of_property_read_u32(np, "ingenic,sfc-max-frequency", (unsigned int *)&sfc->src_clk);
	if (err < 0) {
		dev_err(&pdev->dev, "Cannot get sfc max frequency\n");
		return ERR_PTR(-ENOENT);
	}

	err = of_property_read_u32(np, "ingenic,spiflash_param_offset", (unsigned int *)&board_info->param_offset);
	if (err < 0) {
		dev_err(&pdev->dev, "No dts param_offset, use default.\n");
		board_info->param_offset = -EINVAL;
	}

	err = of_property_read_u8(np, "ingenic,use_board_info", &board_info->use_board_info);
	if (err < 0) {
		dev_err(&pdev->dev, "Cannot get sfc use_board_info\n");
		return ERR_PTR(-ENOENT);
	}

	err = platform_device_add_data(pdev, board_info, sizeof(struct ingenic_sfc_info));
	if(err){
		printk("ERROR: %s %d error !\n",__func__,__LINE__);
		return ERR_PTR(-ENOMEM);
	}

	/* find and map our resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		dev_err(&pdev->dev, "Cannot get IORESOURCE_MEM\n");
		return ERR_PTR(-ENOENT);
	}

	sfc->iomem = devm_ioremap_resource(&pdev->dev, res);
	if (sfc->iomem == NULL) {
		dev_err(&pdev->dev, "Cannot map IO\n");
		return ERR_PTR(-ENXIO);
	}

#ifndef FPGA_TEST
	sfc->clk = devm_clk_get(&pdev->dev, "div_sfc");
	if (IS_ERR(sfc->clk)) {
		dev_err(&pdev->dev, "Cannot get div_sfc clock\n");
		return ERR_PTR(-ENOENT);
	}

	sfc->clk_gate = devm_clk_get(&pdev->dev, "gate_sfc");
	if (IS_ERR(sfc->clk_gate)) {
		dev_err(&pdev->dev, "Cannot get sfc clock\n");
		return ERR_PTR(-ENOENT);
	}

	clk_set_rate(sfc->clk, sfc->src_clk);
	if(clk_prepare_enable(sfc->clk)) {
		dev_err(&pdev->dev, "cgu clk error\n");
		return ERR_PTR(-ENOENT);
	}
	if(clk_prepare_enable(sfc->clk_gate)) {
		dev_err(&pdev->dev, "gate clk error\n");
		return ERR_PTR(-ENOENT);
	}
#endif

	sfc->threshold = THRESHOLD;

	/* request SFC irq */
	sfc->irq = platform_get_irq(pdev, 0);
	if (sfc->irq < 0) {
		dev_err(&pdev->dev, "No IRQ specified\n");
		return ERR_PTR(-ENOENT);
	}

	err = devm_request_irq(&pdev->dev, sfc->irq, ingenic_sfc_pio_irq_callback, 0, pdev->name, sfc);
	if (err) {
		dev_err(&pdev->dev, "Cannot claim IRQ\n");
		return ERR_PTR(-EINVAL);
	}

	/* SFC controller initializations for SFC */
	ingenic_sfc_init_setup(sfc);
	init_completion(&sfc->done);
	return sfc;
}
