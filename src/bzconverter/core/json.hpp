#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace bz {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Value val = nullptr;

    Json() = default;
    Json(std::nullptr_t) : val(nullptr) {}
    Json(bool b) : val(b) {}
    Json(int v) : val(static_cast<double>(v)) {}
    Json(uint32_t v) : val(static_cast<double>(v)) {}
    Json(size_t v) : val(static_cast<double>(v)) {}
    Json(float v) : val(static_cast<double>(v)) {}
    Json(double v) : val(v) {}
    Json(const char* s) : val(std::string(s ? s : "")) {}
    Json(std::string s) : val(std::move(s)) {}
    Json(Array a) : val(std::move(a)) {}
    Json(Object o) : val(std::move(o)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(val); }
    bool is_bool() const { return std::holds_alternative<bool>(val); }
    bool is_num() const { return std::holds_alternative<double>(val); }
    bool is_str() const { return std::holds_alternative<std::string>(val); }
    bool is_arr() const { return std::holds_alternative<Array>(val); }
    bool is_obj() const { return std::holds_alternative<Object>(val); }

    bool as_bool(bool def = false) const { return is_bool() ? std::get<bool>(val) : def; }
    double as_num(double def = 0.0) const { return is_num() ? std::get<double>(val) : def; }
    int as_int(int def = 0) const { return is_num() ? static_cast<int>(std::get<double>(val)) : def; }
    const std::string& as_str() const { static const std::string empty; return is_str() ? std::get<std::string>(val) : empty; }
    const Array& as_arr() const { static const Array empty; return is_arr() ? std::get<Array>(val) : empty; }
    const Object& as_obj() const { static const Object empty; return is_obj() ? std::get<Object>(val) : empty; }

    void push_back(Json item) {
        if (!is_arr()) val = Array{};
        std::get<Array>(val).push_back(std::move(item));
    }

    Json& operator[](const std::string& key) {
        if (!is_obj()) val = Object{};
        return std::get<Object>(val)[key];
    }

    const Json* get(const std::string& key) const {
        if (!is_obj()) return nullptr;
        const auto& o = std::get<Object>(val);
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
        out.push_back('"');
        return out;
    }

    std::string dump(int indent = -1, int level = 0) const {
        std::ostringstream os;
        dump_to(os, indent, level);
        return os.str();
    }

    void dump_to(std::ostringstream& os, int indent, int level) const {
        if (is_null()) {
            os << "null";
        } else if (is_bool()) {
            os << (std::get<bool>(val) ? "true" : "false");
        } else if (is_num()) {
            double d = std::get<double>(val);
            if (!std::isfinite(d)) {
                os << "0";
            } else if (d == std::floor(d) && std::fabs(d) < 9e15) {
                os << static_cast<long long>(d);
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.7g", d);
                os << buf;
            }
        } else if (is_str()) {
            os << escape(std::get<std::string>(val));
        } else if (is_arr()) {
            const auto& a = std::get<Array>(val);
            os << '[';
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) os << ',';
                if (indent >= 0) { os << '\n' << std::string((level + 1) * indent, ' '); }
                a[i].dump_to(os, indent, level + 1);
            }
            if (indent >= 0 && !a.empty()) os << '\n' << std::string(level * indent, ' ');
            os << ']';
        } else if (is_obj()) {
            const auto& o = std::get<Object>(val);
            os << '{';
            bool first = true;
            for (const auto& [k, v] : o) {
                if (!first) os << ',';
                first = false;
                if (indent >= 0) { os << '\n' << std::string((level + 1) * indent, ' '); }
                os << escape(k) << (indent >= 0 ? ": " : ":");
                v.dump_to(os, indent, level + 1);
            }
            if (indent >= 0 && !o.empty()) os << '\n' << std::string(level * indent, ' ');
            os << '}';
        }
    }

    static bool parse(const std::string& text, Json& out, std::string& err) {
        size_t pos = 0;
        auto skip_ws = [&]() {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) ++pos;
        };
        auto parse_str = [&](std::string& s) -> bool {
            if (pos >= text.size() || text[pos] != '"') { err = "Expected '\"'"; return false; }
            ++pos;
            s.clear();
            while (pos < text.size()) {
                char c = text[pos++];
                if (c == '"') return true;
                if (c == '\\') {
                    if (pos >= text.size()) { err = "Unterminated escape"; return false; }
                    char e = text[pos++];
                    switch (e) {
                        case '"': s.push_back('"'); break;
                        case '\\': s.push_back('\\'); break;
                        case '/': s.push_back('/'); break;
                        case 'b': s.push_back('\b'); break;
                        case 'f': s.push_back('\f'); break;
                        case 'n': s.push_back('\n'); break;
                        case 'r': s.push_back('\r'); break;
                        case 't': s.push_back('\t'); break;
                        case 'u': {
                            if (pos + 4 > text.size()) { err = "Bad \\u escape"; return false; }
                            unsigned cp = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = text[pos++];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= (h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                                else { err = "Bad hex in \\u"; return false; }
                            }
                            if (cp < 0x80) s.push_back(static_cast<char>(cp));
                            else if (cp < 0x800) {
                                s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                            break;
                        }
                        default: s.push_back(e); break;
                    }
                } else {
                    s.push_back(c);
                }
            }
            err = "Unterminated string";
            return false;
        };

        auto parse_val = [&](auto&& self, Json& node) -> bool {
            skip_ws();
            if (pos >= text.size()) { err = "Unexpected EOF"; return false; }
            char c = text[pos];
            if (c == '{') {
                ++pos;
                Object obj;
                skip_ws();
                if (pos < text.size() && text[pos] == '}') { ++pos; node = Json(std::move(obj)); return true; }
                while (true) {
                    skip_ws();
                    std::string key;
                    if (!parse_str(key)) return false;
                    skip_ws();
                    if (pos >= text.size() || text[pos] != ':') { err = "Expected ':'"; return false; }
                    ++pos;
                    Json v;
                    if (!self(self, v)) return false;
                    obj[std::move(key)] = std::move(v);
                    skip_ws();
                    if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                    if (pos < text.size() && text[pos] == '}') { ++pos; node = Json(std::move(obj)); return true; }
                    err = "Expected ',' or '}'";
                    return false;
                }
            } else if (c == '[') {
                ++pos;
                Array arr;
                skip_ws();
                if (pos < text.size() && text[pos] == ']') { ++pos; node = Json(std::move(arr)); return true; }
                while (true) {
                    Json v;
                    if (!self(self, v)) return false;
                    arr.push_back(std::move(v));
                    skip_ws();
                    if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                    if (pos < text.size() && text[pos] == ']') { ++pos; node = Json(std::move(arr)); return true; }
                    err = "Expected ',' or ']'";
                    return false;
                }
            } else if (c == '"') {
                std::string s;
                if (!parse_str(s)) return false;
                node = Json(std::move(s));
                return true;
            } else if (text.compare(pos, 4, "true") == 0) {
                pos += 4; node = Json(true); return true;
            } else if (text.compare(pos, 5, "false") == 0) {
                pos += 5; node = Json(false); return true;
            } else if (text.compare(pos, 4, "null") == 0) {
                pos += 4; node = Json(nullptr); return true;
            } else {
                const char* start = text.c_str() + pos;
                char* endptr = nullptr;
                double d = std::strtod(start, &endptr);
                if (endptr == start) { err = "Invalid JSON token"; return false; }
                pos = static_cast<size_t>(endptr - text.c_str());
                node = Json(d);
                return true;
            }
        };
        return parse_val(parse_val, out);
    }
};

} // namespace bz
