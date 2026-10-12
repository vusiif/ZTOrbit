#pragma once
#include "../DataTable.h"
#include <QPointer>
class QLineEdit;
class QPushButton;
namespace vusiif::ztgui::zerotier { class JsonPoller; }
namespace vusiif::ztgui::ui {
class MainWindow;
class AnimatedDialog;
class MoonTab;
class NetworkTab : public DataTable {
    Q_OBJECT
public:
    explicit NetworkTab(MainWindow *mainWindow, QWidget *parent = nullptr);
    void showJoinDialog();
    void showMoons();
    MoonTab *moonTab();
    void importConfigurationFile(const QString &source);
protected:
    void showEvent(QShowEvent *event) override;
private:
    void operate(const QString &action, const QString &id, bool moonConfigured = false);
    void exportMoon(const QString &networkId, const QString &path);
    void importMoon();
    MainWindow *m_main;
    vusiif::ztgui::zerotier::JsonPoller *m_poller;
    QPushButton *m_join;
    QPointer<AnimatedDialog> m_joinDialog;
    QPointer<QLineEdit> m_input;
    QPointer<AnimatedDialog> m_moonDialog;
    MoonTab *m_moon = nullptr;
    bool m_operating = false;
};

} // namespace vusiif::ztgui::ui
