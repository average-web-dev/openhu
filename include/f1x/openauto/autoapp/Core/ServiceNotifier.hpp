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

#pragma once

#include <chrono>
#include <QByteArray>
#include <QObject>
#include <QTimer>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace core
{

// systemd's service notification (sd_notify(3)) without libsystemd: one datagram to the unix socket in
// $NOTIFY_SOCKET (a path, or "@name" for the abstract namespace). OpenHU OS runs OpenHU with Type=notify and
// WatchdogSec= (#129): READY=1 once the shell is up, then WATCHDOG=1 every half of $WATCHDOG_USEC from a timer in the
// GUI thread, so a blocked GUI thread (deadlock, endless loop) misses it and systemd restarts the session. Without
// $NOTIFY_SOCKET (container, a plain service) nothing is sent; without $WATCHDOG_USEC only READY=1.
// $WATCHDOG_PID is not checked: under cage it is cage's pid, OpenHU is cage's child. On OpenHU OS $NOTIFY_SOCKET is
// a relay in openhu-session.service (notify-relay): PAM puts cage and OpenHU into the logind session scope, from
// where systemd would not take OpenHU's messages for the service.
class ServiceNotifier: public QObject
{
    Q_OBJECT

public:
    // The watchdog time systemd's watchdog gets during the restart in place (execv, the pid stays): the new process
    // pings only once its shell is up again, like at the first start (before READY=1 only the start timeout counts).
    static constexpr std::chrono::seconds cRestartWatchdog{60};

    // From $NOTIFY_SOCKET and $WATCHDOG_USEC, and removes them (with $WATCHDOG_PID) from the environment: plugins
    // and the other child processes must not ping for OpenHU. prepareRestart() puts them back for the new process.
    explicit ServiceNotifier(QObject* parent = nullptr);
    ServiceNotifier(QByteArray socket, std::chrono::microseconds watchdog, QObject* parent = nullptr);

    bool enabled() const { return !socket_.isEmpty(); }
    std::chrono::microseconds watchdog() const { return watchdog_; }
    // 0 without a watchdog
    std::chrono::milliseconds pingInterval() const;

    // READY=1 (with the configured WATCHDOG_USEC again, after a restart) and starts the pings
    void ready();
    // Before the restart in place: a longer watchdog time for the new process's start, the environment back
    void prepareRestart();

    // one datagram, "KEY=value" lines; false when disabled or sending fails
    bool send(const QByteArray& state) const;

private:
    QByteArray socket_;
    std::chrono::microseconds watchdog_;
    QTimer timer_;
};

}
}
}
}
