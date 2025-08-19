#include "json_parser.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

// JsonValue implementation
JsonValue::JsonValue(JsonType type) : m_type(type) {}

JsonValue::~JsonValue() {}

JsonType JsonValue::GetType() const { return m_type; }

// JsonNull implementation
JsonNull::JsonNull() : JsonValue(JSON_NULL) {}

JsonNull::~JsonNull() {}

JsonValue* JsonNull::Clone() const { return new JsonNull(); }

std::string JsonNull::ToString() const { return "null"; }

// JsonBool implementation
JsonBool::JsonBool(bool value) : JsonValue(JSON_BOOL), m_value(value) {}

JsonBool::~JsonBool() {}

JsonValue* JsonBool::Clone() const { return new JsonBool(m_value); }

std::string JsonBool::ToString() const { return m_value ? "true" : "false"; }

bool JsonBool::GetValue() const { return m_value; }

// JsonNumber implementation
JsonNumber::JsonNumber(double value) : JsonValue(JSON_NUMBER), m_value(value) {}

JsonNumber::~JsonNumber() {}

JsonValue* JsonNumber::Clone() const { return new JsonNumber(m_value); }

std::string JsonNumber::ToString() const {
  std::ostringstream oss;
  oss << m_value;
  return oss.str();
}

double JsonNumber::GetValue() const { return m_value; }

// JsonString implementation
JsonString::JsonString(const std::string& value)
    : JsonValue(JSON_STRING), m_value(value) {}

JsonString::~JsonString() {}

JsonValue* JsonString::Clone() const { return new JsonString(m_value); }

std::string JsonString::ToString() const {
  std::ostringstream oss;
  oss << "\"";
  // Simple escaping - in a full implementation, we'd handle all JSON escape
  // sequences
  for (size_t i = 0; i < m_value.length(); ++i) {
    char c = m_value[i];
    if (c == '"') {
      oss << "\\\"";
    } else if (c == '\\') {
      oss << "\\\\";
    } else if (c == '\n') {
      oss << "\\n";
    } else if (c == '\r') {
      oss << "\\r";
    } else if (c == '\t') {
      oss << "\\t";
    } else {
      oss << c;
    }
  }
  oss << "\"";
  return oss.str();
}

const std::string& JsonString::GetValue() const { return m_value; }

// JsonArray implementation
JsonArray::JsonArray() : JsonValue(JSON_ARRAY) {}

JsonArray::~JsonArray() {
  for (size_t i = 0; i < m_values.size(); ++i) {
    delete m_values[i];
  }
}

JsonValue* JsonArray::Clone() const {
  JsonArray* clone = new JsonArray();
  for (size_t i = 0; i < m_values.size(); ++i) {
    clone->AddValue(m_values[i]->Clone());
  }
  return clone;
}

std::string JsonArray::ToString() const {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < m_values.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << m_values[i]->ToString();
  }
  oss << "]";
  return oss.str();
}

void JsonArray::AddValue(JsonValue* value) { m_values.push_back(value); }

size_t JsonArray::GetSize() const { return m_values.size(); }

const JsonValue* JsonArray::GetValue(size_t index) const {
  if (index >= m_values.size()) {
    throw std::out_of_range("Array index out of range");
  }
  return m_values[index];
}

// JsonObject implementation
JsonObject::JsonObject() : JsonValue(JSON_OBJECT) {}

JsonObject::~JsonObject() {
  for (std::map<std::string, JsonValue*>::iterator it = m_values.begin();
       it != m_values.end(); ++it) {
    delete it->second;
  }
}

JsonValue* JsonObject::Clone() const {
  JsonObject* clone = new JsonObject();
  for (std::map<std::string, JsonValue*>::const_iterator it = m_values.begin();
       it != m_values.end(); ++it) {
    clone->SetValue(it->first, it->second->Clone());
  }
  return clone;
}

std::string JsonObject::ToString() const {
  std::ostringstream oss;
  oss << "{";
  bool first = true;
  for (std::map<std::string, JsonValue*>::const_iterator it = m_values.begin();
       it != m_values.end(); ++it) {
    if (!first) {
      oss << ", ";
    }
    first = false;
    oss << "\"" << it->first << "\": " << it->second->ToString();
  }
  oss << "}";
  return oss.str();
}

