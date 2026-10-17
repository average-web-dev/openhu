# OpenHU – Project status

As of 11.10.2026. Living document: updated whenever a phase is finished.
User documentation: `README.md` and `docs/` (installation, configuration, API, plugins, troubleshooting).

## Goal

Turn OpenAuto (opencardev) into an extensible head unit system, inspired by Hudiy:
Android Auto in the main window, API for vehicle/media data, plugins with a store, CAN bus,
steering wheel keys, radio, reverse camera. Project name **OpenHU** (the program is `openhu` since openhu#20; the
internal namespaces `f1x::openauto` and the sources in `src/autoapp/` stay, so upstream changes remain mergeable).

## Repositories and branches

| Repo (local) | Branch | Content |
|---|---|---|
| `openauto/` (GitHub `average-web-dev/openhu`) | `main` (current), `upstream-main` (old fork state) | OpenHU |
| `aasdk-ref/` (GitHub fork `average-web-dev/aasdk`, upstream `opencardev/aasdk` as remote `upstream`) | `openhu` | aasdk + navigation state/position, PhoneStatus |
| `openhu-plugins/` (GitHub `average-web-dev/openhu-plugins`) | `main` | official plugins + store index |

All three repos are pushed (OpenHU up to phase 6 + docs, aasdk with the USB filter). aasdk has its own
SemVer version (`project(aasdk VERSION ...)`); the workflow `openhu.yml` in the aasdk fork builds `libaasdk`/`libaasdk-dev`
for amd64 and arm64 and publishes them as a GitHub release on the tag `openhu-v<version>`. The OpenHU CI
(`.github/workflows/ci.yml`) fetches the newest release that fits `OPENHU_AASDK_VERSION`
(`scripts/fetch-aasdk.sh`), builds OpenHU for amd64 and arm64 (native arm runner), installs the packages for a test in a
fresh `debian:trixie-slim` and uploads the OpenHU and aasdk `.deb` as artifacts `openhu-amd64`/`openhu-arm64`.
Safety net (above all for the Qt 6 migration): unit tests with QtTest in `tests/` (EventBus, configuration,
actions/keymap, API incl. WebSocket, plugin store with a local repo), `qmllint` over the shell QML and
`tests/shell-smoke.sh`, which starts the installed shell headless, shows every page through the API and fails on QML
warnings. Locally: `docker/dev.sh test`. For this, all code except `main()` lives in the library `openhu_core`.

Qt 6.8 (branch `qt6`, issue #5): video through an own FFmpeg decoder (`Projection/FfmpegVideoOutput`, one
AA message = one `AVPacket`, newest frame to the `QVideoSink` of the `VideoOutput` in `AaPage.qml`, decoder chosen with
`Video.Decoder`), GStreamer is gone. Audio through `QAudioSink`/`QAudioSource`. Plugin API 2 (`api` in the manifest);
plugins of another API are neither installed nor loaded. Debian's Qt 6 has no eglfs: the service without a desktop
comes with #9 (kiosk compositor `cage`).

## Phases

| Phase | Content | Status |
|---|---|---|
| 0 | Build basis: local aasdk, QML/WebSockets, dev workflow in the container | done |
| 1 | QML shell, Android Auto in the main window (AaBridge/AaSurface, automatic margins) | done, tested with a phone |
| 2 | AA data (nav/media/phone) + WebSocket API, volume, night mode | done, tested with a phone |
| 3 | Actions (keyboard, steering wheel, API) → AA keys / shell | done (`N`, `B`, `M`, widgets → Maps/Spotify tested) |
| 4 | Plugin system + store (apt-like sources, sha256, install/update/remove) | done |
| 5 | Plugins CAN, steering wheel keys, reverse camera, radio | done; camera tested with a webcam, CAN/radio unit tests only |
| 7 | New settings (QML), remove the legacy UI and dead code | done (see below) |
| 6a | Wireless Android Auto (BlueZ + NetworkManager hotspot, built in) | done, tested with a Fairphone 4 |
| 6 | Bluetooth audio to the car radio (A2DP, AVRCP keys/title, calls by key), built in | done, tested with Galaxy Buds+ |
| – | Modernize the aasdk README with an AI note | done (aasdk `c7b91ed`) |
| – | Docs: new README (AI note, screenshots), installation, configuration, troubleshooting, plugin example | done |
| – | Bluetooth panel when tapping the BT symbol (radio, disconnect, settings) | later |
| – | arm64 build for the Raspberry Pi, aasdk as a package, test on a real Pi (eglfs) | at the very end |

