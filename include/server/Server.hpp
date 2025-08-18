#pragma once

#include "config/Config.hpp"
#include "server/FD.hpp"
#include <map>
#include <vector>
#include <string>
#include <poll.h>

struct ClientConnection {
  FD fd;
  std::string readBuf;
  std::string writeBuf;
  bool wantWrite;
  ClientConnection() : wantWrite(false) {}
};

class Server {
 public:
  explicit Server(const Config &cfg);
  bool init();
  bool pollOnce(int timeoutMs);
  void processEvents();
  void shutdown();

 private:
  Server(const Server &);
  Server &operator=(const Server &);

  bool openListeningSockets();
  void acceptNew(int listenFd);
  void handleReadable(ClientConnection &conn);
  void handleWritable(ClientConnection &conn);
  void closeConnection(int fd);
  void buildPollFds(std::vector<struct pollfd> &pfds);

  const Config &config_;
  std::vector<FD> listenSockets_;
  std::map<int, ClientConnection> clients_;
  std::vector<struct pollfd> pfds_;
};
