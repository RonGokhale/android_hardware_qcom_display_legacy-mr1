/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (c) 2010-2012 Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/mman.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <dlfcn.h>

#include <hardware/hardware.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <cutils/atomic.h>

#include <linux/fb.h>
#include <linux/msm_mdp.h>

#include <GLES/gl.h>

#include "gralloc_priv.h"
#include "fb_priv.h"
#include "gr.h"
#include <genlock.h>
#include <cutils/properties.h>
#include <profiler.h>

#define EVEN_OUT(x) if (x & 0x0001) {x--;}
/** min of int a, b */
static inline int min(int a, int b) {
    return (a<b) ? a : b;
}
/** max of int a, b */
static inline int max(int a, int b) {
    return (a>b) ? a : b;
}

enum {
    PAGE_FLIP = 0x00000001,
    LOCKED    = 0x00000002
};

struct fb_context_t {
    framebuffer_device_t  device;
};


static int fb_setSwapInterval(struct framebuffer_device_t* dev,
                              int interval)
{
    //XXX: Get the value here and implement along with
    //single vsync in HWC
    char pval[PROPERTY_VALUE_MAX];
    property_get("debug.egl.swapinterval", pval, "-1");
    int property_interval = atoi(pval);
    if (property_interval >= 0)
        interval = property_interval;

    fb_context_t* ctx = (fb_context_t*)dev;
    private_module_t* m = reinterpret_cast<private_module_t*>(
        dev->common.module);
    if (interval < dev->minSwapInterval || interval > dev->maxSwapInterval)
        return -EINVAL;

    m->swapInterval = interval;
    return 0;
}

static int fb_setUpdateRect(struct framebuffer_device_t* dev,
                            int l, int t, int w, int h)
{
    if (((w|h) <= 0) || ((l|t)<0))
        return -EINVAL;
    fb_context_t* ctx = (fb_context_t*)dev;
    private_module_t* m = reinterpret_cast<private_module_t*>(
        dev->common.module);
    m->info.reserved[0] = 0x54445055; // "UPDT";
    m->info.reserved[1] = (uint16_t)l | ((uint32_t)t << 16);
    m->info.reserved[2] = (uint16_t)(l+w) | ((uint32_t)(t+h) << 16);
    return 0;
}

static int fb_post(struct framebuffer_device_t* dev, buffer_handle_t buffer)
{
#ifdef QCOM_ICS_COMPAT
    if (private_handle_t::validate(buffer) < 0)
        return -EINVAL;
#endif
    fb_context_t* ctx = (fb_context_t*) dev;

    private_handle_t *hnd = static_cast<private_handle_t*>
            (const_cast<native_handle_t*>(buffer));
    private_module_t* m =
        reinterpret_cast<private_module_t*>(dev->common.module);

    if (hnd && hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER) {
#ifdef QCOM_ICS_COMPAT
        genlock_lock_buffer(hnd, GENLOCK_READ_LOCK, GENLOCK_MAX_TIMEOUT);

        const size_t offset = hnd->base - m->framebuffer->base;
        // frame ready to be posted, signal so that hwc can update External
        // display
        pthread_mutex_lock(&m->fbPostLock);
        m->currentOffset = offset;
        m->fbPostDone = true;
        pthread_cond_signal(&m->fbPostCond);
        pthread_mutex_unlock(&m->fbPostLock);
        m->info.activate = FB_ACTIVATE_VBL;
        m->info.yoffset = offset / m->finfo.line_length;
        if (ioctl(m->framebuffer->fd, FBIOPUT_VSCREENINFO, &m->info) == -1) {
            ALOGE("FBIOPUT_VSCREENINFO failed");
            genlock_unlock_buffer(hnd);
            return -errno;
        }

		//Signals the composition thread to unblock and loop over if necessary
        pthread_mutex_lock(&m->fbPanLock);
        m->fbPanDone = true;
        pthread_cond_signal(&m->fbPanCond);
        pthread_mutex_unlock(&m->fbPanLock);

        if (m->currentBuffer) {
            genlock_unlock_buffer(m->currentBuffer);
            m->currentBuffer = 0;
        }

        CALC_FPS();
        m->currentBuffer = hnd;
#else
        m->info.activate = FB_ACTIVATE_VBL | FB_ACTIVATE_FORCE;
        m->info.yoffset = hnd->offset / m->finfo.line_length;
        m->commit.var = m->info;
        if (ioctl(m->framebuffer->fd, MSMFB_DISPLAY_COMMIT, &m->commit) == -1) {
            ALOGE("%s: MSMFB_DISPLAY_COMMIT ioctl failed, err: %s", __FUNCTION__,
                    strerror(errno));
            return -errno;
	}
#endif
    }
    return 0;
}

static int fb_compositionComplete(struct framebuffer_device_t* dev)
{
   // TODO: Properly implement composition complete callback   
#ifdef ANCIENT_GL
    glFinish();
#endif
    return 0;
}