Detailed plan: `~/.claude/plans/ich-h-tte-gene-folgenden-ethereal-lighthouse.md`.

## Important decisions

- QML shell instead of widgets; the old widgets UI is gone since phase 7.
- Plugins are hybrid: manifest + QML page/overlay + a process in any language that talks to the API.
- API: WebSocket + JSON, port 5520, localhost by default; clients from the network and browsers log in with a
  token (docs `docs/api.md`, plugins `docs/plugins.md`).
- Store packages are ready-made archives `dist/<id>-<version>.tar.gz` (instead of folders from the repo tarball),
  built reproducibly by `tools/openhu-repo-index`.
- Volume through `pactl` (PulseAudio and PipeWire) instead of libpulse.
- Navigation channel with type ENUM (the shell draws the arrows).
- Reverse camera: by default in the content area and in the dock, full screen optional. Android Auto is told with
  video focus `NATIVE` while the camera (any plugin overlay, or a plugin page that replaced Android Auto) covers it,
  and gets the gear (`vehicle.gear`) as a sensor (#76, not tested with a phone yet).
- Video flow control (#22): a video message is acknowledged only when the decoder takes it from its queue, so the
  phone (`max_unacked` 1) waits instead of OpenHU dropping frames. Dropping until the next IDR frame stays as a
  fallback, and acks held back longer than 500 ms are sent anyway. Simulated in `tests/test_video_ack.cpp`, not
  tested with a phone yet. Debugging knobs are in [troubleshooting.md](troubleshooting.md).
- Actions (`input.action`) and plugin commands are one-shot events (`bus.emit`), states
  are stored with their last value (`bus.publish`).
- The shell's texts are IDs in the code (`qsTrId`); every language, English too, is a translation file (`translations/`,
  [translations.md](translations.md)), switched at runtime.

## Development

```
openauto/docker/dev.sh run        # build incrementally (aasdk + OpenHU) and start in the Xephyr window
openauto/docker/dev.sh package    # .deb to docker/output/
openauto/docker/dev.sh reconnect  # reconnect the phone by USB reset
openauto/docker/dev.sh clean      # delete the build volumes and ccache
```

Configuration `docker/config/openhu.ini` (fallback `openauto.ini`), plugin data `docker/data/`, plugin repo as store
source `file /opt/openhu-plugins`.

## Known points / findings

- Without `gstreamer1.0-x` (X video sinks, otherwise only Recommends of `gstreamer1.0-plugins-good`) every
  `QMediaPlayer` under xcb takes ~10 s (Wayland probe with a timeout), the connection setup correspondingly longer.
- The aasdk USBHub drops hotplug events while no promise is waiting. After a session ends the phone often comes
  back exactly then (USB reset, AA ended on the phone) or stays in AOA mode without a new event; so
  `App::onAndroidAutoQuit()` resets the hub, and the new registration reports all plugged-in devices.
  `App::armUsbWait()` arms the hub again on every outcome that is not a USB session (hub error, device during a
  wireless session, unusable accessory). The USB watchdog (`core::UsbWatchdog`, every 5 s) arms it again with a
  new registration when a phone (accessory or Android vendor, sysfs) is on USB for 15 s without a session, at most
  3 times until the phone is unplugged or a session starts (openhu#80). The `ConnectedAccessoriesEnumerator` is not
  used any more: together with the hub's registration it sent two request chains to the same phone at startup.
- Status services have to call `channel_->receive()` in `start()`, otherwise OpenHU does not answer the
  channel opening and the phone sends no video (that was the cause of the black picture).
- In the container aasdk lives only in the volume under `/opt/aasdk` (`CPATH`/`LD_LIBRARY_PATH` in the image), not in
  `/usr/local`, otherwise `ldconfig` might bind an old library.
- The QML engine caches plugin files: before they are replaced, pages are released and the cache is trimmed;
  icons get their modification time as a URL parameter.
- The phone sends no signal strength (always 0) and no battery level over Android Auto.
- systemd service: `openhu.ini` is read from the working directory → `WorkingDirectory=/var/lib/openhu`
  (before, the file ended up under `/`).
- The `vcan0` test of the CAN plugin needs `sudo` on the host (see the plugin README).

## Phase 6 – Result (car radio)

- Built in instead of a plugin: `bt::BluetoothManager` (adapter, agent, pairing incl. PIN for old radios,
  search, devices) is shared by wireless AA and the radio; `bt::CarAudio` connects the radio
  (again every 20 s, the radio comes on with the ignition) and asks it for A2DP (`ConnectProfile`, radios connect
  hands-free first); `audio::OutputRouter` switches the card to A2DP, makes the sink the default output and moves
  the playing streams to it through `pactl`, and back on disconnect, car audio off and quit. Car audio off or
  another radio chosen leaves the device connected; only "forget" unpairs it. Sink names: new PipeWire
  `bluez_output.AA:BB:…`, old with `_`.
- AVRCP: own MPRIS player at BlueZ (`Media1.RegisterPlayer`), keys → actions (`bt::CallButtons`): while a call
  rings, play/play-pause answers it (from 2 s on); in a call pause/stop/play-pause hang up only once the call lasted
  5 s and its state did not change for 2 s (radios send pause/stop themselves when they switch to the phone, which
  ended every call); the display shows title/artist or the caller.
- Volume on the radio: AVRCP Absolute Volume through PipeWire → default output → `audio.volume`.
- Call audio goes to the radio by A2DP, the microphone is on the OpenHU device (decision: no HFP gateway).
- Status bar: Bluetooth symbol while the car radio is connected (the phone has its own symbol).
- Tested: pairing/connection/output, reconnect after a restart, keys and sound (Buds+). Call keys and
  title display on a device with a display still untested; wireless AA tested after the rework.

## Wireless Android Auto – Result

- Built into OpenHU (`src/autoapp/Wireless/`); the separate `btservice` with its systemd service and Qt Bluetooth
  is removed. Runs the same on bare metal and in the container: BlueZ and NetworkManager over the system D-Bus
  (container: `/run/dbus/system_bus_socket` mounted, `--privileged` because of AppArmor).
- Flow: BlueZ profile for the AA RFCOMM channel (UUID `4de17a00…`, **channel 8 fixed**, otherwise the
  RFCOMM entry is missing from the SDP record and the phone hangs at "Connecting to Android Auto") → the phone gets
  `WifiStartRequest` (10.42.0.1:5000) and on `WifiInfoRequest` SSID/password/BSSID (security mode 8) →
  the phone joins the hotspot `openhu-hotspot` (NM, IPv4 shared, 5 GHz channel 36, country with `iw reg set`) →
  TCP on port 5000.
- Phone connection: when the system already offers hands-free (PipeWire HFP), OpenHU connects the phone normally
  (`Device1.Connect`); an additional HSP profile next to it makes the phone refuse the connection. Without HFP
  (Pi without PipeWire) an own HSP profile, like the WiFi dongles do.
- Pairing: button "Pair" (visible for 2 min, code confirmation in a dialog, own BlueZ agent only during that
  time) or through the USB cable (the AA Bluetooth channel reports the address, OpenHU accepts exactly this phone
  and sends the code back over AA). The phone waits for OpenHU to start the pairing (`Pair()`);
  tested: paired in < 2 s without a question.
- Setting "Priority with cable" (`Wireless.Priority`): USB (default) – with a cable no WiFi data, TCP
  refused, plugging in ends the WiFi session; WiFi – the phone may switch to wireless also with a cable.
- Hotspot "Always on" / "On demand": on demand it starts when a paired phone opens the AA channel
  (the phone waits, WiFi data only afterwards; measured 0.3 s), and goes off after 2 min without a wireless session
  (switching off tested).
- Band Auto/5/2.4 GHz (Auto: 5 GHz, on an error 2.4 GHz; a card cannot do both at once),
  advanced: channel, address/gateway, DHCP range (NM `ipv4.shared-dhcp-range`, from NM 1.42). Own DNS servers
  do not work: NM does not allow `ipv4.dns` with `method=shared`.
- Start problems with the phone on the cable fixed: (1) a phone still stuck in accessory mode from the previous run
  is reconnected fresh by a USB reset at start (otherwise SSL error "unexpected message"); (2) aasdk asked
  every USB device by AOAP and slept 1 s per device ("VMware workaround") → 22 s until the phone; now a
  filter (hubs/HID/audio/video skipped), the pause removed (aasdk fork) → phone after 1.7 s.
- Crashes fixed: `SensorService` read GPS without a gpsd connection (uninitialized `gps_data_t`);
  on quit `_Exit` after cleaning up instead of `exit` (Qt teardown crash in the QML engine).
- API from the hotspot network refused by default, switch "API also in the WiFi hotspot" (untested).
- Hidden SSID tested and dropped: NM sets up the hotspot hidden, but the phone reports
  `STATUS_WIFI_NETWORK_UNAVAILABLE` – Android Auto does not actively search for hidden networks.
- Important fix in `App::waitForDevice`: the USB hub start ran inline on the app strand and blocked it
  for 10–20 s at program start (hotplug enumeration of all USB devices); the phone gave up after 10 s without data.
  Now an own worker call → wirelessly connected about 5 s after the start.
- When a wireless session ends, or no TCP connection comes within 30 s after the handshake, OpenHU disconnects
  Bluetooth and reconnects (the phone would not try again otherwise). Exception: AA ended on the phone
  (`ByeByeRequest` with `USER_SELECTION`) – then only when the phone starts by itself or through
  "Connect" (disconnects Bluetooth briefly, otherwise the phone does not restart AA).
- The hotspot is shut down on quit, SIGTERM (`docker stop`, the entrypoint passes it on) and when the window is closed
  (`atexit`).

## Phase 7 – Result

- Legacy UI removed (`MainWindow`, `SettingsWindow`, dialogs), as well as dead code: services radio/
  VendorExtension/MediaBrowser/GenericNotification/WifiProjection, `OMXVideoOutput`, `RecentAddressesList`,
  `tests/`, `buildenv/`, `Dockerfile.rootfs`, `helpers/`, `packaging/opt/crankshaft`, fix notes,
  unused icons (~32,500 lines).
- `Configuration` new and lean (same ini keys, file `openhu.ini`), `QGuiApplication` instead of
  `QApplication`, CMake without OMX/taglib/blkid/widgets/Crankshaft installation, udev rule without Crankshaft.
- Settings page in QML (`Shell/qml/settings/`) with the categories General, Android Auto, Audio, Bluetooth,
  Keys, Apps & Store, Network & API, Info. Every change is saved at once; a bar shows when a
  reconnect (USB reset by button) or a restart is needed.
- The apps menu is now the category "Apps & Store" (Open, In the dock, Active, Install/Updates/Sources).
- Restart in the same process (`execv`, same PID, works in the container and under systemd). Before that,
  plugin processes and `pactl subscribe` are ended; the wireless acceptor (port 5000) is
  close-on-exec, otherwise child processes inherit the socket and the new process gets "Address already in use".
- Key mapping editable (record, assign an action, remove, reset; only the differences end up in
  `~/.openhu/keymap.json`). In focused text fields all keys except media/volume go to the field.
- Android Auto:
  - The driver side is reported (was fixed to "right").
  - **Name suffix** (`[General] NameSuffix`): the phone shows "OpenHU <suffix>". The vehicle ID is a
    hash of the suffix, so several head units are configured separately on the phone; the same name stays
    the same car (without a suffix the previous ID).
  - **Automatic resolution** (`[Video] AutoResolution`, on by default): the smallest AA level that covers the AA
    area (480p/720p/1080p, in portrait 720x1280/1080x1920), plus automatic margins. AA only knows
    fixed levels; the choice applies with the next connection.
  - "Exit" in Android Auto → home page (`projection.exit`), night mode only through the API.
- Tested in the container: all categories without QML warnings, saving/loading values, learning keys,
  automatic resolution at 800x480/1280x720/1920x1080/720x1280, portrait layout, restart with a
  running plugin. Still to test with a phone: changed driver side/name suffix/resolution.
