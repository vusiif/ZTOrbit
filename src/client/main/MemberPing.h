#pragma once
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <functional>

namespace vusiif::ztgui::zerotier {
enum class PingState {
    Success,
    Unreachable,
    NoAddress,
    ToolFailure,
};
struct PingResult {
    QString text;
    QString detail;
    PingState state = PingState::Unreachable;
};
QString parsePingReply(const QByteArray &output);
void pingMember(QObject *context, const QHostAddress &address, std::function<void(PingResult)> completed,
                const QString &program = {});
}
