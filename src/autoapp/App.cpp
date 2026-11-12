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

#include <fcntl.h>
#include <thread>
#include <aasdk/USB/AOAPDevice.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <f1x/openauto/autoapp/App.hpp>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
#include <f1x/openauto/autoapp/Core/UsbReset.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x::openauto::autoapp {

  App::App(boost::asio::io_service &ioService, aasdk::usb::USBWrapper &usbWrapper, aasdk::tcp::ITCPWrapper &tcpWrapper,
           service::IAndroidAutoEntityFactory &androidAutoEntityFactory,
           aasdk::usb::IUSBHub::Pointer usbHub,
           aasdk::usb::IConnectedAccessoriesEnumerator::Pointer connectedAccessoriesEnumerator,
           unsigned short wirelessPort)
      : ioService_(ioService), usbWrapper_(usbWrapper), tcpWrapper_(tcpWrapper), strand_(ioService_),
        androidAutoEntityFactory_(androidAutoEntityFactory), usbHub_(std::move(usbHub)),
        connectedAccessoriesEnumerator_(std::move(connectedAccessoriesEnumerator)),
        acceptor_(ioService, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), wirelessPort)), isStopped_(false),
        usbRetryTimer_(ioService), usbReportTimer_(ioService), usbWatchdogTimer_(ioService), serverTimer_(ioService), usbDeviceProbe_(&core::androidUsbDevices) {
      // not inherited by child processes (plugins, pactl), so a restart can bind the port again
      ::fcntl(acceptor_.native_handle(), F_SETFD, FD_CLOEXEC);

  }

  void App::setUsbWatchdog(std::function<QStringList()> probe, std::chrono::steady_clock::duration interval,
                           core::UsbWatchdog watchdog) {
    usbDeviceProbe_ = std::move(probe);
    usbWatchdogInterval_ = interval;
    usbWatchdog_ = watchdog;
  }

  void App::waitForUSBDevice() {
    strand_.dispatch([this, self = this->shared_from_this()]() {
      try {
        this->waitForDevice();
      }
      catch (...) {
        OPENAUTO_LOG(error) << "[App] waitForUSBDevice() -exception caused by this->waitForDevice();";
      }
      // No ConnectedAccessoriesEnumerator here any more: the hub's registration (LIBUSB_HOTPLUG_ENUMERATE) already
      // sends the accessory mode requests to every connected phone. Both at once sent two interleaved request
      // chains to the same phone at startup (openhu#80).
      this->scheduleUsbWatchdog();
    });
  }

  void App::start(aasdk::tcp::ITCPEndpoint::SocketPointer socket) {
    strand_.dispatch([this, self = this->shared_from_this(), socket = std::move(socket)]() mutable {
      OPENAUTO_LOG(info) << "Start from socket";
      if (androidAutoEntity_ != nullptr && usbSession_ && preferUsb_) {
        OPENAUTO_LOG(info) << "[App] phone is on the cable (USB priority), wireless connection refused";
        boost::system::error_code ec;
        socket->close(ec);
        startServerSocket();
        return;
      }
      if (androidAutoEntity_ != nullptr) {
//            tcpWrapper_.close(*socket);
//            OPENAUTO_LOG(warning) << "[App] android auto entity is still running.";
//            return;
        try {
          androidAutoEntity_->stop();
        } catch (...) {
          OPENAUTO_LOG(error) << "[App] onAndroidAutoQuit: exception caused by androidAutoEntity_->stop();";
        }
        try {
          androidAutoEntity_.reset();
        } catch (...) {
          OPENAUTO_LOG(error) << "[App] onAndroidAutoQuit: exception caused by androidAutoEntity_.reset();";
        }
      }

      try {
//            usbHub_->cancel();
//            connectedAccessoriesEnumerator_->cancel();

        auto tcpEndpoint(std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper_, std::move(socket)));
        androidAutoEntity_ = androidAutoEntityFactory_.create(std::move(tcpEndpoint));
        usbSession_ = false;
        networkSession_ = true;
        core::EventBus::instance().publish("projection.transport", {{"transport", "wifi"}});
        androidAutoEntity_->start(*this);
      }
      catch (const aasdk::error::Error &error) {
        OPENAUTO_LOG(error) << "[App] TCP AndroidAutoEntity create error: " << error.what();

        //androidAutoEntity_.reset();
        this->waitForDevice();
      }
    });
  }

  void App::stop() {
    strand_.dispatch([this, self = this->shared_from_this()]() {
      isStopped_ = true;
      networkSession_ = false;
      boost::system::error_code ec;
      usbRetryTimer_.cancel(ec);
      usbReportTimer_.cancel(ec);
      usbWatchdogTimer_.cancel(ec);
      try {
        connectedAccessoriesEnumerator_->cancel();
      } catch (...) {
        OPENAUTO_LOG(error) << "[App] stop: exception caused by connectedAccessoriesEnumerator_->cancel()";
      }
      try {
        usbHub_->cancel();
      } catch (...) {
        OPENAUTO_LOG(error) << "[App] stop: exception caused by usbHub_->cancel();";
      }

      if (androidAutoEntity_ != nullptr) {
        try {
          androidAutoEntity_->stop();
        } catch (...) {
          OPENAUTO_LOG(error) << "[App] stop: exception caused by androidAutoEntity_->stop();";
        }
        try {
          androidAutoEntity_.reset();
        } catch (...) {
          OPENAUTO_LOG(error) << "[App] stop: exception caused by androidAutoEntity_.reset();";
        }
      }
    });

  }

  void App::aoapDeviceHandler(aasdk::usb::DeviceHandle deviceHandle) {
    OPENAUTO_LOG(info) << "[App] Device connected.";

    if (androidAutoEntity_ != nullptr && !usbSession_ && preferUsb_) {
      // plugged in during a wireless session: switch to the cable
      OPENAUTO_LOG(info) << "[App] phone plugged in (USB priority), ending the wireless session";
      try {
        androidAutoEntity_->stop();
      } catch (...) {
        OPENAUTO_LOG(error) << "[App] aoapDeviceHandler: exception caused by androidAutoEntity_->stop();";
      }
      androidAutoEntity_.reset();
    }
    if (androidAutoEntity_ != nullptr) {
      OPENAUTO_LOG(warning) << "[App] android auto entity is still running.";
      // keep waiting: the hub drops every device while nobody waits
      this->armUsbWait(false, "a session is running");
      return;
    }

    try {
      // ignore autostart if exit to csng was used
      if (!disableAutostartEntity) {
        OPENAUTO_LOG(info) << "[App] Start Android Auto allowed - let's go.";
        connectedAccessoriesEnumerator_->cancel();

        auto aoapDevice(aasdk::usb::AOAPDevice::create(usbWrapper_, ioService_, deviceHandle));
        androidAutoEntity_ = androidAutoEntityFactory_.create(std::move(aoapDevice));
        usbSession_ = true;
        networkSession_ = false;
        core::EventBus::instance().publish("projection.transport", {{"transport", "usb"}});
        androidAutoEntity_->start(*this);
      } else {
        OPENAUTO_LOG(info) << "[App] Start Android Auto not allowed - skip.";
        this->armUsbWait(false, "autostart disabled");
      }
    }
    catch (const aasdk::error::Error &error) {
      OPENAUTO_LOG(error) << "[App] USB AndroidAutoEntity create error: " << error.what();

      androidAutoEntity_.reset();
      usbSession_ = false;
      this->armUsbWait(false, "the accessory could not be used");
    }
    catch (const std::exception &error) {
      // anything else would leave the hub without a waiting promise: USB stays deaf until a restart
      OPENAUTO_LOG(error) << "[App] USB AndroidAutoEntity create error: " << error.what();

      androidAutoEntity_.reset();
      usbSession_ = false;
      this->armUsbWait(false, "the accessory could not be used");
    }
  }

  void App::waitForDevice() {
    OPENAUTO_LOG(info) << "[App] Waiting for device...";

    startServerSocket();
    armUsbWait(false, "waiting for a device");
    connectToServer(std::chrono::seconds(0));
  }

  void App::connectToServer(std::chrono::steady_clock::duration delay) {
    if (serverPort_ == 0 || isStopped_) {
      return;
    }
    serverTimer_.expires_after(delay);
    serverTimer_.async_wait(strand_.wrap([this, self = this->shared_from_this()](const boost::system::error_code &ec) {
      if (ec || isStopped_ || androidAutoEntity_ != nullptr) {
        return;
      }
      auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioService_);
      boost::system::error_code error;
      const auto address = boost::asio::ip::make_address(serverHost_, error);
      if (error) {
        OPENAUTO_LOG(error) << "[App] head unit server: bad address " << serverHost_;
        return;
      }
      socket->async_connect(boost::asio::ip::tcp::endpoint(address, serverPort_),
                            strand_.wrap([this, self, socket](const boost::system::error_code &ec) {
        if (ec) {
          OPENAUTO_LOG(debug) << "[App] head unit server " << serverHost_ << ":" << serverPort_ << ": "
                              << ec.message() << ", trying again";
          this->connectToServer(std::chrono::seconds(3));
          return;
        }
        OPENAUTO_LOG(info) << "[App] connected to the head unit server " << serverHost_ << ":" << serverPort_;
        socket->set_option(boost::asio::ip::tcp::no_delay(true));
        this->start(socket);
      }));
    }));
  }

  void App::armUsbWait(bool reregister, const char *reason) {
    if (isStopped_ || serverPort_ != 0) {
      // with the head unit server the phone stays in its USB mode (debugging)
      return;
    }
    const auto generation = ++usbWaitGeneration_;
    OPENAUTO_LOG(info) << "[App] USB wait armed (" << reason << (reregister ? ", reporting connected devices" : "")
                       << ")";
    if (reregister) {
      // the hub registers its hotplug callback again with LIBUSB_HOTPLUG_ENUMERATE: connected phones are reported
      usbHub_->cancel();
    }

    auto promise = aasdk::usb::IUSBHub::Promise::defer(strand_);
    promise->then(std::bind(&App::aoapDeviceHandler, this->shared_from_this(), std::placeholders::_1),
                  [this, self = this->shared_from_this(), generation](const aasdk::error::Error &error) {
                    this->onUSBHubError(generation, error);
                  });
    // Not on our strand: usbHub_->start() runs inline and can take many seconds (hotplug enumeration opens every
    // USB device). Meanwhile a wireless phone would connect, the session could not start (it needs this strand)
    // and the phone gives up after 10 s.
    ioService_.post([hub = usbHub_, promise]() mutable { hub->start(std::move(promise)); });
  }

  void App::startServerSocket() {
    strand_.dispatch([this, self = this->shared_from_this()]() {
      OPENAUTO_LOG(info) << "startServerSocket() - Listening for WIFI Clients on Port 5000";
      auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioService_);
      acceptor_.async_accept(
          *socket,
          std::bind(&App::handleNewClient, this, socket, std::placeholders::_1)
      );
    });
  }

  void
  App::handleNewClient(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const boost::system::error_code &err) {
    OPENAUTO_LOG(info) << "handleNewClient() - Handle WIFI Client Connection";
    if (!err) {
      start(std::move(socket));
    }
  }

  void App::pause() {
    strand_.dispatch([this, self = this->shared_from_this()]() {
      OPENAUTO_LOG(info) << "[App] pause...";
      androidAutoEntity_->pause();
    });
  }

  void App::resume() {
    strand_.dispatch([this, self = this->shared_from_this()]() {
      if (androidAutoEntity_ != nullptr) {
        OPENAUTO_LOG(info) << "[App] resume...";
        androidAutoEntity_->resume();
      } else {
        OPENAUTO_LOG(info) << "[App] Ignore resume -> no androidAutoEntity_ ...";
      }
    });
  }

  bool App::endNetworkSession() {
    if (!networkSession_.exchange(false)) {
      return false;
    }
    OPENAUTO_LOG(info) << "[App] ending the network session to reconnect";
    this->onAndroidAutoQuit();
    return true;
  }

  void App::onAndroidAutoQuit() {
    strand_.dispatch([this, self = this->shared_from_this()]() {
      OPENAUTO_LOG(info) << "[App] onAndroidAutoQuit()";
      networkSession_ = false;

      //acceptor_.close();

      if (androidAutoEntity_ != nullptr) {
        try {
          androidAutoEntity_->stop();
        } catch (...) {
          OPENAUTO_LOG(error) << "[App] onAndroidAutoQuit: exception caused by androidAutoEntity_->stop();";
        }
        try {
          androidAutoEntity_.reset();
        } catch (...) {
          OPENAUTO_LOG(error) << "[App] onAndroidAutoQuit: exception caused by androidAutoEntity_.reset();";
        }
      }

      if (!isStopped_) {
        // The USB hub drops hotplug events while no promise is pending, so it waits again at once (a phone plugged
        // in now is found by its hotplug event). The phones that are plugged in already are reported a moment later
        // by re-registering the hotplug callback (LIBUSB_HOTPLUG_ENUMERATE): one that came back during the session
        // end (USB reset, AA ended on the phone) or stayed in accessory mode would otherwise only be found after
        // replugging. Not at once: while the phone is being unplugged its accessory device is still listed for a
        // moment, and each report started a new session on it that failed right away (openhu#92).
        try {
          OPENAUTO_LOG(info) << "[App] Waiting for device...";
          this->startServerSocket();
          this->armUsbWait(false, "session ended");
          this->connectToServer(std::chrono::seconds(3));
          const auto generation = usbWaitGeneration_;
          usbReportTimer_.expires_after(usbReportDelay_);
          usbReportTimer_.async_wait(strand_.wrap([this, self = this->shared_from_this(), generation](
              const boost::system::error_code &ec) {
            if (!ec && !isStopped_ && generation == usbWaitGeneration_ && androidAutoEntity_ == nullptr) {
              this->armUsbWait(true, "session ended, checking the phones on USB");
            }
          }));
        } catch (...) {
          OPENAUTO_LOG(error) << "[App] onAndroidAutoQuit: exception caused by this->waitForDevice();";
        }
      }
    });
  }

  void App::onUSBHubError(unsigned generation, const aasdk::error::Error &error) {
    if (generation != usbWaitGeneration_) {
      // replaced by a newer wait (start() rejects the old promise): nothing to do
      OPENAUTO_LOG(debug) << "[App] earlier USB wait ended: " << error.what();
      return;
    }
    if (isStopped_ || error == aasdk::error::ErrorCode::OPERATION_ABORTED) {
      OPENAUTO_LOG(info) << "[App] USB wait cancelled";
      return;
    }

    // e.g. the hotplug callback could not be registered: nobody would find a phone, so try again
    OPENAUTO_LOG(error) << "[App] onUSBHubError(): " << error.what() << ", waiting again in "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(usbRetryDelay_).count() << " ms";
    usbRetryTimer_.expires_after(usbRetryDelay_);
    usbRetryTimer_.async_wait(strand_.wrap([this, self = this->shared_from_this(), generation](
        const boost::system::error_code &ec) {
      if (!ec && generation == usbWaitGeneration_ && androidAutoEntity_ == nullptr) {
        this->armUsbWait(true, "the USB hub failed");
      }
    }));
  }

  void App::scheduleUsbWatchdog() {
    if (isStopped_ || !usbDeviceProbe_ || serverPort_ != 0) {
      return;
    }
    usbWatchdogTimer_.expires_after(usbWatchdogInterval_);
    usbWatchdogTimer_.async_wait(strand_.wrap([this, self = this->shared_from_this()](
        const boost::system::error_code &ec) {
      if (ec || isStopped_) {
        return;
      }
      this->checkUsbWatchdog();
      this->scheduleUsbWatchdog();
    }));
  }

  void App::checkUsbWatchdog() {
    const auto devices = usbDeviceProbe_();
    const bool session = androidAutoEntity_ != nullptr;
    const auto now = core::UsbWatchdog::Clock::now();
    if (devices != usbWatchdogDevices_) {
      usbWatchdogDevices_ = devices;
      OPENAUTO_LOG(debug) << "[App] USB watchdog: phones on USB: "
                          << (devices.isEmpty() ? std::string("none") : devices.join(", ").toStdString())
                          << (session ? " (session running)" : "");
    }

    switch (usbWatchdog_.update(devices, session, now)) {
      case core::UsbWatchdog::Action::Rearm:
        OPENAUTO_LOG(warning) << "[App] USB watchdog: " << devices.join(", ").toStdString()
                              << " on USB without an Android Auto session, arming the USB wait again (attempt "
                              << usbWatchdog_.attempts() << "/" << usbWatchdog_.maxAttempts() << ")";
        this->armUsbWait(true, "USB watchdog");
        break;
      case core::UsbWatchdog::Action::GiveUp:
        OPENAUTO_LOG(warning) << "[App] USB watchdog: still no session after " << usbWatchdog_.maxAttempts()
                              << " attempts, waiting until the phone is plugged in again";
        break;
      case core::UsbWatchdog::Action::None:
        break;
    }
  }

}


