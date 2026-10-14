#pragma once

#include <ztgui/ApiVersion.h>

#include <QIcon>
#include <QList>
#include <QString>
#include <functional>

class QQuickItem;
class QQmlEngine;

namespace vusiif::ztgui::api {

inline constexpr char ZeroTierMenuId[] = "builtin/zerotier";
inline constexpr char SettingsMenuId[] = "builtin/settings";

struct PluginInfo {
    QString id;
    QString name;
    QString version;
    int apiMajor = ApiMajor;
    int apiMinor = ApiMinor;
};

// Runs on the GUI thread. Return a new Quick item with both its QObject parent
// and visual parent set to parent. The engine is borrowed from the host.
using PageFactory = std::function<QQuickItem *(QQmlEngine *engine, QQuickItem *parent)>;

struct MenuContribution {
    QString id; // Local to the plugin; the host builds pluginId/menuId.
    QString title;
    QIcon icon;
    QString toolTip;
    int order = 100;
    PageFactory createPage;
};

struct PluginDefinition {
    PluginInfo info;
    QList<MenuContribution> menus;
};

struct RegisteredMenu {
    QString id;
    QString pluginId;
    QString localId;
    QString title;
    QIcon icon;
    QString toolTip;
    int order = 100;
};

enum class Error {
    None,
    WrongThread,
    Busy,
    InvalidPlugin,
    IncompatibleApi,
    DuplicatePlugin,
    InvalidMenu,
    NotFound,
    PageCreationFailed
};

struct Result {
    Error error = Error::None;
    QString message;
    bool ok() const { return error == Error::None; }
};

struct PageResult {
    QQuickItem *page = nullptr;
    Result result;
};

} // namespace vusiif::ztgui::api
