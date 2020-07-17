/*
 * Copyright 2017 NXP.
 * 2018 zaferkaya1960@hotmail.com
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
#include <dlfcn.h>
#include "Composer.h"
#include "MemoryManager.h"
#include <system/window.h>

#if defined(__LP64__)
#define LIB_PATH "/system/lib64"
#else
#define LIB_PATH "/system/lib"
#endif

#define GPUHELPER "libgpuhelper.so"
#define GPUENGINE "libg2d.so"

namespace fsl {

Composer::Composer()
{
    mTarget = NULL;
    mDimBuffer = NULL;
    mRotBuffer = NULL;
    N = 0;
    mHandle = NULL;

    for (int i = 0; i < 64; i++)
	mRotBuffers[i] = NULL;


    char path[PATH_MAX] = {0};
    snprintf(path, PATH_MAX, "%s/%s", LIB_PATH, GPUHELPER);

    void* handle = dlopen(path, RTLD_NOW);
    if (handle == NULL) {
        ALOGV("no %s found", path);
        mGetAlignedSize = NULL;
        mGetFlipOffset = NULL;
        mGetTiling = NULL;
        mAlterFormat = NULL;
        mLockSurface = NULL;
        mUnlockSurface = NULL;
    }
    else {
        mGetAlignedSize = (hwc_func3)dlsym(handle, "hwc_getAlignedSize");
        mGetFlipOffset = (hwc_func2)dlsym(handle, "hwc_getFlipOffset");
        mGetTiling = (hwc_func2)dlsym(handle, "hwc_getTiling");
        mAlterFormat = (hwc_func2)dlsym(handle, "hwc_alterFormat");
        mLockSurface = (hwc_func1)dlsym(handle, "hwc_lockSurface");
        mUnlockSurface = (hwc_func1)dlsym(handle, "hwc_unlockSurface");
    }
    memset(path, 0, sizeof(path));
    snprintf(path, PATH_MAX, "%s/%s", LIB_PATH, GPUENGINE);

    handle = dlopen(path, RTLD_NOW);
    if (handle == NULL) {
        ALOGI("no %s found, switch to 3D composite", path);
        mSetClipping = NULL;
        mBlitFunction = NULL;
        mOpenEngine = NULL;
        mCloseEngine = NULL;
        mClearFunction = NULL;
        mEnableFunction = NULL;
        mDisableFunction = NULL;
        mFinishEngine = NULL;
        mQueryFeature = NULL;
    }
    else {
        mSetClipping = (hwc_func5)dlsym(handle, "g2d_set_clipping");
        mBlitFunction = (hwc_func3)dlsym(handle, "g2d_blitEx");
        if (mBlitFunction == NULL) {
            mBlitFunction = (hwc_func3)dlsym(handle, "g2d_blit");
        }
        mOpenEngine = (hwc_func1)dlsym(handle, "g2d_open");
        mCloseEngine = (hwc_func1)dlsym(handle, "g2d_close");
        mClearFunction = (hwc_func2)dlsym(handle, "g2d_clear");
        mEnableFunction = (hwc_func2)dlsym(handle, "g2d_enable");
        mDisableFunction = (hwc_func2)dlsym(handle, "g2d_disable");
        mFinishEngine = (hwc_func1)dlsym(handle, "g2d_finish");
        mQueryFeature = (hwc_func3)dlsym(handle, "g2d_query_feature");
        openEngine(&mHandle);
    }
}

Composer::~Composer()
{
    MemoryManager* pManager = MemoryManager::getInstance();
    if (mDimBuffer != NULL) {
        pManager->releaseMemory(mDimBuffer);
    }

    for (int i = 0; i < N; i++)
	if (mRotBuffers[i] != NULL) {
		pManager->releaseMemory(mRotBuffers[i]);
	}

    if (mHandle != NULL) {
        closeEngine(mHandle);
    }

    ALOGE("~Composer()");

}

bool Composer::isValid()
{
    return (mHandle != NULL && mBlitFunction != NULL);
}

int Composer::checkDimBuffer()
{
    if (mTarget == NULL) {
        return 0;
    }

    if ((mDimBuffer != NULL) && (mTarget->width == mDimBuffer->width &&
        mTarget->height == mDimBuffer->height &&
        mTarget->fslFormat == mDimBuffer->fslFormat)) {
        return 0;
    }

    MemoryManager* pManager = MemoryManager::getInstance();
    if (mDimBuffer != NULL) {
        pManager->releaseMemory(mDimBuffer);
    }

    MemoryDesc desc;
    desc.mWidth = mTarget->width;
    desc.mHeight = mTarget->height;
    desc.mFormat = mTarget->format;
    desc.mFslFormat = mTarget->fslFormat;
    desc.mProduceUsage |= USAGE_HW_COMPOSER |
                          USAGE_HW_2D | USAGE_HW_RENDER;
    desc.checkFormat();
    int ret = pManager->allocMemory(desc, &mDimBuffer);
    if (ret == 0) {
        Rect rect;
        rect.left = rect.top = 0;
        rect.right = mTarget->width;
        rect.bottom = mTarget->height;
        clearRect(mDimBuffer, rect);
    }

    return ret;
}

int Composer::allocRotBuffer(int width, int height, int transform)
{
    mRotBuffer = NULL;

    if (mTarget == NULL) return 0;

    MemoryDesc desc;

    desc.mWidth  = transform ? height : width;
    desc.mHeight = transform ? width  : height; 

    for (int i = 0; i < N; i++) {
	//if (mRotBuffers[i] != NULL)
	if ((desc.mWidth <= mRotBuffers[i]->width) &&
	    (desc.mHeight <= mRotBuffers[i]->height) &&
	    (mTarget->fslFormat == mRotBuffers[i]->fslFormat)) {
		mRotBuffer = mRotBuffers[i];
        	return 0;
	}
    }

    if (N == 64) return 0;

    desc.mFormat = mTarget->format;
    desc.mFslFormat = mTarget->fslFormat;

    //ALOGE("allocRotBuffer mFormat:0x%x, mFslFormat:0x%x, mWidth:%d, mHeight:%d", desc.mFormat, desc.mFslFormat, desc.mWidth, desc.mHeight);


    desc.mProduceUsage |= USAGE_HW_COMPOSER | USAGE_HW_2D | USAGE_HW_RENDER;
    desc.checkFormat();

    MemoryManager* pManager = MemoryManager::getInstance();
    int ret = pManager->allocMemory(desc, &mRotBuffers[N]);
    if (!ret)
	mRotBuffer = mRotBuffers[N++];
    return ret;
}

int Composer::finishComposite()
{
    finishEngine(mHandle);

    if (N > 0) {
	    MemoryManager* pManager = MemoryManager::getInstance();
	    for (int i = 0; i < N; i++)
		//if (mRotBuffers[i] != NULL) {
			pManager->releaseMemory(mRotBuffers[i]);
		//	mRotBuffers[i] = NULL;
		//}
    }

    N = 0; //mRotBuffers[0] == NULL ? 0 : 1;
    mRotBuffer = NULL;

    //ALOGE("finishComposite()");

    return 0;
}

int Composer::setRenderTarget(Memory* memory)
{
    mTarget = memory;
    return 0;
}

int Composer::clearRect(Memory* target, Rect& rect)
{
    if (target == NULL || rect.isEmpty()) {
        return 0;
    }

    struct g2d_surfaceEx surfaceX;
    memset(&surfaceX, 0, sizeof(surfaceX));
    struct g2d_surface& surface = surfaceX.base;
    ALOGV("clearRect: rect(l:%d,t:%d,r:%d,b:%d)",
            rect.left, rect.top, rect.right, rect.bottom);
    setG2dSurface(surfaceX, target, rect);
    surface.clrcolor = 0xff << 24;
    clearFunction(mHandle, &surface);

    return 0;
}

int Composer::clearWormHole(LayerVector& layers)
{
    if (mTarget == NULL) {
        ALOGE("clearWormHole: no effective render buffer");
        return -EINVAL;
    }

    // calculate opaque region.
    Region opaque;
    size_t count = layers.size();
    for (size_t i=0; i<count; i++) {
        Layer* layer = layers[i];
        if (!layer->busy){
            ALOGE("clearWormHole: compose invalid layer");
            continue;
        }

        if ((layer->blendMode == BLENDING_NONE) ||
             (i==0 && layer->blendMode == BLENDING_PREMULT) ||
             ((i!=0) && (layer->blendMode == BLENDING_DIM) &&
              ((layer->color >> 24)&0xff) == 0xff)) {
            opaque.orSelf(layer->visibleRegion);
        }
    }

    // calculate worm hole.
    Region screen(Rect(mTarget->width, mTarget->height));
    screen.subtractSelf(opaque);
    const Rect *holes = NULL;
    size_t numRect = 0;
    holes = screen.getArray(&numRect);
    // clear worm hole.
    struct g2d_surfaceEx surfaceX;
    memset(&surfaceX, 0, sizeof(surfaceX));
    struct g2d_surface& surface = surfaceX.base;
    for (size_t i=0; i<numRect; i++) {
        if (holes[i].isEmpty()) {
            continue;
        }

        Rect& rect = (Rect&)holes[i];
        ALOGV("clearhole: hole(l:%d,t:%d,r:%d,b:%d)",
                rect.left, rect.top, rect.right, rect.bottom);
        setG2dSurface(surfaceX, mTarget, rect);
        surface.clrcolor = 0xff << 24;
        clearFunction(mHandle, &surface);
    }

    return 0;
}

int Composer::composeLayer(Layer* layer, bool bypass)
{
    if (layer == NULL || mTarget == NULL) {
        ALOGE("composeLayer: invalid layer or target");
        return -EINVAL;
    }

    if (bypass && layer->isSolidColor()) {
        ALOGV("composeLayer dim layer bypassed");
        return 0;
    }

    Rect srect = layer->sourceCrop;
    Rect drect = layer->displayFrame;
    Rect rrect;

    struct g2d_surfaceEx dSurfaceX;
    struct g2d_surface& dSurface = dSurfaceX.base;

    if ((srect.isEmpty() && !layer->isSolidColor()) || drect.isEmpty()) {
        ALOGE("composeLayer: invalid srect or drect");
        return 0;
    }

    if (layer->isSolidColor()) {
        checkDimBuffer();
    }

//    if (mRotBuffers[0] == NULL)
//	allocRotBuffer(1920/*1080*/,1920, 0);
	
    size_t count = 0;
    const Rect* visible = layer->visibleRegion.getArray(&count);
    for (size_t i=0; i<count; i++) {
        Rect srect = layer->sourceCrop;

        Rect clip = visible[i];
        if (clip.isEmpty()) {
            ALOGV("composeLayer: invalid clip");
            continue;
        }

        clip.intersect(drect, &clip);
        if (clip.isEmpty()) {
            ALOGV("composeLayer: invalid clip rect");
            continue;
        }

        setClipping(srect, drect, clip, layer->transform);
/*
        ALOGE("index:%d, i:%d sourceCrop(l:%d,t:%d,r:%d,b:%d), "
             "visible(l:%d,t:%d,r:%d,b:%d), "
             "display(l:%d,t:%d,r:%d,b:%d)", layer->index, (int)i,
             srect.left, srect.top, srect.right, srect.bottom,
             clip.left, clip.top, clip.right, clip.bottom,
             drect.left, drect.top, drect.right, drect.bottom);
        ALOGE("target phys:0x%x, layer phys:0x%x",
                (int)mTarget->phys, (int)layer->handle->phys);
        ALOGE("transform:0x%x, blend:0x%x, alpha:0x%x solid:%d",
                layer->transform, layer->blendMode, layer->planeAlpha, layer->isSolidColor() ? 1:0);
*/
        struct g2d_surfaceEx sSurfaceX;
        memset(&sSurfaceX, 0, sizeof(sSurfaceX));
        struct g2d_surface& sSurface = sSurfaceX.base;

        if (!layer->isSolidColor()) {
	    if (layer->handle == NULL) continue;
            setG2dSurface(sSurfaceX, layer->handle, srect);
	} else {
	    if (mDimBuffer == NULL) continue;
            setG2dSurface(sSurfaceX, mDimBuffer, drect);
	}

	memset(&dSurfaceX, 0, sizeof(dSurfaceX));
        setG2dSurface(dSurfaceX, mTarget, drect);

        convertRotation(layer->transform, sSurface, dSurface);

//workaround for "e8151: PXP: Rotation Engine alignment and operation combination limitations"
	if (dSurface.rot != G2D_ROTATION_0) { //90 180 270

		struct g2d_surfaceEx rSurfaceX;
		memset(&rSurfaceX, 0, sizeof(rSurfaceX));
    		struct g2d_surface& rSurface = rSurfaceX.base;

		int r = ((dSurface.rot == G2D_ROTATION_90) || (dSurface.rot == G2D_ROTATION_270)) ? 1 : 0;

		allocRotBuffer(layer->handle->width,layer->handle->height, r);

		if (mRotBuffer == NULL) {
		        ALOGE("rotBuffer == NULL !");
			continue;
		}

		if (r) {
			rrect.left   = sSurface.top;
			rrect.right  = sSurface.bottom;
			rrect.top    = sSurface.left;
			rrect.bottom = sSurface.right;
		} else {
			rrect.left   = sSurface.left;
			rrect.right  = sSurface.right;
			rrect.top    = sSurface.top;
			rrect.bottom = sSurface.bottom;
		}

	        setG2dSurface(rSurfaceX, mRotBuffer, rrect);
	        rSurface.rot = dSurface.rot;
		dSurface.rot = sSurface.rot;
		sSurface.rot = G2D_ROTATION_0; //discard flip

		enableFunction(mHandle, G2D_BLEND, true);
		enableFunction(mHandle, G2D_GLOBAL_ALPHA, true);
	        sSurface.global_alpha = layer->planeAlpha;
		sSurface.blendfunc = G2D_ONE; //enable alpha
		rSurface.blendfunc = G2D_ZERO;
	        blitSurface(&sSurfaceX, &rSurfaceX); // just rotate please
		rSurface.rot = G2D_ROTATION_0;


	        if (layer->blendMode == BLENDING_NONE || bypass) {
			enableFunction(mHandle, G2D_BLEND, false);
			enableFunction(mHandle, G2D_GLOBAL_ALPHA, false);
		}

		if (!bypass)
			convertBlending(layer->blendMode, rSurface, dSurface);

	        rSurface.global_alpha = layer->planeAlpha;

		blitSurface(&rSurfaceX, &dSurfaceX);

	        if (layer->blendMode != BLENDING_NONE && !bypass) {
			enableFunction(mHandle, G2D_BLEND, false);
			enableFunction(mHandle, G2D_GLOBAL_ALPHA, false);
		}

		continue;
	}

	if (!bypass)
	       	convertBlending(layer->blendMode, sSurface, dSurface);
	sSurface.global_alpha = layer->planeAlpha;

	if (layer->blendMode != BLENDING_NONE && !bypass) {
		enableFunction(mHandle, G2D_GLOBAL_ALPHA, true);
		enableFunction(mHandle, G2D_BLEND, true);
	}

	blitSurface(&sSurfaceX, &dSurfaceX);

	if (layer->blendMode != BLENDING_NONE && !bypass) {
		enableFunction(mHandle, G2D_BLEND, false);
		enableFunction(mHandle, G2D_GLOBAL_ALPHA, false);
	}
    }

    return 0;
}

