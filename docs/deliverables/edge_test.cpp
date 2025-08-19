#include <iostream>

#include "json_parser.hpp"

using namespace selfserv;

int main() {
  JsonParser parser;

  // Test empty structures
  try {
    JsonValuePtr empty_obj(parser.parse("{}"));
    JsonValuePtr empty_arr(parser.parse("[]"));
    std::cout << "✓ Empty structures: " << empty_obj->toString() << " "
              << empty_arr->toString() << std::endl;
  } catch (const std::exception& e) {
    std::cout << "✗ Empty structures failed: " << e.what() << std::endl;
  }

  // Test string escaping
  try {
    JsonValuePtr escaped(parser.parse("{\"test\": \"line1\\nline2\\ttab\"}"));
    std::cout << "✓ String escaping works" << std::endl;
  } catch (const std::exception& e) {
    std::cout << "✗ String escaping failed: " << e.what() << std::endl;
  }

  // Test numbers
  try {
    JsonValuePtr numbers(parser.parse("[0, -1, 1.5, -2.7e10, 3E-5]"));
    std::cout << "✓ Number parsing: " << numbers->toString() << std::endl;
  } catch (const std::exception& e) {
    std::cout << "✗ Number parsing failed: " << e.what() << std::endl;
  }

  return 0;
}
