// FfmpegVideoOutput with a misbehaving decoder (openhu#16). The FFmpeg calls are interposed: this executable
// defines them and reaches the real ones with dlsym(RTLD_NEXT), so a decoder that answers EAGAIN, a slow decoder
// or a missing decoder can be simulated. Separate from test_video, whose decoder must stay untouched.
#include <QVideoSink>
#include <QtTest>
#include <atomic>
#include <condition_variable>
#include <dlfcn.h>
#include <mutex>
#include <f1x/openauto/autoapp/Configuration/Configuration.hpp>
#include <f1x/openauto/autoapp/Projection/FfmpegVideoOutput.hpp>
#include <f1x/openauto/autoapp/Shell/AaBridge.hpp>
#include "TestHome.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

using f1x::openauto::autoapp::configuration::Configuration;
using f1x::openauto::autoapp::projection::FfmpegVideoOutput;
using f1x::openauto::autoapp::shell::AaBridge;

namespace hook
{
std::atomic<int> sendCalls{0};
std::atomic<int> eagainAt{-1};        // send call index that answers EAGAIN once
std::atomic<int> framesOut{0};
std::atomic<int> corruptFrames{0};
std::atomic<int> markCorrupt{0};      // the next n decoded frames get decode errors (concealed slices)
std::atomic<bool> noFrames{false};     // receive_frame never returns a frame ("hardware shows no picture")
std::atomic<bool> noH264{false};       // avcodec_find_decoder_by_name("h264") fails
std::atomic<bool> noDecoderAtAll{false};
std::atomic<bool> recordSlices{false};
std::atomic<int> nonIdrBeforeIdr{0};  // P slices sent after recordSlices was set, before the next IDR
std::atomic<bool> idrSeen{false};
std::mutex lastMutex;
std::vector<uint8_t> lastLuma;  // luma of the last decoded frame
std::mutex gateMutex;
std::condition_variable gateCv;
bool openGate = true;
bool sendGate = true;

void waitGate(bool& gate)
{
    std::unique_lock<std::mutex> lock(gateMutex);
    gateCv.wait(lock, [&gate]() { return gate; });
}
void setGate(bool& gate, bool open)
{
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        gate = open;
    }
    gateCv.notify_all();
}
void reset()
{
    sendCalls = 0; eagainAt = -1; framesOut = 0; corruptFrames = 0; noFrames = false; noH264 = false; markCorrupt = 0;
    noDecoderAtAll = false; recordSlices = false; nonIdrBeforeIdr = 0; idrSeen = false;
    setGate(openGate, true);
    setGate(sendGate, true);
}

template<typename T>
T real(const char* name)
{
    return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

void inspect(const AVPacket* packet)
{
    if(packet == nullptr || !recordSlices || idrSeen)
    {
        return;
    }
    for(int i = 0; i + 3 < packet->size; ++i)
    {
        if(packet->data[i] == 0 && packet->data[i + 1] == 0 && packet->data[i + 2] == 1)
        {
            const int type = packet->data[i + 3] & 0x1f;
            if(type == 5) { idrSeen = true; return; }
            if(type == 1) { ++nonIdrBeforeIdr; return; }
        }
    }
}
}

extern "C" {

int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet)
{
    static auto realSend = hook::real<int (*)(AVCodecContext*, const AVPacket*)>("avcodec_send_packet");
    if(context->codec != nullptr && av_codec_is_decoder(context->codec))
    {
        hook::waitGate(hook::sendGate);
        const int index = hook::sendCalls++;
        if(index == hook::eagainAt)
        {
            return AVERROR(EAGAIN);  // not accepted, like a full v4l2m2m queue
        }
        hook::inspect(packet);
    }
    return realSend(context, packet);
}

