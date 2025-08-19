#include <fstream>
#include <iostream>
#include <sstream>

#include "json_parser.hpp"

using namespace selfserv;

int main() {
  // Read the launch.json file
  std::ifstream inputFile("/home/umaru/Projects/selfserv/.vscode/launch.json");
  if (!inputFile.is_open()) {
    std::cerr << "Failed to open launch.json" << std::endl;
    return 1;
  }

  // Read entire file content
  std::ostringstream buffer;
  buffer << inputFile.rdbuf();
  std::string jsonContent = buffer.str();
  inputFile.close();

  std::cout << "Original JSON content:" << std::endl;
  std::cout << jsonContent << std::endl;
  std::cout << "\n" << std::string(50, '=') << "\n" << std::endl;

  try {
    // Parse the JSON
    JsonParser parser;
    JsonValuePtr root(parser.parse(jsonContent));

    std::cout << "✓ Successfully parsed launch.json!" << std::endl;

    // Access some specific data to demonstrate parsing worked
    JsonObject* rootObj = root->asObject();
    if (rootObj) {
      if (rootObj->hasKey("version")) {
        JsonString* version = rootObj->at("version")->asString();
        if (version) {
          std::cout << "Version: " << version->getValue() << std::endl;
        }
      }

      if (rootObj->hasKey("configurations")) {
        JsonArray* configs = rootObj->at("configurations")->asArray();
        if (configs) {
          std::cout << "Number of configurations: " << configs->size()
                    << std::endl;

          if (configs->size() > 0) {
            JsonObject* firstConfig = configs->at(0)->asObject();
            if (firstConfig && firstConfig->hasKey("name")) {
              JsonString* name = firstConfig->at("name")->asString();
              if (name) {
                std::cout << "First configuration name: " << name->getValue()
                          << std::endl;
              }
            }
          }
        }
      }
    }

    // Generate the reconstructed JSON
    std::string reconstructedJson = root->toString();

    std::cout << "\nReconstructed JSON:" << std::endl;
    std::cout << reconstructedJson << std::endl;

    // Write to the copy file
    std::ofstream outputFile(
        "/home/umaru/Projects/selfserv/.vscode/launch.copy.json");
    if (!outputFile.is_open()) {
      std::cerr << "Failed to create launch.copy.json" << std::endl;
      return 1;
    }

    outputFile << reconstructedJson;
    outputFile.close();

    std::cout << "\n✓ Successfully wrote copy to launch.copy.json" << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "✗ Error parsing JSON: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
