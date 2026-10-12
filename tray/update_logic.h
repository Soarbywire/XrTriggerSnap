// Portable updater logic (no Windows APIs), unit-tested on Linux in test/test_update_logic.cpp.
//   - a minimal JSON parser for the GitHub "latest release" response
//   - version comparison
//   - SHA256SUMS.txt parsing
#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace update_logic {

// ---------------------------------------------------------------- JSON

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const std::string& key) const {
        if (type != Object) return nullptr;
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& key) const {
        const Json* j = get(key);
        return (j && j->type == String) ? j->s : std::string();
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : t_(t) {}

    bool parse(Json& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return p_ == t_.size();
    }

private:
    const std::string& t_;
    size_t p_ = 0;

    void ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
    }
    bool lit(const char* w) {
        size_t n = strlen_(w);
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    static size_t strlen_(const char* w) {
        size_t n = 0;
        while (w[n]) ++n;
        return n;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (p_ + 4 > t_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        if (p_ >= t_.size() || t_[p_] != '"') return false;
        ++p_;
        while (p_ < t_.size()) {
            char c = t_[p_++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p_ >= t_.size()) return false;
            char e = t_[p_++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 6 <= t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                        p_ += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64 || p_ >= t_.size()) return false;
        char c = t_[p_];
        if (c == '{') {
            v.type = Json::Object;
            ++p_;
            ws();
            if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p_ >= t_.size() || t_[p_] != ':') return false;
                ++p_;
                ws();
                Json child;
                if (!value(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
                if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            ++p_;
            ws();
            if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
            for (;;) {
                ws();
                Json child;
                if (!value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
                if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Json::String; return string(v.s); }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Json::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = p_;
            ++p_;
            while (p_ < t_.size() && (isdigit((unsigned char)t_[p_]) || t_[p_] == '.' || t_[p_] == 'e' ||
                                      t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-'))
                ++p_;
            v.type = Json::Number;
            v.n = strtod(t_.substr(start, p_ - start).c_str(), nullptr);
            return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------- release info

struct Release {
    std::string tag;      // e.g. "v1.0.1"
    std::string version;  // e.g. "1.0.1"
    std::string pageUrl;  // html_url of the release
    std::string notes;    // body (markdown)
    std::map<std::string, std::string> assets;  // file name -> download URL
};

inline std::string stripV(const std::string& t) {
    return (!t.empty() && (t[0] == 'v' || t[0] == 'V')) ? t.substr(1) : t;
}

inline bool parseLatestRelease(const std::string& body, Release& r, std::string& err) {
    Json j;
    if (!JsonParser(body).parse(j) || j.type != Json::Object) {
        err = "GitHub's reply wasn't valid JSON";
        return false;
    }
    r.tag = j.str("tag_name");
    if (r.tag.empty()) {
        std::string m = j.str("message");
        err = m.empty() ? "no release found" : m;  // e.g. "Not Found" when the repo has no releases
        return false;
    }
    r.version = stripV(r.tag);
    r.pageUrl = j.str("html_url");
    r.notes = j.str("body");
    if (const Json* a = j.get("assets")) {
        for (auto& asset : a->arr) {
            std::string name = asset.str("name"), url = asset.str("browser_download_url");
            if (!name.empty() && !url.empty()) r.assets[name] = url;
        }
    }
    return true;
}

// ---------------------------------------------------------------- versions

// Compares dotted numeric versions ("1.0.10" > "1.0.9"); a leading "v" is ignored and anything
// after a '-' or '+' (pre-release / build tags) is ignored. Returns <0, 0, >0.
inline int compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](std::string s) {
        s = stripV(s);
        size_t cut = s.find_first_of("-+ ");
        if (cut != std::string::npos) s = s.substr(0, cut);
        std::vector<long> out;
        size_t i = 0;
        while (i <= s.size()) {
            size_t dot = s.find('.', i);
            std::string piece = s.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
            out.push_back(strtol(piece.c_str(), nullptr, 10));
            if (dot == std::string::npos) break;
            i = dot + 1;
        }
        while (out.size() > 1 && out.back() == 0) out.pop_back();
        return out;
    };
    std::vector<long> x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        long xi = i < x.size() ? x[i] : 0, yi = i < y.size() ? y[i] : 0;
        if (xi != yi) return xi < yi ? -1 : 1;
    }
    return 0;
}

// ---------------------------------------------------------------- SHA256SUMS.txt

// Lines of "<64 hex chars>  <file name>" (sha256sum format; a '*' before the name is allowed).
inline std::map<std::string, std::string> parseSha256Sums(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = nl == std::string::npos ? text.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 66) continue;
        std::string hash = line.substr(0, 64);
        bool ok = true;
        for (auto& c : hash) {
            c = (char)tolower((unsigned char)c);
            if (!isxdigit((unsigned char)c)) ok = false;
        }
        if (!ok) continue;
        size_t n = 64;
        while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
        if (n < line.size() && line[n] == '*') ++n;
        std::string name = line.substr(n);
        if (!name.empty()) out[name] = hash;
    }
    return out;
}

// Files an update replaces, in the order they're swapped. The ini (user settings) is never touched.
inline const std::vector<std::string>& updateFiles() {
    static const std::vector<std::string> f = {"XR_APILAYER_trigger_snap.dll", "XR_APILAYER_trigger_snap.json",
                                               "README.md", "XrTriggerSnap.exe"};
    return f;
}
inline bool isRequired(const std::string& name) { return name != "README.md"; }

}  // namespace update_logic
