// z2_updater.exe: standalone Win32 transaction helper for the Z2 automatic
// update. It has no Qt dependency on purpose: it must be able to run while the
// application directories are being replaced.
//
// Usage:
//   z2_updater.exe --plan <plan-file> [--no-restart] [--relaunched]
//                  [--fail-after <n>] [--wait-timeout <seconds>]
//   z2_updater.exe --download <URL/file?version=...> --output <absolute path>
//                  --size <bytes> --sha256 <hex>
//
// --no-restart and --fail-after are also used by the isolated transaction tests.
// The helper never terminates the Z2 main process; it waits for it with a finite
// timeout and refuses to touch the installation when the wait expires.

#include "../UpdaterEngine.h"
#include "../UpdaterPlan.h"
#include "NativeDownload.h"

#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

namespace {

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

void appendFallbackLog(const std::string &planPath, const std::string &text)
{
    const auto path = toWide(planPath + ".error.log");
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

std::string ownExecutablePath()
{
    wchar_t path[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (length == 0 || length >= 32768) return {};
    return toUtf8(std::wstring(path, length));
}

std::string quoteArgument(const std::string &argument)
{
    // Plan paths are controlled by the host. Quote defensively and duplicate
    // embedded quotes with a backslash sequence accepted by CommandLineToArgvW.
    std::string result = "\"";
    std::size_t backslashes = 0;
    for (char ch : argument) {
        if (ch == '\\') {
            ++backslashes;
            result.push_back(ch);
            continue;
        }
        if (ch == '"') {
            result.append(backslashes + 1, '\\');
            result.push_back('"');
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes, '\\');
    result.push_back('"');
    return result;
}

struct Arguments {
    std::string planPath;
    bool noRestart = false;
    bool relaunched = false;
    int failAfter = -1;
    unsigned waitTimeoutMs = 180000;
};

bool parseArguments(const std::vector<std::string> &raw, Arguments &out, std::string &error)
{
    for (std::size_t index = 0; index < raw.size(); ++index) {
        const auto &argument = raw[index];
        if (argument == "--plan") {
            if (index + 1 >= raw.size()) {
                error = "--plan requires a path";
                return false;
            }
            out.planPath = raw[++index];
        } else if (argument == "--no-restart") {
            out.noRestart = true;
        } else if (argument == "--relaunched") {
            out.relaunched = true;
        } else if (argument == "--fail-after") {
            if (index + 1 >= raw.size()) {
                error = "--fail-after requires a count";
                return false;
            }
            out.failAfter = std::atoi(raw[++index].c_str());
        } else if (argument == "--wait-timeout") {
            if (index + 1 >= raw.size()) {
                error = "--wait-timeout requires seconds";
                return false;
            }
            const long seconds = std::atol(raw[++index].c_str());
            if (seconds <= 0 || seconds > 3600) {
                error = "--wait-timeout is out of range";
                return false;
            }
            out.waitTimeoutMs = static_cast<unsigned>(seconds) * 1000U;
        } else {
            error = "unknown argument: " + argument;
            return false;
        }
    }
    if (out.planPath.empty()) {
        error = "--plan is required";
        return false;
    }
    return true;
}

std::string rebuildRelaunchArguments(const std::vector<std::string> &raw)
{
    std::string result;
    for (const auto &argument : raw) {
        if (argument == "--relaunched") continue;
        if (!result.empty()) result.push_back(' ');
        result += quoteArgument(argument);
    }
    if (!result.empty()) result.push_back(' ');
    result += "--relaunched";
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    using namespace vusiif::zto::update::updater;
    int argumentCount = 0;
    LPWSTR *rawArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::vector<std::string> arguments;
    for (int index = 1; index < argumentCount; ++index) arguments.push_back(toUtf8(rawArguments[index]));
    if (rawArguments) LocalFree(rawArguments);
    if (!arguments.empty() && arguments.front() == "--download") return runNativeDownload(arguments);

    Arguments parsed;
    std::string argumentError;
    if (!parseArguments(arguments, parsed, argumentError)) {
        // The plan path is unknown; the fallback log is the only durable channel.
        OutputDebugStringA(("z2_updater: " + argumentError + "\n").c_str());
        return 2;
    }

    std::string planText;
    std::string readError;
    if (!readTextFile(parsed.planPath, planText, readError)) {
        appendFallbackLog(parsed.planPath, "cannot read plan: " + readError + "\n");
        return 2;
    }
    Plan plan;
    std::string planError;
    if (!parsePlan(planText, plan, planError)) {
        appendFallbackLog(parsed.planPath, "invalid plan: " + planError + "\n");
        return 2;
    }

    const auto ownPath = ownExecutablePath();
    const bool replacesSelf = !ownPath.empty() && planReplacesPath(plan, ownPath);
    if (replacesSelf && !parsed.relaunched) {
        // Windows locks a running image. Continue from the runtime directory so
        // the installed helper (and every other package file) can be replaced.
        const auto relaunchArguments = rebuildRelaunchArguments(arguments);
        std::string relaunchError;
        if (!relaunchFromRuntimeDirectory(plan, relaunchArguments, relaunchError)) {
            EngineResult result;
            result.step = "relaunch";
            result.message = relaunchError;
            result.exitCode = 6;
            std::string writeError;
            writeResultFile(plan, result, writeError);
            return 6;
        }
        return 0;
    }

    std::string waitError;
    if (!waitForProcessExit(plan.parentPid, parsed.waitTimeoutMs, waitError)) {
        EngineResult result;
        result.step = "wait";
        result.message = waitError;
        result.exitCode = 4;
        std::string writeError;
        writeResultFile(plan, result, writeError);
        return 4;
    }

    EngineOptions options;
    options.noRestart = parsed.noRestart;
    options.relaunched = parsed.relaunched;
    options.failAfterReplacements = parsed.failAfter;
    options.waitTimeoutMs = parsed.waitTimeoutMs;
    const auto result = runTransaction(plan, options);
    return result.exitCode;
}

#else

int main(int, char **)
{
    return 2; // Automatic updates are implemented for Windows only.
}

#endif