void JsonObject::SetValue(const std::string& key, JsonValue* value) {
  std::map<std::string, JsonValue*>::iterator it = m_values.find(key);
  if (it != m_values.end()) {
    delete it->second;
    it->second = value;
  } else {
    m_values[key] = value;
  }
}

const JsonValue* JsonObject::GetValue(const std::string& key) const {
  std::map<std::string, JsonValue*>::const_iterator it = m_values.find(key);
  if (it != m_values.end()) {
    return it->second;
  }
  return NULL;
}

bool JsonObject::HasKey(const std::string& key) const {
  return m_values.find(key) != m_values.end();
}

std::vector<std::string> JsonObject::GetKeys() const {
  std::vector<std::string> keys;
  for (std::map<std::string, JsonValue*>::const_iterator it = m_values.begin();
       it != m_values.end(); ++it) {
    keys.push_back(it->first);
  }
  return keys;
}

// JsonParser implementation
JsonParser::JsonParser() : m_start(NULL), m_current(NULL), m_end(NULL) {}

JsonParser::~JsonParser() {}

JsonValue* JsonParser::Parse(const std::string& json) {
  m_start = json.c_str();
  m_current = m_start;
  m_end = m_current + json.length();

  SkipWhitespace();
  if (IsAtEnd()) {
    ThrowError("Empty JSON string");
  }

  JsonValue* result = ParseValue();

  SkipWhitespace();
  if (!IsAtEnd()) {
    delete result;
    ThrowError("Unexpected characters after JSON value");
  }

  return result;
}

JsonValue* JsonParser::ParseValue() {
  SkipWhitespace();

  char c = PeekChar();
  switch (c) {
    case 'n':
      return ParseNull();
    case 't':
    case 'f':
      return ParseBool();
    case '"':
      return ParseString();
    case '[':
      return ParseArray();
    case '{':
      return ParseObject();
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return ParseNumber();
    default:
      ThrowError("Unexpected character in JSON");
      return NULL;  // Never reached
  }
}

JsonNull* JsonParser::ParseNull() {
  if (m_end - m_current < 4 || m_current[0] != 'n' || m_current[1] != 'u' ||
      m_current[2] != 'l' || m_current[3] != 'l') {
    ThrowError("Invalid null value");
  }
  m_current += 4;
  return new JsonNull();
}

JsonBool* JsonParser::ParseBool() {
  if (m_end - m_current >= 4 && m_current[0] == 't' && m_current[1] == 'r' &&
      m_current[2] == 'u' && m_current[3] == 'e') {
    m_current += 4;
    return new JsonBool(true);
  } else if (m_end - m_current >= 5 && m_current[0] == 'f' &&
             m_current[1] == 'a' && m_current[2] == 'l' &&
             m_current[3] == 's' && m_current[4] == 'e') {
    m_current += 5;
    return new JsonBool(false);
  } else {
    ThrowError("Invalid boolean value");
    return NULL;  // Never reached
  }
}

JsonNumber* JsonParser::ParseNumber() {
  double value = ParseNumberLiteral();
  return new JsonNumber(value);
}

JsonString* JsonParser::ParseString() {
  std::string value = ParseStringLiteral();
  return new JsonString(value);
}

JsonArray* JsonParser::ParseArray() {
  ExpectChar('[');
  JsonArray* array = new JsonArray();

  try {
    SkipWhitespace();
    if (PeekChar() == ']') {
      NextChar();  // consume ']'
      return array;
    }

    while (true) {
      JsonValue* value = ParseValue();
      array->AddValue(value);

      SkipWhitespace();
      char c = NextChar();
      if (c == ']') {
        break;
      } else if (c == ',') {
        SkipWhitespace();
        if (PeekChar() == ']') {
          ThrowError("Trailing comma in array");
        }
      } else {
        ThrowError("Expected ',' or ']' in array");
      }
    }

    return array;
  } catch (...) {
    delete array;
    throw;
  }
}

JsonObject* JsonParser::ParseObject() {
  ExpectChar('{');
  JsonObject* object = new JsonObject();

  try {
    SkipWhitespace();
    if (PeekChar() == '}') {
      NextChar();  // consume '}'
      return object;
    }

    while (true) {
      SkipWhitespace();
      if (PeekChar() != '"') {
        ThrowError("Expected string key in object");
      }

      std::string key = ParseStringLiteral();

      SkipWhitespace();
      ExpectChar(':');

      JsonValue* value = ParseValue();
      object->SetValue(key, value);

      SkipWhitespace();
      char c = NextChar();
      if (c == '}') {
        break;
      } else if (c == ',') {
        SkipWhitespace();
        if (PeekChar() == '}') {
          ThrowError("Trailing comma in object");
        }
      } else {
        ThrowError("Expected ',' or '}' in object");
      }
    }

    return object;
  } catch (...) {
    delete object;
    throw;
  }
}

