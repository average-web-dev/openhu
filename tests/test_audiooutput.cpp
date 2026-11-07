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

// QtAudioOutput: packets that arrive before the sink runs are played, not dropped (#127)

#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <QBuffer>
#include <QtTest>
#include <f1x/openauto/autoapp/Projection/QtAudioOutput.hpp>

using f1x::openauto::autoapp::projection::QtAudioOutput;

namespace
{

// the sink is a QBuffer per start: no sound server in the tests
class BufferAudioOutput: public QtAudioOutput
{
public:
    BufferAudioOutput()
        : QtAudioOutput(2, 16, 48000, "media")
    {
    }

    // what the newest sink got
    QByteArray played() const
    {
        return sinks.empty() ? QByteArray() : sinks.back()->data();
    }

    std::vector<std::unique_ptr<QBuffer>> sinks;

protected:
    QIODevice* startSink() override
    {
        sinks.push_back(std::make_unique<QBuffer>());
        sinks.back()->open(QIODevice::WriteOnly);
        return sinks.back().get();
    }

    qint64 queuedBytes() const override
    {
        return 0;
    }
};

// start/write/stop come from the io_service thread, the sink is started in the GUI thread (this one): while the
// function runs, the queued onStartPlayback() cannot run yet, as with a busy GUI thread
void inIoThread(const std::function<void()>& function)
{
    std::thread thread(function);
    thread.join();
}

// one media packet of the phone: 42.7 ms at 48 kHz stereo
QByteArray packet(char fill)
{
    return QByteArray(8192, fill);
}

void write(QtAudioOutput& output, const QByteArray& data)
{
    output.write(0, aasdk::common::DataConstBuffer(data.constData(), data.size()));
}

}

class TestAudioOutput: public QObject
{
    Q_OBJECT

private slots:
    void writesBeforeTheSinkRunsArePlayed()
    {
        BufferAudioOutput output;
        inIoThread([&] {
            output.start();
            write(output, packet('a'));
            write(output, packet('b'));
        });
        QVERIFY(output.sinks.empty());
        QCoreApplication::processEvents();
        QCOMPARE(output.sinks.size(), size_t(1));
        QCOMPARE(output.played(), packet('a') + packet('b'));

        inIoThread([&] { write(output, packet('c')); });
        QCOMPARE(output.played(), packet('a') + packet('b') + packet('c'));
    }

    void waitingPacketsAreLimited()
    {
        // like late packets in a running stream: more than 60 ms waiting, the next packet is dropped
        BufferAudioOutput output;
        inIoThread([&] {
            output.start();
            write(output, packet('a'));
            write(output, packet('b'));
            write(output, packet('c'));
        });
        QCoreApplication::processEvents();
        QCOMPARE(output.played(), packet('a') + packet('b'));
    }

    void writesWithoutStartAreDropped()
    {
        BufferAudioOutput output;
        inIoThread([&] { write(output, packet('a')); });
        QCoreApplication::processEvents();
        QVERIFY(output.sinks.empty());
    }

    void stopDropsTheWaitingPackets()
    {
        BufferAudioOutput output;
        inIoThread([&] {
            output.start();
            write(output, packet('a'));
            output.stop();
        });
        QCoreApplication::processEvents();
        QCOMPARE(output.played(), QByteArray());
    }

    void startRightAfterStopKeepsTheNewStream()
    {
        BufferAudioOutput output;
        inIoThread([&] { output.start(); });
        QCoreApplication::processEvents();
        QCOMPARE(output.sinks.size(), size_t(1));

        // the old sink still runs until onStopPlayback(): the new stream's packets must not go there
        inIoThread([&] {
            output.stop();
            output.start();
            write(output, packet('b'));
        });
        QCoreApplication::processEvents();
        QCOMPARE(output.sinks.size(), size_t(2));
        QCOMPARE(output.sinks[0]->data(), QByteArray());
        QCOMPARE(output.played(), packet('b'));
    }

    void resumeAfterSuspendKeepsTheOrder()
    {
        BufferAudioOutput output;
        inIoThread([&] {
            output.start();
            write(output, packet('a'));
        });
        QCoreApplication::processEvents();

        inIoThread([&] {
            output.suspend();
            output.start();
            write(output, packet('b'));
        });
        QCoreApplication::processEvents();
        QCOMPARE(output.sinks.size(), size_t(1));
        QCOMPARE(output.played(), packet('a') + packet('b'));
    }
};

QTEST_GUILESS_MAIN(TestAudioOutput)
#include "test_audiooutput.moc"
