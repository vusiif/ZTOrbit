pragma Singleton
import QtQuick

QtObject {
    property bool dark: false
    readonly property bool transitioning: windowAnimation.running

    // Surfaces: window < side/card < field, all from one blue-grey family so
    // popups and plugin panels no longer drift towards a separate hue.
    property color window: dark ? "#1d252d" : "#eef2f6"
    property color surface: dark ? "#26313b" : "#ffffff"
    property color side: dark ? "#222c35" : "#e4ebf2"
    property color field: dark ? "#1b232b" : "#ffffff"
    property color button: dark ? "#33424f" : "#edf2f7"
    property color popup: dark ? "#2b3742" : "#ffffff"
    property color popupHeader: dark ? "#323f4b" : "#f2f5f9"
    property color popupBorder: dark ? "#4d6172" : "#c3d0dc"
    property color cardBorder: dark ? "#34434f" : "#dbe3eb"

    property color text: dark ? "#e3ecf3" : "#1d3445"
    property color muted: dark ? "#9fb3c2" : "#5f7384"
    property color border: dark ? "#455969" : "#c3d0dc"
    property color accent: dark ? "#8cc4ea" : "#235b8c"
    property color selection: dark ? "#33516a" : "#dceaf6"
    property color hover: dark ? "#2f4051" : "#e8f0f7"

    // Filled call-to-action buttons.
    property color primary: dark ? "#4f95c8" : "#2a6aa3"
    property color primaryHover: dark ? "#63a6d6" : "#225b8d"
    property color primaryText: dark ? "#0f1d28" : "#ffffff"

    property color danger: dark ? "#ff9f96" : "#b03a30"
    property color dangerSoft: dark ? "#3d2a2c" : "#fbecea"

    // Status signals keep the same hue in both themes so they read as state,
    // not decoration.
    readonly property color online: "#25a773"
    readonly property color offline: "#d95059"
    readonly property color warning: "#d49a2a"

    property color noticeSuccess: dark ? "#284b3b" : "#e0f3e7"
    property color noticeSuccessText: dark ? "#daf5e3" : "#16482b"
    property color noticeSuccessBorder: dark ? "#56856a" : "#92c6a5"
    property color noticeError: dark ? "#57363b" : "#fbe3e5"
    property color noticeErrorText: dark ? "#ffe2e3" : "#7f2533"
    property color noticeErrorBorder: dark ? "#a76b73" : "#dfa3ad"
    readonly property color scrim: "#99343a40"

    // Type scale: page title, section heading, body, caption.
    readonly property int fontTitle: 22
    readonly property int fontHeading: 16
    readonly property int fontBody: 14
    readonly property int fontCaption: 12
    readonly property int radius: 6
    readonly property int cardRadius: 10

    Behavior on noticeSuccess { ColorAnimation { duration: 180 } }
    Behavior on noticeSuccessText { ColorAnimation { duration: 180 } }
    Behavior on noticeSuccessBorder { ColorAnimation { duration: 180 } }
    Behavior on noticeError { ColorAnimation { duration: 180 } }
    Behavior on noticeErrorText { ColorAnimation { duration: 180 } }
    Behavior on noticeErrorBorder { ColorAnimation { duration: 180 } }
    Behavior on window { ColorAnimation { id: windowAnimation; duration: 180 } }
    Behavior on surface { ColorAnimation { duration: 180 } }
    Behavior on side { ColorAnimation { duration: 180 } }
    Behavior on text { ColorAnimation { duration: 180 } }
    Behavior on muted { ColorAnimation { duration: 180 } }
    Behavior on border { ColorAnimation { duration: 180 } }
    Behavior on cardBorder { ColorAnimation { duration: 180 } }
    Behavior on accent { ColorAnimation { duration: 180 } }
    Behavior on selection { ColorAnimation { duration: 180 } }
    Behavior on hover { ColorAnimation { duration: 180 } }
    Behavior on button { ColorAnimation { duration: 180 } }
    Behavior on primary { ColorAnimation { duration: 180 } }
    Behavior on primaryHover { ColorAnimation { duration: 180 } }
    Behavior on primaryText { ColorAnimation { duration: 180 } }
    Behavior on danger { ColorAnimation { duration: 180 } }
    Behavior on dangerSoft { ColorAnimation { duration: 180 } }
    Behavior on popup { ColorAnimation { duration: 180 } }
    Behavior on popupHeader { ColorAnimation { duration: 180 } }
    Behavior on popupBorder { ColorAnimation { duration: 180 } }
    Behavior on field { ColorAnimation { duration: 180 } }
}
