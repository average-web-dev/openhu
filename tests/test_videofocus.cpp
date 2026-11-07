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

// Telling the phone when the reverse camera covers Android Auto: video focus and gear (#76)

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <string>
#include <vector>
#include "TestHome.hpp"
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
#include <f1x/openauto/autoapp/Plugins/PluginManager.hpp>
#include <f1x/openauto/autoapp/Service/MediaSink/VideoFocusPolicy.hpp>
// moc cannot parse all aasdk protobuf headers
#ifndef Q_MOC_RUN
#include <f1x/openauto/autoapp/Service/MediaSink/VideoMediaSinkService.hpp>
#endif
#include <f1x/openauto/autoapp/Service/Sensor/VehicleGear.hpp>
#include <f1x/openauto/autoapp/Service/Sensor/SimulatedDriving.hpp>

using f1x::openauto::autoapp::core::EventBus;
using f1x::openauto::autoapp::plugins::PluginManager;
using f1x::openauto::autoapp::service::mediasink::VideoFocusPolicy;
using f1x::openauto::autoapp::service::mediasink::VideoMediaSinkService;
using f1x::openauto::autoapp::service::sensor::gearFromVehicle;
using f1x::openauto::autoapp::service::sensor::simulatedDriving;
using Mode = aap_protobuf::service::media::video::message::VideoFocusMode;
using aap_protobuf::service::sensorsource::message::Gear;

namespace
{
const Mode PROJECTED = Mode::VIDEO_FOCUS_PROJECTED;
const Mode NATIVE = Mode::VIDEO_FOCUS_NATIVE;

QVariantMap overlays(const QStringList& ids)
{
    return {{"overlays", ids}};
}

std::optional<Mode> none()
{
    return std::nullopt;
}

// Records what the service sends to the phone; every send succeeds at once
class FakeChannel: public aasdk::channel::mediasink::video::IVideoMediaSinkService
{
public:
    std::vector<std::pair<Mode, bool>> indications;

    aasdk::messenger::ChannelId getId() const override { return aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO; }
    void send(aasdk::messenger::Message::Pointer, aasdk::channel::SendPromise::Pointer promise) override
    {
        promise->resolve();
    }
    void receive(aasdk::channel::mediasink::video::IVideoMediaSinkServiceEventHandler::Pointer) override {}
    void sendChannelOpenResponse(const aap_protobuf::service::control::message::ChannelOpenResponse&,
                                 aasdk::channel::SendPromise::Pointer promise) override { promise->resolve(); }
    void sendChannelSetupResponse(const aap_protobuf::service::media::shared::message::Config&,
                                  aasdk::channel::SendPromise::Pointer promise) override { promise->resolve(); }
    void sendMediaAckIndication(const aap_protobuf::service::media::source::message::Ack&,
                                aasdk::channel::SendPromise::Pointer promise) override { promise->resolve(); }
    void sendVideoFocusIndication(const aap_protobuf::service::media::video::message::VideoFocusNotification& indication,
                                  aasdk::channel::SendPromise::Pointer promise) override
    {
        indications.emplace_back(indication.focus(), indication.unsolicited());
        promise->resolve();
    }
    void sendUpdateUiConfigRequest(const aap_protobuf::service::control::message::UpdateUiConfigRequest&,
                                   aasdk::channel::SendPromise::Pointer promise) override { promise->resolve(); }
};

class FakeOutput: public f1x::openauto::autoapp::projection::IVideoOutput
{
public:
    bool open() override { return true; }
    bool init() override { return true; }
    void write(aasdk::messenger::Timestamp::ValueType, const aasdk::common::DataConstBuffer&) override {}
    void stop() override { ++stops; }
    void streamStopped() override { events.push_back("stopped"); }
    void streamStarted() override { events.push_back("started"); }
    int stops = 0;
    std::vector<std::string> events;
    aap_protobuf::service::media::sink::message::VideoFrameRateType getVideoFPS() const override
    {
        return aap_protobuf::service::media::sink::message::VIDEO_FPS_30;
    }
    aap_protobuf::service::media::sink::message::VideoCodecResolutionType getVideoResolution() const override
    {
        return aap_protobuf::service::media::sink::message::VIDEO_800x480;
    }
    size_t getScreenDPI() const override { return 140; }
    QRect getVideoMargins() const override { return QRect(); }
};
}

