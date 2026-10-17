#pragma once
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <functional>

namespace vusiif::ztgui::zerotier {
struct PingResult {
    QString text;
    QString detail;
};
QString parsePingReply(const QByteArray &output);
void pingMember(QObject *context, const QHostAddress &address, std::function<void(PingResult)> completed,
                const QString &program = {});
}
