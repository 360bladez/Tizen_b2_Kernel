/* exynos_drm_crtc.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * Authors:
 *	Inki Dae <inki.dae@samsung.com>
 *	Joonyoung Shim <jy0922.shim@samsung.com>
 *	Seung-Woo Kim <sw0312.kim@samsung.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "drmP.h"
#include "drm_crtc_helper.h"

#include "exynos_drm_crtc.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_encoder.h"
#include "exynos_drm_plane.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_gem.h"

#include <linux/dmabuf-sync.h>

/* undefine it if Xorg supports no-vblank mode. */
#define XORG_VBLANK_MODE

#define to_exynos_crtc(x)	container_of(x, struct exynos_drm_crtc,\
				drm_crtc)

enum exynos_crtc_mode {
	CRTC_MODE_NORMAL,	/* normal mode */
	CRTC_MODE_BLANK,	/* The private plane of crtc is blank */
};

/*
 * Exynos specific crtc structure.
 *
 * @drm_crtc: crtc object.
 * @drm_plane: pointer of private plane object for this crtc
 * @pipe: a crtc index created at load() with a new crtc object creation
 *	and the crtc object would be set to private->crtc array
 *	to get a crtc object corresponding to this pipe from private->crtc
 *	array when irq interrupt occured. the reason of using this pipe is that
 *	drm framework doesn't support multiple irq yet.
 *	we can refer to the crtc to current hardware interrupt occured through
 *	this pipe value.
 * @dpms: store the crtc dpms value
 * @mode: store the crtc mode value
 */
struct exynos_drm_crtc {
	struct drm_crtc			drm_crtc;
	struct drm_plane		*plane;
	unsigned int			pipe;
	unsigned int			dpms;
	enum exynos_crtc_mode		mode;
	wait_queue_head_t		pending_flip_queue;
	atomic_t			pending_flip;
	struct list_head		sync_committed;
	atomic_t			partial_mode;
};

static void exynos_drm_dmabuf_sync_free(void *priv)
{
	struct drm_pending_vblank_event *event = priv;

	event->event.reserved = 0;
}

static struct dmabuf_sync_priv_ops dmabuf_sync_ops = {
	.free	= exynos_drm_dmabuf_sync_free,
};

static void exynos_drm_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	if (exynos_crtc->dpms == mode) {
		DRM_DEBUG_KMS("desired dpms mode is same as previous one.\n");
		return;
	}

	DRM_INFO("%s:crtc[%d] mode[%d]\n", __func__, crtc->base.id, mode);

	if (mode > DRM_MODE_DPMS_ON) {
		/* wait for the completion of page flip. */
		wait_event_timeout(exynos_crtc->pending_flip_queue,
				!atomic_read(&exynos_crtc->pending_flip),
				DRM_HZ/20);

		drm_vblank_off(crtc->dev, exynos_crtc->pipe);
	}

	exynos_drm_fn_encoder(crtc, &mode, exynos_drm_encoder_crtc_dpms);
	exynos_crtc->dpms = mode;
}

static void exynos_drm_crtc_prepare(struct drm_crtc *crtc)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);

	/* drm framework doesn't check NULL. */
}

static void exynos_drm_crtc_commit(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	exynos_drm_crtc_dpms(crtc, DRM_MODE_DPMS_ON);
	exynos_plane_commit(exynos_crtc->plane);
	exynos_plane_dpms(exynos_crtc->plane, DRM_MODE_DPMS_ON);
}

static bool
exynos_drm_crtc_mode_fixup(struct drm_crtc *crtc,
			    struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);

	/* drm framework doesn't check NULL */
	return true;
}