int mapFrameBufferLocked(struct private_module_t* module)
{
    // already initialized...
    if (module->framebuffer) {
        return 0;
    }
    char const * const device_template[] = {
        "/dev/graphics/fb%u",
        "/dev/fb%u",
        0 };

    int fd = -1;
    int i=0;
    char name[64];
    char property[PROPERTY_VALUE_MAX];

    while ((fd==-1) && device_template[i]) {
        snprintf(name, 64, device_template[i], 0);
        fd = open(name, O_RDWR, 0);
        i++;
    }
    if (fd < 0)
        return -errno;

#ifndef QCOM_ICS_COMPAT
    memset(&module->fence, 0, sizeof(struct mdp_buf_fence));
    memset(&module->commit, 0, sizeof(struct mdp_display_commit));
#endif

    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    struct fb_var_screeninfo info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
    info.xoffset = 0;
    info.yoffset = 0;
    info.activate = FB_ACTIVATE_NOW;

    /* Interpretation of offset for color fields: All offsets are from the
     * right, inside a "pixel" value, which is exactly 'bits_per_pixel' wide
     * (means: you can use the offset as right argument to <<). A pixel
     * afterwards is a bit stream and is written to video memory as that
     * unmodified. This implies big-endian byte order if bits_per_pixel is
     * greater than 8.
     */

    if(info.bits_per_pixel == 32) {
        /*
         * Explicitly request RGBA_8888
         */
#ifdef SEMC_RGBA_8888_OFFSET
        info.red.offset     = 0;
        info.green.offset   = 8;
        info.blue.offset    = 16;
        info.transp.offset  = 24;
#else
        info.red.offset     = 24;
        info.green.offset   = 16;
        info.blue.offset    = 8;
#endif
        info.bits_per_pixel = 32;
        info.red.length     = 8;
        info.green.length   = 8;
        info.blue.length    = 8;
        info.transp.offset  = 0;
        info.transp.length  = 8;

        /* Note: the GL driver does not have a r=8 g=8 b=8 a=0 config, so if we
         * do not use the MDP for composition (i.e. hw composition == 0), ask
         * for RGBA instead of RGBX. */
        if (property_get("debug.sf.hw", property, NULL) > 0 &&
                                                           atoi(property) == 0)
            module->fbFormat = HAL_PIXEL_FORMAT_RGBX_8888;
        else if(property_get("debug.composition.type", property, NULL) > 0 &&
                (strncmp(property, "mdp", 3) == 0))
            module->fbFormat = HAL_PIXEL_FORMAT_RGBX_8888;
        else
            module->fbFormat = HAL_PIXEL_FORMAT_RGBA_8888;
    } else {
        /*
         * Explicitly request 5/6/5
         */
        info.bits_per_pixel = 16;
        info.red.offset     = 11;
        info.red.length     = 5;
        info.green.offset   = 5;
        info.green.length   = 6;
        info.blue.offset    = 0;
        info.blue.length    = 5;
        info.transp.offset  = 0;
        info.transp.length  = 0;
        module->fbFormat = HAL_PIXEL_FORMAT_RGB_565;
    }

    //adreno needs 4k aligned offsets. Max hole size is 4096-1
    int  size = roundUpToPageSize(info.yres * info.xres *
                                                       (info.bits_per_pixel/8));

    /*
     * Request NUM_BUFFERS screens (at least 2 for page flipping)
     */
    int numberOfBuffers = (int)(finfo.smem_len/size);
    ALOGV("num supported framebuffers in kernel = %d", numberOfBuffers);

    if (property_get("debug.gr.numframebuffers", property, NULL) > 0) {
        int num = atoi(property);
        if ((num >= NUM_FRAMEBUFFERS_MIN) && (num <= NUM_FRAMEBUFFERS_MAX)) {
            numberOfBuffers = num;
        }
    }
    if (numberOfBuffers > NUM_FRAMEBUFFERS_MAX)
        numberOfBuffers = NUM_FRAMEBUFFERS_MAX;

    ALOGV("We support %d buffers", numberOfBuffers);

    //consider the included hole by 4k alignment
    uint32_t line_length = (info.xres * info.bits_per_pixel / 8);
    info.yres_virtual = (size * numberOfBuffers) / line_length;

    uint32_t flags = PAGE_FLIP;
    if (info.yres_virtual < ((size * 2) / line_length) ) {
        // we need at least 2 for page-flipping
        info.yres_virtual = size / line_length;
        flags &= ~PAGE_FLIP;
        ALOGW("page flipping not supported (yres_virtual=%d, requested=%d)",
              info.yres_virtual, info.yres*2);
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    if (int(info.width) <= 0 || int(info.height) <= 0) {
        // the driver doesn't return that information
        // default to 160 dpi
        info.width  = ((info.xres * 25.4f)/160.0f + 0.5f);
        info.height = ((info.yres * 25.4f)/160.0f + 0.5f);
    }

    float xdpi = (info.xres * 25.4f) / info.width;
    float ydpi = (info.yres * 25.4f) / info.height;
    //The reserved[3] field is used to store FPS by the driver.
    float fps  = info.reserved[3] & 0xFF;

    ALOGI("using (fd=%d)\n"
          "id           = %s\n"
          "xres         = %d px\n"
          "yres         = %d px\n"
          "xres_virtual = %d px\n"
          "yres_virtual = %d px\n"
          "bpp          = %d\n"
          "r            = %2u:%u\n"
          "g            = %2u:%u\n"
          "b            = %2u:%u\n",
          fd,
          finfo.id,
          info.xres,
          info.yres,
          info.xres_virtual,
          info.yres_virtual,
          info.bits_per_pixel,
          info.red.offset, info.red.length,
          info.green.offset, info.green.length,
          info.blue.offset, info.blue.length
         );

    ALOGI("width        = %d mm (%f dpi)\n"
          "height       = %d mm (%f dpi)\n"
          "refresh rate = %.2f Hz\n",
          info.width,  xdpi,
          info.height, ydpi,
          fps
         );


    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    if (finfo.smem_len <= 0)
        return -errno;

    module->flags = flags;
    module->info = info;
    module->finfo = finfo;
    module->xdpi = xdpi;
    module->ydpi = ydpi;
    module->fps = fps;
    module->swapInterval = 1;

    CALC_INIT();

    /*
     * map the framebuffer
     */

    int err;
#ifdef QCOM_ICS_COMPAT
    module->numBuffers = info.yres_virtual / info.yres;
#else
    module->numBuffers = 2;
#endif
    module->bufferMask = 0;

    //adreno needs page aligned offsets. Align the fbsize to pagesize.
    size_t fbSize = roundUpToPageSize(finfo.line_length * info.yres)*
                    module->numBuffers;
    module->framebuffer = new private_handle_t(fd, fbSize,
                                        private_handle_t::PRIV_FLAGS_USES_PMEM,
                                        BUFFER_TYPE_UI,
                                        module->fbFormat, info.xres, info.yres);
    void* vaddr = mmap(0, fbSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (vaddr == MAP_FAILED) {
        ALOGE("Error mapping the framebuffer (%s)", strerror(errno));
        return -errno;
    }
    module->framebuffer->base = intptr_t(vaddr);
    memset(vaddr, 0, fbSize);
    module->currentOffset = 0;
    module->fbPostDone = false;
    pthread_mutex_init(&(module->fbPostLock), NULL);
    pthread_cond_init(&(module->fbPostCond), NULL);
    module->fbPanDone = false;
    pthread_mutex_init(&(module->fbPanLock), NULL);
    pthread_cond_init(&(module->fbPanCond), NULL);
    return 0;
}

static int mapFrameBuffer(struct private_module_t* module)
{
    pthread_mutex_lock(&module->lock);
    int err = mapFrameBufferLocked(module);
    pthread_mutex_unlock(&module->lock);
    return err;
}

/*****************************************************************************/

static int fb_close(struct hw_device_t *dev)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    if (ctx) {
        free(ctx);
    }
    return 0;
}

int fb_device_open(hw_module_t const* module, const char* name,
                   hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_FB0)) {
        alloc_device_t* gralloc_device;
        status = gralloc_open(module, &gralloc_device);
        if (status < 0)
            return status;

        /* initialize our state here */
        fb_context_t *dev = (fb_context_t*)malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag      = HARDWARE_DEVICE_TAG;
        dev->device.common.version  = 0;
        dev->device.common.module   = const_cast<hw_module_t*>(module);
        dev->device.common.close    = fb_close;
        dev->device.setSwapInterval = fb_setSwapInterval;
        dev->device.post            = fb_post;
        dev->device.setUpdateRect   = 0;
        dev->device.compositionComplete = fb_compositionComplete;

        private_module_t* m = (private_module_t*)module;
        status = mapFrameBuffer(m);
        if (status >= 0) {
            int stride = m->finfo.line_length / (m->info.bits_per_pixel >> 3);
            const_cast<uint32_t&>(dev->device.flags) = 0;
            const_cast<uint32_t&>(dev->device.width) = m->info.xres;
            const_cast<uint32_t&>(dev->device.height) = m->info.yres;
            const_cast<int&>(dev->device.stride) = stride;
            const_cast<int&>(dev->device.format) = m->fbFormat;
            const_cast<float&>(dev->device.xdpi) = m->xdpi;
            const_cast<float&>(dev->device.ydpi) = m->ydpi;
            const_cast<float&>(dev->device.fps) = m->fps;
            const_cast<int&>(dev->device.minSwapInterval) =
                                                        PRIV_MIN_SWAP_INTERVAL;
            const_cast<int&>(dev->device.maxSwapInterval) =
                                                        PRIV_MAX_SWAP_INTERVAL;
            const_cast<int&>(dev->device.numFramebuffers) = m->numBuffers;
            if (m->finfo.reserved[0] == 0x5444 &&
                m->finfo.reserved[1] == 0x5055) {
                dev->device.setUpdateRect = fb_setUpdateRect;
                ALOGD("UPDATE_ON_DEMAND supported");
            }

            *device = &dev->device.common;
        }

        // Close the gralloc module
        gralloc_close(gralloc_device);
    }
    return status;
}
