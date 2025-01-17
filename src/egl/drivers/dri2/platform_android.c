/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Based on platform_x11, which has
 *
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <stdbool.h>

#if ANDROID_VERSION >= 0x402
#include <sync/sync.h>
#endif

#include "loader.h"
#include "egl_dri2.h"
#include "egl_dri2_fallbacks.h"
#include "state_tracker/drm_driver.h"
#include "gralloc_drm.h"
#include "gralloc_drm_handle.h"

#define ALIGN(val, align)	(((val) + (align) - 1) & ~((align) - 1))

struct droid_yuv_format {
   /* Lookup keys */
   int native; /* HAL_PIXEL_FORMAT_ */
   int is_ycrcb; /* 0 if chroma order is {Cb, Cr}, 1 if {Cr, Cb} */
   int chroma_step; /* Distance in bytes between subsequent chroma pixels. */

   /* Result */
   int fourcc; /* __DRI_IMAGE_FOURCC_ */
};

/* The following table is used to look up a DRI image FourCC based
 * on native format and information contained in android_ycbcr struct. */
static const struct droid_yuv_format droid_yuv_formats[] = {
   /* Native format, YCrCb, Chroma step, DRI image FourCC */
   { HAL_PIXEL_FORMAT_YCbCr_420_888,   0, 2, __DRI_IMAGE_FOURCC_NV12 },
   { HAL_PIXEL_FORMAT_YCbCr_420_888,   0, 1, __DRI_IMAGE_FOURCC_YUV420 },
   { HAL_PIXEL_FORMAT_YCbCr_420_888,   1, 1, __DRI_IMAGE_FOURCC_YVU420 },
   { HAL_PIXEL_FORMAT_YV12,            1, 1, __DRI_IMAGE_FOURCC_YVU420 },
};

static int
get_fourcc_yuv(int native, int is_ycrcb, int chroma_step)
{
   int i;

   for (i = 0; i < ARRAY_SIZE(droid_yuv_formats); ++i)
      if (droid_yuv_formats[i].native == native &&
          droid_yuv_formats[i].is_ycrcb == is_ycrcb &&
          droid_yuv_formats[i].chroma_step == chroma_step)
         return droid_yuv_formats[i].fourcc;

   return -1;
}

static bool
is_yuv(int native)
{
   int i;

   for (i = 0; i < ARRAY_SIZE(droid_yuv_formats); ++i)
      if (droid_yuv_formats[i].native == native)
         return true;

   return false;
}

static int
get_format_bpp(int native)
{
   int bpp;

   switch (native) {
   case HAL_PIXEL_FORMAT_RGBA_8888:
   case HAL_PIXEL_FORMAT_RGBX_8888:
   case HAL_PIXEL_FORMAT_BGRA_8888:
      bpp = 4;
      break;
   case HAL_PIXEL_FORMAT_RGB_565:
      bpp = 2;
      break;
   default:
      bpp = 0;
      break;
   }

   return bpp;
}

/* createImageFromFds requires fourcc format */
static int get_fourcc(int native)
{
   switch (native) {
   case HAL_PIXEL_FORMAT_RGB_565:   return __DRI_IMAGE_FOURCC_RGB565;
   case HAL_PIXEL_FORMAT_BGRA_8888: return __DRI_IMAGE_FOURCC_ARGB8888;
   case HAL_PIXEL_FORMAT_RGBA_8888: return __DRI_IMAGE_FOURCC_ABGR8888;
   case HAL_PIXEL_FORMAT_RGBX_8888: return __DRI_IMAGE_FOURCC_XBGR8888;
   default:
      _eglLog(_EGL_WARNING, "unsupported native buffer format 0x%x", native);
   }
   return -1;
}

static int get_format(int format)
{
   switch (format) {
   case HAL_PIXEL_FORMAT_BGRA_8888: return __DRI_IMAGE_FORMAT_ARGB8888;
   case HAL_PIXEL_FORMAT_RGB_565:   return __DRI_IMAGE_FORMAT_RGB565;
   case HAL_PIXEL_FORMAT_RGBA_8888: return __DRI_IMAGE_FORMAT_ABGR8888;
   case HAL_PIXEL_FORMAT_RGBX_8888: return __DRI_IMAGE_FORMAT_XBGR8888;
   default:
      _eglLog(_EGL_WARNING, "unsupported native buffer format 0x%x", format);
   }
   return -1;
}

static int
get_native_buffer_fd(struct ANativeWindowBuffer *buf)
{
   native_handle_t *handle = (native_handle_t *)buf->handle;
   /*
    * Various gralloc implementations exist, but the dma-buf fd tends
    * to be first. Access it directly to avoid a dependency on specific
    * gralloc versions.
    */
   return (handle && handle->numFds) ? handle->data[0] : -1;
}

static int
get_native_buffer_name(struct ANativeWindowBuffer *buf)
{
   struct gralloc_drm_handle_t *handle = gralloc_drm_handle(buf->handle);
   return (handle) ? handle->name : 0;
}

static const gralloc_module_t *gr_module = NULL;

static EGLBoolean
droid_window_dequeue_buffer(struct dri2_egl_surface *dri2_surf)
{
#if ANDROID_VERSION >= 0x0402
   int fence_fd;

   if (dri2_surf->window->dequeueBuffer(dri2_surf->window, &dri2_surf->buffer,
                                        &fence_fd))
      return EGL_FALSE;

   /* If access to the buffer is controlled by a sync fence, then block on the
    * fence.
    *
    * It may be more performant to postpone blocking until there is an
    * immediate need to write to the buffer. But doing so would require adding
    * hooks to the DRI2 loader.
    *
    * From the ANativeWindow::dequeueBuffer documentation:
    *
    *    The libsync fence file descriptor returned in the int pointed to by
    *    the fenceFd argument will refer to the fence that must signal
    *    before the dequeued buffer may be written to.  A value of -1
    *    indicates that the caller may access the buffer immediately without
    *    waiting on a fence.  If a valid file descriptor is returned (i.e.
    *    any value except -1) then the caller is responsible for closing the
    *    file descriptor.
    */
    if (fence_fd >= 0) {
       /* From the SYNC_IOC_WAIT documentation in <linux/sync.h>:
        *
        *    Waits indefinitely if timeout < 0.
        */
        int timeout = -1;
        sync_wait(fence_fd, timeout);
        close(fence_fd);
   }

   dri2_surf->buffer->common.incRef(&dri2_surf->buffer->common);
#else
   if (dri2_surf->window->dequeueBuffer(dri2_surf->window, &dri2_surf->buffer))
      return EGL_FALSE;

   dri2_surf->buffer->common.incRef(&dri2_surf->buffer->common);
   dri2_surf->window->lockBuffer(dri2_surf->window, dri2_surf->buffer);
#endif

   return EGL_TRUE;
}

