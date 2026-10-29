#include "net/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dcp::net {
namespace {

const Json kNull;
const std::string kEmpty;
const std::vector<Json> kNoItems;

class Parser {
public:
    Parser(std::string_view text, size_t max_depth) : text_(text), max_depth_(max_depth) {}

    std::optional<Json> Run() {
        Json value;
        if (!Value(value, 0)) return std::nullopt;
        Space();
        if (pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void Space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool Literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool Value(Json& out, size_t depth) {
        if (depth > max_depth_) return false;
        Space();
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '{') return ObjectValue(out, depth);
        if (c == '[') return ArrayValue(out, depth);
        if (c == '"') {
            std::string s;
            if (!String(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (c == 't') return Literal("true") && (out = Json(true), true);
        if (c == 'f') return Literal("false") && (out = Json(false), true);
        if (c == 'n') return Literal("null") && (out = Json(), true);
        return Number(out);
    }

    bool ObjectValue(Json& out, size_t depth) {
        ++pos_;
        out = Json::Object();
        Space();
        if (pos_ < text_.size() && text_[pos_] == '}') return ++pos_, true;
        for (;;) {
            Space();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !String(key)) return false;
            Space();
            if (pos_ >= text_.size() || text_[pos_++] != ':') return false;
            Json value;
            if (!Value(value, depth + 1)) return false;
            out.set(key, std::move(value));
            Space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') return ++pos_, true;
            return false;
        }
    }

    bool ArrayValue(Json& out, size_t depth) {
        ++pos_;
        out = Json::Array();
        Space();
        if (pos_ < text_.size() && text_[pos_] == ']') return ++pos_, true;
        for (;;) {
            Json value;
            if (!Value(value, depth + 1)) return false;
            out.push(std::move(value));
            Space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') return ++pos_, true;
            return false;
        }
    }

    bool Hex4(uint32_t& v) {
        if (pos_ + 4 > text_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void Utf8(std::string& out, uint32_t c) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += char(0xe0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        } else {
            out += char(0xf0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3f));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        }
    }

    bool String(std::string& out) {
        ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xd800 && cp < 0xdc00 && text_.substr(pos_, 2) == "\\u") {
                    pos_ += 2;
                    uint32_t low;
                    if (!Hex4(low) || low < 0xdc00 || low >= 0xe000) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xd800 && cp < 0xe000) {
                    cp = 0xfffd;
                }
                Utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool Number(Json& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        bool digits = false, fraction = false;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_, digits = true;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            fraction = true;
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            fraction = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (!digits || pos_ - start > 64) return false;
        const std::string number(text_.substr(start, pos_ - start));
        if (!fraction) {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(number.c_str(), &end, 10);
            if (errno == 0 && end && *end == 0) {
                out = Json(int64_t(v));
                return true;
            }
        }
        const double d = std::strtod(number.c_str(), nullptr);
        if (!std::isfinite(d)) return false;
        out = Json(d);
        return true;
    }

    std::string_view text_;
    size_t pos_ = 0;
    size_t max_depth_;
};

void Escape(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

} // namespace

int64_t Json::as_int(int64_t def) const {
    if (type_ == Type::kInt) return int_;
    if (type_ == Type::kDouble && std::isfinite(double_) && std::fabs(double_) < 9.2e18) return int64_t(double_);
    return def;
}

const std::string& Json::as_string() const { return type_ == Type::kString ? string_ : kEmpty; }

uint64_t Json::as_u64() const {
    if (type_ == Type::kInt) return int_ > 0 ? uint64_t(int_) : 0;
    if (type_ != Type::kString || string_.empty() || string_.size() > 20) return 0;
    uint64_t v = 0;
    for (const char c : string_) {
        if (c < '0' || c > '9') return 0;
        const uint64_t digit = uint64_t(c - '0');
        if (v > (UINT64_MAX - digit) / 10) return 0;
        v = v * 10 + digit;
    }
    return v;
}

const Json& Json::operator[](std::string_view key) const {
    if (type_ == Type::kObject)
        for (const auto& [k, v] : object_)
            if (k == key) return v;
    return kNull;
}

bool Json::has(std::string_view key) const {
    if (type_ == Type::kObject)
        for (const auto& [k, v] : object_)
            if (k == key) return true;
    return false;
}

Json& Json::set(std::string_view key, Json value) {
    if (type_ != Type::kObject) *this = Object();
    for (auto& [k, v] : object_) {
        if (k == key) {
            v = std::move(value);
            return v;
        }
    }
    object_.emplace_back(std::string(key), std::move(value));
    return object_.back().second;
}

const std::vector<Json>& Json::items() const { return type_ == Type::kArray ? array_ : kNoItems; }

Json& Json::push(Json value) {
    if (type_ != Type::kArray) *this = Array();
    array_.push_back(std::move(value));
    return array_.back();
}

void Json::dump_to(std::string& out) const {
    switch (type_) {
    case Type::kNull: out += "null"; break;
    case Type::kBool: out += int_ ? "true" : "false"; break;
    case Type::kInt: out += std::to_string(int_); break;
    case Type::kDouble: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", double_);
        out += buf;
        break;
    }
    case Type::kString: Escape(out, string_); break;
    case Type::kArray:
        out += '[';
        for (size_t i = 0; i < array_.size(); ++i) {
            if (i) out += ',';
            array_[i].dump_to(out);
        }
        out += ']';
        break;
    case Type::kObject:
        out += '{';
        for (size_t i = 0; i < object_.size(); ++i) {
            if (i) out += ',';
            Escape(out, object_[i].first);
            out += ':';
            object_[i].second.dump_to(out);
        }
        out += '}';
        break;
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

std::optional<Json> Json::parse(std::string_view text, size_t max_depth) { return Parser(text, max_depth).Run(); }

std::string u64_text(uint64_t value) { return std::to_string(value); }

std::string hex_encode(const void* data, size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = kDigits[p[i] >> 4];
        out[i * 2 + 1] = kDigits[p[i] & 15];
    }
    return out;
}

bool hex_decode(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.size() % 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

} // namespace dcp::net
