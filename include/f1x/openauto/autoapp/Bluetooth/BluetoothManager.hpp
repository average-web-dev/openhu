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

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <f1x/openauto/autoapp/Configuration/IConfiguration.hpp>
#include <f1x/openauto/autoapp/Wireless/Bluez.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace bt
{

// The Bluetooth adapter shared by wireless Android Auto (phone) and the car radio (audio output), QML/API
// "bluetooth": pairing (visible for 2 minutes with confirmation in the shell, or started by OpenHU for a known
// address: phone over the USB cable, car radio from the search), paired devices, device search.
// Publishes "bluetooth.state" and "bluetooth.pairing".
class BluetoothManager: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList discovered READ discovered NOTIFY devicesChanged)

public:
    explicit BluetoothManager(configuration::IConfiguration::Pointer configuration, QObject* parent = nullptr);
    ~BluetoothManager() override;

    // Finds the adapter (configured address or the first one) and switches it on. False with error() set.
    bool ensureAdapter();
    // BlueZ has an adapter (also before ensureAdapter, follows adapters plugged in and removed)
    bool present() const { return present_; }
    QString adapterPath() const;
    QStringList adapterUuids() const;
    QString error() const;

    QVariantMap state() const;
    // paired devices: path, address, name, icon, connected, uuids
    QVariantList devices() const;
    // unpaired devices found by the search
    QVariantList discovered() const;
    QString deviceName(const QString& path) const;
    QString pathOf(const QString& address) const;
    QVariantMap device(const QString& address) const;

    // Visible for 2 minutes, requests are confirmed in the shell
    Q_INVOKABLE void setPairing(bool enabled);
    Q_INVOKABLE void confirmPairing(bool accept);
    // OpenHU starts the pairing with this device and confirms it by itself (searches it first if needed);
    // pin for legacy pairing, empty = numeric comparison only
    Q_INVOKABLE void pairWith(const QString& address, const QString& pin = QString());
    Q_INVOKABLE void startSearch(int seconds = 30);
    Q_INVOKABLE void stopSearch();
    Q_INVOKABLE void connectDevice(const QString& address);
    // one profile of a connected device (BlueZ ConnectProfile), e.g. A2DP of a car radio that connected hands-free only
    void connectProfile(const QString& address, const QString& uuid);
    Q_INVOKABLE void disconnectDevice(const QString& address);
    Q_INVOKABLE void removeDevice(const QString& address);

signals:
    void changed();
    void devicesChanged();
    // pairing finished (also pairings confirmed in the shell)
    void devicePaired(const QString& address);
    void deviceRemoved(const QString& address);
    // an adapter came or went (present())
    void presenceChanged(bool present);

private slots:
    // BlueZ object/property changes (connected by name, needs a slot)
    void onBluezSignal();
    // objects added or removed: maybe an adapter
    void onBluezObjects();

private:
    void refresh();
    void checkPresence();
    void updateAgent();
    void continuePairWith();
    void onConfirmation(const QString& device, const QString& passkey);
    void setDiscovery(bool enabled);
    void publish();
    static QString addressOfPath(const QString& path);

    configuration::IConfiguration::Pointer configuration_;
    QString adapterPath_;
    QString adapterName_;
    QStringList adapterUuids_;
    QString error_;
    wireless::PairingAgent agent_;

    bool pairing_ = false;
    QTimer pairingTimer_;
    QString pairingDevice_;
    QString pairingPasskey_;

    // pairing started by OpenHU
    QString pairAddress_;
    QString pairPin_;
    bool pairStarted_ = false;
    QTimer pairTimer_;

    bool searching_ = false;
    bool discovering_ = false;
    QTimer searchTimer_;

    QTimer refreshTimer_;
    QTimer presenceTimer_;
    bool present_ = false;
    QVariantList devices_;
    QVariantList discovered_;
    QSet<QString> paired_;
    bool loaded_ = false;
};

}
}
}
}
