/*
*  This file is part of OpenHU.
*  Copyright (C) 2026 Leon Strauß
*
*  OpenHU is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.
*
*  OpenHU is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with OpenHU. If not, see <https://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <QCoreApplication>
#include <QThread>
#include <QVideoFrameFormat>
#include <boost/log/keywords/severity.hpp>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
#include <f1x/openauto/autoapp/Projection/FfmpegVideoOutput.hpp>
#include <f1x/openauto/autoapp/Shell/AaBridge.hpp>
#include <f1x/openauto/Common/Log.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace projection
{

namespace
{

// The phone sends a few frames per message at most; this much backlog means the decoder cannot keep up.
constexpr size_t cMaxQueuedBytes = 8 * 1024 * 1024;
// A hardware decoder that has not produced a frame after this many messages is replaced by software.
constexpr int cHardwareProbePackets = 60;
// avcodec_send_packet() answering EAGAIN: frames are read and the packet is sent again, at most this often
// (2 ms apart while no frame comes out) before it is dropped.
constexpr int cMaxSendAttempts = 50;
constexpr int64_t cStatsIntervalMs = 10000;
// SPS/PPS are a few bytes; more without a picture is not a codec configuration
constexpr qsizetype cMaxConfigBytes = 64 * 1024;
// After a stream (re)start, frames with decode errors (and after a restart blank ones) are held back at most this
// long (the sink keeps the last picture meanwhile); a phone that never sends a clean frame still gets its picture shown.
constexpr int64_t cMaxHoldMs = 1000;
// Blank frame: fewer than 1/cBlankShare of the luma samples brighter than cBlankLuma (black is 16 in limited range).
// After a restart the phone sends black frames for ~170 ms before its UI (Fairphone 4, #88): the flicker.
constexpr int cBlankLuma = 32;
constexpr size_t cBlankShare = 50;
// Messages and frames after a stream (re)start are logged in detail (debug) for this long
constexpr int64_t cDiagnosticMs = 2000;
// Every n-th pixel in both directions for the mean luma and the change between frames
constexpr int cLumaStep = 8;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// NAL unit type of the byte after a start code: H.264 bits 0-4, H.265 bits 1-6
int nalType(uint8_t header, bool hevc)
{
    return hevc ? (header >> 1) & 0x3f : header & 0x1f;
}

// True if the access unit contains a NAL unit of a type in [first, last]
bool containsNal(const QByteArray& data, int first, int last, bool hevc)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.constData());
    for(qsizetype i = 0; i + 3 < data.size(); ++i)
    {
        if(bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
        {
            const int type = nalType(bytes[i + 3], hevc);
            if(type >= first && type <= last)
            {
                return true;
            }
            i += 2;
        }
    }
    return false;
}

// True if the access unit contains a coded picture (H.264 NAL types 1-5, H.265 0-31), false for parameter sets and
// SEI only.
bool containsPicture(const QByteArray& data, bool hevc)
{
    return hevc ? containsNal(data, 0, 31, true) : containsNal(data, 1, 5, false);
}

// Decodable without earlier frames: H.264 IDR (5), H.265 IRAP (BLA, IDR, CRA: 16-21)
bool containsKeyframe(const QByteArray& data, bool hevc)
{
    return hevc ? containsNal(data, 16, 21, true) : containsNal(data, 5, 5, false);
}

// OPENHU_LOG=debug (or lower): the diagnostics after a stream start are logged
bool debugLogEnabled()
{
    return static_cast<bool>(boost::log::trivial::logger::get().open_record(
        boost::log::keywords::severity = boost::log::trivial::debug));
}

// NAL unit types in the access unit, e.g. "7,8" for SPS/PPS or "5" for an IDR picture (H.265: "32,33,34", "19")
std::string nalTypes(const QByteArray& data, bool hevc)
{
    std::string types;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.constData());
    int count = 0;
    for(qsizetype i = 0; i + 3 < data.size() && count < 12; ++i)
    {
        if(bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
        {
            types += (types.empty() ? "" : ",") + std::to_string(nalType(bytes[i + 3], hevc));
            ++count;
            i += 2;
        }
    }
    return types.empty() ? "none" : types;
}

// Subsampled luma plane of a planar 8-bit YUV frame, empty for other formats
std::vector<uint8_t> lumaSamples(const AVFrame* frame)
{
    std::vector<uint8_t> samples;
    switch(static_cast<AVPixelFormat>(frame->format))
    {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_NV12:
        break;
    default:
        return samples;
    }
    samples.reserve(static_cast<size_t>((frame->width / cLumaStep + 1) * (frame->height / cLumaStep + 1)));
    for(int y = 0; y < frame->height; y += cLumaStep)
    {
        const uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
        for(int x = 0; x < frame->width; x += cLumaStep)
        {
            samples.push_back(row[x]);
        }
    }
    return samples;
}

// True for a (nearly) black frame, see cBlankLuma
bool blankFrame(const std::vector<uint8_t>& luma)
{
    if(luma.empty())
    {
        return false;
    }
    const auto bright = static_cast<size_t>(std::count_if(luma.begin(), luma.end(), [](uint8_t value) {
        return value > cBlankLuma;
    }));
    return bright * cBlankShare < luma.size();
}

// In the GUI thread, waiting for it when called from another thread.
template<typename Function>
void runInGuiThread(Function function)
{
    if(QThread::currentThread() == QCoreApplication::instance()->thread())
    {
        function();
    }
    else
    {
        QMetaObject::invokeMethod(QCoreApplication::instance(), function, Qt::BlockingQueuedConnection);
    }
}

}

FfmpegVideoOutput::FfmpegVideoOutput(configuration::IConfiguration::Pointer configuration, QVideoSink* sink)
    : VideoOutput(std::move(configuration))
    , testSink_(sink)
    , useTestSink_(sink != nullptr)
    , maxQueuedBytes_(cMaxQueuedBytes)
{
    // deleteLater() and the frame hand-over run in the GUI thread
    this->moveToThread(QCoreApplication::instance()->thread());

    static std::once_flag logLevel;
    std::call_once(logLevel, []() { av_log_set_level(AV_LOG_ERROR); });

    // New session: resolution/margins from the settings apply from now on
    runInGuiThread([this]() { shell::AaBridge::instance().configure(configuration_); });
}

FfmpegVideoOutput::~FfmpegVideoOutput()
{
    this->stop();
}

bool FfmpegVideoOutput::open()
{
    return true;
}

bool FfmpegVideoOutput::init()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(running_)
        {
            return true;
        }
    }
    // A decoder thread that ended by itself (no decoder) is still joinable
    if(thread_.joinable())
    {
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        waitForKeyframe_ = false;
        droppedMessages_ = 0;
        streamStopped_ = false;
        pendingStartMs_ = -1;
        pendingRestart_ = false;
        queue_.clear();
        queuedBytes_ = 0;
        counters_ = Counters();
    }
    thread_ = std::thread(&FfmpegVideoOutput::decodeLoop, this);

    const void* self = this;
    QMetaObject::invokeMethod(&shell::AaBridge::instance(), [self]() {
        shell::AaBridge::instance().setVideoOutputActive(self, true);
    }, Qt::QueuedConnection);
    return true;
}

void FfmpegVideoOutput::write(uint64_t, const aasdk::common::DataConstBuffer& buffer)
{
    // Messages that left the queue (or never entered it) in this call: reported after the lock is released
    int consumedNow = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.messages;
        consumedNow = this->enqueue(buffer);
    }
    this->consumed(consumedNow);
}

int FfmpegVideoOutput::enqueue(const aasdk::common::DataConstBuffer& buffer)
{
    if(!running_)
    {
        return 1;
    }

    int consumedNow = 0;
    QByteArray data(reinterpret_cast<const char*>(buffer.cdata), static_cast<qsizetype>(buffer.size));
    if(queuedBytes_ + buffer.size > maxQueuedBytes_)
    {
        // The pictures are dropped, the codec configuration (SPS/PPS, no picture) is kept: without it
        // no later picture could be decoded.
        const size_t before = queue_.size();
        std::deque<Message> kept;
        queuedBytes_ = 0;
        for(auto& queued: queue_)
        {
            if(!containsPicture(queued.data, hevc_))
            {
                queuedBytes_ += queued.data.size();
                kept.push_back(std::move(queued));
            }
        }
        queue_ = std::move(kept);
        const int dropped = static_cast<int>(before - queue_.size());
        droppedMessages_ += dropped;
        counters_.droppedMessages += dropped;
        consumedNow += dropped;
        waitForKeyframe_ = true;
        OPENAUTO_LOG(warning) << "[FfmpegVideoOutput] decoder too slow, dropped " << dropped
                              << " queued messages, waiting for the next IDR frame"
                              << (consumedHandler_ ? " (the phone did not wait for the acks)" : "");
    }
    if(waitForKeyframe_ && containsPicture(data, hevc_))
    {
        // P-frames after a drop refer to dropped frames: corrupted picture until the next IDR
        if(!containsKeyframe(data, hevc_))
        {
            ++droppedMessages_;
            ++counters_.droppedMessages;
            return consumedNow + 1;
        }
        waitForKeyframe_ = false;
        OPENAUTO_LOG(info) << "[FfmpegVideoOutput] IDR frame after " << droppedMessages_ << " dropped messages";
    }
    Message message;
    message.data = std::move(data);
    if(pendingStartMs_ >= 0)
    {
        message.streamStartMs = pendingStartMs_;
        message.restart = pendingRestart_;
        pendingStartMs_ = -1;
    }
    queuedBytes_ += message.data.size();
    queue_.push_back(std::move(message));
    counters_.maxQueued = std::max(counters_.maxQueued, queue_.size());
    wake_.notify_one();
    return consumedNow;
}

bool FfmpegVideoOutput::setConsumedHandler(ConsumedHandler handler)
{
    std::lock_guard<std::mutex> lock(mutex_);
    consumedHandler_ = std::move(handler);
    return true;
}

void FfmpegVideoOutput::consumed(int count)
{
    if(count <= 0)
    {
        return;
    }
    ConsumedHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = consumedHandler_;
    }
    if(handler)
    {
        handler(count);
    }
}

FfmpegVideoOutput::Counters FfmpegVideoOutput::counters() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

void FfmpegVideoOutput::setMaxQueuedBytes(size_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    maxQueuedBytes_ = bytes;
}

void FfmpegVideoOutput::streamStopped()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streamStopped_ = true;
        pendingStartMs_ = -1;
    }
    // The shell keeps its page instead of showing Android Auto's last picture until the stream is back
    const void* self = this;
    QMetaObject::invokeMethod(&shell::AaBridge::instance(), [self]() {
        shell::AaBridge::instance().setPictureWaiting(self, true);
    }, Qt::QueuedConnection);
}

void FfmpegVideoOutput::streamStarted()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // The decoder and the sink keep running: the last picture stays until the new stream shows one (#88)
    pendingStartMs_ = nowMs();
    pendingRestart_ = streamStopped_;
    streamStopped_ = false;
}

void FfmpegVideoOutput::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!running_ && !thread_.joinable())
        {
            return;
        }
        running_ = false;
        queue_.clear();
        queuedBytes_ = 0;
        wake_.notify_one();
    }
    if(thread_.joinable())
    {
        thread_.join();
    }

    const void* self = this;
    QMetaObject::invokeMethod(&shell::AaBridge::instance(), [self]() {
        shell::AaBridge::instance().setVideoOutputActive(self, false);
    }, Qt::QueuedConnection);
}

std::string FfmpegVideoOutput::decoderName() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return decoderName_;
}

std::string FfmpegVideoOutput::softwareDecoder() const
{
    return hevc_ ? "hevc" : "h264";
}

std::vector<std::string> FfmpegVideoOutput::decoderCandidates() const
{
    const std::string software = this->softwareDecoder();
    const std::string configured = configuration_->getVideoDecoder();
    if(configured == "software" || configured == software)
    {
        return {software};
    }
    // [Video] Decoder names a decoder of the other codec: the automatic choice for this one
    const bool otherCodec = configured.rfind(hevc_ ? "h264" : "hevc", 0) == 0;
    if(configured.empty() || configured == "auto" || otherCodec)
    {
#if defined(__aarch64__) || defined(__arm__)
        return {software + "_v4l2m2m", software};
#else
        return {software};
#endif
    }
    return {configured, software};
}

void FfmpegVideoOutput::setCodec(aap_protobuf::service::media::shared::message::MediaCodecType codec)
{
    // the phone announces it in the setup message (H.265 when OpenHU offered it, #116)
    hevc_ = codec == aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H265;
    OPENAUTO_LOG(info) << "[FfmpegVideoOutput] codec " << (hevc_ ? "H.265" : "H.264");
}

bool FfmpegVideoOutput::offerH265() const
{
    const std::string codec = configuration_->getVideoCodec();
    if(codec != "auto")
    {
        return codec == "h265";
    }
    // Above 1080p the phone only streams H.265: offered H.264 alone, it ends the session about 3 s after the video
    // setup, before the first picture, and reconnects again and again (Fairphone 4, 2560x1440). Up to 1080p auto
    // stays H.264 until the Raspberry Pi 5 (hardware HEVC, no hardware H.264) is measured (#10).
    using aap_protobuf::service::media::sink::message::VideoCodecResolutionType;
    switch(this->getVideoResolution())
    {
    case VideoCodecResolutionType::VIDEO_2560x1440:
    case VideoCodecResolutionType::VIDEO_3840x2160:
    case VideoCodecResolutionType::VIDEO_1440x2560:
    case VideoCodecResolutionType::VIDEO_2160x3840:
        return true;
    default:
        return false;
    }
}

bool FfmpegVideoOutput::openDecoder(const std::string& name)
{
    const AVCodec* codec = avcodec_find_decoder_by_name(name.c_str());
    if(codec == nullptr)
    {
        OPENAUTO_LOG(info) << "[FfmpegVideoOutput] decoder " << name << " not available";
        return false;
    }

    codec_ = avcodec_alloc_context3(codec);
    // Show every frame as soon as it is decoded: no frame threading (adds a frame of latency per thread)
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_->thread_type = FF_THREAD_SLICE;
    codec_->thread_count = 0;

    if(avcodec_open2(codec_, codec, nullptr) < 0)
    {
        OPENAUTO_LOG(info) << "[FfmpegVideoOutput] decoder " << name << " cannot be opened";
        avcodec_free_context(&codec_);
        return false;
    }

    hardware_ = name != this->softwareDecoder();
    packetsWithoutFrame_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decoderName_ = name;
    }
    OPENAUTO_LOG(info) << "[FfmpegVideoOutput] decoder " << name << (hardware_ ? "" : " (software)");
    return true;
}

void FfmpegVideoOutput::closeDecoder()
{
    avcodec_free_context(&codec_);
    sws_freeContext(sws_);
    sws_ = nullptr;
}

void FfmpegVideoOutput::decodeLoop()
{
    for(const auto& name: this->decoderCandidates())
    {
        if(this->openDecoder(name))
        {
            break;
        }
    }
    if(codec_ == nullptr)
    {
        OPENAUTO_LOG(error) << "[FfmpegVideoOutput] no " << (hevc_ ? "H.265" : "H.264")
                            << " decoder, Android Auto shows no picture";
        this->failed(hevc_ ? qtTrId("video.no_h_265_decoder") : qtTrId("video.no_h_264_decoder"));
        return;
    }

    frame_ = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    stats_ = Stats();
    statsSince_ = nowMs();
    streamStart_ = StreamStart();
    lastFormat_ = QVideoFrameFormat();
    debugLog_ = debugLogEnabled();
    lastLuma_.clear();
    decodeDelayMs_ = 0;
    if(const char* delay = std::getenv("OPENHU_DEBUG_DECODE_DELAY_MS"); delay != nullptr && *delay != '\0')
    {
        decodeDelayMs_ = std::max(0, std::atoi(delay));
        OPENAUTO_LOG(warning) << "[FfmpegVideoOutput] simulated slow decoder: " << decodeDelayMs_
                              << " ms more per picture (OPENHU_DEBUG_DECODE_DELAY_MS)";
    }
    // Messages until the first picture, replayed into the software decoder if the hardware one fails
    std::vector<QByteArray> history;
    bool pictureSeen = false;
    // The phone sends SPS/PPS as a message of its own: decoded together with the next picture
    QByteArray config;

    while(true)
    {
        QByteArray data;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]() { return !running_ || !queue_.empty(); });
            if(!running_)
            {
                break;
            }
            stats_.maxBacklog = std::max(stats_.maxBacklog, queue_.size());
            Message& message = queue_.front();
            if(message.streamStartMs >= 0)
            {
                debugLog_ = debugLogEnabled();
                streamStart_ = StreamStart();
                streamStart_.active = true;
                streamStart_.restart = message.restart;
                streamStart_.startedMs = message.streamStartMs;
            }
            data = std::move(message.data);
            queue_.pop_front();
            queuedBytes_ -= data.size();
        }
        // The message left the queue: the phone may send the next one while this one is decoded
        this->consumed(1);
        this->logMessageAfterStart(data);

        if(!containsPicture(data, hevc_) && config.size() + data.size() <= cMaxConfigBytes)
        {
            config.append(data);
            continue;
        }
        if(!config.isEmpty())
        {
            data.prepend(config);
            config.clear();
        }

        if(!pictureSeen && hardware_)
        {
            history.push_back(data);
        }

        if(decodeDelayMs_ > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(decodeDelayMs_));
        }
        packet->data = reinterpret_cast<uint8_t*>(data.data());
        packet->size = static_cast<int>(data.size());
        const int framesBefore = stats_.frames;
        this->decode(packet);
        if(stats_.frames > framesBefore)
        {
            pictureSeen = true;
            history.clear();
        }

        if(!pictureSeen && hardware_ && packetsWithoutFrame_ >= cHardwareProbePackets)
        {
            OPENAUTO_LOG(warning) << "[FfmpegVideoOutput] " << this->decoderName() << " shows no picture, switching to software";
            this->closeDecoder();
            if(!this->openDecoder(this->softwareDecoder()))
            {
                OPENAUTO_LOG(error) << "[FfmpegVideoOutput] no software decoder either, Android Auto shows no picture";
                this->failed(hevc_ ? qtTrId("video.no_h_265_decoder") : qtTrId("video.no_h_264_decoder"));
                break;
            }
            for(auto& old: history)
            {
                packet->data = reinterpret_cast<uint8_t*>(old.data());
                packet->size = static_cast<int>(old.size());
                this->decode(packet);
            }
            history.clear();
        }

        this->logStats(false);
        this->publishOverlayStats();
    }

    this->logStats(true);
    av_packet_free(&packet);
    av_frame_free(&frame_);
    this->closeDecoder();
}

void FfmpegVideoOutput::decode(AVPacket* packet)
{
    const auto started = std::chrono::steady_clock::now();
    bool gotFrame = false;
    int sent = avcodec_send_packet(codec_, packet);
    // EAGAIN: the packet was not taken, the decoder wants its output read first (e.g. h264_v4l2m2m with a
    // full queue). Dropping it would break the reference chain until the next IDR frame.
    for(int attempt = 1; sent == AVERROR(EAGAIN) && attempt < cMaxSendAttempts; ++attempt)
    {
        if(this->receiveFrames())
        {
            gotFrame = true;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        sent = avcodec_send_packet(codec_, packet);
    }
    if(sent < 0)
    {
        ++stats_.errors;
    }

    if(this->receiveFrames())
    {
        gotFrame = true;
    }
    packetsWithoutFrame_ = gotFrame ? 0 : packetsWithoutFrame_ + 1;
    const double decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    stats_.decodeMs += decodeMs;
    overlay_.decodeMs += decodeMs;
}

bool FfmpegVideoOutput::receiveFrames()
{
    bool gotFrame = false;
    while(avcodec_receive_frame(codec_, frame_) == 0)
    {
        gotFrame = true;
        ++stats_.frames;
        ++overlay_.frames;
        overlay_.width = frame_->width;
        overlay_.height = frame_->height;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counters_.decodedFrames;
        }
        if(this->showAfterStart(frame_))
        {
            this->present(this->toVideoFrame(frame_));
        }
        av_frame_unref(frame_);
    }
    return gotFrame;
}

void FfmpegVideoOutput::logMessageAfterStart(const QByteArray& data)
{
    if(!streamStart_.active)
    {
        return;
    }
    ++streamStart_.messages;
    const bool keyframe = containsKeyframe(data, hevc_);
    streamStart_.idrSeen = streamStart_.idrSeen || keyframe;
    const int64_t elapsed = nowMs() - streamStart_.startedMs;
    if(elapsed < cDiagnosticMs)
    {
        OPENAUTO_LOG(debug) << "[FfmpegVideoOutput] +" << elapsed << " ms after the stream "
                            << (streamStart_.restart ? "restart" : "start") << ": message " << streamStart_.messages
                            << ", " << data.size() << " bytes, NAL types " << nalTypes(data, hevc_);
    }
}

bool FfmpegVideoOutput::showAfterStart(const AVFrame* frame)
{
    // With OPENHU_LOG=debug the luma of every frame is sampled, so the first frame after a restart can be compared
    // with the last one before the stop (the picture the sink kept). Until the first picture after a restart it is
    // sampled anyway, to hold back blank frames.
    const bool restartPending = streamStart_.active && streamStart_.restart && !streamStart_.shown;
    std::vector<uint8_t> luma;
    if(debugLog_ || restartPending)
    {
        luma = lumaSamples(frame);
    }
    if(!streamStart_.active)
    {
        lastLuma_ = std::move(luma);
        return true;
    }
    const int64_t elapsed = nowMs() - streamStart_.startedMs;
    ++streamStart_.frames;
    // Reference frames missing or slices lost: FFmpeg conceals the errors, the picture is partly wrong
    const bool corrupt = (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 || frame->decode_error_flags != 0;
    // After a restart the sink still shows the last picture: black frames before the phone's UI are not shown
    const bool blank = restartPending && blankFrame(luma);
    const bool show = streamStart_.shown || (!corrupt && !blank) || elapsed >= cMaxHoldMs;

    if(elapsed < cDiagnosticMs)
    {
        // A dark or very different frame among these is what the user sees as a flicker. "change": mean absolute
        // luma difference to the previous decoded frame (for the first one: the last frame before the stop).
        double mean = -1;
        double change = -1;
        if(!luma.empty())
        {
            double sum = 0;
            double diff = 0;
            const bool comparable = lastLuma_.size() == luma.size();
            for(size_t i = 0; i < luma.size(); ++i)
            {
                sum += luma[i];
                if(comparable)
                {
                    diff += std::abs(int(luma[i]) - int(lastLuma_[i]));
                }
            }
            mean = sum / luma.size();
            change = comparable ? diff / luma.size() : -1;
        }
        OPENAUTO_LOG(debug) << "[FfmpegVideoOutput] +" << elapsed << " ms after the stream "
                            << (streamStart_.restart ? "restart" : "start") << ": frame " << streamStart_.frames
                            << " " << frame->width << "x" << frame->height << ", type "
                            << av_get_picture_type_char(frame->pict_type) << (corrupt ? ", decode errors" : "")
                            << (blank ? ", blank" : "")
                            << ", mean luma " << mean << ", change " << change << (show ? "" : ", held back");
    }
    lastLuma_ = std::move(luma);

    if(!show)
    {
        ++streamStart_.held;
        return false;
    }
    if(!streamStart_.shown)
    {
        streamStart_.shown = true;
        if(streamStart_.restart)
        {
            restartPicturePending_ = true;
        }
        OPENAUTO_LOG(info) << "[FfmpegVideoOutput] first picture after the stream "
                           << (streamStart_.restart ? "restart" : "start") << ": " << elapsed << " ms, "
                           << streamStart_.messages << " messages, " << (streamStart_.idrSeen ? "IDR" : "no IDR")
                           << ", " << streamStart_.held << " frames held back (decode errors or blank)";
    }
    if(elapsed >= cDiagnosticMs)
    {
        streamStart_.active = false;
    }
    return true;
}

void FfmpegVideoOutput::failed(const QString& error)
{
    int cleared = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        cleared = static_cast<int>(queue_.size());
        queue_.clear();
        queuedBytes_ = 0;
    }
    // No decoder: what was queued is acknowledged, later messages at once (write() without running_)
    this->consumed(cleared);
    const void* self = this;
    QMetaObject::invokeMethod(&shell::AaBridge::instance(), [self, error]() {
        shell::AaBridge::instance().setVideoOutputActive(self, false);
        shell::AaBridge::instance().setVideoError(error);
    }, Qt::QueuedConnection);
}

QVideoFrame FfmpegVideoOutput::toVideoFrame(const AVFrame* frame)
{
    const int width = frame->width;
    const int height = frame->height;
    const auto pixelFormat = static_cast<AVPixelFormat>(frame->format);
    const bool nv12 = pixelFormat == AV_PIX_FMT_NV12;
    const bool yuv420 = pixelFormat == AV_PIX_FMT_YUV420P || pixelFormat == AV_PIX_FMT_YUVJ420P;

    QVideoFrameFormat format(QSize(width, height), nv12 ? QVideoFrameFormat::Format_NV12 : QVideoFrameFormat::Format_YUV420P);
    format.setColorRange(pixelFormat == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG
                             ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
    format.setColorSpace(frame->colorspace == AVCOL_SPC_BT709 ? QVideoFrameFormat::ColorSpace_BT709
                                                              : QVideoFrameFormat::ColorSpace_BT601);
    // The VideoOutput sets up its rendering again for another format: rare, a candidate for a visible flicker
    if(lastFormat_.isValid()
       && (lastFormat_.frameSize() != format.frameSize() || lastFormat_.pixelFormat() != format.pixelFormat()
           || lastFormat_.colorSpace() != format.colorSpace() || lastFormat_.colorRange() != format.colorRange()))
    {
        OPENAUTO_LOG(info) << "[FfmpegVideoOutput] video format changed: " << width << "x" << height << ", "
                           << QVideoFrameFormat::pixelFormatToString(format.pixelFormat()).toStdString()
                           << ", color space " << format.colorSpace() << ", range " << format.colorRange();
    }
    lastFormat_ = format;

    QVideoFrame result(format);
    if(!result.map(QVideoFrame::WriteOnly))
    {
        ++stats_.errors;
        return QVideoFrame();
    }

    if(nv12 || yuv420)
    {
        const int planes = nv12 ? 2 : 3;
        for(int plane = 0; plane < planes; ++plane)
        {
            const int rows = plane == 0 ? height : (height + 1) / 2;
            const int rowBytes = plane == 0 ? width : (nv12 ? 2 : 1) * ((width + 1) / 2);
            const int bytes = std::min(rowBytes, result.bytesPerLine(plane));
            uchar* target = result.bits(plane);
            const uint8_t* source = frame->data[plane];
            for(int row = 0; row < rows; ++row)
            {
                std::memcpy(target + row * result.bytesPerLine(plane), source + row * frame->linesize[plane], bytes);
            }
        }
    }
    else
    {
        // Other decoder output (e.g. 4:2:2): convert to YUV420P
        sws_ = sws_getCachedContext(sws_, width, height, pixelFormat, width, height, AV_PIX_FMT_YUV420P,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        uint8_t* targets[4] = {result.bits(0), result.bits(1), result.bits(2), nullptr};
        int strides[4] = {result.bytesPerLine(0), result.bytesPerLine(1), result.bytesPerLine(2), 0};
        if(sws_ == nullptr || sws_scale(sws_, frame->data, frame->linesize, 0, height, targets, strides) <= 0)
        {
            ++stats_.errors;
            result.unmap();
            return QVideoFrame();
        }
    }

    result.unmap();
    return result;
}

void FfmpegVideoOutput::present(QVideoFrame frame)
{
    if(!frame.isValid())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if(presentScheduled_)
        {
            // The GUI thread has not shown the previous frame yet: show only the newest one
            ++stats_.dropped;
            pendingFrame_ = std::move(frame);
            return;
        }
        pendingFrame_ = std::move(frame);
        presentScheduled_ = true;
    }

    QMetaObject::invokeMethod(this, [this]() {
        QVideoFrame next;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            next = std::move(pendingFrame_);
            pendingFrame_ = QVideoFrame();
            presentScheduled_ = false;
        }
        QVideoSink* sink = useTestSink_ ? testSink_.data() : shell::AaBridge::instance().videoSink();
        if(sink != nullptr)
        {
            sink->setVideoFrame(next);
        }
        if(restartPicturePending_.exchange(false))
        {
            shell::AaBridge::instance().setPictureWaiting(this, false);
        }
    }, Qt::QueuedConnection);
}

void FfmpegVideoOutput::publishOverlayStats()
{
    const int64_t now = nowMs();
    if(now - overlaySince_ < 1000)
    {
        return;
    }
    const int64_t elapsed = now - overlaySince_;
    const OverlayStats stats = overlay_;
    overlay_ = OverlayStats{0, 0, stats.width, stats.height};
    overlaySince_ = now;
    if(!core::EventBus::instance().last("debug.state").value("videoOverlay").toBool())
    {
        return;
    }
    core::EventBus::instance().publish("video.stats", {
        {"fps", std::round(stats.frames * 10000.0 / std::max<int64_t>(elapsed, 1)) / 10},
        {"decodeMs", stats.frames > 0 ? std::round(stats.decodeMs / stats.frames * 10) / 10 : 0.0},
        {"decoder", QString::fromStdString(this->decoderName())},
        {"hardware", hardware_},
        {"codec", hevc_ ? QStringLiteral("h265") : QStringLiteral("h264")},
        {"width", stats.width},
        {"height", stats.height},
    });
}

void FfmpegVideoOutput::logStats(bool force)
{
    const int64_t now = nowMs();
    const int64_t elapsed = now - statsSince_;
    if(!force && elapsed < cStatsIntervalMs)
    {
        return;
    }
    if(stats_.frames > 0 || stats_.errors > 0)
    {
        OPENAUTO_LOG(info) << "[FfmpegVideoOutput] " << this->decoderName() << ": "
                           << (elapsed > 0 ? stats_.frames * 1000.0 / elapsed : 0) << " fps, "
                           << (stats_.frames > 0 ? stats_.decodeMs / stats_.frames : 0) << " ms/frame decoding, "
                           << stats_.dropped << " frames replaced before display, " << stats_.errors << " errors, "
                           << "backlog up to " << stats_.maxBacklog << " messages";
    }
    stats_ = Stats();
    statsSince_ = now;
}

}
}
}
}