static int
exynos_drm_crtc_mode_set(struct drm_crtc *crtc, struct drm_display_mode *mode,
			  struct drm_display_mode *adjusted_mode, int x, int y,
			  struct drm_framebuffer *old_fb)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_plane *plane = exynos_crtc->plane;
	struct dmabuf_sync *sync;
	unsigned int crtc_w;
	unsigned int crtc_h;
	int pipe = exynos_crtc->pipe;
	int ret;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	/* Change it to full screen mode if necessary. */
	change_to_full_screen_mode(crtc, crtc->fb);

	/*
	 * copy the mode data adjusted by mode_fixup() into crtc->mode
	 * so that hardware can be seet to proper mode.
	 */
	memcpy(&crtc->mode, adjusted_mode, sizeof(*adjusted_mode));

	crtc_w = crtc->fb->width - x;
	crtc_h = crtc->fb->height - y;

	ret = exynos_plane_mode_set(plane, crtc, crtc->fb, 0, 0, crtc_w, crtc_h,
				    x, y, crtc_w, crtc_h);
	if (ret)
		return ret;

	plane->crtc = crtc;
	plane->fb = crtc->fb;

	exynos_drm_fn_encoder(crtc, &pipe, exynos_drm_encoder_crtc_pipe);

	if (!dmabuf_sync_is_supported())
		return 0;

	sync = (struct dmabuf_sync *)exynos_drm_dmabuf_sync_work(crtc->fb);
	if (IS_ERR(sync)) {
		/* just ignore buffer synchronization this time. */
		return 0;
	}

	return 0;
}

static int exynos_drm_crtc_mode_set_commit(struct drm_crtc *crtc, int x, int y,
					  struct drm_framebuffer *old_fb)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_plane *plane = exynos_crtc->plane;
	unsigned int crtc_w;
	unsigned int crtc_h;
	int ret;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	crtc_w = crtc->fb->width - x;
	crtc_h = crtc->fb->height - y;

	ret = exynos_plane_mode_set(plane, crtc, crtc->fb, 0, 0, crtc_w, crtc_h,
				    x, y, crtc_w, crtc_h);
	if (ret)
		return ret;

	exynos_drm_crtc_commit(crtc);

	return 0;
}

static int exynos_drm_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
					  struct drm_framebuffer *old_fb)
{
	struct dmabuf_sync *sync;
	int ret;

	/* Change it to full screen mode if necessary. */
	change_to_full_screen_mode(crtc, crtc->fb);

	ret = exynos_drm_crtc_mode_set_commit(crtc, x, y, old_fb);
	if (ret < 0)
		return ret;

	if (!dmabuf_sync_is_supported())
		return 0;

	sync = (struct dmabuf_sync *)exynos_drm_dmabuf_sync_work(crtc->fb);
	if (IS_ERR(sync)) {
		WARN_ON(1);
		/* just ignore buffer synchronization this time. */
		return 0;
	}

	return 0;
}

static void exynos_drm_crtc_load_lut(struct drm_crtc *crtc)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);
	/* drm framework doesn't check NULL */
}

static void exynos_drm_crtc_disable(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	exynos_plane_dpms(exynos_crtc->plane, DRM_MODE_DPMS_OFF);
	exynos_drm_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);
}

static struct drm_crtc_helper_funcs exynos_crtc_helper_funcs = {
	.dpms		= exynos_drm_crtc_dpms,
	.prepare	= exynos_drm_crtc_prepare,
	.commit		= exynos_drm_crtc_commit,
	.mode_fixup	= exynos_drm_crtc_mode_fixup,
	.mode_set	= exynos_drm_crtc_mode_set,
	.mode_set_base	= exynos_drm_crtc_mode_set_base,
	.load_lut	= exynos_drm_crtc_load_lut,
	.disable	= exynos_drm_crtc_disable,
};

void exynos_drm_crtc_set_partial_update(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_plane *plane = exynos_crtc->plane;

	exynos_plane_request_partial_update(plane);
}

static void exynos_drm_crtc_adjust_partial_region(struct drm_crtc *crtc,
					struct exynos_drm_partial_pos *pos)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_plane *plane = exynos_crtc->plane;

	exynos_plane_adjust_partial_region(plane, pos);
}

