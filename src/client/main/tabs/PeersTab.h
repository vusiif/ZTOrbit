#pragma once
#include "../DataTable.h"
namespace vusiif::ztgui::zerotier { class JsonPoller; }
namespace vusiif::ztgui::ui {
class MainWindow;
class PeersTab : public DataTable {
    Q_OBJECT
public:
    explicit PeersTab(MainWindow *mainWindow, QWidget *parent = nullptr);
protected:
    void showEvent(QShowEvent *event) override;
private:
    vusiif::ztgui::zerotier::JsonPoller *m_poller;
};

} // namespace vusiif::ztgui::ui
