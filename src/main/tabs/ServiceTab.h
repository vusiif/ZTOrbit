#pragma once
#include <QWidget>
#include "../ZeroTierClient.h"
class QLabel;
class QPushButton;
class QToolButton;
namespace vusiif::ztgui::zerotier { class JsonPoller; }
namespace vusiif::ztgui::ui {
class MainWindow;
class AnimatedMenu;
class ServiceTab : public QWidget {
    Q_OBJECT
public:
    explicit ServiceTab(MainWindow *mainWindow, QWidget *parent = nullptr);
    void setNodeInfo(const QJsonObject &object);
protected:
    void showEvent(QShowEvent *event) override;
private:
    void updateInstallation();
    void changeService(const QString &action);
    void packageAction(bool uninstall);
    void updateButtons();
    MainWindow *m_main;
    QLabel *m_node;
    QLabel *m_version;
    QLabel *m_statusWarning;
    QLabel *m_feedback;
    QPushButton *m_action;
    QToolButton *m_dropdown;
    AnimatedMenu *m_menu;
    vusiif::ztgui::zerotier::ServiceStatus m_status;
    bool m_busy = false;
};

} // namespace vusiif::ztgui::ui
