#pragma once

// Strict, line-based update plan shared by the Z2 host and z2_updater.exe.
//
// The helper deliberately has no Qt or JSON dependency: the plan is a small
// deterministic text file whose every path is validated here. The same code is
// compiled into the host (to write the plan and to recover interrupted
// transactions), into z2_updater.exe (to execute it) and into the isolated
// transaction tests.
//
// Format (`<key>=<value>` lines, UTF-8, '\n' separators, no quoting):
//
//   z2-update-plan=1
//   version=<package version, informational>
//   install-dir=<absolute Windows path>
//   staging-dir=<absolute Windows path>
//   transaction-dir=<absolute Windows path>
//   runtime-dir=<absolute Windows path>
//   log-file=<absolute Windows path>
//   result-file=<absolute Windows path>
//   parent-pid=<unsigned integer>
//   restart=<relative executable path below install-dir>
//   file=<sha256>|<size>|<relative path>
//   remove=<relative path>
//   end
//
// Every relative path uses '/' separators, must stay below install-dir and may
// never name a host-owned data path (settings.ini, plugins/, plugins-data/,
// logs/, cache/, updates/, .zto/). Values must not contain CR, LF or NUL.

#include <cstdint>
#include <string>
#include <vector>

namespace vusiif::zto::update::updater {

struct PlanFile {
    std::string sha256; // 64 lowercase hex characters
    std::uint64_t size = 0;
    std::string path; // relative to install-dir, '/' separated
};

struct Plan {
    int formatVersion = 1;
    std::string packageVersion;
    std::string installDir;
    std::string stagingDir;
    std::string transactionDir;
    std::string runtimeDir;
    std::string logFile;
    std::string resultFile;
    std::uint64_t parentPid = 0;
    std::string restartRelative;
    std::vector<PlanFile> files;
    std::vector<std::string> removals;
};

// Validates one path relative to the installation directory. Returns false and
// fills `error` for absolute paths, drive letters, "..", Windows reserved device
// names, alternate data streams, control characters and host-owned data paths.
bool isValidRelativePath(const std::string &path, std::string &error);

// True for paths the updater must never replace or remove because the host owns
// them as user data rather than release files.
bool isProtectedInstallPath(const std::string &relativePath);

// Validates an absolute Windows path as written into a plan.
bool isValidAbsolutePath(const std::string &path, std::string &error);

// Joins an absolute installation directory and a validated relative path using
// native ('\\') separators.
std::string joinInstallPath(const std::string &installDir, const std::string &relativePath);

// Normalizes a relative path to the canonical '/' separated form (converts
// '\\', collapses nothing else). Returns an empty string when invalid.
std::string normalizeRelativePath(const std::string &path, std::string &error);

// Full structural validation of a plan, including path safety, duplicate
// detection, hash syntax and absolute-directory rules.
bool validatePlan(const Plan &plan, std::string &error);

// Serializes a validated plan. The caller writes the returned text atomically.
bool serializePlan(const Plan &plan, std::string &out, std::string &error);

// Parses and validates plan text.
bool parsePlan(const std::string &text, Plan &out, std::string &error);

// Small file helpers used by both the host and the helper. They return false
// and describe the failure instead of throwing.
bool readTextFile(const std::string &utf8Path, std::string &out, std::string &error);
bool writeTextFileAtomically(const std::string &utf8Path, const std::string &text, std::string &error);

} // namespace vusiif::zto::update::updater
