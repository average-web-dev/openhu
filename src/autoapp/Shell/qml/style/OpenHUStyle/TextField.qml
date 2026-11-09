import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's text field with its sizes times Theme.scale (README.md)
M.TextField {
    font.pixelSize: Math.round(16 * Theme.scale)
    leftPadding: M.Material.textFieldHorizontalPadding * Theme.scale
    rightPadding: M.Material.textFieldHorizontalPadding * Theme.scale
    topPadding: M.Material.textFieldVerticalPadding * Theme.scale + topInset
    bottomPadding: M.Material.textFieldVerticalPadding * Theme.scale
    background.implicitWidth: 120 * Theme.scale
    background.implicitHeight: M.Material.textFieldHeight * Theme.scale
}
