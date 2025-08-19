#include "server/Server.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>

namespace {
static bool setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
  return true;
}
}  // namespace

// forward declaration for static helper used in timeout sweep
static std::string buildResponse(int code, const std::string &reason,
                                 const std::string &body, const char *ctype,
                                 bool keepAlive, bool headOnly);
static std::string buildRedirect(int code, const std::string &reason,
                                 const std::string &location, bool keepAlive);
static std::string loadErrorPageBody(const ServerConfig &sc, int code,
                                     const std::string &fallback);

Server::Server(const Config &cfg) : m_config(cfg) {}

bool Server::Init() { return OpenListeningSockets(); }

bool Server::OpenListeningSockets() {
  for (size_t i = 0; i < m_config.servers.size(); ++i) {
    const ServerConfig &sc = m_config.servers[i];
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      std::perror("socket");
      return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(sc.port);
    addr.sin_addr.s_addr =
        sc.host.empty() ? htonl(INADDR_ANY) : inet_addr(sc.host.c_str());
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
    m_listenSockets.push_back(FD(fd));
  }
  return true;
}

bool Server::PollOnce(int timeoutMs) {
  BuildPollFds(m_pfds);
  if (m_pfds.empty()) return true;  // nothing to poll
  int dyn = ComputePollTimeout();
  if (dyn >= 0 && (timeoutMs < 0 || dyn < timeoutMs)) timeoutMs = dyn;
  int ret = ::poll(&m_pfds[0], m_pfds.size(), timeoutMs);
  if (ret < 0) {
    std::perror("poll");
    return false;
  }
  return true;
}

int Server::ComputePollTimeout() const {
  if (m_clients.empty()) return -1;
  unsigned long nowMs = (unsigned long)std::time(0) * 1000UL;
  long best = -1;
  for (std::map<int, ClientConnection>::const_iterator it = m_clients.begin();
       it != m_clients.end(); ++it) {
    const ClientConnection &c = it->second;
    unsigned long deadline = 0;
    if (!c.m_headersComplete) {
      // Can't know virtual host yet; use first server's header timeout
      const ServerConfig &scHdr = m_config.servers[0];
      deadline = c.m_createdAtMs + (unsigned long)scHdr.headerTimeoutMs;
    } else {
      const ServerConfig &scRef =
          (c.m_serverIndex >= 0 &&
           (size_t)c.m_serverIndex < m_config.servers.size())
              ? m_config.servers[c.m_serverIndex]
              : m_config.servers[0];
      if (!c.m_bodyComplete)
        deadline = c.m_lastActivityMs + (unsigned long)scRef.bodyTimeoutMs;
      else if (c.m_keepAlive)
        deadline = c.m_lastActivityMs + (unsigned long)scRef.idleTimeoutMs;
    }
    if (deadline) {
      long remain = (long)deadline - (long)nowMs;
      if (remain < 0) remain = 0;
      if (best < 0 || remain < best) best = remain;
    }
  }
  return (int)(best);
}

void Server::ProcessEvents() {
  // Sweep for timeouts before handling events
  unsigned long nowMs = (unsigned long)std::time(0) * 1000UL;
  std::map<int, ClientConnection>::iterator itSweep = m_clients.begin();
  while (itSweep != m_clients.end()) {
    ClientConnection &c = itSweep->second;
    bool closeIt = false;
    // CGI timeout check
    if (c.m_cgiActive && c.m_serverIndex >= 0 &&
        (size_t)c.m_serverIndex < m_config.servers.size()) {
      const ServerConfig &scSrv = m_config.servers[c.m_serverIndex];
      if (scSrv.cgiTimeoutMs > 0 && c.m_cgiStartMs > 0) {
        unsigned long nowMsLocal = nowMs;
        if (nowMsLocal - c.m_cgiStartMs > (unsigned long)scSrv.cgiTimeoutMs) {
          std::cerr << "[cgi-timeout] pid=" << c.m_cgiPid
                    << " fd=" << itSweep->first << "\n";
          if (c.m_cgiPid > 0) ::kill(c.m_cgiPid, SIGKILL);
          ReapCgi(c);
          c.m_keepAlive = false;
          c.m_writeBuf = buildResponse(504, "Gateway Timeout",
                                       "504 Gateway Timeout (CGI)\n",
                                       "text/plain", false, false);
          c.m_phase = ClientConnection::kPhaseRespond;
          c.m_wantWrite = true;
        }
      }
    }
    // timeouts (use per-virtual-host once known)
    if (!c.m_headersComplete) {
      const ServerConfig &scHdr = m_config.servers[0];
      if (scHdr.headerTimeoutMs > 0 &&
          nowMs - c.m_createdAtMs > (unsigned long)scHdr.headerTimeoutMs)
        closeIt = true;
    } else {
      const ServerConfig &scRef =
          (c.m_serverIndex >= 0 &&
           (size_t)c.m_serverIndex < m_config.servers.size())
              ? m_config.servers[c.m_serverIndex]
              : m_config.servers[0];
      if (!c.m_bodyComplete) {
        if (scRef.bodyTimeoutMs > 0 &&
            nowMs - c.m_lastActivityMs > (unsigned long)scRef.bodyTimeoutMs)
          closeIt = true;
      } else if (c.m_keepAlive && scRef.idleTimeoutMs > 0 &&
                 nowMs - c.m_lastActivityMs >
                     (unsigned long)scRef.idleTimeoutMs) {
        closeIt = true;
      }
    }
    if (closeIt) {
      int fd = itSweep->first;
      if (!c.m_headersComplete || !c.m_bodyComplete) {
        std::cerr << "[timeout] fd=" << fd << " sending 408\n";
        if (c.m_writeBuf.empty()) {
          c.m_writeBuf =
              buildResponse(408, "Request Timeout", "408 Request Timeout\n",
                            "text/plain", false, false);
          c.m_wantWrite = true;
        }
      } else {
        std::cerr << "[idle-timeout] fd=" << fd << " closing keep-alive\n";
      }
      c.m_keepAlive = false;
      c.m_phase = ClientConnection::kPhaseClosing;
      ++itSweep;  // leave connection to flush
    } else {
      ++itSweep;
    }
  }
  for (size_t i = 0; i < m_pfds.size(); ++i) {
    struct pollfd &p = m_pfds[i];
    if (!p.revents) continue;
    bool isListen = false;
    for (size_t j = 0; j < m_listenSockets.size(); ++j) {
      if (m_listenSockets[j].Get() == p.fd) {
        isListen = true;
        break;
      }
    }
    // Check CGI fds first
    if (!isListen) {
      if (m_cgiFdToClient.find(p.fd) != m_cgiFdToClient.end()) {
        HandleCgiEvent(p.fd, p.revents);
        continue;
      }
    }
    if (isListen && (p.revents & POLLIN)) {
      AcceptNew(p.fd);
    } else {
      std::map<int, ClientConnection>::iterator it = m_clients.find(p.fd);
      if (it != m_clients.end()) {
        if (p.revents & POLLIN) HandleReadable(it->second);
        if (p.revents & POLLOUT) HandleWritable(it->second);
        if (p.revents & (POLLHUP | POLLERR)) CloseConnection(p.fd);
      }
    }
  }
}