static EGLBoolean
droid_window_enqueue_buffer(_EGLDisplay *disp, struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   /* To avoid blocking other EGL calls, release the display mutex before
    * we enter droid_window_enqueue_buffer() and re-acquire the mutex upon
    * return.
    */
   mtx_unlock(&disp->Mutex);

#if ANDROID_VERSION >= 0x0402
   /* Queue the buffer without a sync fence. This informs the ANativeWindow
    * that it may access the buffer immediately.
    *
    * From ANativeWindow::dequeueBuffer:
    *
    *    The fenceFd argument specifies a libsync fence file descriptor for
    *    a fence that must signal before the buffer can be accessed.  If
    *    the buffer can be accessed immediately then a value of -1 should
    *    be used.  The caller must not use the file descriptor after it
    *    is passed to queueBuffer, and the ANativeWindow implementation
    *    is responsible for closing it.
    */
   int fence_fd = -1;
   dri2_surf->window->queueBuffer(dri2_surf->window, dri2_surf->buffer,
                                  fence_fd);
#else
   dri2_surf->window->queueBuffer(dri2_surf->window, dri2_surf->buffer);
#endif

   dri2_surf->buffer->common.decRef(&dri2_surf->buffer->common);
   dri2_surf->buffer = NULL;

   mtx_lock(&disp->Mutex);

   if (dri2_surf->dri_image_back) {
      dri2_dpy->image->destroyImage(dri2_surf->dri_image_back);
      dri2_surf->dri_image_back = NULL;
   }

   return EGL_TRUE;
}

static void
droid_window_cancel_buffer(_EGLDisplay *disp, struct dri2_egl_surface *dri2_surf)
{
   /* no cancel buffer? */
   droid_window_enqueue_buffer(disp, dri2_surf);
}

static __DRIbuffer *
droid_alloc_local_buffer(struct dri2_egl_surface *dri2_surf,
                         unsigned int att, unsigned int format)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (att >= ARRAY_SIZE(dri2_surf->local_buffers))
      return NULL;

   if (!dri2_surf->local_buffers[att]) {
      dri2_surf->local_buffers[att] =
         dri2_dpy->dri2->allocateBuffer(dri2_dpy->dri_screen, att, format,
               dri2_surf->base.Width, dri2_surf->base.Height);
   }

   return dri2_surf->local_buffers[att];
}

static void
droid_free_local_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->local_buffers); i++) {
      if (dri2_surf->local_buffers[i]) {
         dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
               dri2_surf->local_buffers[i]);
         dri2_surf->local_buffers[i] = NULL;
      }
   }
}

static _EGLSurface *
droid_create_surface(_EGLDriver *drv, _EGLDisplay *disp, EGLint type,
		    _EGLConfig *conf, void *native_window,
		    const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   struct ANativeWindow *window = native_window;
   const __DRIconfig *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "droid_create_surface");
      return NULL;
   }

   if (!_eglInitSurface(&dri2_surf->base, disp, type, conf, attrib_list))
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      int format;

      if (!window || window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "droid_create_surface");
         goto cleanup_surface;
      }
      if (window->query(window, NATIVE_WINDOW_FORMAT, &format)) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "droid_create_surface");
         goto cleanup_surface;
      }

      if (format != dri2_conf->base.NativeVisualID) {
         _eglLog(_EGL_WARNING, "Native format mismatch: 0x%x != 0x%x",
               format, dri2_conf->base.NativeVisualID);
      }

      window->query(window, NATIVE_WINDOW_WIDTH, &dri2_surf->base.Width);
      window->query(window, NATIVE_WINDOW_HEIGHT, &dri2_surf->base.Height);
   }

   config = dri2_get_dri_config(dri2_conf, type,
                                dri2_surf->base.GLColorspace);
   if (!config)
      goto cleanup_surface;

   if (dri2_dpy->dri2) {
      dri2_surf->dri_drawable =
         dri2_dpy->dri2->createNewDrawable(dri2_dpy->dri_screen, config, dri2_surf);
   } else {
      dri2_surf->dri_drawable =
         dri2_dpy->swrast->createNewDrawable(dri2_dpy->dri_screen, config, dri2_surf);
   }

   if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2->createNewDrawable");
      goto cleanup_surface;
   }

   if (window) {
      window->common.incRef(&window->common);
      dri2_surf->window = window;
   }

   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);

   return NULL;
}

static _EGLSurface *
droid_create_window_surface(_EGLDriver *drv, _EGLDisplay *disp,
                            _EGLConfig *conf, void *native_window,
                            const EGLint *attrib_list)
{
   return droid_create_surface(drv, disp, EGL_WINDOW_BIT, conf,
                               native_window, attrib_list);
}

static _EGLSurface *
droid_create_pbuffer_surface(_EGLDriver *drv, _EGLDisplay *disp,
			    _EGLConfig *conf, const EGLint *attrib_list)
{
   return droid_create_surface(drv, disp, EGL_PBUFFER_BIT, conf,
			      NULL, attrib_list);
}