int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame)
{
    static auto realReceive = hook::real<int (*)(AVCodecContext*, AVFrame*)>("avcodec_receive_frame");
    const int result = realReceive(context, frame);
    if(result == 0 && hook::noFrames)
    {
        av_frame_unref(frame);
        return AVERROR(EAGAIN);
    }
    if(result == 0 && hook::markCorrupt > 0)
    {
        --hook::markCorrupt;
        frame->decode_error_flags |= FF_DECODE_ERROR_CONCEALMENT_ACTIVE;
    }
    if(result == 0)
    {
        ++hook::framesOut;
        {
            std::lock_guard<std::mutex> lock(hook::lastMutex);
            hook::lastLuma.clear();
            for(int y = 0; y < frame->height; ++y)
            {
                hook::lastLuma.insert(hook::lastLuma.end(), frame->data[0] + y * frame->linesize[0],
                                      frame->data[0] + y * frame->linesize[0] + frame->width);
            }
        }
        if((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags != 0)
        {
            ++hook::corruptFrames;
        }
    }
    return result;
}

const AVCodec* avcodec_find_decoder_by_name(const char* name)
{
    static auto realFind = hook::real<const AVCodec* (*)(const char*)>("avcodec_find_decoder_by_name");
    if(hook::noDecoderAtAll || (hook::noH264 && std::string(name) == "h264"))
    {
        return nullptr;
    }
    if(std::string(name) == "fakehw")
    {
        return realFind("h264");  // opens, but counts as a hardware decoder (name != "h264")
    }
    return realFind(name);
}

int avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options)
{
    static auto realOpen = hook::real<int (*)(AVCodecContext*, const AVCodec*, AVDictionary**)>("avcodec_open2");
    if(codec != nullptr && av_codec_is_decoder(codec))
    {
        hook::waitGate(hook::openGate);
    }
    return realOpen(context, codec, options);
}
}

namespace
{
constexpr int cWidth = 320;
constexpr int cHeight = 240;

// Access units: 'frames' pictures, first one an IDR with SPS/PPS; luma changes per frame so every frame differs.
// The first 'black' pictures are black (luma 16), like the phone's first frames after a stream restart.
QList<QByteArray> encode(int frames, int luma, int black = 0, const char* encoder = "libx264")
{
    QList<QByteArray> packets;
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder);
    if(codec == nullptr)
    {
        return packets;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    context->width = cWidth;
    context->height = cHeight;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = {1, 30};
    context->framerate = {30, 1};
    context->gop_size = 600;
    context->max_b_frames = 0;
    av_opt_set(context->priv_data, "preset", "ultrafast", 0);
    av_opt_set(context->priv_data, "tune", "zerolatency", 0);
    avcodec_open2(context, codec, nullptr);
    AVFrame* frame = av_frame_alloc();
    frame->format = context->pix_fmt;
    frame->width = cWidth;
    frame->height = cHeight;
    av_frame_get_buffer(frame, 0);
    AVPacket* packet = av_packet_alloc();
    auto drain = [&]() {
        while(avcodec_receive_packet(context, packet) == 0)
        {
            packets.append(QByteArray(reinterpret_cast<const char*>(packet->data), packet->size));
            av_packet_unref(packet);
        }
    };
    for(int i = 0; i < frames; ++i)
    {
        av_frame_make_writable(frame);
        for(int y = 0; y < cHeight; ++y)
        {
            for(int x = 0; x < cWidth; ++x)
            {
                frame->data[0][y * frame->linesize[0] + x] =
                    i < black ? uint8_t(16) : uint8_t(luma + ((x / 16 + y / 16 + i) % 4) * 20);
            }
        }
        for(int plane = 1; plane < 3; ++plane)
        {
            for(int y = 0; y < cHeight / 2; ++y)
            {
                memset(frame->data[plane] + y * frame->linesize[plane], 128, cWidth / 2);
            }
        }
        frame->pts = i;
        avcodec_send_frame(context, frame);
        drain();
    }
    avcodec_send_frame(context, nullptr);
    drain();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return packets;
}

// SPS/PPS/SEI as a message of their own, like the phone sends them
QList<QByteArray> splitCodecConfig(QList<QByteArray> packets)
{
    const QByteArray first = packets.first();
    for(int i = 0; i + 4 < first.size(); ++i)
    {
        if(first[i] == 0 && first[i + 1] == 0 && first[i + 2] == 1 && (first[i + 3] & 0x1f) == 5)
        {
            const int start = i > 0 && first[i - 1] == 0 ? i - 1 : i;
            packets.replace(0, first.mid(start));
            packets.prepend(first.left(start));
            break;
        }
    }
    return packets;
}

// Luma of the last frame when all packets are decoded without loss
std::vector<uint8_t> referenceLastLuma(const QList<QByteArray>& packets)
{
    AVCodecContext* context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    avcodec_open2(context, context->codec, nullptr);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    for(QByteArray data: packets)
    {
        packet->data = reinterpret_cast<uint8_t*>(data.data());
        packet->size = data.size();
        avcodec_send_packet(context, packet);
        while(avcodec_receive_frame(context, frame) == 0) { av_frame_unref(frame); }
    }
    std::vector<uint8_t> result;
    {
        std::lock_guard<std::mutex> lock(hook::lastMutex);
        result = hook::lastLuma;
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return result;
}

double meanAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if(a.size() != b.size() || a.empty()) { return -1; }
    double sum = 0;
    for(size_t i = 0; i < a.size(); ++i) { sum += std::abs(int(a[i]) - int(b[i])); }
    return sum / a.size();
}

void write(FfmpegVideoOutput& output, const QByteArray& data)
{
    output.write(0, aasdk::common::DataConstBuffer(reinterpret_cast<const uint8_t*>(data.constData()), data.size()));
}

// What the shell's sink was given: -1 for an empty frame (cleared), else the luma of the top left pixel, which tells
// the frames of encode() apart (luma + (frame % 4) * 20)
class SinkRecorder
{
public:
    explicit SinkRecorder(QVideoSink& sink)
    {
        QObject::connect(&sink, &QVideoSink::videoFrameChanged, &sink, [this](const QVideoFrame& frame) {
            QVideoFrame copy(frame);
            if(!copy.isValid() || !copy.map(QVideoFrame::ReadOnly))
            {
                frames.append(-1);
                return;
            }
            frames.append(copy.bits(0)[0]);
            copy.unmap();
        });
    }
    QList<int> frames;

