#include "json_parser.hpp"
#include <iostream>
#include <exception>

/**
 * @brief Helper function to safely cast JsonValue to specific type
 */
template<typename T>
const T* SafeCast(const JsonValue* value, JsonType expectedType) {
    if (!value || value->GetType() != expectedType) {
        return NULL;
    }
    return static_cast<const T*>(value);
}

/**
 * @brief Demonstrate parsing and accessing nested JSON data
 */
void DemonstrateValidJson() {
    std::cout << "=== Demonstrating Valid JSON Parsing ===" << std::endl;
    
    // Complex JSON with nested objects and arrays
    std::string jsonStr = 
        "{"
        "  \"name\": \"John Doe\","
        "  \"age\": 30,"
        "  \"isActive\": true,"
        "  \"address\": {"
        "    \"street\": \"123 Main St\","
        "    \"city\": \"New York\","
        "    \"zipCode\": 10001"
        "  },"
        "  \"hobbies\": [\"reading\", \"coding\", \"hiking\"],"
        "  \"spouse\": null"
        "}";
    
    JsonParser parser;
    JsonValuePtr rootPtr(NULL);
    
    try {
        // Parse the JSON string
        JsonValue* root = parser.Parse(jsonStr);
        rootPtr.Reset(root);
        
        std::cout << "Successfully parsed JSON!" << std::endl;
        std::cout << "Root JSON: " << root->ToString() << std::endl << std::endl;
        
        // Cast to object to access nested data
        const JsonObject* rootObj = SafeCast<JsonObject>(root, JSON_OBJECT);
        if (!rootObj) {
            std::cout << "Error: Root is not a JSON object" << std::endl;
            return;
        }
        
        // Access string value
        const JsonValue* nameValue = rootObj->GetValue("name");
        const JsonString* nameStr = SafeCast<JsonString>(nameValue, JSON_STRING);
        if (nameStr) {
            std::cout << "Name: " << nameStr->GetValue() << std::endl;
        }
        
        // Access number value
        const JsonValue* ageValue = rootObj->GetValue("age");
        const JsonNumber* ageNum = SafeCast<JsonNumber>(ageValue, JSON_NUMBER);
        if (ageNum) {
            std::cout << "Age: " << ageNum->GetValue() << std::endl;
        }
        
        // Access boolean value
        const JsonValue* isActiveValue = rootObj->GetValue("isActive");
        const JsonBool* isActiveBool = SafeCast<JsonBool>(isActiveValue, JSON_BOOL);
        if (isActiveBool) {
            std::cout << "Is Active: " << (isActiveBool->GetValue() ? "true" : "false") << std::endl;
        }
        
        // Access nested object
        const JsonValue* addressValue = rootObj->GetValue("address");
        const JsonObject* addressObj = SafeCast<JsonObject>(addressValue, JSON_OBJECT);
        if (addressObj) {
            std::cout << "\nAddress Information:" << std::endl;
            
            const JsonValue* streetValue = addressObj->GetValue("street");
            const JsonString* streetStr = SafeCast<JsonString>(streetValue, JSON_STRING);
            if (streetStr) {
                std::cout << "  Street: " << streetStr->GetValue() << std::endl;
            }
            
            const JsonValue* cityValue = addressObj->GetValue("city");
            const JsonString* cityStr = SafeCast<JsonString>(cityValue, JSON_STRING);
            if (cityStr) {
                std::cout << "  City: " << cityStr->GetValue() << std::endl;
            }
            
            const JsonValue* zipValue = addressObj->GetValue("zipCode");
            const JsonNumber* zipNum = SafeCast<JsonNumber>(zipValue, JSON_NUMBER);
            if (zipNum) {
                std::cout << "  Zip Code: " << static_cast<int>(zipNum->GetValue()) << std::endl;
            }
        }
        
        // Access array
        const JsonValue* hobbiesValue = rootObj->GetValue("hobbies");
        const JsonArray* hobbiesArray = SafeCast<JsonArray>(hobbiesValue, JSON_ARRAY);
        if (hobbiesArray) {
            std::cout << "\nHobbies:" << std::endl;
            for (size_t i = 0; i < hobbiesArray->GetSize(); ++i) {
                const JsonValue* hobbyValue = hobbiesArray->GetValue(i);
                const JsonString* hobbyStr = SafeCast<JsonString>(hobbyValue, JSON_STRING);
                if (hobbyStr) {
                    std::cout << "  " << (i + 1) << ". " << hobbyStr->GetValue() << std::endl;
                }
            }
        }
        
        // Access null value
        const JsonValue* spouseValue = rootObj->GetValue("spouse");
        if (spouseValue && spouseValue->GetType() == JSON_NULL) {
            std::cout << "\nSpouse: null (not married)" << std::endl;
        }
        
        std::cout << "\n=== Memory will be automatically cleaned up by JsonValuePtr ===" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "Unexpected error: " << e.what() << std::endl;
    }
}