void Server::AcceptNew(int listenFd) {
  for (;;) {
    int cfd = ::accept(listenFd, 0, 0);
    if (cfd < 0) {
      break;  // non-blocking accept finished
    }
    if (!setNonBlocking(cfd)) {
      ::close(cfd);
      continue;
    }
    ClientConnection conn;
    conn.m_fd.Reset(cfd);
    conn.m_wantWrite = false;
    unsigned long nowMs = (unsigned long)(std::time(0)) * 1000UL;  // coarse
    conn.m_createdAtMs = nowMs;
    conn.m_lastActivityMs = nowMs;
    conn.m_headersComplete = false;
    conn.m_bodyComplete = false;
    conn.m_phase = ClientConnection::kPhaseAccepted;
    m_clients[cfd] = conn;
    std::cerr << "[accept] fd=" << cfd << " total_clients=" << m_clients.size()
              << "\n";
  }
}

static std::string buildResponse(int code, const std::string &reason,
                                 const std::string &body, const char *ctype,
                                 bool keepAlive, bool headOnly) {
  std::string resp = "HTTP/1.1 ";
  char codeBuf[8];
  std::sprintf(codeBuf, "%d", code);
  resp += codeBuf;
  resp += ' ';
  resp += reason;
  resp += "\r\n";
  char len[32];
  std::sprintf(len, "%lu", (unsigned long)body.size());
  resp += "Content-Length: ";
  resp += len;
  resp += "\r\n";
  resp += "Content-Type: ";
  resp += ctype;
  resp += "\r\n";
  resp += "Connection: ";
  resp += keepAlive ? "keep-alive" : "close";
  resp += "\r\n\r\n";
  if (!headOnly) resp += body;
  return resp;
}

static std::string buildRedirect(int code, const std::string &reason,
                                 const std::string &location, bool keepAlive) {
  std::string body = "<html><body><h1>" + reason + "</h1><a href='" + location +
                     "'>" + location + "</a></body></html>";
  std::string resp = "HTTP/1.1 ";
  char codeBuf[8];
  std::sprintf(codeBuf, "%d", code);
  resp += codeBuf;
  resp += ' ';
  resp += reason;
  resp += "\r\n";
  resp += "Location: ";
  resp += location;
  resp += "\r\n";
  char len[32];
  std::sprintf(len, "%lu", (unsigned long)body.size());
  resp += "Content-Length: ";
  resp += len;
  resp += "\r\nContent-Type: text/html\r\n";
  resp += "Connection: ";
  resp += keepAlive ? "keep-alive" : "close";
  resp += "\r\n\r\n";
  resp += body;
  return resp;
}

static std::string loadErrorPageBody(const ServerConfig &sc, int code,
                                     const std::string &fallback) {
  if (sc.errorPageRoot.empty()) return fallback;
  char fname[16];
  std::sprintf(fname, "%d.html", code);
  std::string path = sc.errorPageRoot;
  if (!path.empty() && path[path.size() - 1] != '/') path += '/';
  path += fname;
  std::string contents;
  std::ifstream ifs(path.c_str());
  if (ifs) {
    std::string tmp;
    char buf[1024];
    while (ifs.good()) {
      ifs.read(buf, sizeof(buf));
      std::streamsize n = ifs.gcount();
      if (n > 0) tmp.append(buf, (size_t)n);
    }
    if (!tmp.empty()) return tmp;
  }
  return fallback;
}

static bool readFile(const std::string &path, std::string &out) {
  std::ifstream ifs(path.c_str(), std::ios::in | std::ios::binary);
  if (!ifs) return false;
  std::string data;
  char buf[4096];
  while (ifs.good()) {
    ifs.read(buf, sizeof(buf));
    std::streamsize n = ifs.gcount();
    if (n > 0) data.append(buf, (size_t)n);
  }
  out.swap(data);
  return true;
}

static bool isDir(const std::string &path) {
  struct stat st;
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  return false;
}

static bool listDir(const std::string &path, std::string &out) {
  DIR *d = ::opendir(path.c_str());
  if (!d) return false;
  std::string body = "<html><body><h1>Index of ";
  body += path;
  body += "</h1><ul>";
  struct dirent *ent;
  while ((ent = ::readdir(d))) {
    const char *name = ent->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
    body += "<li><a href=\"";
    body += name;
    body += "\">";
    body += name;
    body += "</a></li>";
  }
  ::closedir(d);
  body += "</ul></body></html>";
  out.swap(body);
  return true;
}

static const RouteConfig *matchRoute(const ServerConfig &sc,
                                     const std::string &uri) {
  size_t bestLen = 0;
  const RouteConfig *best = 0;
  for (size_t i = 0; i < sc.routes.size(); ++i) {
    const RouteConfig &r = sc.routes[i];
    if (uri.compare(0, r.path.size(), r.path) == 0) {
      if (r.path.size() > bestLen) {
        bestLen = r.path.size();
        best = &r;
      }
    }
  }
  return best;
}

