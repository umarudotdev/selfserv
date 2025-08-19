#pragma once

#include <map>
#include <string>
#include <vector>

/**
 * @brief JSON value types enumeration
 */
enum JsonType {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
};

/**
 * @brief Base class for all JSON values using polymorphic design
 *
 * Since C++98 doesn't have std::variant, we use a traditional polymorphic
 * class hierarchy with a base class and derived classes for each JSON type.
 * Memory management is explicit with manual new/delete.
 */
class JsonValue {
 public:
  /**
   * @brief Constructor
   * @param type The JSON type of this value
   */
  explicit JsonValue(JsonType type);

  /**
   * @brief Virtual destructor for proper cleanup of derived classes
   */
  virtual ~JsonValue();

  /**
   * @brief Get the type of this JSON value
   * @return The JsonType enum value
   */
  JsonType GetType() const;

  /**
   * @brief Clone this value (deep copy)
   * @return A new JsonValue instance (caller owns the memory)
   */
  virtual JsonValue* Clone() const = 0;

  /**
   * @brief Convert to string representation
   * @return String representation of the JSON value
   */
  virtual std::string ToString() const = 0;

 private:
  JsonType m_type;

  // Disable copy constructor and assignment operator
  JsonValue(const JsonValue&);
  JsonValue& operator=(const JsonValue&);
};

/**
 * @brief JSON null value
 */
class JsonNull : public JsonValue {
 public:
  JsonNull();
  virtual ~JsonNull();
  virtual JsonValue* Clone() const;
  virtual std::string ToString() const;
};

/**
 * @brief JSON boolean value
 */
class JsonBool : public JsonValue {
 public:
  explicit JsonBool(bool value);
  virtual ~JsonBool();
  virtual JsonValue* Clone() const;
  virtual std::string ToString() const;

  bool GetValue() const;

 private:
  bool m_value;
};

/**
 * @brief JSON number value (stored as double for simplicity)
 */
class JsonNumber : public JsonValue {
 public:
  explicit JsonNumber(double value);
  virtual ~JsonNumber();
  virtual JsonValue* Clone() const;
  virtual std::string ToString() const;

  double GetValue() const;

 private:
  double m_value;
};

/**
 * @brief JSON string value
 */
class JsonString : public JsonValue {
 public:
  explicit JsonString(const std::string& value);
  virtual ~JsonString();
  virtual JsonValue* Clone() const;
  virtual std::string ToString() const;

  const std::string& GetValue() const;

 private:
  std::string m_value;
};

/**
 * @brief JSON array value
 */
class JsonArray : public JsonValue {
 public:
  JsonArray();
  virtual ~JsonArray();
  virtual JsonValue* Clone() const;
  virtual std::string ToString() const;

  /**
   * @brief Add a value to the array
   * @param value The value to add (JsonArray takes ownership)
   */
  void AddValue(JsonValue* value);

  /**
   * @brief Get the size of the array
   * @return Number of elements in the array
   */
  size_t GetSize() const;

  /**
   * @brief Get a value at the specified index
   * @param index The index to access
   * @return Pointer to the JsonValue (JsonArray retains ownership)
   * @throws std::out_of_range if index is invalid
   */
  const JsonValue* GetValue(size_t index) const;

 private:
  std::vector<JsonValue*> m_values;
};

/**
 * @brief JSON object value
 */
class JsonObject : public JsonValue {
 public:
  JsonObject();
  virtual ~JsonObject();
  virtual JsonValue* Clone() const;
  virtual std::string ToString() const;

  /**
   * @brief Set a key-value pair in the object
   * @param key The key name
   * @param value The value (JsonObject takes ownership)
   */
  void SetValue(const std::string& key, JsonValue* value);

  /**
   * @brief Get a value by key
   * @param key The key to look up
   * @return Pointer to the JsonValue or NULL if key doesn't exist
   */
  const JsonValue* GetValue(const std::string& key) const;

  /**
   * @brief Check if a key exists
   * @param key The key to check
   * @return true if key exists, false otherwise
   */
  bool HasKey(const std::string& key) const;

  /**
   * @brief Get all keys in the object
   * @return Vector of all key names
   */
  std::vector<std::string> GetKeys() const;

 private:
  std::map<std::string, JsonValue*> m_values;
};

/**
 * @brief JSON parser class
 *
 * Parses JSON strings into a tree of JsonValue objects.
 * Uses recursive descent parsing with proper error handling.
 */
class JsonParser {
 public:
  /**
   * @brief Constructor
   */
  JsonParser();

  /**
   * @brief Destructor
   */
  ~JsonParser();

  /**
   * @brief Parse a JSON string
   * @param json The JSON string to parse
   * @return Pointer to the root JsonValue (caller owns the memory)
   * @throws std::runtime_error on parse errors
   */
  JsonValue* Parse(const std::string& json);

 private:
  const char* m_start;
  const char* m_current;
  const char* m_end;

  // Parsing helper methods
  JsonValue* ParseValue();
  JsonNull* ParseNull();
  JsonBool* ParseBool();
  JsonNumber* ParseNumber();
  JsonString* ParseString();
  JsonArray* ParseArray();
  JsonObject* ParseObject();

  // Utility methods
  void SkipWhitespace();
  char PeekChar() const;
  char NextChar();
  void ExpectChar(char expected);
  bool IsAtEnd() const;
  std::string ParseStringLiteral();
  double ParseNumberLiteral();

  // Error reporting
  void ThrowError(const std::string& message) const;

  // Disable copy constructor and assignment operator
  JsonParser(const JsonParser&);
  JsonParser& operator=(const JsonParser&);
};

/**
 * @brief RAII helper for automatic JsonValue cleanup
 *
 * Since C++98 doesn't have smart pointers, this provides
 * a simple RAII wrapper for JsonValue objects.
 */
class JsonValuePtr {
 public:
  /**
   * @brief Constructor
   * @param value The JsonValue to manage (takes ownership)
   */
  explicit JsonValuePtr(JsonValue* value);

  /**
   * @brief Destructor - automatically cleans up the JsonValue
   */
  ~JsonValuePtr();

  /**
   * @brief Get the underlying pointer
   * @return Pointer to the JsonValue
   */
  JsonValue* Get() const;

  /**
   * @brief Release ownership of the JsonValue
   * @return Pointer to the JsonValue (caller now owns it)
   */
  JsonValue* Release();

  /**
   * @brief Reset with a new JsonValue
   * @param value New JsonValue to manage (takes ownership)
   */
  void Reset(JsonValue* value);

 private:
  JsonValue* m_value;

  // Disable copy constructor and assignment operator
  JsonValuePtr(const JsonValuePtr&);
  JsonValuePtr& operator=(const JsonValuePtr&);
};
