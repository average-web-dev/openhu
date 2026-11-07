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

// Local sources (radio) lower during Android Auto guidance and the Assistant (#93)

#include <cmath>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <f1x/openauto/autoapp/Audio/DuckPolicy.hpp>
#include <f1x/openauto/autoapp/Audio/DuckingController.hpp>
#include <f1x/openauto/autoapp/Audio/SinkInputs.hpp>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>

using namespace f1x::openauto::autoapp::audio;
using f1x::openauto::autoapp::core::EventBus;

namespace
{

// Recorded with pactl 17 (pipewire-pulse 1.4, Debian trixie), shortened, personal values replaced:
// 5926 the radio plugin's pacat, 5930 OpenHU's own Android Auto output (Qt), 5504 a native PipeWire client
// (process only in its client), 5940 a filter chain of PipeWire itself, 5950 a loopback module.
const char* cSinkInputs = R"([
{"index":5926,"driver":"PipeWire","owner_module":null,"client":"5925","sink":55,
 "sample_specification":"s16le 1ch 44100Hz","channel_map":"mono","corked":false,"mute":false,
 "volume":{"mono":{"value":65536,"value_percent":"100%","db":"0.00 dB"}},"balance":0.00,
 "properties":{"client.api":"pipewire-pulse","application.name":"OpenHU","media.name":"Radio",
  "application.process.id":"4242","application.process.binary":"pacat",
  "module-stream-restore.id":"sink-input-by-application-name:OpenHU"}},
{"index":5930,"driver":"PipeWire","owner_module":null,"client":"5929","sink":55,
 "sample_specification":"s16le 2ch 48000Hz","channel_map":"front-left,front-right","corked":false,"mute":false,
 "volume":{"front-left":{"value":65536,"value_percent":"100%","db":"0.00 dB"},
           "front-right":{"value":65536,"value_percent":"100%","db":"0.00 dB"}},
 "properties":{"application.name":"openhu","media.name":"QtmPulseStream-1000-1",
  "application.process.id":"1000","application.process.binary":"openhu"}},
{"index":5504,"driver":"PipeWire","owner_module":null,"client":"5503","sink":55,
 "sample_specification":"s16le 2ch 44100Hz","channel_map":"front-left,front-right","corked":false,"mute":false,
 "volume":{"front-left":{"value":59621,"value_percent":"91%","db":"-2.46 dB"},
           "front-right":{"value":52000,"value_percent":"79%","db":"-6.00 dB"}},
 "properties":{"media.name":"snd0","node.name":"player"}},
{"index":5940,"driver":"PipeWire","owner_module":null,"client":"34","sink":55,
 "sample_specification":"float32le 2ch 48000Hz","channel_map":"front-left,front-right","corked":false,"mute":false,
 "volume":{"front-left":{"value":65536,"value_percent":"100%","db":"0.00 dB"},
           "front-right":{"value":65536,"value_percent":"100%","db":"0.00 dB"}},
 "properties":{"media.name":"Equalizer output","node.name":"effect_output.eq"}},
{"index":5950,"driver":"PipeWire","owner_module":"23","client":"5949","sink":55,
 "sample_specification":"float32le 2ch 48000Hz","channel_map":"front-left,front-right","corked":false,"mute":false,
 "volume":{"front-left":{"value":65536,"value_percent":"100%","db":"0.00 dB"},
           "front-right":{"value":65536,"value_percent":"100%","db":"0.00 dB"}},
 "properties":{"media.name":"loopback-1234-5","application.process.id":"4300","application.process.binary":"pw-loopback"}}
])";

const char* cClients = R"([
{"index":34,"driver":"PipeWire","owner_module":"2","properties":{"application.name":"pipewire",
 "application.process.id":"3695","application.process.binary":"pipewire"}},
{"index":5503,"driver":"PipeWire","owner_module":"2","properties":{"application.name":"player",
 "application.process.id":"4444","application.process.binary":"player"}},
{"index":5925,"driver":"PipeWire","owner_module":"2","properties":{"application.name":"OpenHU",
 "application.process.id":"4242","application.process.binary":"pacat"}}
])";

// pactl replacement in front of PATH: lists come from files, volume changes go to "log"; "subscribe" reports a new
// stream every 0.2 s (a radio that starts or ends while ducked)
const char* cFakePactl = R"(#!/bin/sh
dir=$(dirname "$0")
case "$*" in
    subscribe) while true; do sleep 0.2; echo "Event 'new' on sink-input #7"; done ;;
    "-f json list sink-inputs") cat "$dir/sink-inputs.json" ;;
    "-f json list clients") echo "[]" ;;
    set-sink-input-volume*) echo "$*" >> "$dir/log" ;;
