// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Intel Corporation

#include <linux/module.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "ipu3.h"
#include "ipu3-dmamap.h"

/******************** v4l2_subdev_ops ********************/

static int ipu3_subdev_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_rect try_crop = {
		.top = 0,
		.left = 0,
		.width = 1920,
		.height = 1080,
	};
	unsigned int i;

	/* Initialize try_fmt */
	for (i = 0; i < IMGU_NODE_NUM; i++) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_get_try_format(sd, fh->pad, i);

		try_fmt->width = try_crop.width;
		try_fmt->height = try_crop.height;
		try_fmt->code = MEDIA_BUS_FMT_FIXED;
		try_fmt->colorspace = V4L2_COLORSPACE_RAW;
		try_fmt->field = V4L2_FIELD_NONE;
	}

	*v4l2_subdev_get_try_crop(sd, fh->pad, IMGU_NODE_IN) = try_crop;
	*v4l2_subdev_get_try_compose(sd, fh->pad, IMGU_NODE_IN) = try_crop;

	return 0;
}

static int ipu3_subdev_s_stream(struct v4l2_subdev *sd, int enable)
{
	int i;
	unsigned int node;
	int r = 0;
	struct imgu_device *imgu = v4l2_get_subdevdata(sd);
	struct imgu_v4l2_subdev *imgu_sd = container_of(sd,
							struct imgu_v4l2_subdev,
							subdev);
	unsigned int pipe = imgu_sd->pipe;
	struct device *dev = &imgu->pci_dev->dev;
	struct v4l2_pix_format_mplane *fmts[IPU3_CSS_QUEUES] = { NULL };
	struct v4l2_rect *rects[IPU3_CSS_RECTS] = { NULL };
	struct ipu3_css_pipe *css_pipe = &imgu->css.pipes[pipe];
	struct imgu_media_pipe *imgu_pipe = &imgu->imgu_pipe[pipe];

	dev_dbg(dev, "%s %d for pipe %d", __func__, enable, pipe);
	/* grab ctrl after streamon and return after off */
	v4l2_ctrl_grab(imgu_sd->ctrl, enable);

	if (!enable) {
		imgu_sd->active = false;
		return 0;
	}

	for (i = 0; i < IMGU_NODE_NUM; i++)
		imgu_pipe->queue_enabled[i] = imgu_pipe->nodes[i].enabled;

	/* This is handled specially */
	imgu_pipe->queue_enabled[IPU3_CSS_QUEUE_PARAMS] = false;

	/* Initialize CSS formats */
	for (i = 0; i < IPU3_CSS_QUEUES; i++) {
		node = imgu_map_node(imgu, i);
		/* No need to reconfig meta nodes */
		if (node == IMGU_NODE_STAT_3A || node == IMGU_NODE_PARAMS)
			continue;
		fmts[i] = imgu_pipe->queue_enabled[node] ?
			&imgu_pipe->nodes[node].vdev_fmt.fmt.pix_mp : NULL;
	}

	/* Enable VF output only when VF queue requested by user */
	css_pipe->vf_output_en = false;
	if (imgu_pipe->nodes[IMGU_NODE_VF].enabled)
		css_pipe->vf_output_en = true;

	if (atomic_read(&imgu_sd->running_mode) == IPU3_RUNNING_MODE_VIDEO)
		css_pipe->pipe_id = IPU3_CSS_PIPE_ID_VIDEO;
	else
		css_pipe->pipe_id = IPU3_CSS_PIPE_ID_CAPTURE;

	dev_dbg(dev, "IPU3 pipe %d pipe_id %d", pipe, css_pipe->pipe_id);

	rects[IPU3_CSS_RECT_EFFECTIVE] = &imgu_sd->rect.eff;
	rects[IPU3_CSS_RECT_BDS] = &imgu_sd->rect.bds;
	rects[IPU3_CSS_RECT_GDC] = &imgu_sd->rect.gdc;

	r = ipu3_css_fmt_set(&imgu->css, fmts, rects, pipe);
	if (r) {
		dev_err(dev, "failed to set initial formats pipe %d with (%d)",
			pipe, r);
		return r;
	}

	imgu_sd->active = true;

	return 0;
}

static int ipu3_subdev_get_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_pad_config *cfg,
			       struct v4l2_subdev_format *fmt)
{
	struct imgu_device *imgu = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *mf;
	struct imgu_media_pipe *imgu_pipe;
	u32 pad = fmt->pad;
	struct imgu_v4l2_subdev *imgu_sd = container_of(sd,
							struct imgu_v4l2_subdev,
							subdev);
	unsigned int pipe = imgu_sd->pipe;

	imgu_pipe = &imgu->imgu_pipe[pipe];
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		fmt->format = imgu_pipe->nodes[pad].pad_fmt;
	} else {
		mf = v4l2_subdev_get_try_format(sd, cfg, pad);
		fmt->format = *mf;
	}

	return 0;
}

static int ipu3_subdev_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_pad_config *cfg,
			       struct v4l2_subdev_format *fmt)
{
	struct imgu_media_pipe *imgu_pipe;
	struct imgu_device *imgu = v4l2_get_subdevdata(sd);
	struct imgu_v4l2_subdev *imgu_sd = container_of(sd,
							struct imgu_v4l2_subdev,
							subdev);

	struct v4l2_mbus_framefmt *mf;
	u32 pad = fmt->pad;
	unsigned int pipe = imgu_sd->pipe;

	dev_dbg(&imgu->pci_dev->dev, "set subdev %d pad %d fmt to [%dx%d]",
		pipe, pad, fmt->format.width, fmt->format.height);

	imgu_pipe = &imgu->imgu_pipe[pipe];
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		mf = v4l2_subdev_get_try_format(sd, cfg, pad);
	else
		mf = &imgu_pipe->nodes[pad].pad_fmt;

	fmt->format.code = mf->code;
	/* Clamp the w and h based on the hardware capabilities */
	if (imgu_sd->subdev_pads[pad].flags & MEDIA_PAD_FL_SOURCE) {
		fmt->format.width = clamp(fmt->format.width,
					  IPU3_OUTPUT_MIN_WIDTH,
					  IPU3_OUTPUT_MAX_WIDTH);
		fmt->format.height = clamp(fmt->format.height,
					   IPU3_OUTPUT_MIN_HEIGHT,
					   IPU3_OUTPUT_MAX_HEIGHT);
	} else {
		fmt->format.width = clamp(fmt->format.width,
					  IPU3_INPUT_MIN_WIDTH,
					  IPU3_INPUT_MAX_WIDTH);
		fmt->format.height = clamp(fmt->format.height,
					   IPU3_INPUT_MIN_HEIGHT,
					   IPU3_INPUT_MAX_HEIGHT);
	}

