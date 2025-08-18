#include "server/Server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cctype>
#include <sys/types.h>
#include <fstream>
#include <dirent.h>
#include <ctime>
#include <cerrno>
#include <sys/stat.h>
#include <strings.h>

namespace {
static bool setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
  return true;
}
}

// forward declaration for static helper used in timeout sweep
static std::string buildResponse(int code, const std::string &reason, const std::string &body, const char *ctype, bool keepAlive, bool headOnly);
static std::string buildRedirect(int code, const std::string &reason, const std::string &location, bool keepAlive);
static std::string loadErrorPageBody(const ServerConfig &sc, int code, const std::string &fallback);

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
  int dyn = computePollTimeout();
  if (dyn >= 0 && (timeoutMs < 0 || dyn < timeoutMs)) timeoutMs = dyn;
  int ret = ::poll(&pfds_[0], pfds_.size(), timeoutMs);
  if (ret < 0) {
    std::perror("poll");
    return false;
  }
  return true;
}

int Server::computePollTimeout() const {
  if (clients_.empty()) return -1;
  unsigned long nowMs = (unsigned long)std::time(0) * 1000UL;
  long best = -1;
  for (std::map<int, ClientConnection>::const_iterator it = clients_.begin(); it != clients_.end(); ++it) {
    const ClientConnection &c = it->second;
    unsigned long deadline = 0;
    if (!c.headersComplete) {
      // Can't know virtual host yet; use first server's header timeout
      const ServerConfig &scHdr = config_.servers[0];
      deadline = c.createdAtMs + (unsigned long)scHdr.headerTimeoutMs;
    } else {
      const ServerConfig &scRef = (c.serverIndex >= 0 && (size_t)c.serverIndex < config_.servers.size()) ? config_.servers[c.serverIndex] : config_.servers[0];
      if (!c.bodyComplete) deadline = c.lastActivityMs + (unsigned long)scRef.bodyTimeoutMs;
      else if (c.keepAlive) deadline = c.lastActivityMs + (unsigned long)scRef.idleTimeoutMs;
    }
    if (deadline) {
      long remain = (long)deadline - (long)nowMs;
      if (remain < 0) remain = 0;
      if (best < 0 || remain < best) best = remain;
    }
  }
  return (int)(best);
}

