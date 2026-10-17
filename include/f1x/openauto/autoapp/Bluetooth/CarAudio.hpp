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

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <f1x/openauto/autoapp/Audio/OutputRouter.hpp>
#include <f1x/openauto/autoapp/Bluetooth/CallButtons.hpp>
#include <f1x/openauto/autoapp/Bluetooth/MediaPlayer.hpp>
#include <f1x/openauto/autoapp/Configuration/IConfiguration.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace audio
{
class VolumeController;
}
namespace input
{
class ActionDispatcher;
}

namespace bt
{

class BluetoothManager;

// Sound to a Bluetooth car radio (QML: "carAudio"): OpenHU connects the radio (A2DP, PipeWire/PulseAudio make
// it an output) and makes it the default output; the radio's buttons arrive through the media player (AVRCP):
// play/pause/next/previous for Android Auto, during a call see CallButtons (answer, hang up).
// Title and artist (or the caller) are shown on the radio. The radio's volume knob changes the output volume.
// Publishes "caraudio.state".
class CarAudio: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)

public:
    CarAudio(configuration::IConfiguration::Pointer configuration, BluetoothManager& bluetooth,
             input::ActionDispatcher& actions, audio::VolumeController& volume, QObject* parent = nullptr);
    ~CarAudio() override;

    QVariantMap state() const;

    // Starts or stops according to the configuration
    Q_INVOKABLE void apply();
    // Shutdown: the output before comes back at once (blocking); the radio stays connected
    void stop();
    // Use this radio (pairs it if needed) and switch car audio on
    Q_INVOKABLE void selectDevice(const QString& address);
    // Unpair the radio and switch car audio off (the only place that drops its connection)
    Q_INVOKABLE void forgetDevice();

signals:
    void changed();

private:
    // blocking: restore the output at once (shutdown), else in the background
    void halt(bool blocking);
    void onDevicesChanged();
    void onConnected();
    void onDisconnected(bool blocking = false);
    // sink of the radio in PipeWire/PulseAudio, appears a moment after the Bluetooth connection
    void findSink();
    // BlueZ ConnectProfile with A2DP (the radio as audio sink)
    void requestA2dp();
    void onCommand(const QString& name);
    void updatePlayer();
    void publish();

    configuration::IConfiguration::Pointer configuration_;
    BluetoothManager& bluetooth_;
    input::ActionDispatcher& actions_;
    audio::VolumeController& volume_;
    MediaPlayer player_;
    // the radio's sink as the output, the streams moved to it (#112)
    audio::OutputRouter router_;
    bool running_ = false;
    bool connected_ = false;
    QString sink_;
    QString error_;
    int sinkAttempts_ = 0;
    QTimer sinkTimer_;
    QTimer reconnectTimer_;
    QString callState_ = "idle";
    CallButtons callButtons_;
    QElapsedTimer clock_;
    QVariantMap call_;
    QVariantMap metadata_;
    QVariantMap mediaStatus_;
};

}
}
}
}
