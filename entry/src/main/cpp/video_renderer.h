#pragma once

#include <op/op_player.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

/*
 * 视频渲染：XComponent 提供 NativeWindow，这里用 EGL/GLES3 把 op_frame
 * 的 YUV420P 数据上传成两张纹理——Y 一张 R8，U/V 交织成一张 RG8，
 * 片元着色器里做 YUV→RGB 与滤镜。等比缩放通过顶点着色器的 uScale 完成。
 *
 * 线程：SetSurface/ClearSurface/Refresh 在 UI（XComponent 回调）线程；
 *       Render 在 SDK 的视频回调线程。EGL context 与 GL 资源在首次 Render
 *       时创建，之后每次 GL 调用前 eglMakeCurrent、结束后解绑，这样
 *       UI 线程可以在暂停（没有新帧）时自己重建 window surface 并重绘
 *       最后一帧——退出全屏后不必等下一帧就能得到正确比例。
 */
class VideoRenderer {
public:
    static VideoRenderer& Get();

    // window 为 OHNativeWindow*（XComponent 的 surface）
    void SetSurface(void* window, int32_t width, int32_t height);
    void ResizeSurface(int32_t width, int32_t height);
    void ClearSurface();

    // 尺寸变化后主动重建 window surface 并重绘最后一帧（暂停时没有新帧也能刷新）
    void Refresh(int32_t width, int32_t height);

    void Render(const op_frame* frame);

    void SetFilter(int32_t type) { filter_.store(type); }

private:
    VideoRenderer() = default;
    ~VideoRenderer() = default;
    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    bool EnsureDisplayLocked();
    bool EnsureContextLocked();
    bool EnsureGlResourcesLocked();
    // recreate=true 时强制销毁并重建 window surface（context/纹理保留）
    bool BindSurfaceLocked(bool recreate);
    void UnbindLocked();
    void ReleaseEglLocked();
    void ApplyBufferGeometryLocked();
    void UpdateSurfaceSizeLocked(int32_t* outWidth, int32_t* outHeight);
    void DrawFrameLocked(int32_t surfaceWidth, int32_t surfaceHeight);
    void UploadYLocked(int32_t width, int32_t height, const uint8_t* data, int32_t linesize);
    void UploadUvLocked(int32_t width, int32_t height, const uint8_t* u, const uint8_t* v,
                        int32_t linesize);

    std::mutex mutex_;

    void* window_ = nullptr;
    int32_t surfaceWidth_ = 0;
    int32_t surfaceHeight_ = 0;
    int32_t resizeRetry_ = 0;

    // 最后一帧的尺寸：纹理里还留着像素，重建 surface 后可以直接重绘
    int32_t frameWidth_ = 0;
    int32_t frameHeight_ = 0;
    bool hasFrame_ = false;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    void* eglWindow_ = nullptr;

    GLuint program_ = 0;
    GLuint textures_[2] = {0, 0};
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    int32_t texAllocW_[2] = {0, 0};
    int32_t texAllocH_[2] = {0, 0};

    GLint scaleLoc_ = -1;
    GLint filterLoc_ = -1;
    GLint bt709Loc_ = -1;
    GLint texLoc_[2] = {-1, -1};

    // U/V 交织缓冲（RG8）
    std::vector<uint8_t> uvBuf_;

    std::atomic<int32_t> filter_{0};
};