static EGLBoolean
droid_destroy_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   droid_free_local_buffers(dri2_surf);

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (dri2_surf->buffer)
         droid_window_cancel_buffer(disp, dri2_surf);

      dri2_surf->window->common.decRef(&dri2_surf->window->common);
   }

   if (dri2_surf->dri_image_back) {
      _eglLog(_EGL_DEBUG, "%s : %d : destroy dri_image_back", __func__, __LINE__);
      dri2_dpy->image->destroyImage(dri2_surf->dri_image_back);
      dri2_surf->dri_image_back = NULL;
   }

   if (dri2_surf->dri_image_front) {
      _eglLog(_EGL_DEBUG, "%s : %d : destroy dri_image_front", __func__, __LINE__);
      dri2_dpy->image->destroyImage(dri2_surf->dri_image_front);
      dri2_surf->dri_image_front = NULL;
   }

   (*dri2_dpy->core->destroyDrawable)(dri2_surf->dri_drawable);

   free(dri2_surf);

   return EGL_TRUE;
}

static int
update_buffers(struct dri2_egl_surface *dri2_surf)
{
   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return 0;

   /* try to dequeue the next back buffer */
   if (!dri2_surf->buffer && !droid_window_dequeue_buffer(dri2_surf)) {
      _eglLog(_EGL_WARNING, "Could not dequeue buffer from native window");
      return -1;
   }

   /* free outdated buffers and update the surface size */
   if (dri2_surf->base.Width != dri2_surf->buffer->width ||
       dri2_surf->base.Height != dri2_surf->buffer->height) {
      droid_free_local_buffers(dri2_surf);
      dri2_surf->base.Width = dri2_surf->buffer->width;
      dri2_surf->base.Height = dri2_surf->buffer->height;
   }

   return 0;
}

static int
get_back_bo(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int fourcc, pitch;
   int offset = 0, fd;

   if (dri2_surf->dri_image_back)
      return 0;

   if (!dri2_surf->buffer)
      return -1;

   fd = get_native_buffer_fd(dri2_surf->buffer);
   if (fd < 0) {
      _eglLog(_EGL_WARNING, "Could not get native buffer FD");
      return -1;
   }

   fourcc = get_fourcc(dri2_surf->buffer->format);

   pitch = dri2_surf->buffer->stride *
      get_format_bpp(dri2_surf->buffer->format);

   if (fourcc == -1 || pitch == 0) {
      _eglLog(_EGL_WARNING, "Invalid buffer fourcc(%x) or pitch(%d)",
              fourcc, pitch);
      return -1;
   }

   dri2_surf->dri_image_back =
      dri2_dpy->image->createImageFromFds(dri2_dpy->dri_screen,
                                          dri2_surf->base.Width,
                                          dri2_surf->base.Height,
                                          fourcc,
                                          &fd,
                                          1,
                                          &pitch,
                                          &offset,
                                          dri2_surf);
   if (!dri2_surf->dri_image_back)
      return -1;

   return 0;
}

static int
droid_image_get_buffers(__DRIdrawable *driDrawable,
                  unsigned int format,
                  uint32_t *stamp,
                  void *loaderPrivate,
                  uint32_t buffer_mask,
                  struct __DRIimageList *images)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   images->image_mask = 0;
   images->front = NULL;
   images->back = NULL;

   if (update_buffers(dri2_surf) < 0)
      return 0;

   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
         /* According current EGL spec,
          * front buffer rendering for window surface is not supported now */
         _eglLog(_EGL_WARNING,
                 "%s:%d front buffer rendering for window surface is not supported!",
                 __func__, __LINE__);
         return 0;
      }

      /* The EGL 1.5 spec states that pbuffers are single-buffered. Specifically,
       * the spec states that they have a back buffer but no front buffer, in
       * contrast to pixmaps, which have a front buffer but no back buffer.
       *
       * Single-buffered surfaces with no front buffer confuse Mesa; so we deviate
       * from the spec, following the precedent of Mesa's EGL X11 platform. The
       * X11 platform correctly assigns pbuffers to single-buffered configs, but
       * assigns the pbuffer a front buffer instead of a back buffer.
       *
       * Pbuffers in the X11 platform mostly work today, so let's just copy its
       * behavior instead of trying to fix (and hence potentially breaking) the
       * world.
       */
      if (!dri2_surf->dri_image_front &&
          dri2_surf->base.Type == EGL_PBUFFER_BIT) {
         dri2_surf->dri_image_front =
            dri2_dpy->image->createImage(dri2_dpy->dri_screen,
                                         dri2_surf->base.Width,
                                         dri2_surf->base.Height,
                                         format,
                                         0,
                                         dri2_surf);
      }

      if (!dri2_surf->dri_image_front) {
         _eglLog(_EGL_WARNING,
                 "%s:%d dri2_image front buffer allocation failed!",
                 __func__, __LINE__);
         return 0;
      }

      images->front = dri2_surf->dri_image_front;
      images->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
   }

   if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
      if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
         if (get_back_bo(dri2_surf) < 0)
            return 0;
      }

      if (!dri2_surf->dri_image_back) {
         _eglLog(_EGL_WARNING,
                 "%s:%d dri2_image back buffer allocation failed!",
                 __func__, __LINE__);
         return 0;
      }

      images->back = dri2_surf->dri_image_back;
      images->image_mask |= __DRI_IMAGE_BUFFER_BACK;
   }

   return 1;
}

static EGLBoolean
droid_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return EGL_TRUE;

   dri2_flush_drawable_for_swapbuffers(disp, draw);

   if (dri2_surf->buffer)
      droid_window_enqueue_buffer(disp, dri2_surf);

   (*dri2_dpy->flush->invalidate)(dri2_surf->dri_drawable);

   return EGL_TRUE;
}

