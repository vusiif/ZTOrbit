#pragma once

#include <ztgui/ApiVersion.h>

#include <QIcon>
#include <QList>
#include <QString>
#include <functional>

class QWidget;

namespace vusiif::ztgui::api {

inline constexpr char ZeroTierMenuId[] = "builtin/zerotier";

struct PluginInfo {
    QString id;
    QString name;
    QString version;
    int apiMajor = ApiMajor;
    int apiMinor = ApiMinor;
};

// A factory must return a new widget owned by parent. It runs on the GUI thread.
using PageFactory = std::function<QWidget *(QWidget *parent)>;

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
    QWidget *page = nullptr;
    Result result;
};

} // namespace vusiif::ztgui::api
