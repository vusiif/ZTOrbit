#pragma once

#include <QMainWindow>
#include <functional>
#include "ZeroTierClient.h"

class QListWidget;
class QStackedWidget;
class QSplitter;
class QDragEnterEvent;
class QDropEvent;
namespace vusiif::ztgui::plugins { class PluginRegistry; class PluginLoader; }

namespace vusiif::ztgui::ui {
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr, const QString &pluginDirectory = {});
    ~MainWindow() override;
    vusiif::ztgui::plugins::PluginRegistry *pluginRegistry() const { return m_plugins; }
    void runService(const QString &action, std::function<void(bool)> finished = {});
    virtual void runElevatedScript(const QString &label, const QString &script,
                           std::function<void(bool)> finished = {});
    void appendOutput(const QString &text);
    virtual vusiif::ztgui::zerotier::ServiceStatus serviceStatus() const { return vusiif::ztgui::zerotier::serviceStatus(); }
private:
    void setupUi();
    void rebuildNavigation();
    void activateNavigation(const QString &menuId);
    vusiif::ztgui::plugins::PluginRegistry *m_plugins;
    vusiif::ztgui::plugins::PluginLoader *m_pluginLoader = nullptr;
    QWidget *m_zeroTierPage = nullptr;
    QString m_activeMenuId;
    bool m_navigating = false;
    bool m_closing = false;
    bool m_shutdownReady = false;
    bool m_shutdownSlow = false;
    QStringList m_shutdownPending;
    void updateShutdownMessage();
    QListWidget *m_navigation;
    QStackedWidget *m_pages;
    QSplitter *m_horizontal;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};

} // namespace vusiif::ztgui::ui
