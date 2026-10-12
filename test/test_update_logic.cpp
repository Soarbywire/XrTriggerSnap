// Unit tests for tray/update_logic.h (portable part of the updater).
#include <cstdio>
#include <algorithm>
#include "../tray/update_logic.h"

using namespace update_logic;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main() {
    // A trimmed but realistic GitHub "latest release" response, including a nested uploader
    // object with its own fields, escapes and unicode.
    const char* json = R"({
      "url": "https://api.github.com/repos/Soarbywire/XrTriggerSnap/releases/1",
      "html_url": "https://github.com/Soarbywire/XrTriggerSnap/releases/tag/v1.0.1",
      "tag_name": "v1.0.1",
      "name": "XrTriggerSnap 1.0.1",
      "draft": false, "prerelease": false, "id": 12345, "ratio": -1.5e2,
      "author": {"login": "Soarbywire", "id": 1, "site_admin": false},
      "body": "Fixes:\n- \"quoted\" thing é 😀",
      "assets": [
        {"name": "XrTriggerSnap.exe", "size": 100, "uploader": {"login": "x", "name": null},
         "browser_download_url": "https://github.com/Soarbywire/XrTriggerSnap/releases/download/v1.0.1/XrTriggerSnap.exe"},
        {"name": "SHA256SUMS.txt",
         "browser_download_url": "https://github.com/Soarbywire/XrTriggerSnap/releases/download/v1.0.1/SHA256SUMS.txt"},
        {"name": "XR_APILAYER_trigger_snap.dll",
         "browser_download_url": "https://example/dll"}
      ]
    })";
    Release r;
    std::string err;
    CHECK(parseLatestRelease(json, r, err), "parse release: %s", err.c_str());
    CHECK(r.tag == "v1.0.1" && r.version == "1.0.1", "tag/version");
    CHECK(r.pageUrl == "https://github.com/Soarbywire/XrTriggerSnap/releases/tag/v1.0.1", "html_url");
    CHECK(r.assets.size() == 3, "3 assets, got %zu", r.assets.size());
    CHECK(r.assets["XR_APILAYER_trigger_snap.dll"] == "https://example/dll", "asset url");
    CHECK(r.notes.find("\"quoted\"") != std::string::npos, "escaped quotes in body");
    CHECK(r.notes.find("\xC3\xA9") != std::string::npos, "\\u00e9 decoded to UTF-8");
    CHECK(r.notes.find("\xF0\x9F\x98\x80") != std::string::npos, "surrogate pair decoded");

    Release r2;
    CHECK(!parseLatestRelease(R"({"message":"Not Found","documentation_url":"x"})", r2, err) && err == "Not Found",
          "no-release message surfaced, got '%s'", err.c_str());
    CHECK(!parseLatestRelease("<html>rate limited</html>", r2, err), "non-JSON rejected");
    CHECK(!parseLatestRelease(R"({"tag_name": "v1", )", r2, err), "truncated JSON rejected");

    CHECK(compareVersions("1.0.1", "1.0.0") > 0, "1.0.1 > 1.0.0");
    CHECK(compareVersions("v1.0.10", "1.0.9") > 0, "1.0.10 > 1.0.9 (numeric)");
    CHECK(compareVersions("1.0", "1.0.0") == 0, "1.0 == 1.0.0");
    CHECK(compareVersions("1.0.0", "v1.0.0") == 0, "v prefix ignored");
    CHECK(compareVersions("0.9.9", "1.0.0") < 0, "0.9.9 < 1.0.0");
    CHECK(compareVersions("1.1.0-beta", "1.1.0") == 0, "suffix ignored");
    CHECK(compareVersions("2", "1.9.9") > 0, "2 > 1.9.9");

    std::string sums =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  XrTriggerSnap.exe\r\n"
        "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855 *XR_APILAYER_trigger_snap.dll\n"
        "not a hash line\n"
        "zzz0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  bad.txt\n"
        "a3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  XrTriggerSnap-1.0.1.zip";
    auto m = parseSha256Sums(sums);
    CHECK(m.size() == 3, "3 valid sum lines, got %zu", m.size());
    CHECK(m["XR_APILAYER_trigger_snap.dll"] == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "binary-mode '*' and uppercase handled");
    CHECK(m.count("XrTriggerSnap.exe") && m.count("XrTriggerSnap-1.0.1.zip"), "names parsed (CRLF and no final newline)");

    CHECK(updateFiles().back() == "XrTriggerSnap.exe", "exe swapped last");
    CHECK(std::find(updateFiles().begin(), updateFiles().end(), "trigger_snap.ini") == updateFiles().end(),
          "settings file never replaced");

    printf(failures ? "%d FAILURE(S)\n" : "ALL UPDATE LOGIC TESTS PASSED\n", failures);
    return failures != 0;
}
