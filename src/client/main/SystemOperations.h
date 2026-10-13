#pragma once
#include <QObject>
#include <functional>

namespace vusiif::ztgui::ui {
class SystemOperations : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    void runService(const QString &action, std::function<void(bool)> finished = {});
    virtual void runElevatedScript(const QString &label, const QString &script,
                                   std::function<void(bool)> finished = {});
signals:
    void output(const QString &text);
private:
    void report(const QString &text);
};
}
