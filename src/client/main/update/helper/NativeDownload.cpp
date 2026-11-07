#include "NativeDownload.h"
#include "../UpdaterPlan.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cwctype>
#include <filesystem>

namespace {
std::wstring wide(const std::string &text)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length);
    return result;
}
void report(DWORD channel, const std::string &text)
{
    const auto line = text + "\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(channel), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}
struct Internet {
    HINTERNET value = nullptr;
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
struct Output {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring temporary;
    bool owned = false;
    ~Output() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (owned) DeleteFileW(temporary.c_str());
    }
};
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE value = nullptr;
    std::vector<UCHAR> storage;
    ~Hash() {
        if (value) BCryptDestroyHash(value);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    bool start() {
        DWORD length = 0, result = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
            || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length),
                                 sizeof(length), &result, 0) < 0) return false;
        storage.resize(length);
        return BCryptCreateHash(algorithm, &value, storage.data(), length, nullptr, 0, 0) >= 0;
    }
    std::string finish() {
        UCHAR digest[32];
        if (BCryptFinishHash(value, digest, sizeof(digest), 0) < 0) return {};
        static const char hex[] = "0123456789abcdef";
        std::string result;
        for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        return result;
    }
};
bool safeParent(const std::wstring &path)
{
    auto parent = std::filesystem::path(path).parent_path();
    while (!parent.empty()) {
        const auto attributes = GetFileAttributesW(parent.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    return true;
}
int download(const std::vector<std::string> &args, std::string &error)
{
    auto fail = [&](const std::string &message) { error = message; return 7; };
    if (args.size() != 8 || args[0] != "--download" || args[2] != "--output"
        || args[4] != "--size" || args[6] != "--sha256") return fail("invalid download arguments");
    std::uint64_t size = 0;
    const auto number = std::from_chars(args[5].data(), args[5].data() + args[5].size(), size);
    if (number.ec != std::errc() || number.ptr != args[5].data() + args[5].size()
        || size == 0 || size > 512ULL * 1024 * 1024) return fail("invalid package size");
    const auto &expectedHash = args[7];
    if (expectedHash.size() != 64 || !std::all_of(expectedHash.begin(), expectedHash.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return fail("invalid SHA-256");
    std::string pathError;
    if (!vusiif::zto::update::updater::isValidAbsolutePath(args[3], pathError)) return fail(pathError);
    const auto target = wide(args[3]);
    if (target.empty() || !safeParent(target)) return fail("unsafe or missing download directory");
    const auto url = wide(args[1]);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)
        || (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
        || parts.dwHostNameLength == 0 || parts.dwUserNameLength || parts.dwPasswordLength
        || url.find(L'#') != std::wstring::npos
        || std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/file")
        return fail("invalid /file download URL");
    Internet session{WinHttpOpen(L"Z2-Updater/2", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) return fail("cannot initialize HTTP");
    if (!WinHttpSetTimeouts(session.value, 10000, 10000, 30000, 30000))
        return fail("cannot configure HTTP timeouts");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    Internet connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value) return fail("cannot connect to update server");
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet request{WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.value) return fail("cannot open download request");
    DWORD redirects = 5, policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects))
        || !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
        return fail("cannot configure HTTP redirects");
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.value, nullptr))
        return fail("HTTP download failed (Windows error " + std::to_string(GetLastError()) + ")");
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)
        || status < 200 || status >= 300) return fail("HTTP " + std::to_string(status));
    wchar_t type[256]{};
    length = sizeof(type);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            type, &length, WINHTTP_NO_HEADER_INDEX)) {
        std::wstring mime(type);
        mime = mime.substr(0, mime.find(L';'));
        std::transform(mime.begin(), mime.end(), mime.begin(), [](wchar_t c) { return std::towlower(c); });
        if (mime != L"application/zip" && mime != L"application/x-zip-compressed"
            && mime != L"application/octet-stream" && mime != L"binary/octet-stream")
            return fail("unsupported package content type");
    } else if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) return fail("invalid content type header");
    Output output;
    output.temporary = target + L".part-" + std::to_wstring(GetCurrentProcessId());
    output.file = CreateFileW(output.temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output.file == INVALID_HANDLE_VALUE) return fail("cannot create download file");
    output.owned = true;
    Hash hash;
    if (!hash.start()) return fail("cannot initialize SHA-256");
    std::uint64_t received = 0;
    int previousProgress = -1;
    UCHAR buffer[64 * 1024];
    while (true) {
        DWORD bytes = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &bytes)) return fail("download interrupted");
        if (bytes == 0) break;
        received += bytes;
        if (received > size) return fail("download exceeds declared size");
        DWORD written = 0;
        if (!WriteFile(output.file, buffer, bytes, &written, nullptr) || written != bytes
            || BCryptHashData(hash.value, buffer, bytes, 0) < 0) return fail("cannot save or hash download");
        const int progress = static_cast<int>(received * 100 / size);
        if (progress != previousProgress) {
            report(STD_OUTPUT_HANDLE, "progress=" + std::to_string(progress));
            previousProgress = progress;
        }
    }
    if (received != size) return fail("download size mismatch");
    if (hash.finish() != expectedHash) return fail("download SHA-256 mismatch");
    if (!FlushFileBuffers(output.file)) return fail("cannot flush download file");
    CloseHandle(output.file);
    output.file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(output.temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail("cannot commit download file");
    output.owned = false;
    return 0;
}
}

int runNativeDownload(const std::vector<std::string> &arguments)
{
    std::string error;
    try {
        const int result = download(arguments, error);
        if (result != 0) report(STD_ERROR_HANDLE, error);
        return result;
    } catch (const std::exception &exception) {
        report(STD_ERROR_HANDLE, exception.what());
        return 7;
    }
}
#else
int runNativeDownload(const std::vector<std::string> &) { return 7; }
#endif
