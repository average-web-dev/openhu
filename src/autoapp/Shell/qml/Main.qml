import QtQuick
import OpenHU 1.0
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Window
import QtQuick.Layouts
import "."
import "settings"

// ApplicationWindow: its font reaches every control (shell and plugin pages), scaled like the rest (style/OpenHUStyle)
ApplicationWindow {
    id: root
    visible: true
    title: "OpenHU"
    color: Theme.background
    // Material's text size (14 px, editors 16 px: style/OpenHUStyle) and colours; Material sets them only for the style
    // named "Material" (also its QT_QUICK_CONTROLS_MATERIAL_* variables)
    font.pixelSize: Math.round(14 * Theme.scale)
    Material.theme: Material.Dark
    Material.accent: "#4f9cf9"
    Material.background: "#192027"

    // "home" | "aa" | "settings" | "plugin:<id>"
    property string page: "home"
    property string settingsCategory: "general"

    // The page on screen. While the phone restarts its video stream (after the reverse camera or "Exit") the
    // Android Auto page would show its last picture and then the phone's black frames and fade-in: the page shown
    // before stays until the first picture of the new stream is there, at most 2 s (#88)
    property string previousPage: "home"
    property bool pictureWaitOver: false
    readonly property string shownPage: page === "aa" && aaBridge.pictureWaiting && !pictureWaitOver
                                        ? previousPage : page

    // fromApi: ui.navigate (e.g. a plugin leaving its page), otherwise chosen by the user
    function navigate(name, fromApi) {
        if (fromApi)
            name = shell.pageOnNavigate(page, name)
        else
            shell.pageChosenByUser()
        // a category of the settings: "settings/wifi"; apps and store live in the settings
        if (name.startsWith("settings/")) {
            settingsCategory = name.slice(9)
            name = "settings"
        } else if (name === "apps" || name === "store") {
            settingsCategory = "apps"
            name = "settings"
        }
        page = name
    }
    onPageChanged: {
        shell.setCurrentPage(page)
        pictureWaitOver = false
    }
    onShownPageChanged: if (shownPage !== "aa") previousPage = shownPage

    // Fullscreen Android Auto (Settings → Android Auto, #146): the Android Auto page covers the whole window, the
    // other pages keep the dock and the status bar. The layout follows the setting only while no session runs: the
    // projection area decides resolution and margins, and a running session keeps the ones its video was made for.
    readonly property bool fullscreenSetting: settings.values["aa.fullscreen"] === true
    property bool fullscreenLayout: false
    function updateFullscreenLayout() {
        if (!aaBridge.videoActive)
            fullscreenLayout = fullscreenSetting
    }
    onFullscreenSettingChanged: updateFullscreenLayout()
    // Android Auto shown without dock and status bar: only with a picture, the waiting page keeps the dock
    readonly property bool statusBarHidden: fullscreenLayout && shownPage === "aa" && aaBridge.videoActive

    Timer {
        interval: 2000
        running: root.page === "aa" && aaBridge.pictureWaiting && !root.pictureWaitOver
        onTriggered: root.pictureWaitOver = true
    }
    Component.onCompleted: {
        shell.setCurrentPage(page)
        updatePluginPages()
        updateFullscreenLayout()
    }

    // Plugin pages and overlays, one row per plugin. Only the rows of plugins whose page changed are touched:
    // plugins.plugins also changes for the dock, running state, other plugins, ... and recreating a page would
    // e.g. take the camera away from a camera page while it is shown.
    ListModel { id: pluginPages }
    function updatePluginPages() {
        var pages = plugins.plugins.map(p => ({ pluginId: p.id, name: p.name, version: p.version, page: p.page,
                                                overlay: p.overlay, pluginEnabled: p.enabled }))
        for (var i = pluginPages.count - 1; i >= 0; --i) {
            var row = pluginPages.get(i)
            if (!pages.some(p => p.pluginId === row.pluginId))
                pluginPages.remove(i)
        }
        for (var j = 0; j < pages.length; ++j) {
            var index = -1
            for (var k = 0; k < pluginPages.count; ++k) {
                if (pluginPages.get(k).pluginId === pages[j].pluginId) {
                    index = k
                    break
                }
            }
            if (index < 0) {
                pluginPages.append(pages[j])
                continue
            }
            for (var key in pages[j]) {
                if (pluginPages.get(index)[key] !== pages[j][key])
                    pluginPages.setProperty(index, key, pages[j][key])
            }
        }
    }
    Connections {
        target: plugins
        function onPluginsChanged() { root.updatePluginPages() }
    }

    // Sizes are designed for 1280x720 (720x1280 in portrait)
    function updateScale() {
        Theme.scale = width >= height ? Math.min(width / 1280, height / 720) : Math.min(width / 720, height / 1280)
    }
    onWidthChanged: updateScale()
    onHeightChanged: updateScale()

    // Show Android Auto as soon as the phone starts projecting, fall back to home when it stops. A plugin page (the
    // reverse camera) stays: Android Auto takes the screen when the plugin leaves its page (#87).
    Connections {
        target: aaBridge
        function onVideoActiveChanged() {
            root.page = shell.pageOnVideo(root.page, aaBridge.videoActive)
            root.updateFullscreenLayout()
        }
    }

    // "Exit" inside Android Auto
    Connections {
        target: eventBus
        function onEvent(topic, data) {
            if (topic !== "projection.exit")
                return
            shell.pageChosenByUser()
            if (root.page === "aa")
                root.page = "home"
        }
    }

    // Requests from the API (ui.navigate, ui.toast)
    Connections {
        target: shell
        function onNavigateRequested(page) { root.navigate(page, true) }
        function onToastRequested(text, durationMs) { toast.show(text, durationMs) }
    }

    // What only the status bar shows (Android Auto has its own clock, signal and battery): a toast while it is hidden
    readonly property bool carAudioConnected: carAudio.state.connected === true
    onCarAudioConnectedChanged: {
        if (statusBarHidden)
            toast.show(carAudioConnected ? qsTrId("shell.car_audio_connected") : qsTrId("shell.car_audio_disconnected"))
    }
    BusTopic { id: simulation; topic: "vehicle.simulate" }
    readonly property string simulatedState: simulation.data.state || ""
    onSimulatedStateChanged: {
        if (statusBarHidden && simulatedState !== "")
            toast.show(qsTrId("statusbar.debug_simulated").arg(simulatedState))
    }

    // A USB stick with a newer OpenHU OS: tap opens Settings -> System with the Install button
    Connections {
        target: system
        function onUsbUpdateFound(version) {
            toast.show(qsTrId("shell.usb_update_found").arg(version), 8000, "settings/system")
        }
    }

    // Height of the on-screen keyboard while it is shown: the page area gets smaller, so the pages can scroll the
    // focused field into view (revealFocus)
    // Keyboard.qml is loaded by file name, so the shell starts without the module; qmllint only sees a QObject
    // qmllint disable missing-property
    readonly property real keyboardHeight: keyboard.item ? keyboard.item.occupied : 0
    // qmllint enable missing-property

    // Scrolls the Flickable around the focused text field so the field is above the keyboard
    function revealFocus() {
        const item = root.activeFocusItem
        if (!item || keyboardHeight === 0)
            return
        let flickable = item.parent
        while (flickable && !(flickable instanceof Flickable))
            flickable = flickable.parent
        if (!flickable)
            return
        const top = item.mapToItem(flickable.contentItem, 0, 0).y
        const margin = Theme.spacing
        if (top + item.height + margin > flickable.contentY + flickable.height)
            flickable.contentY = Math.min(top + item.height + margin - flickable.height,
                                          Math.max(0, flickable.contentHeight - flickable.height))
        else if (top - margin < flickable.contentY)
            flickable.contentY = Math.max(0, top - margin)
    }
    onActiveFocusItemChanged: revealTimer.restart()
    onKeyboardHeightChanged: revealTimer.restart()
    // after the layout followed the new height
    Timer {
        id: revealTimer
        interval: 50
        onTriggered: root.revealFocus()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Dock {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.dockWidth
            currentPage: root.page
            onPageSelected: name => root.navigate(name)
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            StatusBar {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.statusHeight
                onVolumeClicked: volumePanel.show()
            }

            // the pages: smaller while the on-screen keyboard is shown (dock and status bar stay)
            Item {
                id: pageArea
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.bottomMargin: root.keyboardHeight

                HomePage {
                    anchors.fill: parent
                    visible: root.shownPage === "home"
                    onOpenAndroidAuto: root.navigate("aa")
                    onOpenPage: name => root.navigate(name)
                }

                // in fullscreen above the whole shell (fullscreenArea). One instance: the video sink stays the same.
                AaPage {
                    parent: root.fullscreenLayout && aaBridge.videoActive ? fullscreenArea : pageArea
                    anchors.fill: parent
                    visible: root.shownPage === "aa"
                    viewportSize: root.fullscreenLayout ? Qt.size(root.width, root.height) : Qt.size(width, height)
                }

                Loader {
                    anchors.fill: parent
                    active: root.shownPage === "settings"
                    sourceComponent: SettingsPage {
                        category: root.settingsCategory
                        onCategoryChanged: root.settingsCategory = category
                    }
                }

                // Plugin pages, loaded when first shown and kept alive afterwards
                Repeater {
                    model: pluginPages

                    Loader {
                        id: pluginPage
                        required property string pluginId
                        required property string name
                        required property string page
                        required property bool pluginEnabled
                        anchors.fill: parent
                        visible: root.shownPage === "plugin:" + pluginId
                        property bool used: false
                        onVisibleChanged: if (visible) used = true
                        // created while already on screen (plugin updated or enabled again): no visibleChanged
                        Component.onCompleted: if (visible) used = true
                        active: pluginEnabled && page !== "" && (visible || used)
                        source: page

                        Text {
                            anchors.centerIn: parent
                            visible: pluginPage.status === Loader.Error
                            text: qsTrId("shell.page_cannot_loaded_log").arg(pluginPage.name)
                            color: Theme.textDim
                            font.pixelSize: Theme.fontNormal
                        }
                    }
                }
            }
        }
    }

    // Fullscreen Android Auto: above dock and status bar, below the overlays that follow
    Item {
        id: fullscreenArea
        anchors.fill: parent
        visible: root.statusBarHidden

        // Back to the shell without the phone: a swipe from the dock's edge shows the dock above Android Auto for a
        // few seconds. Touches that start in this strip do not go to the phone. (Also: Android Auto's exit button,
        // the ui.home / ui.back keys.)
        MouseArea {
            id: edgeSwipe
            z: 1
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Theme.spacing
            property real startX: 0
            onPressed: mouse => startX = mouse.x
            onPositionChanged: mouse => {
                if (mouse.x - startX > Theme.dockWidth / 4)
                    dockOverlay.show()
            }
        }
        // a tap next to the shown dock closes it (and does not go to the phone)
        MouseArea {
            z: 2
            anchors.fill: parent
            enabled: dockOverlay.shown
            onPressed: dockOverlay.shown = false
        }
        Dock {
            id: dockOverlay
            z: 3
            property bool shown: false
            function show() {
                shown = true
                dockHideTimer.restart()
            }
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Theme.dockWidth
            x: shown ? 0 : -width
            visible: x > -width
            Behavior on x { NumberAnimation { duration: 150 } }
            currentPage: root.page
            onPageSelected: name => {
                shown = false
                root.navigate(name)
            }
            Timer {
                id: dockHideTimer
                interval: 5000
                onTriggered: dockOverlay.shown = false
            }
        }
        Connections {
            target: root
            function onStatusBarHiddenChanged() { dockOverlay.shown = false }
        }
    }

    // Plugin overlays (e.g. reverse camera) above the whole shell, shown via plugins.setOverlayVisible / ui.overlay.show
    Repeater {
        model: pluginPages

        Loader {
            id: pluginOverlay
            required property string pluginId
            required property string overlay
            anchors.fill: parent
            property bool shown: plugins.overlayVisible(pluginId)
            active: shown && overlay !== ""
            source: overlay

            Connections {
                target: plugins
                function onOverlayChanged(id, visible) {
                    if (id === pluginOverlay.pluginId)
                        pluginOverlay.shown = visible
                }
            }
        }
    }

    // Overlays above everything, including Android Auto
    VolumePanel {
        id: volumePanel
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Theme.statusHeight + Theme.spacing
    }

    PairingDialog {
        anchors.fill: parent
    }

    // Tap outside the focused text field (and the keyboard above this): the field loses focus, the keyboard
    // closes. The tap still reaches what is below (a button, another field, which then takes the focus).
    MouseArea {
        anchors.fill: parent
        enabled: root.keyboardHeight > 0
        onPressed: mouse => {
            const field = root.activeFocusItem
            if (field && !field.contains(field.mapFromItem(null, mouse.x, mouse.y)))
                field.focus = false
            mouse.accepted = false
        }
    }

    // no size: a Loader with a size stretches the panel to it (Keyboard.qml sizes itself by the window)
    Loader {
        id: keyboard
        active: onScreenKeyboardAvailable
        source: "Keyboard.qml"
        // qmllint disable missing-property
        onLoaded: {
            item.allowed = Qt.binding(() => settings.values["general.onScreenKeyboard"] !== false)
            item.language = Qt.binding(() => settings.values["general.language"])
        }
        // qmllint enable missing-property
    }

    Toast {
        id: toast
        onOpened: page => root.navigate(page)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacing * 2 + root.keyboardHeight
    }
}
