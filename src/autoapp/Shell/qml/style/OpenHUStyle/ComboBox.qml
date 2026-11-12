import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's combo box with its sizes times Theme.scale (README.md); the entries are MenuItems (scaled as well)
M.ComboBox {
    id: control
    font.pixelSize: Math.round(16 * Theme.scale)
    indicator.scale: Theme.scale
    // room for the larger arrow
    rightPadding: padding + (indicator && indicator.visible ? indicator.width * Theme.scale + spacing : 0)
    // contentItem is declared as Item: its TextField properties through Binding
    Binding { target: control.contentItem; property: "leftPadding"; value: M.Material.textFieldHorizontalPadding * Theme.scale }
    Binding { target: control.contentItem; property: "topPadding"; value: M.Material.textFieldVerticalPadding * Theme.scale }
    Binding { target: control.contentItem; property: "bottomPadding"; value: M.Material.textFieldVerticalPadding * Theme.scale }
    background.implicitWidth: 120 * Theme.scale
    background.implicitHeight: M.Material.textFieldHeight * Theme.scale
    popup.verticalPadding: 8 * Theme.scale
}
