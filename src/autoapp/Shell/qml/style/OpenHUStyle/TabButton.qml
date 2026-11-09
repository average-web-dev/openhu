import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's tab button with its sizes times Theme.scale (README.md)
M.TabButton {
    font.weight: Font.Medium
    padding: 12 * Theme.scale
    spacing: 6 * Theme.scale
    icon.width: 24 * Theme.scale
    icon.height: 24 * Theme.scale
    background.implicitHeight: M.Material.touchTarget * Theme.scale
}
