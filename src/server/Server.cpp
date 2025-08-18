#include "server/Server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <cstdio>

namespace {
static bool setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
  return true;
}
}

Server::Server(const Config &cfg) : config_(cfg) {}

bool Server::init() { return openListeningSockets(); }

bool Server::openListeningSockets() {
  for (size_t i = 0; i < config_.servers.size(); ++i) {
    const ServerConfig &sc = config_.servers[i];
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      std::perror("socket");
      return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr; std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(sc.port);
    addr.sin_addr.s_addr = sc.host.empty() ? htonl(INADDR_ANY) : inet_addr(sc.host.c_str());
    if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      std::perror("bind");
      ::close(fd);
      return false;
    }
    if (::listen(fd, 128) < 0) {
      std::perror("listen");
      ::close(fd);
      return false;
    }
    if (!setNonBlocking(fd)) {
      std::perror("nonblock");
      ::close(fd);
      return false;
    }
    listenSockets_.push_back(FD(fd));
  }
  return true;
}

bool Server::pollOnce(int timeoutMs) {
  buildPollFds(pfds_);
  if (pfds_.empty()) return true; // nothing to poll
  int ret = ::poll(&pfds_[0], pfds_.size(), timeoutMs);
  if (ret < 0) {
    std::perror("poll");
    return false;
  }
  return true;
}

void Server::processEvents() {
  for (size_t i = 0; i < pfds_.size(); ++i) {
    struct pollfd &p = pfds_[i];
    if (!p.revents) continue;
    bool isListen = false;
    for (size_t j = 0; j < listenSockets_.size(); ++j) {
      if (listenSockets_[j].get() == p.fd) { isListen = true; break; }
    }
    if (isListen && (p.revents & POLLIN)) {
      acceptNew(p.fd);
    } else {
      std::map<int, ClientConnection>::iterator it = clients_.find(p.fd);
      if (it != clients_.end()) {
        if (p.revents & POLLIN) handleReadable(it->second);
        if (p.revents & POLLOUT) handleWritable(it->second);
        if (p.revents & (POLLHUP | POLLERR)) closeConnection(p.fd);
      }
    }
  }
}

void Server::acceptNew(int listenFd) {
  for (;;) {
    int cfd = ::accept(listenFd, 0, 0);
    if (cfd < 0) {
      break; // non-blocking accept finished
    }
    if (!setNonBlocking(cfd)) {
      ::close(cfd);
      continue;
    }
    ClientConnection conn; conn.fd.reset(cfd); conn.wantWrite = false;
    clients_[cfd] = conn;
  }
}

static std::string buildHttpResponse(const std::string &body) {
  std::string resp = "HTTP/1.1 200 OK\r\n";
  char len[32]; std::sprintf(len, "%lu", (unsigned long)body.size());
  resp += "Content-Length: "; resp += len; resp += "\r\n";
  resp += "Content-Type: text/plain\r\n";
  resp += "Connection: close\r\n\r\n";
  resp += body;
  return resp;
}

void Server::handleReadable(ClientConnection &conn) {
  char buf[4096];
  for (;;) {
    ssize_t n = ::recv(conn.fd.get(), buf, sizeof(buf), 0);
    if (n <= 0) break; // EAGAIN or closed
    conn.readBuf.append(buf, n);
    if (conn.readBuf.find("\r\n\r\n") != std::string::npos) {
      conn.writeBuf = buildHttpResponse("selfserv minimal response\n");
      conn.wantWrite = true;
      break;
    }
  }
}

void Server::handleWritable(ClientConnection &conn) {
  while (!conn.writeBuf.empty()) {
    ssize_t n = ::send(conn.fd.get(), conn.writeBuf.data(), conn.writeBuf.size(), 0);
    if (n <= 0) break;
    conn.writeBuf.erase(0, n);
  }
  if (conn.writeBuf.empty()) {
    closeConnection(conn.fd.get());
  }
}

void Server::closeConnection(int fd) {
  std::map<int, ClientConnection>::iterator it = clients_.find(fd);
  if (it != clients_.end()) {
    clients_.erase(it);
  }
}

void Server::buildPollFds(std::vector<struct pollfd> &pfds) {
  pfds.clear();
  for (size_t i = 0; i < listenSockets_.size(); ++i) {
    struct pollfd p; p.fd = listenSockets_[i].get(); p.events = POLLIN; p.revents = 0; pfds.push_back(p);
  }
  std::map<int, ClientConnection>::iterator it = clients_.begin();
  for (; it != clients_.end(); ++it) {
    struct pollfd p; p.fd = it->first; p.events = POLLIN; if (it->second.wantWrite) p.events |= POLLOUT; p.revents = 0; pfds.push_back(p);
  }
}

void Server::shutdown() {
  clients_.clear();
  listenSockets_.clear();
}