void Server::processEvents() {
  // Sweep for timeouts before handling events
  unsigned long nowMs = (unsigned long)std::time(0) * 1000UL;
  std::map<int, ClientConnection>::iterator itSweep = clients_.begin();
  while (itSweep != clients_.end()) {
    ClientConnection &c = itSweep->second;
    bool closeIt = false;
    // CGI timeout check
    if (c.cgiActive && c.serverIndex >= 0 && (size_t)c.serverIndex < config_.servers.size()) {
      const ServerConfig &scSrv = config_.servers[c.serverIndex];
      if (scSrv.cgiTimeoutMs > 0 && c.cgiStartMs > 0) {
        unsigned long nowMsLocal = nowMs;
        if (nowMsLocal - c.cgiStartMs > (unsigned long)scSrv.cgiTimeoutMs) {
          std::cerr << "[cgi-timeout] pid=" << c.cgiPid << " fd=" << itSweep->first << "\n";
          if (c.cgiPid > 0) ::kill(c.cgiPid, SIGKILL);
            reapCgi(c);
            c.keepAlive = false;
            c.writeBuf = buildResponse(504, "Gateway Timeout", "504 Gateway Timeout (CGI)\n", "text/plain", false, false);
            c.phase = ClientConnection::PH_RESPOND; c.wantWrite = true;
        }
      }
    }
    // timeouts (use per-virtual-host once known)
    if (!c.headersComplete) {
      const ServerConfig &scHdr = config_.servers[0];
      if (scHdr.headerTimeoutMs > 0 && nowMs - c.createdAtMs > (unsigned long)scHdr.headerTimeoutMs) closeIt = true;
    } else {
      const ServerConfig &scRef = (c.serverIndex >= 0 && (size_t)c.serverIndex < config_.servers.size()) ? config_.servers[c.serverIndex] : config_.servers[0];
      if (!c.bodyComplete) {
        if (scRef.bodyTimeoutMs > 0 && nowMs - c.lastActivityMs > (unsigned long)scRef.bodyTimeoutMs) closeIt = true;
      } else if (c.keepAlive && scRef.idleTimeoutMs > 0 && nowMs - c.lastActivityMs > (unsigned long)scRef.idleTimeoutMs) {
        closeIt = true;
      }
    }
    if (closeIt) {
      int fd = itSweep->first;
      if (!c.headersComplete || !c.bodyComplete) {
        std::cerr << "[timeout] fd=" << fd << " sending 408\n";
        if (c.writeBuf.empty()) {
          c.writeBuf = buildResponse(408, "Request Timeout", "408 Request Timeout\n", "text/plain", false, false);
          c.wantWrite = true;
        }
      } else {
        std::cerr << "[idle-timeout] fd=" << fd << " closing keep-alive\n";
      }
      c.keepAlive = false;
      c.phase = ClientConnection::PH_CLOSING;
      ++itSweep; // leave connection to flush
    } else {
      ++itSweep;
    }
  }
  for (size_t i = 0; i < pfds_.size(); ++i) {
    struct pollfd &p = pfds_[i];
    if (!p.revents) continue;
    bool isListen = false;
    for (size_t j = 0; j < listenSockets_.size(); ++j) {
      if (listenSockets_[j].get() == p.fd) { isListen = true; break; }
    }
    // Check CGI fds first
    if (!isListen) {
      if (cgiFdToClient_.find(p.fd) != cgiFdToClient_.end()) {
        handleCgiEvent(p.fd, p.revents);
        continue;
      }
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
  unsigned long nowMs = (unsigned long)(std::time(0)) * 1000UL; // coarse
  conn.createdAtMs = nowMs;
  conn.lastActivityMs = nowMs;
  conn.headersComplete = false;
  conn.bodyComplete = false;
  conn.phase = ClientConnection::PH_ACCEPTED;
    clients_[cfd] = conn;
  std::cerr << "[accept] fd=" << cfd << " total_clients=" << clients_.size() << "\n";
  }
}

static std::string buildResponse(int code, const std::string &reason, const std::string &body, const char *ctype, bool keepAlive, bool headOnly) {
  std::string resp = "HTTP/1.1 ";
  char codeBuf[8]; std::sprintf(codeBuf, "%d", code);
  resp += codeBuf; resp += ' '; resp += reason; resp += "\r\n";
  char len[32]; std::sprintf(len, "%lu", (unsigned long)body.size());
  resp += "Content-Length: "; resp += len; resp += "\r\n";
  resp += "Content-Type: "; resp += ctype; resp += "\r\n";
  resp += "Connection: "; resp += keepAlive ? "keep-alive" : "close"; resp += "\r\n\r\n";
  if (!headOnly) resp += body;
  return resp;
}

static std::string buildRedirect(int code, const std::string &reason, const std::string &location, bool keepAlive) {
  std::string body = "<html><body><h1>" + reason + "</h1><a href='" + location + "'>" + location + "</a></body></html>";
  std::string resp = "HTTP/1.1 ";
  char codeBuf[8]; std::sprintf(codeBuf, "%d", code);
  resp += codeBuf; resp += ' '; resp += reason; resp += "\r\n";
  resp += "Location: "; resp += location; resp += "\r\n";
  char len[32]; std::sprintf(len, "%lu", (unsigned long)body.size());
  resp += "Content-Length: "; resp += len; resp += "\r\nContent-Type: text/html\r\n";
  resp += "Connection: "; resp += keepAlive?"keep-alive":"close"; resp += "\r\n\r\n";
  resp += body;
  return resp;
}

static std::string loadErrorPageBody(const ServerConfig &sc, int code, const std::string &fallback) {
  if (sc.errorPageRoot.empty()) return fallback;
  char fname[16]; std::sprintf(fname, "%d.html", code);
  std::string path = sc.errorPageRoot; if (!path.empty() && path[path.size()-1] != '/') path += '/'; path += fname;
  std::string contents;
  std::ifstream ifs(path.c_str());
  if (ifs) {
    std::string tmp; char buf[1024];
    while (ifs.good()) { ifs.read(buf, sizeof(buf)); std::streamsize n = ifs.gcount(); if (n>0) tmp.append(buf, (size_t)n); }
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
  struct stat st; if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode); return false;
}

static bool listDir(const std::string &path, std::string &out) {
  DIR *d = ::opendir(path.c_str());
  if (!d) return false;
  std::string body = "<html><body><h1>Index of "; body += path; body += "</h1><ul>";
  struct dirent *ent;
  while ((ent = ::readdir(d))) {
    const char *name = ent->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
    body += "<li><a href=\""; body += name; body += "\">"; body += name; body += "</a></li>";
  }
  ::closedir(d);
  body += "</ul></body></html>";
  out.swap(body);
  return true;
}

static const RouteConfig *matchRoute(const ServerConfig &sc, const std::string &uri) {
  size_t bestLen = 0; const RouteConfig *best = 0;
  for (size_t i = 0; i < sc.routes.size(); ++i) {
    const RouteConfig &r = sc.routes[i];
    if (uri.compare(0, r.path.size(), r.path) == 0) {
      if (r.path.size() > bestLen) { bestLen = r.path.size(); best = &r; }
    }
  }
  return best;
}

// Virtual host selection: pick server whose serverNames contains Host header (case-insensitive exact match); fallback to first.
static const ServerConfig &selectServer(const Config &cfg, const HttpRequest &req, size_t &idxOut) {
  std::string host;
  for (size_t i = 0; i < req.headers.size(); ++i) {
    std::string name = req.headers[i].name;
    for (size_t j = 0; j < name.size(); ++j) name[j] = (char)std::tolower(name[j]);
    if (name == "host") { host = req.headers[i].value; break; }
  }
  if (host.empty()) { idxOut = 0; return cfg.servers[0]; }
  size_t colon = host.rfind(':'); if (colon != std::string::npos) host = host.substr(0, colon);
  for (size_t i = 0; i < cfg.servers.size(); ++i) {
    const ServerConfig &sc = cfg.servers[i];
  for (size_t j = 0; j < sc.serverNames.size(); ++j) if (sc.serverNames[j] == host) { idxOut = i; return sc; }
  }
  idxOut = 0; return cfg.servers[0];
}

static const char *guessType(const std::string &path) {
  const char *dot = 0;
  for (size_t i = path.size(); i > 0; --i) {
    if (path[i-1] == '.') { dot = path.c_str() + i - 1; break; }
    if (path[i-1] == '/' || path[i-1] == '\\') break;
  }
  if (!dot) return "text/plain";
  if (!std::strcmp(dot, ".html") || !std::strcmp(dot, ".htm")) return "text/html";
  if (!std::strcmp(dot, ".css")) return "text/css";
  if (!std::strcmp(dot, ".js")) return "application/javascript";
  if (!std::strcmp(dot, ".png")) return "image/png";
  if (!std::strcmp(dot, ".jpg") || !std::strcmp(dot, ".jpeg")) return "image/jpeg";
  if (!std::strcmp(dot, ".gif")) return "image/gif";
  return "text/plain";
}

// Sanitize filename by stripping directory components and dangerous chars
static std::string sanitizeFilename(const std::string &in) {
  std::string name;
  // strip path components
  size_t start = 0;
  for (size_t i = 0; i < in.size(); ++i) if (in[i] == '/' || in[i] == '\\') start = i + 1;
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
    return; // exists but not dir
  }
  // attempt create (single level only)
  ::mkdir(path.c_str(), 0755);
}

