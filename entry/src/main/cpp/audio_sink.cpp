#include "audio_sink.h"

#include <hilog/log.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OPlayerAudio"

namespace {
constexpr int32_t kDefaultSampleRate = 48000;
constexpr int32_t kDefaultChannels = 2;

// 临时诊断开关：统计写回调的供数、欠载、回调间隔与设备积压。
// 注意：开启后每次回调都会 snprintf + hilog，写日志发生在音频实时线程上，
// 高频窗口足够长时会自己制造欠载。默认关闭，排查时再打开。
constexpr bool kAudioDiag = false;

// seek 未落地时最多屏蔽这么久；SEEK_DONE 丢失也能恢复，不会永久静音。
constexpr int64_t kSeekGateNs = 2000000000LL;
// 落地后等待预灌的最长时间；取不到足够数据就带着现有数据开播，绝不无限等。
constexpr int64_t kPrimeWaitNs = 300000000LL;

int64_t NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

struct AudioDiag {
    uint64_t callbacks = 0;
    uint64_t reqBytes = 0;
    uint64_t gotBytes = 0;
    uint64_t zeroReads = 0;
    uint64_t shortReads = 0;
    uint64_t silenceBytes = 0;
    uint64_t intervals = 0;
    int64_t lastCbNs = 0;
    int64_t dtSumNs = 0;
    int64_t dtMinNs = INT64_MAX;
    int64_t dtMaxNs = 0;
    int32_t qMin = INT32_MAX;
    int32_t qMax = 0;
    int64_t qSum = 0;
    int32_t devMin = INT32_MAX;
    int32_t devMax = 0;
    int64_t lastReportNs = 0;
    int64_t burstUntilNs = 0;
    int64_t burstStartNs = 0;
};

AudioDiag g_diag;
int64_t g_lastFramesWritten = 0;
int64_t g_lastFramePosition = 0;

void ResetWindow(AudioDiag& d)
{
    d.callbacks = 0;
    d.reqBytes = 0;
    d.gotBytes = 0;
    d.zeroReads = 0;
    d.shortReads = 0;
    d.silenceBytes = 0;
    d.intervals = 0;
    d.dtSumNs = 0;
    d.dtMinNs = INT64_MAX;
    d.dtMaxNs = 0;
    d.qMin = INT32_MAX;
    d.qMax = 0;
    d.qSum = 0;
    d.devMin = INT32_MAX;
    d.devMax = 0;
}
} // namespace

AudioSink& AudioSink::Get()
{
    static AudioSink instance;
    return instance;
}

bool AudioSink::Open(op_player* player, int32_t sampleRate, int32_t channels)
{
    Close();

    if (sampleRate <= 0) sampleRate = kDefaultSampleRate;
    if (channels <= 0) channels = kDefaultChannels;
    sampleRate_ = sampleRate;
    channels_ = channels;
    // 新渲染器从 0 开始计帧，旧的 flush 基准必须作废；同时清掉上一次遗留的
    // seek/预灌状态，否则换文件后会一直静音或设备积压恒为 0。
    flushFrameBase_.store(-1);
    flushing_.store(false);
    seekGate_.store(false);
    priming_.store(true);
    primeUntilNs_.store(NowNs() + kPrimeWaitNs);

    OH_AudioStreamBuilder* builder = nullptr;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS || builder == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "create audio builder failed");
        return false;
    }

    OH_AudioStreamBuilder_SetSamplingRate(builder, sampleRate);
    OH_AudioStreamBuilder_SetChannelCount(builder, channels);
    OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
    OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MOVIE);
    OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, OnWriteData, this);

    OH_AudioRenderer* renderer = nullptr;
    OH_AudioStream_Result result = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer);
    OH_AudioStreamBuilder_Destroy(builder);

    if (result != AUDIOSTREAM_SUCCESS || renderer == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "generate audio renderer failed: %{public}d", (int)result);
        return false;
    }

    renderer_ = renderer;
    player_.store(player);
    paused_.store(false);
    OH_AudioRenderer_Start(renderer_);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "audio renderer started %{public}d/%{public}d", sampleRate, channels);
    return true;
}

void AudioSink::Close()
{
    OH_AudioRenderer* renderer = renderer_;
    renderer_ = nullptr;
    player_.store(nullptr);
    paused_.store(true);
    flushFrameBase_.store(-1);
    flushing_.store(false);
    seekGate_.store(false);
    priming_.store(true);
    if (renderer != nullptr) {
        OH_AudioRenderer_Stop(renderer);
        OH_AudioRenderer_Release(renderer);
    }
}

void AudioSink::Start()
{
    if (renderer_ == nullptr) return;
    // 已经在跑就别重复 Start：OHAudio 会返回 ILLEGAL_STATE（日志里能看到
    // "Start failed. Illegal state"），seek 恢复播放这条路径会重复调用。
    if (!paused_.exchange(false)) return;
    OH_AudioRenderer_Start(renderer_);
}

void AudioSink::Pause()
{
    if (renderer_ == nullptr) return;
    if (paused_.exchange(true)) return;
    OH_AudioRenderer_Pause(renderer_);
}

