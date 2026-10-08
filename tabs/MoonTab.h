#pragma once
#include "../DataTable.h"
#include <QJsonDocument>
#include <QHash>
#include <functional>
class MainWindow;
class JsonPoller;
class QPushButton;
class QMimeData;
class QDragEnterEvent;
class QDropEvent;
class MoonTab : public DataTable {
    Q_OBJECT
public:
    explicit MoonTab(MainWindow *mainWindow, QWidget *parent = nullptr);
    bool acceptsFiles(const QMimeData *mime) const;
    void importDrop(const QMimeData *mime);
    void addMoon(QWidget *dialogParent = nullptr);
    void importFiles(const QStringList &sources, std::function<void(bool)> finished = {}, QWidget *dialogParent = nullptr);
signals:
    void busyChanged(bool busy);
protected:
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void refreshFiles();
    void deleteMoon(const QString &id);
    MainWindow *m_main;
    JsonPoller *m_poller;
    QPushButton *m_add;
    QHash<QString, QString> m_files;
    QJsonDocument m_document;
    bool m_busy = false;
};