esac
exit 0
)";

QString radio(quint32 volume)
{
    return QStringLiteral(R"([{"index":7,"owner_module":null,"client":"1","channel_map":"mono",
        "volume":{"mono":{"value":%1}},
        "properties":{"application.name":"OpenHU","media.name":"Radio","application.process.id":"4242",
                      "application.process.binary":"pacat"}},
      {"index":8,"owner_module":null,"client":"2","channel_map":"mono","volume":{"mono":{"value":65536}},
        "properties":{"application.name":"openhu","application.process.id":"1000",
                      "application.process.binary":"openhu"}}])")
        .arg(volume);
}

quint32 ducked(quint32 volume, int level)
{
    return quint32(std::lround(volume * std::cbrt(level / 100.0)));
}

}

class TestDucking: public QObject
{
    Q_OBJECT

private slots:
    // --- decisions ---

    void assistantMutes()
    {
        DuckPolicy policy;
        QVERIFY(!policy.target().active);
        policy.focusRequested(FocusRequest::GainTransient);
        QCOMPARE(policy.target(), (DuckTarget{true, 0, "transient"}));  // silent from the request on
        policy.guidanceStarted();
        QCOMPARE(policy.target(), (DuckTarget{true, 0, "transient"}));
        // the Assistant listens between its answers: guidance stopped, still silent until the release
        policy.guidanceStopped();
        policy.microphoneChanged(true);
        QCOMPARE(policy.target(), (DuckTarget{true, 0, "transient"}));
        policy.microphoneChanged(false);
        QCOMPARE(policy.target(), (DuckTarget{true, 0, "transient"}));
        policy.guidanceStarted();
        policy.guidanceStopped();
        policy.focusRequested(FocusRequest::Release);
        QCOMPARE(policy.target(), DuckTarget());
    }

    void openMicrophoneMutes()
    {
        DuckPolicy policy;
        policy.microphoneChanged(true);
        QCOMPARE(policy.target(), (DuckTarget{true, 0, "microphone"}));
        policy.microphoneChanged(false);
        QCOMPARE(policy.target(), DuckTarget());
        policy.microphoneChanged(true);
        policy.sessionEnded();
        QCOMPARE(policy.target(), DuckTarget());
    }

    void navigationLowersToTheSetting()
    {
        DuckPolicy policy;
        policy.focusRequested(FocusRequest::GainTransientMayDuck);
        policy.guidanceStarted();
        QCOMPARE(policy.target(), (DuckTarget{true, 20, "may_duck"}));
        policy.setDuckLevel(35);
        QCOMPARE(policy.target().level, 35);
        policy.setDuckLevel(150);
        QCOMPARE(policy.target().level, 100);
    }

    // unknown or no focus request: lowered like a prompt, never silenced by a guess
    void guidanceWithoutRequestLowers()
    {
        DuckPolicy policy(30);
        policy.guidanceStarted();
        QCOMPARE(policy.target(), (DuckTarget{true, 30, "guidance"}));
        policy.focusRequested(FocusRequest::Gain);
        QCOMPARE(policy.target(), (DuckTarget{true, 30, "guidance"}));
        QCOMPARE(focusRequestFromProtocol(7), FocusRequest::None);
        QCOMPARE(focusRequestFromProtocol(2), FocusRequest::GainTransient);
        QCOMPARE(QString(focusRequestName(FocusRequest::GainTransientMayDuck)),
                 QString("AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK"));
    }

    // the release before the stop indication: the end of the Assistant's answer stays without radio
    void releaseDuringGuidanceWaitsForTheStop()
    {
        DuckPolicy policy;
        policy.focusRequested(FocusRequest::GainTransient);
        policy.guidanceStarted();
        policy.focusRequested(FocusRequest::Release);
        QCOMPARE(policy.target().level, 0);
        policy.guidanceStopped();
        QVERIFY(!policy.target().active);
        // the next prompt without a new request: not muted any more
        policy.guidanceStarted();
        QCOMPARE(policy.target(), (DuckTarget{true, 20, "guidance"}));
    }

    void releaseOutsideGuidance()
    {
        DuckPolicy policy;
        policy.focusRequested(FocusRequest::GainTransient);
        policy.focusRequested(FocusRequest::Release);
        policy.guidanceStarted();
        QCOMPARE(policy.target().level, 20);
    }

