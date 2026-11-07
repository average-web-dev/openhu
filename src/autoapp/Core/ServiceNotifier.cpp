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

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <f1x/openauto/autoapp/Core/ServiceNotifier.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{
namespace core
{

ServiceNotifier::ServiceNotifier(QByteArray socket, std::chrono::microseconds watchdog, QObject* parent)
    : QObject(parent)
    , socket_(std::move(socket))
    , watchdog_(watchdog.count() > 0 ? watchdog : std::chrono::microseconds(0))
{
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, [this]() { send("WATCHDOG=1"); });
}

namespace
{

std::chrono::microseconds watchdogFromEnvironment()
{
    bool valid = false;
    const qint64 usec = qEnvironmentVariable("WATCHDOG_USEC").toLongLong(&valid);
    return std::chrono::microseconds(valid ? usec : 0);
}

}

ServiceNotifier::ServiceNotifier(QObject* parent)
    : ServiceNotifier(qgetenv("NOTIFY_SOCKET"), watchdogFromEnvironment(), parent)
{
    qunsetenv("NOTIFY_SOCKET");
    qunsetenv("WATCHDOG_USEC");
    qunsetenv("WATCHDOG_PID");
}

std::chrono::milliseconds ServiceNotifier::pingInterval() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(watchdog_ / 2);
}

void ServiceNotifier::ready()
{
    if(!enabled())
    {
        return;
    }
    QByteArray state = "READY=1";
    if(watchdog_.count() > 0)
    {
        // after a restart in place systemd still has cRestartWatchdog
        state += "\nWATCHDOG_USEC=" + QByteArray::number(qint64(watchdog_.count()));
        timer_.start(std::max<std::chrono::milliseconds>(pingInterval(), std::chrono::milliseconds(1)));
    }
    if(send(state))
    {
        OPENAUTO_LOG(info) << "[ServiceNotifier] ready"
                           << (watchdog_.count() > 0 ? ", watchdog ping every " + std::to_string(pingInterval().count()) + " ms"
                                                     : std::string());
    }
}

void ServiceNotifier::prepareRestart()
{
    if(!enabled())
    {
        return;
    }
    timer_.stop();
    if(watchdog_.count() > 0)
    {
        send("WATCHDOG=1\nWATCHDOG_USEC=" +
             QByteArray::number(qint64(std::chrono::duration_cast<std::chrono::microseconds>(cRestartWatchdog).count())));
        qputenv("WATCHDOG_USEC", QByteArray::number(qint64(watchdog_.count())));
    }
    qputenv("NOTIFY_SOCKET", socket_);
}

bool ServiceNotifier::send(const QByteArray& state) const
{
    if(!enabled())
    {
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if(socket_.size() >= qsizetype(sizeof(address.sun_path)) || (socket_[0] != '/' && socket_[0] != '@'))
    {
        OPENAUTO_LOG(warning) << "[ServiceNotifier] unsupported NOTIFY_SOCKET " << socket_.toStdString();
        return false;
    }
    std::memcpy(address.sun_path, socket_.constData(), socket_.size());
    socklen_t length = offsetof(sockaddr_un, sun_path) + socket_.size();
    if(socket_[0] == '@')
    {
        address.sun_path[0] = '\0';  // abstract namespace: the name without a trailing NUL
    }
    else
    {
        length += 1;
    }

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if(fd < 0)
    {
        return false;
    }
    const ssize_t sent = ::sendto(fd, state.constData(), state.size(), MSG_NOSIGNAL,
                                  reinterpret_cast<const sockaddr*>(&address), length);
    const int error = errno;
    ::close(fd);
    if(sent != state.size())
    {
        OPENAUTO_LOG(warning) << "[ServiceNotifier] sending to " << socket_.toStdString()
                              << " failed: " << std::strerror(error);
        return false;
    }
    return true;
}

}
}
}
}
