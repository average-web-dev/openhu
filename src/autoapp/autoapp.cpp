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

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLoggingCategory>
#include <QNetworkInterface>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>
#include <aasdk/USB/USBHub.hpp>
#include <aasdk/USB/ConnectedAccessoriesEnumerator.hpp>
#include <aasdk/USB/AccessoryModeQueryChain.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/TCP/TCPWrapper.hpp>
#include <boost/log/utility/setup.hpp>
#include <f1x/openauto/autoapp/App.hpp>
#include <f1x/openauto/autoapp/Configuration/Configuration.hpp>
#include <f1x/openauto/autoapp/Service/AndroidAutoEntityFactory.hpp>
#include <f1x/openauto/autoapp/Service/ServiceFactory.hpp>
#include <f1x/openauto/autoapp/Api/ApiServer.hpp>
#include <f1x/openauto/autoapp/Api/CompanionBridge.hpp>
#include <f1x/openauto/autoapp/Api/ClientFilter.hpp>
#include <f1x/openauto/autoapp/Api/CoreMethods.hpp>
#include <f1x/openauto/autoapp/Audio/AudioGraph.hpp>
#include <f1x/openauto/autoapp/Audio/DuckingController.hpp>
#include <f1x/openauto/autoapp/Audio/VolumeController.hpp>
#include <f1x/openauto/autoapp/Core/ChildProcesses.hpp>
#include <f1x/openauto/autoapp/Core/EventBus.hpp>
#include <f1x/openauto/autoapp/Core/InstanceLock.hpp>
#include <f1x/openauto/autoapp/Core/LogLevel.hpp>
#include <f1x/openauto/autoapp/Core/ServiceNotifier.hpp>
#include <f1x/openauto/autoapp/Core/UsbReset.hpp>
#include <f1x/openauto/autoapp/Input/ActionDispatcher.hpp>
#include <f1x/openauto/autoapp/Service/NowPlaying.hpp>
#include <f1x/openauto/autoapp/Plugins/PluginManager.hpp>
#include <f1x/openauto/autoapp/Plugins/PluginMethods.hpp>
#include <f1x/openauto/autoapp/Plugins/PluginStore.hpp>
#include <f1x/openauto/autoapp/Shell/AaBridge.hpp>
#include <f1x/openauto/autoapp/Shell/AaSurface.hpp>
#include <f1x/openauto/autoapp/Shell/SettingsController.hpp>
#include <f1x/openauto/autoapp/Shell/SettingsMethods.hpp>
#include <f1x/openauto/autoapp/Shell/ShellController.hpp>
#include <f1x/openauto/autoapp/Shell/Translations.hpp>
#include <f1x/openauto/autoapp/System/SystemController.hpp>
#include <f1x/openauto/autoapp/System/SystemMethods.hpp>
#include <f1x/openauto/autoapp/Bluetooth/BluetoothManager.hpp>
#include <f1x/openauto/autoapp/Bluetooth/CarAudio.hpp>
#include <f1x/openauto/autoapp/Wireless/WirelessController.hpp>
#include <f1x/openauto/autoapp/Wireless/WirelessMethods.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace autoapp = f1x::openauto::autoapp;
using ThreadPool = std::vector<std::thread>;

void startUSBWorkers(boost::asio::io_service& ioService, libusb_context* usbContext, ThreadPool& threadPool)
{
    auto usbWorker = [&ioService, usbContext]() {
        timeval libusbEventTimeout{180, 0};

        while(!ioService.stopped())
        {
            libusb_handle_events_timeout_completed(usbContext, &libusbEventTimeout, nullptr);
        }
    };

    threadPool.emplace_back(usbWorker);
    threadPool.emplace_back(usbWorker);
    threadPool.emplace_back(usbWorker);
    threadPool.emplace_back(usbWorker);
}

void startIOServiceWorkers(boost::asio::io_service& ioService, ThreadPool& threadPool)
{
    auto ioServiceWorker = [&ioService]() {
        ioService.run();
    };

    threadPool.emplace_back(ioServiceWorker);
    threadPool.emplace_back(ioServiceWorker);
    threadPool.emplace_back(ioServiceWorker);
    threadPool.emplace_back(ioServiceWorker);
}

