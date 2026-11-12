import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's spin box with its sizes times Theme.scale (README.md)
M.SpinBox {
    font.pixelSize: Math.round(16 * Theme.scale)
    spacing: 6 * Theme.scale
    topPadding: M.Material.textFieldVerticalPadding * Theme.scale
    bottomPadding: M.Material.textFieldVerticalPadding * Theme.scale
    up.indicator.implicitWidth: M.Material.touchTarget * Theme.scale
    up.indicator.implicitHeight: M.Material.touchTarget * Theme.scale
    down.indicator.implicitWidth: M.Material.touchTarget * Theme.scale
    down.indicator.implicitHeight: M.Material.touchTarget * Theme.scale
    background.implicitWidth: 140 * Theme.scale
    background.implicitHeight: M.Material.textFieldHeight * Theme.scale
}
