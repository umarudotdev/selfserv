/**
 * Part 3: Example Usage and Explanation (main.cpp)
 *
 * Comprehensive demonstration of the JSON parser including:
 * - Parsing complex, valid JSON
 * - Accessing nested data
 * - Memory management examples
 * - Error handling with invalid JSON
 */

#include <cassert>
#include <iostream>

#include "json_parser.hpp"

using namespace selfserv;

/**
 * @brief Demonstration of JSON parser usage and error handling
 */
int main() {
  std::cout << "=== JSON Parser C++98 Demo ===" << std::endl;

  // Example 1: Parse a complex, valid JSON string
  std::cout << "\n1. Parsing complex JSON..." << std::endl;

  std::string complexJson =
      "{\n"
      "  \"name\": \"WebServer Config\",\n"
      "  \"version\": 1.2,\n"
      "  \"active\": true,\n"
      "  \"servers\": [\n"
      "    {\n"
      "      \"host\": \"localhost\",\n"
      "      \"port\": 8080,\n"
      "      \"ssl\": false\n"
      "    },\n"
      "    {\n"
      "      \"host\": \"example.com\",\n"
      "      \"port\": 443,\n"
      "      \"ssl\": true\n"
      "    }\n"
      "  ],\n"
      "  \"config\": {\n"
      "    \"timeout\": 30.5,\n"
      "    \"max_connections\": 1000,\n"
      "    \"debug\": null\n"
      "  }\n"
      "}";

  try {
    JsonParser parser;
    JsonValuePtr root(parser.parse(complexJson));

    std::cout << "✓ Successfully parsed JSON!" << std::endl;

    // Access nested data within the parsed structure
    std::cout << "\n2. Accessing nested data..." << std::endl;

    JsonObject* rootObj = root->asObject();
    if (rootObj) {
      // Access simple values
      JsonString* name = rootObj->at("name")->asString();
      if (name) {
        std::cout << "Name: " << name->getValue() << std::endl;
      }

      JsonNumber* version = rootObj->at("version")->asNumber();
      if (version) {
        std::cout << "Version: " << version->getValue() << std::endl;
      }

      JsonBoolean* active = rootObj->at("active")->asBoolean();
      if (active) {
        std::cout << "Active: " << (active->getValue() ? "true" : "false")
                  << std::endl;
      }

      // Access array
      JsonArray* servers = rootObj->at("servers")->asArray();
      if (servers) {
        std::cout << "Servers (" << servers->size() << " total):" << std::endl;

        for (size_t i = 0; i < servers->size(); ++i) {
          JsonObject* server = servers->at(i)->asObject();
          if (server) {
            JsonString* host = server->at("host")->asString();
            JsonNumber* port = server->at("port")->asNumber();
            JsonBoolean* ssl = server->at("ssl")->asBoolean();

            std::cout << "  Server " << i << ": " << host->getValue() << ":"
                      << static_cast<int>(port->getValue())
                      << " (SSL: " << (ssl->getValue() ? "yes" : "no") << ")"
                      << std::endl;
          }
        }
      }

      // Access nested object
      JsonObject* config = rootObj->at("config")->asObject();
      if (config) {
        std::cout << "Config:" << std::endl;

        JsonNumber* timeout = config->at("timeout")->asNumber();
        if (timeout) {
          std::cout << "  Timeout: " << timeout->getValue() << "s" << std::endl;
        }

        JsonNumber* maxConn = config->at("max_connections")->asNumber();
        if (maxConn) {
          std::cout << "  Max connections: "
                    << static_cast<int>(maxConn->getValue()) << std::endl;
        }

        JsonValue* debug = config->at("debug");
        if (debug && debug->isNull()) {
          std::cout << "  Debug: null" << std::endl;
        }
      }
    }

    // Demonstrate JSON reconstruction
    std::cout << "\n3. Reconstructed JSON:" << std::endl;
    std::cout << root->toString() << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "✗ Error parsing JSON: " << e.what() << std::endl;
    return 1;
  }

  // Example 2: Error handling with invalid JSON
  std::cout << "\n4. Testing error handling..." << std::endl;

  std::string invalidJsons[] = {
      "{\"key\": }",                   // Missing value
      "{\"key\": \"unclosed string}",  // Unclosed string
      "{\"key\": value}",              // Unquoted value
      "[1, 2, 3,]",                    // Trailing comma
      "{key: \"value\"}",              // Unquoted key
      "{\"a\": 1 \"b\": 2}",           // Missing comma
      ""                               // Empty input
  };

  for (size_t i = 0; i < sizeof(invalidJsons) / sizeof(invalidJsons[0]); ++i) {
    try {
      JsonParser parser;
      JsonValuePtr result(parser.parse(invalidJsons[i]));
      std::cout << "✗ Should have failed for: " << invalidJsons[i] << std::endl;
    } catch (const std::runtime_error& e) {
      std::cout << "✓ Correctly caught error for \"" << invalidJsons[i]
                << "\": " << e.what() << std::endl;
    }
  }

  // Example 3: Demonstrate manual memory management (alternative to
  // JsonValuePtr)
  std::cout << "\n5. Manual memory management example..." << std::endl;

  try {
    JsonParser parser;
    JsonValue* manualRoot = parser.parse("{\"test\": [1, 2, 3]}");

    std::cout << "Manual JSON: " << manualRoot->toString() << std::endl;

    // Manually clean up - very important!
    delete manualRoot;
    std::cout << "✓ Memory cleaned up manually" << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "✗ Error in manual example: " << e.what() << std::endl;
  }

  // Example 4: Demonstrate cloning
  std::cout << "\n6. Cloning example..." << std::endl;

  try {
    JsonParser parser;
    JsonValuePtr original(
        parser.parse("{\"cloned\": true, \"data\": [1, 2, 3]}"));
    JsonValuePtr clone(original->clone());

    std::cout << "Original: " << original->toString() << std::endl;
    std::cout << "Clone: " << clone->toString() << std::endl;
    std::cout << "✓ Cloning successful" << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "✗ Error in cloning example: " << e.what() << std::endl;
  }

  std::cout << "\n=== Demo Complete ===" << std::endl;

  return 0;
}