/**
 * @brief Demonstrate error handling with invalid JSON
 */
void DemonstrateInvalidJson() {
    std::cout << "\n\n=== Demonstrating Invalid JSON Error Handling ===" << std::endl;
    
    // Array of invalid JSON strings to test
    std::string invalidJsonStrings[] = {
        "{ \"name\": \"John\", }",           // Trailing comma
        "{ \"name\" \"John\" }",             // Missing colon
        "{ \"name\": John }",                // Unquoted string value
        "{ \"name\": \"John\"",              // Missing closing brace
        "[ 1, 2, 3, ]",                     // Trailing comma in array
        "{ \"number\": 123abc }",            // Invalid number format
        "{ \"string\": \"unclosed string }",// Unclosed string
        "",                                  // Empty string
        "null null"                          // Multiple values
    };
    
    size_t numTests = sizeof(invalidJsonStrings) / sizeof(invalidJsonStrings[0]);
    
    JsonParser parser;
    
    for (size_t i = 0; i < numTests; ++i) {
        std::cout << "\nTest " << (i + 1) << ": \"" << invalidJsonStrings[i] << "\"" << std::endl;
        
        try {
            JsonValue* result = parser.Parse(invalidJsonStrings[i]);
            // If we get here, the parsing unexpectedly succeeded
            std::cout << "  UNEXPECTED: Parsing succeeded when it should have failed!" << std::endl;
            delete result; // Clean up
        } catch (const std::runtime_error& e) {
            std::cout << "  EXPECTED: Caught parse error: " << e.what() << std::endl;
        } catch (const std::exception& e) {
            std::cout << "  UNEXPECTED: Caught unexpected exception: " << e.what() << std::endl;
        }
    }
}

/**
 * @brief Demonstrate manual memory management vs RAII
 */
void DemonstrateMemoryManagement() {
    std::cout << "\n\n=== Demonstrating Memory Management ===" << std::endl;
    
    std::string jsonStr = "{ \"test\": [1, 2, 3] }";
    JsonParser parser;
    
    // Method 1: Manual memory management
    std::cout << "\nMethod 1: Manual memory management" << std::endl;
    try {
        JsonValue* root = parser.Parse(jsonStr);
        std::cout << "Parsed: " << root->ToString() << std::endl;
        std::cout << "Manually cleaning up memory..." << std::endl;
        delete root; // Must remember to delete!
        std::cout << "Memory cleaned up successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
    }
    
    // Method 2: RAII with JsonValuePtr
    std::cout << "\nMethod 2: RAII with JsonValuePtr (recommended)" << std::endl;
    try {
        JsonValuePtr rootPtr(parser.Parse(jsonStr));
        std::cout << "Parsed: " << rootPtr.Get()->ToString() << std::endl;
        std::cout << "Memory will be automatically cleaned up when rootPtr goes out of scope" << std::endl;
        // No explicit delete needed!
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
    }
    
    std::cout << "RAII cleanup completed automatically" << std::endl;
}

/**
 * @brief Main function demonstrating the JSON parser
 */
int main() {
    std::cout << "C++98 JSON Parser Demonstration" << std::endl;
    std::cout << "================================" << std::endl;
    
    try {
        // Demonstrate valid JSON parsing
        DemonstrateValidJson();
        
        // Demonstrate error handling
        DemonstrateInvalidJson();
        
        // Demonstrate memory management approaches
        DemonstrateMemoryManagement();
        
        std::cout << "\n\n=== All demonstrations completed successfully! ===" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
