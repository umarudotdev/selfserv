#pragma once

#include "config/Config.hpp"
#include <string>
#include <vector>
#include <cstdio>

class ConfigParser {
 public:
  ConfigParser();
  bool parseFile(const char *path, Config &out);

 private:
  bool parseLine(const std::string &line, Config &out, ServerConfig *&currentServer);
};
