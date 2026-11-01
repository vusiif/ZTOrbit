#pragma once
#include <zto/ZeroTierService.h>
#include <QHostAddress>

namespace vusiif::zto::zerotier {
void requestMembers(QObject *context, const QHostAddress &server, const QString &networkId,
                    util::JsonCallback completed);
}