	*mf = fmt->format;

	return 0;
}

static int ipu3_subdev_get_selection(struct v4l2_subdev *sd,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_selection *sel)
{
	struct v4l2_rect *try_sel, *r;
	struct imgu_v4l2_subdev *imgu_sd = container_of(sd,
							struct imgu_v4l2_subdev,
							subdev);

	if (sel->pad != IMGU_NODE_IN)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		try_sel = v4l2_subdev_get_try_crop(sd, cfg, sel->pad);
		r = &imgu_sd->rect.eff;
		break;
	case V4L2_SEL_TGT_COMPOSE:
		try_sel = v4l2_subdev_get_try_compose(sd, cfg, sel->pad);
		r = &imgu_sd->rect.bds;
		break;
	default:
		return -EINVAL;
	}

	if (sel->which == V4L2_SUBDEV_FORMAT_TRY)
		sel->r = *try_sel;
	else
		sel->r = *r;

	return 0;
}

static int ipu3_subdev_set_selection(struct v4l2_subdev *sd,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_selection *sel)
{
	struct imgu_device *imgu = v4l2_get_subdevdata(sd);
	struct imgu_v4l2_subdev *imgu_sd = container_of(sd,
							struct imgu_v4l2_subdev,
							subdev);
	struct v4l2_rect *rect, *try_sel;

	dev_dbg(&imgu->pci_dev->dev,
		 "set subdev %d sel which %d target 0x%4x rect [%dx%d]",
		 imgu_sd->pipe, sel->which, sel->target,
		 sel->r.width, sel->r.height);

	if (sel->pad != IMGU_NODE_IN)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		try_sel = v4l2_subdev_get_try_crop(sd, cfg, sel->pad);
		rect = &imgu_sd->rect.eff;
		break;
	case V4L2_SEL_TGT_COMPOSE:
		try_sel = v4l2_subdev_get_try_compose(sd, cfg, sel->pad);
		rect = &imgu_sd->rect.bds;
		break;
	default:
		return -EINVAL;
	}

	if (sel->which == V4L2_SUBDEV_FORMAT_TRY)
		*try_sel = sel->r;
	else
		*rect = sel->r;

	return 0;
}

/******************** media_entity_operations ********************/

static int ipu3_link_setup(struct media_entity *entity,
			   const struct media_pad *local,
			   const struct media_pad *remote, u32 flags)
{
	struct imgu_media_pipe *imgu_pipe;
	struct v4l2_subdev *sd = container_of(entity, struct v4l2_subdev,
					      entity);
	struct imgu_device *imgu = v4l2_get_subdevdata(sd);
	struct imgu_v4l2_subdev *imgu_sd = container_of(sd,
							struct imgu_v4l2_subdev,
							subdev);
	unsigned int pipe = imgu_sd->pipe;
	u32 pad = local->index;

	WARN_ON(entity->type != MEDIA_ENT_T_V4L2_SUBDEV);
	WARN_ON(pad >= IMGU_NODE_NUM);

	dev_dbg(&imgu->pci_dev->dev, "pipe %d pad %d is %s", pipe, pad,
		 flags & MEDIA_LNK_FL_ENABLED ? "enabled" : "disabled");

	imgu_pipe = &imgu->imgu_pipe[pipe];
	imgu_pipe->nodes[pad].enabled = flags & MEDIA_LNK_FL_ENABLED;

	/* enable input node to enable the pipe */
	if (pad != IMGU_NODE_IN)
		return 0;

	if (flags & MEDIA_LNK_FL_ENABLED)
		__set_bit(pipe, imgu->css.enabled_pipes);
	else
		__clear_bit(pipe, imgu->css.enabled_pipes);

	dev_dbg(&imgu->pci_dev->dev, "pipe %d is %s", pipe,
		 flags & MEDIA_LNK_FL_ENABLED ? "enabled" : "disabled");

	return 0;
}

/******************** vb2_ops ********************/

static int ipu3_vb2_buf_init(struct vb2_buffer *vb)
{
	struct sg_table *sg = vb2_dma_sg_plane_desc(vb, 0);
	struct imgu_device *imgu = vb2_get_drv_priv(vb->vb2_queue);
	struct imgu_buffer *buf = container_of(vb,
		struct imgu_buffer, vid_buf.vbb.vb2_buf);
	struct imgu_video_device *node =
		container_of(vb->vb2_queue, struct imgu_video_device, vbq);
	unsigned int queue = imgu_node_to_queue(node->id);

	if (queue == IPU3_CSS_QUEUE_PARAMS)
		return 0;

	return ipu3_dmamap_map_sg(imgu, sg->sgl, sg->nents, &buf->map);
}

/* Called when each buffer is freed */
static void ipu3_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	struct imgu_device *imgu = vb2_get_drv_priv(vb->vb2_queue);
	struct imgu_buffer *buf = container_of(vb,
		struct imgu_buffer, vid_buf.vbb.vb2_buf);
	struct imgu_video_device *node =
		container_of(vb->vb2_queue, struct imgu_video_device, vbq);
	unsigned int queue = imgu_node_to_queue(node->id);

	if (queue == IPU3_CSS_QUEUE_PARAMS)
		return;

	ipu3_dmamap_unmap(imgu, &buf->map);
}

