/* * drivers/media/platform/ingenic_rotate/rotate.c
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

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/of.h>

#include <linux/platform_device.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "rotate.h"
#include "rotate-regs.h"

#define fh2ctx(__fh) container_of(__fh, struct ingenic_rot_ctx, fh)

static struct timeval time_now, time_last;
static long interval_in_us;

static struct rot_fmt formats[] = {
	{
		.name	= "ARGB_8888",
		.fourcc	= V4L2_PIX_FMT_RGB32,
		.depth	= 32,
		.types	= MEM2MEM_CAPTURE | MEM2MEM_OUTPUT,
	},
	{
		.name	= "RGB_888",
		.fourcc	= V4L2_PIX_FMT_RGB24,
		.depth	= 32,
		.types	= MEM2MEM_OUTPUT,
	},
	{
		.name	= "RGB_565",
		.fourcc	= V4L2_PIX_FMT_RGB565,
		.depth	= 16,
		.types	= MEM2MEM_CAPTURE | MEM2MEM_OUTPUT,
	},
	{
		.name	= "RGB_555",
		.fourcc	= V4L2_PIX_FMT_RGB555,
		.depth	= 16,
		.types	= MEM2MEM_CAPTURE | MEM2MEM_OUTPUT,
	},
	{
		.name	= "XRGB_1555",
		.fourcc	= V4L2_PIX_FMT_RGB555X,
		.depth	= 16,
		.types	= MEM2MEM_OUTPUT,
	},
	{
		.name	= "YUV 422P",
		.fourcc	= V4L2_PIX_FMT_YUYV,
		.depth	= 16,
		.types	= MEM2MEM_CAPTURE | MEM2MEM_OUTPUT,
	},
};
#define NUM_FORMATS ARRAY_SIZE(formats)

static struct rot_fmt *find_fmt(struct v4l2_format *f)
{
	unsigned int i;
	for (i = 0; i < NUM_FORMATS; i++) {
		if (formats[i].fourcc == f->fmt.pix.pixelformat)
			return &formats[i];
	}
	return NULL;
}


static struct rot_frm_info *get_frame(struct ingenic_rot_ctx *ctx,
		enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->in;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->out;
	default:
		return ERR_PTR(-EINVAL);
	}
}

static int rot_queue_setup(struct vb2_queue *vq, const void *parg,
			   unsigned int *nbuffers, unsigned int *nplanes,
			   unsigned int sizes[], void *alloc_ctxs[])
{
	struct ingenic_rot_ctx *ctx = vb2_get_drv_priv(vq);
	struct rot_frm_info *f = get_frame(ctx, vq->type);

	if (IS_ERR(f))
		return PTR_ERR(f);

	*nplanes = 1;
	sizes[0] = f->size;
	alloc_ctxs[0] = ctx->dev->alloc_ctx;

	if (*nbuffers == 0)
		*nbuffers = 1;

	return 0;
}

static int rot_buf_prepare(struct vb2_buffer *vb)
{
	struct ingenic_rot_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct rot_frm_info *f = get_frame(ctx, vb->vb2_queue->type);

	if (IS_ERR(f))
		return PTR_ERR(f);
	vb2_set_plane_payload(vb, 0, f->size);
	return 0;
}

static void rot_buf_queue(struct vb2_buffer *vb)
{
	struct ingenic_rot_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (ctx->m2m_ctx)
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}


static struct vb2_ops rot_qops = {
	.queue_setup	= rot_queue_setup,
	.buf_prepare	= rot_buf_prepare,
	.buf_queue	= rot_buf_queue,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
						struct vb2_queue *dst_vq)
{
	struct ingenic_rot_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rot_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->mutex;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rot_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->mutex;

	return vb2_queue_init(dst_vq);
}

static int rot_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ingenic_rot_ctx *ctx = container_of(ctrl->handler, struct ingenic_rot_ctx,
								ctrl_handler);
	unsigned long flags;

	spin_lock_irqsave(&ctx->dev->ctrl_lock, flags);
	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		ctx->hflip = ctx->ctrl_hflip->val;
		break;
	case V4L2_CID_VFLIP:
		ctx->vflip = ctx->ctrl_vflip->val;
		break;
	case V4L2_CID_ROTATE:
		ctx->angle = ctx->ctrl_rot->val;
		break;
	}
	spin_unlock_irqrestore(&ctx->dev->ctrl_lock, flags);
	return 0;
}

static const struct v4l2_ctrl_ops rot_ctrl_ops = {
	.s_ctrl		= rot_s_ctrl,
};

static int rot_setup_ctrls(struct ingenic_rot_ctx *ctx)
{
	struct ingenic_rot_dev *dev = ctx->dev;

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 3);

	ctx->ctrl_hflip = v4l2_ctrl_new_std(&ctx->ctrl_handler, &rot_ctrl_ops,
						V4L2_CID_HFLIP, 0, 1, 1, 0);

	ctx->ctrl_vflip = v4l2_ctrl_new_std(&ctx->ctrl_handler, &rot_ctrl_ops,
						V4L2_CID_VFLIP, 0, 1, 1, 0);
	ctx->ctrl_rot = v4l2_ctrl_new_std(&ctx->ctrl_handler, &rot_ctrl_ops,
						V4L2_CID_ROTATE, 0, 270, 90, 0);

	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;
		v4l2_err(&dev->v4l2_dev, "rot_setup_ctrls failed\n");
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return err;
	}

	return 0;
}

static void rot_update_info(struct ingenic_rot_ctx *ctx)
{
	struct rot_frm_info *in, *out;

	in = &ctx->in;
	out = &ctx->out;

	in->width = DEFAULT_WIDTH;
	in->height = DEFAULT_HEIGHT;
	in->fmt = &formats[1];
	in->bytesperline = DEFAULT_WIDTH * in->fmt->depth >> 3;
	in->size = in->bytesperline * in->height;

	out->width = DEFAULT_WIDTH;
	out->height = DEFAULT_HEIGHT;
	out->fmt = &formats[0];
	out->bytesperline = DEFAULT_WIDTH * out->fmt->depth >> 3;
	out->size = out->bytesperline * out->height;
}

static int rot_reqdesc(struct ingenic_rot_ctx *ctx)
{
	struct ingenic_rot_dev *dev = ctx->dev;
	struct video_device *vfd = dev->vfd;
	int i, size;

	size = sizeof(struct ingenic_rot_desc) * ROT_DESC_NUM;
	ctx->desc[0] = (struct ingenic_rot_desc *)dma_alloc_coherent(&vfd->dev, size,
						&ctx->desc_phys[0], GFP_KERNEL);
	if (!ctx->desc[0]) {
		dev_err(&vfd->dev, "dma_alloc_coherent of size %d failed\n", size);
		return -ENOMEM;
	}

	for(i = 1; i < ROT_DESC_NUM; i++) {
		ctx->desc[i] = ctx->desc[0] + i;
		ctx->desc_phys[i] = ctx->desc_phys[0] + i*sizeof(struct ingenic_rot_desc);
	}
	return 0;
}

static void rot_freedesc(struct ingenic_rot_ctx *ctx)
{
	struct ingenic_rot_dev *dev = ctx->dev;
	struct video_device *vfd = dev->vfd;
	int size;

	size = sizeof(struct ingenic_rot_desc) * ROT_DESC_NUM;
	dma_free_coherent(&vfd->dev, size, ctx->desc[0], ctx->desc_phys[0]);
}

static int rot_open(struct file *file)
{
	struct ingenic_rot_dev *dev = video_drvdata(file);
	struct ingenic_rot_ctx *ctx = NULL;
	int ret = 0;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	if (mutex_lock_interruptible(&dev->mutex)) {
		kfree(ctx);
		return -ERESTARTSYS;
	}
	ctx->dev = dev;

	/* Set default formats */
	rot_update_info(ctx);

	ret = rot_reqdesc(ctx);
	if(ret) {
		goto free;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	ctx->m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, &queue_init);
	if (IS_ERR(ctx->m2m_ctx)) {
		ret = PTR_ERR(ctx->m2m_ctx);
		goto err;
	}
	ctx->fh.m2m_ctx = ctx->m2m_ctx;

	ret = rot_setup_ctrls(ctx);
	if(ret)
		goto err;

	/* Write the default values to the ctx struct */
	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);

	mutex_unlock(&dev->mutex);

	return 0;
