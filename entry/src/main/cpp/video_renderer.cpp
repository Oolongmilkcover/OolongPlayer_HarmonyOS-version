#include "video_renderer.h"

#include <hilog/log.h>
#include <native_window/external_window.h>

#include <cstring>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OPlayerVideo"

namespace {

// 本机 GLES 实现里纹理单元 0 的采样结果恒为 0（单元 1、2 正常），
// 所以 Y/UV 固定绑到单元 1、2，绕开单元 0。
const int kYUnit = 1;
const int kUvUnit = 2;

const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTex;
uniform vec2 uScale;
out vec2 vTex;
void main()
{
    gl_Position = vec4(aPos * uScale, 0.0, 1.0);
    vTex = aTex;
}
)";

/*
 * 只保留两个采样器：texY（R8 亮度）和 texUV（RG8，U 在 r、V 在 g）。
 * 之前 Y/U/V 三张纹理三个 sampler2D 在本机 GL 上会有一个采样恒为 0
 * （画面因此只剩色度信息、整体发暗），拆成两张纹理后正常。
 *
 * 亮度按 limited range（16-235）展开，色度矩阵按分辨率选 BT.709/BT.601，
 * 与 ffmpeg 解出的同一帧对齐（blast.mp4 是 bt709 + tv range）。
 */
const char* kFragmentShader = R"(#version 300 es
precision mediump float;
in vec2 vTex;
uniform sampler2D texY;
uniform sampler2D texUV;
uniform int filterType;
uniform int useBt709;
out vec4 color;

void main()
{
    vec3 yuv;
    vec3 rgb;

    yuv.x = (texture(texY, vTex).r * 255.0 - 16.0) / 219.0;
    vec2 uv = texture(texUV, vTex).rg;
    yuv.y = (uv.x * 255.0 - 128.0) / 224.0;
    yuv.z = (uv.y * 255.0 - 128.0) / 224.0;

    if (useBt709 == 1) {
        rgb = mat3(
            1.0,      1.0,      1.0,
            0.0,     -0.1873,   1.8556,
            1.5748,  -0.4681,   0.0
        ) * yuv;
    } else {
        rgb = mat3(
            1.0,      1.0,      1.0,
            0.0,     -0.344136, 1.772,
            1.402,   -0.714136, 0.0
        ) * yuv;
    }

    if (filterType == 1) {
        float gray = dot(rgb, vec3(0.299, 0.587, 0.114));
        rgb = vec3(gray);
    } else if (filterType == 2) {
        rgb = 1.0 - rgb;
    } else if (filterType == 3) {
        rgb = rgb * vec3(1.08, 0.96, 0.85);
    } else if (filterType == 4) {
        rgb = rgb * vec3(0.85, 0.96, 1.08);
    }

    color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

const GLfloat kVertices[] = {
    // x, y, u, v
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

GLuint CompileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {0};
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log) - 1, nullptr, log);
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "shader compile failed: %{public}s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

VideoRenderer& VideoRenderer::Get()
{
    static VideoRenderer instance;
    return instance;
}

void VideoRenderer::SetSurface(void* window, int32_t width, int32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // window 变了由 BindSurfaceLocked 里的 eglWindow_ 比较发现，这里只更新尺寸
    window_ = window;
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    resizeRetry_ = 0;
    ApplyBufferGeometryLocked();
}

void VideoRenderer::ResizeSurface(int32_t width, int32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (width != surfaceWidth_ || height != surfaceHeight_) {
        resizeRetry_ = 0;
    }
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    ApplyBufferGeometryLocked();
}

void VideoRenderer::ApplyBufferGeometryLocked()
{
    if (window_ == nullptr || surfaceWidth_ <= 0 || surfaceHeight_ <= 0) {
        return;
    }
    // 让 native window 的缓冲尺寸跟 XComponent 保持一致（旋转/全屏后会变）
    OH_NativeWindow_NativeWindowHandleOpt(static_cast<OHNativeWindow*>(window_), SET_BUFFER_GEOMETRY,
                                          surfaceWidth_, surfaceHeight_);
}

