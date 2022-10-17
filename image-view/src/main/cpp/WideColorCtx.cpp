/*
 * Copyright (C) 2017 The Android Open Source Project
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
 *
 */
#include "ImageViewEngine.h"
#include "android_debug.h"
#include "ColorSpace.h"
#include "math/mat4.h"

struct APP_WIDECOLOR_MODE_CFG {
  DISPLAY_COLORSPACE space_;
  DISPLAY_FORMAT fmt_;
};
struct GL_WIDECOLOR_MODE_CFG {
  EGLint  space_;
  EGLint  r_, g_, b_, a_;
};

static const APP_WIDECOLOR_MODE_CFG appWideColorCfg[] = {
    { DISPLAY_COLORSPACE::P3_PASSTHROUGH, DISPLAY_FORMAT::R8G8B8A8_REV },
    { DISPLAY_COLORSPACE::P3_PASSTHROUGH, DISPLAY_FORMAT::R10G10B10_A2_REV},
    { DISPLAY_COLORSPACE::P3_PASSTHROUGH, DISPLAY_FORMAT::RGBA_FP16},
    { DISPLAY_COLORSPACE::P3, DISPLAY_FORMAT::R8G8B8A8_REV },
    { DISPLAY_COLORSPACE::P3, DISPLAY_FORMAT::R10G10B10_A2_REV},
    { DISPLAY_COLORSPACE::P3, DISPLAY_FORMAT::RGBA_FP16},
    { DISPLAY_COLORSPACE::SRGB,DISPLAY_FORMAT::R8G8B8A8_REV},
};
static GL_WIDECOLOR_MODE_CFG glWideColorCfg[] = {
    {
        EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHROUGH_EXT,
        8, 8, 8, 8
    },
    {
        EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHROUGH_EXT,
        10, 10, 10, 2
    },
    {
        EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHROUGH_EXT,
        16, 16, 16, 16
    },
    {
        EGL_GL_COLORSPACE_DISPLAY_P3_EXT,
        8, 8, 8, 8
    },
    {
        EGL_GL_COLORSPACE_DISPLAY_P3_EXT,
        10, 10, 10, 2
    },
    {
        EGL_GL_COLORSPACE_DISPLAY_P3_EXT,
        16, 16, 16, 16
    },
    {
        EGL_GL_COLORSPACE_SRGB_KHR,
        8, 8, 8, 8
    },
};


static bool CheckRequiredEGLExt(EGLDisplay disp, std::vector<std::string>& exts) {
  std::string eglExt(eglQueryString(disp, EGL_EXTENSIONS)) ;
  for(auto& ext : exts) {
    std::string::size_type extPos = eglExt.find(ext);
    if (extPos == std::string::npos) {
      return false;
    }
  }
  return true;
}

#define EPSILON  0.000001f
#define HAS_GAMMA(x) (std::abs(x) > EPSILON && std::abs((x) - 1.0f) > EPSILON)
#define CLIP_COLOR(color, max) ((color > max) ? max : ((color > 0) ? color : 0))

/*
 * ApplyTransform8888()
 *    dst = matrix * src
 *    and clamp the result to 0 -- 255
 */
static bool TransformR8G8B8(uint8_t* dst, uint8_t srcR, uint8_t srcG, uint8_t srcB,
                              mathfu::mat3& transMatrix) {
//    ASSERT(src && dst, "Wrong image store to %s", __FUNCTION__);

    int32_t m00, m01, m02, m10, m11, m12, m20, m21, m22;
    m00 = static_cast<int32_t>(transMatrix(0, 0) * 1024 + 0.5f),
            m01 = static_cast<int32_t>(transMatrix(0, 1) * 1024 + 0.5f);
    m02 = static_cast<int32_t>(transMatrix(0, 2) * 1024 + 0.5f);
    m10 = static_cast<int32_t>(transMatrix(1, 0) * 1024 + 0.5f);
    m11 = static_cast<int32_t>(transMatrix(1, 1) * 1024 + 0.5f);
    m12 = static_cast<int32_t>(transMatrix(1, 2) * 1024 + 0.5f);
    m20 = static_cast<int32_t>(transMatrix(2, 0) * 1024 + 0.5f);
    m21 = static_cast<int32_t>(transMatrix(2, 1) * 1024 + 0.5f);
    m22 = static_cast<int32_t>(transMatrix(2, 2) * 1024 + 0.5f);

    int32_t r, g, b;
    r = (m00 * srcR + m01 * srcG + m02 * srcB + 512) >> 10;
    g = (m10 * srcR + m11 * srcG + m12 * srcB + 512) >> 10;
    b = (m20 * srcR + m21 * srcG + m22 * srcB + 512) >> 10;
    *dst++ = static_cast<uint8_t>(CLIP_COLOR(r, 255));
    *dst++ = static_cast<uint8_t>(CLIP_COLOR(g, 255));
    *dst++ = static_cast<uint8_t>(CLIP_COLOR(b, 255));

    return true;
}

