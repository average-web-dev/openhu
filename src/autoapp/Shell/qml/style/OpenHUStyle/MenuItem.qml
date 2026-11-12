import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's menu item (also the entries of a ComboBox) with its sizes times Theme.scale (README.md)
M.MenuItem {
    font.pixelSize: Math.round(16 * Theme.scale)
    padding: 16 * Theme.scale
    verticalPadding: M.Material.menuItemVerticalPadding * Theme.scale
    spacing: 16 * Theme.scale
    icon.width: 24 * Theme.scale
    icon.height: 24 * Theme.scale
    background.implicitHeight: M.Material.menuItemHeight * Theme.scale
}