static int exynos_drm_crtc_page_flip(struct drm_crtc *crtc,
				      struct drm_framebuffer *fb,
				      struct drm_pending_vblank_event *event)
{
	struct drm_device *dev = crtc->dev;
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_framebuffer *old_fb = crtc->fb;
	int ret = -EINVAL;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (event) {
		struct exynos_drm_fb *exynos_fb;
		struct drm_gem_object *obj;
		struct dmabuf_sync *sync;
		unsigned int i;

		mutex_lock(&dev->struct_mutex);

		/*
		 * the pipe from user always is 0 so we can set pipe number
		 * of current owner to event.
		 */
		event->pipe = exynos_crtc->pipe;

		ret = drm_vblank_get(dev, exynos_crtc->pipe);
		if (ret) {
			DRM_DEBUG("failed to acquire vblank counter\n");
			mutex_unlock(&dev->struct_mutex);
			return ret;
		}

		if (!dmabuf_sync_is_supported())
			goto out_fence;

		sync = dmabuf_sync_init("DRM", &dmabuf_sync_ops, event);
		if (IS_ERR(sync)) {
			WARN_ON(1);
			goto out_fence;
		}

		exynos_fb = to_exynos_fb(fb);

		for (i = 0; i < exynos_fb->buf_cnt; i++) {
			if (!exynos_fb->exynos_gem_obj[i]) {
				WARN_ON(1);
				continue;
			}

			obj = &exynos_fb->exynos_gem_obj[i]->base;
			if (!obj->export_dma_buf) {
				WARN_ON(1);
				continue;
			}

			/*
			 * set dmabuf to fence and registers reservation
			 * object to reservation entry.
			 */
			ret = dmabuf_sync_get(sync,
					obj->export_dma_buf,
					DMA_BUF_ACCESS_DMA_R);
			if (WARN_ON(ret < 0))
				continue;
		}

		ret = dmabuf_sync_lock(sync);
		if (ret < 0) {
			WARN_ON(1);
			dmabuf_sync_put_all(sync);
			dmabuf_sync_fini(sync);
			goto out_fence;
		}

		event->event.reserved = (unsigned long)sync;

#if defined(XORG_VBLANK_MODE)
		/*
		 * workaround - remove it in case that Xorg supports
		 * no-vblank mode.
		 */
		ret = dmabuf_sync_unlock(sync);
		if (ret < 0)
			WARN_ON(1);

		dmabuf_sync_put_all(sync);
		dmabuf_sync_fini(sync);
#endif

out_fence:

		spin_lock_irq(&dev->event_lock);
		list_add_tail(&event->base.link,
				&dev_priv->pageflip_event_list);
		atomic_set(&exynos_crtc->pending_flip, 1);
		spin_unlock_irq(&dev->event_lock);

		crtc->fb = fb;

		if (check_fb_partial_update(fb)) {
			struct exynos_drm_partial_pos *pos;

			pos = get_partial_pos(fb);

			/*
			 * Change it to full screen mode if one more planes
			 * enabled exist.
			 *
			 * Resolution of display panel and crtc device dma
			 * should be full screen mode if two more planes
			 * (hw overlay) are being used because the resolution
			 * affects all planes.
			 */
			if (get_planes_cnt_enabled(dev)) {
				pos->x = 0;
				pos->y = 0;
				/*
				 * TODO. isn't it more reasonable to use
				 * connector's resolution value instead?
				 */
				pos->w = crtc->mode.hdisplay;
				pos->h = crtc->mode.vdisplay;
			} else {
				/* Adjust pos according to hw limitation. */
				exynos_drm_crtc_adjust_partial_region(crtc,
									pos);
				atomic_set(&exynos_crtc->partial_mode, 1);
			}

			exynos_drm_crtc_set_partial_update(crtc);

			mutex_unlock(&dev->struct_mutex);

			return ret;
		}

		/* Change it to full screen mode if necessary. */
		change_to_full_screen_mode(crtc, fb);

		ret = exynos_drm_crtc_mode_set_commit(crtc, crtc->x, crtc->y,
						    NULL);
		if (ret) {
			crtc->fb = old_fb;

			spin_lock_irq(&dev->event_lock);
			drm_vblank_put(dev, exynos_crtc->pipe);
			list_del(&event->base.link);
			spin_unlock_irq(&dev->event_lock);

			mutex_unlock(&dev->struct_mutex);
			return ret;
		}

		mutex_unlock(&dev->struct_mutex);
	}

	return ret;
}

static void exynos_drm_crtc_destroy(struct drm_crtc *crtc)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct exynos_drm_private *private = crtc->dev->dev_private;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	private->crtc[exynos_crtc->pipe] = NULL;

	drm_crtc_cleanup(crtc);
	kfree(exynos_crtc);
}

