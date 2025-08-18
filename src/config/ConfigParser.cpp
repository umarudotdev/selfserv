#include "config/ConfigParser.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <cstdio>

ConfigParser::ConfigParser() {}

bool ConfigParser::parseFile(const char *path, Config &out) {
  FILE *f = std::fopen(path, "r");
  if (!f) {
    std::perror("open config");
    return false;
  }
  char line[512];
  ServerConfig *current = 0;
  while (std::fgets(line, sizeof(line), f)) {
    std::string s(line);
    if (!parseLine(s, out, current)) {
      std::cerr << "Config parse error on line: " << s;
      std::fclose(f);
      return false;
    }
  }
  std::fclose(f);
  return true;
}

bool ConfigParser::parseLine(const std::string &line, Config &out, ServerConfig *&currentServer) {
  if (line.empty() || line[0] == '#') return true;
  std::string::size_type start = 0; std::vector<std::string> tokens;
  while (start < line.size()) {
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t' || line[start] == '\n')) ++start;
    if (start >= line.size()) break;
    std::string::size_type end = start;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\n') ++end;
    tokens.push_back(line.substr(start, end - start));
    start = end;
  }
  if (tokens.empty()) return true;
  if (tokens[0] == "server") {
    if (tokens.size() < 3) return false;
    ServerConfig sc;
    sc.host = tokens[1];
    sc.port = std::atoi(tokens[2].c_str());
    out.servers.push_back(sc);
    currentServer = &out.servers.back();
    return true;
  } else if (tokens[0] == "route") {
    if (!currentServer || tokens.size() < 3) return false;
    RouteConfig rc;
    rc.path = tokens[1];
    rc.root = tokens[2];
    currentServer->routes.push_back(rc);
    return true;
  }
  return true;
}
