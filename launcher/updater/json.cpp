#include "updater/json.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>

namespace trg::update::json {
namespace {

constexpr int kMaxDepth = 64;

const Value::Array& EmptyArray() {
  static const Value::Array empty;
  return empty;
}

const Value::Object& EmptyObject() {
  static const Value::Object empty;
  return empty;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void AppendUtf8(std::string& text, std::uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    text += char(codepoint);
  } else if (codepoint <= 0x7FF) {
    text += char(0xC0 | (codepoint >> 6));
    text += char(0x80 | (codepoint & 0x3F));
  } else if (codepoint <= 0xFFFF) {
    text += char(0xE0 | (codepoint >> 12));
    text += char(0x80 | ((codepoint >> 6) & 0x3F));
    text += char(0x80 | (codepoint & 0x3F));
  } else {
    text += char(0xF0 | (codepoint >> 18));
    text += char(0x80 | ((codepoint >> 12) & 0x3F));
    text += char(0x80 | ((codepoint >> 6) & 0x3F));
    text += char(0x80 | (codepoint & 0x3F));
  }
}

bool ValidUtf8(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }
    size_t extra = 0;
    if ((lead & 0xE0) == 0xC0) {
      extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= text.size()) {
      return false;
    }
    for (size_t j = 1; j <= extra; ++j) {
      if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) {
        return false;
      }
    }
    i += extra + 1;
  }
  return true;
}

}  // namespace

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<Value> Run(std::string* error) {
    Value value;
    SkipSpace();
    if (!ParseValue(value)) {
      if (error) {
        *error = error_;
      }
      return std::nullopt;
    }
    SkipSpace();
    if (position_ != text_.size()) {
      Fail("unexpected text after the JSON value");
      if (error) {
        *error = error_;
      }
      return std::nullopt;
    }
    return value;
  }

 private:
  friend class Value;

  bool Fail(const char* message) {
    if (error_.empty()) {
      error_ = message;
    }
    return false;
  }

  void SkipSpace() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        break;
      }
      ++position_;
    }
  }

  bool Consume(char expected) {
    if (position_ >= text_.size() || text_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool ParseValue(Value& value) {
    SkipSpace();
    if (position_ >= text_.size()) {
      return Fail("unexpected end of JSON");
    }
    switch (text_[position_]) {
      case '{':
        return ParseObject(value);
      case '[':
        return ParseArray(value);
      case '"':
        value.type_ = Value::Type::kString;
        return ParseString(value.string_);
      case 't':
        return ParseLiteral("true", value, true);
      case 'f':
        return ParseLiteral("false", value, false);
      case 'n':
        return ParseLiteral("null", value, false);
      default:
        return ParseNumber(value);
    }
  }

  bool ParseLiteral(std::string_view literal, Value& value, bool boolean) {
    if (text_.substr(position_, literal.size()) != literal) {
      return Fail("invalid JSON literal");
    }
    position_ += literal.size();
    if (literal == "null") {
      value.type_ = Value::Type::kNull;
    } else {
      value.type_ = Value::Type::kBool;
      value.bool_ = boolean;
    }
    return true;
  }

  bool ParseObject(Value& value) {
    if (++depth_ > kMaxDepth) {
      return Fail("JSON nesting is too deep");
    }
    ++position_;
    value.type_ = Value::Type::kObject;
    value.object_ = std::make_shared<Value::Object>();
    SkipSpace();
    if (Consume('}')) {
      --depth_;
      return true;
    }
    while (true) {
      SkipSpace();
      std::string key;
      if (!ParseString(key)) {
        return false;
      }
      SkipSpace();
      if (!Consume(':')) {
        return Fail("expected ':' after a JSON object key");
      }
      Value member;
      if (!ParseValue(member)) {
        return false;
      }
      if (!value.object_->emplace(std::move(key), std::move(member)).second) {
        return Fail("duplicate JSON object key");
      }
      SkipSpace();
      if (Consume('}')) {
        --depth_;
        return true;
      }
      if (!Consume(',')) {
        return Fail("expected ',' or '}' in a JSON object");
      }
    }
  }

  bool ParseArray(Value& value) {
    if (++depth_ > kMaxDepth) {
      return Fail("JSON nesting is too deep");
    }
    ++position_;
    value.type_ = Value::Type::kArray;
    value.array_ = std::make_shared<Value::Array>();
    SkipSpace();
    if (Consume(']')) {
      --depth_;
      return true;
    }
    while (true) {
      Value element;
      if (!ParseValue(element)) {
        return false;
      }
      value.array_->push_back(std::move(element));
      SkipSpace();
      if (Consume(']')) {
        --depth_;
        return true;
      }
      if (!Consume(',')) {
        return Fail("expected ',' or ']' in a JSON array");
      }
    }
  }

  bool ParseString(std::string& text) {
    if (!Consume('"')) {
      return Fail("expected a JSON string");
    }
    text.clear();
    while (position_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[position_++]);
      if (c == '"') {
        return ValidUtf8(text) ? true : Fail("invalid UTF-8 in a JSON string");
      }
      if (c < 0x20) {
        return Fail("control character in a JSON string");
      }
      if (c != '\\') {
        text += char(c);
        continue;
      }
      if (position_ >= text_.size()) {
        return Fail("unfinished escape in a JSON string");
      }
      const char escape = text_[position_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          text += escape;
          break;
        case 'b':
          text += '\b';
          break;
        case 'f':
          text += '\f';
          break;
        case 'n':
          text += '\n';
          break;
        case 'r':
          text += '\r';
          break;
        case 't':
          text += '\t';
          break;
        case 'u': {
          std::uint32_t codepoint = 0;
          if (!ParseHex4(codepoint)) {
            return false;
          }
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (position_ + 1 >= text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
              return Fail("unpaired high surrogate in a JSON string");
            }
            position_ += 2;
            std::uint32_t low = 0;
            if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
              return Fail("invalid low surrogate in a JSON string");
            }
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
          } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return Fail("unpaired low surrogate in a JSON string");
          }
          AppendUtf8(text, codepoint);
          break;
        }
        default:
          return Fail("invalid escape in a JSON string");
      }
    }
    return Fail("unfinished JSON string");
  }

  bool ParseHex4(std::uint32_t& value) {
    if (position_ + 4 > text_.size()) {
      return Fail("unfinished unicode escape in a JSON string");
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = HexDigit(text_[position_++]);
      if (digit < 0) {
        return Fail("invalid unicode escape in a JSON string");
      }
      value = (value << 4) | std::uint32_t(digit);
    }
    return true;
  }

  bool ParseNumber(Value& value) {
    const size_t start = position_;
    if (Consume('-') && position_ == text_.size()) {
      return Fail("invalid JSON number");
    }
    if (position_ >= text_.size()) {
      return Fail("invalid JSON number");
    }
    if (text_[position_] == '0') {
      ++position_;
    } else if (text_[position_] >= '1' && text_[position_] <= '9') {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    } else {
      return Fail("invalid JSON number");
    }
    bool integer = true;
    if (position_ < text_.size() && text_[position_] == '.') {
      integer = false;
      ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return Fail("invalid JSON number");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      integer = false;
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return Fail("invalid JSON number");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    const std::string_view token = text_.substr(start, position_ - start);
    value.type_ = Value::Type::kNumber;
    if (integer) {
      std::int64_t parsed = 0;
      const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
      if (result.ec == std::errc() && result.ptr == token.data() + token.size()) {
        value.integer_ = parsed;
        value.is_integer_ = true;
      }
    }
    double parsed = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed, std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size() || !std::isfinite(parsed)) {
      return Fail("invalid JSON number");
    }
    value.number_ = parsed;
    return true;
  }

  std::string_view text_;
  size_t position_ = 0;
  int depth_ = 0;
  std::string error_;
};

std::optional<std::int64_t> Value::AsInt64() const {
  if (type_ != Type::kNumber || !is_integer_) {
    return std::nullopt;
  }
  return integer_;
}

const Value::Array& Value::AsArray() const {
  return type_ == Type::kArray && array_ ? *array_ : EmptyArray();
}

const Value::Object& Value::AsObject() const {
  return type_ == Type::kObject && object_ ? *object_ : EmptyObject();
}

const Value* Value::Find(std::string_view key) const {
  if (type_ != Type::kObject || !object_) {
    return nullptr;
  }
  const auto found = object_->find(std::string(key));
  return found == object_->end() ? nullptr : &found->second;
}

const Value* Value::At(size_t index) const {
  if (type_ != Type::kArray || !array_ || index >= array_->size()) {
    return nullptr;
  }
  return &(*array_)[index];
}

std::optional<Value> Parse(std::string_view text, std::string* error) {
  return Parser(text).Run(error);
}

}  // namespace trg::update::json
