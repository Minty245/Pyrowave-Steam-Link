// Steam Link Pyrowave - minimal JSON reader (objects, numbers, booleans; strings/arrays skipped), flattening
// {"a": {"b": 1}} to {"a.b": 1}. Shared by the backend and the settings app.
// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace slpjson {
struct Reader
{
    const char *p, *e;
    static bool keep(const std::string &path) { return !path.empty() && path[0] != '\x01'; }
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool str(std::string *out)
    {
        ws();
        if (p >= e || *p != '"') return false;
        ++p;
        std::string s;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) { ++p; s += *p == 'n' ? '\n' : *p; ++p; continue; }
            s += *p++;
        }
        if (p >= e) return false;
        ++p;
        if (out) *out = s;
        return true;
    }
    bool value(const std::string &path, std::map<std::string, double> &m, int depth)
    {
        ws();
        if (p >= e || depth > 8) return false;
        if (*p == '{') {
            ++p;
            ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                std::string k;
                if (!str(&k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                ++p;
                if (!value(path.empty() ? k : path + "." + k, m, depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (*p == '[') {
            ++p;
            ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                if (!value(std::string(1, '\x01'), m, depth + 1)) return false;   // array contents are not used
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (*p == '"') return str(nullptr);
        if (e - p >= 4 && !strncmp(p, "true", 4)) { p += 4; if (keep(path)) m[path] = 1; return true; }
        if (e - p >= 5 && !strncmp(p, "false", 5)) { p += 5; if (keep(path)) m[path] = 0; return true; }
        if (e - p >= 4 && !strncmp(p, "null", 4)) { p += 4; return true; }
        char *end = nullptr;
        double v = strtod(p, &end);
        if (end == p) return false;
        p = end;
        if (keep(path) && std::isfinite(v)) m[path] = v;
        return true;
    }
};


inline bool parse(const std::string &txt, std::map<std::string, double> &out)
{
    Reader r{txt.data(), txt.data() + txt.size()};
    return r.value(std::string(), out, 0);
}
} // namespace slpjson
