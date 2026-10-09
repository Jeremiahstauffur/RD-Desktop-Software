#pragma once

#include <atomic>
#include <string>

#include <windows.h>

// Update check against the GitHub repository. package.bat publishes dist/update.json
// next to the installer; it names the newest version, the installer URL, its size and
// SHA-256. The functions here are blocking and meant to run through Async::run.
namespace updater {

extern const char* const kCurrentVersion;   // RD_VERSION_STRING from version.txt
extern const char* const kRepositoryUrl;    // GitHub project page
extern const char* const kManifestUrl;      // raw URL of dist/update.json

struct UpdateInfo {
    std::string version;
    std::string installerUrl;
    std::string sha256;             // lowercase hex; empty = not verified
    unsigned long long size = 0;    // bytes; 0 = unknown
    std::string date;               // optional "YYYY-MM-DD"
    std::string notes;              // optional release notes
};

struct CheckResult {
    bool ok = false;
    std::string error;
    UpdateInfo latest;
    bool updateAvailable = false;   // latest.version > kCurrentVersion
};

struct DownloadResult {
    bool ok = false;
    std::string error;
    std::string path;               // downloaded, verified installer
};

// Compares dotted version numbers ("1.2.0" vs "1.10"); missing parts count as 0.
// Returns <0 when a is older than b, 0 when equal, >0 when newer.
int compareVersions(const std::string& a, const std::string& b);

// Fetches and parses the manifest.
CheckResult check();

// Downloads the installer into the updates folder (%LOCALAPPDATA%\RD Desktop Software\updates),
// publishing the bytes received so far in `received`, stopping once `cancel` is set, then
// verifies size and SHA-256. The file is removed on any failure.
DownloadResult download(const UpdateInfo& info, std::atomic<unsigned long long>* received,
                        const std::atomic<bool>* cancel);

// Starts the downloaded installer as a silent in-place update that relaunches the app
// afterwards (Setup closes the running app itself). Returns the process handle (caller closes
// it) or nullptr with `error` set.
HANDLE launchInstaller(const std::string& path, std::string& error);

// Deletes installers left behind by earlier updates. Best effort, never throws.
void cleanupDownloads();

std::string formatBytes(unsigned long long bytes);   // "3.4 MB"

} // namespace updater