// Virtual host selection: pick server whose serverNames contains Host header
// (case-insensitive exact match); fallback to first.
static const ServerConfig &selectServer(const Config &cfg,
                                        const HttpRequest &req,
                                        size_t &idxOut) {
  std::string host;
  for (size_t i = 0; i < req.headers.size(); ++i) {
    std::string name = req.headers[i].name;
    for (size_t j = 0; j < name.size(); ++j)
      name[j] = (char)std::tolower(name[j]);
    if (name == "host") {
      host = req.headers[i].value;
      break;
    }
  }
  if (host.empty()) {
    idxOut = 0;
    return cfg.servers[0];
  }
  size_t colon = host.rfind(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  for (size_t i = 0; i < cfg.servers.size(); ++i) {
    const ServerConfig &sc = cfg.servers[i];
    for (size_t j = 0; j < sc.serverNames.size(); ++j)
      if (sc.serverNames[j] == host) {
        idxOut = i;
        return sc;
      }
  }
  idxOut = 0;
  return cfg.servers[0];
}

static const char *guessType(const std::string &path) {
  const char *dot = 0;
  for (size_t i = path.size(); i > 0; --i) {
    if (path[i - 1] == '.') {
      dot = path.c_str() + i - 1;
      break;
    }
    if (path[i - 1] == '/' || path[i - 1] == '\\') break;
  }
  if (!dot) return "text/plain";
  if (!std::strcmp(dot, ".html") || !std::strcmp(dot, ".htm"))
    return "text/html";
  if (!std::strcmp(dot, ".css")) return "text/css";
  if (!std::strcmp(dot, ".js")) return "application/javascript";
  if (!std::strcmp(dot, ".png")) return "image/png";
  if (!std::strcmp(dot, ".jpg") || !std::strcmp(dot, ".jpeg"))
    return "image/jpeg";
  if (!std::strcmp(dot, ".gif")) return "image/gif";
  return "text/plain";
}

// Sanitize filename by stripping directory components and dangerous chars
static std::string sanitizeFilename(const std::string &in) {
  std::string name;
  // strip path components
  size_t start = 0;
  for (size_t i = 0; i < in.size(); ++i)
    if (in[i] == '/' || in[i] == '\\') start = i + 1;
  name = in.substr(start);
  // remove CR/LF
  std::string clean;
  for (size_t i = 0; i < name.size(); ++i) {
    unsigned char c = name[i];
    if (c == '\r' || c == '\n') continue;
    if (c < 32) continue;
    if (c == '"') continue;
    clean += c;
  }
  if (clean.empty()) clean = "upload.bin";
  return clean;
}

static void ensureDir(const std::string &path) {
  if (path.empty()) return;
  struct stat st;
  if (::stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) return;
    return;  // exists but not dir
  }
  // attempt create (single level only)
  ::mkdir(path.c_str(), 0755);
}

struct MultipartSavedFile {
  std::string field;
  std::string filename;
  size_t size;
};

static bool parseContentDisposition(const std::string &line, std::string &name,
                                    std::string &filename) {
  // Expect: form-data; name="field"; filename="fname"
  size_t pos = line.find(':');
  if (pos == std::string::npos) return false;
  std::string after = line.substr(pos + 1);
  // split by ';'
  size_t p = 0;
  while (p < after.size()) {
    while (p < after.size() && (after[p] == ' ' || after[p] == '\t')) ++p;
    size_t q = p;
    while (q < after.size() && after[q] != ';') ++q;
    std::string token = after.substr(p, q - p);
    size_t eq = token.find('=');
    if (eq != std::string::npos) {
      std::string key = token.substr(0, eq);
      std::string val = token.substr(eq + 1);
      // trim
      while (!key.empty() && (key[0] == ' ' || key[0] == '\t')) key.erase(0, 1);
      while (!key.empty() &&
             (key[key.size() - 1] == ' ' || key[key.size() - 1] == '\t'))
        key.erase(key.size() - 1, 1);
      if (!val.empty() && val[0] == '"' && val[val.size() - 1] == '"' &&
          val.size() >= 2)
        val = val.substr(1, val.size() - 2);
      if (key == "name")
        name = val;
      else if (key == "filename")
        filename = val;
    }
    p = q + 1;
  }
  return true;
}

static bool parseMultipartFormData(const std::string &body,
                                   const std::string &boundary,
                                   const std::string &uploadPath,
                                   std::vector<MultipartSavedFile> &saved) {
  std::string boundaryMarker = "--" + boundary;
  std::string finalMarker = boundaryMarker + "--";
  size_t cursor = 0;
  while (cursor < body.size()) {
    // find next boundary
    size_t b = body.find(boundaryMarker, cursor);
    if (b == std::string::npos) break;
    b += boundaryMarker.size();
    if (b + 2 <= body.size() && body.compare(b, 2, "--") == 0)
      break;  // reached final boundary
    // expect CRLF after boundary
    if (b + 2 > body.size() || body[b] != '\r' || body[b + 1] != '\n') {
      cursor = b;
      continue;
    }
    size_t headerStart = b + 2;
    size_t headerEnd = body.find("\r\n\r\n", headerStart);
    if (headerEnd == std::string::npos) break;
    std::string headers = body.substr(headerStart, headerEnd - headerStart);
    size_t dataStart = headerEnd + 4;
    size_t nextBoundary = body.find(boundaryMarker, dataStart);
    if (nextBoundary == std::string::npos) break;
    size_t dataEnd = nextBoundary;
    // trim CRLF before boundary
    if (dataEnd >= 2 && body[dataEnd - 2] == '\r' && body[dataEnd - 1] == '\n')
      dataEnd -= 2;
    std::string fieldName, fileName;
    size_t hp = 0;
    while (hp < headers.size()) {
      size_t he = headers.find("\r\n", hp);
      if (he == std::string::npos) he = headers.size();
      std::string line = headers.substr(hp, he - hp);
      std::string lower = line;
      for (size_t i = 0; i < lower.size(); ++i)
        lower[i] = (char)std::tolower(lower[i]);
      if (lower.find("content-disposition:") == 0)
        parseContentDisposition(line, fieldName, fileName);
      hp = he + 2;
    }
    if (!fileName.empty()) {
      ensureDir(uploadPath);
      std::string safe = sanitizeFilename(fileName);
      std::string full = uploadPath;
      if (full[full.size() - 1] != '/') full += '/';
      full += safe;
      FILE *wf = std::fopen(full.c_str(), "wb");
      if (wf) {
        if (dataEnd > dataStart)
          std::fwrite(&body[dataStart], 1, dataEnd - dataStart, wf);
        std::fclose(wf);
        MultipartSavedFile sf;
        sf.field = fieldName;
        sf.filename = full;
        sf.size = dataEnd - dataStart;
        saved.push_back(sf);
      }
    }
    cursor = nextBoundary;
  }
  return !saved.empty();
}

static bool hasHeader(const HttpRequest &req, const char *name,
                      std::string &value) {
  for (size_t i = 0; i < req.headers.size(); ++i) {
    if (req.headers[i].name.size() == 0) continue;
    // case-insensitive compare
    const std::string &hn = req.headers[i].name;
    size_t j = 0;
    for (; j < hn.size() && name[j]; ++j) {
      if (std::tolower(hn[j]) != std::tolower(name[j])) break;
    }
    if (name[j] == 0 && j == hn.size()) {
      value = req.headers[i].value;
      return true;
    }
  }
  return false;
}

