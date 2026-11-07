#pragma once

// Transaction engine for z2_updater.exe and for the host-side recovery path.
//
// The engine is deliberately free of Qt and of JSON: it consumes the validated
// line plan from UpdaterPlan.h. The same translation unit is compiled into the
// helper executable, into the Z2 host (recovery of an interrupted transaction)
// and into the isolated transaction tests, so the tested code is exactly the
// code that runs during an update.
//
// Windows is the supported platform. Other platforms get stubs that report a
// clear error instead of silently doing nothing.

#include "UpdaterPlan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vusiif::zto::update::updater {

struct EngineOptions {
    // Test/deployment switch: suppress the post-update restart. The decision is
    // still recorded in the result file and the log.
    bool noRestart = false;
    // Set by the helper itself when it was re-launched from the runtime copy.
    bool relaunched = false;
    // Test-only fault injection: fail right after N replacements succeeded.
    int failAfterReplacements = -1;
    // Finite wait for the Z2 main process to exit normally.
    unsigned waitTimeoutMs = 180000;
};

struct EngineResult {
    bool ok = false;
    std::string step; // plan, wait, staged, backup, replace, verify, remove, commit, restart
    std::string message;
    bool rolledBack = false;
    bool backupRemoved = false;
    bool restartStarted = false;
    bool restartSuppressed = false;
    int exitCode = 0;
};

struct ProcessIdentity {
    std::uint64_t pid = 0;
    std::uint64_t creationTime = 0;
    bool valid = false;
};

ProcessIdentity currentProcessIdentity();

// Opens the process and compares its creation time with the recorded one, so a
// recycled PID is treated as an exited process.
bool processIdentityAlive(std::uint64_t pid, std::uint64_t creationTime);

// Waits until the recorded process exits without ever terminating it. Returns
// false for a timeout or an unexpected wait failure.
bool waitForProcessExit(std::uint64_t pid, unsigned timeoutMs, std::string &error);

std::string sha256FileHex(const std::string &utf8Path, bool &ok, std::string &error);

// True when the plan replaces the executable at `utf8Path` (normally the
// running z2_updater.exe). The helper then re-launches itself from the runtime
// directory so the installed helper can be replaced on Windows.
bool planReplacesPath(const Plan &plan, const std::string &utf8Path);

// Copies this executable into plan.runtimeDir and starts it there with
// `arguments` plus --relaunched. Returns true when the copy was started.
bool relaunchFromRuntimeDirectory(const Plan &plan, const std::string &arguments, std::string &error);

// Executes the whole transaction. Never terminates the parent process, never
// restarts on failure and never removes anything outside the plan.
EngineResult runTransaction(const Plan &plan, const EngineOptions &options);

// Restores the backup of an interrupted transaction and removes its directory.
// The caller must already have verified that the helper is not running.
bool recoverTransaction(const std::string &transactionDir, std::string &error);

// Removes a transaction directory created by the updater (backup included).
bool removeTransactionDirectory(const std::string &transactionDir, std::string &error);

// Writes `<plan.resultFile>` atomically. Used for success, rollback and for
// failures that happened before the transaction started (wait timeout).
bool writeResultFile(const Plan &plan, const EngineResult &result, std::string &error);

} // namespace vusiif::zto::update::updater
