#include "storage/StorageContext.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {
QString modeName(vusiif::zto::storage::StorageContext::Mode mode)
{
    using vusiif::zto::storage::StorageContext;
    switch (mode) {
    case StorageContext::Mode::Personal: return QStringLiteral("personal");
    case StorageContext::Mode::Ephemeral: return QStringLiteral("ephemeral");
    case StorageContext::Mode::Shared: break;
    }
    return QStringLiteral("shared");
}
} // namespace

// Executed by ReadOnlyProbe.ps1 after a native deny ACE is applied to the
// shared application directory. The probe never touches user directories: both
// roots are passed explicitly and the caller owns the temporary tree.
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QString shared, personal;
    for (int index = 1; index + 1 < argc; ++index) {
        const auto option = QString::fromLocal8Bit(argv[index]);
        if (option == QLatin1String("--shared")) shared = QString::fromLocal8Bit(argv[++index]);
        else if (option == QLatin1String("--personal")) personal = QString::fromLocal8Bit(argv[++index]);
        else return 2;
    }
    if (shared.isEmpty() || personal.isEmpty()) return 2;
    if (!QFileInfo(shared).isDir()) return 3;

    auto context = vusiif::zto::storage::StorageContext::resolveForTesting(shared, personal);
    QString error;
    const bool prepared = context.isValid() && context.prepare(error);
    QJsonObject result{{"valid", context.isValid()},
                       {"mode", modeName(context.mode())},
                       {"shared", shared},
                       {"personalBase", personal},
                       {"personalRoot", context.personalRoot()},
                       {"activeRoot", context.activeRoot()},
                       {"prepared", prepared},
                       {"error", error}};
    if (!context.warnings().isEmpty()) result["warnings"] = QJsonArray::fromStringList(context.warnings());
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    const bool personalRoot = context.mode() == vusiif::zto::storage::StorageContext::Mode::Personal
        && !personal.isEmpty()
        && QDir::cleanPath(context.activeRoot()).startsWith(QDir::cleanPath(personal) + '/');
    return prepared && personalRoot ? 0 : 1;
}
