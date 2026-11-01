#include "ZeroTierQueries.h"
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QTimer>
#include <QPointer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QThread>
#include <memory>
namespace vusiif::ztohub {
ZeroTierQueries::ZeroTierQueries(QObject *parent, QUrl base, QString token)
    : QObject(parent), m_base(std::move(base)), m_token(std::move(token)) {
    m_network.setProxy(QNetworkProxy::NoProxy);
}
void ZeroTierQueries::stop() {
    m_stopped = true;
    for (auto *job : m_requests.children()) delete job;
}
void ZeroTierQueries::get(QObject *job, const QString &path, const QByteArray &token, api::JsonCallback callback) {
    QUrl url(m_base); url.setPath(path);
    QNetworkRequest request(url);
    request.setRawHeader("X-ZT1-Auth", token);
    request.setTransferTimeout(10000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = m_network.get(request);
    auto cancelled = std::make_shared<bool>(false);
    connect(job, &QObject::destroyed, reply, [reply, cancelled] {
        *cancelled = true; reply->abort(); reply->deleteLater();
    });
    connect(reply, &QNetworkReply::readyRead, job, [reply] {
        if (reply->bytesAvailable() > 4 * 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, job, [reply, cancelled, callback = std::move(callback)] {
        if (*cancelled) return;
        api::JsonResult result;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200)
            result.error = QStringLiteral("ZeroTier query failed (HTTP %1, network %2)").arg(status).arg(int(reply->error()));
        else {
            QJsonParseError parse;
            result.document = QJsonDocument::fromJson(reply->readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || result.document.isNull()) result.error = "Invalid ZeroTier JSON";
        }
        reply->deleteLater(); callback(result);
    });
}
void ZeroTierQueries::query(const QString &operation, const QJsonObject &parameters, QObject *context, api::JsonCallback callback) {
    if (m_stopped || !callback || !context || QThread::currentThread() != thread() || context->thread() != thread()) return;
    auto *job = new QObject(&m_requests);
    connect(context, &QObject::destroyed, job, [job] { delete job; });
    const QPointer<QObject> alive(context);
    auto done = [job, alive, callback = std::move(callback)](const api::JsonResult &result) {
        job->deleteLater();
        if (alive) callback(result);
    };
    QTimer::singleShot(0, job, [this, job, operation, parameters, done] {
        if (m_requests.children().size() > 64) { done({{}, "Too many pending queries"}); return; }
        const QMap<QString, QString> paths{{"info", "/status"}, {"peers", "/peer"}, {"networks", "/network"}, {"moons", "/moon"}, {"controller.networks", "/controller/network"}};
        QString path = paths.value(operation);
        QString network;
        if (operation == "controller.members") {
            network = parameters.value("networkId").toString().toLower();
            if (!QRegularExpression("^[0-9a-f]{16}$").match(network).hasMatch()) { done({{}, "Invalid network ID"}); return; }
            path = "/controller/network/" + network + "/member";
        }
        if (path.isEmpty()) { done({{}, "Unknown query operation"}); return; }
        QFile file(m_token);
        if (!file.open(QIODevice::ReadOnly)) { done({{}, "ZeroTier authentication file unavailable"}); return; }
        const QByteArray token = file.read(4096).trimmed();
        if (token.isEmpty() || !file.atEnd() || token.contains('\n') || token.contains('\r')) { done({{}, "Invalid ZeroTier authentication file"}); return; }
        get(job, path, token, [this, job, operation, path, token, done](const api::JsonResult &result) {
            if (!result.ok()) { done(result); return; }
            if (operation != "controller.members") {
                const bool valid = operation == "info" ? result.document.isObject() :
                    operation == "controller.networks" ? (result.document.isArray() || result.document.isObject()) : result.document.isArray();
                done(valid ? result : api::JsonResult{{}, "Unexpected ZeroTier JSON type"}); return;
            }
            QStringList ids;
            if (result.document.isObject()) ids = result.document.object().keys();
            else for (const auto &value : result.document.array()) ids.append(value.isString() ? value.toString() : value.toObject().value("address").toString());
            if (ids.size() > 10000) { done({{}, "Too many controller members"}); return; }
            for (const auto &id : ids) if (!QRegularExpression("^[0-9a-fA-F]{10}$").match(id).hasMatch()) { done({{}, "Invalid controller member ID"}); return; }
            if (ids.isEmpty()) { done({QJsonDocument(QJsonArray{}), {}}); return; }
            struct Batch { QStringList ids; QJsonArray rows; int next = 0, active = 0; bool finished = false; };
            auto batch = std::make_shared<Batch>(); batch->ids = ids;
            auto pump = std::make_shared<std::function<void()>>();
            const std::weak_ptr<std::function<void()>> weak = pump;
            auto *timeout = new QTimer(job); timeout->setSingleShot(true); timeout->start(15000);
            connect(timeout, &QTimer::timeout, job, [batch, done] {
                if (!batch->finished) { batch->finished = true; done({{}, "Controller members query timed out"}); }
            });
            *pump = [this, job, path, token, batch, weak, done] {
                if (batch->finished) return;
                while (batch->active < 4 && batch->next < batch->ids.size()) {
                    const auto id = batch->ids[batch->next++]; ++batch->active;
                    const auto keep = weak.lock();
                    get(job, path + '/' + id, token, [batch, keep, done, id](const api::JsonResult &member) {
                        if (batch->finished) return;
                        if (!member.ok() || !member.document.isObject()) {
                            batch->finished = true; done({{}, member.error.isEmpty() ? "Invalid member response" : member.error}); return;
                        }
                        const auto object = member.document.object();
                        batch->rows.append(QJsonObject{{"nodeId", object.value("address").toString(id)}, {"name", object.value("name").toString()},
                            {"authorized", object.value("authorized").toBool()}, {"ipAssignments", object.value("ipAssignments").toArray()}});
                        --batch->active;
                        if (batch->next == batch->ids.size() && batch->active == 0) { batch->finished = true; done({QJsonDocument(batch->rows), {}}); }
                        else if (keep) (*keep)();
                    });
                }
            };
            (*pump)();
        });
    });
}
}