// Severity from OPENHU_LOG (trace, debug, info, warning, error, fatal), default info: debug lines come per
// Android Auto packet and flood the journal (journald then drops lines). openhu-logs.ini replaces it.
void configureLogging() {
    autoapp::core::setLogLevel("info");
    if (const char* level = std::getenv("OPENHU_LOG"); level != nullptr && *level != '\0') {
        // also changeable at runtime: Settings → Debug (debug.logLevel)
        if (!autoapp::core::setLogLevel(QString::fromLocal8Bit(level))) {
            OPENAUTO_LOG(warning) << "[OpenHU] OPENHU_LOG=" << level << " is not a log level, using info";
        }
    }

    const std::string logIni = "openhu-logs.ini";
    std::ifstream logSettings(logIni);
    if (logSettings.good()) {
        try {
            // For boost < 1.71 the severity types are not automatically parsed so lets register them.
            boost::log::register_simple_filter_factory<boost::log::trivial::severity_level>("Severity");
            boost::log::register_simple_formatter_factory<boost::log::trivial::severity_level, char>("Severity");
            boost::log::init_from_stream(logSettings);
        } catch (std::exception const & e) {
            OPENAUTO_LOG(warning) << "[OpenHU] " << logIni << " was provided but was not valid.";
        }
    }
}

