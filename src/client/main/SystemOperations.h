#pragma once
#include <QObject>
#include <functional>

namespace vusiif::zto::ui {
QString explorerSelectParameters(const QString &path);
class SystemOperations : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    void runService(const QString &action, std::function<void(bool)> finished = {});
    virtual void runElevatedScript(const QString &label, const QString &script,
                                   std::function<void(bool)> finished = {});
    // Reveals and selects an existing file in the platform file manager.
    virtual bool revealFile(const QString &path, QString &error);
signals:
    void output(const QString &text);
private:
    void report(const QString &text);
};
}
