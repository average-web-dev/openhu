import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.Material as M
import OpenHU 1.0

// Material's switch with its sizes times Theme.scale (README.md). The indicator is drawn larger (scale), so the
// control's size and the text's place count its scaled size.
M.Switch {
    id: control
    readonly property real indicatorWidth: indicator ? indicator.width * Theme.scale : 0
    readonly property real indicatorHeight: indicator ? indicator.height * Theme.scale : 0
    padding: 8 * Theme.scale
    spacing: 8 * Theme.scale
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding,
                            indicatorWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             indicatorHeight + topPadding + bottomPadding)
    indicator.scale: Theme.scale
    // with a text the indicator sits at the left edge and grows to the right
    indicator.transformOrigin: control.text ? (control.mirrored ? Item.Right : Item.Left) : Item.Center
    // contentItem is declared as Item: its Text properties through Binding
    Binding { target: control.contentItem; property: "leftPadding"; value: control.indicator && !control.mirrored ? control.indicatorWidth + control.spacing : 0 }
    Binding { target: control.contentItem; property: "rightPadding"; value: control.indicator && control.mirrored ? control.indicatorWidth + control.spacing : 0 }
}
