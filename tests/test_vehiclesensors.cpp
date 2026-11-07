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

// Vehicle data from plugins and the API as Android Auto sensors (#115)
#include <QtTest>
#include <map>
#include <vector>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
// moc cannot parse all aasdk protobuf headers
#ifndef Q_MOC_RUN
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/sensorsource/SensorMessageId.pb.h>
#include <aasdk/Messenger/IMessenger.hpp>
#include <f1x/openauto/autoapp/Service/Sensor/SensorService.hpp>
#include <f1x/openauto/autoapp/Service/Sensor/VehicleSensors.hpp>
#endif

using f1x::openauto::autoapp::core::EventBus;
namespace sensor = f1x::openauto::autoapp::service::sensor;
namespace msg = aap_protobuf::service::sensorsource::message;

namespace {

  // the topics of a test instead of the EventBus
  sensor::TopicLookup lookup(const std::map<QString, QVariantMap> &topics) {
    return [topics](const QString &topic) {
      const auto it = topics.find(topic);
      return it == topics.end() ? QVariantMap() : it->second;
    };
  }

  std::optional<msg::SensorBatch> data(msg::SensorType type, const std::map<QString, QVariantMap> &topics) {
    for (const auto &entry: sensor::vehicleSensors()) {
      if (entry.type == type) {
        return sensor::vehicleSensorData(entry, lookup(topics));
      }
    }
    return std::nullopt;
  }

  // records what the sensor service sends to the phone
  class FakeMessenger: public aasdk::messenger::IMessenger {
  public:
    void enqueueReceive(aasdk::messenger::ChannelId, aasdk::messenger::ReceivePromise::Pointer) override {}

    void enqueueSend(aasdk::messenger::Message::Pointer message, aasdk::messenger::SendPromise::Pointer promise) override {
      const auto &payload = message->getPayload();
      const uint16_t id = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
      sent.emplace_back(id, std::string(payload.begin() + 2, payload.end()));
      promise->resolve();
    }

    void stop() override {}

    // the sensor batches sent, oldest first
    std::vector<msg::SensorBatch> batches() const {
      std::vector<msg::SensorBatch> result;
      for (const auto &[id, bytes]: sent) {
        if (id == aap_protobuf::service::sensorsource::SensorMessageId::SENSOR_MESSAGE_BATCH) {
          msg::SensorBatch batch;
          batch.ParseFromString(bytes);
          result.push_back(batch);
        }
      }
      return result;
    }

    std::vector<msg::SensorError> errors() const {
      std::vector<msg::SensorError> result;
      for (const auto &[id, bytes]: sent) {
        if (id == aap_protobuf::service::sensorsource::SensorMessageId::SENSOR_MESSAGE_ERROR) {
          msg::SensorError error;
          error.ParseFromString(bytes);
          result.push_back(error);
        }
      }
      return result;
    }

    std::vector<std::pair<uint16_t, std::string>> sent;
  };

  std::vector<msg::SensorType> announced(sensor::SensorService &service) {
    aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
    service.fillFeatures(response);
    std::vector<msg::SensorType> types;
    for (const auto &entry: response.channels(0).sensor_source_service().sensors()) {
      types.push_back(entry.sensor_type());
    }
    return types;
  }

  bool contains(const std::vector<msg::SensorType> &types, msg::SensorType type) {
    return std::find(types.begin(), types.end(), type) != types.end();
  }

}

class TestVehicleSensors: public QObject {
  Q_OBJECT

private slots:
  void init() {
    for (const QString topic: {"vehicle.speed", "vehicle.parkingBrake", "vehicle.gear", "vehicle.fuel", "vehicle.info",
                               "vehicle.odometer", "vehicle.engine", "vehicle.climate", "vehicle.compass",
                               "vehicle.drivingStatus", "vehicle.simulate"}) {
      EventBus::instance().publish(topic, {});
    }
  }