void AudioSink::Flush()
{
    if (renderer_ == nullptr) return;
    // 写回调线程同时在做 GetFramesWritten/GetTimestamp，flush 期间两边基准不一致，
    // 先挂上标记让那一路跳过上报。
    flushing_.store(true);
    const OH_AudioStream_Result result = OH_AudioRenderer_Flush(renderer_);
    // flush 成功后 timestamp 的 framePosition 归零，记下当前累计写入帧数作为新基准
    if (result == AUDIOSTREAM_SUCCESS) {
        int64_t framesWritten = 0;
        if (OH_AudioRenderer_GetFramesWritten(renderer_, &framesWritten) == AUDIOSTREAM_SUCCESS) {
            flushFrameBase_.store(framesWritten);
        }
        // flush 后设备是空的，重新攒一小段再出声，避免恢复瞬间一堆半块数据。
        priming_.store(true);
        primeUntilNs_.store(NowNs() + kPrimeWaitNs);
    }
    op_player* player = player_.load();
    if (player != nullptr) {
        op_audio_set_device_pending_ms(player, 0);
    }
    flushing_.store(false);
}

void AudioSink::BeginSeek()
{
    priming_.store(false);
    seekGate_.store(true);
    seekGateUntilNs_.store(NowNs() + kSeekGateNs);
    if (kAudioDiag) {
        const int64_t nowNs = NowNs();
        g_diag.burstStartNs = nowNs;
        g_diag.burstUntilNs = nowNs + 3000000000LL;
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "SEEK burst start");
    }
}

void AudioSink::EndSeek()
{
    seekGate_.store(false);
    priming_.store(true);
    primeUntilNs_.store(NowNs() + kPrimeWaitNs);
}

void AudioSink::SetPlayer(op_player* player)
{
    player_.store(player);
}