/*
 * Design Choices and Explanation:
 *
 * 1. POLYMORPHIC CLASS HIERARCHY:
 *    I chose a polymorphic inheritance approach with a base JsonValue class
 *    and derived classes for each JSON type. This provides type safety and
 *    clean object-oriented design while remaining C++98 compatible.
 *    Alternative: Tagged union would be more memory efficient but less
 *    type-safe and harder to extend.
 *
 * 2. MEMORY MANAGEMENT:
 *    - Manual memory management using new/delete since smart pointers
 *      aren't available in C++98
 *    - Provided JsonValuePtr as a simple RAII wrapper to automate cleanup
 *    - Clear ownership semantics: caller owns the JsonValue* returned
 *      by parser.parse()
 *    - Exception safety: destructors clean up allocated memory, and
 *      parser methods use try/catch to clean up on parse errors
 *
 * 3. ERROR HANDLING:
 *    - Uses exceptions (std::runtime_error) for parse errors since they
 *      provide clear error propagation and automatic cleanup
 *    - Detailed error messages include position information for debugging
 *    - Graceful handling of malformed JSON with proper cleanup
 *
 * 4. DATA REPRESENTATION:
 *    - std::map for JSON objects (sorted keys, logarithmic access)
 *    - std::vector for JSON arrays (contiguous memory, constant access)
 *    - double for all numbers (simplified, but covers most use cases)
 *    - Safe casting methods to prevent invalid type access
 *
 * 5. PARSER DESIGN:
 *    - Recursive descent parser that's easy to understand and maintain
 *    - Re-entrant design allows for nested structures
 *    - Proper whitespace handling throughout parsing
 *    - Support for escape sequences in strings
 *
 * 6. C++98 COMPLIANCE:
 *    - No auto, smart pointers, range-based for, or other modern features
 *    - Uses traditional iterator patterns for container access
 *    - Explicit constructors to prevent implicit conversions
 *    - Manual RAII patterns for resource management
 *
 * TRADE-OFFS:
 * - Memory overhead: Polymorphic classes have vtable overhead
 * - Performance: Dynamic allocation for each JSON value vs. more complex
 *   but efficient tagged union approach
 * - Safety: Manual memory management requires careful ownership tracking
 * - Simplicity: Clear, extensible design at the cost of some efficiency
 *
 * This design prioritizes correctness, clarity, and maintainability while
 * working within C++98 constraints. For a production system, consider
 * adding move semantics (when upgrading to C++11+) and memory pooling
 * for better performance.
 */