err:
	rot_freedesc(ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
free:
	mutex_unlock(&dev->mutex);
	kfree(ctx);
	return ret;
}

static int rot_release(struct file *file)
{
	struct ingenic_rot_dev *dev = video_drvdata(file);
	struct ingenic_rot_ctx *ctx = fh2ctx(file->private_data);

	mutex_lock(&dev->mutex);
	v4l2_m2m_ctx_release(ctx->m2m_ctx);
	mutex_unlock(&dev->mutex);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	rot_freedesc(ctx);
	kfree(ctx);
	return 0;
}

static int vidioc_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	strncpy(cap->driver, JZ_ROT_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, JZ_ROT_NAME, sizeof(cap->card) - 1);
	cap->bus_info[0] = 0;
	cap->version = KERNEL_VERSION(1, 0, 0);
	/*
	 * This is only a mem-to-mem video device. The capture and output
	 * device capability flags are left only for backward compatibility
	 * and are scheduled for removal.
	 */
	cap->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M;
	cap->capabilities =  cap->device_caps |
		V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT |
		V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int enum_fmt(struct v4l2_fmtdesc *f, u32 type)
{
	int i, num;
	struct rot_fmt *fmt;

	num = 0;
	for (i = 0; i < NUM_FORMATS; ++i) {
		if (formats[i].types & type) {
			/* index-th format of type type found ? */
			if (num == f->index)
				break;
			/* Correct type but haven't reached our index yet,
			 * just increment per-type index */
			++num;
		}
	}

	if (i < NUM_FORMATS) {
		/* Format found */
		fmt = &formats[i];
		strncpy(f->description, fmt->name, sizeof(f->description) - 1);
		f->pixelformat = fmt->fourcc;
		return 0;
	}

	/* Format not found */
	return -EINVAL;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, MEM2MEM_CAPTURE);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, MEM2MEM_OUTPUT);
}