int Composer::setG2dSurface(struct g2d_surfaceEx& surfaceX, Memory *handle, Rect& rect)
{
    int alignWidth = 0, alignHeight = 0;
    struct g2d_surface& surface = surfaceX.base;

    int ret = getAlignedSize(handle, NULL, &alignHeight);
    if (ret != 0) {
        alignHeight = handle->height;
    }

    alignWidth = handle->stride;
    surface.format = convertFormat(handle->fslFormat, handle);
    surface.stride = alignWidth;
    enum g2d_tiling tile = G2D_LINEAR;
    getTiling(handle, &tile);
    surfaceX.tiling = tile;

    int offset = 0;
    getFlipOffset(handle, &offset);
    surface.planes[0] = (int)handle->phys + offset;

    switch (surface.format) {
        case G2D_RGB565:
        case G2D_YUYV:
        case G2D_RGBA8888:
        case G2D_BGRA8888:
        case G2D_RGBX8888:
        case G2D_BGRX8888:
            break;

        case G2D_NV16:
        case G2D_NV12:
        case G2D_NV21:
            surface.planes[1] = surface.planes[0] + surface.stride * alignHeight;
            break;

        case G2D_I420:
        case G2D_YV12: {
            int c_stride = (alignWidth/2+15)/16*16;
            int stride = alignWidth;

            surface.stride = alignWidth;
            if (surface.format == G2D_I420) {
                surface.planes[1] = surface.planes[0] + stride * handle->height;
                surface.planes[2] = surface.planes[1] + c_stride * handle->height/2;
            }
            else {
                surface.planes[2] = surface.planes[0] + stride * handle->height;
                surface.planes[1] = surface.planes[2] + c_stride * handle->height/2;
            }
            } break;

        default:
            ALOGI("does not support format:%d", surface.format);
            break;
    }
    surface.left = rect.left;
    surface.top = rect.top;
    surface.right = rect.right;
    surface.bottom = rect.bottom;
    surface.width = handle->width;
    surface.height = handle->height;

    return 0;
}