  void units() {
    // 36 km/h = 10 m/s
    QCOMPARE(data(msg::SENSOR_SPEED, {{"vehicle.speed", {{"kmh", 36.0}}}})->speed_data(0).speed_e3(), 10000);
    QVERIFY(data(msg::SENSOR_SPEED, {{"vehicle.speed", {{"kmh", 36}, {"cruise", true}}}})->speed_data(0).cruise_engaged());
    QCOMPARE(data(msg::SENSOR_PARKING_BRAKE, {{"vehicle.parkingBrake", {{"engaged", true}}}})
                 ->parking_brake_data(0).parking_brake(), true);
    const auto fuel = data(msg::SENSOR_FUEL, {{"vehicle.fuel", {{"percent", 42.4}, {"rangeKm", 380}, {"low", false}}}});
    QCOMPARE(fuel->fuel_data(0).fuel_level(), 42);
    QCOMPARE(fuel->fuel_data(0).range(), 380);
    QCOMPARE(fuel->fuel_data(0).low_fuel_warning(), false);
    QCOMPARE(data(msg::SENSOR_FUEL, {{"vehicle.fuel", {{"percent", 130}}}})->fuel_data(0).fuel_level(), 100);
    const auto odometer = data(msg::SENSOR_ODOMETER, {{"vehicle.odometer", {{"km", 123456.7}, {"tripKm", 12.3}}}});
    QCOMPARE(odometer->odometer_data(0).kms_e1(), 1234567);
    QCOMPARE(odometer->odometer_data(0).trip_kms_e1(), 123);
    QCOMPARE(data(msg::SENSOR_RPM, {{"vehicle.engine", {{"rpm", 2500}}}})->rpm_data(0).rpm_e3(), 2500000);
    const auto environment = data(msg::SENSOR_ENVIRONMENT_DATA, {{"vehicle.climate", {{"outsideTemp", -3.5}}}});
    QCOMPARE(environment->environment_data(0).temperature_e3(), -3500);
    QVERIFY(!environment->environment_data(0).has_pressure_e3());
    QCOMPARE(data(msg::SENSOR_COMPASS, {{"vehicle.compass", {{"heading", -90}}}})->compass_data(0).bearing_e6(), 270000000);
    QCOMPARE(data(msg::SENSOR_GEAR, {{"vehicle.gear", {{"gear", "park"}}}})->gear_data(0).gear(), msg::Gear::GEAR_PARK);
  }

  void noValue() {
    // no topic, a topic without the field, a field that is no number, "available": false
    QVERIFY(!data(msg::SENSOR_SPEED, {}));
    QVERIFY(!data(msg::SENSOR_SPEED, {{"vehicle.speed", {{"mph", 20}}}}));
    QVERIFY(!data(msg::SENSOR_SPEED, {{"vehicle.speed", {{"kmh", "fast"}}}}));
    QVERIFY(!data(msg::SENSOR_SPEED, {{"vehicle.speed", {{"kmh", 20}, {"available", false}}}}));
    QVERIFY(!data(msg::SENSOR_PARKING_BRAKE, {{"vehicle.parkingBrake", {{"engaged", QVariant()}}}}));
    // the CAN plugin's example profile publishes headlights into vehicle.lights and outsideTemp into vehicle.climate
    QVERIFY(!data(msg::SENSOR_ENVIRONMENT_DATA, {{"vehicle.climate", {{"insideTemp", 21}}}}));
  }

