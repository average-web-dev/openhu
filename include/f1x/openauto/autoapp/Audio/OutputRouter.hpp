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

#pragma once

#include <functional>
#include <optional>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <f1x/openauto/autoapp/Audio/SinkInputs.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace audio
{

// The Bluetooth card of a device in "pactl -f json list cards"
struct BluetoothCard
{
    QString name;           // bluez_card.E8_78_29_C0_E5_2D
    QString activeProfile;  // a2dp-sink, headset-head-unit, off (PulseAudio: a2dp_sink, ...)
    QString a2dpProfile;    // the best available A2DP playback profile, empty if the card has none

    // came up with a headset profile (or off) although it can play A2DP
    bool needsA2dp() const;
};
std::optional<BluetoothCard> findBluetoothCard(const QByteArray& cardsJson, const QString& address);

// "pactl list short sinks": index, name
struct Sink
{
    quint32 index = 0;
    QString name;
};
QList<Sink> parseShortSinks(const QByteArray& output);
// The sink of the Bluetooth device (address E8:78:29:C0:E5:2D): PipeWire bluez_output.E8:78:29:C0:E5:2D(.1) or
// bluez_output.E8_78_29_C0_E5_2D.1, PulseAudio bluez_sink.E8_78_29_C0_E5_2D.a2dp_sink; empty if there is none
QString findBluetoothSink(const QList<Sink>& sinks, const QString& address);

// Streams that go along when the output changes: applications (OpenHU's own Android Auto outputs, the radio's pacat,
// ...), not the audio server's loopbacks and filters
bool followsOutput(const SinkInput& input);

// Makes the sink of a Bluetooth device (car radio, speaker) the output and back (#112), with pactl like
// VolumeController, without blocking the GUI thread.
// Setting the default sink moves only the streams that follow the default: a stream opened with an explicit sink
// (pacat --device, a target WirePlumber remembered for the application) stays where it is. The streams that still
// play on the old default a moment after the switch, and OpenHU's own (Android Auto) wherever they play, are moved
// with "pactl move-sink-input", while they play; streams on other outputs, chosen on purpose, stay. restore() moves
// them back.
class OutputRouter: public QObject
{
    Q_OBJECT

public:
    explicit OutputRouter(QObject* parent = nullptr);

    // One look for the device's sink (the caller repeats it while the sink is missing, it appears a moment after the
    // Bluetooth connection): a card that came up with a headset profile is switched to A2DP first (its A2DP sink
    // appears then); a card without an A2DP profile yet (connected hands-free only) is waited for a while (15
    // looks) before its headset sink is taken; else the sink becomes the default output and the streams move to
    // it -> routed().
    // Ignored while a switch is running.
    void route(const QString& address);
    // Notes the default output as the one to come back to, unless it is Bluetooth (the audio server may make a
    // new Bluetooth sink the default by itself before route() looks); while not routed
    void rememberDefault();
    // The output before back: default and streams (after a running route()) -> restored()
    void restore();
    // The same, blocking (shutdown: no event loop any more)
    void restoreNow();

    // the sink routed to, empty if none
    QString sink() const;
    bool busy() const;

    // Tests: wait before moving the streams that did not follow the default by themselves
    void setFollowDelay(int ms);
    // Tests: the process that counts as OpenHU (its streams always move along)
    void setOwnProcess(qint64 pid, const QString& binary);
    // Tests on a real audio server: only these streams
    void setStreamFilter(std::function<bool(const SinkInput&)> filter);

signals:
    void routed(const QString& sink);
    void restored();

private:
    using Outputs = QList<QByteArray>;
    // the commands run at once, done() gets their standard outputs when all ended
    void pactl(const QList<QStringList>& commands, std::function<void(const Outputs&)> done);
    void onListed(const QString& address, const Outputs& outputs);
    void switchTo(const QString& sink, const QList<Sink>& sinks);
    void doRestore();
    // moves the streams on sink "from" (and those in "also") to "to"; done() when the moves ended
    void moveStreams(const QList<Sink>& sinks, const QString& from, const QSet<quint32>& also, const QString& to,
                     bool remember, std::function<void()> done);
    QList<quint32> streamsToMove(const QList<SinkInput>& inputs, const QList<Sink>& sinks, const QString& from,
                                 const QSet<quint32>& also, const QString& to) const;
    void finish();

    QString sink_;
    QString previous_;
    // streams moved to sink_ (back on restore, also when the Bluetooth sink is gone already)
    QSet<quint32> moved_;
    bool busy_ = false;
    bool restorePending_ = false;
    int profileSwitches_ = 0;
    int headsetWaits_ = 0;
    // what route() saw last (log)
    QString seen_;
    int followDelayMs_ = 500;
    // restoreNow() ends what still runs
    int generation_ = 0;
    qint64 ownPid_ = 0;
    QString ownBinary_;
    std::function<bool(const SinkInput&)> streamFilter_;
};

}
}
}
}
