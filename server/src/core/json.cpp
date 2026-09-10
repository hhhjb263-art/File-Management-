#include "core/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cv {
namespace json {
namespace {

class Parser {
 public:
  Parser(const std::string& text) : s_(text) {}

  bool run(Value& out) { return parseValue(out) && skipTrailing(); }

  const std::string& error() const { return err_; }

 private:
  const std::string& s_;
  std::size_t i_ = 0;
  std::string err_;

  bool fail(const char* msg) {
    if (err_.empty()) err_ = msg;
    return false;
  }

  void ws() {
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++i_;
      else
        break;
    }
  }

  bool skipTrailing() {
    ws();
    return i_ == s_.size() ? true : fail("trailing content");
  }

  bool parseValue(Value& out) {
    ws();
    if (i_ >= s_.size()) return fail("unexpected end");
    char c = s_[i_];
    switch (c) {
      case '{': return parseObject(out);
      case '[': return parseArray(out);
      case '"': {
        std::string str;
        if (!parseString(str)) return false;
        out = Value(str);
        return true;
      }
      case 't': return literal("true", out, Value(true));
      case 'f': return literal("false", out, Value(false));
      case 'n': return literal("null", out, Value());
      default: return parseNumber(out);
    }
  }

  bool literal(const char* word, Value& out, const Value& v) {
    std::size_t n = std::string(word).size();
    if (s_.compare(i_, n, word) != 0) return fail("bad literal");
    i_ += n;
    out = v;
    return true;
  }

  bool parseObject(Value& out) {
    ++i_;  // {
    out = Value::object();
    ws();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return true;
    }
    while (true) {
      ws();
      if (i_ >= s_.size() || s_[i_] != '"') return fail("expected key");
      std::string key;
      if (!parseString(key)) return false;
      ws();
      if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
      ++i_;
      Value v;
      if (!parseValue(v)) return false;
      out.set(key, v);
      ws();
      if (i_ >= s_.size()) return fail("unexpected end");
      if (s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (s_[i_] == '}') {
        ++i_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool parseArray(Value& out) {
    ++i_;  // [
    out = Value::array();
    ws();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return true;
    }
    while (true) {
      Value v;
      if (!parseValue(v)) return false;
      out.push_back(v);
      ws();
      if (i_ >= s_.size()) return fail("unexpected end");
      if (s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (s_[i_] == ']') {
        ++i_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseHex4(unsigned int& out) {
    if (i_ + 4 > s_.size()) return fail("bad \\u escape");
    out = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s_[i_ + k];
      unsigned int d;
      if (c >= '0' && c <= '9')
        d = static_cast<unsigned int>(c - '0');
      else if (c >= 'a' && c <= 'f')
        d = static_cast<unsigned int>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        d = static_cast<unsigned int>(c - 'A' + 10);
      else
        return fail("bad \\u escape");
      out = (out << 4) | d;
    }
    i_ += 4;
    return true;
  }

  static void encodeUtf8(unsigned int cp, std::string& out) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  }

  bool parseString(std::string& out) {
    if (i_ >= s_.size() || s_[i_] != '"') return fail("expected string");
    ++i_;
    out.clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i_ >= s_.size()) return fail("bad escape");
      char e = s_[i_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned int cp = 0;
          if (!parseHex4(cp)) return false;
          if (cp >= 0xd800 && cp <= 0xdbff && i_ + 1 < s_.size() && s_[i_] == '\\' &&
              s_[i_ + 1] == 'u') {
            std::size_t save = i_;
            i_ += 2;
            unsigned int lo = 0;
            if (!parseHex4(lo)) return false;
            if (lo >= 0xdc00 && lo <= 0xdfff) {
              cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            } else {
              i_ = save;
            }
          }
          encodeUtf8(cp, out);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool parseNumber(Value& out) {
    std::size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
    bool anyDigit = false;
    while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) ||
                              s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
                              ((s_[i_] == '-' || s_[i_] == '+') &&
                               (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E')))) {
      if (std::isdigit(static_cast<unsigned char>(s_[i_]))) anyDigit = true;
      ++i_;
    }
    if (!anyDigit) return fail("bad number");
    double d = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
    out = Value(d);
    return true;
  }
};

void dumpString(const std::string& s, std::string& out) {
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
}

void dumpValue(const Value& v, std::string& out) {
  switch (v.type()) {
    case Value::Type::Null:
      out += "null";
      break;
    case Value::Type::Bool:
      out += v.boolValue() ? "true" : "false";
      break;
    case Value::Type::Number: {
      double d = v.numberValue();
      if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        out += buf;
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
      }
      break;
    }
    case Value::Type::String:
      dumpString(v.stringValue(), out);
      break;
    case Value::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const Value& item : v.items()) {
        if (!first) out.push_back(',');
        first = false;
        dumpValue(item, out);
      }
      out.push_back(']');
      break;
    }
    case Value::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& kv : v.fields()) {
        if (!first) out.push_back(',');
        first = false;
        dumpString(kv.first, out);
        out.push_back(':');
        dumpValue(kv.second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
  Parser p(text);
  if (!p.run(out)) {
    err = p.error();
    return false;
  }
  return true;
}

std::string dump(const Value& v) {
  std::string out;
  dumpValue(v, out);
  return out;
}

}  // namespace json
}  // namespace cv