    // the Assistant during a navigation prompt, a second start without a stop
    void overlappingPrompts()
    {
        DuckPolicy policy;
        policy.focusRequested(FocusRequest::GainTransientMayDuck);
        policy.guidanceStarted();
        QCOMPARE(policy.target().level, 20);
        policy.focusRequested(FocusRequest::GainTransient);
        QCOMPARE(policy.target(), (DuckTarget{true, 0, "transient"}));
        policy.guidanceStarted();
        QCOMPARE(policy.target().level, 0);
        policy.focusRequested(FocusRequest::GainTransientMayDuck);
        QCOMPARE(policy.target().level, 20);
        policy.guidanceStopped();
        QVERIFY(!policy.target().active);
    }

    void disconnectWhileDucked()
    {
        DuckPolicy policy;
        policy.focusRequested(FocusRequest::GainTransient);
        policy.guidanceStarted();
        policy.sessionEnded();
        QVERIFY(!policy.target().active);
        QCOMPARE(policy.focus(), FocusRequest::None);
        // the next session starts from scratch
        policy.guidanceStarted();
        QCOMPARE(policy.target().level, 20);
    }

    // --- streams ---

    void parseRecordedStreams()
    {
        const auto inputs = parseSinkInputs(cSinkInputs, cClients);
        QCOMPARE(inputs.size(), 5);

        const SinkInput& radio = inputs[0];
        QCOMPARE(radio.index, 5926u);
        QCOMPARE(radio.volumes, QList<quint32>{65536});
        QCOMPARE(radio.pid, qint64(4242));
        QCOMPARE(radio.binary, QString("pacat"));
        QCOMPARE(radio.key(), QString("OpenHU\nRadio"));
        QVERIFY(!radio.module);

        // process from the client, channel volumes in channel map order
        const SinkInput& player = inputs[2];
        QCOMPARE(player.pid, qint64(4444));
        QCOMPARE(player.binary, QString("player"));
        QCOMPARE(player.volumes, (QList<quint32>{59621, 52000}));

        QCOMPARE(inputs[3].binary, QString("pipewire"));
        QVERIFY(inputs[4].module);

        QVERIFY(isLocalSource(inputs[0], 1000, "openhu"));
        QVERIFY(!isLocalSource(inputs[1], 1000, "openhu"));   // OpenHU's own Android Auto output
        QVERIFY(isLocalSource(inputs[2], 1000, "openhu"));
        QVERIFY(!isLocalSource(inputs[3], 1000, "openhu"));   // PipeWire's filter carries Android Auto too
        QVERIFY(!isLocalSource(inputs[4], 1000, "openhu"));   // loopback module
        // same pid, other program: another pid namespace (OpenHU in a container)
        QVERIFY(isLocalSource(inputs[1], 1000, "autoapp"));
    }

    void parseWithoutClientsOrJson()
    {
        const auto inputs = parseSinkInputs(cSinkInputs);
        QCOMPARE(inputs.size(), 5);
        QCOMPARE(inputs[2].pid, qint64(0));
        QVERIFY(isLocalSource(inputs[2], 1000, "openhu"));
        QVERIFY(parseSinkInputs("Sink Input #5926\n\tDriver: PipeWire").isEmpty());
        QVERIFY(parseSinkInputs("").isEmpty());
    }

    void loudnessIsCubicVolume()
    {
        QCOMPARE(scaledVolumes({65536, 32768}, 1.0), (QList<quint32>{65536, 32768}));
        QCOMPARE(scaledVolumes({65536}, 0.0), QList<quint32>{0});
        // 20 % loudness like Android = 58 % on the volume scale (-14 dB)
        QCOMPARE(scaledVolumes({65536}, 0.2), QList<quint32>{38326});
        QCOMPARE(scaledVolumes({65536}, 2.0), QList<quint32>{65536});
    }

    // --- controller with a fake pactl ---