struct MultipartSavedFile { std::string field; std::string filename; size_t size; };

static bool parseContentDisposition(const std::string &line, std::string &name, std::string &filename) {
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
      while (!key.empty() && (key[0]==' '||key[0]=='\t')) key.erase(0,1);
      while (!key.empty() && (key[key.size()-1]==' '||key[key.size()-1]=='\t')) key.erase(key.size()-1,1);
      if (!val.empty() && val[0]=='"' && val[val.size()-1]=='"' && val.size()>=2) val = val.substr(1, val.size()-2);
      if (key == "name") name = val;
      else if (key == "filename") filename = val;
    }
    p = q + 1;
  }
  return true;
}

static bool parseMultipartFormData(const std::string &body, const std::string &boundary, const std::string &uploadPath, std::vector<MultipartSavedFile> &saved) {
  std::string boundaryMarker = "--" + boundary;
  std::string finalMarker = boundaryMarker + "--";
  size_t cursor = 0;
  while (cursor < body.size()) {
    // find next boundary
    size_t b = body.find(boundaryMarker, cursor);
    if (b == std::string::npos) break;
    b += boundaryMarker.size();
    if (b + 2 <= body.size() && body.compare(b, 2, "--") == 0) break; // reached final boundary
    // expect CRLF after boundary
    if (b + 2 > body.size() || body[b] != '\r' || body[b+1] != '\n') { cursor = b; continue; }
    size_t headerStart = b + 2;
    size_t headerEnd = body.find("\r\n\r\n", headerStart);
    if (headerEnd == std::string::npos) break;
    std::string headers = body.substr(headerStart, headerEnd - headerStart);
    size_t dataStart = headerEnd + 4;
    size_t nextBoundary = body.find(boundaryMarker, dataStart);
    if (nextBoundary == std::string::npos) break;
    size_t dataEnd = nextBoundary;
    // trim CRLF before boundary
    if (dataEnd >= 2 && body[dataEnd-2]=='\r' && body[dataEnd-1]=='\n') dataEnd -= 2;
    std::string fieldName, fileName;
    size_t hp = 0;
    while (hp < headers.size()) {
      size_t he = headers.find("\r\n", hp);
      if (he == std::string::npos) he = headers.size();
      std::string line = headers.substr(hp, he - hp);
      std::string lower = line; for (size_t i=0;i<lower.size();++i) lower[i] = (char)std::tolower(lower[i]);
      if (lower.find("content-disposition:") == 0) parseContentDisposition(line, fieldName, fileName);
      hp = he + 2;
    }
    if (!fileName.empty()) {
      ensureDir(uploadPath);
      std::string safe = sanitizeFilename(fileName);
      std::string full = uploadPath; if (full[full.size()-1] != '/') full += '/'; full += safe;
      FILE *wf = std::fopen(full.c_str(), "wb");
      if (wf) { if (dataEnd > dataStart) std::fwrite(&body[dataStart],1,dataEnd-dataStart,wf); std::fclose(wf); MultipartSavedFile sf; sf.field=fieldName; sf.filename=full; sf.size=dataEnd-dataStart; saved.push_back(sf); }
    }
    cursor = nextBoundary;
  }
  return !saved.empty();
}

static bool hasHeader(const HttpRequest &req, const char *name, std::string &value) {
  for (size_t i = 0; i < req.headers.size(); ++i) {
    if (req.headers[i].name.size() == 0) continue;
    // case-insensitive compare
    const std::string &hn = req.headers[i].name;
    size_t j=0; for (; j<hn.size() && name[j]; ++j) {
      if (std::tolower(hn[j]) != std::tolower(name[j])) break;
    }
    if (name[j] == 0 && j == hn.size()) { value = req.headers[i].value; return true; }
  }
  return false;
}

