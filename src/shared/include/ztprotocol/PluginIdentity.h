#pragma once
#include <QString>
#include <QRegularExpression>
namespace vusiif::zto::protocol {
inline bool validPluginId(const QString &id) {
    static const QRegularExpression re("\\A[A-Za-z0-9_-]{1,128}\\z");
    return re.match(id).hasMatch() && id.compare("builtin", Qt::CaseInsensitive) != 0;
}
}
