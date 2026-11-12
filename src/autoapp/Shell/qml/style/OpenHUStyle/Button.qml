import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's button with its sizes times Theme.scale (README.md)
M.Button {
    id: control
    font.weight: Font.Medium
    topInset: 6 * Theme.scale
    bottomInset: 6 * Theme.scale
    verticalPadding: M.Material.buttonVerticalPadding * Theme.scale
    leftPadding: M.Material.buttonLeftPadding(flat, hasIcon && display !== T.AbstractButton.TextOnly) * Theme.scale
    rightPadding: M.Material.buttonRightPadding(flat, hasIcon && display !== T.AbstractButton.TextOnly,
                                                text !== "" && display !== T.AbstractButton.IconOnly) * Theme.scale
    spacing: 8 * Theme.scale
    icon.width: 24 * Theme.scale
    icon.height: 24 * Theme.scale
    background.implicitWidth: 64 * Theme.scale
    background.implicitHeight: M.Material.buttonHeight * Theme.scale
}
