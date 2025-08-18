#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "config/Config.hpp"

class ConfigParser {
 public:
  ConfigParser();
  bool parseFile(const char *path, Config &out);

 private:
  bool parseLine(const std::string &line, Config &out,
                 ServerConfig *&currentServer);
};