static _EGLImage *
droid_create_image_from_prime_fd_yuv(_EGLDisplay *disp, _EGLContext *ctx,
                                     struct ANativeWindowBuffer *buf, int fd)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct android_ycbcr ycbcr;
   size_t offsets[3];
   size_t pitches[3];
   int is_ycrcb;
   int fourcc;
   int ret;

   if (!dri2_dpy->gralloc->lock_ycbcr) {
      _eglLog(_EGL_WARNING, "Gralloc does not support lock_ycbcr");
      return NULL;
   }

   memset(&ycbcr, 0, sizeof(ycbcr));
   ret = dri2_dpy->gralloc->lock_ycbcr(dri2_dpy->gralloc, buf->handle,
                                       0, 0, 0, 0, 0, &ycbcr);
   if (ret) {
      _eglLog(_EGL_WARNING, "gralloc->lock_ycbcr failed: %d", ret);
      return NULL;
   }
   dri2_dpy->gralloc->unlock(dri2_dpy->gralloc, buf->handle);

   /* When lock_ycbcr's usage argument contains no SW_READ/WRITE flags
    * it will return the .y/.cb/.cr pointers based on a NULL pointer,
    * so they can be interpreted as offsets. */
   offsets[0] = (size_t)ycbcr.y;
   /* We assume here that all the planes are located in one DMA-buf. */
   is_ycrcb = (size_t)ycbcr.cb < (size_t)ycbcr.cr;
   if (is_ycrcb) {
      offsets[1] = (size_t)ycbcr.cr;
      offsets[2] = (size_t)ycbcr.cb;
   } else {
      offsets[1] = (size_t)ycbcr.cb;
      offsets[2] = (size_t)ycbcr.cr;
   }

   /* .ystride is the line length (in bytes) of the Y plane,
    * .cstride is the line length (in bytes) of any of the remaining
    * Cb/Cr/CbCr planes, assumed to be the same for Cb and Cr for fully
    * planar formats. */
   pitches[0] = ycbcr.ystride;
   pitches[1] = pitches[2] = ycbcr.cstride;

   /* .chroma_step is the byte distance between the same chroma channel
    * values of subsequent pixels, assumed to be the same for Cb and Cr. */
   fourcc = get_fourcc_yuv(buf->format, is_ycrcb, ycbcr.chroma_step);
   if (fourcc == -1) {
      _eglLog(_EGL_WARNING, "unsupported YUV format, native = %x, is_ycrcb = %d, chroma_step = %d",
              buf->format, is_ycrcb, ycbcr.chroma_step);
      return NULL;
   }

   if (ycbcr.chroma_step == 2) {
      /* Semi-planar Y + CbCr or Y + CbCr format. */
      const EGLint attr_list_2plane[] = {
         EGL_WIDTH, buf->width,
         EGL_HEIGHT, buf->height,
         EGL_LINUX_DRM_FOURCC_EXT, fourcc,
         EGL_DMA_BUF_PLANE0_FD_EXT, fd,
         EGL_DMA_BUF_PLANE0_PITCH_EXT, pitches[0],
         EGL_DMA_BUF_PLANE0_OFFSET_EXT, offsets[0],
         EGL_DMA_BUF_PLANE1_FD_EXT, fd,
         EGL_DMA_BUF_PLANE1_PITCH_EXT, pitches[1],
         EGL_DMA_BUF_PLANE1_OFFSET_EXT, offsets[1],
         EGL_NONE, 0
      };

      return dri2_create_image_dma_buf(disp, ctx, NULL, attr_list_2plane);
   } else {
      /* Fully planar Y + Cb + Cr or Y + Cr + Cb format. */
      const EGLint attr_list_3plane[] = {
         EGL_WIDTH, buf->width,
         EGL_HEIGHT, buf->height,
         EGL_LINUX_DRM_FOURCC_EXT, fourcc,
         EGL_DMA_BUF_PLANE0_FD_EXT, fd,
         EGL_DMA_BUF_PLANE0_PITCH_EXT, pitches[0],
         EGL_DMA_BUF_PLANE0_OFFSET_EXT, offsets[0],
         EGL_DMA_BUF_PLANE1_FD_EXT, fd,
         EGL_DMA_BUF_PLANE1_PITCH_EXT, pitches[1],
         EGL_DMA_BUF_PLANE1_OFFSET_EXT, offsets[1],
         EGL_DMA_BUF_PLANE2_FD_EXT, fd,
         EGL_DMA_BUF_PLANE2_PITCH_EXT, pitches[2],
         EGL_DMA_BUF_PLANE2_OFFSET_EXT, offsets[2],
         EGL_NONE, 0
      };

      return dri2_create_image_dma_buf(disp, ctx, NULL, attr_list_3plane);
   }
}

static _EGLImage *
droid_create_image_from_prime_fd(_EGLDisplay *disp, _EGLContext *ctx,
                                 struct ANativeWindowBuffer *buf, int fd)
{
   unsigned int pitch;

   if (is_yuv(buf->format))
      return droid_create_image_from_prime_fd_yuv(disp, ctx, buf, fd);

   const int fourcc = get_fourcc(buf->format);
   if (fourcc == -1) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   pitch = buf->stride * get_format_bpp(buf->format);
   if (pitch == 0) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   const EGLint attr_list[] = {
      EGL_WIDTH, buf->width,
      EGL_HEIGHT, buf->height,
      EGL_LINUX_DRM_FOURCC_EXT, fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT, fd,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_NONE, 0
   };

   return dri2_create_image_dma_buf(disp, ctx, NULL, attr_list);
}

static _EGLImage *
droid_create_image_from_name(_EGLDisplay *disp, _EGLContext *ctx,
                             struct ANativeWindowBuffer *buf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img;
   int name;
   int format;

   name = get_native_buffer_name(buf);
   if (!name) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   format = get_format(buf->format);
   if (format == -1)
       return NULL;

   dri2_img = calloc(1, sizeof(*dri2_img));
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "droid_create_image_mesa_drm");
      return NULL;
   }

   if (!_eglInitImage(&dri2_img->base, disp)) {
      free(dri2_img);
      return NULL;
   }

   dri2_img->dri_image =
      dri2_dpy->image->createImageFromName(dri2_dpy->dri_screen,
					   buf->width,
					   buf->height,
					   format,
					   name,
					   buf->stride,
					   dri2_img);
   if (!dri2_img->dri_image) {
      free(dri2_img);
      _eglError(EGL_BAD_ALLOC, "droid_create_image_mesa_drm");
      return NULL;
   }

   return &dri2_img->base;
}

