# OpenHU API

WebSocket + JSON, default `ws://127.0.0.1:5520` (`openhu.ini`: `[Api] Port=5520`, `Listen=127.0.0.1`;
`Listen=0.0.0.0` for access from the network, `Listen=::` for IPv4 and IPv6).

## Messages

| Direction | Message |
|---|---|
| → | `{"type":"subscribe","topics":["nav.*","media.metadata"]}` – `*` = everything; the current value of each matching topic is sent right away |
| → | `{"type":"unsubscribe","topics":["nav.*"]}` |
| → | `{"type":"request","id":1,"method":"volume.set","params":{"value":40}}` |
| ← | `{"type":"response","id":1,"result":{...}}` or `{"type":"response","id":1,"error":"..."}` |
| ← | `{"type":"event","topic":"nav.turn","data":{...}}` |

Example: `websocat ws://127.0.0.1:5520` and then `{"type":"subscribe","topics":["*"]}`.

## Login (token)

Programs on the device itself (plugins, `openhu`, `websocat` on the head unit) need no login. Clients from the
network and browsers (every client that sends an `Origin` header, also on the device itself) have to send the API
token first. It is created at the first start (`openhu.ini`: `[Api] Token=`) and shown in Settings → Network
→ API token, which also makes a new one (clients logged in with the old one are disconnected). It cannot be read or
set through the API from the network.

The token goes with the handshake or as the first message:

| How | Example |
|---|---|
| Header | `Authorization: Bearer <token>` |
| URL | `ws://<head unit>:5520/?token=<token>` (browsers cannot set headers) |
| Message | → `{"type":"auth","token":"<token>"}` ← `{"type":"auth","authenticated":true}` |