enum g2d_format Composer::convertFormat(int format, Memory *handle)
{
    enum g2d_format halFormat;
    switch (format) {
        case FORMAT_RGBA8888:
            halFormat = G2D_RGBA8888;
            break;
        case FORMAT_RGBX8888:
            halFormat = G2D_RGBX8888;
            break;
        case FORMAT_RGB565:
            halFormat = G2D_RGB565;
            break;
        case FORMAT_BGRA8888:
            halFormat = G2D_BGRA8888;
            break;

        case FORMAT_NV21:
            halFormat = G2D_NV21;
            break;
        case FORMAT_NV12:
            halFormat = G2D_NV12;
            break;

        case FORMAT_I420:
            halFormat = G2D_I420;
            break;
        case FORMAT_YV12:
            halFormat = G2D_YV12;
            break;

        case FORMAT_NV16:
            halFormat = G2D_NV16;
            break;
        case FORMAT_YUYV:
            halFormat = G2D_YUYV;
            break;

        default:
            ALOGE("unsupported format:0x%x", format);
            halFormat = G2D_RGBA8888;
            break;
    }

    halFormat = alterFormat(handle, halFormat);
    return halFormat;
}

int Composer::convertRotation(int transform, struct g2d_surface& src,
                        struct g2d_surface& dst)
{
    switch (transform) {
        case 0:
            dst.rot = G2D_ROTATION_0;
            break;
        case TRANSFORM_ROT90:
            dst.rot =  G2D_ROTATION_90;
            break;
        case TRANSFORM_FLIPH | TRANSFORM_FLIPV:
            dst.rot =  G2D_ROTATION_180;
            break;
        case TRANSFORM_FLIPH | TRANSFORM_FLIPV
             | HAL_TRANSFORM_ROT_90:
            dst.rot =  G2D_ROTATION_270;
            break;
        case TRANSFORM_FLIPH:
//            dst.rot =  G2D_FLIP_H;
              src.rot =  G2D_FLIP_H;
            break;
        case TRANSFORM_FLIPV:
//            dst.rot =  G2D_FLIP_V;
            src.rot =  G2D_FLIP_V;
            break;
        case TRANSFORM_FLIPH | TRANSFORM_ROT90:
            dst.rot =  G2D_ROTATION_90;
            src.rot =  G2D_FLIP_H;
            break;
        case TRANSFORM_FLIPV | TRANSFORM_ROT90:
            dst.rot =  G2D_ROTATION_90;
            src.rot =  G2D_FLIP_V;
            break;
        default:
            dst.rot =  G2D_ROTATION_0;
            break;
    }

