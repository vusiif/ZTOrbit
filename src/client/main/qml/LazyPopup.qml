import QtQuick

Loader {
    id: loader
    active: false
    asynchronous: false
    property Item popupParent: parent

    // Keep an opened dialog for subsequent use, but do not construct its
    // controls, editors and views during application startup.
    function ensure() {
        active = true
        return item
    }
    function open() {
        const popup = ensure()
        if (popup) popup.open()
    }
    function close() {
        if (item) item.close()
    }
    function popup(x, y) {
        const menu = ensure()
        if (menu) {
            if (x !== undefined && y !== undefined) { menu.x = x; menu.y = y; menu.open() }
            else menu.popup()
        }
    }
    onLoaded: if (item && item.hasOwnProperty("parent")) item.parent = popupParent
}