class TestVideoFocus: public QObject
{
    Q_OBJECT

private slots:
    void overlayShownAndHidden()
    {
        VideoFocusPolicy policy("aa");
        QCOMPARE(policy.setup(), PROJECTED);
        QCOMPARE(policy.setOverlays(overlays({"org.openhu.camera"})), std::optional<Mode>(NATIVE));
        // no duplicates: the same state again, a second overlay
        QCOMPARE(policy.setOverlays(overlays({"org.openhu.camera"})), none());
        QCOMPARE(policy.setOverlays(overlays({"org.openhu.camera", "other"})), none());
        QCOMPARE(policy.setOverlays(overlays({"other"})), none());
        QCOMPARE(policy.setOverlays(overlays({})), std::optional<Mode>(PROJECTED));
        QCOMPARE(policy.setOverlays(QVariantMap()), none());
    }

    void nothingBeforeTheSetup()
    {
        VideoFocusPolicy policy("aa");
        QCOMPARE(policy.setOverlays(overlays({"org.openhu.camera"})), none());
        // the camera was already shown when the phone connected
        QCOMPARE(policy.setup(), NATIVE);
        QCOMPARE(policy.setOverlays(overlays({})), std::optional<Mode>(PROJECTED));

        VideoFocusPolicy covered("aa", overlays({"org.openhu.camera"}));
        QCOMPARE(covered.setup(), NATIVE);
    }

    void cameraPageInsteadOfAndroidAuto()
    {
        VideoFocusPolicy policy("aa");
        policy.setup();
        QCOMPARE(policy.setPage("plugin:org.openhu.camera"), std::optional<Mode>(NATIVE));
        // back to Android Auto when the camera is hidden
        QCOMPARE(policy.setPage("aa"), std::optional<Mode>(PROJECTED));
        // left by hand to the home page while reversing: Android Auto is still not shown
        QCOMPARE(policy.setPage("plugin:org.openhu.camera"), std::optional<Mode>(NATIVE));
        QCOMPARE(policy.setPage("home"), none());
        QCOMPARE(policy.setPage("aa"), std::optional<Mode>(PROJECTED));
    }

    void otherPagesKeepTheProjection()
    {
        // leaving Android Auto by hand does not change anything for the phone (as before #76)
        VideoFocusPolicy policy("aa");
        policy.setup();
        QCOMPARE(policy.setPage("home"), none());
        QCOMPARE(policy.setPage("settings"), none());
        // a plugin page opened from the home page did not replace Android Auto
        QCOMPARE(policy.setPage("plugin:org.openhu.camera"), none());
        QCOMPARE(policy.setPage("aa"), none());
        QCOMPARE(policy.mode(), PROJECTED);
    }

    void exitInAndroidAuto()
    {
        VideoFocusPolicy policy("aa");
        policy.setup();
        // the phone asks for NATIVE ("Exit"), the answer is NATIVE (oaa video.md), the shell shows its home page
        QCOMPARE(policy.request(NATIVE), NATIVE);
        QCOMPARE(policy.setPage("home"), none());
        // the user opens Android Auto again: the phone has to render again
        QCOMPARE(policy.setPage("aa"), std::optional<Mode>(PROJECTED));
    }

    void phoneAsksForTheProjection()
    {
        VideoFocusPolicy policy("home");
        QCOMPARE(policy.request(PROJECTED), PROJECTED);
        QCOMPARE(policy.request(Mode::VIDEO_FOCUS_PROJECTED_NO_INPUT_FOCUS), PROJECTED);
        QCOMPARE(policy.request(NATIVE), NATIVE);
        QCOMPARE(policy.request(PROJECTED), PROJECTED);
        // the camera has the screen: the phone gets NATIVE, the overlay's end brings PROJECTED
        QCOMPARE(policy.setOverlays(overlays({"org.openhu.camera"})), std::optional<Mode>(NATIVE));
        QCOMPARE(policy.request(PROJECTED), NATIVE);
        QCOMPARE(policy.setOverlays(overlays({})), std::optional<Mode>(PROJECTED));
    }

    void overlayAfterExit()
    {
        VideoFocusPolicy policy("aa");
        policy.setup();
        policy.request(NATIVE);
        policy.setPage("home");
        // NATIVE already: the camera adds nothing, its end does not bring Android Auto back
        QCOMPARE(policy.setOverlays(overlays({"org.openhu.camera"})), none());
        QCOMPARE(policy.setOverlays(overlays({})), none());
        QCOMPARE(policy.mode(), NATIVE);
    }