    return 0;
}


//    BLENDING_NONE     = 0x0100,
int Composer::convertBlending(int blending, struct g2d_surface& src,
                        struct g2d_surface& dst)
{
    switch (blending) {
        case BLENDING_PREMULT: //0x0105
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        case BLENDING_COVERAGE: //0x0405
            src.blendfunc = G2D_SRC_ALPHA;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        case BLENDING_DIM: //0x0805
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        default:
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;
    }

    return 0;
}

int Composer::getAlignedSize(Memory *handle, int *width, int *height)
{
    if (mGetAlignedSize == NULL) {
        return -EINVAL;
    }

    return (*mGetAlignedSize)(handle, (void*)width, (void*)height);
}

int Composer::getFlipOffset(Memory *handle, int *offset)
{
    if (mGetFlipOffset == NULL) {
        return -EINVAL;
    }

    return (*mGetFlipOffset)(handle, (void*)offset);
}

int Composer::getTiling(Memory *handle, enum g2d_tiling* tile)
{
    if (mGetTiling == NULL) {
        return -EINVAL;
    }

    return (*mGetTiling)(handle, (void*)tile);
}

enum g2d_format Composer::alterFormat(Memory *handle, enum g2d_format format)
{
    if (mAlterFormat == NULL) {
        return format;
    }

