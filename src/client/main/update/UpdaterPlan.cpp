#include "UpdaterPlan.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vusiif::zto::update::updater {
namespace {

constexpr std::size_t MaxRelativePathLength = 240;
constexpr std::size_t MaxAbsolutePathLength = 900;

std::string lowerAscii(std::string value)
{
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : path) {
        if (ch == '/') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

bool isReservedDeviceName(const std::string &component)
{
    const auto lowered = lowerAscii(component);
    const auto dot = lowered.find('.');
    const auto stem = dot == std::string::npos ? lowered : lowered.substr(0, dot);
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
    if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0)) {
        return stem[3] >= '1' && stem[3] <= '9';
    }
    return false;
}

bool containsControlOrForbidden(const std::string &value)
{
    for (char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch < 0x20 || ch == 0x7f) return true;
        switch (raw) {
        case '<': case '>': case ':': case '"': case '|': case '?': case '*':
            return true;
        default:
            break;
        }
    }
    return false;
}

bool validateComponent(const std::string &component, std::string &error)
{
    if (component.empty()) {
        error = "path component is empty";
        return false;
    }
    if (component.size() > 255) {
        error = "path component is too long";
        return false;
    }
    if (component.back() == '.' || component.back() == ' ') {
        error = "path component ends with a dot or space";
        return false;
    }
    if (component == "." || component == "..") {
        error = "relative path contains a traversal component";
        return false;
    }
    if (isReservedDeviceName(component)) {
        error = "path uses a reserved Windows device name";
        return false;
    }
    return true;
}

bool containsNewline(const std::string &value)
{
    return value.find('\n') != std::string::npos || value.find('\r') != std::string::npos
        || value.find('\0') != std::string::npos;
}

bool equalsIgnoreCase(const std::string &left, const std::string &right)
{
    return lowerAscii(left) == lowerAscii(right);
}

bool startsWithIgnoreCase(const std::string &value, const std::string &prefix)
{
    if (value.size() < prefix.size()) return false;
    return equalsIgnoreCase(value.substr(0, prefix.size()), prefix);
}

bool isHexDigest(const std::string &value)
{
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::filesystem::path nativePath(const std::string &utf8)
{
    return std::filesystem::u8path(utf8);
}

std::string utf8Path(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

bool isValidAbsolutePath(const std::string &path, std::string &error)
{
    error.clear();
    if (path.empty() || path.size() > MaxAbsolutePathLength) {
        error = "absolute path is empty or too long";
        return false;
    }
    for (std::size_t index = 0; index < path.size(); ++index) {
        const auto ch = static_cast<unsigned char>(path[index]);
        if (ch < 0x20 || ch == 0x7f) {
            error = "absolute path contains a control character";
            return false;
        }
        if (ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
            error = "absolute path contains forbidden characters";
            return false;
        }
        if (ch == ':' && index != 1) {
            error = "absolute path contains a colon outside the drive letter";
            return false;
        }
    }
    const bool drive = path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0]))
        && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    const bool unc = path.size() >= 3 && path[0] == '\\' && path[1] == '\\';
    if (!drive && !unc) {
        error = "path is not an absolute Windows path";
        return false;
    }
    std::string normalized;
    if (drive) normalized = path.substr(0, 2) + "\\" + path.substr(3);
    else normalized = path;
    const auto parts = splitPath(normalized);
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (parts[index].empty()) continue;
        if (parts[index] == "." || parts[index] == "..") {
            error = "absolute path contains a traversal component";
            return false;
        }
    }
    if (path.back() == ' ' || path.back() == '.') {
        error = "absolute path ends with a space or dot";
        return false;
    }
    return true;
}