static int exynos_drm_crtc_set_property(struct drm_crtc *crtc,
					struct drm_property *property,
					uint64_t val)
{
	struct drm_device *dev = crtc->dev;
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	DRM_DEBUG_KMS("%s\n", __func__);

	if (dev_priv->crtc_mode_property == property) {
		enum exynos_crtc_mode mode = val;

		if (exynos_crtc->mode == mode)
			return 0;

		exynos_crtc->mode = mode;

		switch (mode) {
		case CRTC_MODE_NORMAL:
			exynos_drm_crtc_commit(crtc);
			break;
		case CRTC_MODE_BLANK:
			exynos_plane_dpms(exynos_crtc->plane,
					  DRM_MODE_DPMS_OFF);
			break;
		default:
			break;
		}

		return 0;
	}

	return -EINVAL;
}

static struct drm_crtc_funcs exynos_crtc_funcs = {
	.set_config	= drm_crtc_helper_set_config,
	.page_flip	= exynos_drm_crtc_page_flip,
	.destroy	= exynos_drm_crtc_destroy,
	.set_property	= exynos_drm_crtc_set_property,
};

static const struct drm_prop_enum_list mode_names[] = {
	{ CRTC_MODE_NORMAL, "normal" },
	{ CRTC_MODE_BLANK, "blank" },
};

static void exynos_drm_crtc_attach_property(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct drm_property *prop;

	DRM_DEBUG_KMS("%s\n", __func__);

	prop = dev_priv->crtc_mode_property;
	if (!prop) {
		prop = drm_property_create_enum(dev, 0, "mode", mode_names,
						ARRAY_SIZE(mode_names));
		if (!prop) {
			DRM_ERROR("failed to create mode property.\n");
			return;
		}

		dev_priv->crtc_mode_property = prop;
	}

	drm_object_attach_property(&crtc->base, prop, 0);
}

int exynos_drm_crtc_create(struct drm_device *dev, unsigned int nr)
{
	struct exynos_drm_crtc *exynos_crtc;
	struct exynos_drm_private *private = dev->dev_private;
	struct drm_crtc *crtc;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	exynos_crtc = kzalloc(sizeof(*exynos_crtc), GFP_KERNEL);
	if (!exynos_crtc) {
		DRM_ERROR("failed to allocate exynos crtc\n");
		return -ENOMEM;
	}

	exynos_crtc->pipe = nr;
	exynos_crtc->dpms = DRM_MODE_DPMS_OFF;
	init_waitqueue_head(&exynos_crtc->pending_flip_queue);
	atomic_set(&exynos_crtc->pending_flip, 0);
	exynos_crtc->plane = exynos_plane_init(dev, 1 << nr, true);
	if (!exynos_crtc->plane) {
		kfree(exynos_crtc);
		return -ENOMEM;
	}

#if !defined(XORG_VBLANK_MODE)
	INIT_LIST_HEAD(&exynos_crtc->sync_committed);
#endif

	crtc = &exynos_crtc->drm_crtc;

	private->crtc[nr] = crtc;

	drm_crtc_init(dev, crtc, &exynos_crtc_funcs);
	drm_crtc_helper_add(crtc, &exynos_crtc_helper_funcs);

	exynos_drm_crtc_attach_property(crtc);

	return 0;
}

void exynos_drm_crtc_prepare_vblank(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *private = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc =
		to_exynos_crtc(private->crtc[crtc]);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (exynos_crtc->dpms != DRM_MODE_DPMS_ON)
		return;

	exynos_drm_fn_encoder(private->crtc[crtc], &crtc,
				exynos_drm_prepare_vblank);
}

int exynos_drm_crtc_enable_vblank(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *private = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc =
		to_exynos_crtc(private->crtc[crtc]);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (exynos_crtc->dpms != DRM_MODE_DPMS_ON)
		return -EPERM;

	exynos_drm_fn_encoder(private->crtc[crtc], &crtc,
				exynos_drm_enable_vblank);

	return 0;
}

void exynos_drm_crtc_disable_vblank(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *private = dev->dev_private;
	struct exynos_drm_crtc *exynos_crtc =
		to_exynos_crtc(private->crtc[crtc]);

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (exynos_crtc->dpms != DRM_MODE_DPMS_ON)
		return;

	exynos_drm_fn_encoder(private->crtc[crtc], &crtc,
				exynos_drm_disable_vblank);
}

