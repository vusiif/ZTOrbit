#pragma once
#include <QString>
#include <QList>
#include <QPair>

// Shell generators are platform-independent so their quoting can be checked on Windows.
namespace MacPlatform {
QString quote(const QString &value);
QString appleScriptString(const QString &value);
QString serviceCommands(const QString &action, const QString &cli);
QString packageScript(bool uninstall, const QString &dataDirectory);
QString copyMoons(const QList<QPair<QString, QString>> &files, const QString &directory);
QString deleteMoon(const QString &path, const QString &directory);
QString synchronizeSession(const QString &systemHome, const QString &sessionHome, unsigned int userId);
}