void Server::HandleReadable(ClientConnection &conn) {
  char buf[4096];
  for (;;) {
    ssize_t n = ::recv(conn.m_fd.Get(), buf, sizeof(buf), 0);
    if (n <= 0) break;  // EAGAIN or closed
    conn.m_readBuf.append(buf, n);
    conn.m_lastActivityMs = (unsigned long)std::time(0) * 1000UL;
    if (conn.m_readBuf.size() < 2048) {
      // lightweight debug
      if (conn.m_readBuf.find("POST /upload") != std::string::npos) {
        std::cerr << "[DBG] recv bytes=" << n
                  << " total=" << conn.m_readBuf.size() << " first100='"
                  << conn.m_readBuf.substr(
                         0, (conn.m_readBuf.size() < 100 ? conn.m_readBuf.size()
                                                         : 100))
                  << "'\n";
      }
    }
    if (conn.m_parser.Parse(conn.m_readBuf, conn.m_request) ||
        conn.m_parser.Error()) {
      // Future: if request requires CGI, transition to PH_HANDLE then spawn CGI
      // before PH_RESPOND
      if (conn.m_parser.Error()) {
        conn.m_keepAlive = false;
        const ServerConfig &scTmp = m_config.servers[0];
        std::string bodyErr =
            loadErrorPageBody(scTmp, 400, "400 Bad Request\n");
        conn.m_writeBuf = buildResponse(400, "Bad Request", bodyErr,
                                        "text/plain", false, false);
        conn.m_phase = ClientConnection::kPhaseRespond;
        std::cerr << "[400] malformed request bytes=" << conn.m_readBuf.size()
                  << "\n";
        conn.m_wantWrite = true;
        break;
      }
      conn.m_headersComplete = true;  // we have at least parsed headers (parser
                                      // only flips after full body though)
      if (conn.m_phase == ClientConnection::kPhaseAccepted)
        conn.m_phase = ClientConnection::kPhaseHeaders;
      size_t serverIdx = 0;
      const ServerConfig &sc =
          selectServer(m_config, conn.m_request, serverIdx);
      conn.m_serverIndex = (int)serverIdx;
      if (conn.m_request.body.size() > sc.clientMaxBodySize) {
        conn.m_keepAlive = false;
        std::string body413 =
            loadErrorPageBody(sc, 413, "413 Payload Too Large\n");
        conn.m_writeBuf = buildResponse(413, "Payload Too Large", body413,
                                        "text/plain", false, false);
        conn.m_phase = ClientConnection::kPhaseRespond;
        std::cerr << "[413] body_size=" << conn.m_request.body.size()
                  << " limit=" << sc.clientMaxBodySize << "\n";
        conn.m_wantWrite = true;
        break;
      }
      const RouteConfig *route = matchRoute(sc, conn.m_request.uri);
      if (!route) {
        conn.m_keepAlive = false;
        std::string body404 = loadErrorPageBody(sc, 404, "404 Not Found\n");
        conn.m_writeBuf =
            buildResponse(404, "Not Found", body404, "text/plain",
                          conn.m_keepAlive, conn.m_request.method == "HEAD");
        conn.m_phase = ClientConnection::kPhaseRespond;
        std::cerr << "[404] uri=" << conn.m_request.uri << "\n";
      } else {
        if (!route->methods.empty()) {
          bool ok = false;
          for (size_t i = 0; i < route->methods.size(); ++i)
            if (route->methods[i] == conn.m_request.method) {
              ok = true;
              break;
            }
          if (!ok) {
            conn.m_keepAlive = false;
            conn.m_writeBuf = buildResponse(405, "Method Not Allowed",
                                            "405 Method Not Allowed\n",
                                            "text/plain", conn.m_keepAlive,
                                            conn.m_request.method == "HEAD");
            std::cerr << "[405] method=" << conn.m_request.method
                      << " uri=" << conn.m_request.uri << "\n";
            conn.m_phase = ClientConnection::kPhaseRespond;
            conn.m_wantWrite = true;
            break;
          }
        }
        std::string rel = conn.m_request.uri.substr(route->path.size());
        if (rel.empty() || rel == "/") {
          if (!route->index.empty()) rel = "/" + route->index;
        }
        // Redirect handling
        if (!route->redirect.empty()) {
          conn.m_keepAlive = false;  // simpler; could keep-alive later
          std::cerr << "[302] redirect uri=" << conn.m_request.uri << " -> "
                    << route->redirect << "\n";
          conn.m_writeBuf =
              buildRedirect(302, "Found", route->redirect, conn.m_keepAlive);
          conn.m_phase = ClientConnection::kPhaseRespond;
          conn.m_bodyComplete = true;
          conn.m_wantWrite = true;
          break;
        }
        // Basic traversal guard
        if (rel.find("..") != std::string::npos) {
          conn.m_keepAlive = false;
          std::string body403 = loadErrorPageBody(sc, 403, "403 Forbidden\n");
          conn.m_writeBuf =
              buildResponse(403, "Forbidden", body403, "text/plain",
                            conn.m_keepAlive, conn.m_request.method == "HEAD");
          conn.m_phase = ClientConnection::kPhaseRespond;
          std::cerr << "[403] traversal attempt uri=" << conn.m_request.uri
                    << "\n";
        } else {
          std::string filePath = route->root + rel;
          // Decide if CGI based on extension match
          bool wantsCgi = false;
          if (!route->cgiExtension.empty() &&
              filePath.size() >= route->cgiExtension.size()) {
            if (filePath.compare(filePath.size() - route->cgiExtension.size(),
                                 route->cgiExtension.size(),
                                 route->cgiExtension) == 0)
              wantsCgi = true;
          }
          std::string body;
          if (wantsCgi) {
            if (MaybeStartCgi(conn, *route, filePath)) {
              conn.m_cgiStartMs = (unsigned long)std::time(0) * 1000UL;
              conn.m_phase = ClientConnection::kPhaseHandle;
              conn.m_wantWrite = false;
              std::cerr << "[CGI] started pid=" << conn.m_cgiPid
                        << " script=" << filePath << "\n";
            } else {
              conn.m_keepAlive = false;
              std::string body500 =
                  loadErrorPageBody(sc, 500, "500 Internal Server Error\n");
              conn.m_writeBuf =
                  buildResponse(500, "Internal Server Error", body500,
                                "text/plain", false, false);
              conn.m_phase = ClientConnection::kPhaseRespond;
              conn.m_wantWrite = true;
            }
          } else if (conn.m_request.method == "POST" && route->uploadsEnabled) {
            std::string keep;
            conn.m_keepAlive = false;
            if (hasHeader(conn.m_request, "Connection", keep)) {
              if (keep == "keep-alive" || keep == "Keep-Alive")
                conn.m_keepAlive = true;
              if (keep == "close" || keep == "Close") conn.m_keepAlive = false;
            } else if (conn.m_request.version == "HTTP/1.1") {
              conn.m_keepAlive = true;
            }
            std::string ctype;
            hasHeader(conn.m_request, "Content-Type", ctype);
            std::cerr << "[POST] uri=" << conn.m_request.uri << " ctype='"
                      << ctype << "' body_size=" << conn.m_request.body.size()
                      << "\n";
            std::string destDir =
                route->uploadPath.empty() ? route->root : route->uploadPath;
            ensureDir(destDir);
            std::string respBody = "Received POST (";
            char num[64];
            std::sprintf(num, "%lu", (unsigned long)conn.m_request.body.size());
            respBody += num;
            respBody += " bytes)\n";
            if (ctype.find("multipart/form-data") != std::string::npos) {
              std::string boundary;
              size_t bpos = ctype.find("boundary=");
              if (bpos != std::string::npos) {
                boundary = ctype.substr(bpos + 9);
                if (!boundary.empty() && boundary[0] == '"') {
                  size_t endq = boundary.find('"', 1);
                  if (endq != std::string::npos)
                    boundary = boundary.substr(1, endq - 1);
                }
              }
              if (!boundary.empty()) {
                std::vector<MultipartSavedFile> saved;
                if (parseMultipartFormData(conn.m_request.body, boundary,
                                           destDir, saved)) {
                  if (saved.empty())
                    respBody += "No file parts saved\n";
                  else {
                    for (size_t i = 0; i < saved.size(); ++i) {
                      respBody += "Saved field='";
                      respBody += saved[i].field;
                      respBody += "' -> ";
                      respBody += saved[i].filename;
                      respBody += " (";
                      char sz[32];
                      std::sprintf(sz, "%lu", (unsigned long)saved[i].size);
                      respBody += sz;
                      respBody += ")\n";
                    }
                  }
                } else {
                  respBody += "Multipart parse error\n";
                }
              } else {
                respBody += "Missing boundary parameter\n";
              }
            } else {
              static unsigned long uploadCounter = 0;
              ++uploadCounter;
              char fname[64];
              std::sprintf(fname, "upload_%lu.bin", uploadCounter);
              std::string full = destDir;
              if (full[full.size() - 1] != '/') full += '/';
              full += fname;
              FILE *wf = std::fopen(full.c_str(), "wb");
              if (wf) {
                if (!conn.m_request.body.empty())
                  std::fwrite(conn.m_request.body.data(), 1,
                              conn.m_request.body.size(), wf);
                std::fclose(wf);
                respBody += "Stored raw body as ";
                respBody += full;
                respBody += "\n";
              }
            }
            conn.m_writeBuf = buildResponse(200, "OK", respBody, "text/plain",
                                            conn.m_keepAlive, false);
            conn.m_bodyComplete = true;
            conn.m_phase = ClientConnection::kPhaseRespond;
          } else if (isDir(filePath)) {
            if (route->directoryListing) {
              if (listDir(filePath, body)) {
                std::string keep;
                conn.m_keepAlive = false;
                if (hasHeader(conn.m_request, "Connection", keep)) {
                  if (keep == "keep-alive" || keep == "Keep-Alive")
                    conn.m_keepAlive = true;
                  if (keep == "close" || keep == "Close")
                    conn.m_keepAlive = false;
                } else if (conn.m_request.version == "HTTP/1.1") {
                  conn.m_keepAlive = true;
                }
                conn.m_writeBuf = buildResponse(
                    200, "OK", body, "text/html", conn.m_keepAlive,
                    conn.m_request.method == "HEAD");
                conn.m_phase = ClientConnection::kPhaseRespond;
                std::cerr << "[200] dir listing uri=" << conn.m_request.uri
                          << (conn.m_keepAlive ? " keep-alive" : " close")
                          << "\n";
              } else {
                conn.m_keepAlive = false;
                std::string body500 =
                    loadErrorPageBody(sc, 500, "500 Internal Server Error\n");
                conn.m_writeBuf = buildResponse(
                    500, "Internal Server Error", body500, "text/plain", false,
                    conn.m_request.method == "HEAD");
                conn.m_phase = ClientConnection::kPhaseRespond;
              }
            } else {
              conn.m_keepAlive = false;
              std::string body403 =
                  loadErrorPageBody(sc, 403, "403 Forbidden\n");
              conn.m_writeBuf =
                  buildResponse(403, "Forbidden", body403, "text/plain", false,
                                conn.m_request.method == "HEAD");
              conn.m_phase = ClientConnection::kPhaseRespond;
            }
          } else if (conn.m_request.method == "DELETE") {
            // Handle deletion of file
            struct stat st;
            if (::stat(filePath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
              if (::unlink(filePath.c_str()) == 0) {
                std::string keep;
                conn.m_keepAlive = false;
                if (hasHeader(conn.m_request, "Connection", keep)) {
                  if (keep == "keep-alive" || keep == "Keep-Alive")
                    conn.m_keepAlive = true;
                  if (keep == "close" || keep == "Close")
                    conn.m_keepAlive = false;
                } else if (conn.m_request.version == "HTTP/1.1") {
                  conn.m_keepAlive = true;
                }
                conn.m_writeBuf =
                    buildResponse(204, "No Content", "", "text/plain",
                                  conn.m_keepAlive, false);
                conn.m_phase = ClientConnection::kPhaseRespond;
                std::cerr << "[204] deleted uri=" << conn.m_request.uri << "\n";
              } else {
                conn.m_keepAlive = false;
                std::string body500 =
                    loadErrorPageBody(sc, 500, "500 Internal Server Error\n");
                conn.m_writeBuf =
                    buildResponse(500, "Internal Server Error", body500,
                                  "text/plain", false, false);
                conn.m_phase = ClientConnection::kPhaseRespond;
                std::cerr << "[500] delete failed uri=" << conn.m_request.uri
                          << " errno=" << errno << "\n";
              }
            } else if (isDir(filePath)) {
              conn.m_keepAlive = false;
              std::string body403 =
                  loadErrorPageBody(sc, 403, "403 Forbidden\n");
              conn.m_writeBuf = buildResponse(403, "Forbidden", body403,
                                              "text/plain", false, false);
              conn.m_phase = ClientConnection::kPhaseRespond;
            } else {
              conn.m_keepAlive = false;
              std::string body404f =
                  loadErrorPageBody(sc, 404, "404 Not Found\n");
              conn.m_writeBuf = buildResponse(404, "Not Found", body404f,
                                              "text/plain", false, false);
              conn.m_phase = ClientConnection::kPhaseRespond;
            }
          } else if (readFile(filePath, body)) {
            std::string keep;
            conn.m_keepAlive = false;
            if (hasHeader(conn.m_request, "Connection", keep)) {
              if (keep == "keep-alive" || keep == "Keep-Alive")
                conn.m_keepAlive = true;
              if (keep == "close" || keep == "Close") conn.m_keepAlive = false;
            } else if (conn.m_request.version == "HTTP/1.1") {
              conn.m_keepAlive =
                  true;  // default for 1.1 unless close specified
            }
            if (conn.m_request.method == "GET" ||
                conn.m_request.method == "HEAD") {
              conn.m_writeBuf = buildResponse(
                  200, "OK", body, guessType(filePath), conn.m_keepAlive,
                  conn.m_request.method == "HEAD");
              std::cerr << "[200] uri=" << conn.m_request.uri
                        << " size=" << body.size()
                        << (conn.m_keepAlive ? " keep-alive" : " close")
                        << "\n";
              conn.m_bodyComplete = true;
              conn.m_phase = ClientConnection::kPhaseRespond;
            } else if (conn.m_request.method == "POST") {
              std::string respBody = "Received POST (";
              char num[64];
              std::sprintf(num, "%lu",
                           (unsigned long)conn.m_request.body.size());
              respBody += num;
              respBody += " bytes)\n";
              if (route->uploadsEnabled && !route->uploadPath.empty()) {
                // naive unique name
                std::string base = route->uploadPath;
                if (!base.empty() && base[base.size() - 1] != '/') base += "/";
                static unsigned long uploadCounter = 0;  // primitive counter
                ++uploadCounter;
                char fname[128];
                std::sprintf(fname, "upload_%lu.dat", uploadCounter);
                std::string full = base + fname;
                FILE *wf = std::fopen(full.c_str(), "wb");
                if (wf) {
                  if (!conn.m_request.body.empty())
                    std::fwrite(conn.m_request.body.data(), 1,
                                conn.m_request.body.size(), wf);
                  std::fclose(wf);
                  respBody += "Stored as ";
                  respBody += fname;
                  respBody += "\n";
                  std::cerr << "[UPLOAD] saved " << full
                            << " size=" << conn.m_request.body.size() << "\n";
                } else {
                  respBody += "Upload save failed errno=";
                  respBody += std::strerror(errno);
                  respBody += "\n";
                  std::cerr << "[UPLOAD-ERR] path=" << full
                            << " errno=" << errno << "\n";
                }
              }
              conn.m_writeBuf = buildResponse(200, "OK", respBody, "text/plain",
                                              conn.m_keepAlive, false);
              conn.m_phase = ClientConnection::kPhaseRespond;
            } else if (conn.m_request.method == "DELETE") {
              // Not implemented deletion semantics yet
              conn.m_keepAlive = false;
              std::string body501 =
                  loadErrorPageBody(sc, 501, "501 Not Implemented\n");
              conn.m_writeBuf = buildResponse(501, "Not Implemented", body501,
                                              "text/plain", false, false);
              conn.m_phase = ClientConnection::kPhaseRespond;
            } else {
              conn.m_keepAlive = false;
              std::string body405 =
                  loadErrorPageBody(sc, 405, "405 Method Not Allowed\n");
              conn.m_writeBuf =
                  buildResponse(405, "Method Not Allowed", body405,
                                "text/plain", false, false);
              conn.m_phase = ClientConnection::kPhaseRespond;
            }
          } else {
            conn.m_keepAlive = false;
            std::string body404g =
                loadErrorPageBody(sc, 404, "404 Not Found\n");
            conn.m_writeBuf = buildResponse(404, "Not Found", body404g,
                                            "text/plain", conn.m_keepAlive,
                                            conn.m_request.method == "HEAD");
            std::cerr << "[404] file=" << filePath << "\n";
            conn.m_phase = ClientConnection::kPhaseRespond;
          }
        }
      }
      conn.m_wantWrite = true;
      break;
    }
  }
}

void Server::HandleWritable(ClientConnection &conn) {
  while (!conn.m_writeBuf.empty()) {
    ssize_t n = ::send(conn.m_fd.Get(), conn.m_writeBuf.data(),
                       conn.m_writeBuf.size(), 0);
    if (n <= 0) break;
    conn.m_writeBuf.erase(0, n);
  }
  if (conn.m_writeBuf.empty()) {
    if (!conn.m_keepAlive || conn.m_phase == ClientConnection::kPhaseClosing) {
      CloseConnection(conn.m_fd.Get());
      return;
    }
    // Remove consumed bytes in case of pipelining
    size_t consumed = conn.m_parser.Consumed();
    if (consumed && consumed <= conn.m_readBuf.size()) {
      conn.m_readBuf.erase(0, consumed);
    } else {
      conn.m_readBuf.clear();
    }
    conn.m_wantWrite = false;
    conn.m_request = HttpRequest();
    conn.m_parser.Reset();
    conn.m_keepAlive = false;  // will be set by next response
    conn.m_phase = ClientConnection::kPhaseIdle;
  }
}

void Server::CloseConnection(int fd) {
  std::map<int, ClientConnection>::iterator it = m_clients.find(fd);
  if (it != m_clients.end()) {
    m_clients.erase(it);
  }
}

void Server::BuildPollFds(std::vector<struct pollfd> &pfds) {
  pfds.clear();
  for (size_t i = 0; i < m_listenSockets.size(); ++i) {
    struct pollfd p;
    p.fd = m_listenSockets[i].Get();
    p.events = POLLIN;
    p.revents = 0;
    pfds.push_back(p);
  }
  std::map<int, ClientConnection>::iterator it = m_clients.begin();
  for (; it != m_clients.end(); ++it) {
    struct pollfd p;
    p.fd = it->first;
    p.events = POLLIN;
    if (it->second.m_wantWrite) p.events |= POLLOUT;
    p.revents = 0;
    pfds.push_back(p);
    if (it->second.m_cgiActive) {
      if (it->second.m_cgiInFd >= 0) {
        struct pollfd pc;
        pc.fd = it->second.m_cgiInFd;
        pc.events = POLLOUT;
        pc.revents = 0;
        pfds.push_back(pc);
      }
      if (it->second.m_cgiOutFd >= 0) {
        struct pollfd pr;
        pr.fd = it->second.m_cgiOutFd;
        pr.events = POLLIN;
        pr.revents = 0;
        pfds.push_back(pr);
      }
    }
  }
}

void Server::Shutdown() {
  m_clients.clear();
  m_listenSockets.clear();
}

bool Server::MaybeStartCgi(ClientConnection &conn, const RouteConfig &route,
                           const std::string &filePath) {
  int inPipe[2];
  int outPipe[2];
  if (::pipe(inPipe) < 0) return false;
  if (::pipe(outPipe) < 0) {
    ::close(inPipe[0]);
    ::close(inPipe[1]);
    return false;
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(inPipe[0]);
    ::close(inPipe[1]);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    return false;
  }
  if (pid == 0) {
    // child
    ::dup2(inPipe[0], 0);   // stdin
    ::dup2(outPipe[1], 1);  // stdout
    ::close(inPipe[0]);
    ::close(inPipe[1]);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    // Change working directory to script's directory for relative includes
    std::string scriptDir = filePath;
    size_t slash = scriptDir.rfind('/');
    if (slash != std::string::npos) {
      scriptDir.erase(slash);
      if (!scriptDir.empty()) ::chdir(scriptDir.c_str());
    }
    // Derive PATH_INFO and QUERY_STRING from original URI
    std::string uri = conn.m_request.uri;
    std::string query;
    size_t qpos = uri.find('?');
    if (qpos != std::string::npos) {
      query = uri.substr(qpos + 1);
      uri = uri.substr(0, qpos);
    }
    // For PATH_INFO we supply the script path itself; more advanced splitting
    // could map script vs extra path
    std::string pathInfo = uri;
    // Content-Length & Type
    char lenBuf[32];
    std::sprintf(lenBuf, "%lu", (unsigned long)conn.m_request.body.size());
    std::string contentType;
    for (size_t i = 0; i < conn.m_request.headers.size(); ++i) {
      std::string n = conn.m_request.headers[i].name;
      for (size_t j = 0; j < n.size(); ++j) n[j] = (char)std::tolower(n[j]);
      if (n == "content-type") {
        contentType = conn.m_request.headers[i].value;
        break;
      }
    }
    // Build environment
    std::vector<std::string> envStrs;
    envStrs.push_back("REQUEST_METHOD=" + conn.m_request.method);
    envStrs.push_back("SCRIPT_FILENAME=" + filePath);
    envStrs.push_back("SCRIPT_NAME=" + filePath);
    envStrs.push_back("PATH_INFO=" + pathInfo);
    envStrs.push_back("QUERY_STRING=" + query);
    envStrs.push_back(std::string("CONTENT_LENGTH=") + lenBuf);
    if (!contentType.empty()) envStrs.push_back("CONTENT_TYPE=" + contentType);
    envStrs.push_back("GATEWAY_INTERFACE=CGI/1.1");
    envStrs.push_back("SERVER_PROTOCOL=HTTP/1.1");
    envStrs.push_back("REDIRECT_STATUS=200");  // for PHP
    // SERVER_NAME / PORT (best-effort)
    // SERVER_NAME / PORT from selected server config (best-effort)
    std::string serverName = "localhost";
    std::string serverPort = "80";
    if (conn.m_serverIndex >= 0 &&
        (size_t)conn.m_serverIndex < m_config.servers.size()) {
      const ServerConfig &scRef = m_config.servers[conn.m_serverIndex];
      if (!scRef.serverNames.empty())
        serverName = scRef.serverNames[0];
      else if (!scRef.host.empty())
        serverName = scRef.host;
      char pbuf[16];
      std::sprintf(pbuf, "%d", scRef.port);
      serverPort = pbuf;
    }
    envStrs.push_back("SERVER_NAME=" + serverName);
    envStrs.push_back("SERVER_PORT=" + serverPort);
    // Pass HTTP_* headers (basic sanitization)
    for (size_t i = 0; i < conn.m_request.headers.size(); ++i) {
      const std::string &hn = conn.m_request.headers[i].name;
      if (hn.empty()) continue;
      std::string key;
      key.reserve(hn.size() + 6);
      key = "HTTP_";
      for (size_t j = 0; j < hn.size(); ++j) {
        char c = hn[j];
        if (c == '-') c = '_';
        key += (char)std::toupper((unsigned char)c);
      }
      envStrs.push_back(key + "=" + conn.m_request.headers[i].value);
    }
    std::vector<char *> envp;
    for (size_t i = 0; i < envStrs.size(); ++i)
      envp.push_back(const_cast<char *>(envStrs[i].c_str()));
    envp.push_back(0);
    std::vector<char *> argv;
    std::string interpreter =
        route.cgiInterpreter.empty() ? "" : route.cgiInterpreter;
    if (!interpreter.empty())
      argv.push_back(const_cast<char *>(interpreter.c_str()));
    argv.push_back(const_cast<char *>(filePath.c_str()));
    argv.push_back(0);
    if (!interpreter.empty()) {
      ::execlp(interpreter.c_str(), interpreter.c_str(), filePath.c_str(),
               (char *)0);
    } else {
      ::execlp(filePath.c_str(), filePath.c_str(), (char *)0);
    }
    _exit(1);
  }
  // parent
  ::close(inPipe[0]);
  ::close(outPipe[1]);
  setNonBlocking(inPipe[1]);
  setNonBlocking(outPipe[0]);
  conn.m_cgiInFd = inPipe[1];
  conn.m_cgiOutFd = outPipe[0];
  conn.m_cgiPid = pid;
  conn.m_cgiActive = true;
  m_cgiFdToClient[conn.m_cgiInFd] = conn.m_fd.Get();
  m_cgiFdToClient[conn.m_cgiOutFd] = conn.m_fd.Get();
  return true;
}

bool Server::DriveCgiIO(ClientConnection &conn) {
  // write request body to CGI stdin
  if (conn.m_cgiInFd >= 0 &&
      conn.m_cgiWriteOffset < conn.m_request.body.size()) {
    ssize_t n = ::write(conn.m_cgiInFd,
                        conn.m_request.body.data() + conn.m_cgiWriteOffset,
                        conn.m_request.body.size() - conn.m_cgiWriteOffset);
    if (n > 0) conn.m_cgiWriteOffset += (size_t)n;
    if (n <= 0) { /* EAGAIN or done; do nothing now */
    }
    if (conn.m_cgiWriteOffset >= conn.m_request.body.size()) {
      ::close(conn.m_cgiInFd);
      m_cgiFdToClient.erase(conn.m_cgiInFd);
      conn.m_cgiInFd = -1;
    }
  }
  // read CGI stdout
  if (conn.m_cgiOutFd >= 0) {
    for (;;) {
      char buf[4096];
      ssize_t n = ::read(conn.m_cgiOutFd, buf, sizeof(buf));
      if (n > 0) conn.m_cgiBuffer.append(buf, n);
      if (n <= 0) break;
    }
  }
  // check if child exited
  int status = 0;
  pid_t r = ::waitpid(conn.m_cgiPid, &status, WNOHANG);
  if (r == conn.m_cgiPid) {
    if (conn.m_cgiOutFd >= 0) {
      ::close(conn.m_cgiOutFd);
      m_cgiFdToClient.erase(conn.m_cgiOutFd);
      conn.m_cgiOutFd = -1;
    }
    conn.m_cgiActive = false;
  }
  // If output available and not yet built response, parse headers
  if (!conn.m_cgiBuffer.empty() && !conn.m_cgiHeadersDone) {
    size_t pos = conn.m_cgiBuffer.find("\r\n\r\n");
    if (pos != std::string::npos) {
      conn.m_cgiHeadersDone = true;
      conn.m_cgiBodyStart = pos + 4;
      std::string headerBlock = conn.m_cgiBuffer.substr(0, pos);
      int code = 200;
      std::string reason = "OK";
      std::string contentType = "text/html";
      std::string connectionHdr;
      std::vector<std::pair<std::string, std::string> > passHeaders;
      size_t start = 0;
      while (start <= headerBlock.size()) {
        size_t end = headerBlock.find("\r\n", start);
        std::string line = headerBlock.substr(
            start, end == std::string::npos ? headerBlock.size() - start
                                            : end - start);
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
          std::string name = line.substr(0, colon);
          std::string value = line.substr(colon + 1);
          while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
            value.erase(0, 1);
          std::string lower = name;
          for (size_t i = 0; i < lower.size(); ++i)
            lower[i] = (char)std::tolower(lower[i]);
          if (lower == "status") {
            int c = atoi(value.c_str());
            if (c >= 100 && c <= 599) code = c;
            // optional reason phrase after code
            size_t sp = value.find(' ');
            if (sp != std::string::npos) {
              std::string r = value.substr(sp + 1);
              if (!r.empty()) reason = r;
            }
          } else if (lower == "content-type") {
            contentType = value;
          } else if (lower == "connection") {
            connectionHdr = value;
          } else if (lower == "content-length") {
            passHeaders.push_back(
                std::make_pair(name, value));  // preserve provided length
          } else {
            passHeaders.push_back(std::make_pair(name, value));
          }
        }
        if (end == std::string::npos)
          break;
        else
          start = end + 2;
      }
      std::string body = conn.m_cgiBuffer.substr(conn.m_cgiBodyStart);
      // Determine keep-alive
      if (connectionHdr.empty()) {
        // Derive from HTTP/1.1 default
        conn.m_keepAlive = true;
      } else {
        std::string lower = connectionHdr;
        for (size_t i = 0; i < lower.size(); ++i)
          lower[i] = (char)std::tolower(lower[i]);
        conn.m_keepAlive = (lower == "keep-alive");
      }
      // Build full response manually (not using buildResponse to allow header
      // passthrough)
      std::string resp = "HTTP/1.1 ";
      char codeBuf[8];
      std::sprintf(codeBuf, "%d", code);
      resp += codeBuf;
      resp += ' ';
      resp += reason;
      resp += "\r\n";
      bool haveCL = false;
      for (size_t i = 0; i < passHeaders.size(); ++i) {
        std::string n = passHeaders[i].first;
        std::string v = passHeaders[i].second;
        std::string lower = n;
        for (size_t j = 0; j < lower.size(); ++j)
          lower[j] = (char)std::tolower(lower[j]);
        if (lower == "content-length") haveCL = true;
        if (lower == "connection") continue;  // override
        resp += n;
        resp += ": ";
        resp += v;
        resp += "\r\n";
      }
      if (!haveCL) {
        char lenBuf2[32];
        std::sprintf(lenBuf2, "%lu", (unsigned long)body.size());
        resp += "Content-Length: ";
        resp += lenBuf2;
        resp += "\r\n";
      }
      // Ensure Content-Type present
      bool haveCT = false;
      for (size_t i = 0; i < passHeaders.size(); ++i) {
        std::string l = passHeaders[i].first;
        for (size_t j = 0; j < l.size(); ++j) l[j] = (char)std::tolower(l[j]);
        if (l == "content-type") {
          haveCT = true;
          break;
        }
      }
      if (!haveCT && !contentType.empty())
        resp += "Content-Type: " + contentType + "\r\n";
      resp += "Connection: ";
      resp += conn.m_keepAlive ? "keep-alive" : "close";
      resp += "\r\n\r\n";
      resp += body;
      conn.m_writeBuf = resp;
      conn.m_phase = ClientConnection::kPhaseRespond;
      conn.m_wantWrite = true;
      return true;
    }
  }
  // if child done and no headers, return error
  if (!conn.m_cgiActive && !conn.m_cgiHeadersDone) {
    conn.m_keepAlive = false;
    conn.m_writeBuf =
        buildResponse(500, "Internal Server Error", "CGI Execution Failed\n",
                      "text/plain", false, false);
    conn.m_phase = ClientConnection::kPhaseRespond;
    conn.m_wantWrite = true;
    return false;
  }
  return true;  // continue
}

