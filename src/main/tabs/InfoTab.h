#pragma once
#include "../DataTable.h"
class MainWindow;
class JsonPoller;
class InfoTab : public DataTable {
    Q_OBJECT
public:
    explicit InfoTab(MainWindow *mainWindow, QWidget *parent = nullptr);
protected:
    void showEvent(QShowEvent *event) override;
private:
    JsonPoller *m_poller;
};
