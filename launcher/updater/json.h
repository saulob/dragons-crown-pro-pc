#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trg::update::json {

class Value {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;

  Value() = default;

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::kNull; }
  bool IsBool() const { return type_ == Type::kBool; }
  bool IsNumber() const { return type_ == Type::kNumber; }
  bool IsString() const { return type_ == Type::kString; }
  bool IsArray() const { return type_ == Type::kArray; }
  bool IsObject() const { return type_ == Type::kObject; }

  bool AsBool() const { return bool_; }
  double AsNumber() const { return number_; }
  std::optional<std::int64_t> AsInt64() const;
  const std::string& AsString() const { return string_; }
  const Array& AsArray() const;
  const Object& AsObject() const;

  const Value* Find(std::string_view key) const;
  const Value* At(size_t index) const;

 private:
  friend class Parser;

  Type type_ = Type::kNull;
  bool bool_ = false;
  double number_ = 0.0;
  std::int64_t integer_ = 0;
  bool is_integer_ = false;
  std::string string_;
  std::shared_ptr<Array> array_;
  std::shared_ptr<Object> object_;
};

std::optional<Value> Parse(std::string_view text, std::string* error = nullptr);

}  // namespace trg::update::json
