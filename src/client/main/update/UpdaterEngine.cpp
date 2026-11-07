#include "UpdaterEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <tlhelp32.h>
#endif

namespace vusiif::zto::update::updater {
namespace {

#ifdef _WIN32

std::wstring toWide(const std::string &utf8)
{
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        result.data(), length);
    return result;
}

std::string toUtf8(const std::wstring &wide)
{
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length,
                        nullptr, nullptr);
    return result;
}

std::string winError(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : std::wstring();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    std::ostringstream stream;
    stream << "Windows error " << code;
    if (!text.empty()) stream << " (" << toUtf8(text) << ")";
    return stream.str();
}

std::uint64_t fileTimeValue(const FILETIME &time)
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool isReparsePoint(DWORD attributes)
{
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

DWORD attributesOf(const std::wstring &path)
{
    return GetFileAttributesW(path.c_str());
}

std::vector<std::string> splitRelative(const std::string &relative)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : relative) {
        if (ch == '/' || ch == '\\') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

std::wstring joined(const std::wstring &base, const std::wstring &component)
{
    std::wstring result = base;
    while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    result += L'\\';
    result += component;
    return result;
}

bool ensureDirectory(const std::wstring &path, std::string &error)
{
    const DWORD attributes = attributesOf(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (isReparsePoint(attributes)) {
            error = "refusing to use a reparse point as a directory: " + toUtf8(path);
            return false;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = "path exists but is not a directory: " + toUtf8(path);
            return false;
        }
        return true;
    }
    const auto parent = path.find_last_of(L"\\/");
    if (parent != std::wstring::npos && parent > 2) {
        if (!ensureDirectory(path.substr(0, parent), error)) return false;
    }
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "cannot create directory " + toUtf8(path) + ": " + winError(GetLastError());
        return false;
    }
    return true;
}

// Resolves a validated relative plan path below the installation directory and
// rejects reparse points in every component the updater would traverse or
// create. Missing intermediate directories are created for the install step.
bool resolveTargetPath(const std::string &installDir, const std::string &relative, bool createParents,
                       std::wstring &target, std::string &error)
{
    const auto parts = splitRelative(relative);
    if (parts.empty()) {
        error = "empty relative path";
        return false;
    }
    std::wstring base = toWide(installDir);
    if (base.empty()) {
        error = "installation directory is not representable as a path";
        return false;
    }
    const DWORD baseAttributes = attributesOf(base);
    if (baseAttributes == INVALID_FILE_ATTRIBUTES || !(baseAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "installation directory does not exist";
        return false;
    }
    std::wstring current = base;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        current = joined(current, toWide(parts[index]));
        const DWORD attributes = attributesOf(current);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (!createParents) {
                error = "parent directory is missing: " + toUtf8(current);
                return false;
            }
            if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                error = "cannot create directory " + toUtf8(current) + ": " + winError(GetLastError());
                return false;
            }
            continue;
        }
        if (isReparsePoint(attributes)) {
            error = "refusing to traverse a reparse point: " + toUtf8(current);
            return false;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = "path component is not a directory: " + toUtf8(current);
            return false;
        }
    }
    target = joined(current, toWide(parts.back()));
    const DWORD targetAttributes = attributesOf(target);
    if (targetAttributes != INVALID_FILE_ATTRIBUTES) {
        if (isReparsePoint(targetAttributes)) {
            error = "refusing to replace a reparse point: " + toUtf8(target);
            return false;
        }
        if (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            error = "refusing to replace a directory: " + toUtf8(target);
            return false;
        }
    }
    return true;
}

bool deleteTree(const std::wstring &path, std::string &error)
{
    const DWORD attributes = attributesOf(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (isReparsePoint(attributes) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!DeleteFileW(path.c_str())) {
            error = "cannot delete file " + toUtf8(path) + ": " + winError(GetLastError());
            return false;
        }
        return true;
    }
    WIN32_FIND_DATAW data{};
    const auto pattern = joined(path, L"*");
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            if (!deleteTree(joined(path, name), error)) {
                FindClose(handle);
                return false;
            }
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }
    if (!RemoveDirectoryW(path.c_str())) {
        error = "cannot remove directory " + toUtf8(path) + ": " + winError(GetLastError());
        return false;
    }
    return true;
}

