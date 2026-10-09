#include "Updater.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "Http.h"
#include "version.h"

using json = nlohmann::json;

namespace updater {

const char* const kCurrentVersion = RD_VERSION_STRING;
const char* const kRepositoryUrl = "https://github.com/Jeremiahstauffur/RD-Desktop-Software";
const char* const kManifestUrl =
    "https://raw.githubusercontent.com/Jeremiahstauffur/RD-Desktop-Software/master/dist/update.json";

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::filesystem::path updatesDirectory() {
    std::filesystem::path dir;
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wpath))) {
        dir = std::filesystem::path(wpath) / "RD Desktop Software" / "updates";
        CoTaskMemFree(wpath);
    } else {
        dir = std::filesystem::temp_directory_path() / "RD Desktop Software" / "updates";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Lowercase hex SHA-256 of a file (CNG); empty string on failure.
std::string sha256File(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    struct AlgGuard { BCRYPT_ALG_HANDLE h; ~AlgGuard() { BCryptCloseAlgorithmProvider(h, 0); } } algGuard{alg};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) return "";
    struct HashGuard { BCRYPT_HASH_HANDLE h; ~HashGuard() { BCryptDestroyHash(h); } } hashGuard{hash};

    std::vector<char> buf(256 * 1024);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0 && BCryptHashData(hash, (PUCHAR)buf.data(), (ULONG)n, 0) != 0) return "";
    }
    unsigned char digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) return "";

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

