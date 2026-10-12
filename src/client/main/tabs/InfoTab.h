#pragma once
#include "../DataTable.h"
namespace vusiif::ztgui::zerotier { class JsonPoller; }
namespace vusiif::ztgui::ui {
class MainWindow;
class InfoTab : public DataTable {
    Q_OBJECT
public:
    explicit InfoTab(MainWindow *mainWindow, QWidget *parent = nullptr);
protected:
    void showEvent(QShowEvent *event) override;
private:
    vusiif::ztgui::zerotier::JsonPoller *m_poller;
};

} // namespace vusiif::ztgui::ui