static inline struct ingenic_rot_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct ingenic_rot_ctx, fh);
}

static int vidioc_g_fmt(struct file *file, void *prv, struct v4l2_format *f)
{
	struct ingenic_rot_ctx *ctx = fh_to_ctx(prv);
	struct vb2_queue *vq;
	struct rot_frm_info *frm;

	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;
	frm = get_frame(ctx, f->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	f->fmt.pix.width		= frm->width;
	f->fmt.pix.height		= frm->height;
	f->fmt.pix.field		= V4L2_FIELD_NONE;
	f->fmt.pix.pixelformat		= frm->fmt->fourcc;
	f->fmt.pix.bytesperline		= (frm->width * frm->fmt->depth) >> 3;
	f->fmt.pix.sizeimage		= frm->size;
	return 0;
}

static int vidioc_try_fmt(struct file *file, void *prv, struct v4l2_format *f)
{
	struct rot_fmt *fmt;
	enum v4l2_field *field;

	fmt = find_fmt(f);
	if (!fmt)
		return -EINVAL;

	field = &f->fmt.pix.field;
	if (*field == V4L2_FIELD_ANY)
		*field = V4L2_FIELD_NONE;
	else if (*field != V4L2_FIELD_NONE)
		return -EINVAL;

	if (f->fmt.pix.width > MAX_WIDTH
		|| f->fmt.pix.height > MAX_HEIGHT
		|| f->fmt.pix.width < MIN_WIDTH
		|| f->fmt.pix.height < MIN_HEIGHT) {
		return -EINVAL;
	}

	f->fmt.pix.bytesperline = (f->fmt.pix.width * fmt->depth) >> 3;
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;
	return 0;
}

static int vidioc_s_fmt(struct file *file, void *prv, struct v4l2_format *f)
{
	struct ingenic_rot_ctx *ctx = fh_to_ctx(prv);
	struct ingenic_rot_dev *dev = ctx->dev;
	struct vb2_queue *vq;
	struct rot_frm_info *frm;
	struct rot_fmt *fmt;
	int ret = 0;

	/* Adjust all values accordingly to the hardware capabilities
	 * and chosen format. */
	ret = vidioc_try_fmt(file, prv, f);
	if (ret)
		return ret;
	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, f->type);
	if (vb2_is_busy(vq)) {
		v4l2_err(&dev->v4l2_dev, "queue (%d) bust\n", f->type);
		return -EBUSY;
	}
	frm = get_frame(ctx, f->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);
	fmt = find_fmt(f);
	if (!fmt)
		return -EINVAL;
	frm->width	= f->fmt.pix.width;
	frm->height	= f->fmt.pix.height;
	frm->size	= f->fmt.pix.sizeimage;
	frm->fmt	= fmt;
	frm->bytesperline	= f->fmt.pix.bytesperline;
	return 0;
}

static inline long timeval_sub_to_us(struct timeval lhs,
				struct timeval rhs)
{
	long sec, usec;
	sec = lhs.tv_sec - rhs.tv_sec;
	usec = lhs.tv_usec - rhs.tv_usec;

	return (sec*1000000 + usec);
}

