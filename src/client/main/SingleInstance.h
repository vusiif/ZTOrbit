#pragma once
#include <QObject>
#include <QLockFile>
#include <QLocalServer>
#include <memory>
#include <functional>
class QWidget;

namespace vusiif::ztgui::platform {
class SingleInstance final : public QObject {
public:
    enum class Result { Primary, Activated, Failed };
    explicit SingleInstance(QObject *parent = nullptr, const QString &identitySuffix = {});
    Result acquire();
    void setActivation(std::function<void()> activate);
    static void activateWindow(QWidget &window);
    QString error() const { return m_error; }
private:
    QString m_key, m_error;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer m_server;
    std::function<void()> m_activate;
    bool m_pending = false;
};
}