int main(int argc, char* argv[])
{
    // first: what OpenHU inherited (cage's pipe) stays with OpenHU, its children do not get it
    autoapp::core::protectInheritedDescriptors();
    QElapsedTimer startup;
    startup.start();
    configureLogging();

    // before USB, the API port and the display: a second OpenHU ends here. Without a lock file (error) it starts
    // anyway, a head unit without its shell would be worse.
    autoapp::core::InstanceLock instanceLock;
    if(!instanceLock.acquire())
    {
        if(instanceLock.error().isEmpty())
        {
            const qint64 holder = instanceLock.holder();
            OPENAUTO_LOG(error) << "[AutoApp] OpenHU is already running"
                                << (holder > 0 ? " (pid " + std::to_string(holder) + ")" : std::string()) << ".";
            return 1;
        }
        OPENAUTO_LOG(warning) << "[AutoApp] No instance lock: " << instanceLock.error().toStdString();
    }

    libusb_context* usbContext;
    if(libusb_init(&usbContext) != 0)
    {
        OPENAUTO_LOG(error) << "[AutoApp] libusb_init failed.";
        return 1;
    }

    boost::asio::io_service ioService;
    boost::asio::io_service::work work(ioService);
    std::vector<std::thread> threadPool;
    startUSBWorkers(ioService, usbContext, threadPool);
    startIOServiceWorkers(ioService, threadPool);

    // Dark Material style for Qt Quick Controls in the shell and plugin pages, its sizes scaled with the display
    // (OpenHUStyle derives from Material, found in qrc:/shell/qml/style, Shell/qml/style/OpenHUStyle/README.md)
    qputenv("QT_QUICK_CONTROLS_STYLE", "OpenHUStyle");
    // its colours are set on the window (Main.qml)

    // On-screen keyboard for text fields (Qt Virtual Keyboard, Keyboard.qml); a real keyboard keeps working with it.
    // Not when another input method was chosen or the plugin is missing (the shell loads without the panel then).
    const bool onScreenKeyboard = qEnvironmentVariableIsEmpty("QT_IM_MODULE")
        && QFile::exists(QLibraryInfo::path(QLibraryInfo::PluginsPath)
                         + "/platforminputcontexts/libqtvirtualkeyboardplugin.so");
    if(onScreenKeyboard)
    {
        qputenv("QT_IM_MODULE", "qtvirtualkeyboard");
    }
    // word suggestions need hunspell dictionaries, which a head unit does not have
    QLoggingCategory::setFilterRules("qt.virtualkeyboard.hunspell.warning=false");

    QGuiApplication application(argc, argv);
    // systemd (OpenHU OS, #129): READY=1 once the shell is up, then watchdog pings from the GUI thread. Takes
    // NOTIFY_SOCKET out of the environment before the first child process starts.
    autoapp::core::ServiceNotifier serviceNotifier;

    auto configuration = std::make_shared<autoapp::configuration::Configuration>();
    autoapp::shell::AaBridge::instance().configure(configuration);

    // before anything produces text: the language of the shell and of the plugins (General.Language)
    autoapp::shell::Translations translations;
    translations.setPluginsDir(autoapp::plugins::PluginManager::pluginsDir());
    translations.setLanguage(QString::fromStdString(configuration->getLanguage()));

    autoapp::shell::SettingsController settingsController(configuration, argc, argv);
    settingsController.applyCursor();

    // Android Auto (USB and wireless via TCP)
    aasdk::tcp::TCPWrapper tcpWrapper;
    aasdk::usb::USBWrapper usbWrapper(usbContext);
    aasdk::usb::AccessoryModeQueryFactory queryFactory(usbWrapper, ioService);
    aasdk::usb::AccessoryModeQueryChainFactory queryChainFactory(usbWrapper, ioService, queryFactory);
    autoapp::service::ServiceFactory serviceFactory(ioService, configuration);
    autoapp::service::AndroidAutoEntityFactory androidAutoEntityFactory(ioService, configuration, serviceFactory);

    auto usbHub(std::make_shared<aasdk::usb::USBHub>(usbWrapper, ioService, queryChainFactory));
    auto connectedAccessoriesEnumerator(std::make_shared<aasdk::usb::ConnectedAccessoriesEnumerator>(usbWrapper, ioService, queryChainFactory));
    auto app = std::make_shared<autoapp::App>(ioService, usbWrapper, tcpWrapper, androidAutoEntityFactory, std::move(usbHub), std::move(connectedAccessoriesEnumerator));

    autoapp::shell::ShellController shellController;
    autoapp::audio::VolumeController volumeController;
    // radio and other local sources lower while Android Auto plays a prompt or the Assistant (#93)
    autoapp::audio::DuckingController duckingController([configuration]() { return configuration->duckLevel(); });
    duckingController.start();
    // media.current: what plays now (home page, Bluetooth car audio)
    autoapp::service::NowPlaying nowPlaying;
    autoapp::input::ActionDispatcher actionDispatcher(volumeController, shellController);

    // WebSocket API for external tools and plugins (docs/api.md)
    autoapp::api::ApiServer apiServer;
    autoapp::api::registerCoreMethods(apiServer, volumeController, shellController, actionDispatcher);
    autoapp::shell::registerSettingsMethods(apiServer, settingsController);
    // token for clients from the network and browsers (openhu#23), created at the first start
    if(configuration->getApiToken().empty())
    {
        configuration->setApiToken(autoapp::api::generateToken().toStdString());
        configuration->save();
    }
    const auto applyApiAccess = [configuration, &apiServer]() {
        apiServer.setToken(QString::fromStdString(configuration->getApiToken()));
        apiServer.setAllowedOrigins(
            autoapp::api::parseOrigins(QString::fromStdString(configuration->getApiAllowedOrigins())));
    };
    applyApiAccess();
    QObject::connect(&settingsController, &autoapp::shell::SettingsController::apiAccessChanged, &apiServer,
                     applyApiAccess);
    apiServer.listen(autoapp::api::listenAddress(QString::fromStdString(configuration->getApiListenAddress())),
                     configuration->getApiPort());
    // the API over Bluetooth for the companion app on the Android Auto phone (openhu#158)
    autoapp::api::CompanionBridge companionBridge(configuration->getApiPort(), [configuration]() {
        return QString::fromStdString(configuration->getApiToken());
    });
    companionBridge.start();

    // Plugins (~/.openhu/plugins) and the store (~/.openhu/sources.list), docs/plugins.md
    autoapp::plugins::PluginManager pluginManager(configuration->getApiPort());
    autoapp::plugins::PluginStore pluginStore(pluginManager);
    autoapp::plugins::registerPluginMethods(apiServer, pluginManager, pluginStore);
    autoapp::plugins::QmlApi qmlApi(apiServer);

    // Every source of sound plays into its own PipeWire sink with its own level (#154): Android Auto's outputs and
    // the sources the plugins declare. Plugin processes get their sinks in the environment.
    autoapp::audio::AudioGraph audioGraph(configuration->sourceLevels(),
                                          [configuration](const QHash<QString, int>& levels) {
                                              configuration->setSourceLevels(levels);
                                              configuration->save();
                                          });
    autoapp::api::registerAudioMethods(apiServer, audioGraph);
    pluginManager.setEnvironmentProvider([](const QString& id, const QVariantMap& manifest) {
        return autoapp::audio::sinkEnvironment(autoapp::audio::pluginSources(id, manifest));
    });
    const auto updateAudioSources = [&pluginManager, &audioGraph]() {
        QHash<QString, QList<autoapp::audio::AudioSource>> sources;
        const auto manifests = pluginManager.activeManifests();
        for(auto it = manifests.cbegin(); it != manifests.cend(); ++it)
        {
            sources.insert(it.key(), autoapp::audio::pluginSources(it.key(), it.value()));
        }
        audioGraph.setPluginSources(sources);
    };
    QObject::connect(&pluginManager, &autoapp::plugins::PluginManager::pluginsChanged, &audioGraph, updateAudioSources);
    audioGraph.start();
    pluginManager.load();
    updateAudioSources();

    // Wireless Android Auto: Bluetooth (BlueZ) + WiFi hotspot (NetworkManager)
    autoapp::bt::BluetoothManager bluetoothManager(configuration);
    // Known WiFi networks while the hotspot does not need the card (online updates, remote management)
    autoapp::wireless::WifiClient wifiClient;
    autoapp::wireless::WirelessController wirelessController(configuration, bluetoothManager, wifiClient);
    // Sound to a Bluetooth car radio, its buttons control Android Auto
    autoapp::bt::CarAudio carAudio(configuration, bluetoothManager, actionDispatcher, volumeController);
    autoapp::wireless::registerWirelessMethods(apiServer, wirelessController, wifiClient, bluetoothManager, carAudio);

    // OpenHU OS: system update and ssh (Settings -> System)
    autoapp::system::SystemController systemController(shellController.osVersion());
    autoapp::system::registerSystemMethods(apiServer, systemController);

    // the wireless hotspot is for the phone; API devices there only when allowed
    // plugin processes log in with their token: their values count as the plugin's (#123)
    apiServer.setPluginResolver([&pluginManager](const QString& token) { return pluginManager.pluginForToken(token); });
    apiServer.setClientFilter([configuration, &wirelessController](const QHostAddress& peer) {
        autoapp::api::HotspotNetwork hotspot;
        hotspot.subnet = QString::fromStdString(configuration->getWirelessSettings().address);
        hotspot.interfaceName = wirelessController.hotspotInterface();
        if(!hotspot.interfaceName.isEmpty())
        {
            hotspot.addresses = QNetworkInterface::interfaceFromName(hotspot.interfaceName).addressEntries();
        }
        const bool blockHotspot = configuration->getWirelessProjectionEnabled() && !configuration->getApiAllowHotspot();
        return autoapp::api::clientAllowed(peer, blockHotspot, hotspot);
    });

    // QML shell: Android Auto inside the main window next to the rest of the UI
    QQmlApplicationEngine engine;
    translations.setEngine(&engine);
    QObject::connect(&settingsController, &autoapp::shell::SettingsController::languageChanged,
                     &translations, &autoapp::shell::Translations::setLanguage);
    // a plugin installed or updated may bring a translation
    QObject::connect(&pluginManager, &autoapp::plugins::PluginManager::pluginsChanged,
                     &translations, &autoapp::shell::Translations::reload);

    engine.addImportPath(QStringLiteral("qrc:/shell/qml/style"));
    // QML module for the shell and plugin pages: import OpenHU 1.0
    qmlRegisterType<autoapp::shell::AaSurface>("OpenHU", 1, 0, "AaSurface");
    qmlRegisterSingletonType(QUrl("qrc:/shell/qml/module/Theme.qml"), "OpenHU", 1, 0, "Theme");
    qmlRegisterType(QUrl("qrc:/shell/qml/BusTopic.qml"), "OpenHU", 1, 0, "BusTopic");

    engine.rootContext()->setContextProperty("aaBridge", &autoapp::shell::AaBridge::instance());
    engine.rootContext()->setContextProperty("eventBus", &autoapp::core::EventBus::instance());
    engine.rootContext()->setContextProperty("shell", &shellController);
    engine.rootContext()->setContextProperty("settings", &settingsController);
    engine.rootContext()->setContextProperty("volume", &volumeController);
    engine.rootContext()->setContextProperty("audioSources", &audioGraph);
    engine.rootContext()->setContextProperty("actions", &actionDispatcher);
    engine.rootContext()->setContextProperty("plugins", &pluginManager);
    engine.rootContext()->setContextProperty("store", &pluginStore);
    engine.rootContext()->setContextProperty("api", &qmlApi);
    engine.rootContext()->setContextProperty("wireless", &wirelessController);
    engine.rootContext()->setContextProperty("wifi", &wifiClient);
    engine.rootContext()->setContextProperty("bluetooth", &bluetoothManager);
    engine.rootContext()->setContextProperty("carAudio", &carAudio);
    engine.rootContext()->setContextProperty("system", &systemController);
    engine.rootContext()->setContextProperty("onScreenKeyboardAvailable",
                                             qgetenv("QT_IM_MODULE") == "qtvirtualkeyboard");

    // Clean up what outlives the process (hotspot, plugin processes), then end without running destructors:
    // exit() from inside the event loop tears down Qt's globals while the QML engine still exists (crash).
    auto shutdown = [&]() {
        // before the plugins end: the audio server remembers the volume a stream had at its end
        duckingController.restoreNow();
        carAudio.stop();
        wirelessController.stop();
        pluginManager.stopAll();
        audioGraph.stop();
        volumeController.stop();
        configuration->save();
        std::fflush(nullptr);
        std::_Exit(0);
    };
    QObject::connect(&shellController, &autoapp::shell::ShellController::quitRequested, shutdown);

    // SIGTERM (systemd, docker stop) and SIGINT: same clean shutdown, handled in the GUI thread
    static int signalPipe[2];
    ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, signalPipe);
    for(int sig: {SIGTERM, SIGINT})
    {
        std::signal(sig, [](int) { [[maybe_unused]] auto written = ::write(signalPipe[0], "x", 1); });
    }
    QSocketNotifier signalNotifier(signalPipe[1], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, shutdown);

    QObject::connect(&settingsController, &autoapp::shell::SettingsController::wirelessChanged,
                     &wirelessController, &autoapp::wireless::WirelessController::apply);
    QObject::connect(&settingsController, &autoapp::shell::SettingsController::carAudioChanged,
                     &carAudio, &autoapp::bt::CarAudio::apply);
    // a Bluetooth adapter plugged in later: the car radio connects then
    QObject::connect(&bluetoothManager, &autoapp::bt::BluetoothManager::presenceChanged, &carAudio,
                     &autoapp::bt::CarAudio::apply);
    // selecting or forgetting a radio switches car audio on/off
    QObject::connect(&carAudio, &autoapp::bt::CarAudio::changed, &settingsController,
                     &autoapp::shell::SettingsController::refresh);
    app->setPreferUsb(configuration->getWirelessSettings().priority != "wifi");
    QObject::connect(&settingsController, &autoapp::shell::SettingsController::wirelessChanged, [&]() {
        app->setPreferUsb(configuration->getWirelessSettings().priority != "wifi");
    });
    QObject::connect(&shellController, &autoapp::shell::ShellController::restartRequested,
                     &settingsController, &autoapp::shell::SettingsController::restart);
    QObject::connect(&settingsController, &autoapp::shell::SettingsController::aboutToRestart, [&]() {
        // first: stopping the plugins below can take seconds
        serviceNotifier.prepareRestart();
        duckingController.restoreNow();
        carAudio.stop();
        wirelessController.stop();
        pluginManager.stopAll();
        audioGraph.stop();
        volumeController.stop();
    });

    // Keyboard -> actions (keymap.json)
    actionDispatcher.installKeyboardHandler();

    pluginManager.setQmlCacheClearer([&engine]() { autoapp::plugins::PluginManager::dropQmlCache(engine); });

    engine.load(QUrl(QStringLiteral("qrc:/shell/qml/Main.qml")));
    if(engine.rootObjects().isEmpty())
    {
        OPENAUTO_LOG(error) << "[AutoApp] Failed to load the QML shell.";
        std::exit(1); // worker threads are still running, a plain return would abort
    }

    if(auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first()))
    {
        if(QScreen* screen = QGuiApplication::primaryScreen())
        {
            window->setGeometry(screen->geometry());
            OPENAUTO_LOG(info) << "[AutoApp] Display " << screen->geometry().width() << "x" << screen->geometry().height()
                               << ", shell loaded after " << startup.elapsed() << " ms";
        }
        window->showFullScreen();
    }

    // A phone left in accessory mode by a previous run (restart, crash) still runs its old session and answers
    // with data the new session cannot decrypt: reset it, it reconnects from scratch.
    // OPENHU_AA_SERVER=host:port: Android Auto's head unit server instead of USB/wireless (development, openhu#158)
    const QString aaServer = qEnvironmentVariable("OPENHU_AA_SERVER");
    if(!aaServer.isEmpty())
    {
        const int colon = aaServer.lastIndexOf(':');
        const QString host = colon > 0 ? aaServer.left(colon) : aaServer;
        const auto port = static_cast<unsigned short>(colon > 0 ? aaServer.mid(colon + 1).toUInt() : 5277);
        OPENAUTO_LOG(info) << "[AutoApp] Android Auto through the head unit server " << host.toStdString() << ":" << port;
        app->setAndroidAutoServer(host.toStdString(), port);
    }
    else if(!autoapp::core::resetAndroidUsbDevices(true).isEmpty())
    {
        OPENAUTO_LOG(info) << "[AutoApp] phone was still in accessory mode, reset it";
    }
    app->waitForUSBDevice();
    wirelessController.apply();
    carAudio.apply();

    // Qt ends the process with exit() from its event thread when the display goes away (window closed, X11/Wayland
    // server gone). The hotspot goes down there too, then the process ends without static destructors: the threads
    // of an Android Auto session still log and crashed in the destroyed boost::log core (SIGSEGV), and QML read
    // destroyed context objects. Registered last, so it runs before every static destructor; systemd/cage restart
    // OpenHU.
    static autoapp::wireless::WirelessController* wirelessAtExit = nullptr;
    static autoapp::bt::CarAudio* carAudioAtExit = nullptr;
    static autoapp::audio::DuckingController* duckingAtExit = nullptr;
    wirelessAtExit = &wirelessController;
    carAudioAtExit = &carAudio;
    duckingAtExit = &duckingController;
    std::atexit([]() {
        if(wirelessAtExit == nullptr)
        {
            return;  // regular end of main()
        }
        if(duckingAtExit) duckingAtExit->restoreNow();
        if(carAudioAtExit) carAudioAtExit->stop();
        wirelessAtExit->stop();
        std::fflush(nullptr);
        std::_Exit(1);
    });

    // the event loop runs: the shell is up
    QTimer::singleShot(0, &serviceNotifier, &autoapp::core::ServiceNotifier::ready);

    auto result = application.exec();
    carAudio.stop();
    wirelessController.stop();
    duckingController.restoreNow();
    carAudioAtExit = nullptr;
    duckingAtExit = nullptr;
    wirelessAtExit = nullptr;

    std::for_each(threadPool.begin(), threadPool.end(), std::bind(&std::thread::join, std::placeholders::_1));

    libusb_exit(usbContext);
    return result;
}