OH_AudioData_Callback_Result AudioSink::OnWriteData(OH_AudioRenderer* renderer, void* userData,
                                                    void* audioData, int32_t audioDataSize)
{
    auto* self = static_cast<AudioSink*>(userData);
    if (self == nullptr || audioData == nullptr || audioDataSize <= 0) {
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;
    }

    const int64_t nowNs = NowNs();
    op_player* player = self->player_.load();
    const int32_t queueBefore = (player != nullptr) ? op_audio_pending(player) : 0;

    // 两种情况先不出声（缓冲补静音）：
    //   seekGate_：seek 还没落地，旧位置的 PCM 不能再进设备；
    //   priming_ ：刚 flush/刚落地，环形缓冲里只有零碎数据，先攒够再连续出声。
    bool canRead = true;
    if (player != nullptr && !self->paused_.load()) {
        if (self->seekGate_.load()) {
            if (nowNs < self->seekGateUntilNs_.load()) {
                canRead = false;
            } else {
                // SEEK_DONE 没来（丢失/失败）：到点自动放行，不能永久静音
                self->seekGate_.store(false);
                self->priming_.store(true);
                self->primeUntilNs_.store(nowNs + kPrimeWaitNs);
            }
        }
        if (canRead && self->priming_.load()) {
            // 1.5 个回调块：保证这次能读满，又不会为攒数据等太久。
            // 回调块特别大时再按 60ms 封顶，避免为了凑数把恢复出声拖到几百毫秒。
            int32_t primeBytes = audioDataSize + audioDataSize / 2;
            const int32_t frameBytes = (self->channels_ > 0 ? self->channels_ : 2) * 2;
            const int32_t capBytes = self->sampleRate_ * frameBytes * 60 / 1000;
            if (primeBytes > capBytes) primeBytes = capBytes;
            if (primeBytes < audioDataSize) primeBytes = audioDataSize;
            if (queueBefore >= primeBytes || nowNs >= self->primeUntilNs_.load()) {
                self->priming_.store(false);
            } else {
                canRead = false;
            }
        }
    } else {
        canRead = false;
    }

    int32_t validBytes = 0;
    if (canRead && player != nullptr) {
        int32_t got = op_audio_read(player, audioData, audioDataSize);
        if (got > 0) {
            validBytes = got < audioDataSize ? got : audioDataSize;
        }
    }

    // 欠载时补静音：回调必须交回整块缓冲，否则设备会反复欠载/爆音。
    if (validBytes < audioDataSize) {
        std::memset(static_cast<uint8_t*>(audioData) + validBytes, 0, (size_t)(audioDataSize - validBytes));
    }

    int32_t devicePendingMs = -1;
    if (player != nullptr) {
        devicePendingMs = self->ReportDevicePendingMs(renderer);
    }

    if (kAudioDiag) {
        AudioDiag& d = g_diag;
        int64_t dtNs = 0;
        if (d.lastCbNs != 0) {
            dtNs = nowNs - d.lastCbNs;
            d.dtSumNs += dtNs;
            d.intervals++;
            if (dtNs < d.dtMinNs) d.dtMinNs = dtNs;
            if (dtNs > d.dtMaxNs) d.dtMaxNs = dtNs;
        }
        d.lastCbNs = nowNs;
        d.callbacks++;
        d.reqBytes += (uint64_t)audioDataSize;
        d.gotBytes += (uint64_t)validBytes;
        d.silenceBytes += (uint64_t)(audioDataSize - validBytes);
        if (validBytes == 0) d.zeroReads++;
        else if (validBytes < audioDataSize) d.shortReads++;
        d.qSum += queueBefore;
        if (queueBefore < d.qMin) d.qMin = queueBefore;
        if (queueBefore > d.qMax) d.qMax = queueBefore;
        if (devicePendingMs >= 0) {
            if (devicePendingMs < d.devMin) d.devMin = devicePendingMs;
            if (devicePendingMs > d.devMax) d.devMax = devicePendingMs;
        }

        const int64_t sinceRef = d.burstStartNs != 0 ? d.burstStartNs : nowNs;
        if (d.burstUntilNs > nowNs) {
            char line[256];
            snprintf(line, sizeof(line),
                     "BURST t=%lldms dt=%0.2fms req=%d got=%d q=%d dev=%d lat=%lld fw=%lld fp=%lld",
                     (long long)((nowNs - sinceRef) / 1000000), (double)dtNs / 1e6, audioDataSize,
                     validBytes, queueBefore, devicePendingMs,
                     (long long)(player != nullptr ? op_audio_latency_ms(player) : -1),
                     (long long)g_lastFramesWritten, (long long)g_lastFramePosition);
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "%{public}s", line);
        }

        if (d.lastReportNs == 0) d.lastReportNs = nowNs;
        if (nowNs - d.lastReportNs >= 1000000000LL) {
            const double windowSec = (double)(nowNs - d.lastReportNs) / 1e9;
            const double fill = d.reqBytes > 0 ? 100.0 * (double)d.gotBytes / (double)d.reqBytes : 0.0;
            const double dtAvg = d.intervals > 0 ? (double)d.dtSumNs / (double)d.intervals / 1e6 : 0.0;
            const double dtMin = d.dtMinNs == INT64_MAX ? 0.0 : (double)d.dtMinNs / 1e6;
            const double dtMax = (double)d.dtMaxNs / 1e6;
            const double expMs = (double)audioDataSize * 1000.0 / ((double)self->sampleRate_ * self->channels_ * 2.0);
            const double qAvg = d.callbacks > 0 ? (double)d.qSum / (double)d.callbacks : 0.0;
            char line[512];
            snprintf(line, sizeof(line),
                     "STAT %0.1fs cb=%llu req=%llu got=%llu zero=%llu short=%llu silence=%llu "
                     "fill=%0.1f%% dt=%0.2f/%0.2f/%0.2fms exp=%0.2fms q=%d/%d/%0.0f dev=%d/%d",
                     windowSec, (unsigned long long)d.callbacks, (unsigned long long)d.reqBytes,
                     (unsigned long long)d.gotBytes, (unsigned long long)d.zeroReads,
                     (unsigned long long)d.shortReads, (unsigned long long)d.silenceBytes, fill,
                     dtAvg, dtMin, dtMax, expMs, d.qMin == INT32_MAX ? 0 : d.qMin, d.qMax, qAvg,
                     d.devMin == INT32_MAX ? -1 : d.devMin, d.devMax);
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "%{public}s", line);
            d.lastReportNs = nowNs;
            ResetWindow(d);
        }
    }

    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

int32_t AudioSink::ReportDevicePendingMs(OH_AudioRenderer* renderer)
{
    if (renderer == nullptr || sampleRate_ <= 0) return -1;
    // flush 进行中：framesWritten 与 framePosition 可能分属新旧基准，这一拍不报
    if (flushing_.load()) return -1;

    int64_t framesWritten = 0;
    int64_t framePosition = 0;
    int64_t timestamp = 0;

    if (OH_AudioRenderer_GetFramesWritten(renderer, &framesWritten) != AUDIOSTREAM_SUCCESS) {
        return -1;
    }
    if (OH_AudioRenderer_GetTimestamp(renderer, CLOCK_MONOTONIC, &framePosition, &timestamp) != AUDIOSTREAM_SUCCESS) {
        return -1;
    }

    int64_t pendingFrames = framesWritten - framePosition;
    const int64_t base = flushFrameBase_.load();
    if (base >= 0) {
        // flush 之后 framePosition 从 0 重新计，framesWritten 仍是累计值
        pendingFrames = (framesWritten - base) - framePosition;
    }
    if (pendingFrames < 0) pendingFrames = 0;
    const int32_t pendingMs = (int32_t)(pendingFrames * 1000 / sampleRate_);
    // 正常设备积压不可能到秒级；flush 后若 GetFramesWritten 失败会算出这种异常值，
    // 宁可不报（SDK 继续用上一次锚点），也不能把音频时钟一次带偏几秒。
    if (pendingMs > 1000) return -1;
    if (kAudioDiag) {
        g_lastFramesWritten = framesWritten;
        g_lastFramePosition = framePosition;
    }

    op_player* player = player_.load();
    if (player != nullptr) {
        op_audio_set_device_pending_ms(player, pendingMs);
    }
    return pendingMs;
}
