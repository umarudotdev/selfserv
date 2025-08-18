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
#include <sys/types.h>
#include <fstream>
#include <dirent.h>
#include <ctime>
#include <cerrno>
#include <sys/stat.h>

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
  const ServerConfig &sc = config_.servers[0];
  long best = -1;
  for (std::map<int, ClientConnection>::const_iterator it = clients_.begin(); it != clients_.end(); ++it) {
    const ClientConnection &c = it->second;
    unsigned long deadline = 0;
    if (!c.headersComplete) deadline = c.createdAtMs + (unsigned long)sc.headerTimeoutMs;
    else if (!c.bodyComplete) deadline = c.lastActivityMs + (unsigned long)sc.bodyTimeoutMs;
    else if (c.keepAlive) deadline = c.lastActivityMs + (unsigned long)sc.idleTimeoutMs;
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
  const ServerConfig &scTO = config_.servers[0];
  std::map<int, ClientConnection>::iterator itSweep = clients_.begin();
  while (itSweep != clients_.end()) {
    ClientConnection &c = itSweep->second;
    bool closeIt = false;
    // header timeout
    if (!c.headersComplete && scTO.headerTimeoutMs > 0 && nowMs - c.createdAtMs > (unsigned long)scTO.headerTimeoutMs) {
      closeIt = true;
    } else if (c.headersComplete && !c.bodyComplete && scTO.bodyTimeoutMs > 0 && nowMs - c.lastActivityMs > (unsigned long)scTO.bodyTimeoutMs) {
      closeIt = true;
    } else if (c.keepAlive && c.bodyComplete && scTO.idleTimeoutMs > 0 && nowMs - c.lastActivityMs > (unsigned long)scTO.idleTimeoutMs) {
      closeIt = true;
    }
    if (closeIt) {
      int fd = itSweep->first;
      std::cerr << "[timeout] fd=" << fd << " sending 408\n";
      // send 408 if we haven't queued a response yet
      if (c.writeBuf.empty()) {
        c.writeBuf = buildResponse(408, "Request Timeout", "408 Request Timeout\n", "text/plain", false, false);
        c.wantWrite = true;
      }
      c.keepAlive = false;
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

static const ServerConfig &selectServer(const Config &cfg) {
  return cfg.servers[0]; // placeholder: first server only
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
    if (req.headers[i].name == name) { value = req.headers[i].value; return true; }
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
  if (conn.parser.error()) {
  conn.keepAlive = false;
  conn.writeBuf = buildResponse(400, "Bad Request", "400 Bad Request\n", "text/plain", false, false);
  std::cerr << "[400] malformed request bytes=" << conn.readBuf.size() << "\n";
        conn.wantWrite = true;
        break;
      }
  conn.headersComplete = true; // we have at least parsed headers (parser only flips after full body though)
      const ServerConfig &sc = selectServer(config_);
  if (conn.request.body.size() > sc.clientMaxBodySize) {
        conn.keepAlive = false;
        conn.writeBuf = buildResponse(413, "Payload Too Large", "413 Payload Too Large\n", "text/plain", false, false);
        std::cerr << "[413] body_size=" << conn.request.body.size() << " limit=" << sc.clientMaxBodySize << "\n";
        conn.wantWrite = true;
        break;
      }
      const RouteConfig *route = matchRoute(sc, conn.request.uri);
  if (!route) {
  conn.keepAlive = false;
  conn.writeBuf = buildResponse(404, "Not Found", "404 Not Found\n", "text/plain", conn.keepAlive, conn.request.method == "HEAD");
  std::cerr << "[404] uri=" << conn.request.uri << "\n";
      } else {
        if (!route->methods.empty()) {
          bool ok = false;
          for (size_t i = 0; i < route->methods.size(); ++i) if (route->methods[i] == conn.request.method) { ok = true; break; }
          if (!ok) {
            conn.keepAlive = false;
            conn.writeBuf = buildResponse(405, "Method Not Allowed", "405 Method Not Allowed\n", "text/plain", conn.keepAlive, conn.request.method == "HEAD");
            std::cerr << "[405] method=" << conn.request.method << " uri=" << conn.request.uri << "\n";
            conn.wantWrite = true; break;
          }
        }
        std::string rel = conn.request.uri.substr(route->path.size());
        if (rel.empty() || rel == "/") {
          if (!route->index.empty()) rel = "/" + route->index;
        }
        // Basic traversal guard
        if (rel.find("..") != std::string::npos) {
          conn.keepAlive = false;
          conn.writeBuf = buildResponse(403, "Forbidden", "403 Forbidden\n", "text/plain", conn.keepAlive, conn.request.method == "HEAD");
          std::cerr << "[403] traversal attempt uri=" << conn.request.uri << "\n";
        } else {
          std::string filePath = route->root + rel;
          std::string body;
          if (conn.request.method == "POST" && route->uploadsEnabled) {
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
            conn.bodyComplete = true;
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
                std::cerr << "[200] dir listing uri=" << conn.request.uri << (conn.keepAlive?" keep-alive":" close") << "\n";
              } else {
                conn.keepAlive = false;
                conn.writeBuf = buildResponse(500, "Internal Server Error", "500 Internal Server Error\n", "text/plain", false, conn.request.method == "HEAD");
              }
            } else {
              conn.keepAlive = false;
              conn.writeBuf = buildResponse(403, "Forbidden", "403 Forbidden\n", "text/plain", false, conn.request.method == "HEAD");
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
                std::cerr << "[204] deleted uri=" << conn.request.uri << "\n";
              } else {
                conn.keepAlive = false;
                conn.writeBuf = buildResponse(500, "Internal Server Error", "500 Internal Server Error\n", "text/plain", false, false);
                std::cerr << "[500] delete failed uri=" << conn.request.uri << " errno=" << errno << "\n";
              }
            } else if (isDir(filePath)) {
              conn.keepAlive = false;
              conn.writeBuf = buildResponse(403, "Forbidden", "403 Forbidden\n", "text/plain", false, false);
            } else {
              conn.keepAlive = false;
              conn.writeBuf = buildResponse(404, "Not Found", "404 Not Found\n", "text/plain", false, false);
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
              conn.bodyComplete = true;
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
            } else if (conn.request.method == "DELETE") {
              // Not implemented deletion semantics yet
              conn.keepAlive = false;
              conn.writeBuf = buildResponse(501, "Not Implemented", "501 Not Implemented\n", "text/plain", false, false);
            } else {
              conn.keepAlive = false;
              conn.writeBuf = buildResponse(405, "Method Not Allowed", "405 Method Not Allowed\n", "text/plain", false, false);
            }
          } else {
            conn.keepAlive = false;
            conn.writeBuf = buildResponse(404, "Not Found", "404 Not Found\n", "text/plain", conn.keepAlive, conn.request.method == "HEAD");
            std::cerr << "[404] file=" << filePath << "\n";
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
    if (!conn.keepAlive) {
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