/* Transfer buffer ownership to me */
static void ipu3_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct imgu_device *imgu = vb2_get_drv_priv(vb->vb2_queue);
	struct imgu_video_device *node =
		container_of(vb->vb2_queue, struct imgu_video_device, vbq);
	unsigned int queue = imgu_node_to_queue(node->id);
	unsigned long need_bytes;
	unsigned int pipe = node->pipe;

	if (vb->vb2_queue->type == V4L2_BUF_TYPE_META_CAPTURE ||
	    vb->vb2_queue->type == V4L2_BUF_TYPE_META_OUTPUT)
		need_bytes = node->vdev_fmt.fmt.meta.buffersize;
	else
		need_bytes = node->vdev_fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	if (queue == IPU3_CSS_QUEUE_PARAMS) {
		unsigned long payload = vb2_get_plane_payload(vb, 0);
		struct vb2_v4l2_buffer *buf =
			container_of(vb, struct vb2_v4l2_buffer, vb2_buf);
		int r = -EINVAL;

		if (payload == 0) {
			payload = need_bytes;
			vb2_set_plane_payload(vb, 0, payload);
		}
		if (payload >= need_bytes)
			r = ipu3_css_set_parameters(&imgu->css, pipe,
						    vb2_plane_vaddr(vb, 0));
		buf->flags = V4L2_BUF_FLAG_DONE;
		vb2_buffer_done(vb, r == 0 ? VB2_BUF_STATE_DONE
					   : VB2_BUF_STATE_ERROR);

	} else {
		struct imgu_buffer *buf = container_of(vb, struct imgu_buffer,
						       vid_buf.vbb.vb2_buf);

		mutex_lock(&imgu->lock);
		ipu3_css_buf_init(&buf->css_buf, queue, buf->map.daddr);
		list_add_tail(&buf->vid_buf.list,
			      &node->buffers);
		mutex_unlock(&imgu->lock);

		vb2_set_plane_payload(&buf->vid_buf.vbb.vb2_buf, 0, need_bytes);

		if (imgu->streaming)
			imgu_queue_buffers(imgu, false, pipe);
	}

	dev_dbg(&imgu->pci_dev->dev, "%s for pipe %d node %d", __func__,
		node->pipe, node->id);

}

static int ipu3_vb2_queue_setup(struct vb2_queue *vq, const void *parg,
				unsigned int *num_buffers,
				unsigned int *num_planes,
				unsigned int sizes[],
				void *alloc_ctxs[])
{
	struct imgu_device *imgu = vb2_get_drv_priv(vq);
	struct imgu_video_device *node =
		container_of(vq, struct imgu_video_device, vbq);
	const struct v4l2_format *fmt = &node->vdev_fmt;
	unsigned int size;

	*num_buffers = clamp_val(*num_buffers, 1, VB2_MAX_FRAME);
	alloc_ctxs[0] = imgu->vb2_alloc_ctx;

	if (vq->type == V4L2_BUF_TYPE_META_CAPTURE ||
	    vq->type == V4L2_BUF_TYPE_META_OUTPUT)
		size = fmt->fmt.meta.buffersize;
	else
		size = fmt->fmt.pix_mp.plane_fmt[0].sizeimage;

	if (*num_planes) {
		if (sizes[0] < size)
			return -EINVAL;
		size = sizes[0];
	}

	*num_planes = 1;
	sizes[0] = size;

	/* Initialize buffer queue */
	INIT_LIST_HEAD(&node->buffers);

	return 0;
}

/* Check if all enabled video nodes are streaming, exception ignored */
static bool ipu3_all_nodes_streaming(struct imgu_device *imgu,
				     struct imgu_video_device *except)
{
	unsigned int i, pipe, p;
	struct imgu_video_device *node;
	struct device *dev = &imgu->pci_dev->dev;

	pipe = except->pipe;
	if (!test_bit(pipe, imgu->css.enabled_pipes)) {
		dev_warn(&imgu->pci_dev->dev,
			 "pipe %d link is not ready yet", pipe);
		return false;
	}

	for_each_set_bit(p, imgu->css.enabled_pipes, IMGU_MAX_PIPE_NUM) {
		for (i = 0; i < IMGU_NODE_NUM; i++) {
			node = &imgu->imgu_pipe[p].nodes[i];
			dev_dbg(dev, "%s pipe %u queue %u name %s enabled = %u",
				__func__, p, i, node->name, node->enabled);
			if (node == except)
				continue;
			if (node->enabled && !vb2_start_streaming_called(&node->vbq))
				return false;
		}
	}

	return true;
}

static void ipu3_return_all_buffers(struct imgu_device *imgu,
				    struct imgu_video_device *node,
				    enum vb2_buffer_state state)
{
	struct ipu3_vb2_buffer *b, *b0;

	/* Return all buffers */
	mutex_lock(&imgu->lock);
	list_for_each_entry_safe(b, b0, &node->buffers, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vbb.vb2_buf, state);
	}
	mutex_unlock(&imgu->lock);
}

static int ipu3_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct imgu_media_pipe *imgu_pipe;
	struct imgu_device *imgu = vb2_get_drv_priv(vq);
	struct device *dev = &imgu->pci_dev->dev;
	struct imgu_video_device *node =
		container_of(vq, struct imgu_video_device, vbq);
	int r;
	unsigned int pipe;

	dev_dbg(dev, "%s node name %s pipe %d id %u", __func__,
		node->name, node->pipe, node->id);

	if (imgu->streaming) {
		r = -EBUSY;
		goto fail_return_bufs;
	}

	if (!node->enabled) {
		dev_err(dev, "IMGU node is not enabled");
		r = -EINVAL;
		goto fail_return_bufs;
	}

	pipe = node->pipe;
	imgu_pipe = &imgu->imgu_pipe[pipe];
	r = media_entity_pipeline_start(&node->vdev.entity,
					&imgu_pipe->pipeline);
	if (r < 0)
		goto fail_return_bufs;


	if (!ipu3_all_nodes_streaming(imgu, node))
		return 0;

	for_each_set_bit(pipe, imgu->css.enabled_pipes, IMGU_MAX_PIPE_NUM) {
		r = v4l2_subdev_call(&imgu->imgu_pipe[pipe].imgu_sd.subdev,
				     video, s_stream, 1);
		if (r < 0)
			goto fail_stop_pipeline;
	}

	/* Start streaming of the whole pipeline now */
	dev_dbg(dev, "IMGU streaming is ready to start");
	r = imgu_s_stream(imgu, true);
	if (!r)
		imgu->streaming = true;

	return 0;

fail_stop_pipeline:
	media_entity_pipeline_stop(&node->vdev.entity);
fail_return_bufs:
	ipu3_return_all_buffers(imgu, node, VB2_BUF_STATE_QUEUED);

	return r;
}

