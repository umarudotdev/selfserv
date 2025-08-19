#include "json_parser.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace selfserv {

// ============================================================================
// JsonValue Safe Casting Methods
// ============================================================================

JsonNull* JsonValue::asNull() {
  return isNull() ? static_cast<JsonNull*>(this) : NULL;
}

JsonBoolean* JsonValue::asBoolean() {
  return isBoolean() ? static_cast<JsonBoolean*>(this) : NULL;
}

JsonNumber* JsonValue::asNumber() {
  return isNumber() ? static_cast<JsonNumber*>(this) : NULL;
}

JsonString* JsonValue::asString() {
  return isString() ? static_cast<JsonString*>(this) : NULL;
}

JsonArray* JsonValue::asArray() {
  return isArray() ? static_cast<JsonArray*>(this) : NULL;
}

JsonObject* JsonValue::asObject() {
  return isObject() ? static_cast<JsonObject*>(this) : NULL;
}

const JsonNull* JsonValue::asNull() const {
  return isNull() ? static_cast<const JsonNull*>(this) : NULL;
}

const JsonBoolean* JsonValue::asBoolean() const {
  return isBoolean() ? static_cast<const JsonBoolean*>(this) : NULL;
}

const JsonNumber* JsonValue::asNumber() const {
  return isNumber() ? static_cast<const JsonNumber*>(this) : NULL;
}

const JsonString* JsonValue::asString() const {
  return isString() ? static_cast<const JsonString*>(this) : NULL;
}

const JsonArray* JsonValue::asArray() const {
  return isArray() ? static_cast<const JsonArray*>(this) : NULL;
}

const JsonObject* JsonValue::asObject() const {
  return isObject() ? static_cast<const JsonObject*>(this) : NULL;
}

// ============================================================================
// JsonNumber Implementation
// ============================================================================

std::string JsonNumber::toString() const {
  std::ostringstream oss;
  oss << m_value;
  return oss.str();
}

// ============================================================================
// JsonString Implementation
// ============================================================================

std::string JsonString::toString() const {
  std::ostringstream oss;
  oss << '"';

  // Escape special characters
  for (size_t i = 0; i < m_value.length(); ++i) {
    char c = m_value[i];
    switch (c) {
      case '"':
        oss << "\\\"";
        break;
      case '\\':
        oss << "\\\\";
        break;
      case '\b':
        oss << "\\b";
        break;
      case '\f':
        oss << "\\f";
        break;
      case '\n':
        oss << "\\n";
        break;
      case '\r':
        oss << "\\r";
        break;
      case '\t':
        oss << "\\t";
        break;
      default:
        if (c < 0x20) {
          oss << "\\u" << std::hex << (int)c;
        } else {
          oss << c;
        }
        break;
    }
  }

  oss << '"';
  return oss.str();
}

// ============================================================================
// JsonArray Implementation
// ============================================================================

JsonArray::~JsonArray() {
  for (std::vector<JsonValue*>::iterator it = m_elements.begin();
       it != m_elements.end(); ++it) {
    delete *it;
  }
}

JsonValue* JsonArray::clone() const {
  JsonArray* newArray = new JsonArray();
  for (std::vector<JsonValue*>::const_iterator it = m_elements.begin();
       it != m_elements.end(); ++it) {
    newArray->push_back((*it)->clone());
  }
  return newArray;
}

std::string JsonArray::toString() const {
  std::ostringstream oss;
  oss << '[';

  for (size_t i = 0; i < m_elements.size(); ++i) {
    if (i > 0) {
      oss << ',';
    }
    oss << m_elements[i]->toString();
  }

  oss << ']';
  return oss.str();
}

// ============================================================================
// JsonObject Implementation
// ============================================================================

JsonObject::~JsonObject() {
  for (std::map<std::string, JsonValue*>::iterator it = m_members.begin();
       it != m_members.end(); ++it) {
    delete it->second;
  }
}

JsonValue* JsonObject::clone() const {
  JsonObject* newObject = new JsonObject();
  for (std::map<std::string, JsonValue*>::const_iterator it = m_members.begin();
       it != m_members.end(); ++it) {
    newObject->insert(it->first, it->second->clone());
  }
  return newObject;
}

std::string JsonObject::toString() const {
  std::ostringstream oss;
  oss << '{';

  bool first = true;
  for (std::map<std::string, JsonValue*>::const_iterator it = m_members.begin();
       it != m_members.end(); ++it) {
    if (!first) {
      oss << ',';
    }
    first = false;

    JsonString keyStr(it->first);
    oss << keyStr.toString() << ':' << it->second->toString();
  }

  oss << '}';
  return oss.str();
}

// ============================================================================
// JsonParser Implementation
// ============================================================================

JsonValue* JsonParser::parse(const std::string& input) {
  m_input = input;
  m_position = 0;

  skipWhitespace();

  if (m_position >= m_input.length()) {
    throwError("Unexpected end of input");
  }

  JsonValue* result = parseValue();

  skipWhitespace();
  if (m_position < m_input.length()) {
    delete result;
    throwError("Unexpected characters after JSON value");
  }

  return result;
}

void JsonParser::skipWhitespace() {
  while (m_position < m_input.length() &&
         (m_input[m_position] == ' ' || m_input[m_position] == '\t' ||
          m_input[m_position] == '\n' || m_input[m_position] == '\r')) {
    ++m_position;
  }
}

char JsonParser::peek() const {
  if (m_position >= m_input.length()) {
    return '\0';
  }
  return m_input[m_position];
}

char JsonParser::consume() {
  if (m_position >= m_input.length()) {
    return '\0';
  }
  return m_input[m_position++];
}

