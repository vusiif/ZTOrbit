#pragma once
#include "../DataTable.h"
class MainWindow;
class JsonPoller;
class PeersTab : public DataTable {
    Q_OBJECT
public:
    explicit PeersTab(MainWindow *mainWindow, QWidget *parent = nullptr);
protected:
    void showEvent(QShowEvent *event) override;
private:
    JsonPoller *m_poller;
};