void VideoRenderer::ClearSurface()
{
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = nullptr;
    surfaceWidth_ = 0;
    surfaceHeight_ = 0;
}

void VideoRenderer::Render(const op_frame* frame)
{
    if (frame == nullptr || frame->data == nullptr || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (window_ == nullptr) {
        return;
    }

    if (!BindSurfaceLocked(false)) {
        ReleaseEglLocked();
        return;
    }
    if (!EnsureGlResourcesLocked()) {
        UnbindLocked();
        return;
    }

    const int32_t width = frame->width;
    const int32_t height = frame->height;
    const int32_t linesizeY = frame->linesize[0] > 0 ? frame->linesize[0] : width;
    const int32_t linesizeU = frame->linesize[1] > 0 ? frame->linesize[1] : (width / 2);
    const int32_t linesizeV = frame->linesize[2] > 0 ? frame->linesize[2] : (width / 2);

    const uint8_t* y = frame->data;
    const uint8_t* u = frame->data + (size_t)linesizeY * (size_t)height;
    const uint8_t* v = u + (size_t)linesizeU * (size_t)(height / 2);

    const int32_t chromaWidth = (width + 1) / 2;
    const int32_t chromaHeight = (height + 1) / 2;

    UploadYLocked(width, height, y, linesizeY);
    UploadUvLocked(chromaWidth, chromaHeight, u, v, linesizeU);

    frameWidth_ = width;
    frameHeight_ = height;
    hasFrame_ = true;

    int32_t sw = 0;
    int32_t sh = 0;
    UpdateSurfaceSizeLocked(&sw, &sh);
    // 旋转/全屏后窗口表面可能仍沿用旧尺寸：重建 surface 让缓冲跟上 JS 侧的组件尺寸
    if (surfaceWidth_ > 0 && surfaceHeight_ > 0 && (sw != surfaceWidth_ || sh != surfaceHeight_)) {
        if (resizeRetry_ < 5) {
            resizeRetry_++;
            ApplyBufferGeometryLocked();
            if (!BindSurfaceLocked(true)) {
                UnbindLocked();
                return;
            }
            UpdateSurfaceSizeLocked(&sw, &sh);
        }
    } else {
        resizeRetry_ = 0;
    }

    DrawFrameLocked(sw, sh);
    UnbindLocked();
}

void VideoRenderer::Refresh(int32_t width, int32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (window_ == nullptr) {
        return;
    }
    if (width > 0 && height > 0) {
        surfaceWidth_ = width;
        surfaceHeight_ = height;
    }
    ApplyBufferGeometryLocked();
    resizeRetry_ = 0;

    // UI 线程主动重建 window surface：暂停时没有新帧，也能立刻按新尺寸重绘最后一帧
    if (!BindSurfaceLocked(true)) {
        ReleaseEglLocked();
        return;
    }
    if (!EnsureGlResourcesLocked()) {
        UnbindLocked();
        return;
    }

    int32_t sw = 0;
    int32_t sh = 0;
    UpdateSurfaceSizeLocked(&sw, &sh);
    if (hasFrame_) {
        DrawFrameLocked(sw, sh);
    } else {
        glViewport(0, 0, sw, sh);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display_, surface_);
    }
    UnbindLocked();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                 "refresh surface %{public}dx%{public}d lastFrame=%{public}dx%{public}d",
                 sw, sh, frameWidth_, frameHeight_);
}

void VideoRenderer::UpdateSurfaceSizeLocked(int32_t* outWidth, int32_t* outHeight)
{
    int32_t w = surfaceWidth_ > 0 ? surfaceWidth_ : frameWidth_;
    int32_t h = surfaceHeight_ > 0 ? surfaceHeight_ : frameHeight_;
    // 以 EGL 窗口表面实际尺寸为准（旋转/全屏后 XComponent 的缓冲尺寸可能与 JS 传入值不同）
    EGLint eglW = 0;
    EGLint eglH = 0;
    if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE &&
        eglQuerySurface(display_, surface_, EGL_WIDTH, &eglW) == EGL_TRUE &&
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &eglH) == EGL_TRUE &&
        eglW > 0 && eglH > 0) {
        w = eglW;
        h = eglH;
    }
    *outWidth = w;
    *outHeight = h;
}