std::string normalizeRelativePath(const std::string &path, std::string &error)
{
    error.clear();
    if (path.empty()) {
        error = "relative path is empty";
        return {};
    }
    if (path.size() > MaxRelativePathLength) {
        error = "relative path is too long";
        return {};
    }
    if (path.find('\n') != std::string::npos || path.find('\r') != std::string::npos
        || path.find('\0') != std::string::npos) {
        error = "relative path contains a newline or NUL";
        return {};
    }
    if (path[0] == '/' || path[0] == '\\') {
        error = "relative path must not be absolute";
        return {};
    }
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        error = "relative path must not contain a drive letter";
        return {};
    }
    std::string canonical;
    canonical.reserve(path.size());
    for (char ch : path) canonical.push_back(ch == '\\' ? '/' : ch);
    if (canonical.find(':') != std::string::npos) {
        error = "relative path must not contain an alternate data stream";
        return {};
    }
    if (containsControlOrForbidden(canonical)) {
        error = "relative path contains forbidden characters";
        return {};
    }
    if (canonical.back() == '/') {
        error = "relative path must name a file";
        return {};
    }
    auto parts = splitPath(canonical);
    for (const auto &part : parts) {
        if (!validateComponent(part, error)) return {};
    }
    return canonical;
}

bool isProtectedInstallPath(const std::string &relativePath)
{
    const auto lowered = lowerAscii(relativePath);
    if (lowered == "settings.ini") return true;
    static const char *const prefixes[] = {
        "plugins/", "plugins-data/", "logs/", "cache/", "updates/", ".zto/",
        "settings.ini/", // pathological but still host-owned
    };
    for (const char *prefix : prefixes) {
        if (startsWithIgnoreCase(lowered, prefix)) return true;
    }
    return false;
}

bool isValidRelativePath(const std::string &path, std::string &error)
{
    const auto normalized = normalizeRelativePath(path, error);
    if (normalized.empty()) return false;
    if (isProtectedInstallPath(normalized)) {
        error = "path belongs to host-owned user data";
        return false;
    }
    return true;
}

std::string joinInstallPath(const std::string &installDir, const std::string &relativePath)
{
    std::string result = installDir;
    while (!result.empty() && (result.back() == '\\' || result.back() == '/')) result.pop_back();
    result += '\\';
    for (char ch : relativePath) result.push_back(ch == '/' ? '\\' : ch);
    return result;
}

