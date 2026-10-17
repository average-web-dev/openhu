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

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioFormat>
#include <f1x/openauto/autoapp/Projection/IAudioOutput.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace projection
{

class QtAudioOutput: public QObject, public IAudioOutput
{
    Q_OBJECT

public:
    // name: the channel in the log (media, guidance, system)
    QtAudioOutput(uint32_t channelCount, uint32_t sampleSize, uint32_t sampleRate, std::string name = "audio");
    bool open() override;
    void write(aasdk::messenger::Timestamp::ValueType, const aasdk::common::DataConstBuffer& buffer) override;
    void start() override;
    void stop() override;
    void suspend() override;
    uint32_t getSampleSize() const override;
    uint32_t getChannelCount() const override;
    uint32_t getSampleRate() const override;

signals:
    void startPlayback();
    void suspendPlayback();
    void stopPlayback();

protected slots:
    void createAudioOutput();
    void onStartPlayback();
    void onSuspendPlayback();
    void onStopPlayback();

private:
    QAudioFormat audioFormat_;
    // audioOutput_ and audioBuffer_: write() in the io_service thread, start/suspend/stop in the GUI thread
    std::mutex outputMutex_;
    QIODevice * audioBuffer_;
    std::unique_ptr<QAudioSink> audioOutput_;
    // the device audioOutput_ plays on (the default when it was created)
    QAudioDevice device_;
    bool playbackStarted_;
    std::atomic<uint64_t> droppedPackets_{0};

    // stall watcher: packets that come much later than the stream's rhythm (while the phone did not stop it)
    void watchPacket(qint64 bytes);
    void logStreamEnd();
    const std::string name_;
    std::mutex watchMutex_;
    bool streaming_ = false;
    std::chrono::steady_clock::time_point streamStart_;
    std::chrono::steady_clock::time_point lastPacket_;
    uint64_t stalls_ = 0;
    int64_t longestStallMs_ = 0;
};

}
}
}
}
