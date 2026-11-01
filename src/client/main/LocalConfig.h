#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>

// ZeroTier's system-wide local.conf is the only authority for these settings.
// This module contains the platform-independent validation/merge rules and the
// script generators used when the system directory needs elevation. Nothing
// here stores a second copy of the values in ZTOrbit's settings.ini.
namespace vusiif::zto::zerotier::localconfig {

constexpr int RelayPort = 9443;
constexpr qint64 MaxConfigSize = 1024 * 1024;

struct State {
    bool exists = false;
    QByteArray raw;
    QJsonObject root;
    bool allowTcpFallbackRelay = true;
    bool forceTcpRelay = false;
    QString tcpFallbackRelay; // Native ZeroTier form, for example 1.2.3.4/9443.
    QString relayIp;
    int relayPort = 0;
    QString error;

    bool valid() const { return error.isEmpty(); }
};

struct ReadResult {
    bool ok = false;
    bool permissionDenied = false;
    State state;
    QString error;
};

struct WriteResult {
    bool ok = false;
    bool permissionDenied = false;
    QString error;
};

// Reads and validates local.conf. A missing file is not an error; the returned
// state has exists=false and the documented defaults.
ReadResult read(const QString &path);
// Atomically replaces path after the caller has validated and merged content.
WriteResult writeAtomic(const QString &path, const QByteArray &bytes);
// Creates the documented initial file only when path does not exist yet.
WriteResult createDefault(const QString &path);
State parse(const QByteArray &bytes, bool exists);
QByteArray defaultContent();

bool parseRelay(const QString &text, QString &ip, int &port);
bool parseDisplayRelay(const QString &text, QString &ip, int &port);
QString nativeRelay(const QString &ip, int port);
QString displayRelay(const QString &ip, int port);
QString relayText(const State &state);

// Re-read -> validate -> merge the requested fields. Both switches are merged in
// one document so the caller can never persist allow=false/force=true.
bool mergeSwitches(const State &state, bool allow, bool force, QByteArray &content, QString &error);
bool mergeRelay(const State &state, const QString &ip, int port, QByteArray &content, QString &error);
bool mergeRelayDisplay(const State &state, const QString &display, QByteArray &content, QString &error);

struct Candidate {
    QString networkId;
    QString name;
    QString ip;
    QString relay;
    QString reason;
    QString note;
    bool enabled = false;
    bool current = false;
};

// Uses the same eligibility predicate as .z2bundle export. Moon addresses are
// taken only from the structured listmoons metadata; no .moon binary is parsed.
QList<Candidate> candidates(const QJsonArray &networks, const QJsonDocument &moons,
                            const std::function<bool(const QString &)> &exportable,
                            const State &state);
QString serverIpv4(const QJsonObject &moon, const QString &networkId,
                   QString *reason = nullptr, QString *note = nullptr);
// Returns "IP:9443" only when the local native relay is the same server/port.
QString exportRelay(const State &state, const QString &networkId,
                    const QJsonDocument &moons, QString *reason = nullptr);

struct Install {
    QString path;
    QString proposedPath;
    QString expectedSha256; // Empty when the target did not exist.
};

QString ensureScript(const QString &path, const QString &snapshotPath);
QString installCommands(const Install &install);
QString installScript(const Install &install);

} // namespace vusiif::zto::zerotier::localconfig
