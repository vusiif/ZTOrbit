#pragma once

#include <QMainWindow>
#include <functional>
#include "ZeroTierClient.h"

class QListWidget;
class QStackedWidget;
class QSplitter;
class QDragEnterEvent;
class QDropEvent;
namespace ZtGui::Plugins { class PluginRegistry; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    ZtGui::Plugins::PluginRegistry *pluginRegistry() const { return m_plugins; }
    void runService(const QString &action, std::function<void(bool)> finished = {});
    virtual void runElevatedScript(const QString &label, const QString &script,
                           std::function<void(bool)> finished = {});
    void appendOutput(const QString &text);
    virtual ZeroTier::ServiceStatus serviceStatus() const { return ZeroTier::serviceStatus(); }
private:
    void setupUi();
    void rebuildNavigation();
    void activateNavigation(const QString &menuId);
    ZtGui::Plugins::PluginRegistry *m_plugins;
    QWidget *m_zeroTierPage = nullptr;
    QString m_activeMenuId;
    QListWidget *m_navigation;
    QStackedWidget *m_pages;
    QSplitter *m_horizontal;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};
