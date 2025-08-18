#include "config/ConfigParser.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

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

bool ConfigParser::parseLine(const std::string &line, Config &out,
                             ServerConfig *&currentServer) {
  if (line.empty() || line[0] == '#') return true;
  std::string::size_type start = 0;
  std::vector<std::string> tokens;
  while (start < line.size()) {
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t' || line[start] == '\n'))
      ++start;
    if (start >= line.size()) break;
    std::string::size_type end = start;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t' &&
           line[end] != '\n')
      ++end;
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
  } else if (tokens[0] == "server_name") {
    if (!currentServer || tokens.size() < 2) return false;
    for (size_t i = 1; i < tokens.size(); ++i)
      currentServer->serverNames.push_back(tokens[i]);
    return true;
  } else if (tokens[0] == "error_page_root") {
    if (!currentServer || tokens.size() < 2) return false;
    currentServer->errorPageRoot = tokens[1];
    return true;
  } else if (tokens[0] == "client_max_body_size") {
    if (!currentServer || tokens.size() < 2) return false;
    currentServer->clientMaxBodySize = (size_t)std::atoi(tokens[1].c_str());
    return true;
  } else if (tokens[0] == "header_timeout") {
    if (!currentServer || tokens.size() < 2) return false;
    currentServer->headerTimeoutMs = std::atoi(tokens[1].c_str());
    return true;
  } else if (tokens[0] == "body_timeout") {
    if (!currentServer || tokens.size() < 2) return false;
    currentServer->bodyTimeoutMs = std::atoi(tokens[1].c_str());
    return true;
  } else if (tokens[0] == "idle_timeout") {
    if (!currentServer || tokens.size() < 2) return false;
    currentServer->idleTimeoutMs = std::atoi(tokens[1].c_str());
    return true;
  } else if (tokens[0] == "cgi_timeout") {
    if (!currentServer || tokens.size() < 2) return false;
    currentServer->cgiTimeoutMs = std::atoi(tokens[1].c_str());
    return true;
  } else if (tokens[0] == "route") {
    if (!currentServer || tokens.size() < 3) return false;
    RouteConfig rc;
    rc.path = tokens[1];
    rc.root = tokens[2];
    // Optional tokens: key=value
    for (size_t i = 3; i < tokens.size(); ++i) {
      std::string::size_type eq = tokens[i].find('=');
      if (eq == std::string::npos) continue;
      std::string key = tokens[i].substr(0, eq);
      std::string val = tokens[i].substr(eq + 1);
      if (key == "index") {
        rc.index = val;
      } else if (key == "methods") {
        // comma separated
        size_t start = 0;
        while (start < val.size()) {
          size_t comma = val.find(',', start);
          if (comma == std::string::npos) comma = val.size();
          rc.methods.push_back(val.substr(start, comma - start));
          start = comma + 1;
        }
      } else if (key == "upload") {
        if (val == "on" || val == "1" || val == "true")
          rc.uploadsEnabled = true;
      } else if (key == "upload_path") {
        rc.uploadPath = val;
      } else if (key == "autoindex") {
        if (val == "on" || val == "1" || val == "true")
          rc.directoryListing = true;
      } else if (key == "redirect") {
        rc.redirect = val;
      } else if (key == "cgi_ext") {
        rc.cgiExtension = val;
      } else if (key == "cgi_bin") {
        rc.cgiInterpreter = val;
      }
    }
    currentServer->routes.push_back(rc);
    return true;
  }
  return true;
}