Plugin processes log in with their own token: → `{"type":"auth","pluginToken":"<OPENHU_PLUGIN_TOKEN>"}` ←
`{"type":"auth","authenticated":true,"plugin":"<id>"}`. Their values then count as the plugin's (which matters for
Android Auto, see [Vehicle data](#vehicle-data-android-auto-sensors) and [plugins.md](plugins.md#android-auto)); a
wrong plugin token closes the connection.

Before the login every other message is answered with the error `authentication required`, and no events are
sent. A wrong token closes the connection (close code 1008), and so does no login within 10 seconds.

```sh
websocat -H "Authorization: Bearer $TOKEN" ws://192.168.1.50:5520
```

Browsers may connect from every origin by default (`[Api] AllowedOrigins=*`); a list such as
`http://192.168.1.10:8080,http://dashboard.local` allows only those, empty allows no browser at all. Browsers always
need the token and never get passwords (`secret` settings), not even on the device itself, so a web page cannot use
the API behind the user's back.

The connection is not encrypted (`ws://`): the token travels in clear text through the network, use the API only in
networks you trust.

## Topics (events)

| Topic | Data |
|---|---|
| `projection.state` | `connected` (bool) – Android Auto session running |
| `projection.video` | `active` (bool) – projection video playing |
| `projection.transport` | `transport` (`usb`, `wifi`) – how the current session is connected |
| `androidauto.mediaAudio` | `playing` (bool) – the phone streams music (Android Auto's media audio channel is open) |
| `nav.status` | `state`: `active`, `inactive`, `rerouting`, `unavailable` |
| `nav.turn` | `road`, `maneuver` (e.g. `turn_normal_left`, `roundabout_enter`, `destination`), `side` (`left`/`right`/`unspecified`), `roundaboutExit`, `turnAngle`, `image` (data URL, if the phone sends one), `lanes` (`[{directions:[...], highlighted}]`), `cue` |
| `nav.distance` | to the next maneuver: `meters`, `seconds`, `display` (as shown by the phone), `unit` (`meters`, `kilometers`, `miles`, …) |
| `nav.destination` | `address`, `meters`, `display`, `unit`, `seconds`, `eta` |
| `nav.road` | `name` – current road |
| `media.metadata` | `title`, `artist`, `album`, `playlist`, `duration` (s), `cover` (data URL), `source` |
| `media.status` | `state` (`playing`, `paused`, `stopped`), `app`, `position` (s), `shuffle`, `repeat`, `repeatOne`, `source` |
| `media.current` | `source`, `plugin` (the id of the plugin behind the source, empty for Android Auto), `metadata`, `status` – what plays now: the `media.metadata` and `media.status` of the source that started playing last (the one with the media keys), kept per source by OpenHU. `media.metadata` / `media.status` hold whatever source published last, so after a switch they show the new source's track only with its next one; the home page and Bluetooth car audio read `media.current`; a tap on the home page's card opens the plugin's page, for Android Auto its media app |
| `radio.state` / `radio.command` | of the radio plugin (`org.openhu.radio`, format in its `bin/radio.py`); OpenHU passes them to Android Auto's radio channel while the plugin runs (Settings → Android Auto → Radio, `Radio.AndroidAuto`): the plugin's stations become a media source in Android Auto, the phone's tune/seek/favorite/mute requests come back as `radio.command` |
| `radio.logos` | of the radio plugin: `fm`, `am`, `dab` – station logos (`{"96.2": data URL}`, DAB by service id `"0xD882"`, from image files the user put into the plugin's data dir, nothing from the internet), published only when they change; OpenHU scales them to 128×128 for the station list, favorites and current station in Android Auto |
| `radio.slide` | of the radio plugin: `band`, `station` (DAB service id), `image` (data URL, empty if none) – the DAB+ slideshow picture (MOT SLS) of the station that plays, from the broadcast; album art of the current station in Android Auto |
| `phone.status` | `callState` (`idle`, `incoming`, `in_call`, `on_hold`, …), `calls` (`[{state, duration, number, name, numberType, thumbnail}]`), `signal` |
| `audio.volume` | `volume` (0–100), `muted` |
| `audio.sources` | `sources` (list of `{id, name, textId, sink, plugin, pluginName, level, baseLevel, canPause, present}`: the audio sources with their sinks, `level`: the user's level in dB, relative to `baseLevel`, the plugin's normalization in dB (the chain plays at the sum), `present`: the sink exists), `min`, `max` (level range in dB, −20 and 0). See `audio.level.set` and [configuration.md](configuration.md#audio-sources) |
| `audio.duck` | `active`, `level` (loudness of local sources in %, 0 = muted, 100 = unchanged), `reason` (`transient`: audio focus GAIN_TRANSIENT, expected for the Assistant, muted; `may_duck`: GAIN_TRANSIENT_MAY_DUCK, expected for navigation prompts; `guidance`: another or no focus request; both lowered to Settings → Audio, `Audio.DuckLevel`; empty while inactive). While Android Auto plays on its guidance channel, OpenHU lowers the streams of other processes (e.g. the radio plugin's `pacat`) itself with `pactl`, with short fades, and restores them afterwards; plugins need not do anything, the topic is for UI hints |
| `system.daynight` | `night` (bool) – switches Android Auto to night mode while Settings → Android Auto → Night mode is "Automatic" |
| `system.nightMode` | `mode` (`auto`, `day`, `night`) – that setting (`General.NightMode`) |
| `input.action` | `action`, `phase` (`click`, `press`, `release`) – every triggered action (one-shot event, no stored value). Media keys (`media.*`) go to the current media source: to the phone while `media.status.source` is `androidauto` (or empty), otherwise only here, to the plugin that published `media.status` with its own `source` (e.g. `radio`). With `source` (the buttons of Android Auto's media card, see [Local media](#local-media-android-auto-media-card)) the action is meant for that source, also while another one owns the media keys |
| `ui.page` | `page` – page shown in the shell (`home`, `aa`, `settings`, `plugin:<id>`) |
| `ui.overlay` | `overlays` – ids of the plugins whose overlay is shown (`ui.overlay.show`) |
| `ui.language` | `language` (`de`, `en`), `available` – language of the shell; texts of plugin processes can follow it |
| `projection.exit` | – one-shot: "exit" inside Android Auto, the shell shows its home page |
| `vehicle.gear` | published by a plugin (e.g. the CAN plugin): `reverse` (bool) and/or `gear` (`park`, `reverse`, `neutral`, `drive` or a number 0–10; wins over `reverse`) → Android Auto `SENSOR_GEAR` (`reverse: false` without `gear` counts as drive). See [Vehicle data](#vehicle-data-android-auto-sensors) |
| `vehicle.speed`, `vehicle.parkingBrake`, `vehicle.drivingStatus`, `vehicle.fuel`, `vehicle.info`, `vehicle.odometer`, `vehicle.engine`, `vehicle.climate`, `vehicle.compass` | vehicle data from plugins or API clients, passed to Android Auto as sensors, see [Vehicle data](#vehicle-data-android-auto-sensors) |
| `debug.state` | `enabled` (debug mode), `videoOverlay` (#120) |
| `video.stats` | once a second while the debug video overlay is on: `codec`, `width`, `height`, `fps`, `decodeMs` (per frame), `decoder`, `hardware` |
| `vehicle.controls` | car controls for Android Auto's climate rail, from the plugin that declares `carControl`, see [Car controls](#car-controls-android-auto-climate-rail) |
| `vehicle.control.set`, `vehicle.control.result`, `vehicle.control.action` | events between OpenHU and that plugin, see [Car controls](#car-controls-android-auto-climate-rail) |
| `vehicle.simulate` | `state` (`park`, `drive`, `reverse`, empty = off) – a driving state for tests, through the API or Settings → Debug (`debug.simulate`, debug mode; the status bar then shows it) (`openhu-cli api bus.publish '{"topic": "vehicle.simulate", "data": {"state": "park"}}'`), kept until OpenHU ends: Android Auto gets its gear, parking brake, speed (0, 50 or 5 km/h) and driving status (parked: unrestricted, otherwise every restriction) instead of the vehicle data from plugins. Parking brake and speed are announced only when a state is set at the connect: set it, then reconnect the phone; afterwards it can be switched during the session |
| `plugins.state` | `plugins` – installed plugins (like `plugin.list`), on every change (load, enable, disable, dock) |
| `plugin.config.<id>` | `values`, `changed` (keys) – the [config store](plugins.md#config-store) of a plugin: when it is loaded and on every change (also from outside, also while it is disabled); declared settings with their default where nothing valid is stored |
| `store.state` | `busy`, `status`, `error` – progress of store operations |
| `wireless.state` | `enabled`, `wifiAvailable` (a WiFi card is there), `wifiInterface`, `bluetoothAvailable` (a Bluetooth adapter is there; wireless Android Auto needs both and starts by itself when one is plugged in later), `running`, `hotspot` (`off`, `starting`, `on`, `error`), `hotspotMode` (`always`, `demand`), `hotspotError`, `hotspotRoom` (the hotspot leaves the card to WiFi networks: `search`, `client`, empty), `searchUntil` (ms since 1970), `wifiOff`, `interface`, `channel`, `ssid`, `bluetooth` (`off`, `on`, `error`), `bluetoothError`, `phase` (`idle`, `waiting`, `hotspot`, `handshake`, `joining`, `connected`, `error`), `phaseText`, `device` |
| `wifi.state` | `available` (a WiFi card is there), `ready` (the cards are known), `enabled`, `devices` (WiFi cards), `client` (`ssid`, `ip`, `device`, `uuid`) while OpenHU is in a known WiFi network, `internet`, `networks` (saved ones, `uuid`, `ssid`, `priority`, `secured`, `hidden`, `active`, `joined` = connected at least once; highest priority first), `found` (after `wifi.scan`: `ssid`, `signal`, `security`, `secured`, `known`), `scanning`, `busy`, `message`, `hotspotDevice` (the card runs OpenHU's hotspot) |
| `bluetooth.pairing` | `active`, `device`, `passkey` – a device asks to pair (confirmed in the shell or with `bluetooth.confirm`) |
| `bluetooth.phone` | `address` – the Bluetooth address of the Android Auto phone, sent by the phone when the session starts (needs Bluetooth: wireless Android Auto or `Bluetooth.AdapterType` local) |
| `companion.state` | `connected`, `app`, `version`, `label` – the companion app on the phone ([below](#companion-app)) |
| `bluetooth.state` | `available` (an adapter is there, also before it is used), `error`, `adapter`, `pairing` (visible), `pairingDevice`, `pairingPasskey`, `pairingWith` (pairing started by OpenHU), `searching` |
| `caraudio.state` | `enabled`, `device`, `name`, `paired`, `connected`, `sink`, `error`, `status` – sound to the Bluetooth car radio |
| `system.update` | OpenHU OS only: `state` (`idle`, `checking`, `installing`, `done`, `uptodate`, `error`, `cancelled`), `phase` (`root`, `kernel`), `percent`, `from`, `version`, `error`, `source` (`usb`, `online`), after a check `checked`, `current`, `newest`, `available`, `source`, `checkError`; `usb` while a USB stick with OpenHU OS is plugged in (`version`, `newer`, `path`); `offer` – what `system.update.install` installs (`version`, `source`: the newer of the checked version and the stick's); `busy`, `blocked` (Android Auto connected) |
| `settings.changed` | `key`, `value` (not for passwords), `pending` (`reconnect`, `restart`) – a setting was changed (settings page or `settings.set`) |
| `system.ssh` | OpenHU OS only: `enabled`, `running`, `password` (set), `keys`, `busy`, `addresses` |
| `system.ethernet` | OpenHU OS only: `mode` (`client`, `server`), `address` (of OpenHU as the server, `10.42.1.1`), `serverAllowed` (false while another DHCP server gave a port an address), `devices` (`interface`, `state`: `connected`, `connecting`, `no-cable`, `disconnected`; `address`, `lease`, `dhcpServer`), `clients` (server: `ip`, `mac`, `name`), `busy` |
| `system.time` | OpenHU OS only: `zone` (`Europe/Berlin`, `UTC`), `offset` (seconds east of UTC), `ntp` (time from the internet), `synchronized`, `time` (seconds since 1970), `rtc` (a hardware clock exists), `source` (where the clock came from: `ntp`, `rtc`, `saved` = the time saved at the last shutdown, `build` = the image's build time, `manual`), `busy` |

Plugins publish their own topics with `bus.publish` (e.g. `vehicle.speed`).

### Vehicle data (Android Auto sensors)

OpenHU reads no vehicle data itself. Plugins or API clients publish it with `bus.publish`, and OpenHU passes it to
Android Auto as sensors. Who may feed Android Auto (#123):

- **Plugins** only with the sensor declared in their manifest (`androidAuto`, [plugins.md](plugins.md#android-auto)).
  OpenHU announces the declared sensors of enabled plugins at every connect, also before a value is there. Data from
  a plugin that does not declare the sensor is not passed on.
- **API clients that are not plugins** (scripts, other devices): their sensors are announced only when the topic has
  a value when the phone connects; one that starts later is picked up at the next connection. When the connection
  closes, OpenHU sets its `vehicle.*` topics to `{"available": false}`.

Values are
sent on every change; speed, rpm and heading are also repeated at the rate the phone asks for (at most every 250 ms),
so plugins may publish only changes. A topic with `"available": false` (e.g. the plugin lost the CAN bus) tells the
phone that the sensor failed; the next value recovers it. Fields that are missing or no number are left out.

| Topic | Fields | Sensor |
|---|---|---|
| `vehicle.speed` | `kmh`, `cruise` (bool, cruise control on) | `SENSOR_SPEED` |
| `vehicle.parkingBrake` | `engaged` (bool) | `SENSOR_PARKING_BRAKE` |
| `vehicle.gear` | see above | `SENSOR_GEAR` |
| `vehicle.drivingStatus` | `restricted` (bool), or `restrictions` (list of `video`, `keyboard`, `voice`, `config`, `messages`), or `status` (the protocol's bits) | `SENSOR_DRIVING_STATUS_DATA`, see below |
| `vehicle.fuel` | `percent` (of a full tank), `rangeKm`, `low` (bool, warning light) | `SENSOR_FUEL` (units on the wire unverified) |
| `vehicle.info` | `fuelTypes` (`petrol`, `diesel`, `lpg`, `cng`, `e85`, `biodiesel`, `electric`, `hydrogen`, …), `evConnectors` (`type1`, `type2`, `ccs1`, `ccs2`, `chademo`, `gbt`, `tesla`, …) | announced with the sensors (what the car tanks/charges) |
| `vehicle.odometer` | `km`, `tripKm` | `SENSOR_ODOMETER` |
| `vehicle.engine` | `rpm` | `SENSOR_RPM` |
| `vehicle.climate` | `outsideTemp` (°C), `pressureKpa` | `SENSOR_ENVIRONMENT_DATA` |
| `vehicle.compass` | `heading` (degrees, 0 = north, clockwise), `pitch`, `roll` | `SENSOR_COMPASS` |

**Driving status** (restrictions in Android Auto: keyboard, video, long lists and messages):
1. `vehicle.drivingStatus` from a plugin wins.
2. Otherwise Android Auto is unrestricted only while the car is parked: `vehicle.parkingBrake` engaged or `gear: park`,
   and slower than 2 km/h (or no speed known). Everything else with vehicle data gets every restriction, also standing
   at a traffic light without the parking brake. A gear known only as `reverse` (the reverse light) does not count.
3. Without parking brake, gear and speed, Android Auto is unrestricted (head units without vehicle data).

`vehicle.simulate` (topics table above) replaces gear, parking brake, speed and driving status for tests.

### Car controls (Android Auto climate rail)

A plugin that declares the service `carControl` ([plugins.md](plugins.md#android-auto)), or an API client that is not a
plugin, describes the car's controls in the kept topic `vehicle.controls`. OpenHU shows them in Android Auto's climate
rail (#121) and passes the user's changes back. The rail exists only in the phone's "hero" layout, which Android Auto
uses only for **video above 720p** (Settings → Android Auto → Resolution, e.g. 1920x1080).

```json
{"driverSide": "left", "units": "celsius", "properties": [
  {"name": "temperature", "area": "driver", "min": 16, "max": 28, "step": 0.5, "value": 21.5},
  {"name": "temperature", "area": "passenger", "min": 16, "max": 28, "step": 0.5, "value": 22},
  {"name": "seatHeating", "area": "driver", "min": 0, "max": 3, "value": 0},
  {"name": "steeringWheelHeating", "min": 0, "max": 3, "value": 1},
  {"name": "ac", "area": "front", "value": true},
  {"name": "ignition", "value": true}
]}
```

| Field | |
|---|---|
| `driverSide` | `left` (default) or `right`: which front seat `driver` is |
| `units` | display unit of the temperature: `celsius` (default) or `fahrenheit`; values are always °C |
| `name` | `temperature` (°C), `seatHeating` (levels; `min` < 0 = cooling), `steeringWheelHeating` (levels), `ac`, `fanSpeed`, `recirculation`, `dual` (sync), `defrost`, with a widget in the rail; `auto`, `maxAc`, `hvacPower` (the temperature shows only while it is on) and `ignition` (the controls work only while it is `true`) have none |
| `area` | seats: `driver`, `passenger`, `front`, `rearLeft`, `rearRight`, `rear` (default `front`); `defrost`: `windshield` (default), `rearWindow`; `steeringWheelHeating`, `ignition`: none |
| `min`, `max`, `step` | range of numbers; the temperature is shown in steps of `step` (default 0.5), `min` and `max` as LO and HI |
| `value` | current value (number or bool); missing or `"available": false`: shown as unavailable |
| `writable` | `false` for a value the user cannot change (default `true` for controls with a widget) |

- The structure (which controls and areas) is read when the phone connects; publish `vehicle.controls` before that.
  Afterwards only the values follow, every new `vehicle.controls` updates the rail. Unknown names or areas are logged
  and left out.
- A change on the phone becomes the event `vehicle.control.set` {`name`, `area`, `value`, `requestId`, `restricted`}
  (`restricted`: the car is not parked, see the driving status rules above; the plugin decides what it allows). The
  plugin answers with the event `vehicle.control.result` {`requestId`, `ok`, `error`} and publishes the new value in
  `vehicle.controls`: the phone shows the change right away and falls back to the old value without the new one
  within 2 seconds. Without a result within 2 seconds the phone is told that the change failed.
- An action button of the phone becomes the event `vehicle.control.action` {`action`} (e.g. `launchHvac`).
- When the plugin process ends, `vehicle.controls` becomes `{"available": false}` and the rail shows the controls as
  unavailable.

Android Auto is told when the head unit covers it (video focus `NATIVE`, the phone may stop rendering): while a
plugin overlay is shown, and while a plugin page replaced the Android Auto page (`aa` → `plugin:<id>`, e.g. the camera
in page mode) until the Android Auto page is shown again. Leaving Android Auto for another page by hand does not
change it. After "Exit" inside Android Auto the phone renders again when the Android Auto page is opened.

The shell shows the Android Auto page as soon as the phone starts projecting, except while a plugin page is shown
(e.g. the camera page in reverse gear when the phone connects or restarts its stream): the plugin page stays, the
phone gets `NATIVE`, and the next `ui.navigate` away from that page (e.g. the camera returning to its previous page)
shows Android Auto instead of the requested page. Choosing a page by hand, "Exit" inside Android Auto and the end of
the projection cancel this.

API clients on the wireless hotspot network are refused unless "API auch im WLAN-Hotspot" is switched on
(the hotspot is meant for the phone; connect API devices over Ethernet). If they are allowed, they need the token too.
Devices on the Ethernet port (OpenHU OS: Settings → Network → Ethernet, DHCP server: OpenHU is `10.42.1.1` and
`openhu.local`) are clients from the network like any other: they reach the API once "Share the API in the network"
is on, and always need the token.

### Local media (Android Auto media card)

A plugin that declares the service `localMedia` ([plugins.md](plugins.md#android-auto)), or an API client that is not
a plugin, gets its `media.status` / `media.metadata` into Android Auto's media card (#147): title, artist, album,
cover (a `data:image/...;base64` URL, scaled to at most 512 px), the source's name (the plugin's name) and the
buttons play, pause, previous and next. The phone shows the source that started playing last; `playing` brings it to
the front of the card.

A button in the card comes back as `input.action` (`media.play`, `media.pause`, `media.previous`, `media.next`,
`phase` `click`) with `"source": "<the source>"`. The plugin handles it even while Android Auto's own music owns the
media keys. Android Auto has no song list for these sources: the songs are chosen on the plugin's page, or in the
companion app. While the companion app is connected, the card shows the source stopped, so Android Auto lists only
the app; the card comes back when the app goes away.

### Companion app

The companion app ([openhu-companion](https://github.com/average-web-dev/openhu-companion), #158) shows the music
plugin's library as a media app in Android Auto and controls the head unit's player. It plays no sound on the phone.

**Connection.** The app listens on an RFCOMM service (UUID `57b07c95-708d-46ae-a658-9f296dfd034c`). When Android Auto
starts, the phone sends its Bluetooth address (`bluetooth.phone`). OpenHU then connects to the app on that phone, a
few times until the app's service is up. Nothing is set up in the app. This needs the Bluetooth service of the
Android Auto session (wireless Android Auto, or `Bluetooth.AdapterType` local) and a phone paired with the head unit.

**Protocol.** One JSON message per line, the messages of this API. The app sends its hello first:

```
→ {"type":"hello","app":"org.openhu.companion","version":"1.0.0","label":"OpenHU","protocol":1,"features":["media"]}
← {"type":"hello","openhu":1}
```

Then it is a client like any other (`subscribe`, `request`, events). It is not a local client: OpenHU logs the link
in with the token, because the bonded link to the session's phone is the trust. `companion.state` says that the app is
connected and gives its `label`, the name Android Auto reports for it (`media.status` `app`).

**Android Auto with the app.**
- While Android Auto shows the app and a local source starts, the phone keeps the audio focus. A focus loss would
  show the app as interrupted, and the music plays on the head unit anyway.
- The app's own playback state from the phone is published as `stopped` without metadata. As phone music it would
  pause the local sources.
- When the phone streams music next to it (`androidauto.mediaAudio`, for example Spotify started on the phone), that
  is another app. OpenHU publishes `media.status` `{"source":"androidauto","state":"playing"}`, and the local sources
  pause as usual.

**Bandwidth.** RFCOMM carries roughly 100–200 KB/s. OpenHU does not send Android Auto's own `media.current`,
`media.metadata` or `media.status` (source `androidauto`, every second with its cover). The app follows the music
plugin's `music.state` and takes the cover from its `media.metadata`. When the phone does not read (over 8 MB waiting),
the link is dropped.

## Methods

| Method | Params | Result |
|---|---|---|
| `system.ping` | – | `{pong:true}` |
| `system.methods` | – | list of methods |
| `bus.publish` | `topic`, `data` (object) | `{}` |
| `bus.emit` | `topic`, `data` (object) | `{}` – event without keeping a value (commands for plugin processes) |
| `bus.get` | `topic` | last value |
| `bus.topics` | – | topics that have a value |
| `settings.keys` | – | per key: `type` (`bool`, `int`, `string`, `choice`), `choices`, `min`/`max`, `maxLength`, `applies` (`now`, `reconnect`: next Android Auto connection, `restart`: of OpenHU), `secret`, `readOnly`, `debug` (the `debug.*` keys are listed only while `debug.enabled` is on, and can only be set then) |
| `settings.list` | – | `{values, pending}` – all settings (keys as in the settings page: `aa.fps`, `wireless.hotspot`, …); passwords and the API token (`secret`) only for clients on the device |
| `settings.get` | `key` | `{key, value}`; unknown key → error |
| `settings.set` | `key`, `value` | `{key, value, applies, pending}` – the same checks as the settings page, saved to `openhu.ini` right away; the value may be text (`"60"`, `"on"`, `"false"`) and is converted to the key's type; a wrong value → error with the reason |
| `audio.sources` | – | `{sources, min, max}` like the topic `audio.sources` |
| `audio.level.set` | `source` (id, e.g. `aa.media`, `org.openhu.radio.fm`), `level` (dB, −20 to 0, clamped) | `{source, level}`; stored in `Audio.SourceLevels`, applied at once |
| `volume.get` | – | `{volume, muted}` |
| `volume.set` | `value` (0–100) | `{volume, muted}` |
| `volume.change` | `delta` (e.g. `5`, `-5`) | `{volume, muted}` |
| `volume.mute` | `muted` (optional, toggles without) | `{volume, muted}` |
| `ui.navigate` | `page` (`home`, `aa`, `settings`, `settings/<category>`, `store` = `settings/apps`, `settings/apps/<plugin id>` = the settings of a plugin, `plugin:<id>`); categories: `general`, `aa`, `audio`, `wifi`, `bluetooth`, `network`, `keys`, `apps`, `system`, `info` (one that is not there, e.g. without its adapter, shows General) | `{}` |
| `ui.toast` | `text`, `duration` (ms, default 3000) | `{}` |
| `daynight.set` | `night` (bool) | `{}` |
| `projection.reconnect` | – | `{reset:[...]}` – USB reset of the phone, Android Auto reconnects |
| `input.action` | `action`, `phase` (`click` default, `press`, `release`) | `{handled}` |
| `input.actions` | – | built-in actions with description |
| `input.keymap` | – | active keyboard mapping |
| `plugin.list` | – | installed plugins (`id`, `name`, `version`, `enabled`, `running`, `dock`, `page`, `overlay`, `androidAuto`, `requires`, `recommends`, `missing`: [{`id`, `version`, `reason`: `notInstalled`/`tooOld`/`disabled`/`incompatible`}], `settings` (the checked `settings` of the manifest, only when it has some), …) |
| `plugin.enable` / `plugin.disable` | `id`, `force` (disable) | plugin; enabling also enables what it `requires`; disabling one that enabled plugins require is refused (`required by …`) unless `force: true`, which disables them too. Error `android auto connected` for a plugin with `androidAuto` (or one such a plugin requires) while the phone is connected |
| `plugin.restart` | `id` | plugin – restarts its process |
| `plugin.config.get` | `id`, `key` (optional) | `{id, values}`, or `{id, key, value}`; an unknown key → error ([config store](plugins.md#config-store)) |
| `plugin.config.set` | `id` and `key`, `value` – or `values` (object, several keys, all or none) | `{id, key, value}` or `{id, values}` – declared settings are checked and converted like `settings.set` (a wrong value → error with the reason), free keys take any JSON value; at most 1 MB per plugin; also for disabled plugins. A changed setting with `"applies": "restart"` restarts the process |
| `plugin.config.remove` | `id` and `key` or `keys` | `{id, values}` – a free key is deleted, a declared one goes back to its default |
| `plugin.config.reset` | `id` | `{id, values}` – all settings back to their defaults, free keys stay |
| `plugin.setDock` | `id`, `dock` (bool) | plugin – page as dock entry |
| `ui.overlay.show` / `ui.overlay.hide` | `id` | `{}` – plugin overlay above everything, including Android Auto (the phone is told, see above) |
| `store.list` | – | packages of all sources with `installed`, `installedVersion`, `updateAvailable`, `source`, `official` |
| `store.refresh` | – | `{queued}` – reload `index.json` of all sources |
| `store.install` | `id` | `{queued}` – install or update, what it `requires` first (refused when a requirement is not offered); a new plugin with `androidAuto` is installed disabled while the phone is connected, updating one is refused (`android auto connected`) |
| `store.update` | `id` (optional) | `{queued}` – one or all updates (same rule) |
| `store.remove` | `id`, `force` | `{queued}` (same rule); refused (`required by …`) for a plugin that enabled plugins require, unless `force: true` (they no longer start) |
| `store.sources.list` | – | sources |
| `store.sources.add` | `type` (`github`/`url`/`file`), `location`, `branch` | sources |
| `store.sources.remove` | `index` | sources |
| `wireless.state` | – | like the topic `wireless.state` |
| `wifi.state` | – | like the topic `wifi.state` |
| `wifi.scan` | – | `{queued}` – networks in range in `found`; on a card shared with an "Always on" hotspot the hotspot is off for 5 minutes (not during wireless Android Auto) |
| `wifi.add` | `ssid`, `password` (8–63 characters, empty for an open network), `hidden` | `{queued}` – saved as NetworkManager profile `openhu-wifi-<ssid>` and joined like `wifi.connect`; a wrong password removes it again |
| `wifi.connect` | `uuid` | `{queued}` – join it now; on a shared card the hotspot stays off while the connection lasts. Joined once, NetworkManager joins it by itself afterwards |
| `wifi.remove` | `uuid` | `{queued}` |
| `wifi.moveUp` | `uuid` | `{queued}` – one place higher in the order NetworkManager tries them |
| `bluetooth.state` | – | like the topic `bluetooth.state` |
| `bluetooth.devices` | – | paired devices (`address`, `name`, `connected`, `icon`, `uuids`) |
| `bluetooth.search` | `enabled` (default `true`), `seconds` (default 30) | `{discovered}` – devices found so far |
| `bluetooth.pair` | `address`, `pin` (optional, legacy pairing) | `{}` – OpenHU pairs and confirms by itself |
| `bluetooth.remove` | `address` | `{}` |
| `system.os` | – | `openhuOs` (bool), `version` (OpenHU OS version), `boot` (`state`: `normal`, `trial`, `updated`, `rollback`; `running`, `failed`) |
| `system.update.state` | – | like the topic `system.update` (OpenHU OS only, the others too) |
| `system.update.check` | – | `{queued}` – the USB sticks and the update server, result in `system.update` (`newest`, `available`, `source`) |
| `system.update.install` | – | `{queued}` – `offer` of `system.update`: the newest version found by the check or on a USB stick; refused while Android Auto is connected. Progress in `system.update`; after `done` reboot (`input.action` or the shell) |
| `system.update.cancel` | – | `{}` – the running version stays |
| `system.ssh.state` | – | like the topic `system.ssh` |
| `system.ssh.enable` | `enabled` (default `true`) | `{queued}` – needs a root password, which is set in the shell only (Settings → System) |
| `system.ethernet.state` | – | like the topic `system.ethernet` |
| `system.ethernet.setMode` | `mode` (`client`, `server`) | `{queued}` – `server` is refused while another DHCP server answers on the cable (never two on one network) |
| `system.time.state` | – | like the topic `system.time` |
| `system.time.zones` | – | the time zones to choose from (`Africa/Abidjan`, …, `UTC`) |
| `system.time.setZone` | `zone` | `{queued}` – applies at once |
| `system.time.setNtp` | `enabled` (default `true`) | `{queued}` – time from the internet on or off |
| `system.time.set` | `time` (seconds since 1970, UTC) | `{queued}` – only while the time from the internet is off; also sets the hardware clock |
| `caraudio.state` | – | like the topic `caraudio.state` |
| `caraudio.select` | `address` | car audio state – use this radio (pairs it), switches car audio on |
| `caraudio.forget` | – | car audio state – unpairs the radio, switches car audio off |
| `bluetooth.pairing` | `enabled` (default `true`) | `{pairing}` – visible for 2 minutes, wireless Android Auto must be on |
| `bluetooth.confirm` | `accept` (bool) | `{}` – answer to `bluetooth.pairing` |
| `bluetooth.connect` | `address` | `{}` – use this phone for wireless Android Auto and connect it |

Store operations run in the background; progress and errors arrive as `store.state` (`busy`, `status`, `error`).
Plugins: see [plugins.md](plugins.md).

## Actions

Steering wheel buttons, CAN, GPIO and keyboard all end up as actions (`input.action`).

| Action | Effect |
|---|---|
| `media.play_pause`, `media.play`, `media.pause`, `media.next`, `media.previous` | Android Auto media keys |
| `call.answer`, `call.end` | accept / end or reject a call |
| `voice` | voice assistant |
| `aa.home`, `aa.back`, `aa.navigation`, `aa.media`, `aa.phone` | Android Auto screens |
| `dpad.up`, `dpad.down`, `dpad.left`, `dpad.right`, `dpad.enter`, `rotary.left`, `rotary.right` | navigation inside Android Auto |
| `volume.up`, `volume.down` (±5 %), `volume.mute` | system volume |
| `ui.home`, `ui.aa`, `ui.settings` | shell pages |
| `ui.back` | back inside Android Auto while it is shown, otherwise to the home page |
| `aa.reconnect` | USB reset of the phone (also: tap on the phone icon in the status bar) |

Any other name is published as `input.action` only, so plugins can define actions (e.g. `camera.toggle`).
For long presses send `press` and `release` separately.

## Keyboard

Edited on the settings page (Keys), stored as differences to the defaults in `~/.openhu/keymap.json`
(container: `docker/data/keymap.json`).
Keys use Qt names, an empty action removes a key:

```json
{"N": "media.next", "F5": "camera.toggle", "Volume Up": "volume.up", "H": ""}
```

Defaults: arrows/Enter = D-pad, Esc = back, H = AA home, P/O = answer/end call, X/C/B = play/pause/toggle,
V/N = previous/next, M = voice, 1/2 = rotary, F/J/G = navigation/media/phone, +/- and media/volume keys.
