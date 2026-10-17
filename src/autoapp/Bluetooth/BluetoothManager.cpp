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

#include <QDBusConnection>
#include <f1x/openauto/autoapp/Bluetooth/BluetoothManager.hpp>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x::openauto::autoapp::bt
{

namespace bluez = wireless::bluez;

namespace
{
const int cPairingSeconds = 120;
}

BluetoothManager::BluetoothManager(configuration::IConfiguration::Pointer configuration, QObject* parent)
    : QObject(parent)
    , configuration_(std::move(configuration))
{
    bluez::registerTypes();

    connect(&agent_, &wireless::PairingAgent::confirmationRequested, this, &BluetoothManager::onConfirmation);
    connect(&agent_, &wireless::PairingAgent::finished, this, [this]() {
        pairingDevice_.clear();
        pairingPasskey_.clear();
        core::EventBus::instance().publish("bluetooth.pairing", {{"active", false}});
        this->publish();
    });
    agent_.setPinProvider([this](const QString& device) {
        return addressOfPath(device) == pairAddress_ ? pairPin_ : QString();
    });

    pairingTimer_.setSingleShot(true);
    connect(&pairingTimer_, &QTimer::timeout, this, [this]() { this->setPairing(false); });

    pairTimer_.setSingleShot(true);
    connect(&pairTimer_, &QTimer::timeout, this, [this]() {
        OPENAUTO_LOG(info) << "[BluetoothManager] pairing with " << pairAddress_.toStdString() << " timed out";
        pairAddress_.clear();
        pairStarted_ = false;
        this->setDiscovery(searching_);
        this->updateAgent();
        this->publish();
    });

    searchTimer_.setSingleShot(true);
    connect(&searchTimer_, &QTimer::timeout, this, &BluetoothManager::stopSearch);

    // BlueZ sends bursts of property changes, collect them
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(300);
    connect(&refreshTimer_, &QTimer::timeout, this, &BluetoothManager::refresh);

    presenceTimer_.setSingleShot(true);
    presenceTimer_.setInterval(300);
    connect(&presenceTimer_, &QTimer::timeout, this, &BluetoothManager::checkPresence);

    auto bus = QDBusConnection::systemBus();
    for(const char* name: {"InterfacesAdded", "InterfacesRemoved"})
    {
        bus.connect(bluez::cService, "/", "org.freedesktop.DBus.ObjectManager", name, this, SLOT(onBluezSignal()));
        bus.connect(bluez::cService, "/", "org.freedesktop.DBus.ObjectManager", name, this, SLOT(onBluezObjects()));
    }
    // bluetoothd started or stopped
    bus.connect("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                QStringList{bluez::cService}, QString(), this, SLOT(onBluezObjects()));
    bus.connect(bluez::cService, QString(), "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                SLOT(onBluezSignal()));

    this->checkPresence();

    // pairing over the USB cable (Android Auto Bluetooth channel)
    connect(&core::EventBus::instance(), &core::EventBus::event, this, [this](const QString& topic, const QVariantMap& data) {
        if(topic == "bluetooth.expect")
        {
            this->pairWith(data.value("address").toString());
        }
    });
}

BluetoothManager::~BluetoothManager()
{
    this->setDiscovery(false);
    agent_.unregisterAgent();
}

QString BluetoothManager::addressOfPath(const QString& path)
{
    // /org/bluez/hci0/dev_E8_78_29_C0_E5_2D -> E8:78:29:C0:E5:2D
    return path.section('/', -1).mid(4).replace('_', ':').toUpper();
}

bool BluetoothManager::ensureAdapter()
{
    const QString wanted = QString::fromStdString(configuration_->getBluetoothAdapterAddress()).toUpper();
    const BluezObjects objects = bluez::managedObjects();
    QString path;
    for(auto it = objects.cbegin(); it != objects.cend(); ++it)
    {
        const QVariantMap adapter = it.value().value(bluez::cAdapter);
        if(adapter.isEmpty())
        {
            continue;
        }
        if(path.isEmpty() || adapter.value("Address").toString().toUpper() == wanted)
        {
            path = it.key().path();
            adapterName_ = adapter.value("Alias").toString();
            adapterUuids_ = adapter.value("UUIDs").toStringList();
        }
    }

    if(path.isEmpty())
    {
        error_ = objects.isEmpty() ? qtTrId("bluetooth.bluetooth_service_bluetoothd_not") : qtTrId("bluetooth.no_bluetooth_adapter_found");
        OPENAUTO_LOG(warning) << "[BluetoothManager] " << error_.toStdString();
        adapterPath_.clear();
        this->publish();
        return false;
    }

    const QString powerError = bluez::setProperty(path, bluez::cAdapter, "Powered", true);
    if(!powerError.isEmpty())
    {
        error_ = qtTrId("bluetooth.bluetooth_cannot_switched").arg(powerError);
        this->publish();
        return false;
    }

    error_.clear();
    if(adapterPath_ != path)
    {
        adapterPath_ = path;
        loaded_ = false;
        OPENAUTO_LOG(info) << "[BluetoothManager] adapter " << path.toStdString();
        this->refresh();
    }
    this->publish();
    return true;
}

QString BluetoothManager::adapterPath() const
{
    return adapterPath_;
}

QStringList BluetoothManager::adapterUuids() const
{
    return adapterUuids_;
}

QString BluetoothManager::error() const
{
    return error_;
}

void BluetoothManager::onBluezObjects()
{
    presenceTimer_.start();
}

void BluetoothManager::checkPresence()
{
    bool present = false;
    const BluezObjects objects = bluez::managedObjects();
    for(auto it = objects.cbegin(); it != objects.cend() && !present; ++it)
    {
        present = it.value().contains(bluez::cAdapter);
    }
    // the adapter in use is gone (USB dongle pulled): found again by ensureAdapter
    if(!adapterPath_.isEmpty() && !objects.contains(QDBusObjectPath(adapterPath_)))
    {
        OPENAUTO_LOG(info) << "[BluetoothManager] adapter " << adapterPath_.toStdString() << " removed";
        adapterPath_.clear();
        error_ = qtTrId("bluetooth.no_bluetooth_adapter_found");
    }
    if(present == present_)
    {
        return;
    }
    present_ = present;
    OPENAUTO_LOG(info) << "[BluetoothManager] " << (present ? "adapter present" : "no adapter");
    this->publish();
    emit presenceChanged(present);
}

void BluetoothManager::onBluezSignal()
{
    if(!adapterPath_.isEmpty())
    {
        refreshTimer_.start();
    }
}

QString BluetoothManager::deviceName(const QString& path) const
{
    for(const auto& list: {devices_, discovered_})
    {
        for(const auto& device: list)
        {
            const QVariantMap map = device.toMap();
            if(map.value("path").toString() == path)
            {
                return map.value("name").toString();
            }
        }
    }
    const QVariantMap properties = bluez::managedObjects().value(QDBusObjectPath(path)).value(bluez::cDevice);
    return properties.value("Alias", properties.value("Address")).toString();
}

QString BluetoothManager::pathOf(const QString& address) const
{
    return adapterPath_.isEmpty() ? QString() : adapterPath_ + "/dev_" + address.toUpper().replace(':', '_');
}

QVariantMap BluetoothManager::device(const QString& address) const
{
    for(const auto& device: devices_)
    {
        const QVariantMap map = device.toMap();
        if(map.value("address").toString().compare(address, Qt::CaseInsensitive) == 0)
        {
            return map;
        }
    }
    return {};
}

void BluetoothManager::refresh()
{
    if(adapterPath_.isEmpty())
    {
        return;
    }
    if(!pairAddress_.isEmpty() && !pairStarted_)
    {
        this->continuePairWith();
    }

    QVariantList devices;
    QVariantList discovered;
    QSet<QString> paired;
    const BluezObjects objects = bluez::managedObjects();
    for(auto it = objects.cbegin(); it != objects.cend(); ++it)
    {
        const QVariantMap device = it.value().value(bluez::cDevice);
        if(device.isEmpty() || device.value("Adapter").value<QDBusObjectPath>().path() != adapterPath_)
        {
            continue;
        }
        const QString path = it.key().path();
        const QString address = device.value("Address").toString();
        const QVariantMap entry{{"path", path},
                                {"address", address},
                                {"name", device.value("Alias", address).toString()},
                                {"icon", device.value("Icon").toString()},
                                {"connected", device.value("Connected").toBool()},
                                {"uuids", device.value("UUIDs").toStringList()}};
        if(!device.value("Paired").toBool())
        {
            // devices of the search with a name (nameless ones cannot be told apart)
            if(searching_ && device.contains("Name"))
            {
                discovered.append(entry);
            }
            continue;
        }

        paired.insert(path);
        devices.append(entry);
        if(loaded_ && !paired_.contains(path))
        {
            // newly paired: trust it, it may reconnect without asking
            OPENAUTO_LOG(info) << "[BluetoothManager] paired with " << address.toStdString();
            bluez::setProperty(path, bluez::cDevice, "Trusted", true);
            if(pairing_)
            {
                this->setPairing(false);
            }
            if(address.compare(pairAddress_, Qt::CaseInsensitive) == 0)
            {
                pairAddress_.clear();
                pairStarted_ = false;
                pairTimer_.stop();
                this->setDiscovery(searching_);
                // unregistering right away would cancel the pairing that is still finishing
                QTimer::singleShot(5000, this, &BluetoothManager::updateAgent);
            }
            QTimer::singleShot(0, this, [this, address]() { emit devicePaired(address.toUpper()); });
        }
    }
    for(const QString& path: paired_)
    {
        if(!paired.contains(path))
        {
            emit deviceRemoved(addressOfPath(path));
        }
    }

    paired_ = paired;
    loaded_ = true;
    if(devices != devices_ || discovered != discovered_)
    {
        devices_ = devices;
        discovered_ = discovered;
        emit devicesChanged();
    }
}

QVariantList BluetoothManager::devices() const
{
    return devices_;
}

QVariantList BluetoothManager::discovered() const
{
    return discovered_;
}

void BluetoothManager::updateAgent()
{
    if(pairing_ || !pairAddress_.isEmpty())
    {
        const QString error = agent_.registerAgent();
        if(!error.isEmpty())
        {
            OPENAUTO_LOG(warning) << "[BluetoothManager] pairing agent: " << error.toStdString();
        }
    }
    else
    {
        agent_.unregisterAgent();
    }
}

void BluetoothManager::pairWith(const QString& address, const QString& pin)
{
    if(!this->ensureAdapter())
    {
        return;
    }
    // the phone repeats its request every few seconds until the pairing is done
    if(pairAddress_ == address.toUpper())
    {
        return;
    }
    OPENAUTO_LOG(info) << "[BluetoothManager] pairing with " << address.toStdString();
    pairAddress_ = address.toUpper();
    pairPin_ = pin;
    pairStarted_ = false;
    pairTimer_.start(60000);
    bluez::setProperty(adapterPath_, bluez::cAdapter, "Pairable", true);
    this->updateAgent();
    this->continuePairWith();
    this->publish();
}

void BluetoothManager::continuePairWith()
{
    const QVariantMap device = bluez::managedObjects().value(QDBusObjectPath(this->pathOf(pairAddress_))).value(bluez::cDevice);
    if(!device.isEmpty())
    {
        pairStarted_ = true;
        this->setDiscovery(searching_);
        if(!device.value("Paired").toBool())
        {
            bluez::callAsync(this->pathOf(pairAddress_), bluez::cDevice, "Pair");
        }
        return;
    }
    // BlueZ only knows devices it has seen: search until the device shows up
    this->setDiscovery(true);
}

void BluetoothManager::onConfirmation(const QString& device, const QString& passkey)
{
    const QString address = addressOfPath(device);
    if(!pairAddress_.isEmpty() && address == pairAddress_)
    {
        // started by OpenHU for this device (cable, car radio search): no question
        OPENAUTO_LOG(info) << "[BluetoothManager] accepting pairing of " << address.toStdString();
        agent_.answer(true);
        // the phone on the cable confirms with it (Android Auto Bluetooth channel)
        core::EventBus::instance().emitEvent("bluetooth.passkey", {{"address", address}, {"passkey", passkey}});
        return;
    }

    pairingDevice_ = this->deviceName(device);
    pairingPasskey_ = passkey;
    OPENAUTO_LOG(info) << "[BluetoothManager] pairing request from " << pairingDevice_.toStdString();
    core::EventBus::instance().publish("bluetooth.pairing",
                                       {{"active", true}, {"device", pairingDevice_}, {"passkey", passkey}});
    this->publish();
}

void BluetoothManager::setPairing(bool enabled)
{
    if(enabled == pairing_)
    {
        return;
    }
    if(enabled && !this->ensureAdapter())
    {
        return;
    }

    pairing_ = enabled;
    this->updateAgent();
    if(enabled)
    {
        bluez::setProperty(adapterPath_, bluez::cAdapter, "Pairable", true);
        bluez::setProperty(adapterPath_, bluez::cAdapter, "DiscoverableTimeout", quint32(cPairingSeconds));
        bluez::setProperty(adapterPath_, bluez::cAdapter, "Discoverable", true);
        pairingTimer_.start(cPairingSeconds * 1000);
    }
    else
    {
        pairingTimer_.stop();
        if(!adapterPath_.isEmpty())
        {
            bluez::setProperty(adapterPath_, bluez::cAdapter, "Discoverable", false);
        }
    }
    OPENAUTO_LOG(info) << "[BluetoothManager] visible for pairing: " << (enabled ? "on" : "off");
    this->publish();
}

void BluetoothManager::confirmPairing(bool accept)
{
    OPENAUTO_LOG(info) << "[BluetoothManager] pairing " << (accept ? "accepted" : "rejected");
    agent_.answer(accept);
}

void BluetoothManager::setDiscovery(bool enabled)
{
    // also needed while a pairing waits for its device
    enabled = enabled || (!pairAddress_.isEmpty() && !pairStarted_);
    if(enabled == discovering_ || adapterPath_.isEmpty())
    {
        return;
    }
    if(enabled)
    {
        bluez::call(adapterPath_, bluez::cAdapter, "SetDiscoveryFilter", {QVariantMap{{"Transport", "bredr"}}});
        discovering_ = bluez::call(adapterPath_, bluez::cAdapter, "StartDiscovery").isEmpty();
    }
    else
    {
        discovering_ = false;
        bluez::call(adapterPath_, bluez::cAdapter, "StopDiscovery");
    }
    OPENAUTO_LOG(info) << "[BluetoothManager] search " << (discovering_ ? "on" : "off");
}

void BluetoothManager::startSearch(int seconds)
{
    if(!this->ensureAdapter())
    {
        return;
    }
    searching_ = true;
    searchTimer_.start(seconds * 1000);
    this->setDiscovery(true);
    this->publish();
}

void BluetoothManager::stopSearch()
{
    searching_ = false;
    searchTimer_.stop();
    this->setDiscovery(false);
    this->refresh();
    this->publish();
}

void BluetoothManager::connectDevice(const QString& address)
{
    if(this->ensureAdapter())
    {
        bluez::callAsync(this->pathOf(address), bluez::cDevice, "Connect");
    }
}

void BluetoothManager::connectProfile(const QString& address, const QString& uuid)
{
    if(!adapterPath_.isEmpty())
    {
        bluez::callAsync(this->pathOf(address), bluez::cDevice, "ConnectProfile", {uuid});
    }
}

void BluetoothManager::disconnectDevice(const QString& address)
{
    if(!adapterPath_.isEmpty())
    {
        bluez::callAsync(this->pathOf(address), bluez::cDevice, "Disconnect");
    }
}

void BluetoothManager::removeDevice(const QString& address)
{
    if(adapterPath_.isEmpty())
    {
        return;
    }
    bluez::call(adapterPath_, bluez::cAdapter, "RemoveDevice", {QVariant::fromValue(QDBusObjectPath(this->pathOf(address)))});
    this->refresh();
}

QVariantMap BluetoothManager::state() const
{
    return {
        // an adapter is there (the settings show Bluetooth only then)
        {"available", present_},
        {"error", error_},
        {"adapter", adapterName_},
        {"pairing", pairing_},
        {"pairingDevice", pairingDevice_},
        {"pairingPasskey", pairingPasskey_},
        {"pairingWith", pairAddress_},
        {"searching", searching_},
    };
}

void BluetoothManager::publish()
{
    emit changed();
    core::EventBus::instance().publish("bluetooth.state", this->state());
}

}
