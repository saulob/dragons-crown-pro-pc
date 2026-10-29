// JSON for the lobby protocol (docs/ONLINE.md): objects keep their insertion order, numbers are 64-bit integers or
// doubles, and parsing refuses deep nesting and oversized input.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dcp::net {

class Json {
public:
    enum class Type { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::kBool), int_(v) {}
    Json(int v) : type_(Type::kInt), int_(v) {}
    Json(int64_t v) : type_(Type::kInt), int_(v) {}
    Json(uint32_t v) : type_(Type::kInt), int_(v) {}
    Json(double v) : type_(Type::kDouble), double_(v) {}
    Json(const char* v) : type_(Type::kString), string_(v) {}
    Json(std::string v) : type_(Type::kString), string_(std::move(v)) {}
    Json(std::string_view v) : type_(Type::kString), string_(v) {}

    static Json Array() { Json j; j.type_ = Type::kArray; return j; }
    static Json Object() { Json j; j.type_ = Type::kObject; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::kNull; }
    bool is_string() const { return type_ == Type::kString; }
    bool is_array() const { return type_ == Type::kArray; }
    bool is_object() const { return type_ == Type::kObject; }
    bool is_number() const { return type_ == Type::kInt || type_ == Type::kDouble; }

    // Defaults for a missing member or a value of another type.
    int64_t as_int(int64_t def = 0) const;
    bool as_bool(bool def = false) const { return type_ == Type::kBool ? int_ != 0 : def; }
    const std::string& as_string() const;
    // A 64-bit id written as a decimal string; 0 when it is not one.
    uint64_t as_u64() const;

    // Object members; a missing key reads as null.
    const Json& operator[](std::string_view key) const;
    Json& set(std::string_view key, Json value);
    bool has(std::string_view key) const;
    const std::vector<std::pair<std::string, Json>>& members() const { return object_; }

    // Array elements (an empty list for other types).
    const std::vector<Json>& items() const;
    Json& push(Json value);
    size_t size() const { return type_ == Type::kArray ? array_.size() : type_ == Type::kObject ? object_.size() : 0; }

    std::string dump() const;
    static std::optional<Json> parse(std::string_view text, size_t max_depth = 32);

private:
    void dump_to(std::string& out) const;

    Type type_ = Type::kNull;
    int64_t int_ = 0;
    double double_ = 0;
    std::string string_;
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

std::string u64_text(uint64_t value);
std::string hex_encode(const void* data, size_t size);
// False for odd length or a character that is not hex.
bool hex_decode(std::string_view text, std::vector<uint8_t>& out);

} // namespace dcp::net
