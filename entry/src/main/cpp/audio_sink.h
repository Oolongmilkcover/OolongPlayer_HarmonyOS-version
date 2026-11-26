#pragma once

#include <op/op_player.h>

#include <atomic>
#include <cstdint>

#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiorenderer.h>

/*
 * 音频出口：SDK 是 pull（op_audio_read），鸿蒙 AudioRenderer 是 push
 * （系统在写回调里要数据）。这里在 OHAudio 的写回调里把 PCM 拉出来，
 * 并顺手把"设备里还没播出去的毫秒数"报回 SDK（音频时钟的延迟补偿）。
 *
 * 音量不归 SDK 管：在写回调里对 S16 样本做缩放。
 * 线程：写回调跑在 OHAudio 的音频线程；Open/Close/Start/Pause 在 JS 线程。
 */
class AudioSink {
public:
    static AudioSink& Get();

    bool Open(op_player* player, int32_t sampleRate, int32_t channels);
    void Close();

    void Start();
    void Pause();
    void Flush();
    // seek 期间屏蔽旧 PCM（静音），SEEK_DONE 后先预灌一小段再出声；
    // 超时会自动解除，避免 SEEK_DONE 丢失后永久静音。
    void BeginSeek();
    void EndSeek();

    void SetPlayer(op_player* player);
    void SetVolume(float volume);
    float GetVolume() const;

    bool IsOpen() const { return renderer_ != nullptr; }
    int32_t SampleRate() const { return sampleRate_; }

private:
    AudioSink() = default;
    ~AudioSink() = default;
    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    static OH_AudioData_Callback_Result OnWriteData(OH_AudioRenderer* renderer, void* userData,
                                                    void* audioData, int32_t audioDataSize);
    int32_t ReportDevicePendingMs(OH_AudioRenderer* renderer);
    void ApplyVolume(void* audioData, int32_t validBytes);

    OH_AudioRenderer* renderer_ = nullptr;
    std::atomic<op_player*> player_{nullptr};
    std::atomic<float> volume_{0.5f};

    int32_t sampleRate_ = 48000;
    int32_t channels_ = 2;
    std::atomic<bool> paused_{true};
    // OH_AudioRenderer_Flush 会把时间戳的 framePosition 归零，但 GetFramesWritten
    // 仍是累计值：直接相减会得到一个"从开播到现在"的巨大积压。这里记住 flush 时的
    // 累计写入帧数，之后只按增量计算。
    std::atomic<int64_t> flushFrameBase_{-1};
    // flush 执行期间禁止上报设备积压：此时 framesWritten/framePosition 分属新旧两套
    // 基准，算出来的值可能是"从开播到现在"的巨大积压，SDK 的音频时钟会被带偏。
    std::atomic<bool> flushing_{false};
    // seek 未落地前不读 SDK 环形缓冲，只输出静音：设备 flush 之后写回调如果继续
    // 拉旧数据，旧位置的音频会重新灌进设备，落地后再出新生数据就会听到一段旧声。
    std::atomic<bool> seekGate_{false};
    std::atomic<int64_t> seekGateUntilNs_{0};
    // seek/换文件落地后先攒够一小段 PCM 再出声：否则"半块数据 + 半块静音"会反复
    // 拼接，这正是 seek 之后听起来断断续续的直接原因。
    std::atomic<bool> priming_{true};
    std::atomic<int64_t> primeUntilNs_{0};
};
