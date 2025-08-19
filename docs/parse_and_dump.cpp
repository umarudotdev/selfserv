#include "json_parser.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

/**
 * @brief Read entire file content into a string
 */
std::string ReadFile(const std::string& filename) {
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filename);
    }
    
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/**
 * @brief Write string content to a file
 */
void WriteFile(const std::string& filename, const std::string& content) {
    std::ofstream file(filename.c_str());
    if (!file.is_open()) {
        throw std::runtime_error("Could not create file: " + filename);
    }
    
    file << content;
}

/**
 * @brief Remove C-style comments from JSON content
 * This is a simple implementation to handle single-line and multi-line comments
 */
std::string RemoveComments(const std::string& jsonWithComments) {
    std::string result;
    result.reserve(jsonWithComments.length());
    
    bool inString = false;
    bool inSingleLineComment = false;
    bool inMultiLineComment = false;
    bool escaped = false;
    
    for (size_t i = 0; i < jsonWithComments.length(); ++i) {
        char c = jsonWithComments[i];
        char next = (i + 1 < jsonWithComments.length()) ? jsonWithComments[i + 1] : '\0';
        
        if (inSingleLineComment) {
            if (c == '\n' || c == '\r') {
                inSingleLineComment = false;
                result += c; // Keep the newline
            }
            continue;
        }
        
        if (inMultiLineComment) {
            if (c == '*' && next == '/') {
                inMultiLineComment = false;
                ++i; // Skip the '/'
            }
            continue;
        }
        
        if (inString) {
            result += c;
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        
        // Not in string, comment, or escape sequence
        if (c == '"') {
            inString = true;
            result += c;
        } else if (c == '/' && next == '/') {
            inSingleLineComment = true;
            ++i; // Skip the second '/'
        } else if (c == '/' && next == '*') {
            inMultiLineComment = true;
            ++i; // Skip the '*'
        } else {
            result += c;
        }
    }
    
    return result;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <input_file> <output_file>" << std::endl;
        return 1;
    }
    
    std::string inputFile = argv[1];
    std::string outputFile = argv[2];
    
    try {
        // Read the input file
        std::cout << "Reading file: " << inputFile << std::endl;
        std::string jsonContent = ReadFile(inputFile);
        
        // Remove comments (since launch.json is JSONC format)
        std::cout << "Removing comments..." << std::endl;
        std::string cleanJson = RemoveComments(jsonContent);
        
        // Parse the JSON
        std::cout << "Parsing JSON..." << std::endl;
        JsonParser parser;
        JsonValuePtr rootPtr(parser.Parse(cleanJson));
        
        // Convert back to string
        std::cout << "Converting back to JSON..." << std::endl;
        std::string outputJson = rootPtr.Get()->ToString();
        
        // Write to output file
        std::cout << "Writing to file: " << outputFile << std::endl;
        WriteFile(outputFile, outputJson);
        
        std::cout << "Successfully parsed and dumped JSON!" << std::endl;
        std::cout << "Input file: " << inputFile << std::endl;
        std::cout << "Output file: " << outputFile << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        return 1;
    }
}