bool validatePlan(const Plan &plan, std::string &error)
{
    error.clear();
    if (plan.formatVersion != 1) {
        error = "unsupported plan format version";
        return false;
    }
    if (!isValidAbsolutePath(plan.installDir, error)) return false;
    if (!isValidAbsolutePath(plan.stagingDir, error)) return false;
    if (!isValidAbsolutePath(plan.transactionDir, error)) return false;
    if (!isValidAbsolutePath(plan.runtimeDir, error)) return false;
    if (!isValidAbsolutePath(plan.logFile, error)) return false;
    if (!isValidAbsolutePath(plan.resultFile, error)) return false;
    if (plan.parentPid == 0) {
        error = "parent-pid must be a positive process id";
        return false;
    }
    {
        std::string restartError;
        const auto restart = normalizeRelativePath(plan.restartRelative, restartError);
        if (restart.empty()) {
            error = "restart executable: " + restartError;
            return false;
        }
        if (restart.find('/') != std::string::npos) {
            error = "restart executable must be a file directly below install-dir";
            return false;
        }
        const auto lower = lowerAscii(restart);
        if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".exe") != 0) {
            error = "restart executable must be an .exe";
            return false;
        }
        if (isProtectedInstallPath(restart)) {
            error = "restart executable must not be a host-owned data path";
            return false;
        }
    }
    struct DirectoryEntry {
        const char *name;
        const std::string *value;
    };
    const DirectoryEntry directories[] = {
        {"staging-dir", &plan.stagingDir}, {"transaction-dir", &plan.transactionDir},
        {"runtime-dir", &plan.runtimeDir}, {"log-file", &plan.logFile},
        {"result-file", &plan.resultFile}};
    for (const auto &entry : directories) {
        if (equalsIgnoreCase(plan.installDir, *entry.value)) {
            error = std::string(entry.name) + " must differ from install-dir";
            return false;
        }
    }
    if (equalsIgnoreCase(plan.stagingDir, plan.transactionDir)
        || equalsIgnoreCase(plan.stagingDir, plan.runtimeDir)
        || equalsIgnoreCase(plan.transactionDir, plan.runtimeDir)) {
        error = "staging, transaction and runtime directories must differ";
        return false;
    }
    if (plan.files.empty() && plan.removals.empty()) {
        error = "plan does not name any file";
        return false;
    }
    if (containsNewline(plan.packageVersion)) {
        error = "package version contains a newline";
        return false;
    }
    std::vector<std::string> seen;
    seen.reserve(plan.files.size() + plan.removals.size());
    for (const auto &file : plan.files) {
        std::string pathError;
        const auto normalized = isValidRelativePath(file.path, pathError) ? normalizeRelativePath(file.path, pathError) : std::string();
        if (normalized.empty()) {
            error = "plan file path: " + pathError;
            return false;
        }
        if (!isHexDigest(file.sha256)) {
            error = "plan file hash for '" + normalized + "' is not a lowercase SHA-256 digest";
            return false;
        }
        const auto lower = lowerAscii(normalized);
        if (std::find(seen.begin(), seen.end(), lower) != seen.end()) {
            error = "plan lists '" + normalized + "' more than once";
            return false;
        }
        seen.push_back(lower);
        const auto full = joinInstallPath(plan.installDir, normalized);
        if (full.size() > MaxAbsolutePathLength) {
            error = "plan target path is too long";
            return false;
        }
    }
    for (const auto &removal : plan.removals) {
        std::string pathError;
        const auto normalized = isValidRelativePath(removal, pathError) ? normalizeRelativePath(removal, pathError) : std::string();
        if (normalized.empty()) {
            error = "plan removal path: " + pathError;
            return false;
        }
        const auto lower = lowerAscii(normalized);
        if (std::find(seen.begin(), seen.end(), lower) != seen.end()) {
            error = "plan lists '" + normalized + "' more than once";
            return false;
        }
        seen.push_back(lower);
    }
    return true;
}

bool serializePlan(const Plan &plan, std::string &out, std::string &error)
{
    out.clear();
    if (!validatePlan(plan, error)) return false;
    auto value = [](const std::string &text) { return text; };
    std::ostringstream stream;
    stream << "z2-update-plan=" << plan.formatVersion << "\n";
    stream << "version=" << value(plan.packageVersion) << "\n";
    stream << "install-dir=" << value(plan.installDir) << "\n";
    stream << "staging-dir=" << value(plan.stagingDir) << "\n";
    stream << "transaction-dir=" << value(plan.transactionDir) << "\n";
    stream << "runtime-dir=" << value(plan.runtimeDir) << "\n";
    stream << "log-file=" << value(plan.logFile) << "\n";
    stream << "result-file=" << value(plan.resultFile) << "\n";
    stream << "parent-pid=" << plan.parentPid << "\n";
    stream << "restart=" << value(plan.restartRelative) << "\n";
    for (const auto &file : plan.files) {
        std::string pathError;
        const auto normalized = normalizeRelativePath(file.path, pathError);
        stream << "file=" << file.sha256 << "|" << file.size << "|" << normalized << "\n";
    }
    for (const auto &removal : plan.removals) {
        std::string pathError;
        stream << "remove=" << normalizeRelativePath(removal, pathError) << "\n";
    }
    stream << "end\n";
    out = stream.str();
    return true;
}