static void ipu3_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct imgu_media_pipe *imgu_pipe;
	struct imgu_device *imgu = vb2_get_drv_priv(vq);
	struct device *dev = &imgu->pci_dev->dev;
	struct imgu_video_device *node =
		container_of(vq, struct imgu_video_device, vbq);
	int r;
	unsigned int pipe;

	WARN_ON(!node->enabled);

	pipe = node->pipe;
	dev_dbg(dev, "Try to stream off node [%d][%d]", pipe, node->id);
	imgu_pipe = &imgu->imgu_pipe[pipe];
	r = v4l2_subdev_call(&imgu_pipe->imgu_sd.subdev, video, s_stream, 0);
	if (r)
		dev_err(&imgu->pci_dev->dev,
			"failed to stop subdev streaming\n");

	/* Was this the first node with streaming disabled? */
	if (imgu->streaming && ipu3_all_nodes_streaming(imgu, node)) {
		/* Yes, really stop streaming now */
		dev_dbg(dev, "IMGU streaming is ready to stop");
		r = imgu_s_stream(imgu, false);
		if (!r)
			imgu->streaming = false;
	}

	ipu3_return_all_buffers(imgu, node, VB2_BUF_STATE_ERROR);
	media_entity_pipeline_stop(&node->vdev.entity);
}

/******************** v4l2_ioctl_ops ********************/

#define VID_CAPTURE	0
#define VID_OUTPUT	1
#define DEF_VID_CAPTURE	0
#define DEF_VID_OUTPUT	1

struct ipu3_fmt {
	u32	fourcc;
	u16	type; /* VID_CAPTURE or VID_OUTPUT not both */
};

/* format descriptions for capture and preview */
static const struct ipu3_fmt formats[] = {
	{ V4L2_PIX_FMT_NV12, VID_CAPTURE },
	{ V4L2_PIX_FMT_IPU3_SGRBG10, VID_OUTPUT },
	{ V4L2_PIX_FMT_IPU3_SBGGR10, VID_OUTPUT },
	{ V4L2_PIX_FMT_IPU3_SGBRG10, VID_OUTPUT },
	{ V4L2_PIX_FMT_IPU3_SRGGB10, VID_OUTPUT },
};

/* Find the first matched format, return default if not found */
static const struct ipu3_fmt *find_format(struct v4l2_format *f, u32 type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if (formats[i].fourcc == f->fmt.pix_mp.pixelformat &&
		    formats[i].type == type)
			return &formats[i];
	}

	return type == VID_CAPTURE ? &formats[DEF_VID_CAPTURE] :
				     &formats[DEF_VID_OUTPUT];
}

static int ipu3_vidioc_querycap(struct file *file, void *fh,
				struct v4l2_capability *cap)
{
	struct imgu_video_device *node = file_to_intel_ipu3_node(file);

	strscpy(cap->driver, IMGU_NAME, sizeof(cap->driver));
	strscpy(cap->card, IMGU_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s", node->name);

	return 0;
}

static int enum_fmts(struct v4l2_fmtdesc *f, u32 type)
{
	unsigned int i, j;

	for (i = j = 0; i < ARRAY_SIZE(formats); ++i) {
		if (formats[i].type == type) {
			if (j == f->index)
				break;
			++j;
		}
	}

	if (i < ARRAY_SIZE(formats)) {
		f->pixelformat = formats[i].fourcc;
		return 0;
	}

	return -EINVAL;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	return enum_fmts(f, VID_CAPTURE);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	return enum_fmts(f, VID_OUTPUT);
}

/* Propagate forward always the format from the CIO2 subdev */
static int ipu3_vidioc_g_fmt(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct imgu_video_device *node = file_to_intel_ipu3_node(file);

	f->fmt = node->vdev_fmt.fmt;

	return 0;
}

/*
 * Set input/output format. Unless it is just a try, this also resets
 * selections (ie. effective and BDS resolutions) to defaults.
 */
static int imgu_fmt(struct imgu_device *imgu, unsigned int pipe, int node,
		    struct v4l2_format *f, bool try)
{
	struct device *dev = &imgu->pci_dev->dev;
	struct v4l2_pix_format_mplane try_fmts[IPU3_CSS_QUEUES];
	struct v4l2_pix_format_mplane *fmts[IPU3_CSS_QUEUES] = { NULL };
	struct v4l2_rect *rects[IPU3_CSS_RECTS] = { NULL };
	struct v4l2_mbus_framefmt pad_fmt;
	unsigned int i, css_q;
	int r;
	struct ipu3_css_pipe *css_pipe = &imgu->css.pipes[pipe];
	struct imgu_media_pipe *imgu_pipe = &imgu->imgu_pipe[pipe];
	struct imgu_v4l2_subdev *imgu_sd = &imgu_pipe->imgu_sd;

	dev_dbg(dev, "set fmt node [%u][%u](try = %d)", pipe, node, try);

	for (i = 0; i < IMGU_NODE_NUM; i++)
		dev_dbg(dev, "IMGU pipe %d node %d enabled = %d",
			pipe, i, imgu_pipe->nodes[i].enabled);

	if (imgu_pipe->nodes[IMGU_NODE_VF].enabled)
		css_pipe->vf_output_en = true;

	if (atomic_read(&imgu_sd->running_mode) == IPU3_RUNNING_MODE_VIDEO)
		css_pipe->pipe_id = IPU3_CSS_PIPE_ID_VIDEO;
	else
		css_pipe->pipe_id = IPU3_CSS_PIPE_ID_CAPTURE;

	dev_dbg(dev, "IPU3 pipe %d pipe_id = %d", pipe, css_pipe->pipe_id);

	for (i = 0; i < IPU3_CSS_QUEUES; i++) {
		unsigned int inode = imgu_map_node(imgu, i);

		/* Skip the meta node */
		if (inode == IMGU_NODE_STAT_3A || inode == IMGU_NODE_PARAMS)
			continue;

		if (try) {
			try_fmts[i] =
				imgu_pipe->nodes[inode].vdev_fmt.fmt.pix_mp;
			fmts[i] = &try_fmts[i];
		} else {
			fmts[i] = &imgu_pipe->nodes[inode].vdev_fmt.fmt.pix_mp;
		}

		/* CSS expects some format on OUT queue */
		if (i != IPU3_CSS_QUEUE_OUT &&
		    !imgu_pipe->nodes[inode].enabled)
			fmts[i] = NULL;
	}