    // The phone connects (or restarts its stream) while the camera page is shown: the shell keeps the camera page,
    // Android Auto stays NATIVE until its page is shown (#87)
    void cameraPageWhenTheSessionStarts()
    {
        VideoFocusPolicy policy("plugin:org.openhu.camera");
        QCOMPARE(policy.setup(), NATIVE);
        QCOMPARE(policy.request(PROJECTED), NATIVE);
        QCOMPARE(policy.setPage("aa"), std::optional<Mode>(PROJECTED));

        // the user leaves the camera page for home by hand: still NATIVE until Android Auto is opened
        VideoFocusPolicy byHand("plugin:org.openhu.camera");
        byHand.setup();
        QCOMPARE(byHand.setPage("home"), none());
        QCOMPARE(byHand.setPage("aa"), std::optional<Mode>(PROJECTED));

        VideoFocusPolicy home("home");
        QCOMPARE(home.setup(), PROJECTED);
    }

    void pluginManagerPublishesOverlays()
    {
        TestHome home;
        const QString dir = PluginManager::pluginsDir() + "/org.openhu.camera";
        QDir().mkpath(dir);
        QFile file(dir + "/manifest.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject::fromVariantMap(
            {{"id", "org.openhu.camera"}, {"name", "Camera"}, {"version", "1.0.0"}, {"api", PluginManager::cApiVersion},
             {"ui", QVariantMap{{"overlay", "qml/Overlay.qml"}}}})).toJson());
        file.close();

        PluginManager manager(0);
        manager.load();
        QVERIFY(manager.setOverlayVisible("org.openhu.camera", true));
        QCOMPARE(EventBus::instance().last("ui.overlay").value("overlays").toStringList(),
                 QStringList{"org.openhu.camera"});
        QVERIFY(manager.setOverlayVisible("org.openhu.camera", false));
        QVERIFY(EventBus::instance().last("ui.overlay").value("overlays").toStringList().isEmpty());

        // gone with the plugin (disabled, updated)
        QVERIFY(manager.setOverlayVisible("org.openhu.camera", true));
        manager.unload("org.openhu.camera");
        QVERIFY(EventBus::instance().last("ui.overlay").value("overlays").toStringList().isEmpty());
    }

    // The service with the EventBus and a fake channel: what reaches the phone
    void serviceSendsIndications()
    {
        auto& bus = EventBus::instance();
        bus.publish("ui.page", {{"page", "aa"}});
        bus.publish("ui.overlay", overlays({}));

        boost::asio::io_service ioService;
        auto channel = std::make_shared<FakeChannel>();
        auto service = std::make_shared<VideoMediaSinkService>(ioService, channel, std::make_shared<FakeOutput>());
        service->start();
        ioService.poll();
        ioService.restart();

        // nothing before the channel setup
        bus.publish("ui.overlay", overlays({"org.openhu.camera"}));
        bus.publish("ui.overlay", overlays({}));
        ioService.poll();
        ioService.restart();
        QVERIFY(channel->indications.empty());

        service->onMediaChannelSetupRequest(aap_protobuf::service::media::shared::message::Setup());
        ioService.poll();
        ioService.restart();
        QCOMPARE(channel->indications.size(), size_t(1));
        QCOMPARE(channel->indications.back(), std::make_pair(PROJECTED, false));

        // camera overlay: NATIVE unsolicited, once
        bus.publish("ui.overlay", overlays({"org.openhu.camera"}));
        bus.publish("ui.overlay", overlays({"org.openhu.camera"}));
        ioService.poll();
        ioService.restart();
        QCOMPARE(channel->indications.size(), size_t(2));
        QCOMPARE(channel->indications.back(), std::make_pair(NATIVE, true));

        bus.publish("ui.overlay", overlays({}));
        ioService.poll();
        ioService.restart();
        QCOMPARE(channel->indications.size(), size_t(3));
        QCOMPARE(channel->indications.back(), std::make_pair(PROJECTED, true));

        // "Exit" in Android Auto: answered with NATIVE, the shell gets projection.exit
        bool exited = false;
        auto connection = QObject::connect(&bus, &EventBus::event, [&exited](const QString& topic, const QVariantMap&) {
            exited = exited || topic == "projection.exit";
        });
        aap_protobuf::service::media::video::message::VideoFocusRequestNotification request;
        request.set_mode(NATIVE);
        service->onVideoFocusRequest(request);
        ioService.poll();
        ioService.restart();
        QObject::disconnect(connection);
        QVERIFY(exited);
        QCOMPARE(channel->indications.size(), size_t(4));
        QCOMPARE(channel->indications.back(), std::make_pair(NATIVE, false));

        bus.publish("ui.page", {{"page", "home"}});
        bus.publish("ui.page", {{"page", "aa"}});
        ioService.poll();
        ioService.restart();
        QCOMPARE(channel->indications.size(), size_t(5));
        QCOMPARE(channel->indications.back(), std::make_pair(PROJECTED, true));

        // after stop() the service no longer listens
        service->stop();
        ioService.poll();
        ioService.restart();
        bus.publish("ui.overlay", overlays({"org.openhu.camera"}));
        ioService.poll();
        QCOMPARE(channel->indications.size(), size_t(5));
        bus.publish("ui.overlay", overlays({}));
    }

    // The phone stops and restarts the stream within the connection (#88): the output only hears about it and is
    // not stopped (which would clear the picture)
    void streamStopAndStartReachTheOutput()
    {
        boost::asio::io_service ioService;
        auto channel = std::make_shared<FakeChannel>();
        auto output = std::make_shared<FakeOutput>();
        auto service = std::make_shared<VideoMediaSinkService>(ioService, channel, output);
        service->onMediaChannelSetupRequest(aap_protobuf::service::media::shared::message::Setup());
        aap_protobuf::service::media::shared::message::Start start;
        start.set_session_id(1);
        start.set_configuration_index(0);
        service->onMediaChannelStartIndication(start);
        service->onMediaChannelStopIndication(aap_protobuf::service::media::shared::message::Stop());
        start.set_session_id(2);
        service->onMediaChannelStartIndication(start);
        ioService.poll();
        QCOMPARE(output->events, (std::vector<std::string>{"started", "stopped", "started"}));
        QCOMPARE(output->stops, 0);
    }

    void gearMapping()
    {
        // the CAN plugin's example profile: only the reverse gear
        QCOMPARE(gearFromVehicle({{"reverse", true}}), std::optional<Gear>(Gear::GEAR_REVERSE));
        QCOMPARE(gearFromVehicle({{"reverse", false}}), std::optional<Gear>(Gear::GEAR_DRIVE));
        QCOMPARE(int(Gear::GEAR_REVERSE), 102);
        // names and numbers
        QCOMPARE(gearFromVehicle({{"gear", "park"}}), std::optional<Gear>(Gear::GEAR_PARK));
        QCOMPARE(gearFromVehicle({{"gear", "R"}}), std::optional<Gear>(Gear::GEAR_REVERSE));
        QCOMPARE(gearFromVehicle({{"gear", "neutral"}}), std::optional<Gear>(Gear::GEAR_NEUTRAL));
        QCOMPARE(gearFromVehicle({{"gear", "drive"}}), std::optional<Gear>(Gear::GEAR_DRIVE));
        QCOMPARE(gearFromVehicle({{"gear", 3}}), std::optional<Gear>(Gear::GEAR_3));
        QCOMPARE(gearFromVehicle({{"gear", 0}}), std::optional<Gear>(Gear::GEAR_NEUTRAL));
        // "gear" wins over "reverse"
        QCOMPARE(gearFromVehicle({{"gear", "park"}, {"reverse", false}}), std::optional<Gear>(Gear::GEAR_PARK));
        // unknown: nothing (a closed enum on the phone)
        QCOMPARE(gearFromVehicle({{"gear", 11}}), std::optional<Gear>());
        QCOMPARE(gearFromVehicle({{"gear", "sport"}}), std::optional<Gear>());
        QCOMPARE(gearFromVehicle({}), std::optional<Gear>());
        QCOMPARE(gearFromVehicle({{"kmh", 12}}), std::optional<Gear>());
    }

    void simulatedDrivingStates()
    {
        // parked: no restrictions, parking brake on, standing
        const auto park = simulatedDriving({{"state", "park"}});
        QVERIFY(park);
        QCOMPARE(park->gear, Gear::GEAR_PARK);
        QVERIFY(park->parkingBrake);
        QCOMPARE(park->kmh, 0.0);
        QCOMPARE(park->drivingStatus, 0);
        // driving: every restriction (31), 50 km/h
        const auto drive = simulatedDriving({{"state", "drive"}});
        QVERIFY(drive);
        QCOMPARE(drive->gear, Gear::GEAR_DRIVE);
        QVERIFY(!drive->parkingBrake);
        QCOMPARE(drive->kmh, 50.0);
        QCOMPARE(drive->drivingStatus, 31);
        const auto reverse = simulatedDriving({{"state", "reverse"}});
        QVERIFY(reverse);
        QCOMPARE(reverse->gear, Gear::GEAR_REVERSE);
        QCOMPARE(reverse->drivingStatus, 31);
        // off: the real data
        QVERIFY(!simulatedDriving({{"state", ""}}));
        QVERIFY(!simulatedDriving({{"state", "off"}}));
        QVERIFY(!simulatedDriving({}));
    }
};

QTEST_GUILESS_MAIN(TestVideoFocus)
#include "test_videofocus.moc"
