#pragma once
#include <ztohub/HostServices.h>
#include <QNetworkAccessManager>
#include <QUrl>
namespace vusiif::ztohub {
class ZeroTierQueries final : public QObject, public api::ZeroTierQueries {
public:
    explicit ZeroTierQueries(QObject *parent = nullptr,
        QUrl base = QUrl("http://127.0.0.1:9993"), QString token = "/var/lib/zerotier-one/authtoken.secret");
    void query(const QString &, const QJsonObject &, QObject *, api::JsonCallback) override;
    void stop();
private:
    QNetworkAccessManager m_network;
    QUrl m_base;
    QString m_token;
    QObject m_requests;
    bool m_stopped = false;
    void get(QObject *job, const QString &path, const QByteArray &token, api::JsonCallback callback);
};
}