bool fileSize(const std::wstring &path, std::uint64_t &size, std::string &error)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        error = winError(GetLastError());
        return false;
    }
    size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

bool hashFile(const std::wstring &path, std::string &hex, std::uint64_t &size, std::string &error)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        error = "cannot open the SHA-256 provider";
        return false;
    }
    std::vector<unsigned char> digest(32);
    bool ok = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open " + toUtf8(path) + ": " + winError(GetLastError());
    } else if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        error = "cannot initialise the SHA-256 hash";
    } else {
        std::vector<unsigned char> buffer(1024 * 1024);
        std::uint64_t total = 0;
        bool failed = false;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                error = "cannot read " + toUtf8(path) + ": " + winError(GetLastError());
                failed = true;
                break;
            }
            if (read == 0) break;
            total += read;
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0))) {
                error = "cannot hash file data";
                failed = true;
                break;
            }
        }
        if (!failed) {
            if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
                error = "cannot finish the SHA-256 hash";
            } else {
                static const char *digits = "0123456789abcdef";
                hex.clear();
                hex.reserve(64);
                for (unsigned char byte : digest) {
                    hex.push_back(digits[byte >> 4]);
                    hex.push_back(digits[byte & 0x0f]);
                }
                size = total;
                ok = true;
            }
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &seconds);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

class Log {
public:
    Log(const std::string &path, std::string &error) : m_path(path)
    {
        const auto directory = std::filesystem::u8path(path).parent_path();
        if (!directory.empty()) {
            std::string directoryError;
            ensureDirectory(directory.wstring(), directoryError);
        }
        m_handle = CreateFileW(toWide(path).c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) error = "cannot open the updater log";
    }
    ~Log()
    {
        if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
    }
    void write(const std::string &line)
    {
        if (m_handle == INVALID_HANDLE_VALUE) return;
        const auto text = "[" + timestamp() + "] " + line + "\r\n";
        DWORD written = 0;
        WriteFile(m_handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        FlushFileBuffers(m_handle);
    }
    bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
private:
    std::string m_path;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

class Journal {
public:
    bool open(const std::string &path, std::string &error)
    {
        m_handle = CreateFileW(toWide(path).c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            error = "cannot create the transaction journal: " + winError(GetLastError());
            return false;
        }
        return true;
    }
    ~Journal()
    {
        close();
    }
    // The transaction directory cannot be removed while this handle is open, so
    // the success path closes the journal explicitly before cleanup.
    void close()
    {
        if (m_handle == INVALID_HANDLE_VALUE) return;
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    bool append(const std::string &operation, const std::string &path = {})
    {
        if (m_handle == INVALID_HANDLE_VALUE) return false;
        const auto line = path.empty() ? operation : operation + "|" + path;
        const auto text = line + "\n";
        DWORD written = 0;
        if (!WriteFile(m_handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) return false;
        FlushFileBuffers(m_handle);
        return true;
    }
private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

struct JournalEntry {
    std::string operation;
    std::string path;
};

std::vector<JournalEntry> readJournal(const std::string &path)
{
    std::vector<JournalEntry> entries;
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        JournalEntry entry;
        const auto bar = line.find('|');
        entry.operation = bar == std::string::npos ? line : line.substr(0, bar);
        if (bar != std::string::npos) entry.path = line.substr(bar + 1);
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool journalContains(const std::vector<JournalEntry> &entries, const std::string &operation)
{
    return std::any_of(entries.begin(), entries.end(),
                       [&](const JournalEntry &entry) { return entry.operation == operation; });
}

bool copyFileBytes(const std::wstring &source, const std::wstring &target, bool failIfExists, std::string &error)
{
    if (!CopyFileW(source.c_str(), target.c_str(), failIfExists ? TRUE : FALSE)) {
        error = "cannot copy " + toUtf8(source) + " to " + toUtf8(target) + ": " + winError(GetLastError());
        return false;
    }
    return true;
}

bool rollback(const Plan &plan, const std::vector<JournalEntry> &entries, std::string &error)
{
    bool ok = true;
    std::vector<std::string> restored;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->operation != "installed" && it->operation != "removed") continue;
        if (std::find(restored.begin(), restored.end(), it->path) != restored.end()) continue;
        restored.push_back(it->path);
        const std::string backupRelative = it->path;
        std::wstring backup;
        std::string pathError;
        if (!resolveTargetPath(plan.transactionDir, "backup/" + backupRelative, false, backup, pathError)) {
            // The backup may be missing when the failure happened before it.
            backup.clear();
        }
        std::wstring target;
        if (!resolveTargetPath(plan.installDir, it->path, true, target, pathError)) {
            error = pathError;
            ok = false;
            continue;
        }
        std::uint64_t size = 0;
        std::string ignored;
        const bool haveBackup = !backup.empty() && fileSize(backup, size, ignored);
        if (haveBackup) {
            if (!copyFileBytes(backup, target, false, error)) ok = false;
        } else {
            const DWORD attributes = attributesOf(target);
            if (attributes != INVALID_FILE_ATTRIBUTES && !DeleteFileW(target.c_str())) {
                error = "cannot delete newly installed file " + toUtf8(target) + ": " + winError(GetLastError());
                ok = false;
            }
        }
    }
    return ok;
}

void appendLogLine(std::string &text, const std::string &line)
{
    text += line;
    text += "\n";
}

#endif // _WIN32

} // namespace

#ifdef _WIN32

ProcessIdentity currentProcessIdentity()
{
    ProcessIdentity identity;
    identity.pid = GetCurrentProcessId();
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        identity.creationTime = fileTimeValue(creation);
        identity.valid = true;
    }
    return identity;
}

bool processIdentityAlive(std::uint64_t pid, std::uint64_t creationTime)
{
    if (pid == 0 || pid > 0xffffffffULL) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process) return false;
    bool alive = false;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        alive = fileTimeValue(creation) == creationTime;
    }
    CloseHandle(process);
    return alive;
}

