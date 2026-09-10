#pragma once

#include <map>
#include <string>
#include <vector>

namespace cv {
namespace json {

// 零依赖最小 JSON：够用于 REST 接口（对象/数组/字符串/数字/布尔/null）。
class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool v) : type_(Type::Bool), bool_(v) {}
  Value(int v) : type_(Type::Number), number_(static_cast<double>(v)) {}
  Value(long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
  Value(double v) : type_(Type::Number), number_(v) {}
  Value(const char* v) : type_(Type::String), string_(v ? v : "") {}
  Value(const std::string& v) : type_(Type::String), string_(v) {}

  static Value array() { return Value(Type::Array); }
  static Value object() { return Value(Type::Object); }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }

  void push_back(const Value& v) { array_.push_back(v); }
  void set(const std::string& key, const Value& v) { object_[key] = v; }
  void set(const std::string& key, const char* v) { object_[key] = Value(v); }

  const Value* find(const std::string& key) const {
    auto it = object_.find(key);
    return it == object_.end() ? nullptr : &it->second;
  }

  bool boolValue(bool def = false) const {
    return type_ == Type::Bool ? bool_ : def;
  }
  double numberValue(double def = 0.0) const {
    return type_ == Type::Number ? number_ : def;
  }
  std::string stringValue(const std::string& def = "") const {
    return type_ == Type::String ? string_ : def;
  }
  const std::vector<Value>& items() const { return array_; }
  const std::map<std::string, Value>& fields() const { return object_; }

 private:
  explicit Value(Type t) : type_(t) {}

  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> array_;
  std::map<std::string, Value> object_;
};

// 解析；失败时 err 给出原因。
bool parse(const std::string& text, Value& out, std::string& err);

// 序列化为紧凑 JSON 字符串。
std::string dump(const Value& v);

}  // namespace json
}  // namespace cv
