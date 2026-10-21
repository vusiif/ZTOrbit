#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <ztgui/PluginStorage.h>

namespace vusiif::ztgui::voice_chat {
// Voice owns its business format. config.json is versioned independently from
// the host application version.
struct VoiceConfig {
    QString address;
    QString nickname;
    int volume = 80;
    int inputVolume = 100;
    bool muted = false;
    bool deafened = false;
    QByteArray input;
    QByteArray output;
    bool writable = true;
    QString error;
};

struct VoiceConfigResult {
    VoiceConfig config;
    QString error;
    // Non-fatal problems such as a source key that could not be cleared after
    // the target commit. The one-way import marker stays open in that case.
    QStringList warnings;
};

// Reads the latest config through the host transaction, fills missing fields
// from the one-time legacy QSettings import (target values win), normalizes
// devices and volumes, and commits the target atomically.
VoiceConfigResult loadVoiceConfig(util::PluginStorage &storage);
// Replaces one top-level field without touching other users' concurrent edits.
bool saveVoiceConfigField(util::PluginStorage &storage, const QString &key, const QJsonValue &value,
                          QString &error);
// Member remarks are persistent business data, never a disposable cache.
bool updateMemberRemark(util::PluginStorage &storage, const QString &ztid, const QString &remark,
                        QJsonObject &resultObject, QString &error);
}
