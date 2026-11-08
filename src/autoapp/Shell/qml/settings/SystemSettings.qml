import QtQuick
import QtQuick.Controls
import OpenHU 1.0

// OpenHU OS: version, time zone and time, system update, ssh access, restart and power off of the device (only on
// OpenHU OS, see SystemController).
Flickable {
    id: page

    // not "update"/"ssh": inside child items "update" would be Item.update()
    readonly property var updateInfo: system.update
    readonly property var sshInfo: system.ssh
    readonly property var timeInfo: system.time
    // "Europe/Berlin", ... sorted by region, so the drop-down keeps the regions together
    readonly property var zones: system.timeZones()
    readonly property bool installing: updateInfo.state === "checking" || updateInfo.state === "installing"
    // what Install installs: a found version, online or on a USB stick
    readonly property var offer: updateInfo.offer || null

    contentWidth: width
    contentHeight: column.implicitHeight + Theme.spacing * 2
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    Connections {
        target: system
        function onSshError(message) { shell.toast(message, 5000) }
        function onTimeError(message) { shell.toast(message, 5000) }
    }
    // synchronized changes on its own
    Component.onCompleted: system.refreshTime()

    function regionOf(zone) {
        const slash = zone.indexOf("/")
        return slash > 0 ? zone.slice(0, slash) : zone
    }
    function cityOf(zone) {
        const slash = zone.indexOf("/")
        return (slash > 0 ? zone.slice(slash + 1) : zone).replace(/_/g, " ")
    }
    // "Europe / Berlin", "America / Argentina/Salta", "UTC"
    function zoneLabel(zone) {
        const region = regionOf(zone)
        return region === zone ? zone : regionName(region) + " / " + cityOf(zone)
    }
    function regionName(region) {
        switch (region) {
        case "Africa": return qsTrId("settings.system.africa")
        case "America": return qsTrId("settings.system.america")
        case "Antarctica": return qsTrId("settings.system.antarctica")
        case "Arctic": return qsTrId("settings.system.arctic")
        case "Asia": return qsTrId("settings.system.asia")
        case "Atlantic": return qsTrId("settings.system.atlantic_ocean")
        case "Australia": return qsTrId("settings.system.australia")
        case "Europe": return qsTrId("settings.system.europe")
        case "Indian": return qsTrId("settings.system.indian_ocean")
        case "Pacific": return qsTrId("settings.system.pacific_ocean")
        }
        return region
    }
    // "UTC+02:00"
    function offsetText(seconds) {
        const minutes = Math.abs(seconds) / 60
        const pad = n => String(Math.floor(n)).padStart(2, "0")
        return "UTC" + (seconds < 0 ? "−" : "+") + pad(minutes / 60) + ":" + pad(minutes % 60)
    }
    function zoneText() {
        if (page.timeInfo.error)
            return page.timeInfo.error
        if (!page.timeInfo.zone)
            return ""
        return page.timeInfo.zone === "UTC" ? "UTC"
             : page.cityOf(page.timeInfo.zone) + " (" + page.offsetText(page.timeInfo.offset || 0) + ")"
    }
    function ntpText() {
        if (page.timeInfo.ntp && page.timeInfo.synchronized)
            return qsTrId("settings.system.synchronized_time_server")
        const text = page.timeInfo.ntp ? qsTrId("settings.system.not_synchronized_yet_needs")
                                       : qsTrId("settings.system.off_set_date_time")
        const source = page.sourceText()
        return source ? text + " " + source : text
    }
    // where the clock came from without a time server (OpenHU OS: openhu-time status)
    function sourceText() {
        switch (page.timeInfo.source) {
        case "rtc": return qsTrId("settings.system.time_source_rtc")
        case "saved": return qsTrId("settings.system.time_source_saved")
        case "build": return qsTrId("settings.system.time_source_build")
        case "manual": return qsTrId("settings.system.time_source_manual")
        default: return ""
        }
    }
    // the date and time pickers start at the current time
    function loadNow() {
        const now = new Date()
        year.currentIndex = now.getFullYear() - year.first
        month.currentIndex = now.getMonth()
        day.currentIndex = now.getDate() - 1
        hour.currentIndex = now.getHours()
        minute.currentIndex = now.getMinutes()
    }

    // a column of numbers to spin through (01, 02, ...), or of names
    component NumberTumbler: Tumbler {
        id: tumbler
        property int first: 0
        property int digits: 2
        property var names: null
        width: Theme.fontNormal * (digits + 1.6)
        height: Theme.fontNormal * 5
        visibleItemCount: 3
        delegate: Text {
            required property int index
            text: tumbler.names ? tumbler.names[index] : String(index + tumbler.first).padStart(tumbler.digits, "0")
            color: Theme.text
            opacity: 1.0 - Math.abs(Tumbler.displacement) / (tumbler.visibleItemCount / 2)
            font.pixelSize: Theme.fontNormal
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    function bootText() {
        const boot = system.bootStatus
        if (boot.state === "updated")
            return qsTrId("settings.system.boot_updated").arg(boot.running)
        if (boot.state === "rollback")
            return qsTrId("settings.system.update_failed_previous_version").arg(boot.failed)
        if (boot.state === "trial")
            return qsTrId("settings.system.new_version_being_checked")
        return qsTrId("settings.system.updated_whole_if_new")
    }

    function updateTitle() {
        switch (page.updateInfo.state) {
        case "checking": return qsTrId("settings.system.preparing_update")
        case "installing": return qsTrId("settings.system.installing_update").arg(page.updateInfo.version)
        case "done": return qsTrId("settings.system.update_installed").arg(page.updateInfo.version)
        case "error": return qsTrId("settings.system.update_failed")
        case "cancelled": return qsTrId("settings.system.update_cancelled")
        }
        if (page.updateInfo.busy)
            return qsTrId("settings.system.searching_updates")
        if (page.offer)
            return page.offer.source === "usb" ? qsTrId("settings.system.usb_update").arg(page.offer.version)
                                               : qsTrId("settings.system.new_version").arg(page.offer.version)
        if (page.updateInfo.checked && page.updateInfo.checkError)
            return qsTrId("settings.system.search_failed")
        if (page.updateInfo.checked)
            return qsTrId("settings.system.openhu_os_up_date")
        return qsTrId("settings.system.search_updates")
    }

    function updateDescription() {
        switch (page.updateInfo.state) {
        case "installing":
            return page.updateInfo.percent + " % · " + (page.updateInfo.phase === "kernel" ? qsTrId("settings.system.kernel") : qsTrId("settings.system.system"))
                   + " · " + (page.updateInfo.source === "usb" ? qsTrId("settings.system.keep_stick_plugged")
                                                               : qsTrId("settings.system.openhu_keeps_running_please"))
        case "done": return qsTrId("settings.system.active_after_restart_if").arg(page.updateInfo.from)
        case "error": return page.updateInfo.error || ""
        case "cancelled": return qsTrId("settings.system.running_version_unchanged")
        }
        if (page.offer && !page.updateInfo.busy)
            return (page.offer.source === "usb" ? qsTrId("settings.system.usb_no_download")
                                                : qsTrId("settings.system.about_500_mb_download"))
                   + (page.updateInfo.blocked ? " " + qsTrId("settings.system.not_android_auto_connected") : "")
        // without a network the hotspot may have the WiFi card: WiFi networks only after Search or Connect
        const hotspotOnCard = !!wifi.state.available && !!wifi.state.hotspotDevice && !(wifi.state.client || {}).ssid
        if (page.updateInfo.checked && page.updateInfo.checkError)
            return page.updateInfo.checkError
                   + (hotspotOnCard ? " " + qsTrId("settings.system.wifi_card_runs_hotspot") : "")
        return qsTrId("settings.system.online_github_releases_needs")
    }

    function sshText() {
        if (page.sshInfo.error)
            return page.sshInfo.error
        let text = !page.sshInfo.enabled ? qsTrId("settings.system.off")
                 : page.sshInfo.running ? qsTrId("settings.system.ssh_root").arg(page.sshInfo.addresses && page.sshInfo.addresses.length ? page.sshInfo.addresses.join(" / ") : qsTrId("settings.system.address"))
                 : qsTrId("settings.system.starting")
        if (!page.sshInfo.enabled && !page.sshInfo.password && !page.sshInfo.keys)
            text += " " + qsTrId("settings.system.set_root_password_first")
        return text + " " + qsTrId("settings.system.reachable_every_network_device")
    }

    // plain Column: rows with wrapped text get their width from here (no layout loop)
    Column {
        id: column
        x: Theme.spacing * 1.5
        y: Theme.spacing
        width: page.width - Theme.spacing * 3
        spacing: Theme.spacing / 2

        SectionTitle { text: "OpenHU OS" }
        SettingRow {
            title: qsTrId("settings.system.version").arg(system.osVersion)
            description: page.bootText()
        }

        SectionTitle { text: qsTrId("settings.system.date_time") }
        SettingRow {
            title: qsTrId("settings.system.time_zone")
            description: page.zoneText()
            ComboBox {
                width: Theme.dockWidth * 4
                enabled: !page.timeInfo.busy && !page.timeInfo.error
                model: page.zones.map(page.zoneLabel)
                currentIndex: page.zones.indexOf(page.timeInfo.zone)
                onActivated: index => system.setTimeZone(page.zones[index])
            }
        }
        SettingRow {
            title: qsTrId("settings.system.time_internet")
            description: page.ntpText()
            Switch {
                checked: !!page.timeInfo.ntp
                enabled: !page.timeInfo.busy && !page.timeInfo.error
                onToggled: system.setNtp(checked)
            }
        }
        SettingRow {
            visible: !page.timeInfo.ntp && !page.timeInfo.error && page.timeInfo.zone !== undefined
            // after the tumblers are set up (an index set before is reset), also when visible from the start
            Component.onCompleted: Qt.callLater(page.loadNow)
            onVisibleChanged: if (visible) Qt.callLater(page.loadNow)
            title: qsTrId("settings.system.date_time")
            description: qsTrId("settings.system.time_zone_above_also")
            Row {
                spacing: Theme.spacing / 4
                NumberTumbler {
                    id: day
                    first: 1
                    // always 31: a model that follows the month would reset the day (Set keeps it in the month)
                    model: 31
                }
                // the month by its name: day, month and year are clear in every language
                NumberTumbler {
                    id: month
                    width: Theme.fontNormal * 4
                    model: 12
                    names: [...Array(12).keys()].map(index => Qt.locale(settings.values["general.language"])
                                                                .standaloneMonthName(index, Locale.ShortFormat))
                }
                NumberTumbler {
                    id: year
                    first: 2024
                    digits: 4
                    model: 30
                }
                Item { width: Theme.spacing / 2; height: 1 }
                NumberTumbler {
                    id: hour
                    model: 24
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: ":"
                    color: Theme.text
                    font.pixelSize: Theme.fontNormal
                }
                NumberTumbler {
                    id: minute
                    model: 60
                }
                Button {
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: !page.timeInfo.busy
                    text: qsTrId("settings.system.set")
                    onClicked: {
                        const y = year.currentIndex + year.first
                        const lastDay = new Date(y, month.currentIndex + 1, 0).getDate()
                        const time = new Date(y, month.currentIndex, Math.min(day.currentIndex + 1, lastDay),
                                              hour.currentIndex, minute.currentIndex)
                        const error = system.setTime(Math.floor(time.getTime() / 1000))
                        if (error)
                            shell.toast(error, 4000)
                    }
                }
            }
        }

        SectionTitle { text: qsTrId("settings.system.update") }
        SettingRow {
            title: page.updateTitle()
            description: page.updateDescription()

            Row {
                spacing: Theme.spacing / 2
                Button {
                    visible: !page.installing && page.updateInfo.state !== "done" && !page.offer
                    enabled: !page.updateInfo.busy
                    text: page.updateInfo.checked ? qsTrId("settings.system.search_again") : qsTrId("settings.system.search")
                    onClicked: system.checkForUpdate()
                }
                ConfirmButton {
                    visible: !page.installing && page.updateInfo.state !== "done" && !!page.offer
                    enabled: !page.updateInfo.blocked && !page.updateInfo.busy
                    label: qsTrId("settings.system.install")
                    confirmText: qsTrId("settings.system.really_install")
                    onConfirmed: {
                        const error = system.installUpdate()
                        if (error)
                            shell.toast(error, 4000)
                    }
                }
                Button {
                    visible: page.installing
                    text: qsTrId("settings.system.cancel")
                    onClicked: system.cancelUpdate()
                }
                ConfirmButton {
                    visible: page.updateInfo.state === "done"
                    label: qsTrId("settings.system.restart")
                    confirmText: qsTrId("settings.system.really_restart")
                    onConfirmed: shell.reboot()
                }
            }
        }
        ProgressBar {
            visible: page.updateInfo.state === "installing"
            width: parent.width
            from: 0
            to: 100
            value: page.updateInfo.percent || 0
        }

        SectionTitle { text: qsTrId("settings.system.ssh_access") }
        SettingRow {
            title: "SSH"
            description: page.sshText()
            Switch {
                checked: !!page.sshInfo.enabled
                enabled: !page.sshInfo.busy && !page.sshInfo.error && (!!page.sshInfo.enabled || !!page.sshInfo.password || !!page.sshInfo.keys)
                onToggled: system.setSshEnabled(checked)
            }
        }
        SettingRow {
            title: qsTrId("settings.system.root_password")
            description: page.sshInfo.password ? qsTrId("settings.system.set_new_one_replaces") : qsTrId("settings.system.not_set_yet_least")
            Row {
                spacing: Theme.spacing / 2
                TextField {
                    id: password
                    width: Theme.dockWidth * 3
                    // one field: visible while typing (no second field to catch typos), hidden afterwards
                    echoMode: TextInput.PasswordEchoOnEdit
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    placeholderText: qsTrId("settings.system.new_password")
                    onAccepted: saveButton.clicked()
                }
                Button {
                    id: saveButton
                    text: qsTrId("settings.system.save")
                    enabled: !page.sshInfo.busy && password.text.length > 0
                    onClicked: {
                        const error = system.setSshPassword(password.text)
                        if (error) {
                            shell.toast(error, 4000)
                            return
                        }
                        // the row says "Set" once the helper stored it, errors come as a toast
                        password.text = ""
                        password.focus = false  // closes the on-screen keyboard
                    }
                }
            }
        }

        SectionTitle { text: qsTrId("settings.system.device") }
        SettingRow {
            title: qsTrId("settings.system.restart")
            description: qsTrId("settings.system.restarts_device")
            ConfirmButton {
                label: qsTrId("settings.system.restart")
                confirmText: qsTrId("settings.system.really_restart")
                onConfirmed: shell.reboot()
            }
        }
        SettingRow {
            title: qsTrId("settings.system.power_off")
            description: qsTrId("settings.system.shuts_device_down")
            ConfirmButton {
                label: qsTrId("settings.system.power_off")
                confirmText: qsTrId("settings.system.really_power_off")
                onConfirmed: shell.powerOff()
            }
        }
    }
}
