import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's item delegate with its sizes times Theme.scale (README.md)
M.ItemDelegate {
    padding: 16 * Theme.scale
    verticalPadding: 8 * Theme.scale
    spacing: 16 * Theme.scale
    icon.width: 24 * Theme.scale
    icon.height: 24 * Theme.scale
    background.implicitHeight: M.Material.delegateHeight * Theme.scale
}