std::string lowercase(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The manifest URL, or the RD_UPDATE_MANIFEST_URL environment variable when set (lets the
// update flow be tested against a local web server without publishing anything).
std::string manifestUrl() {
    char buf[2048];
    DWORD n = GetEnvironmentVariableA("RD_UPDATE_MANIFEST_URL", buf, (DWORD)sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::string(buf, n);
    return kManifestUrl;
}

// Resolves `url` against the manifest location when it is relative ("dist/x.exe" or "x.exe").
std::string resolveUrl(const std::string& url) {
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) return url;
    std::string base = manifestUrl();
    size_t q = base.find('?');
    if (q != std::string::npos) base.erase(q);
    base.erase(base.find_last_of('/') + 1);
    std::string rel = url;
    if (rel.rfind("dist/", 0) == 0) rel.erase(0, 5);
    return base + rel;
}

std::string fileNameFromUrl(const std::string& url) {
    std::string name = url.substr(url.find_last_of('/') + 1);
    size_t q = name.find('?');
    if (q != std::string::npos) name.erase(q);
    for (char& c : name) {
        if (c == '\\' || c == ':' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    return name;
}

} // namespace

int compareVersions(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& v) {
        std::vector<long> parts;
        std::string num;
        for (size_t i = 0; i <= v.size(); ++i) {
            char c = i < v.size() ? v[i] : '.';
            if (std::isdigit((unsigned char)c)) {
                num.push_back(c);
            } else if (c == '.') {
                parts.push_back(num.empty() ? 0 : std::strtol(num.c_str(), nullptr, 10));
                num.clear();
            } else {
                break;   // "1.2.0-beta": ignore the suffix
            }
        }
        if (!num.empty()) parts.push_back(std::strtol(num.c_str(), nullptr, 10));
        return parts;
    };
    std::vector<long> pa = parse(a), pb = parse(b);
    size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        long x = i < pa.size() ? pa[i] : 0;
        long y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

CheckResult check() {
    CheckResult result;
    // Cache-buster: raw.githubusercontent.com caches by URL for a few minutes.
    std::string url = manifestUrl();
    url += (url.find('?') == std::string::npos ? "?t=" : "&t=") + std::to_string((long long)std::time(nullptr));
    http::Response res = http::request("GET", url);
    if (!res.error.empty()) {
        result.error = "Could not reach GitHub: " + res.error;
        return result;
    }
    if (res.status == 404) {
        result.error = "No update information has been published yet (dist/update.json missing).";
        return result;
    }
    if (!res.ok()) {
        result.error = "GitHub answered HTTP " + std::to_string(res.status) + ".";
        return result;
    }
    try {
        json j = json::parse(res.body);
        result.latest.version = j.value("version", "");
        result.latest.installerUrl = resolveUrl(j.value("installer", ""));
        result.latest.sha256 = lowercase(j.value("sha256", ""));
        result.latest.date = j.value("date", "");
        result.latest.notes = j.value("notes", "");
        if (j.contains("size")) {
            if (j["size"].is_number()) result.latest.size = j["size"].get<unsigned long long>();
            else if (j["size"].is_string()) result.latest.size = std::strtoull(j["size"].get<std::string>().c_str(), nullptr, 10);
        }
    } catch (const std::exception& e) {
        result.error = std::string("Update information is not valid JSON: ") + e.what();
        return result;
    }
    if (result.latest.version.empty() || result.latest.installerUrl.empty()) {
        result.error = "Update information is incomplete (version or installer missing).";
        return result;
    }
    result.ok = true;
    result.updateAvailable = compareVersions(result.latest.version, kCurrentVersion) > 0;
    return result;
}

DownloadResult download(const UpdateInfo& info, std::atomic<unsigned long long>* received,
                        const std::atomic<bool>* cancel) {
    DownloadResult result;
    std::filesystem::path dir = updatesDirectory();
    std::string name = fileNameFromUrl(info.installerUrl);
    if (name.empty() || name.size() < 5 || lowercase(name.substr(name.size() - 4)) != ".exe") {
        name = "RD_Desktop_Software-" + info.version + "-setup.exe";
    }
    std::filesystem::path target = dir / widen(name);
    std::error_code ec;
    std::filesystem::remove(target, ec);

    std::string targetUtf8;
    {
        std::wstring w = target.wstring();
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        targetUtf8.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), targetUtf8.data(), len, nullptr, nullptr);
    }

    http::Response res = http::downloadToFile(info.installerUrl, targetUtf8,
                                              [received, cancel](unsigned long long got, unsigned long long) {
                                                  if (received) received->store(got);
                                                  return !(cancel && cancel->load());
                                              });
    if (cancel && cancel->load()) {
        result.error = "Cancelled";
        return result;
    }
    if (!res.error.empty()) {
        result.error = "Download failed: " + res.error;
        return result;
    }
    if (!res.ok()) {
        result.error = "Download failed: GitHub answered HTTP " + std::to_string(res.status) + ".";
        return result;
    }

    unsigned long long actualSize = std::filesystem::file_size(target, ec);
    if (ec) {
        result.error = "Downloaded file could not be read.";
        return result;
    }
    if (info.size > 0 && actualSize != info.size) {
        std::filesystem::remove(target, ec);
        result.error = "Downloaded file is incomplete (" + formatBytes(actualSize) + " of " + formatBytes(info.size) +
                       "). GitHub may still be updating its cache - try again in a few minutes.";
        return result;
    }
    if (!info.sha256.empty()) {
        std::string actual = sha256File(target);
        if (actual != info.sha256) {
            std::filesystem::remove(target, ec);
            result.error = "Downloaded file failed the SHA-256 check. GitHub may still be updating its cache - "
                           "try again in a few minutes.";
            return result;
        }
    }
    result.ok = true;
    result.path = targetUtf8;
    return result;
}

HANDLE launchInstaller(const std::string& path, std::string& error) {
    std::wstring wpath = widen(path);
    std::wstring logPath = (updatesDirectory() / L"setup.log").wstring();
    // /SILENT: progress window only (the user already confirmed); the previous install
    // folder, mode and shortcut choices are reused by the installer. /FROMAPP=1 makes the
    // installer start the app again when it is done.
    std::wstring params = L"/SILENT /NORESTART /FROMAPP=1 /LOG=\"" + logPath + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = wpath.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD code = GetLastError();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Windows error %lu", (unsigned long)code);
        error = code == ERROR_CANCELLED ? "the request was cancelled" : buf;
        return nullptr;
    }
    if (!sei.hProcess) {
        error = "no process handle was returned";
        return nullptr;
    }
    return sei.hProcess;
}

void cleanupDownloads() {
    std::error_code ec;
    std::filesystem::path dir = updatesDirectory();
    // setup.log is kept: it is the only trace of what the last update did.
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (lowercase(entry.path().extension().string()) == ".exe") std::filesystem::remove(entry.path(), ec);
    }
}

std::string formatBytes(unsigned long long bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024ull) std::snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ull) std::snprintf(buf, sizeof(buf), "%.0f KB", (double)bytes / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%llu bytes", bytes);
    return buf;
}

} // namespace updater
