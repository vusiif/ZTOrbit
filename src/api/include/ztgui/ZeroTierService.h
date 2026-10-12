#pragma once

#include <QJsonDocument>
#include <QString>
#include <functional>

class QObject;

namespace vusiif::ztgui::util {
struct JsonResult {
    QJsonDocument document;
    QString error;
    bool ok() const { return error.isEmpty() && !document.isNull(); }
};
using JsonCallback = std::function<void(const JsonResult &)>;

// Call on the GUI thread with a live GUI-thread QObject context.
// Completion is asynchronous on that thread. Destroying context cancels the
// request and releases its callback. Shutdown cancels all plugin requests.
class ZeroTierService {
public:
    virtual ~ZeroTierService() = default;
    virtual void getZTInfo(QObject *context, JsonCallback completed) = 0;
    virtual void getZTPeers(QObject *context, JsonCallback completed) = 0;
    virtual void getZTNetworks(QObject *context, JsonCallback completed) = 0;
    virtual void getZTMoons(QObject *context, JsonCallback completed) = 0;
};
}