void VideoRenderer::DrawFrameLocked(int32_t surfaceWidth, int32_t surfaceHeight)
{
    const int32_t sw = surfaceWidth > 0 ? surfaceWidth : frameWidth_;
    const int32_t sh = surfaceHeight > 0 ? surfaceHeight : frameHeight_;
    const int32_t vw = frameWidth_ > 0 ? frameWidth_ : sw;
    const int32_t vh = frameHeight_ > 0 ? frameHeight_ : sh;
    if (sw <= 0 || sh <= 0) {
        return;
    }

    glViewport(0, 0, sw, sh);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    float scaleX = 1.0f;
    float scaleY = 1.0f;
    const float videoAspect = (float)vw / (float)vh;
    const float surfaceAspect = (float)sw / (float)sh;
    if (surfaceAspect > videoAspect) {
        scaleX = videoAspect / surfaceAspect;
    } else if (surfaceAspect < videoAspect) {
        scaleY = surfaceAspect / videoAspect;
    }

    glUseProgram(program_);
    int32_t filter = filter_.load();
    if (filter < 0 || filter > 4) filter = 0;
    glUniform1i(filterLoc_, filter);
    // 高清及以上用 BT.709，标清用 BT.601
    glUniform1i(bt709Loc_, vh >= 720 ? 1 : 0);
    glUniform2f(scaleLoc_, scaleX, scaleY);

    glActiveTexture(GL_TEXTURE0 + kYUnit);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glUniform1i(texLoc_[0], kYUnit);
    glActiveTexture(GL_TEXTURE0 + kUvUnit);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glUniform1i(texLoc_[1], kUvUnit);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    eglSwapBuffers(display_, surface_);
}

bool VideoRenderer::EnsureDisplayLocked()
{
    if (display_ != EGL_NO_DISPLAY) {
        return true;
    }

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "eglGetDisplay failed");
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display_, &major, &minor) != EGL_TRUE) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "eglInitialize failed: 0x%{public}x", eglGetError());
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint configCount = 0;
    if (eglChooseConfig(display_, configAttribs, &config_, 1, &configCount) != EGL_TRUE || configCount <= 0) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "eglChooseConfig failed: 0x%{public}x", eglGetError());
        ReleaseEglLocked();
        return false;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "EGL ready %{public}d.%{public}d", (int)major, (int)minor);
    return true;
}

bool VideoRenderer::EnsureContextLocked()
{
    if (!EnsureDisplayLocked()) {
        return false;
    }
    if (context_ != EGL_NO_CONTEXT) {
        return true;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "eglCreateContext failed: 0x%{public}x", eglGetError());
        return false;
    }
    return true;
}

bool VideoRenderer::BindSurfaceLocked(bool recreate)
{
    if (!EnsureContextLocked()) {
        return false;
    }

    // 只重建 window surface，context 和纹理都保留，重绘最后一帧即可
    if (recreate && surface_ != EGL_NO_SURFACE) {
        UnbindLocked();
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
        eglWindow_ = nullptr;
    }

    if (surface_ == EGL_NO_SURFACE || eglWindow_ != window_) {
        if (surface_ != EGL_NO_SURFACE) {
            UnbindLocked();
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        surface_ = eglCreateWindowSurface(display_, config_, (EGLNativeWindowType)window_, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG,
                         "eglCreateWindowSurface failed: 0x%{public}x", eglGetError());
            return false;
        }
        eglWindow_ = window_;
    }

    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "eglMakeCurrent failed: 0x%{public}x", eglGetError());
        return false;
    }
    return true;
}

