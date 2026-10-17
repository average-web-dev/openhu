/*
*  This file is part of openauto project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  openauto is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  openauto is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with openauto. If not, see <http://www.gnu.org/licenses/>.
*/

#include <QCoreApplication>
#include <QThread>
#include <f1x/openauto/autoapp/Projection/QtAudioOutput.hpp>
#include <f1x/openauto/Common/Log.hpp>

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
// unplayed audio before a packet is written: normally what is left of the previous packet plus jitter
// (a packet is 42.7 ms for media; 40 ms dropped a packet in normal playback once)
constexpr qint64 cMaxQueuedUs = 60000;
// a gap between packets above this (and above two packets) is logged as a stall; normal: at most ~63 ms for
// media packets every 42.7 ms (wireless captures)
constexpr int64_t cStallMs = 100;
}

QtAudioOutput::QtAudioOutput(uint32_t channelCount, uint32_t sampleSize, uint32_t sampleRate, std::string name)
    : audioBuffer_(nullptr)
    , playbackStarted_(false)
    , name_(std::move(name))
{
    audioFormat_.setChannelCount(channelCount);
    audioFormat_.setSampleRate(sampleRate);
    // Android Auto sends and expects 16 bit PCM (sampleSize is always 16)
    audioFormat_.setSampleFormat(sampleSize == 16 ? QAudioFormat::Int16 : QAudioFormat::Int32);

    // the sink lives in the GUI thread; write() comes from the io_service thread (outputMutex_)
    this->moveToThread(QCoreApplication::instance()->thread());

    connect(this, &QtAudioOutput::startPlayback, this, &QtAudioOutput::onStartPlayback);
    connect(this, &QtAudioOutput::suspendPlayback, this, &QtAudioOutput::onSuspendPlayback);
    connect(this, &QtAudioOutput::stopPlayback, this, &QtAudioOutput::onStopPlayback);

    QMetaObject::invokeMethod(this, "createAudioOutput", Qt::BlockingQueuedConnection);
}

void QtAudioOutput::createAudioOutput()
{
    OPENAUTO_LOG(info) << "[QtAudioOutput] createAudioOutput()";
    std::lock_guard<std::mutex> lock(outputMutex_);
    // QAudioSink is opened on a device, not on "the default". With PipeWire 1.4 its stream followed a new default
    // all the same, but a new stream is opened on the default of that moment (onStartPlayback) so that it does not
    // depend on that (Bluetooth car radio, #112; OutputRouter moves the playing ones).
    device_ = QMediaDevices::defaultAudioOutput();
    audioOutput_ = std::make_unique<QAudioSink>(device_, audioFormat_);
}

bool QtAudioOutput::open()
{
    return true;
}

void QtAudioOutput::write(aasdk::messenger::Timestamp::ValueType, const aasdk::common::DataConstBuffer& buffer)
{
    this->watchPacket(qint64(buffer.size));
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (audioBuffer_ == nullptr)
    {
        return;
    }
    // After a transmission stall the phone sends the late packets at once; playing them all would keep that
    // delay for the rest of the stream (measured: 18 -> 210 ms after a 250 ms stall). With more unplayed audio
    // than the jitter of a normal stream the packet is dropped instead: one skip, then the normal latency again.
    const qint64 queued = audioOutput_->bufferSize() - audioOutput_->bytesFree();
    if (queued > audioFormat_.bytesForDuration(cMaxQueuedUs))
    {
        ++droppedPackets_;
        return;
    }
    audioBuffer_->write(reinterpret_cast<const char*>(buffer.cdata), buffer.size);
}

void QtAudioOutput::start()
{
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        streaming_ = true;
        streamStart_ = std::chrono::steady_clock::now();
        lastPacket_ = {};
    }
    emit startPlayback();
}

void QtAudioOutput::stop()
{
    this->logStreamEnd();
    emit stopPlayback();
}

void QtAudioOutput::suspend()
{
    this->logStreamEnd();
    emit suspendPlayback();
}

void QtAudioOutput::watchPacket(qint64 bytes)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(watchMutex_);
    if(!streaming_)
    {
        return;
    }
    if(lastPacket_ != std::chrono::steady_clock::time_point{})
    {
        using std::chrono::duration_cast;
        using std::chrono::milliseconds;
        const int64_t gapMs = duration_cast<milliseconds>(now - lastPacket_).count();
        const int64_t packetMs = audioFormat_.durationForBytes(bytes) / 1000;
        if(gapMs > std::max<int64_t>(cStallMs, 2 * packetMs))
        {
            ++stalls_;
            longestStallMs_ = std::max(longestStallMs_, gapMs);
            OPENAUTO_LOG(info) << "[QtAudioOutput] " << name_ << ": stall, no packet for " << gapMs
                               << " ms (one every " << packetMs << " ms), "
                               << duration_cast<milliseconds>(now - streamStart_).count() / 1000.0
                               << " s into the stream";
        }
    }
    lastPacket_ = now;
}

void QtAudioOutput::logStreamEnd()
{
    std::lock_guard<std::mutex> lock(watchMutex_);
    const uint64_t dropped = droppedPackets_.exchange(0);
    if(streaming_ && (stalls_ > 0 || dropped > 0))
    {
        OPENAUTO_LOG(info) << "[QtAudioOutput] " << name_ << ": stream ended after "
                           << std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - streamStart_).count()
                           << " s, " << stalls_ << " stalls (longest " << longestStallMs_ << " ms), " << dropped
                           << " late packets dropped";
    }
    streaming_ = false;
    stalls_ = 0;
    longestStallMs_ = 0;
}

uint32_t QtAudioOutput::getSampleSize() const
{
    return audioFormat_.bytesPerSample() * 8;
}

uint32_t QtAudioOutput::getChannelCount() const
{
    return audioFormat_.channelCount();
}

uint32_t QtAudioOutput::getSampleRate() const
{
    return audioFormat_.sampleRate();
}

void QtAudioOutput::onStartPlayback()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    if(!playbackStarted_)
    {
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if(!device.isNull() && device.id() != device_.id())
        {
            OPENAUTO_LOG(info) << "[QtAudioOutput] " << name_ << ": default output now " << device.id().toStdString();
            device_ = device;
            audioOutput_ = std::make_unique<QAudioSink>(device_, audioFormat_);
        }
        audioBuffer_ = audioOutput_->start();
        playbackStarted_ = true;
    }
    else
    {
        audioOutput_->resume();
    }
}

void QtAudioOutput::onSuspendPlayback()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    audioOutput_->suspend();
}

void QtAudioOutput::onStopPlayback()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    if(playbackStarted_)
    {
        // reset(), not stop(): stop() drains the stream and waits for that in this (GUI) thread, without a
        // timeout; after suspend() the stream is corked and never drains, OpenHU hung at the end of an Android
        // Auto connection. reset() closes it right away, what is buffered is dropped (the connection ended).
        // Both delete the device start() returned: a later write() must not use it.
        audioOutput_->reset();
        audioBuffer_ = nullptr;
        playbackStarted_ = false;
    }
}

}
}
}
}