static EGLBoolean
droid_query_surface(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                    EGLint attribute, EGLint *value)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   switch (attribute) {
      case EGL_WIDTH:
         if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
            dri2_surf->window->query(dri2_surf->window,
                                     NATIVE_WINDOW_DEFAULT_WIDTH, value);
            return EGL_TRUE;
         }
         break;
      case EGL_HEIGHT:
         if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
            dri2_surf->window->query(dri2_surf->window,
                                     NATIVE_WINDOW_DEFAULT_HEIGHT, value);
            return EGL_TRUE;
         }
         break;
      default:
         break;
   }
   return _eglQuerySurface(drv, dpy, surf, attribute, value);
}

static _EGLImage *
dri2_create_image_android_native_buffer(_EGLDisplay *disp,
                                        _EGLContext *ctx,
                                        struct ANativeWindowBuffer *buf)
{
   int fd;

   if (ctx != NULL) {
      /* From the EGL_ANDROID_image_native_buffer spec:
       *
       *     * If <target> is EGL_NATIVE_BUFFER_ANDROID and <ctx> is not
       *       EGL_NO_CONTEXT, the error EGL_BAD_CONTEXT is generated.
       */
      _eglError(EGL_BAD_CONTEXT, "eglCreateEGLImageKHR: for "
                "EGL_NATIVE_BUFFER_ANDROID, the context must be "
                "EGL_NO_CONTEXT");
      return NULL;
   }

   if (!buf || buf->common.magic != ANDROID_NATIVE_BUFFER_MAGIC ||
       buf->common.version != sizeof(*buf)) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   fd = get_native_buffer_fd(buf);
   if (fd >= 0)
      return droid_create_image_from_prime_fd(disp, ctx, buf, fd);

   return droid_create_image_from_name(disp, ctx, buf);
}

static _EGLImage *
droid_create_image_khr(_EGLDriver *drv, _EGLDisplay *disp,
		       _EGLContext *ctx, EGLenum target,
		       EGLClientBuffer buffer, const EGLint *attr_list)
{
   switch (target) {
   case EGL_NATIVE_BUFFER_ANDROID:
      return dri2_create_image_android_native_buffer(disp, ctx,
            (struct ANativeWindowBuffer *) buffer);
   default:
      return dri2_create_image_khr(drv, disp, ctx, target, buffer, attr_list);
   }
}

static void
droid_flush_front_buffer(__DRIdrawable * driDrawable, void *loaderPrivate)
{
}

static int
droid_get_buffers_parse_attachments(struct dri2_egl_surface *dri2_surf,
                                    unsigned int *attachments, int count)
{
   int num_buffers = 0, i;

   /* fill dri2_surf->buffers */
   for (i = 0; i < count * 2; i += 2) {
      __DRIbuffer *buf, *local;

      assert(num_buffers < ARRAY_SIZE(dri2_surf->buffers));
      buf = &dri2_surf->buffers[num_buffers];

      switch (attachments[i]) {
      case __DRI_BUFFER_BACK_LEFT:
         if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
            buf->attachment = attachments[i];
            buf->name = get_native_buffer_name(dri2_surf->buffer);
            buf->cpp = get_format_bpp(dri2_surf->buffer->format);
            buf->pitch = dri2_surf->buffer->stride * buf->cpp;
            buf->flags = 0;

            if (buf->name)
               num_buffers++;

            break;
         }
         /* fall through for pbuffers */
      case __DRI_BUFFER_DEPTH:
      case __DRI_BUFFER_STENCIL:
      case __DRI_BUFFER_ACCUM:
      case __DRI_BUFFER_DEPTH_STENCIL:
      case __DRI_BUFFER_HIZ:
         local = droid_alloc_local_buffer(dri2_surf,
               attachments[i], attachments[i + 1]);

         if (local) {
            *buf = *local;
            num_buffers++;
         }
         break;
      case __DRI_BUFFER_FRONT_LEFT:
      case __DRI_BUFFER_FRONT_RIGHT:
      case __DRI_BUFFER_FAKE_FRONT_LEFT:
      case __DRI_BUFFER_FAKE_FRONT_RIGHT:
      case __DRI_BUFFER_BACK_RIGHT:
      default:
         /* no front or right buffers */
         break;
      }
   }

   return num_buffers;
}

static __DRIbuffer *
droid_get_buffers_with_format(__DRIdrawable * driDrawable,
			     int *width, int *height,
			     unsigned int *attachments, int count,
			     int *out_count, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (update_buffers(dri2_surf) < 0)
      return NULL;

   dri2_surf->buffer_count =
      droid_get_buffers_parse_attachments(dri2_surf, attachments, count);

   if (width)
      *width = dri2_surf->base.Width;
   if (height)
      *height = dri2_surf->base.Height;

   *out_count = dri2_surf->buffer_count;

   return dri2_surf->buffers;
}

static EGLBoolean
droid_add_configs_for_visuals(_EGLDriver *drv, _EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   static const struct {
      int format;
      unsigned int rgba_masks[4];
   } visuals[] = {
      { HAL_PIXEL_FORMAT_RGBA_8888, { 0xff, 0xff00, 0xff0000, 0xff000000 } },
      { HAL_PIXEL_FORMAT_RGBX_8888, { 0xff, 0xff00, 0xff0000, 0x0 } },
      { HAL_PIXEL_FORMAT_RGB_565,   { 0xf800, 0x7e0, 0x1f, 0x0 } },
      { HAL_PIXEL_FORMAT_BGRA_8888, { 0xff0000, 0xff00, 0xff, 0xff000000 } },
   };
   EGLint config_attrs[] = {
     EGL_NATIVE_VISUAL_ID,   0,
     EGL_NATIVE_VISUAL_TYPE, 0,
     EGL_FRAMEBUFFER_TARGET_ANDROID, EGL_TRUE,
     EGL_RECORDABLE_ANDROID, EGL_TRUE,
     EGL_NONE
   };
   unsigned int format_count[ARRAY_SIZE(visuals)] = { 0 };
   int count, i, j;

   count = 0;
   for (i = 0; dri2_dpy->driver_configs[i]; i++) {
      const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
      struct dri2_egl_config *dri2_conf;

      for (j = 0; j < ARRAY_SIZE(visuals); j++) {
         config_attrs[1] = visuals[j].format;
         config_attrs[3] = visuals[j].format;

         dri2_conf = dri2_add_config(dpy, dri2_dpy->driver_configs[i],
               count + 1, surface_type, config_attrs, visuals[j].rgba_masks);
         if (dri2_conf) {
            count++;
            format_count[j]++;
         }
      }
   }

   for (i = 0; i < ARRAY_SIZE(format_count); i++) {
      if (!format_count[i]) {
         _eglLog(_EGL_DEBUG, "No DRI config supports native format 0x%x",
                 visuals[i].format);
      }
   }

   return (count != 0);
}