bool JsonParser::consumeString(const std::string& str) {
  if (m_position + str.length() > m_input.length()) {
    return false;
  }

  for (size_t i = 0; i < str.length(); ++i) {
    if (m_input[m_position + i] != str[i]) {
      return false;
    }
  }

  m_position += str.length();
  return true;
}

JsonValue* JsonParser::parseValue() {
  skipWhitespace();

  char c = peek();

  switch (c) {
    case 'n':
      return parseNull();
    case 't':
    case 'f':
      return parseBoolean();
    case '"':
      return parseString();
    case '[':
      return parseArray();
    case '{':
      return parseObject();
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
      return parseNumber();
    default:
      throwError("Unexpected character in JSON: " + std::string(1, c));
      return NULL;  // Never reached, but silences compiler warnings
  }
}

JsonNull* JsonParser::parseNull() {
  if (consumeString("null")) {
    return new JsonNull();
  }
  throwError("Expected 'null'");
  return NULL;  // Never reached
}

JsonBoolean* JsonParser::parseBoolean() {
  if (consumeString("true")) {
    return new JsonBoolean(true);
  } else if (consumeString("false")) {
    return new JsonBoolean(false);
  }
  throwError("Expected 'true' or 'false'");
  return NULL;  // Never reached
}

JsonNumber* JsonParser::parseNumber() {
  size_t start = m_position;

  // Optional minus sign
  if (peek() == '-') {
    consume();
  }

  // Integer part
  if (peek() == '0') {
    consume();
  } else if (std::isdigit(peek())) {
    while (std::isdigit(peek())) {
      consume();
    }
  } else {
    throwError("Invalid number format");
  }

  // Optional fractional part
  if (peek() == '.') {
    consume();
    if (!std::isdigit(peek())) {
      throwError("Invalid number format: digit expected after decimal point");
    }
    while (std::isdigit(peek())) {
      consume();
    }
  }

  // Optional exponent part
  if (peek() == 'e' || peek() == 'E') {
    consume();
    if (peek() == '+' || peek() == '-') {
      consume();
    }
    if (!std::isdigit(peek())) {
      throwError("Invalid number format: digit expected in exponent");
    }
    while (std::isdigit(peek())) {
      consume();
    }
  }

  std::string numStr = m_input.substr(start, m_position - start);
  char* endPtr;
  double value = std::strtod(numStr.c_str(), &endPtr);

  if (endPtr != numStr.c_str() + numStr.length()) {
    throwError("Invalid number format");
  }

  return new JsonNumber(value);
}

JsonString* JsonParser::parseString() {
  if (consume() != '"') {
    throwError("Expected '\"' at start of string");
  }

  std::string str = parseStringLiteral();

  if (consume() != '"') {
    throwError("Expected '\"' at end of string");
  }

  return new JsonString(unescapeString(str));
}

JsonArray* JsonParser::parseArray() {
  if (consume() != '[') {
    throwError("Expected '[' at start of array");
  }

  JsonArray* array = new JsonArray();

  skipWhitespace();

  // Handle empty array
  if (peek() == ']') {
    consume();
    return array;
  }

  try {
    // Parse first element
    array->push_back(parseValue());

    // Parse remaining elements
    while (true) {
      skipWhitespace();

      if (peek() == ']') {
        consume();
        break;
      } else if (peek() == ',') {
        consume();
        array->push_back(parseValue());
      } else {
        throwError("Expected ',' or ']' in array");
      }
    }
  } catch (...) {
    delete array;
    throw;
  }

  return array;
}

JsonObject* JsonParser::parseObject() {
  if (consume() != '{') {
    throwError("Expected '{' at start of object");
  }

  JsonObject* object = new JsonObject();

  skipWhitespace();

  // Handle empty object
  if (peek() == '}') {
    consume();
    return object;
  }

  try {
    // Parse first key-value pair
    skipWhitespace();
    JsonString* key = parseString();
    std::string keyStr = key->getValue();
    delete key;

    skipWhitespace();
    if (consume() != ':') {
      throwError("Expected ':' after object key");
    }

    JsonValue* value = parseValue();
    object->insert(keyStr, value);

    // Parse remaining key-value pairs
    while (true) {
      skipWhitespace();

      if (peek() == '}') {
        consume();
        break;
      } else if (peek() == ',') {
        consume();

        skipWhitespace();
        JsonString* nextKey = parseString();
        std::string nextKeyStr = nextKey->getValue();
        delete nextKey;

        skipWhitespace();
        if (consume() != ':') {
          throwError("Expected ':' after object key");
        }

        JsonValue* nextValue = parseValue();
        object->insert(nextKeyStr, nextValue);
      } else {
        throwError("Expected ',' or '}' in object");
      }
    }
  } catch (...) {
    delete object;
    throw;
  }

  return object;
}

std::string JsonParser::parseStringLiteral() {
  std::string result;

  while (peek() != '"' && peek() != '\0') {
    if (peek() == '\\') {
      consume();  // consume backslash
      char escaped = consume();
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result += escaped;
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
        case 'u':
          // Unicode escape sequence (simplified - just store as-is for this
          // example)
          result += "\\u";
          for (int i = 0; i < 4; ++i) {
            char hex = consume();
            if (!std::isxdigit(hex)) {
              throwError("Invalid unicode escape sequence");
            }
            result += hex;
          }
          break;
        default:
          throwError("Invalid escape sequence: \\" + std::string(1, escaped));
      }
    } else {
      result += consume();
    }
  }

  return result;
}

std::string JsonParser::unescapeString(const std::string& str) {
  // For this simplified implementation, we'll just return the string as-is
  // since we handle escaping during parsing
  return str;
}

void JsonParser::throwError(const std::string& message) const {
  std::ostringstream oss;
  oss << "JSON Parse Error at position " << m_position << ": " << message;
  throw std::runtime_error(oss.str());
}

}  // namespace selfserv