	if (!try) {
		/* eff and bds res got by imgu_s_sel */
		struct imgu_v4l2_subdev *imgu_sd = &imgu_pipe->imgu_sd;

		rects[IPU3_CSS_RECT_EFFECTIVE] = &imgu_sd->rect.eff;
		rects[IPU3_CSS_RECT_BDS] = &imgu_sd->rect.bds;
		rects[IPU3_CSS_RECT_GDC] = &imgu_sd->rect.gdc;

		/* suppose that pad fmt was set by subdev s_fmt before */
		pad_fmt = imgu_pipe->nodes[IMGU_NODE_IN].pad_fmt;
		rects[IPU3_CSS_RECT_GDC]->width = pad_fmt.width;
		rects[IPU3_CSS_RECT_GDC]->height = pad_fmt.height;
	}

	/*
	 * imgu doesn't set the node to the value given by user
	 * before we return success from this function, so set it here.
	 */
	css_q = imgu_node_to_queue(node);
	if (fmts[css_q])
		*fmts[css_q] = f->fmt.pix_mp;
	else
		return -EINVAL;

	if (try)
		r = ipu3_css_fmt_try(&imgu->css, fmts, rects, pipe);
	else
		r = ipu3_css_fmt_set(&imgu->css, fmts, rects, pipe);

	/* r is the binary number in the firmware blob */
	if (r < 0)
		return r;

	if (try)
		f->fmt.pix_mp = *fmts[css_q];
	else
		f->fmt = imgu_pipe->nodes[node].vdev_fmt.fmt;

	return 0;
}

static int ipu3_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pixm = &f->fmt.pix_mp;
	const struct ipu3_fmt *fmt;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		fmt = find_format(f, VID_CAPTURE);
	else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = find_format(f, VID_OUTPUT);
	else
		return -EINVAL;

	pixm->pixelformat = fmt->fourcc;

	memset(pixm->plane_fmt[0].reserved, 0,
	       sizeof(pixm->plane_fmt[0].reserved));

	return 0;
}

static int ipu3_vidioc_try_fmt(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct imgu_device *imgu = video_drvdata(file);
	struct device *dev = &imgu->pci_dev->dev;
	struct imgu_video_device *node = file_to_intel_ipu3_node(file);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	int r;

	dev_dbg(dev, "%s [%ux%u] for node %d\n", __func__,
		pix_mp->width, pix_mp->height, node->id);

	r = ipu3_try_fmt(file, fh, f);
	if (r)
		return r;

	return imgu_fmt(imgu, node->pipe, node->id, f, true);
}

static int ipu3_vidioc_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct imgu_device *imgu = video_drvdata(file);
	struct device *dev = &imgu->pci_dev->dev;
	struct imgu_video_device *node = file_to_intel_ipu3_node(file);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	int r;

	dev_dbg(dev, "%s [%ux%u] for node %d\n", __func__,
		pix_mp->width, pix_mp->height, node->id);

	r = ipu3_try_fmt(file, fh, f);
	if (r)
		return r;

	return imgu_fmt(imgu, node->pipe, node->id, f, false);
}

static int ipu3_meta_enum_format(struct file *file, void *fh,
				 struct v4l2_fmtdesc *f)
{
	struct imgu_video_device *node = file_to_intel_ipu3_node(file);

	/* Each node is dedicated to only one meta format */
	if (f->index > 0 || f->type != node->vbq.type)
		return -EINVAL;

	f->pixelformat = node->vdev_fmt.fmt.meta.dataformat;

	return 0;
}

static int ipu3_vidioc_g_meta_fmt(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	struct imgu_video_device *node = file_to_intel_ipu3_node(file);

	if (f->type != node->vbq.type)
		return -EINVAL;

	f->fmt = node->vdev_fmt.fmt;

	return 0;
}

static int ipu3_vidioc_enum_input(struct file *file, void *fh,
				  struct v4l2_input *input)
{
	if (input->index > 0)
		return -EINVAL;
	strscpy(input->name, "camera", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;

	return 0;
}

static int ipu3_vidioc_g_input(struct file *file, void *fh, unsigned int *input)
{
	*input = 0;

	return 0;
}

static int ipu3_vidioc_s_input(struct file *file, void *fh, unsigned int input)
{
	return input == 0 ? 0 : -EINVAL;
}

static int ipu3_vidioc_enum_output(struct file *file, void *fh,
				   struct v4l2_output *output)
{
	if (output->index > 0)
		return -EINVAL;
	strscpy(output->name, "camera", sizeof(output->name));
	output->type = V4L2_INPUT_TYPE_CAMERA;

