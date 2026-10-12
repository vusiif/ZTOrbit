#pragma once
#include <ztgui/ZeroTierService.h>
#include <QHostAddress>

namespace vusiif::ztgui::zerotier {
void requestMembers(QObject *context, const QHostAddress &server, const QString &networkId,
                    util::JsonCallback completed);
}