    void init()
    {
        QVERIFY(dir_.isValid());
        QFile script(dir_.filePath("pactl"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(cFakePactl);
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::remove(dir_.filePath("log"));
        QFile::remove(dir_.filePath("state.json"));
        this->setStreams(radio(65536));
        level_ = 20;
        oldPath_ = qgetenv("PATH");
        qputenv("PATH", dir_.path().toLocal8Bit() + ":" + oldPath_);
    }

    void cleanup()
    {
        qputenv("PATH", oldPath_);
    }

    void fadesDownAndBack()
    {
        auto controller = this->controller();
        controller->onFocusRequest(2);
        controller->onGuidance(true);
        QCOMPARE(EventBus::instance().last("audio.duck"),
                 (QVariantMap{{"active", true}, {"level", 0}, {"reason", "transient"}}));
        QTRY_COMPARE(this->lastVolume(7), QString("0"));
        // a fade, not a jump; OpenHU's own stream untouched
        const QStringList down = this->log();
        QVERIFY2(down.size() >= 3, qPrintable(down.join(" | ")));
        QVERIFY(!down.join("\n").contains("volume 8 "));
        // the state for a crash
        QVERIFY(QFile::exists(dir_.filePath("state.json")));

        // transient focus: silent until the phone releases it (the Assistant listens between its answers)
        controller->onGuidance(false);
        QTest::qWait(200);
        QCOMPARE(this->lastVolume(7), QString("0"));
        controller->onFocusRequest(4);
        QTRY_COMPARE(this->lastVolume(7), QString("65536"));
        QCOMPARE(EventBus::instance().last("audio.duck").value("active").toBool(), false);
        QVERIFY(this->log().size() >= down.size() + 3);
        QTRY_VERIFY(!QFile::exists(dir_.filePath("state.json")));
    }

    void navigationToTheConfiguredLevel()
    {
        level_ = 35;
        auto controller = this->controller();
        controller->onFocusRequest(3);
        controller->onGuidance(true);
        QTRY_COMPARE(this->lastVolume(7), QString::number(ducked(65536, 35)));
        QCOMPARE(controller->target(), (DuckTarget{true, 35, "may_duck"}));
        // the Assistant during the prompt
        controller->onFocusRequest(2);
        QTRY_COMPARE(this->lastVolume(7), QString("0"));
        controller->onSessionEnded();
        QTRY_COMPARE(this->lastVolume(7), QString("65536"));
    }

    // relative to the stream's own volume, which comes back exactly
    void relativeToTheOwnVolume()
    {
        this->setStreams(radio(40000));
        auto controller = this->controller();
        controller->onFocusRequest(3);
        controller->onGuidance(true);
        QTRY_COMPARE(this->lastVolume(7), QString::number(ducked(40000, 20)));
        controller->onGuidance(false);
        QTRY_COMPARE(this->lastVolume(7), QString("40000"));
    }

    void staysDownBetweenPrompts()
    {
        auto controller = this->controller();
        controller->setTimings(300, 40, 40, 10);
        controller->onFocusRequest(3);
        controller->onGuidance(true);
        QTRY_COMPARE(this->lastVolume(7), QString::number(ducked(65536, 20)));
        controller->onGuidance(false);
        QTest::qWait(100);
        controller->onGuidance(true);
        QTest::qWait(400);
        QCOMPARE(this->lastVolume(7), QString::number(ducked(65536, 20)));
        QVERIFY(controller->target().active);
    }

    void restoreNowOnExit()
    {
        auto controller = this->controller();
        controller->onFocusRequest(2);
        controller->onGuidance(true);
        QTRY_COMPARE(this->lastVolume(7), QString("0"));
        controller->restoreNow();
        QCOMPARE(this->lastVolume(7), QString("65536"));
        QVERIFY(!controller->target().active);
        QVERIFY(!QFile::exists(dir_.filePath("state.json")));
    }

    // a radio that starts while the Assistant speaks is silent too
    void streamStartingWhileDucked()
    {
        this->setStreams("[]");
        auto controller = this->controller();
        controller->onFocusRequest(2);
        controller->onGuidance(true);
        QTest::qWait(100);
        QVERIFY(this->log().isEmpty());
        this->setStreams(radio(65536));
        // fake "pactl subscribe" reports it
        QTRY_COMPARE(this->lastVolume(7), QString("0"));
    }

    // a radio that ends while ducked, e.g. switched off: its next stream gets the volume back that the audio server
    // would restore lowered
    void streamEndingWhileDucked()
    {
        auto controller = this->controller();
        controller->onFocusRequest(3);
        controller->onGuidance(true);
        QTRY_COMPARE(this->lastVolume(7), QString::number(ducked(65536, 20)));
        this->setStreams("[]");
        QTest::qWait(500);  // the next list (subscribe event) sees it gone
        QVERIFY(QFile::exists(dir_.filePath("state.json")));
        controller->onGuidance(false);
        QTest::qWait(200);
        // a new radio stream with the remembered, lowered volume
        this->clearLog();
        this->setStreams(radio(ducked(65536, 20)).replace("\"index\":7", "\"index\":9"));
        QTRY_COMPARE(this->lastVolume(9), QString("65536"));
    }

    // OpenHU crashed while ducked: the radio's next stream starts with the lowered volume the audio server kept
    void restoresAfterACrash()
    {
        QFile state(dir_.filePath("state.json"));
        QVERIFY(state.open(QIODevice::WriteOnly));
        state.write(R"({"streams":[{"app":"OpenHU","media":"Radio","original":[65536]}]})");
        state.close();
        this->setStreams(radio(ducked(65536, 20)));
        auto controller = this->controller();
        QTRY_COMPARE(this->lastVolume(7), QString("65536"));
        QTRY_VERIFY(!QFile::exists(dir_.filePath("state.json")));
        QVERIFY(!controller->target().active);
    }

    // a stream at its normal volume after the restart: left alone, the entry is done
    void crashStateWithoutLoweredStream()
    {
        QFile state(dir_.filePath("state.json"));
        QVERIFY(state.open(QIODevice::WriteOnly));
        state.write(R"({"streams":[{"app":"OpenHU","media":"Radio","original":[65536]}]})");
        state.close();
        auto controller = this->controller();
        QTRY_VERIFY(!QFile::exists(dir_.filePath("state.json")));
        QVERIFY(this->log().isEmpty());
    }

    // With OPENHU_TEST_PULSE=1 and a reachable audio server (PipeWire/PulseAudio): a real pacat stream is lowered and
    // restored. Plays silence.
    void realAudioServer()
    {
        if(qgetenv("OPENHU_TEST_PULSE") != "1")
        {
            QSKIP("OPENHU_TEST_PULSE=1 for the test with the real audio server");
        }
        qputenv("PATH", oldPath_);
        QProcess pacat;
        pacat.setStandardInputFile("/dev/zero");
        pacat.start("pacat", {"--raw", "--format=s16le", "--rate=44100", "--channels=1",
                              "--client-name=OpenHUDuckTest", "--stream-name=DuckTest"});
        QVERIFY(pacat.waitForStarted());
        const auto volume = [&pacat]() -> qint64 {
            QProcess pactl;
            pactl.start("pactl", {"-f", "json", "list", "sink-inputs"});
            pactl.waitForFinished(2000);
            for(const SinkInput& input: parseSinkInputs(pactl.readAllStandardOutput()))
            {
                if(input.pid == pacat.processId())
                {
                    return input.volumes.value(0);
                }
            }
            return -1;
        };
        QTRY_VERIFY(volume() >= 0);
        const qint64 original = volume();
        DuckingController controller([]() { return 20; });
        controller.setStateFile(dir_.filePath("state.json"));
        // only the test's stream, not what else plays on this audio server
        const qint64 pid = pacat.processId();
        controller.setSourceFilter([pid](const SinkInput& input) { return input.pid == pid; });
        controller.start();
        controller.onFocusRequest(3);
        controller.onGuidance(true);
        // the audio server keeps volumes as floats: a few steps of rounding
        const qint64 expected = ducked(quint32(original), 20);
        QTRY_VERIFY2(qAbs(volume() - expected) <= 100, qPrintable(QString::number(volume())));
        controller.onGuidance(false);
        QTRY_VERIFY2(qAbs(volume() - original) <= 100, qPrintable(QString::number(volume())));
        pacat.kill();
        pacat.waitForFinished();
    }

private:
    std::unique_ptr<DuckingController> controller()
    {
        auto controller = std::make_unique<DuckingController>([this]() { return level_; });
        controller->setOwnProcess(1000, "openhu");
        controller->setStateFile(dir_.filePath("state.json"));
        controller->setTimings(30, 60, 60, 10);
        controller->start();
        return controller;
    }

    void setStreams(const QString& json)
    {
        QFile file(dir_.filePath("sink-inputs.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(json.toUtf8());
    }

    QStringList log() const
    {
        QFile file(dir_.filePath("log"));
        if(!file.open(QIODevice::ReadOnly))
        {
            return {};
        }
        return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    }

    void clearLog()
    {
        QFile::remove(dir_.filePath("log"));
    }

    // volume of the last "set-sink-input-volume <index> <volume>"
    QString lastVolume(quint32 index) const
    {
        QString result;
        for(const QString& line: this->log())
        {
            const QStringList parts = line.split(' ');
            if(parts.size() >= 3 && parts[1] == QString::number(index))
            {
                result = parts[2];
            }
        }
        return result;
    }

    QTemporaryDir dir_;
    QByteArray oldPath_;
    int level_ = 20;
};

QTEST_GUILESS_MAIN(TestDucking)
#include "test_ducking.moc"