/*
 * CreateGammaEncodeTable():
 *     sRGB =
 *        12.92 * LinearRGB            0 < LinearRGB < 0.0031308
 *        1.055 * power(LinearRGB, gamma)-0.055 0.0031308 <= LinarRGB <= 1.0f
 */
static void CreateGammaEncodeTable(float gamma, std::vector<uint8_t>& table) {
    ASSERT(gamma < 1.0f, "Wrong Gamma (%f) for encoding", gamma);
    uint32_t maxPixeli = ( 1<<8 ) - 1;
    float maxPixelf = static_cast<float>(maxPixeli);

    uint32_t maxLinearVal = static_cast<uint32_t>(0.0031308f * maxPixeli);

    table.resize(0);
    for(uint32_t idx = 0; idx < maxLinearVal; idx++) {
        double val = idx * 12.92 + .5f;
        table.push_back(static_cast<uint8_t>(val));
    }

    for (uint32_t idx = maxLinearVal; idx <= maxPixeli; idx++) {
        double val = (1.055f * pow(idx / maxPixelf, gamma) - 0.055f);
        val = val * maxPixeli + 0.5f;
        table.push_back(static_cast<uint8_t>(CLIP_COLOR(val, maxPixelf)));
    }
}
/*
 * CreateGammaEncodeTable()
 *    Retrieve linear RGB data
 *    Linear =  sRGB / 12.92    0 <= sRGB < 0.04045
 *              pow((sRGB + 0.055)/1.055, gamma)
 */
static void CreateGammaDecodeTable(float gamma, std::vector<uint8_t>&table) {
    ASSERT(gamma > 1.0, "Wrong Gamma(%f) for decoding", gamma);
    uint32_t maxPixeli = ( 1<<8 ) - 1;
    float maxPixelf = static_cast<float>(maxPixeli);

    uint32_t maxLinearVal = static_cast<uint32_t>(0.04045 * maxPixeli);
    for(uint32_t idx = 0; idx < maxLinearVal; idx++) {
        double val = idx / 12.92 + .5f;
        table.push_back(static_cast<uint8_t>(val));
    }

    for (uint32_t idx = maxLinearVal; idx <= maxPixeli; idx++) {
        double val;
        val = (idx / maxPixelf + 0.055f) / 1.055f;
        val = pow(val, gamma) * maxPixeli + 0.5f;
        table.push_back(static_cast<uint8_t>(CLIP_COLOR(val, maxPixelf)));
    }
}

/*
 * ApplyGamma()
 *    Perform gamma lookup for RGBA8888 format
 */
static bool ApplyGamma(uint8_t* dst, uint8_t srcR, uint8_t srcG, uint8_t srcB, std::vector<uint8_t>& gammaTable) {

    uint8_t* imgDst = static_cast<uint8_t*>(dst);

    *imgDst++ = gammaTable[srcR];
    *imgDst++ = gammaTable[srcG];
    *imgDst++ = gammaTable[srcB];

    return true;
}

/*
 * Initialize an EGL eglContext_ for the current display_.
 *
 * Supported Format:
 *     8888:     EGL_COLOR_COMPONENT_TYPE_EXT  EGL_COLOR_COMPONENT_TYPE_FIXED_EXT
 *     101010102:EGL_COLOR_COMPONENT_TYPE_EXT  EGL_COLOR_COMPONENT_TYPE_FIXED_EXT
 *     16161616: EGL_COLOR_COMPONENT_TYPE_EXT  EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT
 *
 */
