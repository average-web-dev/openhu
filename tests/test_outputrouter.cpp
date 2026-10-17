/*
*  This file is part of openauto project.
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

// The Bluetooth car radio/speaker becomes the output: A2DP profile, default sink and the playing streams (#112)

#include <QAudioSink>
#include <QCoreApplication>
#include <QFile>
#include <QMediaDevices>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <f1x/openauto/autoapp/Audio/OutputRouter.hpp>
#include <f1x/openauto/autoapp/Audio/SinkInputs.hpp>

using namespace f1x::openauto::autoapp::audio;

namespace
{

const QString cAddress = QStringLiteral("E8:78:29:C0:E5:2D");
const QString cSpeakers = QStringLiteral("alsa_output.pci-0000_00_1f.3.analog-stereo");
const QString cHdmi = QStringLiteral("alsa_output.pci-0000_00_1f.3.hdmi-stereo");
const QString cRadio = QStringLiteral("bluez_output.E8:78:29:C0:E5:2D.1");
const QString cCard = QStringLiteral("bluez_card.E8_78_29_C0_E5_2D");

// "pactl -f json list cards" of PipeWire 1.4 (pipewire-pulse), shortened
QString cards(const QString& activeProfile)
{
    return QStringLiteral(R"json([
{"index":40,"name":"alsa_card.pci-0000_00_1f.3","driver":"PipeWire","active_profile":"output:analog-stereo",
 "profiles":{"off":{"description":"Off","sinks":0,"sources":0,"priority":0,"available":true}}},
{"index":77,"name":"bluez_card.E8_78_29_C0_E5_2D","driver":"PipeWire","owner_module":null,
 "properties":{"device.description":"Speaker","api.bluez5.address":"E8:78:29:C0:E5:2D"},
 "profiles":{
  "off":{"description":"Off","sinks":0,"sources":0,"priority":0,"available":true},
  "a2dp-sink-sbc":{"description":"High Fidelity Playback (A2DP Sink, codec SBC)","sinks":1,"sources":0,"priority":18,"available":true},
  "a2dp-sink":{"description":"High Fidelity Playback (A2DP Sink, codec AAC)","sinks":1,"sources":0,"priority":19,"available":true},
  "a2dp-sink-sbc_xq":{"description":"High Fidelity Playback (A2DP Sink, codec SBC-XQ)","sinks":1,"sources":0,"priority":17,"available":false},
  "headset-head-unit":{"description":"Headset Head Unit (HSP/HFP)","sinks":1,"sources":1,"priority":1,"available":true}},
 "active_profile":"%1"}
])json")
        .arg(activeProfile);
}

QString sinks(bool radio)
{
    QString result = "55\t" + cSpeakers + "\tPipeWire\ts32le 2ch 48000Hz\tRUNNING\n" + "70\t" + cHdmi
                     + "\tPipeWire\ts32le 2ch 48000Hz\tSUSPENDED\n";
    if(radio)
    {
        result += "80\t" + cRadio + "\tPipeWire\ts16le 2ch 48000Hz\tIDLE\n";
    }
    return result;
}

// OpenHU's Android Auto media and guidance outputs (Qt, pid 1000) and the radio plugin's pacat on the speakers, a
// video player on HDMI (chosen there on purpose), a loopback module and a filter chain of PipeWire on the speakers
QString streams(quint32 qtSink, quint32 pacatSink)
{
    return QStringLiteral(R"([
{"index":11,"owner_module":null,"client":"5","sink":%1,"channel_map":"front-left,front-right",
 "volume":{"front-left":{"value":65536},"front-right":{"value":65536}},
 "properties":{"application.name":"openhu","media.name":"QtmPulseStream-1000-1","application.process.id":"1000",
  "application.process.binary":"openhu"}},
{"index":12,"owner_module":null,"client":"5","sink":%1,"channel_map":"mono","volume":{"mono":{"value":65536}},
 "properties":{"application.name":"openhu","media.name":"QtmPulseStream-1000-2","application.process.id":"1000",
  "application.process.binary":"openhu"}},
{"index":13,"owner_module":null,"client":"6","sink":%2,"channel_map":"mono","volume":{"mono":{"value":65536}},
 "properties":{"application.name":"OpenHU","media.name":"Radio","application.process.id":"4242",
  "application.process.binary":"pacat"}},
{"index":14,"owner_module":null,"client":"7","sink":70,"channel_map":"mono","volume":{"mono":{"value":65536}},
 "properties":{"application.name":"mpv","application.process.id":"4300","application.process.binary":"mpv"}},
{"index":15,"owner_module":"23","client":"8","sink":55,"channel_map":"mono","volume":{"mono":{"value":65536}},
 "properties":{"media.name":"loopback-1234-5","application.process.binary":"pw-loopback"}},
{"index":16,"owner_module":null,"client":"34","sink":55,"channel_map":"mono","volume":{"mono":{"value":65536}},
 "properties":{"media.name":"Equalizer output","node.name":"effect_output.eq"}}
])")
        .arg(qtSink)
        .arg(pacatSink);
}

const char* cClients = R"([
{"index":34,"owner_module":"2","properties":{"application.name":"pipewire","application.process.id":"3695",
 "application.process.binary":"pipewire"}}
])";

// pactl replacement in front of PATH: state in files, changes go to "log"
const char* cFakePactl = R"(#!/bin/sh
dir=$(dirname "$0")
case "$*" in
    "-f json list cards") cat "$dir/cards.json" ;;
    "-f json list sink-inputs") cat "$dir/sink-inputs.json" ;;
    "-f json list clients") cat "$dir/clients.json" ;;
    "list short sinks") cat "$dir/sinks" ;;
    get-default-sink) echo "$*" >> "$dir/calls"; cat "$dir/default" ;;
    set-default-sink*) echo "$*" >> "$dir/log"; echo "$2" > "$dir/default" ;;
    set-card-profile*|move-sink-input*) echo "$*" >> "$dir/log" ;;
esac
exit 0
)";

// endless silence for a QAudioSink in pull mode
class Silence: public QIODevice
{
public:
    Silence() { this->open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }

protected:
    qint64 readData(char* data, qint64 size) override
    {
        std::fill(data, data + size, 0);
        return size;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
};

}

class TestOutputRouter: public QObject
{
    Q_OBJECT

private slots:
    // --- what the audio server reports ---

    void headsetProfileNeedsA2dp()
    {
        const auto card = findBluetoothCard(cards("headset-head-unit").toUtf8(), cAddress);
        QVERIFY(card);
        QCOMPARE(card->name, cCard);
        QCOMPARE(card->a2dpProfile, QString("a2dp-sink"));  // highest priority among the available ones
        QVERIFY(card->needsA2dp());
        QVERIFY(findBluetoothCard(cards("off").toUtf8(), cAddress)->needsA2dp());
        QVERIFY(!findBluetoothCard(cards("a2dp-sink-sbc").toUtf8(), cAddress)->needsA2dp());
        // lower case address, other device, no JSON (pactl < 16)
        QVERIFY(findBluetoothCard(cards("a2dp-sink").toUtf8(), cAddress.toLower()));
        QVERIFY(!findBluetoothCard(cards("a2dp-sink").toUtf8(), "11:22:33:44:55:66"));
        QVERIFY(!findBluetoothCard("No valid command specified.", cAddress));
    }

    void headsetOnlyDeviceKeepsItsProfile()
    {
        const QByteArray json = R"([{"name":"bluez_card.E8_78_29_C0_E5_2D","active_profile":"headset_head_unit",
            "profiles":{"headset_head_unit":{"sinks":1,"sources":1,"priority":30,"available":true},
                        "a2dp_sink":{"sinks":1,"sources":0,"priority":40,"available":false}}}])";
        const auto card = findBluetoothCard(json, cAddress);
        QVERIFY(card);
        QVERIFY(card->a2dpProfile.isEmpty());
        QVERIFY(!card->needsA2dp());
    }

    void bluetoothSinkNames()
    {
        const QList<Sink> list = parseShortSinks(sinks(true).toUtf8());
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[2].index, 80u);
        QCOMPARE(findBluetoothSink(list, cAddress), cRadio);
        QCOMPARE(findBluetoothSink(list, "11:22:33:44:55:66"), QString());
        QCOMPARE(findBluetoothSink(parseShortSinks(sinks(false).toUtf8()), cAddress), QString());
        // older PipeWire, PulseAudio
        QCOMPARE(findBluetoothSink(parseShortSinks("3\tbluez_output.E8_78_29_C0_E5_2D.1\tPipeWire\n"), cAddress),
                 QString("bluez_output.E8_78_29_C0_E5_2D.1"));
        QCOMPARE(findBluetoothSink(parseShortSinks("4\tbluez_sink.E8_78_29_C0_E5_2D.a2dp_sink\tmodule-bluez5-device.c\n"),
                                   cAddress.toLower()),
                 QString("bluez_sink.E8_78_29_C0_E5_2D.a2dp_sink"));
    }

    void streamsThatFollow()
    {
        const QList<SinkInput> inputs = parseSinkInputs(streams(55, 55).toUtf8(), cClients);
        QCOMPARE(inputs.size(), 6);
        QCOMPARE(inputs[0].sink, 55u);
        QVERIFY(followsOutput(inputs[0]));   // OpenHU's Android Auto output
        QVERIFY(followsOutput(inputs[2]));   // the radio's pacat
        QVERIFY(followsOutput(inputs[3]));   // another application
        QVERIFY(!followsOutput(inputs[4]));  // loopback module
        QVERIFY(!followsOutput(inputs[5]));  // PipeWire's filter chain
    }

    // --- the router with a fake pactl ---

    void init()
    {
        QVERIFY(dir_.isValid());
        QFile script(dir_.filePath("pactl"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(cFakePactl);
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::remove(dir_.filePath("log"));
        QFile::remove(dir_.filePath("calls"));
        this->write("cards.json", cards("a2dp-sink"));
        this->write("sinks", sinks(true));
        this->write("default", cSpeakers + "\n");
        this->write("sink-inputs.json", streams(55, 55));
        this->write("clients.json", cClients);
        oldPath_ = qgetenv("PATH");
        qputenv("PATH", dir_.path().toLocal8Bit() + ":" + oldPath_);
    }

    void cleanup()
    {
        qputenv("PATH", oldPath_);
    }

    // The case of the report: Android Auto plays while the speaker connects; its streams move as they play
    void playingStreamsMoveAndBack()
    {
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        QCOMPARE(routed[0][0].toString(), cRadio);
        QCOMPARE(router->sink(), cRadio);
        QVERIFY(!router->busy());
        QCOMPARE(this->sortedLog(), sorted(QStringList{"set-default-sink " + cRadio, "move-sink-input 11 " + cRadio,
                                           "move-sink-input 12 " + cRadio, "move-sink-input 13 " + cRadio}));

        // back: the moved streams and what started on the radio's sink since
        this->clearLog();
        this->write("sink-inputs.json", streams(80, 80));
        QSignalSpy restored(router.get(), &OutputRouter::restored);
        router->restore();
        QTRY_COMPARE(restored.size(), 1);
        QVERIFY(router->sink().isEmpty());
        QCOMPARE(this->sortedLog(), sorted(QStringList{"set-default-sink " + cSpeakers, "move-sink-input 11 " + cSpeakers,
                                           "move-sink-input 12 " + cSpeakers, "move-sink-input 13 " + cSpeakers}));
    }

    void streamsThatFollowTheDefaultAreLeftAlone()
    {
        // the pacat moved with the default by itself already
        this->write("sink-inputs.json", streams(55, 80));
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        QVERIFY(!this->log().contains("move-sink-input 13 " + cRadio));
        QVERIFY(this->log().contains("move-sink-input 11 " + cRadio));
    }

    void headsetProfileSwitchedFirst()
    {
        this->write("cards.json", cards("headset-head-unit"));
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_VERIFY(!router->busy());
        QCOMPARE(this->log(), QStringList{"set-card-profile " + cCard + " a2dp-sink"});
        QCOMPARE(routed.size(), 0);

        // the A2DP sink is there on the next look
        this->write("cards.json", cards("a2dp-sink"));
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        QVERIFY(this->log().contains("set-default-sink " + cRadio));
    }

    // a car radio that connected hands-free only (A2DP not offered yet): its headset sink only after a while
    void handsFreeOnlyWaitsForA2dp()
    {
        QString json = cards("headset-head-unit");
        json.replace(R"("sinks":1,"sources":0,"priority":18,"available":true)", R"("sinks":1,"sources":0,"priority":18,"available":false)");
        json.replace(R"("sinks":1,"sources":0,"priority":19,"available":true)", R"("sinks":1,"sources":0,"priority":19,"available":false)");
        this->write("cards.json", json);
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        for(int i = 0; i < 15; ++i)
        {
            router->route(cAddress);
            QTRY_VERIFY(!router->busy());
        }
        QCOMPARE(routed.size(), 0);
        QVERIFY(this->log().isEmpty());  // no profile to switch to
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
    }

    void cardThatRefusesA2dpPlaysAnyway()
    {
        this->write("cards.json", cards("headset-head-unit"));
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        for(int i = 0; i < 4 && routed.isEmpty(); ++i)
        {
            router->route(cAddress);
            QTRY_VERIFY(!router->busy());
        }
        QCOMPARE(routed.size(), 1);
        QCOMPARE(this->log().count("set-card-profile " + cCard + " a2dp-sink"), 3);
    }

    void noSinkYet()
    {
        this->write("sinks", sinks(false));
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_VERIFY(!router->busy());
        QCOMPARE(routed.size(), 0);
        QVERIFY(this->log().isEmpty());
        QVERIFY(router->sink().isEmpty());
        // nothing to restore
        QSignalSpy restored(router.get(), &OutputRouter::restored);
        router->restore();
        QVERIFY(!router->busy());
        QCOMPARE(restored.size(), 0);
    }

    // WirePlumber made the new Bluetooth sink the default before OpenHU looked: the output before is not known, but
    // OpenHU's own streams move all the same
    void audioServerSwitchedItself()
    {
        this->write("default", cRadio + "\n");
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        QCOMPARE(this->sortedLog(), sorted(QStringList{"set-default-sink " + cRadio, "move-sink-input 11 " + cRadio,
                                           "move-sink-input 12 " + cRadio}));
    }

    // ... and back to an output that is not Bluetooth, with nothing known of the one before
    void unknownOutputBeforeComesBackToANonBluetoothOne()
    {
        this->write("default", cRadio + "\n");
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        this->clearLog();
        this->write("sink-inputs.json", streams(80, 80));
        QSignalSpy restored(router.get(), &OutputRouter::restored);
        router->restore();
        QTRY_COMPARE(restored.size(), 1);
        QCOMPARE(this->defaultSink(), cSpeakers);
        QVERIFY(this->log().contains("move-sink-input 11 " + cSpeakers));
    }

    // the output noted before the radio connected (CarAudio: when it starts), the audio server switches by itself
    void rememberedOutputComesBack()
    {
        this->write("default", cHdmi + "\n");
        auto router = this->router();
        router->rememberDefault();
        QTRY_COMPARE(this->pactlCalls("get-default-sink"), 1);
        QTest::qWait(100);
        this->write("default", cRadio + "\n");
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        QSignalSpy restored(router.get(), &OutputRouter::restored);
        router->restore();
        QTRY_COMPARE(restored.size(), 1);
        QCOMPARE(this->defaultSink(), cHdmi);
    }

    void restoreDuringRouteWaits()
    {
        auto router = this->router();
        router->setFollowDelay(100);
        QSignalSpy restored(router.get(), &OutputRouter::restored);
        router->route(cAddress);
        router->restore();
        QTRY_COMPARE(restored.size(), 1);
        QVERIFY(!router->busy());
        QCOMPARE(this->defaultSink(), cSpeakers);
    }

    void radioGoneDefaultBack()
    {
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        // the radio switched off: its sink is gone, the audio server fell back to HDMI and moved the streams there
        this->clearLog();
        this->write("sinks", sinks(false));
        this->write("default", cHdmi + "\n");
        this->write("sink-inputs.json", streams(70, 70));
        QSignalSpy restored(router.get(), &OutputRouter::restored);
        router->restore();
        QTRY_COMPARE(restored.size(), 1);
        // the default and the moved streams back to the output before; the HDMI player stays
        QCOMPARE(this->sortedLog(), sorted(QStringList{"set-default-sink " + cSpeakers, "move-sink-input 11 " + cSpeakers,
                                           "move-sink-input 12 " + cSpeakers, "move-sink-input 13 " + cSpeakers}));
    }

    void userChoseAnotherOutputMeanwhile()
    {
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        this->clearLog();
        this->write("default", cHdmi + "\n");
        this->write("sink-inputs.json", streams(70, 70));
        QSignalSpy restored(router.get(), &OutputRouter::restored);
        router->restore();
        QTRY_COMPARE(restored.size(), 1);
        // the streams stay where the user chose, the default too
        QCOMPARE(this->defaultSink(), cHdmi);
        QVERIFY(this->log().isEmpty());
    }

    void restoreNowBlocks()
    {
        auto router = this->router();
        QSignalSpy routed(router.get(), &OutputRouter::routed);
        router->route(cAddress);
        QTRY_COMPARE(routed.size(), 1);
        this->clearLog();
        this->write("sink-inputs.json", streams(80, 80));
        router->restoreNow();
        QCOMPARE(this->sortedLog(), sorted(QStringList{"set-default-sink " + cSpeakers, "move-sink-input 11 " + cSpeakers,
                                           "move-sink-input 12 " + cSpeakers, "move-sink-input 13 " + cSpeakers}));
        QVERIFY(router->sink().isEmpty());
    }

    // With OPENHU_TEST_PULSE_ROUTING=1: on a real PipeWire. It CHANGES THE DEFAULT OUTPUT for a moment: only for a
    // private audio server (a container with its own PipeWire), not the desktop's. Two null sinks stand in for the
    // speakers and the Bluetooth speaker; a QAudioSink (like Android Auto's outputs) and two pacat play silence.
    void realAudioServer()
    {
        if(qgetenv("OPENHU_TEST_PULSE_ROUTING") != "1")
        {
            QSKIP("OPENHU_TEST_PULSE_ROUTING=1 with a private audio server");
        }
        qputenv("PATH", oldPath_);
        const QString speakers = "openhu_test_speakers";
        const QString bluetooth = "bluez_output.00_11_22_33_44_55.1";
        const auto pactl = [](const QStringList& arguments) {
            QProcess process;
            process.start("pactl", arguments);
            process.waitForFinished(3000);
            return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        };
        const QString module1 = pactl({"load-module", "module-null-sink", "sink_name=" + speakers});
        const QString module2 = pactl({"load-module", "module-null-sink", "sink_name=" + bluetooth});
        QVERIFY(!module1.isEmpty() && !module2.isEmpty());
        const QString defaultBefore = pactl({"get-default-sink"});
        pactl({"set-default-sink", speakers});
        QTRY_COMPARE(QMediaDevices::defaultAudioOutput().id(), speakers.toUtf8());

        // where a stream plays: name of its sink
        const auto sinkOf = [&pactl](const std::function<bool(const SinkInput&)>& which) {
            QProcess process;
            process.start("pactl", {"-f", "json", "list", "sink-inputs"});
            process.waitForFinished(3000);
            const QByteArray inputs = process.readAllStandardOutput();
            process.start("pactl", {"-f", "json", "list", "clients"});
            process.waitForFinished(3000);
            const QList<Sink> sinks = parseShortSinks(pactl({"list", "short", "sinks"}).toUtf8());
            for(const SinkInput& input: parseSinkInputs(inputs, process.readAllStandardOutput()))
            {
                if(which(input))
                {
                    for(const Sink& sink: sinks)
                    {
                        if(sink.index == input.sink) return sink.name;
                    }
                }
            }
            return QString();
        };

        QAudioFormat format;
        format.setChannelCount(2);
        format.setSampleRate(48000);
        format.setSampleFormat(QAudioFormat::Int16);
        Silence silence;
        QAudioSink qt(QMediaDevices::defaultAudioOutput(), format);
        qt.start(&silence);
        const qint64 ownPid = QCoreApplication::applicationPid();
        const auto isQt = [ownPid](const SinkInput& input) { return input.pid == ownPid; };

        QProcess follower;  // like the radio plugin: no --device
        follower.setStandardInputFile("/dev/zero");
        follower.start("pacat", {"--raw", "--client-name=OpenHURouteTest", "--stream-name=Follower"});
        QProcess pinned;  // another application that chose the speakers explicitly
        pinned.setStandardInputFile("/dev/zero");
        pinned.start("pacat", {"--raw", "--device=" + speakers, "--client-name=OpenHURouteTest", "--stream-name=Pinned"});
        QVERIFY(follower.waitForStarted() && pinned.waitForStarted());
        const qint64 followerPid = follower.processId();
        const qint64 pinnedPid = pinned.processId();
        const auto isFollower = [followerPid](const SinkInput& input) { return input.pid == followerPid; };
        const auto isPinned = [pinnedPid](const SinkInput& input) { return input.pid == pinnedPid; };
        QTRY_COMPARE(sinkOf(isQt), speakers);
        QTRY_COMPARE(sinkOf(isFollower), speakers);
        QTRY_COMPARE(sinkOf(isPinned), speakers);

        // Whether a stream opened on an explicit sink (Qt's, pacat --device) follows a new default differs (seen both
        // with PipeWire 1.4.2 / WirePlumber 0.5.8): not checked, the router moves them either way.

        OutputRouter router;
        router.setStreamFilter([&](const SinkInput& input) { return isQt(input) || isFollower(input) || isPinned(input); });
        QSignalSpy routed(&router, &OutputRouter::routed);
        router.route("00:11:22:33:44:55");
        QTRY_COMPARE(routed.size(), 1);
        QCOMPARE(pactl({"get-default-sink"}), bluetooth);
        QTRY_COMPARE(sinkOf(isQt), bluetooth);
        QTRY_COMPARE(sinkOf(isFollower), bluetooth);
        QTRY_COMPARE(sinkOf(isPinned), bluetooth);
        // a QtAudioOutput opened now plays on the radio (onStartPlayback compares this device)
        QTRY_COMPARE(QMediaDevices::defaultAudioOutput().id(), bluetooth.toUtf8());

        QSignalSpy restored(&router, &OutputRouter::restored);
        router.restore();
        QTRY_COMPARE(restored.size(), 1);
        QCOMPARE(pactl({"get-default-sink"}), speakers);
        QTRY_COMPARE(sinkOf(isQt), speakers);
        QTRY_COMPARE(sinkOf(isFollower), speakers);
        QTRY_COMPARE(sinkOf(isPinned), speakers);

        qt.stop();
        follower.kill();
        pinned.kill();
        follower.waitForFinished();
        pinned.waitForFinished();
        if(!defaultBefore.isEmpty() && defaultBefore != speakers && defaultBefore != bluetooth)
        {
            pactl({"set-default-sink", defaultBefore});
        }
        pactl({"unload-module", module2});
        pactl({"unload-module", module1});
    }

private:
    std::unique_ptr<OutputRouter> router()
    {
        auto router = std::make_unique<OutputRouter>();
        router->setOwnProcess(1000, "openhu");
        router->setFollowDelay(10);
        return router;
    }

    void write(const QString& name, const QString& content)
    {
        QFile file(dir_.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(content.toUtf8());
    }

    static QStringList sorted(QStringList list)
    {
        list.sort();
        return list;
    }

    // the moves run at once, their order is not fixed
    QStringList sortedLog() const
    {
        return sorted(this->log());
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

    int pactlCalls(const QString& command) const
    {
        QFile file(dir_.filePath("calls"));
        if(!file.open(QIODevice::ReadOnly))
        {
            return 0;
        }
        return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts).count(command);
    }

    QString defaultSink() const
    {
        QFile file(dir_.filePath("default"));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
    }

    QTemporaryDir dir_;
    QByteArray oldPath_;
};

QTEST_GUILESS_MAIN(TestOutputRouter)
#include "test_outputrouter.moc"