static int vidioc_streamon(struct file *file, void *priv,
					enum v4l2_buf_type type)
{
	struct ingenic_rot_ctx *ctx = priv;
	struct ingenic_rot_dev *dev = ctx->dev;

	clk_prepare_enable(dev->clk);

	return v4l2_m2m_streamon(file, ctx->m2m_ctx, type);
}

static int vidioc_streamoff(struct file *file, void *priv,
					enum v4l2_buf_type type)
{
	struct ingenic_rot_ctx *ctx = priv;
	struct ingenic_rot_dev *dev = ctx->dev;

	clk_disable_unprepare(dev->clk);

	return v4l2_m2m_streamoff(file, ctx->m2m_ctx, type);
}

/* Need change, One frame spends times */
#define ROT_TIMEOUT 400

static void job_abort(void *prv)
{
	struct ingenic_rot_ctx *ctx = prv;
	struct ingenic_rot_dev *dev = ctx->dev;
	int ret;

	if (dev->curr == NULL) /* No job currently running */
		return;

	ret = wait_event_timeout(dev->irq_queue,
		dev->curr == NULL,
		msecs_to_jiffies(ROT_TIMEOUT));
}

static void device_run(void *prv)
{
	struct ingenic_rot_ctx *ctx = prv;
	struct ingenic_rot_dev *dev = ctx->dev;
	struct vb2_buffer *src, *dst;
	unsigned long flags;

	dev->curr = ctx;

	src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);

	spin_lock_irqsave(&dev->ctrl_lock, flags);

	rot_reset(dev);

	rot_set_src_cfg(dev, &ctx->in);
	rot_set_src_desc(dev, vb2_dma_contig_plane_dma_addr(src, 0));

	rot_set_dst_cfg(dev, &ctx->out);
	rot_set_dst_desc(dev, vb2_dma_contig_plane_dma_addr(dst, 0));

	rot_set_angle(dev, ctx->angle);
	rot_set_vflip(dev, ctx->vflip);
	rot_set_hflip(dev, ctx->hflip);

	do_gettimeofday(&time_now);

	rot_start(dev);

#ifdef ROT_GEN_STOP
	rot_gen_stop(dev);
#endif

	spin_unlock_irqrestore(&dev->ctrl_lock, flags);
}

static irqreturn_t rot_irq_handler(int irq, void *prv)
{
	struct ingenic_rot_dev *dev = prv;
	struct ingenic_rot_ctx *ctx = dev->curr;
	struct vb2_v4l2_buffer *src, *dst;

	do_gettimeofday(&time_last);
	interval_in_us = timeval_sub_to_us(time_last, time_now);

	rot_clr_irq(dev);

	if(unlikely(ctx == NULL)) {
		printk("Rotater:ctx == NULL\n");
		return IRQ_HANDLED;
	}

	src = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
	if(unlikely(src == NULL)) {
		printk("Rotater:src == NULL\n");
		return IRQ_HANDLED;
	}
	dst = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
	if(unlikely(dst == NULL)) {
		printk("Rotater:dst == NULL\n");
		return IRQ_HANDLED;
	}

	dst->timecode = src->timecode;
	dst->timestamp = src->timestamp;

	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(dev->m2m_dev, ctx->m2m_ctx);

	dev->curr = NULL;
#ifdef ROT_QCK_STOP
	rot_qck_stop(dev);
#endif
#if defined(ROT_GEN_STOP) || defined(ROT_QCK_STOP)
	schedule_work(&dev->rot_work);
#endif
	wake_up(&dev->irq_queue);
	return IRQ_HANDLED;
}

#if defined(ROT_GEN_STOP) || defined(ROT_QCK_STOP)
static void rot_wait_dev_end(struct work_struct *rot_work)
{
	struct ingenic_rot_dev *dev;

	dev = container_of(rot_work, struct ingenic_rot_dev, rot_work);
	wait_rot_state(dev, ROT_WORKING, 0);
}
#endif