	return 0;
}

static int ipu3_vidioc_g_output(struct file *file, void *fh,
				unsigned int *output)
{
	*output = 0;

	return 0;
}

static int ipu3_vidioc_s_output(struct file *file, void *fh,
				unsigned int output)
{
	return output == 0 ? 0 : -EINVAL;
}

/******************** function pointers ********************/

static struct v4l2_subdev_internal_ops ipu3_subdev_internal_ops = {
	.open = ipu3_subdev_open,
};

static const struct v4l2_subdev_core_ops ipu3_subdev_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops ipu3_subdev_video_ops = {
	.s_stream = ipu3_subdev_s_stream,
};

static const struct v4l2_subdev_pad_ops ipu3_subdev_pad_ops = {
	.link_validate = v4l2_subdev_link_validate_default,
	.get_fmt = ipu3_subdev_get_fmt,
	.set_fmt = ipu3_subdev_set_fmt,
	.get_selection = ipu3_subdev_get_selection,
	.set_selection = ipu3_subdev_set_selection,
};

static const struct v4l2_subdev_ops ipu3_subdev_ops = {
	.core = &ipu3_subdev_core_ops,
	.video = &ipu3_subdev_video_ops,
	.pad = &ipu3_subdev_pad_ops,
};

static const struct media_entity_operations ipu3_media_ops = {
	.link_setup = ipu3_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

/****************** vb2_ops of the Q ********************/

static const struct vb2_ops ipu3_vb2_ops = {
	.buf_init = ipu3_vb2_buf_init,
	.buf_cleanup = ipu3_vb2_buf_cleanup,
	.buf_queue = ipu3_vb2_buf_queue,
	.queue_setup = ipu3_vb2_queue_setup,
	.start_streaming = ipu3_vb2_start_streaming,
	.stop_streaming = ipu3_vb2_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/****************** v4l2_file_operations *****************/

static const struct v4l2_file_operations ipu3_v4l2_fops = {
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

/******************** v4l2_ioctl_ops ********************/

static const struct v4l2_ioctl_ops ipu3_v4l2_ioctl_ops = {
	.vidioc_querycap = ipu3_vidioc_querycap,

	.vidioc_enum_fmt_vid_cap_mplane = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap_mplane = ipu3_vidioc_g_fmt,
	.vidioc_s_fmt_vid_cap_mplane = ipu3_vidioc_s_fmt,
	.vidioc_try_fmt_vid_cap_mplane = ipu3_vidioc_try_fmt,

	.vidioc_enum_fmt_vid_out_mplane = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out_mplane = ipu3_vidioc_g_fmt,
	.vidioc_s_fmt_vid_out_mplane = ipu3_vidioc_s_fmt,
	.vidioc_try_fmt_vid_out_mplane = ipu3_vidioc_try_fmt,

	.vidioc_enum_output = ipu3_vidioc_enum_output,
	.vidioc_g_output = ipu3_vidioc_g_output,
	.vidioc_s_output = ipu3_vidioc_s_output,

	.vidioc_enum_input = ipu3_vidioc_enum_input,
	.vidioc_g_input = ipu3_vidioc_g_input,
	.vidioc_s_input = ipu3_vidioc_s_input,

	/* buffer queue management */
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_expbuf = vb2_ioctl_expbuf,
};

static const struct v4l2_ioctl_ops ipu3_v4l2_meta_ioctl_ops = {
	.vidioc_querycap = ipu3_vidioc_querycap,

	/* meta capture */
	.vidioc_enum_fmt_meta_cap = ipu3_meta_enum_format,
	.vidioc_g_fmt_meta_cap = ipu3_vidioc_g_meta_fmt,
	.vidioc_s_fmt_meta_cap = ipu3_vidioc_g_meta_fmt,
	.vidioc_try_fmt_meta_cap = ipu3_vidioc_g_meta_fmt,

	/* meta output */
	.vidioc_enum_fmt_meta_out = ipu3_meta_enum_format,
	.vidioc_g_fmt_meta_out = ipu3_vidioc_g_meta_fmt,
	.vidioc_s_fmt_meta_out = ipu3_vidioc_g_meta_fmt,
	.vidioc_try_fmt_meta_out = ipu3_vidioc_g_meta_fmt,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_expbuf = vb2_ioctl_expbuf,
};

static int ipu3_sd_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imgu_v4l2_subdev *imgu_sd =
		container_of(ctrl->handler, struct imgu_v4l2_subdev, ctrl_handler);
	struct imgu_device *imgu = v4l2_get_subdevdata(&imgu_sd->subdev);
	struct device *dev = &imgu->pci_dev->dev;

	dev_dbg(dev, "set val %d to ctrl 0x%8x for subdev %d",
		ctrl->val, ctrl->id, imgu_sd->pipe);

	switch (ctrl->id) {
	case V4L2_CID_INTEL_IPU3_MODE:
		atomic_set(&imgu_sd->running_mode, ctrl->val);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops ipu3_subdev_ctrl_ops = {
	.s_ctrl = ipu3_sd_s_ctrl,
};

static const struct v4l2_ctrl_config ipu3_subdev_ctrl_mode = {
	.ops = &ipu3_subdev_ctrl_ops,
	.id = V4L2_CID_INTEL_IPU3_MODE,
	.name = "IPU3 Pipe Mode",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = IPU3_RUNNING_MODE_VIDEO,
	.max = IPU3_RUNNING_MODE_STILL,
	.step = 1,
	.def = IPU3_RUNNING_MODE_VIDEO,
};

/******************** Framework registration ********************/

/* helper function to config node's video properties */
static void ipu3_node_to_v4l2(u32 node, struct video_device *vdev,
			      struct v4l2_format *f)
{
	u32 cap;

	/* Should not happen */
	WARN_ON(node >= IMGU_NODE_NUM);

	switch (node) {
	case IMGU_NODE_IN:
		cap = V4L2_CAP_VIDEO_OUTPUT_MPLANE;
		f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		vdev->ioctl_ops = &ipu3_v4l2_ioctl_ops;
		break;
	case IMGU_NODE_PARAMS:
		cap = V4L2_CAP_META_OUTPUT;
		f->type = V4L2_BUF_TYPE_META_OUTPUT;
		f->fmt.meta.dataformat = V4L2_META_FMT_IPU3_PARAMS;
		vdev->ioctl_ops = &ipu3_v4l2_meta_ioctl_ops;
		ipu3_css_meta_fmt_set(&f->fmt.meta);
		break;
	case IMGU_NODE_STAT_3A:
		cap = V4L2_CAP_META_CAPTURE;
		f->type = V4L2_BUF_TYPE_META_CAPTURE;
		f->fmt.meta.dataformat = V4L2_META_FMT_IPU3_STAT_3A;
		vdev->ioctl_ops = &ipu3_v4l2_meta_ioctl_ops;
		ipu3_css_meta_fmt_set(&f->fmt.meta);
		break;
	default:
		cap = V4L2_CAP_VIDEO_CAPTURE_MPLANE;
		f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		vdev->ioctl_ops = &ipu3_v4l2_ioctl_ops;
	}

	vdev->device_caps = V4L2_CAP_STREAMING | cap;
}

static int ipu3_v4l2_subdev_register(struct imgu_device *imgu,
				     struct imgu_v4l2_subdev *imgu_sd,
				     unsigned int pipe)
{
	int i, r;
	struct v4l2_ctrl_handler *hdl = &imgu_sd->ctrl_handler;
	struct imgu_media_pipe *imgu_pipe = &imgu->imgu_pipe[pipe];

	/* Initialize subdev media entity */
	r = media_entity_init(&imgu_sd->subdev.entity, IMGU_NODE_NUM,
			      imgu_sd->subdev_pads, 0);
	if (r) {
		dev_err(&imgu->pci_dev->dev,
			"failed initialize subdev media entity (%d)\n", r);
		return r;
	}
	imgu_sd->subdev.entity.ops = &ipu3_media_ops;
	for (i = 0; i < IMGU_NODE_NUM; i++) {
		imgu_sd->subdev_pads[i].flags = imgu_pipe->nodes[i].output ?
			MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;
	}

	/* Initialize subdev */
	v4l2_subdev_init(&imgu_sd->subdev, &ipu3_subdev_ops);
	imgu_sd->subdev.internal_ops = &ipu3_subdev_internal_ops;
	imgu_sd->subdev.flags = V4L2_SUBDEV_FL_HAS_DEVNODE |
				V4L2_SUBDEV_FL_HAS_EVENTS;
	snprintf(imgu_sd->subdev.name, sizeof(imgu_sd->subdev.name),
		 "%s %d", IMGU_NAME, pipe);
	v4l2_set_subdevdata(&imgu_sd->subdev, imgu);
	atomic_set(&imgu_sd->running_mode, IPU3_RUNNING_MODE_VIDEO);
	v4l2_ctrl_handler_init(hdl, 1);
	imgu_sd->subdev.ctrl_handler = hdl;
	imgu_sd->ctrl = v4l2_ctrl_new_custom(hdl, &ipu3_subdev_ctrl_mode, NULL);
	if (hdl->error) {
		r = hdl->error;
		dev_err(&imgu->pci_dev->dev,
			"failed to create subdev v4l2 ctrl with err %d", r);
		goto fail_subdev;
	}
	r = v4l2_device_register_subdev(&imgu->v4l2_dev, &imgu_sd->subdev);
	if (r) {
		dev_err(&imgu->pci_dev->dev,
			"failed initialize subdev (%d)\n", r);
		goto fail_subdev;
	}

	imgu_sd->pipe = pipe;
	return 0;

fail_subdev:
	v4l2_ctrl_handler_free(imgu_sd->subdev.ctrl_handler);
	media_entity_cleanup(&imgu_sd->subdev.entity);

	return r;
}

static int ipu3_v4l2_node_setup(struct imgu_device *imgu, unsigned int pipe,
				int node_num)
{
	int r;
	u32 flags;
	struct v4l2_mbus_framefmt def_bus_fmt = { 0 };
	struct v4l2_pix_format_mplane def_pix_fmt = { 0 };
	struct device *dev = &imgu->pci_dev->dev;
	struct imgu_media_pipe *imgu_pipe = &imgu->imgu_pipe[pipe];
	struct v4l2_subdev *sd = &imgu_pipe->imgu_sd.subdev;
	struct imgu_video_device *node = &imgu_pipe->nodes[node_num];
	struct video_device *vdev = &node->vdev;
	struct vb2_queue *vbq = &node->vbq;

	/* Initialize formats to default values */
	def_bus_fmt.width = 1920;
	def_bus_fmt.height = 1080;
	def_bus_fmt.code = MEDIA_BUS_FMT_FIXED;
	def_bus_fmt.field = V4L2_FIELD_NONE;
	def_bus_fmt.colorspace = V4L2_COLORSPACE_RAW;
	def_bus_fmt.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	def_bus_fmt.quantization = V4L2_QUANTIZATION_DEFAULT;
	def_bus_fmt.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	def_pix_fmt.width = def_bus_fmt.width;
	def_pix_fmt.height = def_bus_fmt.height;
	def_pix_fmt.field = def_bus_fmt.field;
	def_pix_fmt.num_planes = 1;
	def_pix_fmt.plane_fmt[0].bytesperline = def_pix_fmt.width * 2;
	def_pix_fmt.plane_fmt[0].sizeimage =
		def_pix_fmt.height * def_pix_fmt.plane_fmt[0].bytesperline;
	def_pix_fmt.flags = 0;
	def_pix_fmt.colorspace = def_bus_fmt.colorspace;
	def_pix_fmt.ycbcr_enc = def_bus_fmt.ycbcr_enc;
	def_pix_fmt.quantization = def_bus_fmt.quantization;
	def_pix_fmt.xfer_func = def_bus_fmt.xfer_func;

	/* Initialize miscellaneous variables */
	mutex_init(&node->lock);
	INIT_LIST_HEAD(&node->buffers);

	/* Initialize formats to default values */
	node->pad_fmt = def_bus_fmt;
	node->id = node_num;
	node->pipe = pipe;
	ipu3_node_to_v4l2(node_num, vdev, &node->vdev_fmt);
	if (node->vdev_fmt.type ==
	    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    node->vdev_fmt.type ==
	    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		def_pix_fmt.pixelformat = node->output ?
			V4L2_PIX_FMT_IPU3_SGRBG10 :
			V4L2_PIX_FMT_NV12;
		node->vdev_fmt.fmt.pix_mp = def_pix_fmt;
	}

	/* Initialize media entities */
	r = media_entity_init(&vdev->entity, 1, &node->vdev_pad, 0);
	if (r) {
		dev_err(dev, "failed initialize media entity (%d)\n", r);
		mutex_destroy(&node->lock);
		return r;
	}
	node->vdev_pad.flags = node->output ?
		MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
	vdev->entity.ops = NULL;

	/* Initialize vbq */
	vbq->type = node->vdev_fmt.type;
	vbq->io_modes = VB2_USERPTR | VB2_MMAP | VB2_DMABUF;
	vbq->ops = &ipu3_vb2_ops;
	vbq->mem_ops = &vb2_dma_sg_memops;
	if (imgu->buf_struct_size <= 0)
		imgu->buf_struct_size =
			sizeof(struct ipu3_vb2_buffer);
	vbq->buf_struct_size = imgu->buf_struct_size;
	vbq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	/* can streamon w/o buffers */
	vbq->min_buffers_needed = 0;
	vbq->drv_priv = imgu;
	vbq->lock = &node->lock;
	r = vb2_queue_init(vbq);
	if (r) {
		dev_err(dev, "failed to initialize video queue (%d)", r);
		media_entity_cleanup(&vdev->entity);
		return r;
	}

	/* Initialize vdev */
	snprintf(vdev->name, sizeof(vdev->name), "%s %d %s",
		 IMGU_NAME, pipe, node->name);
	vdev->release = video_device_release_empty;
	vdev->fops = &ipu3_v4l2_fops;
	vdev->lock = &node->lock;
	vdev->v4l2_dev = &imgu->v4l2_dev;
	vdev->queue = &node->vbq;
	vdev->vfl_dir = node->output ? VFL_DIR_TX : VFL_DIR_RX;
	video_set_drvdata(vdev, imgu);
	r = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (r) {
		dev_err(dev, "failed to register video device (%d)", r);
		media_entity_cleanup(&vdev->entity);
		return r;
	}

	/* Create link between video node and the subdev pad */
	flags = 0;
	if (node->enabled)
		flags |= MEDIA_LNK_FL_ENABLED;
	if (node->output) {
		r = media_entity_create_link(&vdev->entity, 0, &sd->entity,
					     node_num, flags);
	} else {
		r = media_entity_create_link(&sd->entity, node_num, &vdev->entity,
					     0, flags);
	}
	if (r) {
		dev_err(dev, "failed to create pad link (%d)", r);
		video_unregister_device(vdev);
		return r;
	}

	return 0;
}

static void ipu3_v4l2_nodes_cleanup_pipe(struct imgu_device *imgu,
					 unsigned int pipe, int node)
{
	int i;
	struct imgu_media_pipe *imgu_pipe = &imgu->imgu_pipe[pipe];

	for (i = 0; i < node; i++) {
		video_unregister_device(&imgu_pipe->nodes[i].vdev);
		media_entity_cleanup(&imgu_pipe->nodes[i].vdev.entity);
		mutex_destroy(&imgu_pipe->nodes[i].lock);
	}
}

static int ipu3_v4l2_nodes_setup_pipe(struct imgu_device *imgu, int pipe)
{
	int i, r;

	for (i = 0; i < IMGU_NODE_NUM; i++) {
		r = ipu3_v4l2_node_setup(imgu, pipe, i);
		if (r)
			goto cleanup;
	}

	return 0;

cleanup:
	ipu3_v4l2_nodes_cleanup_pipe(imgu, pipe, i);
	return r;
}

static void ipu3_v4l2_subdev_cleanup(struct imgu_device *imgu, unsigned int i)
{
	struct imgu_media_pipe *imgu_pipe = &imgu->imgu_pipe[i];

	v4l2_device_unregister_subdev(&imgu_pipe->imgu_sd.subdev);
	v4l2_ctrl_handler_free(imgu_pipe->imgu_sd.subdev.ctrl_handler);
	media_entity_cleanup(&imgu_pipe->imgu_sd.subdev.entity);
}

static void ipu3_v4l2_cleanup_pipes(struct imgu_device *imgu, unsigned int pipe)
{
	int i;

	for (i = 0; i < pipe; i++) {
		ipu3_v4l2_nodes_cleanup_pipe(imgu, i, IMGU_NODE_NUM);
		ipu3_v4l2_subdev_cleanup(imgu, i);
	}
}

static int ipu3_v4l2_register_pipes(struct imgu_device *imgu)
{
	struct imgu_media_pipe *imgu_pipe;
	int i, r;

	for (i = 0; i < IMGU_MAX_PIPE_NUM; i++) {
		imgu_pipe = &imgu->imgu_pipe[i];
		r = ipu3_v4l2_subdev_register(imgu, &imgu_pipe->imgu_sd, i);
		if (r) {
			dev_err(&imgu->pci_dev->dev,
				"failed to register subdev%d ret (%d)\n", i, r);
			goto pipes_cleanup;
		}
		r = ipu3_v4l2_nodes_setup_pipe(imgu, i);
		if (r) {
			ipu3_v4l2_subdev_cleanup(imgu, i);
			goto pipes_cleanup;
		}
	}

	return 0;

pipes_cleanup:
	ipu3_v4l2_cleanup_pipes(imgu, i);
	return r;
}

int ipu3_v4l2_register(struct imgu_device *imgu)
{
	int r;

	/* Initialize miscellaneous variables */
	imgu->streaming = false;

	/* Set up media device */
	imgu->media_dev.dev = &imgu->pci_dev->dev;
	strscpy(imgu->media_dev.model, IMGU_NAME,
		sizeof(imgu->media_dev.model));
	snprintf(imgu->media_dev.bus_info, sizeof(imgu->media_dev.bus_info),
		 "%s", dev_name(&imgu->pci_dev->dev));
	imgu->media_dev.driver_version = LINUX_VERSION_CODE;
	imgu->media_dev.hw_revision = 0;
	r = media_device_register(&imgu->media_dev);
	if (r) {
		dev_err(&imgu->pci_dev->dev,
			"failed to register media device (%d)\n", r);
		return r;
	}

	/* Set up v4l2 device */
	imgu->v4l2_dev.mdev = &imgu->media_dev;
	imgu->v4l2_dev.ctrl_handler = NULL;
	r = v4l2_device_register(&imgu->pci_dev->dev, &imgu->v4l2_dev);
	if (r) {
		dev_err(&imgu->pci_dev->dev,
			"failed to register V4L2 device (%d)\n", r);
		goto fail_v4l2_dev;
	}

	r = ipu3_v4l2_register_pipes(imgu);
	if (r) {
		dev_err(&imgu->pci_dev->dev,
			"failed to register pipes (%d)\n", r);
		goto fail_v4l2_pipes;
	}

	r = v4l2_device_register_subdev_nodes(&imgu->v4l2_dev);
	if (r) {
		dev_err(&imgu->pci_dev->dev,
			"failed to register subdevs (%d)\n", r);
		goto fail_subdevs;
	}

	return 0;

fail_subdevs:
	ipu3_v4l2_cleanup_pipes(imgu, IMGU_MAX_PIPE_NUM);
fail_v4l2_pipes:
	v4l2_device_unregister(&imgu->v4l2_dev);
fail_v4l2_dev:
	media_device_unregister(&imgu->media_dev);

	return r;
}
EXPORT_SYMBOL_GPL(ipu3_v4l2_register);

int ipu3_v4l2_unregister(struct imgu_device *imgu)
{
	media_device_unregister(&imgu->media_dev);
	ipu3_v4l2_cleanup_pipes(imgu, IMGU_MAX_PIPE_NUM);
	v4l2_device_unregister(&imgu->v4l2_dev);

	return 0;
}
EXPORT_SYMBOL_GPL(ipu3_v4l2_unregister);

void ipu3_v4l2_buffer_done(struct vb2_buffer *vb,
			   enum vb2_buffer_state state)
{
	struct ipu3_vb2_buffer *b =
		container_of(vb, struct ipu3_vb2_buffer, vbb.vb2_buf);

	list_del(&b->list);
	vb2_buffer_done(&b->vbb.vb2_buf, state);
}
EXPORT_SYMBOL_GPL(ipu3_v4l2_buffer_done);