    return (enum g2d_format)(*mAlterFormat)(handle, (void*)format);
}

int Composer::lockSurface(Memory *handle)
{
    if (mLockSurface == NULL) {
        return -EINVAL;
    }

    return (*mLockSurface)(handle);
}

int Composer::unlockSurface(Memory *handle)
{
    if (mUnlockSurface == NULL) {
        return -EINVAL;
    }

    return (*mUnlockSurface)(handle);
}

int Composer::setClipping(Rect& src, Rect& dst, Rect& clip, int rotation)
{
    if (mSetClipping == NULL) {
        return -EINVAL;
    }

    return (*mSetClipping)(mHandle, (void*)(intptr_t)clip.left,
            (void*)(intptr_t)clip.top, (void*)(intptr_t)clip.right,
            (void*)(intptr_t)clip.bottom);
}

int Composer::blitSurface(struct g2d_surfaceEx *srcEx, struct g2d_surfaceEx *dstEx)
{
    if (mBlitFunction == NULL) {
        return -EINVAL;
    }

    return (*mBlitFunction)(mHandle, srcEx, dstEx);
}

int Composer::openEngine(void** handle)
{
    if (mOpenEngine == NULL) {
        return -EINVAL;
    }

    return (*mOpenEngine)((void*)handle);
}