static int swrastUpdateBuffer(struct dri2_egl_surface *dri2_surf)
{
   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
       if (!dri2_surf->buffer && !droid_window_dequeue_buffer(dri2_surf)) {
          _eglLog(_EGL_WARNING, "failed to dequeue buffer for window");
          return 1;
       }
       dri2_surf->base.Width = dri2_surf->buffer->width;
       dri2_surf->base.Height = dri2_surf->buffer->height;
   }
   return 0;
}

static void
swrastGetDrawableInfo(__DRIdrawable * draw,
                      int *x, int *y, int *w, int *h,
                      void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   swrastUpdateBuffer(dri2_surf);

   *x = 0;
   *y = 0;
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static void
swrastPutImage2(__DRIdrawable * draw, int op,
                int x, int y, int w, int h, int stride,
                char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct _EGLDisplay *egl_dpy = dri2_surf->base.Resource.Display;
   char *dstPtr, *srcPtr;
   size_t BPerPixel, dstStride, copyWidth, xOffset;

//   _eglLog(_EGL_WARNING, "calling swrastPutImage with draw=%p, private=%p, x=%d, y=%d, w=%d, h=%d",
//                    draw, loaderPrivate, x, y, w, h);

   if (swrastUpdateBuffer(dri2_surf)) {
      return;
   }

   BPerPixel = get_format_bpp(dri2_surf->buffer->format);
   dstStride = BPerPixel * dri2_surf->buffer->stride;
   copyWidth = BPerPixel * w;
   xOffset = BPerPixel * x;

   /* drivers expect we do these checks (and some rely on it) */
   if (copyWidth > dstStride - xOffset)
      copyWidth = dstStride - xOffset;
   if (h > dri2_surf->base.Height - y)
      h = dri2_surf->base.Height - y;

   if (gr_module->lock(gr_module, dri2_surf->buffer->handle, GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                       0, 0, dri2_surf->buffer->width, dri2_surf->buffer->height, (void**)&dstPtr)) {
      _eglLog(_EGL_WARNING, "can not lock window buffer");
      return;
   }

   dstPtr += y * dstStride + xOffset;
   srcPtr = data;

   if (xOffset == 0 && copyWidth == stride && copyWidth == dstStride) {
      memcpy(dstPtr, srcPtr, copyWidth * h);
   } else {
      for (; h>0; h--) {
         memcpy(dstPtr, srcPtr, copyWidth);
         srcPtr += stride;
         dstPtr += dstStride;
      }
   }

   if (gr_module->unlock(gr_module, dri2_surf->buffer->handle)) {
      _eglLog(_EGL_WARNING, "unlock buffer failed");
   }

   droid_window_enqueue_buffer(egl_dpy, dri2_surf);
}

static void
swrastPutImage(__DRIdrawable * draw, int op,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int stride;

   if (swrastUpdateBuffer(dri2_surf)) {
      return;
   }

   stride = get_format_bpp(dri2_surf->buffer->format) * w;
   swrastPutImage2(draw, op, x, y, w, h, stride, data, loaderPrivate);
}

static void
swrastGetImage(__DRIdrawable * read,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   size_t BPerPixel, srcStride, copyWidth, xOffset;
   char *dstPtr, *srcPtr;

   _eglLog(_EGL_WARNING, "calling swrastGetImage with read=%p, private=%p, w=%d, h=%d", read, loaderPrivate, w, h);

   if (swrastUpdateBuffer(dri2_surf)) {
      _eglLog(_EGL_WARNING, "swrastGetImage failed data unchanged");
      return;
   }

   BPerPixel = get_format_bpp(dri2_surf->buffer->format);
   srcStride = BPerPixel * dri2_surf->buffer->stride;
   copyWidth = BPerPixel * w;
   xOffset = BPerPixel * x;

   if (gr_module->lock(gr_module, dri2_surf->buffer->handle, GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                       0, 0, dri2_surf->buffer->width, dri2_surf->buffer->height, (void**)&srcPtr)) {
      _eglLog(_EGL_WARNING, "can not lock window buffer");
      memset(data, 0, copyWidth * h);
      return;
   }

   srcPtr += y * srcStride + xOffset;
   dstPtr = data;

   if (xOffset == 0 && copyWidth == srcStride) {
      memcpy(dstPtr, srcPtr, copyWidth * h);
   } else {
      for (; h>0; h--) {
         memcpy(dstPtr, srcPtr, copyWidth);
         srcPtr += srcStride;
         dstPtr += copyWidth;
      }
   }

   if (gr_module->unlock(gr_module, dri2_surf->buffer->handle)) {
      _eglLog(_EGL_WARNING, "unlock buffer failed");
   }
}

static EGLBoolean
swrast_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   dri2_dpy->core->swapBuffers(dri2_surf->dri_drawable);

   return EGL_TRUE;
}