void Server::handleReadable(ClientConnection &conn) {
  char buf[4096];
  for (;;) {
    ssize_t n = ::recv(conn.fd.get(), buf, sizeof(buf), 0);
    if (n <= 0) break; // EAGAIN or closed
    conn.readBuf.append(buf, n);
  conn.lastActivityMs = (unsigned long)std::time(0) * 1000UL;
    if (conn.readBuf.size() < 2048) {
      // lightweight debug
      if (conn.readBuf.find("POST /upload") != std::string::npos) {
        std::cerr << "[DBG] recv bytes=" << n << " total=" << conn.readBuf.size() << " first100='" << conn.readBuf.substr(0, (conn.readBuf.size()<100?conn.readBuf.size():100)) << "'\n";
      }
    }
  if (conn.parser.parse(conn.readBuf, conn.request) || conn.parser.error()) {
  // Future: if request requires CGI, transition to PH_HANDLE then spawn CGI before PH_RESPOND
  if (conn.parser.error()) {
  conn.keepAlive = false;
  const ServerConfig &scTmp = config_.servers[0];
  std::string bodyErr = loadErrorPageBody(scTmp, 400, "400 Bad Request\n");
  conn.writeBuf = buildResponse(400, "Bad Request", bodyErr, "text/plain", false, false);
  conn.phase = ClientConnection::PH_RESPOND;
  std::cerr << "[400] malformed request bytes=" << conn.readBuf.size() << "\n";
        conn.wantWrite = true;
        break;
      }
  conn.headersComplete = true; // we have at least parsed headers (parser only flips after full body though)
  if (conn.phase == ClientConnection::PH_ACCEPTED) conn.phase = ClientConnection::PH_HEADERS;
  size_t serverIdx = 0; const ServerConfig &sc = selectServer(config_, conn.request, serverIdx); conn.serverIndex = (int)serverIdx;
  if (conn.request.body.size() > sc.clientMaxBodySize) {
  conn.keepAlive = false;
  std::string body413 = loadErrorPageBody(sc, 413, "413 Payload Too Large\n");
  conn.writeBuf = buildResponse(413, "Payload Too Large", body413, "text/plain", false, false);
  conn.phase = ClientConnection::PH_RESPOND;
        std::cerr << "[413] body_size=" << conn.request.body.size() << " limit=" << sc.clientMaxBodySize << "\n";
        conn.wantWrite = true;
        break;
      }
      const RouteConfig *route = matchRoute(sc, conn.request.uri);
  if (!route) {
  conn.keepAlive = false;
  std::string body404 = loadErrorPageBody(sc, 404, "404 Not Found\n");
  conn.writeBuf = buildResponse(404, "Not Found", body404, "text/plain", conn.keepAlive, conn.request.method == "HEAD");
  conn.phase = ClientConnection::PH_RESPOND;
  std::cerr << "[404] uri=" << conn.request.uri << "\n";
      } else {
    if (!route->methods.empty()) {
          bool ok = false;
          for (size_t i = 0; i < route->methods.size(); ++i) if (route->methods[i] == conn.request.method) { ok = true; break; }
          if (!ok) {
            conn.keepAlive = false;
            conn.writeBuf = buildResponse(405, "Method Not Allowed", "405 Method Not Allowed\n", "text/plain", conn.keepAlive, conn.request.method == "HEAD");
            std::cerr << "[405] method=" << conn.request.method << " uri=" << conn.request.uri << "\n";
      conn.phase = ClientConnection::PH_RESPOND;
            conn.wantWrite = true; break;
          }
        }
        std::string rel = conn.request.uri.substr(route->path.size());
        if (rel.empty() || rel == "/") {
          if (!route->index.empty()) rel = "/" + route->index;
        }
        // Redirect handling
        if (!route->redirect.empty()) {
          conn.keepAlive = false; // simpler; could keep-alive later
          std::cerr << "[302] redirect uri=" << conn.request.uri << " -> " << route->redirect << "\n";
          conn.writeBuf = buildRedirect(302, "Found", route->redirect, conn.keepAlive);
          conn.phase = ClientConnection::PH_RESPOND; conn.bodyComplete = true; conn.wantWrite = true; break;
        }
        // Basic traversal guard
        if (rel.find("..") != std::string::npos) {
          conn.keepAlive = false;
          std::string body403 = loadErrorPageBody(sc, 403, "403 Forbidden\n");
          conn.writeBuf = buildResponse(403, "Forbidden", body403, "text/plain", conn.keepAlive, conn.request.method == "HEAD");
          conn.phase = ClientConnection::PH_RESPOND;
          std::cerr << "[403] traversal attempt uri=" << conn.request.uri << "\n";
        } else {
          std::string filePath = route->root + rel;
          // Decide if CGI based on extension match
          bool wantsCgi = false;
          if (!route->cgiExtension.empty() && filePath.size() >= route->cgiExtension.size()) {
            if (filePath.compare(filePath.size()-route->cgiExtension.size(), route->cgiExtension.size(), route->cgiExtension) == 0) wantsCgi = true;
          }
          std::string body;
          if (wantsCgi) {
            if (maybeStartCgi(conn, *route, filePath)) {
              conn.cgiStartMs = (unsigned long)std::time(0) * 1000UL;
              conn.phase = ClientConnection::PH_HANDLE;
              conn.wantWrite = false;
              std::cerr << "[CGI] started pid=" << conn.cgiPid << " script=" << filePath << "\n";
            } else {
              conn.keepAlive = false;
              std::string body500 = loadErrorPageBody(sc, 500, "500 Internal Server Error\n");
              conn.writeBuf = buildResponse(500, "Internal Server Error", body500, "text/plain", false, false);
              conn.phase = ClientConnection::PH_RESPOND; conn.wantWrite = true;
            }
          } else if (conn.request.method == "POST" && route->uploadsEnabled) {
            std::string keep; conn.keepAlive = false;
            if (hasHeader(conn.request, "Connection", keep)) {
              if (keep == "keep-alive" || keep == "Keep-Alive") conn.keepAlive = true;
              if (keep == "close" || keep == "Close") conn.keepAlive = false;
            } else if (conn.request.version == "HTTP/1.1") { conn.keepAlive = true; }
            std::string ctype; hasHeader(conn.request, "Content-Type", ctype);
            std::cerr << "[POST] uri=" << conn.request.uri << " ctype='" << ctype << "' body_size=" << conn.request.body.size() << "\n";
            std::string destDir = route->uploadPath.empty()?route->root:route->uploadPath;
            ensureDir(destDir);
            std::string respBody = "Received POST ("; char num[64]; std::sprintf(num, "%lu", (unsigned long)conn.request.body.size()); respBody += num; respBody += " bytes)\n";
            if (ctype.find("multipart/form-data") != std::string::npos) {
              std::string boundary; size_t bpos = ctype.find("boundary=");
              if (bpos != std::string::npos) { boundary = ctype.substr(bpos + 9); if (!boundary.empty() && boundary[0]=='"') { size_t endq = boundary.find('"',1); if (endq != std::string::npos) boundary = boundary.substr(1, endq-1); } }
              if (!boundary.empty()) {
                std::vector<MultipartSavedFile> saved;
                if (parseMultipartFormData(conn.request.body, boundary, destDir, saved)) {
                  if (saved.empty()) respBody += "No file parts saved\n"; else {
                    for (size_t i=0;i<saved.size(); ++i) {
                      respBody += "Saved field='"; respBody += saved[i].field; respBody += "' -> "; respBody += saved[i].filename; respBody += " ("; char sz[32]; std::sprintf(sz, "%lu", (unsigned long)saved[i].size); respBody += sz; respBody += ")\n";
                    }
                  }
                } else { respBody += "Multipart parse error\n"; }
              } else { respBody += "Missing boundary parameter\n"; }
            } else {
              static unsigned long uploadCounter = 0; ++uploadCounter; char fname[64]; std::sprintf(fname, "upload_%lu.bin", uploadCounter);
              std::string full = destDir; if (full[full.size()-1] != '/') full += '/'; full += fname;
              FILE *wf = std::fopen(full.c_str(), "wb"); if (wf) { if (!conn.request.body.empty()) std::fwrite(conn.request.body.data(),1,conn.request.body.size(),wf); std::fclose(wf); respBody += "Stored raw body as "; respBody += full; respBody += "\n"; }
            }
            conn.writeBuf = buildResponse(200, "OK", respBody, "text/plain", conn.keepAlive, false);
            conn.bodyComplete = true; conn.phase = ClientConnection::PH_RESPOND;
          } else if (isDir(filePath)) {
            if (route->directoryListing) {
              if (listDir(filePath, body)) {
                std::string keep; conn.keepAlive = false;
                if (hasHeader(conn.request, "Connection", keep)) {
                  if (keep == "keep-alive" || keep == "Keep-Alive") conn.keepAlive = true;
                  if (keep == "close" || keep == "Close") conn.keepAlive = false;
                } else if (conn.request.version == "HTTP/1.1") {
                  conn.keepAlive = true;
                }
                conn.writeBuf = buildResponse(200, "OK", body, "text/html", conn.keepAlive, conn.request.method == "HEAD");
                conn.phase = ClientConnection::PH_RESPOND;
                std::cerr << "[200] dir listing uri=" << conn.request.uri << (conn.keepAlive?" keep-alive":" close") << "\n";
              } else {
                conn.keepAlive = false;
                std::string body500 = loadErrorPageBody(sc, 500, "500 Internal Server Error\n");
                conn.writeBuf = buildResponse(500, "Internal Server Error", body500, "text/plain", false, conn.request.method == "HEAD");
                conn.phase = ClientConnection::PH_RESPOND;
              }
            } else {
              conn.keepAlive = false;
              std::string body403 = loadErrorPageBody(sc, 403, "403 Forbidden\n");
              conn.writeBuf = buildResponse(403, "Forbidden", body403, "text/plain", false, conn.request.method == "HEAD");
              conn.phase = ClientConnection::PH_RESPOND;
            }
          } else if (conn.request.method == "DELETE") {
            // Handle deletion of file
            struct stat st;
            if (::stat(filePath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
              if (::unlink(filePath.c_str()) == 0) {
                std::string keep; conn.keepAlive = false;
                if (hasHeader(conn.request, "Connection", keep)) {
                  if (keep == "keep-alive" || keep == "Keep-Alive") conn.keepAlive = true;
                  if (keep == "close" || keep == "Close") conn.keepAlive = false;
                } else if (conn.request.version == "HTTP/1.1") { conn.keepAlive = true; }
                conn.writeBuf = buildResponse(204, "No Content", "", "text/plain", conn.keepAlive, false);
                conn.phase = ClientConnection::PH_RESPOND;
                std::cerr << "[204] deleted uri=" << conn.request.uri << "\n";
              } else {
                conn.keepAlive = false;
                std::string body500 = loadErrorPageBody(sc, 500, "500 Internal Server Error\n");
                conn.writeBuf = buildResponse(500, "Internal Server Error", body500, "text/plain", false, false);
                conn.phase = ClientConnection::PH_RESPOND;
                std::cerr << "[500] delete failed uri=" << conn.request.uri << " errno=" << errno << "\n";
              }
            } else if (isDir(filePath)) {
              conn.keepAlive = false;
              std::string body403 = loadErrorPageBody(sc, 403, "403 Forbidden\n");
              conn.writeBuf = buildResponse(403, "Forbidden", body403, "text/plain", false, false);
              conn.phase = ClientConnection::PH_RESPOND;
            } else {
              conn.keepAlive = false;
              std::string body404f = loadErrorPageBody(sc, 404, "404 Not Found\n");
              conn.writeBuf = buildResponse(404, "Not Found", body404f, "text/plain", false, false);
              conn.phase = ClientConnection::PH_RESPOND;
            }
          } else if (readFile(filePath, body)) {
            std::string keep; conn.keepAlive = false;
            if (hasHeader(conn.request, "Connection", keep)) {
              if (keep == "keep-alive" || keep == "Keep-Alive") conn.keepAlive = true;
              if (keep == "close" || keep == "Close") conn.keepAlive = false;
            } else if (conn.request.version == "HTTP/1.1") {
              conn.keepAlive = true; // default for 1.1 unless close specified
            }
            if (conn.request.method == "GET" || conn.request.method == "HEAD") {
              conn.writeBuf = buildResponse(200, "OK", body, guessType(filePath), conn.keepAlive, conn.request.method == "HEAD");
              std::cerr << "[200] uri=" << conn.request.uri << " size=" << body.size() << (conn.keepAlive?" keep-alive":" close") << "\n";
              conn.bodyComplete = true; conn.phase = ClientConnection::PH_RESPOND;
            } else if (conn.request.method == "POST") {
              std::string respBody = "Received POST (";
              char num[64]; std::sprintf(num, "%lu", (unsigned long)conn.request.body.size());
              respBody += num; respBody += " bytes)\n";
              if (route->uploadsEnabled && !route->uploadPath.empty()) {
                // naive unique name
                std::string base = route->uploadPath;
                if (!base.empty() && base[base.size()-1] != '/') base += "/";
                static unsigned long uploadCounter = 0; // primitive counter
                ++uploadCounter;
                char fname[128]; std::sprintf(fname, "upload_%lu.dat", uploadCounter);
                std::string full = base + fname;
                FILE *wf = std::fopen(full.c_str(), "wb");
                if (wf) {
                  if (!conn.request.body.empty()) std::fwrite(conn.request.body.data(), 1, conn.request.body.size(), wf);
                  std::fclose(wf);
                  respBody += "Stored as "; respBody += fname; respBody += "\n";
                  std::cerr << "[UPLOAD] saved " << full << " size=" << conn.request.body.size() << "\n";
                } else {
                  respBody += "Upload save failed errno="; respBody += std::strerror(errno); respBody += "\n";
                  std::cerr << "[UPLOAD-ERR] path=" << full << " errno=" << errno << "\n";
                }
              }
              conn.writeBuf = buildResponse(200, "OK", respBody, "text/plain", conn.keepAlive, false);
              conn.phase = ClientConnection::PH_RESPOND;
            } else if (conn.request.method == "DELETE") {
              // Not implemented deletion semantics yet
              conn.keepAlive = false;
              std::string body501 = loadErrorPageBody(sc, 501, "501 Not Implemented\n");
              conn.writeBuf = buildResponse(501, "Not Implemented", body501, "text/plain", false, false);
              conn.phase = ClientConnection::PH_RESPOND;
            } else {
              conn.keepAlive = false;
              std::string body405 = loadErrorPageBody(sc, 405, "405 Method Not Allowed\n");
              conn.writeBuf = buildResponse(405, "Method Not Allowed", body405, "text/plain", false, false);
              conn.phase = ClientConnection::PH_RESPOND;
            }
          } else {
            conn.keepAlive = false;
            std::string body404g = loadErrorPageBody(sc, 404, "404 Not Found\n");
            conn.writeBuf = buildResponse(404, "Not Found", body404g, "text/plain", conn.keepAlive, conn.request.method == "HEAD");
            std::cerr << "[404] file=" << filePath << "\n";
            conn.phase = ClientConnection::PH_RESPOND;
          }
        }
      }
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
    if (!conn.keepAlive || conn.phase == ClientConnection::PH_CLOSING) {
      closeConnection(conn.fd.get());
      return;
    }
    // Remove consumed bytes in case of pipelining
    size_t consumed = conn.parser.consumed();
    if (consumed && consumed <= conn.readBuf.size()) {
      conn.readBuf.erase(0, consumed);
    } else {
      conn.readBuf.clear();
    }
    conn.wantWrite = false;
    conn.request = HttpRequest();
    conn.parser.reset();
  conn.keepAlive = false; // will be set by next response
  conn.phase = ClientConnection::PH_IDLE;
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
    if (it->second.cgiActive) {
      if (it->second.cgiInFd >= 0) { struct pollfd pc; pc.fd = it->second.cgiInFd; pc.events = POLLOUT; pc.revents = 0; pfds.push_back(pc); }
      if (it->second.cgiOutFd >= 0) { struct pollfd pr; pr.fd = it->second.cgiOutFd; pr.events = POLLIN; pr.revents = 0; pfds.push_back(pr); }
    }
  }
}

void Server::shutdown() {
  clients_.clear();
  listenSockets_.clear();
}

bool Server::maybeStartCgi(ClientConnection &conn, const RouteConfig &route, const std::string &filePath) {
  int inPipe[2]; int outPipe[2];
  if (::pipe(inPipe) < 0) return false;
  if (::pipe(outPipe) < 0) { ::close(inPipe[0]); ::close(inPipe[1]); return false; }
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(inPipe[0]); ::close(inPipe[1]); ::close(outPipe[0]); ::close(outPipe[1]);
    return false;
  }
  if (pid == 0) {
    // child
    ::dup2(inPipe[0], 0); // stdin
    ::dup2(outPipe[1], 1); // stdout
    ::close(inPipe[0]); ::close(inPipe[1]);
    ::close(outPipe[0]); ::close(outPipe[1]);
    // Change working directory to script's directory for relative includes
    std::string scriptDir = filePath;
    size_t slash = scriptDir.rfind('/');
    if (slash != std::string::npos) {
      scriptDir.erase(slash);
      if (!scriptDir.empty()) ::chdir(scriptDir.c_str());
    }
    // Derive PATH_INFO and QUERY_STRING from original URI
    std::string uri = conn.request.uri;
    std::string query;
    size_t qpos = uri.find('?');
    if (qpos != std::string::npos) { query = uri.substr(qpos + 1); uri = uri.substr(0, qpos); }
    // For PATH_INFO we supply the script path itself; more advanced splitting could map script vs extra path
    std::string pathInfo = uri;
    // Content-Length & Type
    char lenBuf[32]; std::sprintf(lenBuf, "%lu", (unsigned long)conn.request.body.size());
    std::string contentType;
    for (size_t i=0;i<conn.request.headers.size();++i) {
      std::string n = conn.request.headers[i].name; for (size_t j=0;j<n.size();++j) n[j]=(char)std::tolower(n[j]);
      if (n == "content-type") { contentType = conn.request.headers[i].value; break; }
    }
    // Build environment
    std::vector<std::string> envStrs;
    envStrs.push_back("REQUEST_METHOD=" + conn.request.method);
    envStrs.push_back("SCRIPT_FILENAME=" + filePath);
    envStrs.push_back("SCRIPT_NAME=" + filePath);
    envStrs.push_back("PATH_INFO=" + pathInfo);
    envStrs.push_back("QUERY_STRING=" + query);
    envStrs.push_back(std::string("CONTENT_LENGTH=") + lenBuf);
    if (!contentType.empty()) envStrs.push_back("CONTENT_TYPE=" + contentType);
    envStrs.push_back("GATEWAY_INTERFACE=CGI/1.1");
    envStrs.push_back("SERVER_PROTOCOL=HTTP/1.1");
    envStrs.push_back("REDIRECT_STATUS=200"); // for PHP
    // SERVER_NAME / PORT (best-effort)
    // SERVER_NAME / PORT from selected server config (best-effort)
    std::string serverName = "localhost";
    std::string serverPort = "80";
    if (conn.serverIndex >= 0 && (size_t)conn.serverIndex < config_.servers.size()) {
      const ServerConfig &scRef = config_.servers[conn.serverIndex];
      if (!scRef.serverNames.empty()) serverName = scRef.serverNames[0];
      else if (!scRef.host.empty()) serverName = scRef.host;
      char pbuf[16]; std::sprintf(pbuf, "%d", scRef.port); serverPort = pbuf;
    }
    envStrs.push_back("SERVER_NAME=" + serverName);
    envStrs.push_back("SERVER_PORT=" + serverPort);
    // Pass HTTP_* headers (basic sanitization)
    for (size_t i=0;i<conn.request.headers.size(); ++i) {
      const std::string &hn = conn.request.headers[i].name;
      if (hn.empty()) continue;
      std::string key; key.reserve(hn.size()+6);
      key = "HTTP_";
      for (size_t j=0;j<hn.size(); ++j) {
        char c = hn[j];
        if (c == '-') c = '_';
        key += (char)std::toupper((unsigned char)c);
      }
      envStrs.push_back(key + "=" + conn.request.headers[i].value);
    }
    std::vector<char*> envp; for (size_t i=0;i<envStrs.size();++i) envp.push_back(const_cast<char*>(envStrs[i].c_str())); envp.push_back(0);
    std::vector<char*> argv;
    std::string interpreter = route.cgiInterpreter.empty()?"":route.cgiInterpreter;
    if (!interpreter.empty()) argv.push_back(const_cast<char*>(interpreter.c_str()));
    argv.push_back(const_cast<char*>(filePath.c_str()));
    argv.push_back(0);
    if (!interpreter.empty()) {
      ::execlp(interpreter.c_str(), interpreter.c_str(), filePath.c_str(), (char*)0);
    } else {
      ::execlp(filePath.c_str(), filePath.c_str(), (char*)0);
    }
    _exit(1);
  }
  // parent
  ::close(inPipe[0]); ::close(outPipe[1]);
  setNonBlocking(inPipe[1]); setNonBlocking(outPipe[0]);
  conn.cgiInFd = inPipe[1];
  conn.cgiOutFd = outPipe[0];
  conn.cgiPid = pid;
  conn.cgiActive = true;
  cgiFdToClient_[conn.cgiInFd] = conn.fd.get();
  cgiFdToClient_[conn.cgiOutFd] = conn.fd.get();
  return true;
}

bool Server::driveCgiIO(ClientConnection &conn) {
  // write request body to CGI stdin
  if (conn.cgiInFd >= 0 && conn.cgiWriteOffset < conn.request.body.size()) {
    ssize_t n = ::write(conn.cgiInFd, conn.request.body.data()+conn.cgiWriteOffset, conn.request.body.size()-conn.cgiWriteOffset);
    if (n > 0) conn.cgiWriteOffset += (size_t)n;
    if (n <= 0) { /* EAGAIN or done; do nothing now */ }
    if (conn.cgiWriteOffset >= conn.request.body.size()) { ::close(conn.cgiInFd); cgiFdToClient_.erase(conn.cgiInFd); conn.cgiInFd = -1; }
  }
  // read CGI stdout
  if (conn.cgiOutFd >= 0) {
    for (;;) {
      char buf[4096]; ssize_t n = ::read(conn.cgiOutFd, buf, sizeof(buf));
      if (n > 0) conn.cgiBuffer.append(buf, n);
      if (n <= 0) break;
    }
  }
  // check if child exited
  int status=0; pid_t r = ::waitpid(conn.cgiPid, &status, WNOHANG);
  if (r == conn.cgiPid) {
    if (conn.cgiOutFd >= 0) { ::close(conn.cgiOutFd); cgiFdToClient_.erase(conn.cgiOutFd); conn.cgiOutFd = -1; }
    conn.cgiActive = false;
  }
  // If output available and not yet built response, parse headers
  if (!conn.cgiBuffer.empty() && !conn.cgiHeadersDone) {
    size_t pos = conn.cgiBuffer.find("\r\n\r\n");
    if (pos != std::string::npos) {
      conn.cgiHeadersDone = true; conn.cgiBodyStart = pos + 4;
      std::string headerBlock = conn.cgiBuffer.substr(0, pos);
      int code = 200; std::string reason = "OK";
      std::string contentType = "text/html";
      std::string connectionHdr;
      std::vector<std::pair<std::string,std::string> > passHeaders;
      size_t start=0;
      while (start <= headerBlock.size()) {
        size_t end = headerBlock.find("\r\n", start);
        std::string line = headerBlock.substr(start, end == std::string::npos ? headerBlock.size()-start : end-start);
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
          std::string name = line.substr(0, colon);
          std::string value = line.substr(colon+1);
          while (!value.empty() && (value[0]==' '||value[0]=='\t')) value.erase(0,1);
          std::string lower = name; for (size_t i=0;i<lower.size();++i) lower[i]=(char)std::tolower(lower[i]);
          if (lower == "status") {
            int c = std::atoi(value.c_str()); if (c>=100 && c<=599) code = c;
            // optional reason phrase after code
            size_t sp = value.find(' ');
            if (sp != std::string::npos) {
              std::string r = value.substr(sp+1); if (!r.empty()) reason = r;
            }
          } else if (lower == "content-type") {
            contentType = value;
          } else if (lower == "connection") {
            connectionHdr = value;
          } else if (lower == "content-length") {
            passHeaders.push_back(std::make_pair(name, value)); // preserve provided length
          } else {
            passHeaders.push_back(std::make_pair(name, value));
          }
        }
        if (end == std::string::npos) break; else start = end + 2;
      }
      std::string body = conn.cgiBuffer.substr(conn.cgiBodyStart);
      // Determine keep-alive
      if (connectionHdr.empty()) {
        // Derive from HTTP/1.1 default
        conn.keepAlive = true;
      } else {
        std::string lower = connectionHdr; for (size_t i=0;i<lower.size();++i) lower[i]=(char)std::tolower(lower[i]);
        conn.keepAlive = (lower == "keep-alive");
      }
      // Build full response manually (not using buildResponse to allow header passthrough)
      std::string resp = "HTTP/1.1 ";
      char codeBuf[8]; std::sprintf(codeBuf, "%d", code); resp += codeBuf; resp += ' '; resp += reason; resp += "\r\n";
      bool haveCL = false;
      for (size_t i=0;i<passHeaders.size(); ++i) {
        std::string n = passHeaders[i].first; std::string v = passHeaders[i].second;
        std::string lower = n; for (size_t j=0;j<lower.size(); ++j) lower[j] = (char)std::tolower(lower[j]);
        if (lower == "content-length") haveCL = true;
        if (lower == "connection") continue; // override
        resp += n; resp += ": "; resp += v; resp += "\r\n";
      }
      if (!haveCL) {
        char lenBuf2[32]; std::sprintf(lenBuf2, "%lu", (unsigned long)body.size());
        resp += "Content-Length: "; resp += lenBuf2; resp += "\r\n";
      }
      // Ensure Content-Type present
      bool haveCT = false;
      for (size_t i=0;i<passHeaders.size(); ++i) { std::string l=passHeaders[i].first; for(size_t j=0;j<l.size();++j) l[j]=(char)std::tolower(l[j]); if (l=="content-type") {haveCT=true; break;} }
      if (!haveCT && !contentType.empty()) resp += "Content-Type: " + contentType + "\r\n";
      resp += "Connection: "; resp += conn.keepAlive?"keep-alive":"close"; resp += "\r\n\r\n";
      resp += body;
      conn.writeBuf = resp;
      conn.phase = ClientConnection::PH_RESPOND; conn.wantWrite = true;
      return true;
    }
  }
  // if child done and no headers, return error
  if (!conn.cgiActive && !conn.cgiHeadersDone) {
    conn.keepAlive = false;
    conn.writeBuf = buildResponse(500, "Internal Server Error", "CGI Execution Failed\n", "text/plain", false, false);
    conn.phase = ClientConnection::PH_RESPOND; conn.wantWrite = true;
    return false;
  }
  return true; // continue
}

