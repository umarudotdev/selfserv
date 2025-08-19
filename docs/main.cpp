// Entry point: initialize configuration and start event loop
// C++98 compliant; no exceptions thrown explicitly.

#include <cassert>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "config/Config.hpp"
#include "config/ConfigParser.hpp"
#include "selfserv.h"
#include "server/Server.hpp"

static volatile std::sig_atomic_t g_running = 1;

static void handle_sigint(int) { g_running = 0; }

static std::string defaultConfigPath() {
  return "conf/selfserv.conf";  // relative to working directory
}

int main(int argc, char **argv) {
  assert(SELFSERV_VERSION_MAJOR == 0);

  std::signal(SIGINT, handle_sigint);
  std::string path;
  if (argc > 1) {
    path = argv[1];
  } else {
    path = defaultConfigPath();
  }

  Config config;
  ConfigParser parser;
  if (!parser.ParseFile(path.c_str(), config)) {
    std::cerr << "Failed to parse config: " << path << "\n";
    return 1;
  }

  if (config.servers.empty()) {
    std::cerr << "No server blocks configured.\n";
    return 1;
  }

  Server server(config);
  if (!server.Init()) {
    std::cerr << "Server initialization failed.\n";
    return 1;
  }

  while (g_running) {
    if (!server.PollOnce(1000)) {  // 1s timeout to allow signal check
      break;                       // poll error
    }
    server.ProcessEvents();
  }

  server.Shutdown();
  return 0;
}
