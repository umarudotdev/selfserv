#pragma once

#include "config/Config.hpp"
#include "server/FD.hpp"
#include "http/HttpRequest.hpp"
#include <map>
#include <vector>
#include <string>
#include <poll.h>

struct ClientConnection {
  FD fd;
  std::string readBuf;
  std::string writeBuf;
  bool wantWrite;
  HttpRequest request;
  HttpRequestParser parser;
  bool keepAlive;
  unsigned long createdAtMs;
  unsigned long lastActivityMs;
  bool headersComplete;
  bool bodyComplete;
  bool timedOut;
  // High level connection phase (for future extensibility / debugging)
  enum Phase { PH_ACCEPTED, PH_HEADERS, PH_BODY, PH_HANDLE, PH_RESPOND, PH_IDLE, PH_CLOSING } phase;
  // CGI execution context (optional active)
  int cgiInFd;      // write-end to CGI stdin
  int cgiOutFd;     // read-end from CGI stdout
  pid_t cgiPid;     // child PID
  bool cgiActive;   // CGI process active
  bool cgiHeadersDone; // parsed CGI headers
  std::string cgiBuffer; // raw CGI output buffer
  size_t cgiBodyStart;   // offset where body starts after headers
  size_t cgiWriteOffset; // how many bytes of request body written to CGI
  unsigned long cgiStartMs; // when CGI launched
  int serverIndex; // index of selected server config
  ClientConnection() : wantWrite(false), keepAlive(false), createdAtMs(0), lastActivityMs(0), headersComplete(false), bodyComplete(false), timedOut(false), cgiInFd(-1), cgiOutFd(-1), cgiPid(-1), cgiActive(false), cgiHeadersDone(false), cgiBodyStart(0), cgiWriteOffset(0), cgiStartMs(0), serverIndex(0) {}
};

class Server {
 public:
  explicit Server(const Config &cfg);
  bool init();
  bool pollOnce(int timeoutMs);
  int computePollTimeout() const; // dynamic based on earliest deadline
  void processEvents();
  void shutdown();

 private:
  Server(const Server &);
  Server &operator=(const Server &);

  bool openListeningSockets();
  void acceptNew(int listenFd);
  void handleReadable(ClientConnection &conn);
  void handleWritable(ClientConnection &conn);
  // CGI helpers (to be implemented)
  bool maybeStartCgi(ClientConnection &conn, const RouteConfig &route, const std::string &filePath);
  bool driveCgiIO(ClientConnection &conn); // returns false if should close
  void reapCgi(ClientConnection &conn);
  void closeConnection(int fd);
  void buildPollFds(std::vector<struct pollfd> &pfds);
  bool handleCgiEvent(int fd, short revents);

  const Config &config_;
  std::vector<FD> listenSockets_;
  std::map<int, ClientConnection> clients_;
  std::vector<struct pollfd> pfds_;
  std::map<int,int> cgiFdToClient_; // map cgi pipe fd -> client fd
};