void JsonParser::SkipWhitespace() {
  while (!IsAtEnd() && std::isspace(*m_current)) {
    ++m_current;
  }
}

char JsonParser::PeekChar() const {
  if (IsAtEnd()) {
    ThrowError("Unexpected end of JSON");
  }
  return *m_current;
}

char JsonParser::NextChar() {
  char c = PeekChar();
  ++m_current;
  return c;
}

void JsonParser::ExpectChar(char expected) {
  char c = NextChar();
  if (c != expected) {
    std::ostringstream oss;
    oss << "Expected '" << expected << "' but got '" << c << "'";
    ThrowError(oss.str());
  }
}

bool JsonParser::IsAtEnd() const { return m_current >= m_end; }

std::string JsonParser::ParseStringLiteral() {
  ExpectChar('"');
  std::string result;

  while (!IsAtEnd() && PeekChar() != '"') {
    char c = NextChar();
    if (c == '\\') {
      if (IsAtEnd()) {
        ThrowError("Unterminated escape sequence");
      }
      char escaped = NextChar();
      switch (escaped) {
        case '"':
          result += '"';
          break;
        case '\\':
          result += '\\';
          break;
        case '/':
          result += '/';
          break;
        case 'b':
          result += '\b';
          break;
        case 'f':
          result += '\f';
          break;
        case 'n':
          result += '\n';
          break;
        case 'r':
          result += '\r';
          break;
        case 't':
          result += '\t';
          break;
        default:
          ThrowError("Invalid escape sequence");
      }
    } else if (c < 32) {
      ThrowError("Control character in string");
    } else {
      result += c;
    }
  }

  ExpectChar('"');
  return result;
}

double JsonParser::ParseNumberLiteral() {
  const char* start = m_current;

  // Handle optional minus sign
  if (PeekChar() == '-') {
    NextChar();
  }

  // Parse integer part
  if (PeekChar() == '0') {
    NextChar();
  } else if (std::isdigit(PeekChar())) {
    while (!IsAtEnd() && std::isdigit(PeekChar())) {
      NextChar();
    }
  } else {
    ThrowError("Invalid number format");
  }

  // Parse optional fractional part
  if (!IsAtEnd() && PeekChar() == '.') {
    NextChar();
    if (IsAtEnd() || !std::isdigit(PeekChar())) {
      ThrowError("Invalid number format after decimal point");
    }
    while (!IsAtEnd() && std::isdigit(PeekChar())) {
      NextChar();
    }
  }

  // Parse optional exponent part
  if (!IsAtEnd() && (PeekChar() == 'e' || PeekChar() == 'E')) {
    NextChar();
    if (!IsAtEnd() && (PeekChar() == '+' || PeekChar() == '-')) {
      NextChar();
    }
    if (IsAtEnd() || !std::isdigit(PeekChar())) {
      ThrowError("Invalid number format in exponent");
    }
    while (!IsAtEnd() && std::isdigit(PeekChar())) {
      NextChar();
    }
  }

  // Convert to double
  std::string numberStr(start, m_current);
  char* endPtr;
  double value = std::strtod(numberStr.c_str(), &endPtr);
  if (endPtr != numberStr.c_str() + numberStr.length()) {
    ThrowError("Invalid number format");
  }

  return value;
}

void JsonParser::ThrowError(const std::string& message) const {
  std::ostringstream oss;
  oss << "JSON Parse Error: " << message;
  if (m_start && m_current && m_end) {
    size_t position = m_current - m_start;
    oss << " at position " << position;
  }
  throw std::runtime_error(oss.str());
}

// JsonValuePtr implementation (RAII helper)
JsonValuePtr::JsonValuePtr(JsonValue* value) : m_value(value) {}

JsonValuePtr::~JsonValuePtr() { delete m_value; }

JsonValue* JsonValuePtr::Get() const { return m_value; }

JsonValue* JsonValuePtr::Release() {
  JsonValue* temp = m_value;
  m_value = NULL;
  return temp;
}

void JsonValuePtr::Reset(JsonValue* value) {
  delete m_value;
  m_value = value;
}