static _EGLImage *
swrast_create_image_android_native_buffer(_EGLDisplay *disp, _EGLContext *ctx,
                                        struct ANativeWindowBuffer *buf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img;
   struct winsys_handle whandle;
   EGLint format;

   if (ctx != NULL) {
      /* From the EGL_ANDROID_image_native_buffer spec:
       *
       *     * If <target> is EGL_NATIVE_BUFFER_ANDROID and <ctx> is not
       *       EGL_NO_CONTEXT, the error EGL_BAD_CONTEXT is generated.
       */
      _eglError(EGL_BAD_CONTEXT, "eglCreateEGLImageKHR: for "
                "EGL_NATIVE_BUFFER_ANDROID, the context must be "
                "EGL_NO_CONTEXT");
      return NULL;
   }

   if (!buf || buf->common.magic != ANDROID_NATIVE_BUFFER_MAGIC ||
       buf->common.version != sizeof(*buf)) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   /* see the table in droid_add_configs_for_visuals */
   format = get_format(buf->format);
   if (format < 0)
      return NULL;

   dri2_img = calloc(1, sizeof(*dri2_img));
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "droid_create_image_mesa_drm");
      return NULL;
   }

   if (!_eglInitImage(&dri2_img->base, disp)) {
      free(dri2_img);
      return NULL;
   }

   memset(&whandle, 0, sizeof(whandle));
   whandle.type = DRM_API_HANDLE_TYPE_BUFFER;
   whandle.externalBuffer = buf;
   whandle.stride = buf->stride * get_format_bpp(buf->format);

   dri2_img->dri_image =
         dri2_dpy->swrast->createImageFromWinsys(dri2_dpy->dri_screen,
                                                 buf->width,
                                                 buf->height,
                                                 format,
                                                 1, &whandle,
                                                 dri2_img);

   if (!dri2_img->dri_image) {
      free(dri2_img);
      _eglError(EGL_BAD_ALLOC, "droid_create_image_mesa_drm");
      return NULL;
   }

   return &dri2_img->base;
}

static _EGLImage *
swrast_create_image_khr(_EGLDriver *drv, _EGLDisplay *disp,
		       _EGLContext *ctx, EGLenum target,
		       EGLClientBuffer buffer, const EGLint *attr_list)
{
   switch (target) {
   case EGL_NATIVE_BUFFER_ANDROID:
      return swrast_create_image_android_native_buffer(disp, ctx,
            (struct ANativeWindowBuffer *) buffer);
   default:
      return dri2_create_image_khr(drv, disp, ctx, target, buffer, attr_list);
   }
}

static int
load_gralloc(void)
{
   const hw_module_t *mod;
   int err;

   err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mod);
   if (!err) {
      gr_module = (gralloc_module_t *) mod;
   } else {
      _eglLog(_EGL_WARNING, "fail to load gralloc");
   }
   return err;
}

static int
is_drm_gralloc(void)
{
   /* need a cleaner way to distinguish drm_gralloc and gralloc.default */
   return !!gr_module->perform;
}

static int
droid_open_device(struct dri2_egl_display *dri2_dpy)
{
   int fd = -1, err = -EINVAL;

   if (dri2_dpy->gralloc->perform)
         err = dri2_dpy->gralloc->perform(dri2_dpy->gralloc,
                                          GRALLOC_MODULE_PERFORM_GET_DRM_FD,
                                          &fd);
   if (err || fd < 0) {
      _eglLog(_EGL_WARNING, "fail to get drm fd");
      fd = -1;
   }

   return (fd >= 0) ? fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;
}

/* support versions < JellyBean */
#ifndef ALOGW
#define ALOGW LOGW
#endif
#ifndef ALOGD
#define ALOGD LOGD
#endif
#ifndef ALOGI
#define ALOGI LOGI
#endif

static void
droid_log(EGLint level, const char *msg)
{
   switch (level) {
   case _EGL_DEBUG:
      ALOGD("%s", msg);
      break;
   case _EGL_INFO:
      ALOGI("%s", msg);
      break;
   case _EGL_WARNING:
      ALOGW("%s", msg);
      break;
   case _EGL_FATAL:
      LOG_FATAL("%s", msg);
      break;
   default:
      break;
   }
}

static struct dri2_egl_display_vtbl droid_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = droid_create_window_surface,
   .create_pixmap_surface = dri2_fallback_create_pixmap_surface,
   .create_pbuffer_surface = droid_create_pbuffer_surface,
   .destroy_surface = droid_destroy_surface,
   .create_image = droid_create_image_khr,
   .swap_interval = dri2_fallback_swap_interval,
   .swap_buffers = droid_swap_buffers,
   .swap_buffers_with_damage = dri2_fallback_swap_buffers_with_damage,
   .swap_buffers_region = dri2_fallback_swap_buffers_region,
   .post_sub_buffer = dri2_fallback_post_sub_buffer,
   .copy_buffers = dri2_fallback_copy_buffers,
   .query_buffer_age = dri2_fallback_query_buffer_age,
   .query_surface = droid_query_surface,
   .create_wayland_buffer_from_image = dri2_fallback_create_wayland_buffer_from_image,
   .get_sync_values = dri2_fallback_get_sync_values,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static const __DRIdri2LoaderExtension droid_dri2_loader_extension = {
   .base = { __DRI_DRI2_LOADER, 3 },

   .getBuffers           = NULL,
   .flushFrontBuffer     = droid_flush_front_buffer,
   .getBuffersWithFormat = droid_get_buffers_with_format,
};

static const __DRIimageLoaderExtension droid_image_loader_extension = {
   .base = { __DRI_IMAGE_LOADER, 1 },

   .getBuffers          = droid_image_get_buffers,
   .flushFrontBuffer    = droid_flush_front_buffer,
};

static const __DRIswrastLoaderExtension droid_swrast_loader_extension = {
   .base = { __DRI_SWRAST_LOADER, 2 },

   .getDrawableInfo     = swrastGetDrawableInfo,
   .putImage            = swrastPutImage,
   .getImage            = swrastGetImage,
   .putImage2           = swrastPutImage2,
};

static const __DRIextension *droid_dri2_loader_extensions[] = {
   &droid_dri2_loader_extension.base,
   &image_lookup_extension.base,
   &use_invalidate.base,
   NULL,
};