int Composer::closeEngine(void* handle)
{
    if (mCloseEngine == NULL) {
        return -EINVAL;
    }

    return (*mCloseEngine)(handle);
}

int Composer::clearFunction(void* handle, struct g2d_surface* area)
{
    if (mClearFunction == NULL) {
        return -EINVAL;
    }

    return (*mClearFunction)(handle, area);
}

int Composer::enableFunction(void* handle, enum g2d_cap_mode cap, bool enable)
{
    if (mEnableFunction == NULL || mDisableFunction == NULL) {
        return -EINVAL;
    }

    int ret = 0;
    if (enable) {
        ret = (*mEnableFunction)(handle, (void*)cap);
    }
    else {
        ret = (*mDisableFunction)(handle, (void*)cap);
    }

    return ret;
}

int Composer::finishEngine(void* handle)
{
    if (mFinishEngine == NULL) {
        return -EINVAL;
    }

    return (*mFinishEngine)(handle);
}

bool Composer::isFeatureSupported(g2d_feature feature)
{
    if (mQueryFeature == NULL) {
        return false;
    }

    int enable = 0;
    (*mQueryFeature)(mHandle, (void*)feature, (void*)&enable);
    return (enable != 0);
}

}


/*
ver
11-06 12:06:01.208   236   236 E display : index:0, i:0 sourceCrop(l:0,t:0,r:480,b:854), visible(l:0,t:0,r:480,b:854), display(l:0,t:0,r:480,b:854) main
11-06 12:06:01.208   236   236 E display : target phys:0x9ed00000, layer phys:0xa0e00000
11-06 12:06:01.208   236   236 E display : transform:0x0, blend:0x0, alpha:0xff solid:0

11-06 12:06:01.208   236   236 E display : index:1, i:0 sourceCrop(l:0,t:0,r:480,b:854), visible(l:0,t:0,r:480,b:854), display(l:0,t:0,r:480,b:854) menu
11-06 12:06:01.208   236   236 E display : target phys:0x9ed00000, layer phys:0x9f700000
11-06 12:06:01.208   236   236 E display : transform:0x0, blend:0x105, alpha:0xff solid:0

11-06 12:06:01.208   236   236 E display : index:2, i:0 sourceCrop(l:0,t:0,r:480,b:48), visible(l:0,t:806,r:480,b:854), display(l:0,t:806,r:480,b:854) nav bar
11-06 12:06:01.208   236   236 E display : target phys:0x9ed00000, layer phys:0x9f0c0000
11-06 12:06:01.208   236   236 E display : transform:0x0, blend:0x105, alpha:0xff solid:0



hor
11-06 12:02:21.548   236   236 E display : index:1, i:0 sourceCrop(l:0,t:0,r:854,b:480), visible(l:0,t:0,r:480,b:854), display(l:0,t:0,r:480,b:854) main
11-06 12:02:21.548   236   236 E display : target phys:0x9ed00000, layer phys:0x9f900000
11-06 12:02:21.548   236   236 E display : transform:0x4, blend:0x0, alpha:0xff solid:0

11-06 12:02:21.548   236   236 E display : index:0, i:0 sourceCrop(l:0,t:0,r:854,b:480), visible(l:0,t:0,r:480,b:854), display(l:0,t:0,r:480,b:854) menu
11-06 12:02:21.548   236   236 E display : target phys:0x9ed00000, layer phys:0x9f700000
11-06 12:02:21.548   236   236 E display : transform:0x4, blend:0x105, alpha:0xff solid:0

11-06 12:02:21.548   236   236 E display : index:3, i:0 sourceCrop(l:0,t:0,r:854,b:48), visible(l:0,t:0,r:48,b:854), display(l:0,t:0,r:48,b:854) nav bar
11-06 12:02:21.548   236   236 E display : target phys:0x9ed00000, layer phys:0x9f2c0000
11-06 12:02:21.548   236   236 E display : transform:0x4, blend:0x105, alpha:0xff solid:0





11-06 12:13:00.042   236   236 E display : index:0, i:0 sourceCrop(l:0,t:0,r:854,b:480), visible(l:0,t:0,r:480,b:854), display(l:0,t:0,r:480,b:854) main
11-06 12:13:00.042   236   236 E display : target phys:0x9e900000, layer phys:0x9ff00000
11-06 12:13:00.042   236   236 E display : transform:0x4, blend:0x0, alpha:0xff solid:0

11-06 12:13:00.042   236   236 E display : index:1, i:0 sourceCrop(l:0,t:0,r:854,b:24), visible(l:456,t:0,r:480,b:854), display(l:456,t:0,r:480,b:854) stat bar 
11-06 12:13:00.042   236   236 E display : target phys:0x9e900000, layer phys:0x9f4c0000
11-06 12:13:00.042   236   236 E display : transform:0x4, blend:0x105, alpha:0xff solid:0

11-06 12:13:00.042   236   236 E display : index:2, i:0 sourceCrop(l:0,t:0,r:854,b:48), visible(l:0,t:0,r:48,b:854), display(l:0,t:0,r:48,b:854) nav bar
11-06 12:13:00.042   236   236 E display : target phys:0x9e900000, layer phys:0x9f2c0000
11-06 12:13:00.042   236   236 E display : transform:0x4, blend:0x105, alpha:0xff solid:0
*/
