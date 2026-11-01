#pragma once
#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QVideoFrame>
#include <functional>
#include <memory>

namespace vusiif::zto::media::engine {
// Worker-private Windows Graphics Capture adapter. No native handles or WinRT
// types cross the media service boundary.
class WindowsWindowCapture final : public QObject {
public:
    struct Window {
        quintptr handle = 0;
        quint32 process = 0, thread = 0;
        QString title;
        bool operator==(const Window &other) const {
            return handle == other.handle && process == other.process && thread == other.thread;
        }
    };
    static QList<Window> windows();
    explicit WindowsWindowCapture(QObject *parent = nullptr);
    ~WindowsWindowCapture() override;
    QString start(const Window &window, int frameRate);
    void stop();
    bool isActive() const;
    QJsonObject statistics() const;
    std::function<void(QVideoFrame)> frame;
    std::function<void(QString)> failed;
private:
    struct State;
    std::unique_ptr<State> state;
    void poll();
};
}
