#pragma once

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace selfserv {

// Forward declarations
class JsonValue;
class JsonNull;
class JsonBoolean;
class JsonNumber;
class JsonString;
class JsonArray;
class JsonObject;

/**
 * @brief Enumeration of JSON value types
 */
enum JsonType {
  JSON_NULL,
  JSON_BOOLEAN,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
};

/**
 * @brief Abstract base class for all JSON values
 *
 * Uses polymorphic inheritance to represent different JSON types.
 * Memory management is explicit - callers are responsible for cleanup.
 */
class JsonValue {
 public:
  virtual ~JsonValue() {}
  virtual JsonType getType() const = 0;
  virtual JsonValue* clone() const = 0;
  virtual std::string toString() const = 0;

  // Type checking methods
  bool isNull() const { return getType() == JSON_NULL; }
  bool isBoolean() const { return getType() == JSON_BOOLEAN; }
  bool isNumber() const { return getType() == JSON_NUMBER; }
  bool isString() const { return getType() == JSON_STRING; }
  bool isArray() const { return getType() == JSON_ARRAY; }
  bool isObject() const { return getType() == JSON_OBJECT; }

  // Safe casting methods - return NULL if type doesn't match
  JsonNull* asNull();
  JsonBoolean* asBoolean();
  JsonNumber* asNumber();
  JsonString* asString();
  JsonArray* asArray();
  JsonObject* asObject();

  const JsonNull* asNull() const;
  const JsonBoolean* asBoolean() const;
  const JsonNumber* asNumber() const;
  const JsonString* asString() const;
  const JsonArray* asArray() const;
  const JsonObject* asObject() const;
};

/**
 * @brief Represents a JSON null value
 */
class JsonNull : public JsonValue {
 public:
  JsonNull() {}
  virtual ~JsonNull() {}

  virtual JsonType getType() const { return JSON_NULL; }
  virtual JsonValue* clone() const { return new JsonNull(); }
  virtual std::string toString() const { return "null"; }
};

/**
 * @brief Represents a JSON boolean value
 */
class JsonBoolean : public JsonValue {
 private:
  bool m_value;

 public:
  explicit JsonBoolean(bool value) : m_value(value) {}
  virtual ~JsonBoolean() {}

  virtual JsonType getType() const { return JSON_BOOLEAN; }
  virtual JsonValue* clone() const { return new JsonBoolean(m_value); }
  virtual std::string toString() const { return m_value ? "true" : "false"; }

  bool getValue() const { return m_value; }
  void setValue(bool value) { m_value = value; }
};

/**
 * @brief Represents a JSON number value
 */
class JsonNumber : public JsonValue {
 private:
  double m_value;

 public:
  explicit JsonNumber(double value) : m_value(value) {}
  virtual ~JsonNumber() {}

  virtual JsonType getType() const { return JSON_NUMBER; }
  virtual JsonValue* clone() const { return new JsonNumber(m_value); }
  virtual std::string toString() const;

  double getValue() const { return m_value; }
  void setValue(double value) { m_value = value; }
};

/**
 * @brief Represents a JSON string value
 */
class JsonString : public JsonValue {
 private:
  std::string m_value;

 public:
  explicit JsonString(const std::string& value) : m_value(value) {}
  virtual ~JsonString() {}

  virtual JsonType getType() const { return JSON_STRING; }
  virtual JsonValue* clone() const { return new JsonString(m_value); }
  virtual std::string toString() const;

  const std::string& getValue() const { return m_value; }
  void setValue(const std::string& value) { m_value = value; }
};

/**
 * @brief Represents a JSON array value
 */
class JsonArray : public JsonValue {
 private:
  std::vector<JsonValue*> m_elements;

 public:
  JsonArray() {}
  virtual ~JsonArray();

  virtual JsonType getType() const { return JSON_ARRAY; }
  virtual JsonValue* clone() const;
  virtual std::string toString() const;

  // Array operations
  size_t size() const { return m_elements.size(); }
  bool empty() const { return m_elements.empty(); }

  void push_back(JsonValue* value) { m_elements.push_back(value); }

  JsonValue* at(size_t index) const {
    if (index >= m_elements.size()) {
      throw std::out_of_range("Array index out of range");
    }
    return m_elements[index];
  }

  JsonValue* operator[](size_t index) const { return at(index); }

  // Iterator support for C++98
  typedef std::vector<JsonValue*>::iterator iterator;
  typedef std::vector<JsonValue*>::const_iterator const_iterator;

  iterator begin() { return m_elements.begin(); }
  iterator end() { return m_elements.end(); }
  const_iterator begin() const { return m_elements.begin(); }
  const_iterator end() const { return m_elements.end(); }
};

/**
 * @brief Represents a JSON object value
 */
class JsonObject : public JsonValue {
 private:
  std::map<std::string, JsonValue*> m_members;

 public:
  JsonObject() {}
  virtual ~JsonObject();

  virtual JsonType getType() const { return JSON_OBJECT; }
  virtual JsonValue* clone() const;
  virtual std::string toString() const;

  // Object operations
  size_t size() const { return m_members.size(); }
  bool empty() const { return m_members.empty(); }

  void insert(const std::string& key, JsonValue* value) {
    // Delete existing value if present
    std::map<std::string, JsonValue*>::iterator it = m_members.find(key);
    if (it != m_members.end()) {
      delete it->second;
    }
    m_members[key] = value;
  }

  JsonValue* at(const std::string& key) const {
    std::map<std::string, JsonValue*>::const_iterator it = m_members.find(key);
    if (it == m_members.end()) {
      throw std::out_of_range("Object key not found: " + key);
    }
    return it->second;
  }

  JsonValue* operator[](const std::string& key) const { return at(key); }

  bool hasKey(const std::string& key) const {
    return m_members.find(key) != m_members.end();
  }

  // Iterator support for C++98
  typedef std::map<std::string, JsonValue*>::iterator iterator;
  typedef std::map<std::string, JsonValue*>::const_iterator const_iterator;

  iterator begin() { return m_members.begin(); }
  iterator end() { return m_members.end(); }
  const_iterator begin() const { return m_members.begin(); }
  const_iterator end() const { return m_members.end(); }
};

/**
 * @brief JSON parser class
 *
 * Parses JSON strings into a tree of JsonValue objects.
 * The caller is responsible for deleting the returned JsonValue.
 */
class JsonParser {
 private:
  std::string m_input;
  size_t m_position;

  // Parser helper methods
  void skipWhitespace();
  char peek() const;
  char consume();
  bool consumeString(const std::string& str);

  JsonValue* parseValue();
  JsonNull* parseNull();
  JsonBoolean* parseBoolean();
  JsonNumber* parseNumber();
  JsonString* parseString();
  JsonArray* parseArray();
  JsonObject* parseObject();

  std::string parseStringLiteral();
  std::string unescapeString(const std::string& str);

  void throwError(const std::string& message) const;

 public:
  JsonParser() : m_position(0) {}
  ~JsonParser() {}

  /**
   * @brief Parse a JSON string
   * @param input The JSON string to parse
   * @return Pointer to the parsed JsonValue (caller owns the memory)
   * @throws std::runtime_error if the JSON is malformed
   */
  JsonValue* parse(const std::string& input);
};

/**
 * @brief RAII wrapper for JsonValue to ensure automatic cleanup
 *
 * This provides a simple smart pointer-like interface for C++98
 */
class JsonValuePtr {
 private:
  JsonValue* m_ptr;

  // Disable copy constructor and assignment operator
  JsonValuePtr(const JsonValuePtr&);
  JsonValuePtr& operator=(const JsonValuePtr&);

 public:
  explicit JsonValuePtr(JsonValue* ptr = NULL) : m_ptr(ptr) {}

  ~JsonValuePtr() { delete m_ptr; }

  JsonValue* get() const { return m_ptr; }
  JsonValue* operator->() const { return m_ptr; }
  JsonValue& operator*() const { return *m_ptr; }

  void reset(JsonValue* ptr = NULL) {
    delete m_ptr;
    m_ptr = ptr;
  }

  JsonValue* release() {
    JsonValue* tmp = m_ptr;
    m_ptr = NULL;
    return tmp;
  }

  bool isNull() const { return m_ptr == NULL; }
};

}  // namespace selfserv
