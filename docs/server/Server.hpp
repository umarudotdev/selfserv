#pragma once

#include <poll.h>

#include <map>
#include <string>
#include <vector>

#include "config/Config.hpp"
#include "http/HttpRequest.hpp"
#include "server/FD.hpp"

struct ClientConnection {
  // Connection state
  FD m_fd;
  std::string m_readBuf;
  std::string m_writeBuf;
  bool m_wantWrite;
  HttpRequest m_request;
  HttpRequestParser m_parser;
  bool m_keepAlive;

  // Timing
  unsigned long m_createdAtMs;
  unsigned long m_lastActivityMs;

  // Protocol state
  bool m_headersComplete;
  bool m_bodyComplete;
  bool m_timedOut;

  // Connection phase (for debugging and state management)
  enum Phase {
    kPhaseAccepted,
    kPhaseHeaders,
    kPhaseBody,
    kPhaseHandle,
    kPhaseRespond,
    kPhaseIdle,
    kPhaseClosing
  } m_phase;

  // CGI execution context
  int m_cgiInFd;               // write-end to CGI stdin
  int m_cgiOutFd;              // read-end from CGI stdout
  pid_t m_cgiPid;              // child PID
  bool m_cgiActive;            // CGI process active
  bool m_cgiHeadersDone;       // parsed CGI headers
  std::string m_cgiBuffer;     // raw CGI output buffer
  size_t m_cgiBodyStart;       // offset where body starts after headers
  size_t m_cgiWriteOffset;     // how many bytes of request body written to CGI
  unsigned long m_cgiStartMs;  // when CGI launched
  int m_serverIndex;           // index of selected server config

  ClientConnection()
      : m_wantWrite(false),
        m_keepAlive(false),
        m_createdAtMs(0),
        m_lastActivityMs(0),
        m_headersComplete(false),
        m_bodyComplete(false),
        m_timedOut(false),
        m_phase(kPhaseAccepted),
        m_cgiInFd(-1),
        m_cgiOutFd(-1),
        m_cgiPid(-1),
        m_cgiActive(false),
        m_cgiHeadersDone(false),
        m_cgiBodyStart(0),
        m_cgiWriteOffset(0),
        m_cgiStartMs(0),
        m_serverIndex(0) {}
};

class Server {
 public:
  explicit Server(const Config &config);

  // Core server lifecycle
  bool Init();
  bool PollOnce(int timeoutMs);
  int ComputePollTimeout() const;  // dynamic based on earliest deadline
  void ProcessEvents();
  void Shutdown();

 private:
  // Non-copyable
  Server(const Server &);
  Server &operator=(const Server &);

  // Connection management
  bool OpenListeningSockets();
  void AcceptNew(int listenFd);
  void HandleReadable(ClientConnection &conn);
  void HandleWritable(ClientConnection &conn);
  void CloseConnection(int fd);
  void BuildPollFds(std::vector<struct pollfd> &pfds);

  // CGI support
  bool MaybeStartCgi(ClientConnection &conn, const RouteConfig &route,
                     const std::string &filePath);
  bool DriveCgiIO(ClientConnection &conn);  // returns false if should close
  void ReapCgi(ClientConnection &conn);
  bool HandleCgiEvent(int fd, short revents);

  // Member variables
  const Config &m_config;
  std::vector<FD> m_listenSockets;
  std::map<int, ClientConnection> m_clients;
  std::vector<struct pollfd> m_pfds;
  std::map<int, int> m_cgiFdToClient;  // map cgi pipe fd -> client fd
};