bool parsePlan(const std::string &text, Plan &out, std::string &error)
{
    out = Plan{};
    bool sawHeader = false;
    bool sawEnd = false;
    std::istringstream stream(text);
    std::string line;
    int lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        // The end marker is the only line without a key=value payload.
        if (line == "end") {
            sawEnd = true;
            break;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            error = "line " + std::to_string(lineNumber) + " is not a key=value line";
            return false;
        }
        const auto key = line.substr(0, equals);
        const auto raw = line.substr(equals + 1);
        auto invalid = [&](const char *detail) {
            error = "line " + std::to_string(lineNumber) + ": " + detail;
            return false;
        };
        if (containsNewline(raw)) return invalid("value contains a newline");
        if (key == "z2-update-plan") {
            if (raw != "1") return invalid("unsupported plan format");
            sawHeader = true;
        } else if (key == "end") {
            if (!raw.empty()) return invalid("unexpected end marker payload");
            sawEnd = true;
        } else if (key == "version") {
            out.packageVersion = raw;
        } else if (key == "install-dir") {
            out.installDir = raw;
        } else if (key == "staging-dir") {
            out.stagingDir = raw;
        } else if (key == "transaction-dir") {
            out.transactionDir = raw;
        } else if (key == "runtime-dir") {
            out.runtimeDir = raw;
        } else if (key == "log-file") {
            out.logFile = raw;
        } else if (key == "result-file") {
            out.resultFile = raw;
        } else if (key == "parent-pid") {
            if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos)
                return invalid("parent-pid must be a decimal process id");
            try {
                out.parentPid = static_cast<std::uint64_t>(std::stoull(raw));
            } catch (...) {
                return invalid("parent-pid is out of range");
            }
        } else if (key == "restart") {
            out.restartRelative = raw;
        } else if (key == "file") {
            const auto first = raw.find('|');
            const auto second = first == std::string::npos ? std::string::npos : raw.find('|', first + 1);
            if (first == std::string::npos || second == std::string::npos)
                return invalid("file line needs sha256|size|path");
            PlanFile file;
            file.sha256 = raw.substr(0, first);
            const auto sizeText = raw.substr(first + 1, second - first - 1);
            if (sizeText.empty() || sizeText.find_first_not_of("0123456789") != std::string::npos)
                return invalid("file size must be decimal");
            try {
                file.size = static_cast<std::uint64_t>(std::stoull(sizeText));
            } catch (...) {
                return invalid("file size is out of range");
            }
            file.path = raw.substr(second + 1);
            out.files.push_back(std::move(file));
        } else if (key == "remove") {
            out.removals.push_back(raw);
        } else {
            return invalid("unknown plan key");
        }
        if (sawEnd) break;
    }
    if (sawEnd) {
        // No trailing content is tolerated: a second document after `end` would
        // be silently ignored by the helper and could hide a mismatch.
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == static_cast<char>(13)) line.pop_back();
            if (line.empty()) continue;
            error = "unexpected content after the plan end marker";
            return false;
        }
    }
    if (!sawHeader) {
        error = "plan header is missing";
        return false;
    }
    if (!sawEnd) {
        error = "plan end marker is missing";
        return false;
    }
    std::string validateError;
    if (!validatePlan(out, validateError)) {
        error = validateError;
        return false;
    }
    return true;
}

bool readTextFile(const std::string &utf8Path, std::string &out, std::string &error)
{
    out.clear();
    std::ifstream input(nativePath(utf8Path), std::ios::binary);
    if (!input) {
        error = "cannot open file";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        error = "cannot read file";
        return false;
    }
    out = buffer.str();
    return true;
}

bool writeTextFileAtomically(const std::string &utf8Path, const std::string &text, std::string &error)
{
    error.clear();
    const auto target = nativePath(utf8Path);
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "cannot create temporary file";
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = "cannot write temporary file";
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "cannot replace file";
        return false;
    }
#else
    std::error_code code;
    std::filesystem::rename(temporary, target, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "cannot replace file";
        return false;
    }
#endif
    return true;
}

} // namespace vusiif::zto::update::updater