void exynos_drm_crtc_finish_pageflip(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct drm_pending_vblank_event *e, *t;
	struct drm_crtc *drm_crtc = dev_priv->crtc[crtc];
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(drm_crtc);
	unsigned long flags;

	DRM_DEBUG_KMS("%s\n", __FILE__);

#if !defined(XORG_VBLANK_MODE)
	if (!list_empty(&exynos_crtc->sync_committed) &&
			dmabuf_sync_is_supported()) {
		struct dmabuf_sync *sync;

		sync = list_first_entry(&exynos_crtc->sync_committed,
					struct dmabuf_sync, list);
		if (!dmabuf_sync_unlock(sync)) {
			list_del_init(&sync->list);
			dmabuf_sync_put_all(sync);
			dmabuf_sync_fini(sync);
		}
	}
#endif

	spin_lock_irqsave(&dev->event_lock, flags);

	list_for_each_entry_safe(e, t, &dev_priv->pageflip_event_list,
			base.link) {

		/* if event's pipe isn't same as crtc then ignore it. */
		if (crtc != e->pipe)
			continue;

#if !defined(XORG_VBLANK_MODE)
		if (e->event.reserved && dmabuf_sync_is_supported()) {
			struct dmabuf_sync *sync;

			sync = (struct dmabuf_sync *)e->event.reserved;
			e->event.reserved = 0;
			list_add_tail(&sync->list,
					&exynos_crtc->sync_committed);
		}
#endif

		list_del(&e->base.link);
		drm_send_vblank_event(dev, -1, e);
		drm_vblank_put(dev, crtc);
		atomic_set(&exynos_crtc->pending_flip, 0);
		wake_up(&exynos_crtc->pending_flip_queue);
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

int exynos_drm_get_pendingflip(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct drm_crtc *drm_crtc = dev_priv->crtc[crtc];
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(drm_crtc);

	return atomic_read(&exynos_crtc->pending_flip);
}

void exynos_drm_wait_finish_pageflip(struct drm_device *dev, int crtc)
{
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct drm_crtc *drm_crtc = dev_priv->crtc[crtc];
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(drm_crtc);

	if (atomic_read(&exynos_crtc->pending_flip))
		DRM_INFO("%s:wait\n", __func__);

	/* wait for the completion of page flip. */
	wait_event_timeout(exynos_crtc->pending_flip_queue,
			!atomic_read(&exynos_crtc->pending_flip),
			DRM_HZ/20);

	DRM_INFO("%s:done\n", __func__);
}

void change_to_full_screen_mode(struct drm_crtc *crtc,
					struct drm_framebuffer *fb)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);

	/*
	 * Change it to full screen mode
	 * if previous mode of this crtc device was partial one.
	 */
	if (atomic_read(&exynos_crtc->partial_mode)) {
		struct exynos_drm_partial_pos *pos;

		pos = get_partial_pos(fb);

		pos->x = 0;
		pos->y = 0;
		/*
		 * TODO. isn't it more reasonable to use
		 * connector's resolution value instead?
		 */
		pos->w = crtc->mode.hdisplay;
		pos->h = crtc->mode.vdisplay;

		DRM_INFO("%s:update_partial_region\n", __func__);

		update_partial_region(crtc, pos);

		atomic_set(&exynos_crtc->partial_mode, 0);
	}
}

void request_crtc_partial_update(struct drm_device *dev, int pipe)
{
	struct exynos_drm_private *dev_priv = dev->dev_private;
	struct drm_crtc *crtc = dev_priv->crtc[pipe];

	exynos_drm_fb_request_partial_update(crtc, crtc->fb);
}

int exynos_drm_crtc_set_partial_region(struct drm_crtc *crtc,
					struct exynos_drm_partial_pos *pos)
{
	struct exynos_drm_crtc *exynos_crtc = to_exynos_crtc(crtc);
	struct drm_plane *plane = exynos_crtc->plane;
	int ret;

	ret = exynos_plane_mode_set(plane, crtc, crtc->fb, 0, 0,
					pos->w, pos->h, pos->x, pos->y,
					pos->w, pos->h);
	if (ret)
		return ret;

	plane->crtc = crtc;
	plane->fb = crtc->fb;

	exynos_plane_commit(plane);

	exynos_plane_partial_resolution(plane, pos);

	return 0;
}
