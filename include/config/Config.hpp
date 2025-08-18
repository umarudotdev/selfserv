// Basic configuration data structures (C++98 POD-style)
#pragma once

#include <string>
#include <vector>

struct RouteConfig {
  std::string path;        // location path prefix
  std::string root;        // filesystem root for this route
  std::vector<std::string> methods; // allowed methods
  std::string redirect;    // optional redirect target
  std::string index;       // default file
  bool directoryListing;   // enable/disable autoindex
  bool uploadsEnabled;     // allow uploads
  std::string uploadPath;  // where to store uploads
  RouteConfig() : directoryListing(false), uploadsEnabled(false) {}
};

struct ServerConfig {
  std::string host; // e.g. 0.0.0.0
  int port;         // listening port
  std::vector<std::string> serverNames;
  std::string errorPageRoot;
  size_t clientMaxBodySize; // bytes
  // timeouts (milliseconds)
  int headerTimeoutMs;   // time to receive full headers
  int bodyTimeoutMs;     // time to receive full body
  int idleTimeoutMs;     // keep-alive idle timeout
  std::vector<RouteConfig> routes;
  ServerConfig() : port(0), clientMaxBodySize(1 << 20), headerTimeoutMs(5000), bodyTimeoutMs(10000), idleTimeoutMs(15000) {}
};

struct Config {
  std::vector<ServerConfig> servers;
};
