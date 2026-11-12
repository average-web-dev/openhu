/*
*  This file is part of OpenHU, based on the openauto project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
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

#include <atomic>
#include <chrono>
#include <functional>
#include <QStringList>
#include <boost/asio/steady_timer.hpp>

#include <aasdk/USB/IUSBHub.hpp>
#include <aasdk/USB/IConnectedAccessoriesEnumerator.hpp>
#include <aasdk/USB/USBWrapper.hpp>
#include <aasdk/TCP/ITCPWrapper.hpp>
#include <aasdk/TCP/ITCPEndpoint.hpp>
#include <f1x/openauto/autoapp/Service/IAndroidAutoEntityEventHandler.hpp>
#include <f1x/openauto/autoapp/Service/IAndroidAutoEntityFactory.hpp>
#include <f1x/openauto/autoapp/Core/UsbWatchdog.hpp>

namespace f1x
{
namespace openauto
{
namespace autoapp
{

class App: public service::IAndroidAutoEntityEventHandler, public std::enable_shared_from_this<App>
{
public:
    typedef std::shared_ptr<App> Pointer;

    // wirelessPort: TCP port for wireless Android Auto (0: any free port, for tests)
    App(boost::asio::io_service& ioService, aasdk::usb::USBWrapper& usbWrapper, aasdk::tcp::ITCPWrapper& tcpWrapper, service::IAndroidAutoEntityFactory& androidAutoEntityFactory,
        aasdk::usb::IUSBHub::Pointer usbHub, aasdk::usb::IConnectedAccessoriesEnumerator::Pointer connectedAccessoriesEnumerator,
        unsigned short wirelessPort = 5000);

    void waitForUSBDevice();
    void start(aasdk::tcp::ITCPEndpoint::SocketPointer socket);
    void stop();
    void pause();
    void resume();
    void onAndroidAutoQuit() override;
    // Ends a session over TCP (wireless or the head unit server) so that it starts again: the wireless controller
    // brings the phone back over Bluetooth, the head unit server is connected again after 3 s. False when there is
    // none (a USB session is restarted by a USB reset instead). Any thread.
    bool endNetworkSession();
    bool disableAutostartEntity = false;
    // Wireless.Priority: true = USB session wins over wireless, false = wireless replaces the USB session
    void setPreferUsb(bool value) { preferUsb_ = value; }

    // USB watchdog (openhu#80): phones on the cable (core::androidUsbDevices) are checked every interval; without a
    // session for the watchdog's timeout the USB wait is armed again. Call before waitForUSBDevice() (tests).
    void setUsbWatchdog(std::function<QStringList()> probe, std::chrono::steady_clock::duration interval,
                        core::UsbWatchdog watchdog);
    // delay before the USB wait is armed again after the hub failed (default 5 s)
    void setUsbRetryDelay(std::chrono::steady_clock::duration delay) { usbRetryDelay_ = delay; }
    // delay after a session end before the phones still on USB are reported again (default 3 s, openhu#92)
    void setUsbReportDelay(std::chrono::steady_clock::duration delay) { usbReportDelay_ = delay; }
    // Development (openhu#158): connect to Android Auto's "head unit server" (developer settings, port 5277, reached
    // through `adb forward tcp:5277 tcp:5277`) instead of waiting for a phone. No USB wait then: switching the phone
    // to accessory mode would end its USB debugging. Call before waitForUSBDevice().
    void setAndroidAutoServer(const std::string& host, unsigned short port)
    {
        serverHost_ = host;
        serverPort_ = port;
    }

private:
    using std::enable_shared_from_this<App>::shared_from_this;
    void waitForDevice();
    // Every way out of the USB wait ends here again, unless a USB session started (onAndroidAutoQuit arms it then).
    // reregister: new hotplug registration, which reports the connected devices again (LIBUSB_HOTPLUG_ENUMERATE)
    void armUsbWait(bool reregister, const char* reason);
    void aoapDeviceHandler(aasdk::usb::DeviceHandle deviceHandle);
    void onUSBHubError(unsigned generation, const aasdk::error::Error& error);
    void scheduleUsbWatchdog();
    void checkUsbWatchdog();

    boost::asio::io_service& ioService_;
    aasdk::usb::USBWrapper& usbWrapper_;
    aasdk::tcp::ITCPWrapper& tcpWrapper_;
    boost::asio::io_service::strand strand_;
    service::IAndroidAutoEntityFactory& androidAutoEntityFactory_;
    aasdk::usb::IUSBHub::Pointer usbHub_;
    aasdk::usb::IConnectedAccessoriesEnumerator::Pointer connectedAccessoriesEnumerator_;
    boost::asio::ip::tcp::acceptor acceptor_;
    // a session over TCP runs (read by endNetworkSession from other threads)
    std::atomic<bool> networkSession_{false};
    service::IAndroidAutoEntity::Pointer androidAutoEntity_;
    bool isStopped_;
    // the running session uses the cable
    bool usbSession_ = false;
    // phone on the cable while wireless is possible: the cable wins (charging, latency)
    std::atomic<bool> preferUsb_{true};
    // counts armUsbWait(): errors of a replaced wait (OPERATION_ABORTED) are not a reason to arm again
    unsigned usbWaitGeneration_ = 0;
    boost::asio::steady_timer usbRetryTimer_;
    std::chrono::steady_clock::duration usbRetryDelay_ = std::chrono::seconds(5);
    boost::asio::steady_timer usbReportTimer_;
    std::chrono::steady_clock::duration usbReportDelay_ = std::chrono::seconds(3);
    boost::asio::steady_timer usbWatchdogTimer_;
    std::string serverHost_;
    unsigned short serverPort_ = 0;
    boost::asio::steady_timer serverTimer_;
    void connectToServer(std::chrono::steady_clock::duration delay);
    std::function<QStringList()> usbDeviceProbe_;
    std::chrono::steady_clock::duration usbWatchdogInterval_ = std::chrono::seconds(5);
    core::UsbWatchdog usbWatchdog_;
    QStringList usbWatchdogDevices_;

    void startServerSocket();

    void handleNewClient(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const boost::system::error_code &err);
};

}
}
}
