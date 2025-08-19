#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "config/Config.hpp"

class ConfigParser {
 public:
  ConfigParser();
  bool ParseFile(const char *path, Config &out);

 private:
  bool ParseLine(const std::string &line, Config &out,
                 ServerConfig *&currentServer);
};
