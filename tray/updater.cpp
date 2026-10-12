#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "updater.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdio>
#include <vector>

#include "update_logic.h"
#include "../version.h"

namespace updater {
namespace {

using update_logic::Release;

std::atomic<bool> g_busy{false};

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}


std::wstring stagingDir(const std::wstring& appDir) { return appDir + L"\\update"; }
std::wstring markerPath(const std::wstring& appDir) { return stagingDir(appDir) + L"\\VERIFIED"; }

// Where to ask for the latest release. XRTS_UPDATE_API overrides it (used for testing).
std::wstring apiUrl() {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"XRTS_UPDATE_API", buf, 1024);
    if (n > 0 && n < 1024) return buf;
    return L"https://api.github.com/repos/" + widen(XRTS_GITHUB_REPO) + L"/releases/latest";
}

bool allowedUrl(const std::wstring& url) {
    // Real updates only ever come over HTTPS. Plain HTTP is accepted only when the API URL
    // itself was overridden for testing.
    if (url.rfind(L"https://", 0) == 0) return true;
    wchar_t buf[8];
    return url.rfind(L"http://", 0) == 0 && GetEnvironmentVariableW(L"XRTS_UPDATE_API", buf, 8) > 0;
}

// GET a URL into memory (follows redirects; GitHub asset downloads redirect to its CDN).
bool httpGet(const std::wstring& url, std::string& body, size_t maxBytes, std::wstring& err) {
    body.clear();
    if (!allowedUrl(url)) {
        err = L"refusing non-HTTPS URL";
        return false;
    }
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[1024] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        err = L"bad URL";
        return false;
    }
    std::wstring ua = std::wstring(L"XrTriggerSnap/") + widen(XRTS_VERSION);
    HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        err = L"network unavailable";
        return false;
    }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = nullptr;
    if (c) {
        std::wstring obj = std::wstring(path) + extra;
        r = WinHttpOpenRequest(c, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    if (r) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        const wchar_t* hdrs = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
        if (WinHttpSendRequest(r, hdrs, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &sz, WINHTTP_NO_HEADER_INDEX);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                if (body.size() + avail > maxBytes) {
                    err = L"download too large";
                    body.clear();
                    status = 0;
                    break;
                }
                size_t old = body.size();
                body.resize(old + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, &body[old], avail, &got)) {
                    body.clear();
                    status = 0;
                    break;
                }
                body.resize(old + got);
            }
            if (status == 200) ok = true;
            else if (status == 404) err = L"not found (has a release been published?)";
            else if (status == 403) err = L"GitHub rate limit reached, will try again later";
            else if (status && err.empty()) err = L"server returned HTTP " + std::to_wstring(status);
            else if (err.empty()) err = L"download interrupted";
            // The API answers 404 with a JSON message; keep the body so the caller can show it.
            if (!ok && status != 404) body.clear();
        } else {
            err = L"couldn't reach GitHub (offline?)";
        }
    } else if (err.empty()) {
        err = L"couldn't reach GitHub (offline?)";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

std::string sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
        BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
        static const char* d = "0123456789abcdef";
        for (unsigned char b : digest) {
            hex += d[b >> 4];
            hex += d[b & 15];
        }
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool writeFile(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    return ok;
}

bool readFile(const std::wstring& path, std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    data.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = data.empty() || (ReadFile(f, &data[0], (DWORD)data.size(), &got, nullptr) && got == data.size());
    CloseHandle(f);
    return ok;
}

void removeDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeDir(dir + L"\\" + n);
            else DeleteFileW((dir + L"\\" + n).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Downloads every update file plus SHA256SUMS.txt, verifies each hash, and writes the
// VERIFIED marker last so a half-finished download is never applied.
bool downloadAndStage(const std::wstring& appDir, const Release& rel, std::wstring& err) {
    auto sumsIt = rel.assets.find("SHA256SUMS.txt");
    if (sumsIt == rel.assets.end()) {
        err = L"the release has no SHA256SUMS.txt";
        return false;
    }
    std::string sumsText;
    if (!httpGet(widen(sumsIt->second), sumsText, 64 * 1024, err)) return false;
    auto sums = update_logic::parseSha256Sums(sumsText);

    std::wstring stage = stagingDir(appDir);
    removeDir(stage);
    if (!CreateDirectoryW(stage.c_str(), nullptr)) {
        err = L"can't write to the program folder (is it read-only?)";
        return false;
    }
    for (const std::string& name : update_logic::updateFiles()) {
        auto a = rel.assets.find(name);
        if (a == rel.assets.end()) {
            if (update_logic::isRequired(name)) {
                err = L"the release is missing " + widen(name);
                removeDir(stage);
                return false;
            }
            continue;
        }
        auto expected = sums.find(name);
        if (expected == sums.end()) {
            err = L"no checksum listed for " + widen(name);
            removeDir(stage);
            return false;
        }
        std::string data;
        if (!httpGet(widen(a->second), data, 64 * 1024 * 1024, err)) {
            err = widen(name) + L": " + err;
            removeDir(stage);
            return false;
        }
        if (sha256Hex(data) != expected->second) {
            err = L"checksum mismatch for " + widen(name) + L" (download corrupted?)";
            removeDir(stage);
            return false;
        }
        if (!writeFile(stage + L"\\" + widen(name), data)) {
            err = L"couldn't save " + widen(name);
            removeDir(stage);
            return false;
        }
    }
    if (!writeFile(markerPath(appDir), rel.version)) {
        err = L"couldn't finish staging the update";
        removeDir(stage);
        return false;
    }
    return true;
}

struct Job {
    HWND notify;
    UINT msg;
    std::wstring appDir;
    bool download, manual;
};

DWORD WINAPI worker(LPVOID p) {
    Job* job = static_cast<Job*>(p);
    auto* res = new Result;
    res->manual = job->manual;

    std::string body;
    std::wstring err;
    bool got = httpGet(apiUrl(), body, 2 * 1024 * 1024, err);
    Release rel;
    std::string perr;
    if (!body.empty() && update_logic::parseLatestRelease(body, rel, perr)) {
        res->version = widen(rel.version);
        res->pageUrl = widen(rel.pageUrl);
        if (update_logic::compareVersions(rel.version, XRTS_VERSION) <= 0) {
            res->kind = Result::UpToDate;
        } else if (!job->download) {
            res->kind = Result::Available;
            res->message = widen(rel.notes);
        } else if (stagedVersion(job->appDir) == res->version) {
            res->kind = Result::Staged;  // already downloaded earlier
        } else if (downloadAndStage(job->appDir, rel, err)) {
            res->kind = Result::Staged;
            res->message = widen(rel.notes);
        } else {
            res->kind = Result::Error;
            res->message = err;
        }
    } else {
        res->kind = Result::Error;
        res->message = !got && !err.empty() ? err : widen(perr.empty() ? "unexpected reply from GitHub" : perr);
    }

    if (!PostMessageW(job->notify, job->msg, 0, (LPARAM)res)) delete res;
    delete job;
    g_busy = false;
    return 0;
}

}  // namespace

bool busy() { return g_busy; }

bool startCheck(HWND notify, UINT msg, const std::wstring& appDir, bool download, bool manual) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    auto* job = new Job{notify, msg, appDir, download, manual};
    HANDLE t = CreateThread(nullptr, 0, worker, job, 0, nullptr);
    if (!t) {
        delete job;
        g_busy = false;
        return false;
    }
    CloseHandle(t);
    return true;
}