bool ImageViewEngine::CreateWideColorCtx(WIDECOLOR_MODE mode) {
    EGLBoolean status;

    const android::ColorSpace srgb(android::ColorSpace::sRGB());
    const android::ColorSpace displayP3(android::ColorSpace::DisplayP3());
    const android::ColorSpace dciP3(android::ColorSpace::DCIP3());
    const android::ColorSpace bt2020(android::ColorSpace::BT2020());

    android::mat4 mSrgbToXyz;
    android::mat4 mDisplayP3ToXyz;
    android::mat4 mDciP3ToXyz;
    android::mat4 mBt2020ToXyz;
    android::mat4 mXyzToSrgb;
    android::mat4 mXyzToDisplayP3;
    android::mat4 mXyzToDciP3;
    android::mat4 mXyzToBt2020;
    android::mat4 mSrgbToDisplayP3;
    android::mat4 mSrgbToBt2020;
    android::mat4 mDisplayP3ToSrgb;
    android::mat4 mDisplayP3ToBt2020;
    android::mat4 mBt2020ToSrgb;
    android::mat4 mBt2020ToDisplayP3;
    // android::mat4 test;

    // no chromatic adaptation needed since all color spaces use D65 for their white points.
    mSrgbToXyz = android::mat4(srgb.getRGBtoXYZ());
    mDisplayP3ToXyz = android::mat4(displayP3.getRGBtoXYZ());
    mDciP3ToXyz = android::mat4(dciP3.getRGBtoXYZ());
    mBt2020ToXyz = android::mat4(bt2020.getRGBtoXYZ());

    mXyzToSrgb = android::mat4(srgb.getXYZtoRGB());
    mXyzToDisplayP3 = android::mat4(displayP3.getXYZtoRGB());
    mXyzToDciP3 = android::mat4(dciP3.getXYZtoRGB());
    mXyzToBt2020 = android::mat4(bt2020.getXYZtoRGB());

    // Compute sRGB to Display P3 and BT2020 transform matrix.
    // NOTE: For now, we are limiting output wide color space support to
    // Display-P3 and BT2020 only.
    mSrgbToDisplayP3 = mXyzToDisplayP3 * mSrgbToXyz;
    mSrgbToBt2020 = mXyzToBt2020 * mSrgbToXyz;

    // Compute Display P3 to sRGB and BT2020 transform matrix.
    mDisplayP3ToSrgb = mXyzToSrgb * mDisplayP3ToXyz;
    mDisplayP3ToBt2020 = mXyzToBt2020 * mDisplayP3ToXyz;

    // Compute BT2020 to sRGB and Display P3 transform matrix
    mBt2020ToSrgb = mXyzToSrgb * mBt2020ToXyz;
    mBt2020ToDisplayP3 = mXyzToDisplayP3 * mBt2020ToXyz;

    LOGD("========mBt2020ToSrgb=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mBt2020ToSrgb.asArray()[i]);
    }
    LOGD("========mBt2020ToXyz=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mBt2020ToXyz.asArray()[i]);
    }
    LOGD("========mSrgbToXyz=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mSrgbToXyz.asArray()[i]);
    }
    LOGD("========mXyzToSrgb=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mXyzToSrgb.asArray()[i]);
    }
    LOGD("========mDisplayP3ToXyz=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mDisplayP3ToXyz.asArray()[i]);
    }
    LOGD("========mDisplayP3TosRGB=========");
        for (int i = 0; i < 16; i++) {
            LOGD("%7.10ff", mDisplayP3ToSrgb.asArray()[i]);
        }
    LOGD("========mXyzToDisplayP3=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mXyzToDisplayP3.asArray()[i]);
    }
    LOGD("========mDciP3ToXyz=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mDciP3ToXyz.asArray()[i]);
    }
    LOGD("========mXyzToDciP3=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mXyzToDciP3.asArray()[i]);
    }
    LOGD("========mXyzToBt2020=========");
    for (int i = 0; i < 16; i++) {
        LOGD("%7.10ff", mXyzToBt2020.asArray()[i]);
    }

    std::vector<uint8_t> gammaTableDec;
    CreateGammaDecodeTable(1.0f/DEFAULT_P3_IMAGE_GAMMA, gammaTableDec);

    std::vector<uint8_t> gammaTableEnc;
    CreateGammaEncodeTable(DEFAULT_DISPLAY_GAMMA, gammaTableEnc);

    mathfu::mat3 p3ToXyz = mathfu::mat3(0.4865709245f, 0.2289745510f, 0.000000000f,
                                        0.2656676769f, 0.6917385459f, 0.0451133996f,
                                        0.1982172877f, 0.0792869106f, 1.0439443588f);

    mathfu::mat3 xyzToSrgb = mathfu::mat3( 3.2409696579f, -0.9692436457f, 0.0556300320f,
                                           -1.5373830795f, 1.8759675026f, -0.2039768547f,
                                           -0.4986107349f, 0.0415550880f, 1.0569714308f);

    uint8_t dstBits[3];

    // transform from P3 to sRGB

    // const vec3 color = vec3(234.0, 51.0, 36.0); => (255.0, 0.0, 0.0)
    // const vec3 color = vec3(117.0, 251.0, 76.0); => (1.0. 254.0, 0.0)
    // const vec3 color = vec3(8.0, 0.0, 245.0); => (3.0, 0.0, 255.0)

//    for (int16_t r = 0; r <= 255; r++) {
//        for (int16_t g = 0; g <= 255; g++) {
//            for (int16_t b = 0; b <= 255; b++) {
//                ApplyGamma(dstBits, r, g, b, gammaTableDec);
//
//                mathfu::mat3 matrix = xyzToSrgb * p3ToXyz;
//
//                TransformR8G8B8(dstBits, dstBits[0], dstBits[1], dstBits[2], matrix);
//
//                ApplyGamma(dstBits, dstBits[0], dstBits[1], dstBits[2], gammaTableEnc);
//
//                LOGD("TONY P3(%03d, %03d, %03d) -> sRGB(%03d, %03d, %03d)",
//                     r, g, b,
//                     dstBits[0], dstBits[1], dstBits[2]);
//            }
//        }
//    }

    mathfu::mat3 srgbToXyz = mathfu::mat3(0.4123908281f, 0.2126390338f, 0.0193308201f,
                                          0.3575843275f, 0.7151686549f, 0.1191947237f,
                                          0.1804807931f, 0.0721923113f, 0.9505321383f);

    mathfu::mat3 xyzToP3 = mathfu::mat3(2.4934973717f, -0.8294889927f, 0.0358458459f,
                                        -0.9313836098f, 1.7626641989f, -0.0761724263f,
                                        -0.4027108550f, 0.0236246940f, 0.9568846226f);

    // transform from sRGB to P3
    for (int16_t r = 0; r <= 255; r++) {
        for (int16_t g = 0; g <= 255; g++) {
            for (int16_t b = 0; b <= 255; b++) {
                ApplyGamma(dstBits, r, g, b, gammaTableDec);

                mathfu::mat3 matrix = xyzToP3 * srgbToXyz;

                TransformR8G8B8(dstBits, dstBits[0], dstBits[1], dstBits[2], matrix);

                ApplyGamma(dstBits, dstBits[0], dstBits[1], dstBits[2], gammaTableEnc);

                LOGD("TONY sRGB(%03d, %03d, %03d) -> P3(%03d, %03d, %03d)",
                     r, g, b,
                     dstBits[0], dstBits[1], dstBits[2]);
            }
        }
    }

  std::vector<EGLint> attributes {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_BLUE_SIZE,  glWideColorCfg[mode].b_,
      EGL_GREEN_SIZE, glWideColorCfg[mode].g_,
      EGL_RED_SIZE,   glWideColorCfg[mode].r_,
      EGL_ALPHA_SIZE, glWideColorCfg[mode].a_,
  };

  // for RGBA888, still set to EGL_COLOR_COMPONENT_TYPE_FIXED_EXT
  if (mode == WIDECOLOR_MODE::P3_FP16) {
    attributes.push_back(EGL_COLOR_COMPONENT_TYPE_EXT);
    attributes.push_back(EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT);
  } else {
    attributes.push_back(EGL_COLOR_COMPONENT_TYPE_EXT);
    attributes.push_back(EGL_COLOR_COMPONENT_TYPE_FIXED_EXT);
  }
  attributes.push_back(EGL_NONE);

  // request just one matching config and use it
  EGLint    cfgCount = 1;
  EGLConfig config;
  status = eglChooseConfig(display_, attributes.data(), &config,
                           cfgCount, &cfgCount);
  if (!status || cfgCount != 1) {
    // if not support, report to caller so caller could choose another one
    LOGI("==== Chosen Config type(%d) is not supported", mode);
    return false;
  }

  // Create GL3 Context
  attributes.resize(0);
  attributes.push_back(EGL_CONTEXT_CLIENT_VERSION);
  attributes.push_back(3);
  attributes.push_back(EGL_NONE);
  eglContext_ = eglCreateContext(display_, config,
                                 EGL_NO_CONTEXT, attributes.data());
  if(eglContext_ == EGL_NO_CONTEXT) {
    return false;
  }

  EGLint format;
  eglGetConfigAttrib(display_, config, EGL_NATIVE_VISUAL_ID, &format);
  int32_t res = ANativeWindow_setBuffersGeometry(app_->window, 0, 0, format);
  if(res < 0) {
    eglDestroyContext(display_, eglContext_);
    eglContext_ = EGL_NO_CONTEXT;
    return false;
  }

  // Create Surface, which will turn on Display P3 wide gamut mode
  attributes.resize(0);
  attributes.push_back(EGL_GL_COLORSPACE_KHR);
  attributes.push_back(glWideColorCfg[mode].space_);
  attributes.push_back(EGL_NONE);
  surface_ = eglCreateWindowSurface(
      display_, config, app_->window, attributes.data());
  if(surface_ == EGL_NO_SURFACE) {
    LOGI("====Surface for mode (%d) is not supported", mode);
    eglDestroyContext(display_, eglContext_);
    eglContext_ = EGL_NO_CONTEXT;
    return false;
  }
  status = eglMakeCurrent(display_, surface_,
                          surface_, eglContext_);
  ASSERT(status, "eglMakeCurrent() Failed");

  dispColorSpace_ = appWideColorCfg[mode].space_;
  dispFormat_ = appWideColorCfg[mode].fmt_;

  eglQuerySurface(display_, surface_, EGL_WIDTH, &renderTargetWidth_);
  eglQuerySurface(display_, surface_, EGL_HEIGHT, &renderTargetHeight_);

  return true;
}