  void drivingStatus() {
    using Topics = std::map<QString, QVariantMap>;
    const int full = sensor::kFullyRestricted;
    QCOMPARE(full, 31);
    // no vehicle data: unrestricted as before
    QCOMPARE(sensor::drivingStatus(lookup({})), 0);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.engine", {{"rpm", 800}}}})), 0);
    // parked: brake on or P, standing (or no speed)
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.parkingBrake", {{"engaged", true}}}})), 0);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.gear", {{"gear", "park"}}}})), 0);
    QCOMPARE(sensor::drivingStatus(lookup(Topics{{"vehicle.parkingBrake", {{"engaged", true}}},
                                                 {"vehicle.speed", {{"kmh", 0.5}}}})), 0);
    // moving with the brake on, standing without brake or P (traffic light), driving
    QCOMPARE(sensor::drivingStatus(lookup(Topics{{"vehicle.parkingBrake", {{"engaged", true}}},
                                                 {"vehicle.speed", {{"kmh", 12}}}})), full);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.speed", {{"kmh", 0}}}})), full);
    QCOMPARE(sensor::drivingStatus(lookup(Topics{{"vehicle.parkingBrake", {{"engaged", false}}},
                                                 {"vehicle.gear", {{"gear", "drive"}}}})), full);
    // only the reverse light (CAN example profile): says nothing about parking
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.gear", {{"reverse", false}}}})), 0);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.gear", {{"reverse", true}}}})), 0);
    // a stale speed ("available": false) counts as unknown
    QCOMPARE(sensor::drivingStatus(lookup(Topics{{"vehicle.parkingBrake", {{"engaged", true}}},
                                                 {"vehicle.speed", {{"kmh", 50}, {"available", false}}}})), 0);
    // a plugin's driving status wins
    QCOMPARE(sensor::drivingStatus(lookup(Topics{{"vehicle.drivingStatus", {{"restricted", false}}},
                                                 {"vehicle.speed", {{"kmh", 80}}}})), 0);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.drivingStatus", {{"restricted", true}}}})), full);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.drivingStatus", {{"restrictions", QStringList{"video", "keyboard"}}}}})), 3);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.drivingStatus", {{"status", 255}}}})), full);
    // the simulated state wins over everything
    QCOMPARE(sensor::drivingStatus(lookup(Topics{{"vehicle.simulate", {{"state", "park"}}},
                                                 {"vehicle.drivingStatus", {{"restricted", true}}}})), 0);
    QCOMPARE(sensor::drivingStatus(lookup({{"vehicle.simulate", {{"state", "drive"}}}})), full);
  }

  void simulatedTopics() {
    const auto topics = lookup({{"vehicle.simulate", {{"state", "drive"}}}, {"vehicle.gear", {{"reverse", true}}}});
    QCOMPARE(sensor::vehicleTopic("vehicle.gear", topics).value("gear").toString(), QString("drive"));
    QCOMPARE(sensor::vehicleTopic("vehicle.speed", topics).value("kmh").toDouble(), 50.0);
    QCOMPARE(sensor::vehicleTopic("vehicle.parkingBrake", topics).value("engaged").toBool(), false);
    // other topics stay real
    QVERIFY(sensor::vehicleTopic("vehicle.fuel", topics).isEmpty());
  }

  void fuelAndConnectorTypes() {
    const QVariantMap info{{"fuelTypes", QStringList{"petrol", "Diesel", "lpg", "electric", "kerosene", "petrol"}},
                           {"evConnectors", QStringList{"type2", "ccs2", "chademo", "unknown"}}};
    QCOMPARE(sensor::fuelTypes(info), (std::vector<msg::FuelType>{msg::FUEL_TYPE_UNLEADED, msg::FUEL_TYPE_DIESEL_1,
                                                                  msg::FUEL_TYPE_LPG, msg::FUEL_TYPE_ELECTRIC}));
    QCOMPARE(sensor::evConnectorTypes(info),
             (std::vector<msg::EvConnectorType>{msg::EV_CONNECTOR_TYPE_MENNEKES, msg::EV_CONNECTOR_TYPE_COMBO_2,
                                                msg::EV_CONNECTOR_TYPE_CHADEMO}));
    QVERIFY(sensor::fuelTypes({}).empty());
  }

  // the service: announcement, first value, changes, unchanged values, errors
  void service() {
    boost::asio::io_service ioService;
    auto messenger = std::make_shared<FakeMessenger>();
    auto service = std::make_shared<sensor::SensorService>(ioService, messenger);

    // nothing published: only the sensors as before
    auto types = announced(*service);
    QVERIFY(contains(types, msg::SENSOR_DRIVING_STATUS_DATA));
    QVERIFY(!contains(types, msg::SENSOR_SPEED));
    QVERIFY(!contains(types, msg::SENSOR_PARKING_BRAKE));

    EventBus::instance().publish("vehicle.speed", {{"kmh", 0}});
    EventBus::instance().publish("vehicle.parkingBrake", {{"engaged", true}});
    EventBus::instance().publish("vehicle.fuel", {{"percent", 60}});
    EventBus::instance().publish("vehicle.info", {{"fuelTypes", QStringList{"diesel"}}});
    aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
    service->fillFeatures(response);
    const auto &channel = response.channels(0).sensor_source_service();
    QCOMPARE(channel.supported_fuel_types_size(), 1);
    types = announced(*service);
    QVERIFY(contains(types, msg::SENSOR_SPEED));
    QVERIFY(contains(types, msg::SENSOR_PARKING_BRAKE));
    QVERIFY(contains(types, msg::SENSOR_FUEL));
    QVERIFY(!contains(types, msg::SENSOR_ODOMETER));

    service->start();
    ioService.poll();
    for (const auto type: {msg::SENSOR_DRIVING_STATUS_DATA, msg::SENSOR_PARKING_BRAKE, msg::SENSOR_FUEL}) {
      msg::SensorRequest request;
      request.set_type(type);
      request.set_min_update_period(0);
      service->onSensorStartRequest(request);
    }
    ioService.poll();
    // parked: unrestricted, brake on, fuel
    auto batches = messenger->batches();
    QCOMPARE(batches.size(), size_t(3));
    QCOMPARE(batches[0].driving_status_data_size(), 1);
    QCOMPARE(batches[0].driving_status_data(0).status(), 0);
    QCOMPARE(batches[1].parking_brake_data_size(), 1);
    QVERIFY(batches[1].parking_brake_data(0).parking_brake());
    QCOMPARE(batches[2].fuel_data_size(), 1);
    QCOMPARE(batches[2].fuel_data(0).fuel_level(), 60);

    // the same value again: nothing; released brake while standing: brake and restrictions
    EventBus::instance().publish("vehicle.fuel", {{"percent", 60}});
    EventBus::instance().publish("vehicle.parkingBrake", {{"engaged", false}});
    ioService.poll();
    batches = messenger->batches();
    QCOMPARE(batches.size(), size_t(5));
    bool brakeReleased = false;
    bool restricted = false;
    for (size_t i = 3; i < batches.size(); ++i) {
      brakeReleased |= batches[i].parking_brake_data_size() == 1 && !batches[i].parking_brake_data(0).parking_brake();
      restricted |= batches[i].driving_status_data_size() == 1
                    && batches[i].driving_status_data(0).status() == sensor::kFullyRestricted;
    }
    QVERIFY(brakeReleased);
    QVERIFY(restricted);

    // the plugin lost its source: one error, back: SENSOR_OK and the value
    EventBus::instance().publish("vehicle.fuel", {{"available", false}});
    EventBus::instance().publish("vehicle.fuel", {{"available", false}});
    ioService.poll();
    QCOMPARE(messenger->errors().size(), size_t(1));
    QCOMPARE(messenger->errors()[0].sensor_type(), msg::SENSOR_FUEL);
    QCOMPARE(messenger->errors()[0].sensor_error_type(), msg::SENSOR_ERROR_TRANSIENT);
    EventBus::instance().publish("vehicle.fuel", {{"percent", 59}});
    ioService.poll();
    QCOMPARE(messenger->errors().size(), size_t(2));
    QCOMPARE(messenger->errors()[1].sensor_error_type(), msg::SENSOR_OK);
    QCOMPARE(messenger->batches().back().fuel_data_size(), 1);
    QCOMPARE(messenger->batches().back().fuel_data(0).fuel_level(), 59);

    service->stop();
    ioService.poll();
  }
};

QTEST_GUILESS_MAIN(TestVehicleSensors)
#include "test_vehiclesensors.moc"