static const __DRIextension *droid_image_loader_extensions[] = {
   &droid_image_loader_extension.base,
   &image_lookup_extension.base,
   &use_invalidate.base,
   NULL,
};

static const __DRIextension *droid_swrast_loader_extensions[] = {
   &droid_swrast_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

static EGLBoolean
dri2_initialize_android_drm(_EGLDriver *drv, _EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy;
   const char *err;
   int ret;

   _eglSetLogProc(droid_log);

   loader_set_logger(_eglLog);

   dri2_dpy = calloc(1, sizeof(*dri2_dpy));
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                       (const hw_module_t **)&dri2_dpy->gralloc);
   if (ret) {
      err = "DRI2: failed to get gralloc module";
      goto cleanup_display;
   }

   dpy->DriverData = (void *) dri2_dpy;

   dri2_dpy->fd = droid_open_device(dri2_dpy);
   if (dri2_dpy->fd < 0) {
      err = "DRI2: failed to open device";
      goto cleanup_display;
   }

   dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd);
   if (dri2_dpy->driver_name == NULL) {
      err = "DRI2: failed to get driver name";
      goto cleanup_device;
   }

   if (!dri2_load_driver(dpy)) {
      err = "DRI2: failed to load driver";
      goto cleanup_driver_name;
   }

   dri2_dpy->is_render_node = drmGetNodeTypeFromFd(dri2_dpy->fd) == DRM_NODE_RENDER;

   /* render nodes cannot use Gem names, and thus do not support
    * the __DRI_DRI2_LOADER extension */
   if (!dri2_dpy->is_render_node)
      dri2_dpy->loader_extensions = droid_dri2_loader_extensions;
   else
      dri2_dpy->loader_extensions = droid_image_loader_extensions;

   if (!dri2_create_screen(dpy)) {
      err = "DRI2: failed to create screen";
      goto cleanup_driver;
   }

   if (!droid_add_configs_for_visuals(drv, dpy)) {
      err = "DRI2: failed to add configs";
      goto cleanup_screen;
   }

   dpy->Extensions.ANDROID_framebuffer_target = EGL_TRUE;
   dpy->Extensions.ANDROID_image_native_buffer = EGL_TRUE;
   dpy->Extensions.ANDROID_recordable = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &droid_display_vtbl;

   return EGL_TRUE;

cleanup_screen:
   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);
cleanup_driver:
   dlclose(dri2_dpy->driver);
cleanup_driver_name:
   free(dri2_dpy->driver_name);
cleanup_device:
   close(dri2_dpy->fd);
cleanup_display:
   free(dri2_dpy);
   dpy->DriverData = NULL;

   return _eglError(EGL_NOT_INITIALIZED, err);
}

/* differs with droid_display_vtbl in create_image, swap_buffers */
static struct dri2_egl_display_vtbl swrast_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = droid_create_window_surface,
   .create_pixmap_surface = dri2_fallback_create_pixmap_surface,
   .create_pbuffer_surface = droid_create_pbuffer_surface,
   .destroy_surface = droid_destroy_surface,
   .create_image = swrast_create_image_khr,
   .swap_interval = dri2_fallback_swap_interval,
   .swap_buffers = swrast_swap_buffers,
   .swap_buffers_with_damage = dri2_fallback_swap_buffers_with_damage,
   .swap_buffers_region = dri2_fallback_swap_buffers_region,
   .post_sub_buffer = dri2_fallback_post_sub_buffer,
   .copy_buffers = dri2_fallback_copy_buffers,
   .query_buffer_age = dri2_fallback_query_buffer_age,
   .create_wayland_buffer_from_image = dri2_fallback_create_wayland_buffer_from_image,
   .get_sync_values = dri2_fallback_get_sync_values,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static EGLBoolean
dri2_initialize_android_swrast(_EGLDriver *drv, _EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy;
   const char *err = "";
   const hw_module_t *mod;

   _eglSetLogProc(droid_log);

   loader_set_logger(_eglLog);

   dri2_dpy = calloc(1, sizeof(*dri2_dpy));
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   dpy->DriverData = (void *) dri2_dpy;

   dri2_dpy->driver_name = strdup("swrast");
   if (!dri2_load_driver_swrast(dpy)) {
      err = "DRISW: failed to load swrast driver";
      goto cleanup_driver_name;
   }

   dri2_dpy->loader_extensions = droid_swrast_loader_extensions;

   if (!dri2_create_screen(dpy)) {
      err = "DRISW: failed to create screen";
      goto cleanup_driver;
   }

   if (!droid_add_configs_for_visuals(drv, dpy)) {
      err = "DRISW: failed to add configs";
      goto cleanup_screen;
   }

   dpy->Extensions.ANDROID_framebuffer_target = EGL_TRUE;
   dpy->Extensions.ANDROID_image_native_buffer = EGL_TRUE;
   dpy->Extensions.ANDROID_recordable = EGL_TRUE;
   dpy->Extensions.KHR_image_base = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &swrast_display_vtbl;

   return EGL_TRUE;

cleanup_screen:
   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);
cleanup_driver:
   dlclose(dri2_dpy->driver);
cleanup_driver_name:
   free(dri2_dpy->driver_name);
   free(dri2_dpy);

   return _eglError(EGL_NOT_INITIALIZED, err);
}

EGLBoolean
dri2_initialize_android(_EGLDriver *drv, _EGLDisplay *dpy)
{
   EGLBoolean initialized = EGL_TRUE;

   if (load_gralloc()) {
      return EGL_FALSE;
   }

   int droid_hw_accel = (getenv("LIBGL_ALWAYS_SOFTWARE") == NULL) && is_drm_gralloc();

   if (droid_hw_accel) {
      if (!dri2_initialize_android_drm(drv, dpy)) {
         initialized = dri2_initialize_android_swrast(drv, dpy);
         if (initialized) {
            _eglLog(_EGL_INFO, "Android: Fallback to software renderer");
         }
      }
   } else {
      initialized = dri2_initialize_android_swrast(drv, dpy);
   }

   return initialized;
}
