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


#include <f1x/openauto/autoapp/Audio/VolumeController.hpp>
#include <f1x/openauto/autoapp/Bluetooth/BluetoothManager.hpp>
#include <f1x/openauto/autoapp/Bluetooth/CarAudio.hpp>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
#include <f1x/openauto/autoapp/Input/ActionDispatcher.hpp>
#include <f1x/openauto/autoapp/Wireless/Bluez.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x::openauto::autoapp::bt
{

CarAudio::CarAudio(configuration::IConfiguration::Pointer configuration, BluetoothManager& bluetooth,
                   input::ActionDispatcher& actions, audio::VolumeController& volume, QObject* parent)
    : QObject(parent)
    , configuration_(std::move(configuration))
    , bluetooth_(bluetooth)
    , actions_(actions)
    , volume_(volume)
{
    connect(&bluetooth_, &BluetoothManager::devicesChanged, this, &CarAudio::onDevicesChanged);
    connect(&bluetooth_, &BluetoothManager::devicePaired, this, [this](const QString& address) {
        if(running_ && address.compare(QString::fromStdString(configuration_->getCarAudioSettings().device),
                                       Qt::CaseInsensitive) == 0)
        {
            bluetooth_.connectDevice(address);
        }
    });
    connect(&player_, &MediaPlayer::command, this, &CarAudio::onCommand);
    clock_.start();
    connect(&router_, &audio::OutputRouter::routed, this, [this](const QString& sink) {
        if(!connected_)
        {
            return;  // disconnected meanwhile: restore() follows
        }
        sinkTimer_.stop();
        sinkTimer_.setInterval(1000);
        sink_ = sink;
        OPENAUTO_LOG(info) << "[CarAudio] output " << sink_.toStdString();
        volume_.refresh();
        error_.clear();
        this->publish();
    });
    connect(&router_, &audio::OutputRouter::restored, this, [this]() { volume_.refresh(); });

    sinkTimer_.setInterval(1000);
    connect(&sinkTimer_, &QTimer::timeout, this, &CarAudio::findSink);

    // the radio is switched on with the ignition: keep trying while it is off
    reconnectTimer_.setInterval(20000);
    connect(&reconnectTimer_, &QTimer::timeout, this, [this]() {
        const auto settings = configuration_->getCarAudioSettings();
        if(running_ && !connected_ && !settings.device.empty())
        {
            router_.rememberDefault();
            bluetooth_.connectDevice(QString::fromStdString(settings.device));
        }
    });

    connect(&core::EventBus::instance(), &core::EventBus::event, this, [this](const QString& topic, const QVariantMap& data) {
        if(topic == "media.metadata")
        {
            metadata_ = data;
        }
        else if(topic == "media.status")
        {
            mediaStatus_ = data;
        }
        else if(topic == "phone.status")
        {
            callState_ = data.value("callState").toString();
            callButtons_.setCallState(callState_, clock_.elapsed());
            call_ = data.value("calls").toList().value(0).toMap();
        }
        else
        {
            return;
        }
        this->updatePlayer();
    });
}

CarAudio::~CarAudio()
{
    this->stop();
}

void CarAudio::apply()
{
    const auto settings = configuration_->getCarAudioSettings();
    // no adapter (removed, or not there at all): off until one is plugged in (presenceChanged)
    if(!settings.enabled || settings.device.empty() || !bluetooth_.present())
    {
        this->halt(false);
        this->publish();
        return;
    }
    if(!bluetooth_.ensureAdapter())
    {
        error_ = bluetooth_.error();
        this->publish();
        return;
    }

    running_ = true;
    error_.clear();
    if(!connected_)
    {
        router_.rememberDefault();
    }
    const QString registerError = player_.registerAt(bluetooth_.adapterPath());
    if(!registerError.isEmpty())
    {
        OPENAUTO_LOG(warning) << "[CarAudio] media player: " << registerError.toStdString();
    }
    this->updatePlayer();

    const QString address = QString::fromStdString(settings.device);
    if(bluetooth_.device(address).isEmpty())
    {
        bluetooth_.pairWith(address, QString::fromStdString(settings.pin));
    }
    else
    {
        bluetooth_.connectDevice(address);
    }
    reconnectTimer_.start();
    this->onDevicesChanged();
    this->publish();
}

void CarAudio::stop()
{
    this->halt(true);
}

void CarAudio::halt(bool blocking)
{
    if(!running_)
    {
        return;
    }
    running_ = false;
    reconnectTimer_.stop();
    sinkTimer_.stop();
    player_.unregister();
    // the device stays connected (headphones, a phone call through the radio): it is only no longer the output
    if(connected_)
    {
        connected_ = false;
        sink_.clear();
        OPENAUTO_LOG(info) << "[CarAudio] radio no longer the output";
    }
    if(blocking)
    {
        router_.restoreNow();
    }
    else
    {
        router_.restore();
    }
}

void CarAudio::selectDevice(const QString& address)
{
    OPENAUTO_LOG(info) << "[CarAudio] radio " << address.toStdString();
    bluetooth_.stopSearch();
    auto settings = configuration_->getCarAudioSettings();
    settings.device = address.toUpper().toStdString();
    settings.enabled = true;
    configuration_->setCarAudioSettings(settings);
    configuration_->save();
    this->halt(false);
    this->apply();
}

void CarAudio::forgetDevice()
{
    auto settings = configuration_->getCarAudioSettings();
    const QString address = QString::fromStdString(settings.device);
    this->halt(false);
    settings.device.clear();
    settings.enabled = false;
    configuration_->setCarAudioSettings(settings);
    configuration_->save();
    if(!address.isEmpty())
    {
        bluetooth_.removeDevice(address);
    }
    this->publish();
}

void CarAudio::onDevicesChanged()
{
    if(!running_)
    {
        return;
    }
    const QVariantMap device = bluetooth_.device(QString::fromStdString(configuration_->getCarAudioSettings().device));
    const bool connected = device.value("connected").toBool();
    if(connected && !connected_)
    {
        this->onConnected();
    }
    else if(!connected && connected_)
    {
        this->onDisconnected();
    }
}

void CarAudio::onConnected()
{
    connected_ = true;
    const QString address = QString::fromStdString(configuration_->getCarAudioSettings().device);
    OPENAUTO_LOG(info) << "[CarAudio] radio connected, profiles: "
                       << bluetooth_.device(address).value("uuids").toStringList().join(' ').toStdString();
    sinkAttempts_ = 0;
    sinkTimer_.setInterval(1000);
    sinkTimer_.start();
    // car radios often connect hands-free (HFP) only and A2DP when asked for it (or when Bluetooth audio is chosen
    // on the radio): without A2DP there is no sink for the music (#112)
    this->requestA2dp();
    this->findSink();
    this->publish();
}

void CarAudio::onDisconnected(bool blocking)
{
    connected_ = false;
    sinkTimer_.stop();
    OPENAUTO_LOG(info) << "[CarAudio] radio disconnected";
    sink_.clear();
    if(blocking)
    {
        router_.restoreNow();
    }
    else
    {
        router_.restore();
    }
    this->publish();
}

void CarAudio::requestA2dp()
{
    const QString address = QString::fromStdString(configuration_->getCarAudioSettings().device);
    OPENAUTO_LOG(info) << "[CarAudio] asking " << address.toStdString() << " for A2DP";
    bluetooth_.connectProfile(address, wireless::bluez::cAudioSinkUuid);
}

void CarAudio::findSink()
{
    // the A2DP profile and the sink: OutputRouter (in the background, routed() when done)
    router_.route(QString::fromStdString(configuration_->getCarAudioSettings().device));
    ++sinkAttempts_;
    if(sinkAttempts_ % 5 == 0)
    {
        this->requestA2dp();
    }
    if(sinkAttempts_ == 20)
    {
        // shown, but the radio may still start A2DP (Bluetooth audio chosen on it): keep looking while it is
        // connected, less often
        error_ = qtTrId("caraudio.connected_but_no_audio");
        OPENAUTO_LOG(warning) << "[CarAudio] no sink for the radio yet, still trying";
        sinkTimer_.setInterval(5000);
        this->publish();
    }
}

void CarAudio::onCommand(const QString& name)
{
    const auto decision = callButtons_.decide(name, clock_.elapsed());
    OPENAUTO_LOG(info) << "[CarAudio] radio button: " << name.toStdString() << " (call: " << callState_.toStdString()
                       << ") -> " << (decision.action.isEmpty() ? std::string("ignored") : decision.action.toStdString())
                       << (decision.reason.isEmpty() ? std::string() : ": " + decision.reason.toStdString());
    if(!decision.action.isEmpty())
    {
        actions_.trigger(decision.action);
    }
}

void CarAudio::updatePlayer()
{
    if(callState_ == "incoming" || callState_ == "in_call" || callState_ == "on_hold")
    {
        const QString caller = call_.value("name").toString().isEmpty() ? call_.value("number").toString()
                                                                        : call_.value("name").toString();
        player_.update("Playing", (callState_ == "incoming" ? qtTrId("caraudio.call") : qtTrId("caraudio.talking")).arg(caller),
                       call_.value("number").toString(), QString(), 0, call_.value("duration").toDouble());
        return;
    }
    const QString state = mediaStatus_.value("state").toString();
    player_.update(state == "playing" ? "Playing" : state == "paused" ? "Paused" : "Stopped",
                   metadata_.value("title").toString(), metadata_.value("artist").toString(),
                   metadata_.value("album").toString(), metadata_.value("duration").toDouble(),
                   mediaStatus_.value("position").toDouble());
}

QVariantMap CarAudio::state() const
{
    const auto settings = configuration_->getCarAudioSettings();
    const QString address = QString::fromStdString(settings.device);
    const QVariantMap device = bluetooth_.device(address);
    QString status;
    if(!settings.enabled || address.isEmpty()) status = qtTrId("caraudio.off");
    else if(!error_.isEmpty()) status = error_;
    else if(connected_ && !sink_.isEmpty()) status = qtTrId("caraudio.sound_plays_through").arg(device.value("name", address).toString());
    else if(connected_) status = qtTrId("caraudio.connected_setting_up_audio");
    else if(device.isEmpty()) status = qtTrId("caraudio.pairing");
    else status = qtTrId("caraudio.waiting_radio").arg(device.value("name", address).toString());

    return {
        {"enabled", settings.enabled},
        {"device", address},
        {"name", device.value("name", address)},
        {"paired", !device.isEmpty()},
        {"connected", connected_},
        {"sink", sink_},
        {"error", error_},
        {"status", status},
    };
}

void CarAudio::publish()
{
    emit changed();
    core::EventBus::instance().publish("caraudio.state", this->state());
}

}