static const struct v4l2_file_operations rot_fops = {
	.owner		= THIS_MODULE,
	.open		= rot_open,
	.release	= rot_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct v4l2_ioctl_ops rot_ioctl_ops = {
	.vidioc_querycap		= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap	= vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= vidioc_g_fmt,
	.vidioc_try_fmt_vid_cap		= vidioc_try_fmt,
	.vidioc_s_fmt_vid_cap		= vidioc_s_fmt,

	.vidioc_enum_fmt_vid_out	= vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out		= vidioc_g_fmt,
	.vidioc_try_fmt_vid_out		= vidioc_try_fmt,
	.vidioc_s_fmt_vid_out		= vidioc_s_fmt,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,

	.vidioc_streamon		= vidioc_streamon,
	.vidioc_streamoff		= vidioc_streamoff,
};

static struct video_device rot_videodev = {
	.name		= "ingenic-rot",
	.fops		= &rot_fops,
	.ioctl_ops	= &rot_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.vfl_dir	= VFL_DIR_M2M,
};

static struct v4l2_m2m_ops rot_m2m_ops = {
	.device_run	= device_run,
	.job_abort	= job_abort,
};

static int ingenic_rot_probe(struct platform_device *pdev)
{
	struct ingenic_rot_dev *dev;
	struct video_device *vfd;
	struct resource *res;
	int ret = 0;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;
	spin_lock_init(&dev->ctrl_lock);
	mutex_init(&dev->mutex);
	atomic_set(&dev->num_inst, 0);
	init_waitqueue_head(&dev->irq_queue);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	dev->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dev->regs))
		return PTR_ERR(dev->regs);

	/* interrupt service routine registration */
	dev->irq = ret = platform_get_irq(pdev, 0);
	if (dev->irq < 0) {
		dev_err(&pdev->dev, "cannot find IRQ\n");
		return ret;
	}

	ret = devm_request_irq(&pdev->dev, dev->irq, rot_irq_handler,
						0, pdev->name, dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to install IRQ\n");
		return ret;
	}

	/* clocks */
	dev->clk = clk_get(&pdev->dev, "gate_rot");
	if (IS_ERR(dev->clk)) {
		dev_err(&pdev->dev, "cannot get clock\n");
		ret = PTR_ERR(dev->clk);
		return ret;
	}
	dev_dbg(&pdev->dev, "rot clock source %p\n", dev->clk);

#if defined(ROT_QCK_STOP) || defined(ROT_GEN_STOP)
	INIT_WORK(&dev->rot_work, rot_wait_dev_end);
#endif
	dev->alloc_ctx = vb2_dma_contig_init_ctx(&pdev->dev);
	if (IS_ERR(dev->alloc_ctx)) {
		ret = PTR_ERR(dev->alloc_ctx);
		goto clk_get_rollback;
	}

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		goto alloc_ctx_cleanup;
	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&dev->v4l2_dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto unreg_v4l2_dev;
	}
	*vfd = rot_videodev;
	vfd->lock = &dev->mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto rel_vdev;
	}
	video_set_drvdata(vfd, dev);
	snprintf(vfd->name, sizeof(vfd->name), "%s", rot_videodev.name);
	dev->vfd = vfd;
	v4l2_info(&dev->v4l2_dev, "device registered as /dev/video%d\n",
								vfd->num);
	platform_set_drvdata(pdev, dev);
	dev->m2m_dev = v4l2_m2m_init(&rot_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(dev->m2m_dev);
		goto unreg_video_dev;
	}
	return 0;

unreg_video_dev:
	video_unregister_device(dev->vfd);
rel_vdev:
	video_device_release(vfd);
unreg_v4l2_dev:
	v4l2_device_unregister(&dev->v4l2_dev);
alloc_ctx_cleanup:
	vb2_dma_contig_cleanup_ctx(dev->alloc_ctx);
clk_get_rollback:
	clk_put(dev->clk);

	return ret;
}

static int ingenic_rot_remove(struct platform_device *pdev)
{
	struct ingenic_rot_dev *dev = (struct ingenic_rot_dev *)platform_get_drvdata(pdev);

	v4l2_info(&dev->v4l2_dev, "Removing " JZ_ROT_NAME);
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(dev->vfd);
	video_device_release(dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	vb2_dma_contig_cleanup_ctx(dev->alloc_ctx);
	clk_put(dev->clk);
	return 0;
}

static const struct of_device_id ingenic_rotate_match[] = {
	{
		.compatible = "ingenic,x2000-rotate",
		.data = NULL,
	},
};

MODULE_DEVICE_TABLE(of, ingenic_rotate_match);


static struct platform_driver rot_pdrv = {
	.probe		= ingenic_rot_probe,
	.remove		= ingenic_rot_remove,
	.driver		= {
		.owner = THIS_MODULE,
		.of_match_table	= of_match_ptr(ingenic_rotate_match),
		.name = JZ_ROT_NAME,
		.owner = THIS_MODULE,
	},
};

module_platform_driver(rot_pdrv);

MODULE_AUTHOR("clwang<chunlei.wang@ingenic.com>");
MODULE_DESCRIPTION("X2000 rotate driver");
MODULE_LICENSE("GPL");