void Server::reapCgi(ClientConnection &conn) {
  if (conn.cgiInFd >= 0) { ::close(conn.cgiInFd); cgiFdToClient_.erase(conn.cgiInFd); conn.cgiInFd = -1; }
  if (conn.cgiOutFd >= 0) { ::close(conn.cgiOutFd); cgiFdToClient_.erase(conn.cgiOutFd); conn.cgiOutFd = -1; }
  if (conn.cgiPid > 0) { int st; ::waitpid(conn.cgiPid, &st, WNOHANG); }
  conn.cgiActive = false;
}

bool Server::handleCgiEvent(int fd, short revents) {
  std::map<int,int>::iterator it = cgiFdToClient_.find(fd);
  if (it == cgiFdToClient_.end()) return true;
  int clientFd = it->second;
  std::map<int, ClientConnection>::iterator cit = clients_.find(clientFd);
  if (cit == clients_.end()) { ::close(fd); cgiFdToClient_.erase(it); return true; }
  ClientConnection &conn = cit->second;
  if (!conn.cgiActive) return true;
  if (revents & (POLLHUP | POLLERR)) {
    // mark child likely done; drive IO then close
    driveCgiIO(conn);
    if (conn.cgiOutFd == fd) { ::close(conn.cgiOutFd); cgiFdToClient_.erase(fd); conn.cgiOutFd = -1; }
    if (conn.cgiInFd == fd) { ::close(conn.cgiInFd); cgiFdToClient_.erase(fd); conn.cgiInFd = -1; }
    conn.cgiActive = false;
  }
  if (!driveCgiIO(conn)) return false;
  return true;
}