    // Frame numbers (frame % 4) of the recorded frames from 'from' on, -1 for an empty frame
    QList<int> numbers(int luma, qsizetype from = 0) const
    {
        QList<int> result;
        for(const int value: frames.mid(from))
        {
            result.append(value < 0 ? -1 : qRound((value - luma) / 20.0));
        }
        return result;
    }
    int valid() const { return int(frames.size() - frames.count(-1)); }
};

// One message at a time, each decoded frame reaches the sink before the next message (no frame is replaced
// before display)
void writeSlowly(FfmpegVideoOutput& output, const QList<QByteArray>& packets)
{
    for(const auto& packet: packets)
    {
        const int before = hook::framesOut;
        write(output, packet);
        bool picture = false;
        for(int i = 0; i + 3 < packet.size(); ++i)
        {
            const int type = packet[i + 3] & 0x1f;
            if(packet[i] == 0 && packet[i + 1] == 0 && packet[i + 2] == 1 && (type == 1 || type == 5))
            {
                picture = true;
            }
        }
        if(picture)
        {
            QTRY_COMPARE_WITH_TIMEOUT(hook::framesOut.load(), before + 1, 2000);
        }
        QTest::qWait(20);
    }
}
}

class TestVideoDecoder: public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if(encode(1, 100).isEmpty())
        {
            QSKIP("FFmpeg has no libx264 encoder to create the test stream");
        }
    }

    void init() { hook::reset(); }

    // H.265 (#116): the codec of the setup message picks the decoder; a [Video] Decoder of the other codec does not
    // apply
    void hevcStream()
    {
        const auto packets = encode(10, 40, 0, "libx265");
        if(packets.isEmpty())
        {
            QSKIP("FFmpeg has no libx265 encoder to create the test stream");
        }
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("h264_v4l2m2m");
        configuration->setVideoCodec("h265");
        QVideoSink sink;
        FfmpegVideoOutput output(configuration, &sink);
        QVERIFY(output.offerH265());
        output.setCodec(aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H265);
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("hevc"));
        for(const auto& packet: packets)
        {
            write(output, packet);
        }
        QTRY_COMPARE(hook::framesOut.load(), 10);
        output.stop();

        configuration->setVideoCodec("auto");
        QVERIFY(!output.offerH265());
    }

    // auto: H.265 above 1080p (the phone streams 1440p and 4K only in H.265), H.264 up to 1080p
    void autoOffersH265Above1080p()
    {
        using aap_protobuf::service::media::sink::message::VideoCodecResolutionType;
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoCodec("auto");
        configuration->setVideoResolutionAuto(false);
        QVideoSink sink;
        FfmpegVideoOutput output(configuration, &sink);
        const std::pair<VideoCodecResolutionType, bool> cases[] = {
            {VideoCodecResolutionType::VIDEO_1920x1080, false}, {VideoCodecResolutionType::VIDEO_1080x1920, false},
            {VideoCodecResolutionType::VIDEO_2560x1440, true},  {VideoCodecResolutionType::VIDEO_1440x2560, true},
            {VideoCodecResolutionType::VIDEO_3840x2160, true}};
        for(const auto& [resolution, h265]: cases)
        {
            configuration->setVideoResolution(resolution);
            f1x::openauto::autoapp::shell::AaBridge::instance().configure(configuration);
            QCOMPARE(output.offerH265(), h265);
        }
        // a codec set by hand wins
        configuration->setVideoCodec("h264");
        QVERIFY(!output.offerH265());
    }

    // EAGAIN from avcodec_send_packet: the packet is sent again, every picture is decoded
    void eagainSendsThePacketAgain()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        FfmpegVideoOutput output(configuration, &sink);
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("h264"));

        const auto packets = splitCodecConfig(encode(10, 40));
        QCOMPARE(packets.size(), 11);
        hook::eagainAt = 4;  // the 5th picture
        for(const auto& packet: packets)
        {
            write(output, packet);
        }
        QTRY_COMPARE(hook::framesOut.load(), 10);
        QCOMPARE(hook::sendCalls.load(), 11);
        output.stop();
    }

    // Overflow before the decoder has read SPS/PPS: they are kept, the next IDR frame shows a picture
    void overflowKeepsCodecConfig()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        FfmpegVideoOutput output(configuration, &sink);

        hook::setGate(hook::openGate, false);  // slow decoder start (e.g. a hardware decoder opening)
        QVERIFY(output.init());
        for(const auto& packet: splitCodecConfig(encode(5, 40)))
        {
            write(output, packet);  // SPS/PPS, IDR, P...
        }
        const QByteArray filler = QByteArray("\x00\x00\x01\x41", 4) + QByteArray(1024 * 1024, char(0x55));
        for(int i = 0; i < 9; ++i)
        {
            write(output, filler);  // more than 8 MB queued: the pictures are dropped
        }
        hook::setGate(hook::openGate, true);
        QTest::qWait(300);
        QCOMPARE(hook::framesOut.load(), 0);

        // next IDR from the phone, without SPS/PPS (sent once per session)
        auto next = splitCodecConfig(encode(5, 120));
        next.removeFirst();
        for(const auto& packet: next)
        {
            write(output, packet);
        }
        QTRY_COMPARE(hook::framesOut.load(), 5);
        output.stop();
    }

    // Overflow in the middle of the stream: nothing reaches the decoder until the next IDR frame, which is
    // then decoded exactly like a lossless stream
    void overflowWaitsForTheNextIdr()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        FfmpegVideoOutput output(configuration, &sink);
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("h264"));

        const auto packets = splitCodecConfig(encode(30, 40));
        for(int i = 0; i < 6; ++i)
        {
            write(output, packets[i]);  // SPS/PPS, IDR, 4 P
        }
        QTRY_COMPARE(hook::framesOut.load(), 5);

        hook::setGate(hook::sendGate, false);  // decoder busy
        write(output, packets[6]);             // taken by the decoder thread, blocks in send
        QTest::qWait(100);
        for(int i = 7; i < 12; ++i)
        {
            write(output, packets[i]);
        }
        const QByteArray filler = QByteArray("\x00\x00\x01\x41", 4) + QByteArray(1024 * 1024, char(0x55));
        for(int i = 0; i < 9; ++i)
        {
            write(output, filler);  // more than 8 MB queued: dropped
        }
        hook::recordSlices = true;
        hook::setGate(hook::sendGate, true);
        for(int i = 12; i < packets.size(); ++i)
        {
            write(output, packets[i]);  // P-frames whose references were dropped
        }
        QTest::qWait(500);
        // only packets[6], taken by the decoder before the overflow
        QCOMPARE(hook::nonIdrBeforeIdr.load(), 1);
        QCOMPARE(hook::framesOut.load(), 6);  // the 5 before and packets[6], already in the decoder

        auto next = splitCodecConfig(encode(5, 120));
        const QList<QByteArray> withoutConfig = next.mid(1);
        for(const auto& packet: withoutConfig)
        {
            write(output, packet);
        }
        QTRY_COMPARE(hook::framesOut.load(), 11);
        QVERIFY(hook::idrSeen.load());
        std::vector<uint8_t> shown;
        {
            std::lock_guard<std::mutex> lock(hook::lastMutex);
            shown = hook::lastLuma;
        }
        output.stop();
        QCOMPARE(meanAbsDiff(shown, referenceLastLuma(next)), 0.0);
    }

    // The phone stops the stream (video focus NATIVE: reverse camera, "Exit") and starts it again with a new
    // encoder (SPS/PPS, IDR) in the same connection: the last picture stays, nothing clears it (#88)
    void streamRestartKeepsTheLastPicture()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        AaBridge::instance().setVideoSink(&sink);  // the shell's sink, as in the running shell
        SinkRecorder recorder(sink);
        FfmpegVideoOutput output(configuration);
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("h264"));
        QTRY_VERIFY(AaBridge::instance().videoActive());

        output.streamStarted();
        writeSlowly(output, splitCodecConfig(encode(4, 40)));
        QTRY_COMPARE(recorder.valid(), 4);
        const qsizetype mark = recorder.frames.size();

        QVERIFY(!AaBridge::instance().pictureWaiting());
        output.streamStopped();
        QTRY_VERIFY(AaBridge::instance().pictureWaiting());  // the shell keeps its page meanwhile
        QTest::qWait(100);
        output.streamStarted();
        writeSlowly(output, splitCodecConfig(encode(4, 120)));
        QTRY_COMPARE(recorder.frames.size(), mark + 4);
        QVERIFY(!AaBridge::instance().pictureWaiting());  // cleared with the first picture of the new stream
        QCOMPARE(recorder.numbers(120, mark), (QList<int>{0, 1, 2, 3}));
        QVERIFY(AaBridge::instance().videoActive());
        QVERIFY(sink.videoFrame().isValid());

        output.stop();
        AaBridge::instance().setVideoSink(nullptr);
    }

    // Frames with decode errors after a restart (e.g. P-frames whose references the phone never sent) are not
    // shown: the last picture stays until the first complete one
    void framesWithErrorsAfterRestartAreHeldBack()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        SinkRecorder recorder(sink);
        FfmpegVideoOutput output(configuration, &sink);
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("h264"));

        output.streamStarted();
        writeSlowly(output, splitCodecConfig(encode(2, 40)));
        QTRY_COMPARE(recorder.valid(), 2);
        const qsizetype mark = recorder.frames.size();

        output.streamStopped();
        output.streamStarted();
        hook::markCorrupt = 2;
        writeSlowly(output, splitCodecConfig(encode(6, 120)));
        // frames 0 and 1 held back, the first one shown is frame 2
        QTRY_COMPARE(recorder.frames.size(), mark + 4);
        QCOMPARE(recorder.numbers(120, mark), (QList<int>{2, 3, 0, 1}));

        // later errors are shown as before (the decoder conceals them, holding would freeze the picture)
        hook::markCorrupt = 1;
        writeSlowly(output, splitCodecConfig(encode(2, 200)));
        QTRY_COMPARE(recorder.frames.size(), mark + 6);
        output.stop();
    }

    // After a restart the phone sends black frames before its UI (Fairphone 4, #88): they are held back, the last
    // picture stays until the first one with content. On the first start of a connection nothing is held back.
    void blankFramesAfterRestartAreHeldBack()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        SinkRecorder recorder(sink);
        FfmpegVideoOutput output(configuration, &sink);
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("h264"));

        output.streamStarted();
        writeSlowly(output, splitCodecConfig(encode(3, 40, 1)));
        QTRY_COMPARE(recorder.valid(), 3);
        QCOMPARE(recorder.frames.first(), 16);  // first start: the black frame is shown
        const qsizetype mark = recorder.frames.size();

        output.streamStopped();
        output.streamStarted();
        writeSlowly(output, splitCodecConfig(encode(6, 120, 3)));
        // frames 0-2 black and held back, the first one shown is frame 3
        QTRY_COMPARE(recorder.frames.size(), mark + 3);
        QCOMPARE(recorder.numbers(120, mark), (QList<int>{3, 0, 1}));
        output.stop();
    }

    // A phone that sends only frames with errors after the restart still gets its picture shown after cMaxHoldMs
    void heldBackAtMostOneSecond()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        SinkRecorder recorder(sink);
        FfmpegVideoOutput output(configuration, &sink);
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("h264"));
        output.streamStarted();
        writeSlowly(output, splitCodecConfig(encode(1, 40)));
        QTRY_COMPARE(recorder.valid(), 1);
        const qsizetype mark = recorder.frames.size();

        output.streamStopped();
        output.streamStarted();
        hook::markCorrupt = 1000;
        const auto packets = splitCodecConfig(encode(8, 120));
        writeSlowly(output, packets.mid(0, 4));  // SPS/PPS and 3 pictures
        QTest::qWait(200);
        QCOMPARE(recorder.frames.size(), mark);
        QTest::qWait(1000);
        writeSlowly(output, packets.mid(4));
        QTRY_COMPARE(recorder.frames.size(), mark + 5);
        QVERIFY(!recorder.numbers(120, mark).contains(-1));
        output.stop();
    }

    // Hardware decoder without a picture, no software decoder: the video is not left marked active
    void noSoftwareFallbackDeactivatesVideo()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("fakehw");
        QVideoSink sink;
        FfmpegVideoOutput output(configuration, &sink);
        hook::noFrames = true;
        hook::noH264 = true;
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("fakehw"));
        QTRY_VERIFY(AaBridge::instance().videoActive());

        for(const auto& packet: splitCodecConfig(encode(70, 40)))
        {
            write(output, packet);
        }
        QTRY_VERIFY(!AaBridge::instance().videoActive());
        QVERIFY(!AaBridge::instance().videoError().isEmpty());

        // a new setup of the video channel starts a new decoder thread
        hook::noFrames = false;
        hook::noH264 = false;
        QVERIFY(output.init());
        QTRY_VERIFY(AaBridge::instance().videoActive());
        QVERIFY(AaBridge::instance().videoError().isEmpty());
        output.stop();
    }

    // No decoder at all, then a second setup of the video channel: no std::terminate
    void noDecoderThenInitAgain()
    {
        TestHome home;
        auto configuration = std::make_shared<Configuration>();
        configuration->setVideoDecoder("software");
        QVideoSink sink;
        FfmpegVideoOutput output(configuration, &sink);
        hook::noDecoderAtAll = true;
        QVERIFY(output.init());
        QTRY_VERIFY(!AaBridge::instance().videoError().isEmpty());
        QTRY_VERIFY(!AaBridge::instance().videoActive());

        hook::noDecoderAtAll = false;
        QVERIFY(output.init());
        QTRY_COMPARE(output.decoderName(), std::string("h264"));
        for(const auto& packet: splitCodecConfig(encode(3, 40)))
        {
            write(output, packet);
        }
        QTRY_COMPARE(hook::framesOut.load(), 3);
        output.stop();
    }
};

QTEST_MAIN(TestVideoDecoder)
#include "test_video_decoder.moc"