void Server::ReapCgi(ClientConnection &conn) {
  if (conn.m_cgiInFd >= 0) {
    ::close(conn.m_cgiInFd);
    m_cgiFdToClient.erase(conn.m_cgiInFd);
    conn.m_cgiInFd = -1;
  }
  if (conn.m_cgiOutFd >= 0) {
    ::close(conn.m_cgiOutFd);
    m_cgiFdToClient.erase(conn.m_cgiOutFd);
    conn.m_cgiOutFd = -1;
  }
  if (conn.m_cgiPid > 0) {
    int st;
    ::waitpid(conn.m_cgiPid, &st, WNOHANG);
  }
  conn.m_cgiActive = false;
}

bool Server::HandleCgiEvent(int fd, short revents) {
  std::map<int, int>::iterator it = m_cgiFdToClient.find(fd);
  if (it == m_cgiFdToClient.end()) return true;
  int clientFd = it->second;
  std::map<int, ClientConnection>::iterator cit = m_clients.find(clientFd);
  if (cit == m_clients.end()) {
    ::close(fd);
    m_cgiFdToClient.erase(it);
    return true;
  }
  ClientConnection &conn = cit->second;
  if (!conn.m_cgiActive) return true;
  if (revents & (POLLHUP | POLLERR)) {
    // mark child likely done; drive IO then close
    DriveCgiIO(conn);
    if (conn.m_cgiOutFd == fd) {
      ::close(conn.m_cgiOutFd);
      m_cgiFdToClient.erase(fd);
      conn.m_cgiOutFd = -1;
    }
    if (conn.m_cgiInFd == fd) {
      ::close(conn.m_cgiInFd);
      m_cgiFdToClient.erase(fd);
      conn.m_cgiInFd = -1;
    }
    conn.m_cgiActive = false;
  }
  if (!DriveCgiIO(conn)) return false;
  return true;
}
