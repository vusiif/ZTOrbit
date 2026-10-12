#include "PluginImport.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QLockFile>
namespace vusiif::ztgui::plugins {
QString importPlugin(const QString &source, const QString &directory) {
    const QFileInfo input(source), targetDirectory(directory);
#ifdef Q_OS_WIN
    const QString extension = "dll";
#elif defined(Q_OS_MACOS)
    const QString extension = "dylib";
#else
    const QString extension = "so";
#endif
    if (!input.isFile() || input.suffix().compare(extension, Qt::CaseInsensitive) != 0)
        return QStringLiteral("请选择有效的插件文件。");
    if (targetDirectory.isSymLink() || targetDirectory.isJunction() || (targetDirectory.exists() && !targetDirectory.isDir()))
        return QStringLiteral("插件目录必须是普通目录。");
    if (!QDir().mkpath(directory)) return QStringLiteral("无法创建插件目录。");
    QLockFile lock(QDir(directory).filePath(".import.lock"));
    if (!lock.tryLock()) return QStringLiteral("另一个导入操作正在进行。");
    const auto destination = QDir(directory).filePath(input.fileName());
    const QFileInfo existing(destination);
    if (existing.exists() || existing.isSymLink()) return QStringLiteral("同名插件已存在，请退出程序后手动替换。");
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) return file.errorString();
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) return output.errorString();
    while (!file.atEnd()) {
        const auto bytes = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError) return file.errorString();
        if (output.write(bytes) != bytes.size()) return output.errorString();
    }
    if (QFileInfo::exists(destination)) return QStringLiteral("同名插件已存在。");
    return output.commit() ? QString() : output.errorString();
}
}