bool ImageViewEngine::CreateWideColorCtx(void) {
  EGLint major, minor;
  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  eglInitialize(display_, &major, &minor);

  /*
   * check for GL_EXT_gl_colorspace_display_p3_passthrough supportability:
   * implemented from Android 10+.
   * In this sample, the texture passing to GPU is already OETF applied.
   *
   * When p3_passthrough_ext is enabled:
   *  - OETF hardware is disabled when writing framebuffer
   *  - the texture content is OETF encoded
   *  - but sampler's EOTF should be disabled
   *  So sample sets texture to be RGBA ( to by pass sampler's EOTF hardware )
   * When p3_ext is enabled:
   *  - OETF hardware (when output from hardware blender) is enabled
   *  - the texture content is OETF encoded
   *  - sampler's EOTF should be enabled to avoid OETF twice
   *  So sample sets texture to be SRGB8_ALPHA8 to enable sampler's EOTF
   */
  std::vector<std::string> passthruExt {
      "EGL_KHR_gl_colorspace",
      "GL_EXT_gl_colorspace_display_p3_passthrough"
  };

  /*
   * Display-P3 needs EGL_EXT_gl_colorspace_display_p3 extension
   * which needs EGL 1.4. If not available, Display P3 is not supported
   * in that case, create legacy RGBA8888 eglContext_.
   */
  std::vector<std::string> p3Exts {
        "EGL_KHR_gl_colorspace",
        "EGL_EXT_gl_colorspace_display_p3"
    };

    // Default is P3 wide color gamut modes
    WIDECOLOR_MODE modes[] = {
            P3_R8G8B8A8_REV,
            P3_R10G10B10A2_REV,
            P3_FP16,
            SRGBA_R8G8B8A8_REV,
    };

    if (CheckRequiredEGLExt(display_, passthruExt)) {
        modes[0] = P3_PASSTHROUGH_R8G8B8A8_REV;
        modes[1] = P3_PASSTHROUGH_R10G10B10A2_REV;
        modes[2] = P3_PASSTHROUGH_FP16;
    } else
        if (!CheckRequiredEGLExt(display_, p3Exts)) {
        LOGW("====Warning: Display P3 is not supported,"
             "creating legacy mode GL Context");
        return CreateWideColorCtx(SRGBA_R8G8B8A8_REV);
    }

  // create the wide color gamut context
  int index = 0;
  for (auto mode : modes) {
      index = index + 1;
    if (CreateWideColorCtx(mode)) {
      LOGW("CreateWideColorCtx: %d", index);
      return true;
    }
  }
  return false;
}

void ImageViewEngine::DestroyWideColorCtx() {
  if (display_ == EGL_NO_DISPLAY) {
    return;
  }

  eglMakeCurrent(display_, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (eglContext_ != EGL_NO_CONTEXT) {
    eglDestroyContext(display_, eglContext_);
  }
  if (surface_ != EGL_NO_SURFACE) {
    eglDestroySurface(display_, surface_);
  }
  eglTerminate(display_);

  display_ = EGL_NO_DISPLAY;
  eglContext_ = EGL_NO_CONTEXT;
  surface_ = EGL_NO_SURFACE;
  dispColorSpace_ = DISPLAY_COLORSPACE::INVALID;
  dispFormat_ = DISPLAY_FORMAT::INVALID_FORMAT;
}
