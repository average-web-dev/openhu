# Troubleshooting

## Logs

| | |
|---|---|
| Service | `journalctl -u openhu -f` |
| Container | output of `docker/dev.sh run` (or `docker logs openhu`) |
| Plugins | `~/.openhu/logs/<id>.log` (container: `docker/data/logs/`) |
| Live data | `websocat ws://127.0.0.1:5520`, then `{"type":"subscribe","topics":["*"]}` ([api.md](api.md)) |
| Crashes | `coredumpctl list openhu` (also catches crashes inside the container) |

## Android Auto over USB

**The phone is not detected / Android Auto does not start.** Tap the phone icon in the status bar (or
`docker/dev.sh reconnect`, API `projection.reconnect`): it resets the phone on USB as if it was plugged in again.
Check that the udev rule is installed (`/lib/udev/rules.d/99-openhu.rules`) when not running as root.

**Black picture, but Android Auto is connected.** The video is decoded with FFmpeg: the log shows
`[FfmpegVideoOutput] decoder …` at the start of a session and every 10 s fps, decoding time and errors. A hardware
decoder that shows no picture is replaced by software automatically; `Video.Decoder=software` in `openhu.ini`
forces it. When building yourself, make sure only one aasdk is
installed: builds before the `libaasdk` packages put it into `/usr/local`, and the compiler prefers the old headers in
`/usr/local/include`. Remove it with
`sudo rm -r /usr/local/lib/libaasdk* /usr/local/lib/libaap_protobuf* /usr/local/lib/cmake/aasdk /usr/local/lib/cmake/aap_protobuf /usr/local/include/aasdk /usr/local/include/aap_protobuf`.

**The picture lags behind or freezes for a while.** The decoder cannot keep up. OpenHU acknowledges a video message
to the phone only when the decoder takes it from its queue (#22). The phone keeps at most `max_unacked` messages
unacknowledged, so it waits and skips frames, and no frame has to be dropped. The log shows every 10 s
`[VideoMediaSinkService] video acks: …` with the ack delay and how many messages were unacknowledged. With
`OPENHU_LOG=debug` it also shows every ack (`ack 1 after … ms, … unacked in the queue`). If the phone does not wait and
the queue still overflows, OpenHU drops everything up to the next IDR frame (`decoder too slow, dropped …`). The
picture then freezes until the phone sends the next IDR frame. These environment variables are debugging knobs
(on OpenHU OS one `VAR=value` per line in `/var/lib/openhu/session.env`, then `openhu-cli restart`):

| Variable | Effect |
|---|---|
| `OPENHU_DEBUG_DECODE_DELAY_MS=100` | simulates a slow decoder: waits 100 ms before each picture (about 10 fps) |
| `OPENHU_VIDEO_ACK=immediate` | acknowledges every message as soon as it is queued (the behaviour before #22) |
| `OPENHU_VIDEO_MAX_UNACKED=<n>` | the `max_unacked` OpenHU sends to the phone (default 1) |
| `OPENHU_VIDEO_ACK_MAX_DELAY_MS=<ms>` | the longest an ack is held back while the decoder is blocked (default 500) |

**SSL errors right after a restart of OpenHU** (`SSL routines::unexpected message`). The phone was still in
accessory mode from the previous run. OpenHU resets such a phone at startup; if it still happens, unplug it once.

## Wireless Android Auto

The state is shown under **Settings → Android Auto → Wireless Android Auto** and in the topic `wireless.state`.

| Message / symptom | Cause, fix |
|---|---|
| "WiFi is switched off." | WiFi radio off or blocked: button "Switch WiFi on" (`nmcli radio wifi on`), `rfkill list` |
| "No WiFi device found." / "NetworkManager (nmcli) is missing." | NetworkManager does not manage the card (`nmcli device`) or is not installed |
| "The hotspot does not start …" | card without access point mode on that band, or the country does not allow it: set the country, try band "Auto" or "2.4 GHz", a fixed channel 36–48 |
| "Bluetooth service (bluetoothd) not reachable." | BlueZ not running; in the container: `/run/dbus/system_bus_socket` not mounted or not `--privileged` (AppArmor blocks D-Bus) |
| Phone stays at "Connecting to Android Auto" | the phone does not get the WiFi data: check that OpenHU's Bluetooth profile is registered (log `[WirelessController] Bluetooth ready`); `btmon` shows whether the phone opens the RFCOMM channel |
| "The phone reports: STATUS_WIFI_NETWORK_UNAVAILABLE" | the phone does not see the hotspot: band (phones often want 5 GHz), country, distance |
| The phone cannot connect when pairing | remove the pairing on both sides and pair again; on desktops with PipeWire OpenHU uses its hands-free profile (log `phone link via hands-free (system)`) |
| Wireless on although the phone is on the cable | "Priority with cable" is "WiFi"; with "USB" the cable wins |

Useful tools: `nmcli connection show --active`, `journalctl -u NetworkManager` (DHCP of the phone),
`tcpdump -i <wifi> port 5000` (Android Auto connection), `btmon` (Bluetooth).

## Bluetooth car radio

| Message / symptom | Cause, fix |
|---|---|
| "Connected, but no audio output yet (A2DP)" | Many car radios connect hands-free (HFP) only and start A2DP when asked or when Bluetooth audio is chosen on the radio. OpenHU asks for A2DP (BlueZ `ConnectProfile`) every 5 s and switches a card that came up with a headset profile to A2DP; it keeps looking while the radio is connected. The log shows what the audio server offers: `[OutputRouter] <address>: card …, profile …, A2DP profile …, sink …`. If no card appears, A2DP is not available in the audio server: PipeWire needs its Bluetooth support (Debian `libspa-0.2-bluetooth`, `wireplumber`), PulseAudio `pulseaudio-module-bluetooth`; `pactl list short sinks` must list `bluez_output…` |
| Radio connected, sound stays on the old output | OpenHU makes the radio's sink the default and moves the streams that played on the old default (and its own Android Auto streams) to it; the log shows `[OutputRouter] output …` and `… stream(s) to …`. Streams on another output, chosen there on purpose, stay |
| Radio not found by the search | radio not in pairing mode, or it only shows up with its address (no name yet): search again |
| Old radio rejects pairing | it wants a PIN: set "Kopplungs-PIN" (often `0000` or `1234`) |
| Buttons do nothing | the radio needs AVRCP; the log shows `[CarAudio] radio button: …` for every press that arrives |
| Calls end by themselves | the radio's pause/stop ends a call only once it lasted 5 s (radios send them when they switch to their phone source); the log shows `[CarAudio] radio button: … -> ignored: …` or `-> call.end` |

When OpenHU stops, the radio disconnects or car audio is switched off, the previous default output is restored and the moved streams go back. Switching car audio off does not disconnect the device.

## Settings and plugins

- Values on the settings page that are not accepted show a hint at the bottom of the screen.
- Plugin process crashing: its output is in `logs/<id>.log`; OpenHU restarts it with a growing delay (up to
  60 s), `plugin.list` shows whether it is running.
- Plugin page or icon does not update after an update: open the page again; OpenHU clears its QML cache when a
  plugin is replaced.