void VideoRenderer::UnbindLocked()
{
    if (display_ != EGL_NO_DISPLAY) {
        // 不把 context 绑在某一根线程上：SDK 渲染线程和 UI 线程都能接手
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

bool VideoRenderer::EnsureGlResourcesLocked()
{
    if (program_ != 0) {
        return true;
    }

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertexShader == 0 || fragmentShader == 0) {
        if (vertexShader != 0) glDeleteShader(vertexShader);
        if (fragmentShader != 0) glDeleteShader(fragmentShader);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vertexShader);
    glAttachShader(program_, fragmentShader);
    glLinkProgram(program_);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {0};
        glGetProgramInfoLog(program_, (GLsizei)sizeof(log) - 1, nullptr, log);
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "program link failed: %{public}s", log);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    scaleLoc_ = glGetUniformLocation(program_, "uScale");
    filterLoc_ = glGetUniformLocation(program_, "filterType");
    bt709Loc_ = glGetUniformLocation(program_, "useBt709");
    texLoc_[0] = glGetUniformLocation(program_, "texY");
    texLoc_[1] = glGetUniformLocation(program_, "texUV");

    glGenTextures(2, textures_);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        texAllocW_[i] = 0;
        texAllocH_[i] = 0;
    }

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    return true;
}

void VideoRenderer::UploadYLocked(int32_t width, int32_t height, const uint8_t* data,
                                  int32_t linesize)
{
    if (width <= 0 || height <= 0 || data == nullptr) {
        return;
    }

    glActiveTexture(GL_TEXTURE0 + kYUnit);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);

    if (texAllocW_[0] != width || texAllocH_[0] != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        texAllocW_[0] = width;
        texAllocH_[0] = height;
    }

    if (linesize > width) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, data);
    if (linesize > width) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}

void VideoRenderer::UploadUvLocked(int32_t width, int32_t height, const uint8_t* u,
                                   const uint8_t* v, int32_t linesize)
{
    if (width <= 0 || height <= 0 || u == nullptr || v == nullptr) {
        return;
    }

    const size_t need = (size_t)width * (size_t)height * 2;
    if (uvBuf_.size() < need) {
        uvBuf_.resize(need);
    }
    uint8_t* dst = uvBuf_.data();
    for (int32_t row = 0; row < height; ++row) {
        const uint8_t* srcU = u + (size_t)row * (size_t)linesize;
        const uint8_t* srcV = v + (size_t)row * (size_t)linesize;
        uint8_t* dstRow = dst + (size_t)row * (size_t)width * 2;
        for (int32_t col = 0; col < width; ++col) {
            dstRow[col * 2] = srcU[col];
            dstRow[col * 2 + 1] = srcV[col];
        }
    }

    glActiveTexture(GL_TEXTURE0 + kUvUnit);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);

    if (texAllocW_[1] != width || texAllocH_[1] != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width, height, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
        texAllocW_[1] = width;
        texAllocH_[1] = height;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RG, GL_UNSIGNED_BYTE, dst);
}

void VideoRenderer::ReleaseEglLocked()
{
    if (display_ != EGL_NO_DISPLAY) {
        // GL 对象得在 context 还 current 的时候删；拿不到 current 就只清 id，
        // 它们会随 context 一起销毁
        const bool current = (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE &&
                              eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE);
        if (current) {
            if (program_ != 0) {
                glDeleteProgram(program_);
            }
            if (textures_[0] != 0) {
                glDeleteTextures(2, textures_);
            }
            if (vbo_ != 0) {
                glDeleteBuffers(1, &vbo_);
            }
            if (vao_ != 0) {
                glDeleteVertexArrays(1, &vao_);
            }
        }
        program_ = 0;
        textures_[0] = textures_[1] = 0;
        vbo_ = 0;
        vao_ = 0;
        texAllocW_[0] = texAllocW_[1] = 0;
        texAllocH_[0] = texAllocH_[1] = 0;
        // 纹理没了，最后一帧也重绘不出来了
        hasFrame_ = false;
        frameWidth_ = 0;
        frameHeight_ = 0;

        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
        config_ = nullptr;
    }
    eglWindow_ = nullptr;
}