std::wstring stagedVersion(const std::wstring& appDir) {
    std::string v;
    if (!readFile(markerPath(appDir), v)) return L"";
    // Only trust a staged update that is newer than what's running.
    if (update_logic::compareVersions(v, XRTS_VERSION) <= 0) return L"";
    return widen(v);
}

bool applyStaged(const std::wstring& appDir, std::wstring& err) {
    std::wstring stage = stagingDir(appDir);
    if (stagedVersion(appDir).empty()) {
        err = L"no verified update is waiting";
        return false;
    }
    struct Done {
        std::wstring target, old;
        bool hadOld;
    };
    std::vector<Done> done;
    auto rollback = [&]() {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->target.c_str());
            if (it->hadOld) MoveFileExW(it->old.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };
    for (const std::string& name : update_logic::updateFiles()) {
        std::wstring src = stage + L"\\" + widen(name);
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;  // optional file not in release
        std::wstring target = appDir + L"\\" + widen(name), old = target + L".old";
        bool hadOld = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        // Renaming works even for the running exe; a file in use can't be overwritten but can be moved aside.
        if (hadOld && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            err = L"couldn't replace " + widen(name) + L" (in use?)";
            rollback();
            return false;
        }
        if (!MoveFileExW(src.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            if (hadOld) MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            err = L"couldn't install " + widen(name);
            rollback();
            return false;
        }
        done.push_back({target, old, hadOld});
    }
    removeDir(stage);
    return true;
}

void cleanupOld(const std::wstring& appDir) {
    for (const std::string& name : update_logic::updateFiles())
        DeleteFileW((appDir + L"\\" + widen(name) + L".old").c_str());
    // A staging folder for a version we already have (or a broken one) is useless.
    if (stagedVersion(appDir).empty()) removeDir(stagingDir(appDir));
}

}  // namespace updater