bool waitForProcessExit(std::uint64_t pid, unsigned timeoutMs, std::string &error)
{
    error.clear();
    if (pid == 0) {
        error = "the plan does not name a parent process";
        return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process) {
        // The documented normal case: the main process already exited.
        return true;
    }
    const DWORD result = WaitForSingleObject(process, timeoutMs);
    CloseHandle(process);
    if (result == WAIT_OBJECT_0) return true;
    if (result == WAIT_TIMEOUT) {
        error = "timed out waiting for the main process to exit";
        return false;
    }
    error = "waiting for the main process failed: " + winError(GetLastError());
    return false;
}

std::string sha256FileHex(const std::string &utf8Path, bool &ok, std::string &error)
{
    std::string hex;
    std::uint64_t size = 0;
    ok = hashFile(toWide(utf8Path), hex, size, error);
    return hex;
}

bool planReplacesPath(const Plan &plan, const std::string &utf8Path)
{
    std::wstring own = toWide(utf8Path);
    std::replace(own.begin(), own.end(), L'/', L'\\');
    for (const auto &file : plan.files) {
        std::wstring target;
        std::string error;
        if (!resolveTargetPath(plan.installDir, file.path, false, target, error)) continue;
        if (CompareStringOrdinal(target.c_str(), -1, own.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}

bool relaunchFromRuntimeDirectory(const Plan &plan, const std::string &arguments, std::string &error)
{
    wchar_t ownPath[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, ownPath, 32768);
    if (length == 0 || length >= 32768) {
        error = "cannot determine the helper executable path";
        return false;
    }
    std::string directoryError;
    if (!ensureDirectory(toWide(plan.runtimeDir), directoryError)) {
        error = directoryError;
        return false;
    }
    const std::wstring source = ownPath;
    const std::wstring target = joined(toWide(plan.runtimeDir),
                                       L"z2_updater-run-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                                           + std::to_wstring(GetTickCount64()) + L".exe");
    if (!copyFileBytes(source, target, true, error)) return false;
    std::wstring command = L"\"" + target + L"\" " + toWide(arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        toWide(plan.installDir).c_str(), &startup, &process)) {
        error = "cannot start the runtime copy of the updater: " + winError(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool writeResultFile(const Plan &plan, const EngineResult &result, std::string &error)
{
    const auto parent = std::filesystem::u8path(plan.resultFile).parent_path();
    if (!parent.empty()) {
        std::string directoryError;
        if (!ensureDirectory(parent.wstring(), directoryError)) {
            error = directoryError;
            return false;
        }
    }
    std::string text;
    appendLogLine(text, std::string("result=") + (result.ok ? "ok" : "failed"));
    appendLogLine(text, "step=" + result.step);
    std::string message = result.message;
    for (char &ch : message) {
        if (ch == '\r' || ch == '\n') ch = ' ';
    }
    appendLogLine(text, "message=" + message);
    appendLogLine(text, "version=" + plan.packageVersion);
    appendLogLine(text, std::string("restart=")
                            + (result.restartStarted ? "started"
                                                     : result.restartSuppressed ? "suppressed"
                                                                                : "not-requested"));
    appendLogLine(text, "time=" + timestamp());
    return writeTextFileAtomically(plan.resultFile, text, error);
}

EngineResult runTransaction(const Plan &plan, const EngineOptions &options)
{
    EngineResult result;
    result.step = "plan";
    Journal journal;
    std::string error;
    if (!validatePlan(plan, error)) {
        result.message = error;
        result.exitCode = 2;
        return result;
    }
    Log log(plan.logFile, error);
    auto fail = [&](const std::string &step, const std::string &message, bool rolledBack) {
        result.ok = false;
        result.step = step;
        result.message = message;
        result.rolledBack = rolledBack;
        result.exitCode = step == "wait" ? 4 : 3;
        log.write("FAILED " + step + ": " + message);
        std::string writeError;
        if (!writeResultFile(plan, result, writeError)) log.write("cannot write result file: " + writeError);
        return result;
    };
    auto rollbackAndFail = [&](const std::string &step, const std::string &message) {
        std::string rollbackError;
        const bool restored = rollback(plan, readJournal(plan.transactionDir + "\\journal.txt"), rollbackError);
        journal.append("rolled-back");
        std::string detail = message;
        if (!restored) detail += " (rollback incomplete: " + rollbackError + ")";
        result.ok = false;
        result.step = step;
        result.message = detail;
        result.rolledBack = restored;
        result.exitCode = 3;
        log.write("FAILED " + step + ": " + detail);
        std::string writeError;
        if (!writeResultFile(plan, result, writeError)) log.write("cannot write result file: " + writeError);
        return result;
    };
    log.write("plan " + plan.packageVersion + " received for " + plan.installDir);

    std::wstring installRoot;
    std::string pathError;
    if (!ensureDirectory(toWide(plan.installDir), pathError)) return fail("plan", pathError, false);
    if (!ensureDirectory(toWide(plan.stagingDir), pathError)) return fail("plan", pathError, false);
    if (!ensureDirectory(toWide(plan.runtimeDir), pathError)) return fail("plan", pathError, false);
    installRoot = toWide(plan.installDir);
    if (!ensureDirectory(toWide(plan.transactionDir), pathError)) return fail("plan", pathError, false);

    // Verify every staged source before touching the installation.
    for (const auto &file : plan.files) {
        std::wstring source;
        if (!resolveTargetPath(plan.stagingDir, file.path, false, source, pathError))
            return fail("staged", pathError, false);
        std::string hex;
        std::uint64_t size = 0;
        if (!hashFile(source, hex, size, pathError)) return fail("staged", pathError, false);
        if (hex != file.sha256 || size != file.size)
            return fail("staged", "staged file does not match the plan: " + file.path, false);
    }
    log.write("staged files verified");

    if (!journal.open(plan.transactionDir + "\\journal.txt", error)) return fail("plan", error, false);
    journal.append("started");
    {
        std::string planText;
        if (!serializePlan(plan, planText, error)) return fail("plan", error, false);
        if (!writeTextFileAtomically(plan.transactionDir + "\\plan.txt", planText, error))
            return fail("plan", error, false);
    }
    const auto identity = currentProcessIdentity();
    {
        std::string identityText = std::to_string(identity.pid) + "|" + std::to_string(identity.creationTime);
        if (!writeTextFileAtomically(plan.transactionDir + "\\helper.pid", identityText, error))
            return fail("plan", error, false);
    }
    log.write("transaction journal created");

    // Back up every file that will be replaced or removed.
    std::vector<std::string> targets;
    targets.reserve(plan.files.size() + plan.removals.size());
    for (const auto &file : plan.files) targets.push_back(file.path);
    for (const auto &removal : plan.removals) targets.push_back(removal);
    for (const auto &relative : targets) {
        std::wstring target;
        if (!resolveTargetPath(plan.installDir, relative, false, target, pathError)) {
            // A missing parent directory simply means the file does not exist yet.
            if (pathError.find("parent directory is missing") == std::string::npos)
                return fail("backup", pathError, false);
        }
        const DWORD attributes = target.empty() ? INVALID_FILE_ATTRIBUTES : attributesOf(target);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            journal.append("new", relative);
            log.write("new file " + relative);
            continue;
        }
        std::wstring backup;
        if (!resolveTargetPath(plan.transactionDir, "backup/" + relative, true, backup, pathError)) {
            // Create the backup parent tree explicitly; resolveTargetPath refuses
            // missing parents unless asked, and the transaction root exists.
            return fail("backup", pathError, false);
        }
        if (!ensureDirectory(backup.substr(0, backup.find_last_of(L"\\/")), pathError))
            return fail("backup", pathError, false);
        if (!copyFileBytes(target, backup, false, pathError)) return fail("backup", pathError, false);
        journal.append("backup", relative);
        log.write("backup " + relative);
    }

    // Replace files, verifying each installed copy before the next step.
    int installedCount = 0;
    for (const auto &file : plan.files) {
        if (options.failAfterReplacements >= 0 && installedCount >= options.failAfterReplacements) {
            return rollbackAndFail("replace", "injected replacement failure (test mode)");
        }
        std::wstring source;
        if (!resolveTargetPath(plan.stagingDir, file.path, false, source, pathError))
            return fail("replace", pathError, false);
        std::wstring target;
        if (!resolveTargetPath(plan.installDir, file.path, true, target, pathError)) {
            return rollbackAndFail("replace", pathError);
        }
        const DWORD targetAttributes = attributesOf(target);
        if (targetAttributes != INVALID_FILE_ATTRIBUTES && !DeleteFileW(target.c_str())) {
            return rollbackAndFail("replace", "cannot remove the old file: " + toUtf8(target) + ": "
                                       + winError(GetLastError()));
        }
        if (!copyFileBytes(source, target, false, pathError) || !SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL)) {
            return rollbackAndFail("replace", pathError);
        }
        std::string hex;
        std::uint64_t size = 0;
        if (!hashFile(target, hex, size, pathError)) {
            return rollbackAndFail("verify", pathError);
        }
        if (hex != file.sha256 || size != file.size) {
            return rollbackAndFail("verify", "installed file hash mismatch: " + file.path);
        }
        journal.append("installed", file.path);
        log.write("installed " + file.path);
        ++installedCount;
    }

    // Explicit old-release whitelist: only the paths the manifest named.
    for (const auto &removal : plan.removals) {
        std::wstring target;
        if (!resolveTargetPath(plan.installDir, removal, false, target, pathError)) {
            if (pathError.find("parent directory is missing") != std::string::npos) continue;
            return rollbackAndFail("remove", pathError);
        }
        const DWORD attributes = attributesOf(target);
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        if (!DeleteFileW(target.c_str())) {
            return rollbackAndFail("remove", "cannot remove " + toUtf8(target) + ": " + winError(GetLastError()));
        }
        journal.append("removed", removal);
        log.write("removed " + removal);
    }

    // Final pass over every installed file before the backup is cleared.
    for (const auto &file : plan.files) {
        std::wstring target;
        if (!resolveTargetPath(plan.installDir, file.path, false, target, pathError))
            return rollbackAndFail("verify", pathError);
        std::string hex;
        std::uint64_t size = 0;
        if (!hashFile(target, hex, size, pathError)) return rollbackAndFail("verify", pathError);
        if (hex != file.sha256 || size != file.size)
            return rollbackAndFail("verify", "final verification failed: " + file.path);
    }
    journal.append("verified");
    log.write("all files verified");

    {
        std::string backupError;
        if (!deleteTree(toWide(plan.transactionDir) + L"\\backup", backupError)) {
            // A retained backup is not a data-loss problem; report it but keep
            // the successful install and let the next start remove the tree.
            log.write("cannot remove backup: " + backupError);
        } else {
            result.backupRemoved = true;
        }
    }
    journal.append("committed");
    result.ok = true;
    result.step = "commit";
    result.exitCode = 0;
    log.write("transaction committed");

    if (options.noRestart) {
        result.restartSuppressed = true;
        log.write("restart suppressed by request (test mode)");
    } else {
        std::wstring exe;
        if (!resolveTargetPath(plan.installDir, plan.restartRelative, false, exe, pathError)) {
            result.ok = false;
            result.step = "restart";
            result.message = pathError;
            result.exitCode = 5;
            log.write("restart failed: " + pathError);
            std::string writeError;
            if (!writeResultFile(plan, result, writeError)) log.write("cannot write result file: " + writeError);
            return result;
        }
        std::wstring command = L"\"" + exe + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            installRoot.c_str(), &startup, &process)) {
            result.ok = false;
            result.step = "restart";
            result.message = "cannot restart the application: " + winError(GetLastError());
            result.exitCode = 5;
            log.write(result.message);
            std::string writeError;
            if (!writeResultFile(plan, result, writeError)) log.write("cannot write result file: " + writeError);
            return result;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        result.restartStarted = true;
        log.write("application restarted");
    }

    std::string writeError;
    if (!writeResultFile(plan, result, writeError)) log.write("cannot write result file: " + writeError);

    // The staging tree and the transaction journal are no longer needed; the
    // installed files were verified and the backup is gone. Leftovers from an
    // interrupted run are handled by recoverTransaction() on the next start.
    journal.close();
    std::string cleanupError;
    if (!removeTransactionDirectory(plan.transactionDir, cleanupError))
        log.write("cannot remove the transaction directory: " + cleanupError);
    return result;
}

bool recoverTransaction(const std::string &transactionDir, std::string &error)
{
    std::string planText;
    std::string pathError;
    if (!readTextFile(transactionDir + "\\plan.txt", planText, pathError)) {
        // No plan was written: the helper did not get past creating the folder.
        return removeTransactionDirectory(transactionDir, error);
    }
    Plan plan;
    if (!parsePlan(planText, plan, pathError)) {
        error = "cannot parse the interrupted plan: " + pathError;
        return false;
    }
    const auto entries = readJournal(transactionDir + "\\journal.txt");
    if (!journalContains(entries, "committed") && !journalContains(entries, "rolled-back")
        && !journalContains(entries, "recovered")) {
        std::string rollbackError;
        if (!rollback(plan, entries, rollbackError)) {
            error = "cannot restore the backup of an interrupted update: " + rollbackError;
            return false;
        }
        // Keep an audit trail inside the transaction folder only.
        std::ofstream journal(std::filesystem::u8path(transactionDir + "\\journal.txt"), std::ios::app);
        if (journal) journal << "recovered\n";
    }
    return removeTransactionDirectory(transactionDir, error);
}

bool removeTransactionDirectory(const std::string &transactionDir, std::string &error)
{
    return deleteTree(toWide(transactionDir), error);
}

#else // _WIN32

ProcessIdentity currentProcessIdentity()
{
    ProcessIdentity identity;
    identity.pid = 0;
    identity.valid = false;
    return identity;
}

bool processIdentityAlive(std::uint64_t, std::uint64_t)
{
    return false;
}

bool waitForProcessExit(std::uint64_t, unsigned, std::string &error)
{
    error = "automatic updates are implemented for Windows only";
    return false;
}

std::string sha256FileHex(const std::string &, bool &ok, std::string &error)
{
    ok = false;
    error = "automatic updates are implemented for Windows only";
    return {};
}

bool planReplacesPath(const Plan &, const std::string &)
{
    return false;
}

bool relaunchFromRuntimeDirectory(const Plan &, const std::string &, std::string &error)
{
    error = "automatic updates are implemented for Windows only";
    return false;
}

bool writeResultFile(const Plan &, const EngineResult &, std::string &error)
{
    error = "automatic updates are implemented for Windows only";
    return false;
}

EngineResult runTransaction(const Plan &, const EngineOptions &)
{
    EngineResult result;
    result.step = "plan";
    result.message = "automatic updates are implemented for Windows only";
    result.exitCode = 2;
    return result;
}

bool recoverTransaction(const std::string &, std::string &error)
{
    error = "automatic updates are implemented for Windows only";
    return false;
}

bool removeTransactionDirectory(const std::string &, std::string &error)
{
    error = "automatic updates are implemented for Windows only";
    return false;
}

#endif // _WIN32

} // namespace vusiif::zto::update::updater
