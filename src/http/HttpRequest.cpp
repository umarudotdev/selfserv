#include "http/HttpRequest.hpp"
#include <cctype>
#include <cstdlib>
#include <iostream>

HttpRequestParser::HttpRequestParser() : state_(S_REQUEST_LINE), contentLength_(0), consumed_(0), chunked_(false), headerEndOffset_(0), chunkState_(CHUNK_SIZE), currentChunkSize_(0), currentChunkRead_(0) {}

void HttpRequestParser::reset() { state_ = S_REQUEST_LINE; contentLength_ = 0; consumed_ = 0; chunked_ = false; headerEndOffset_ = 0; chunkState_ = CHUNK_SIZE; currentChunkSize_ = 0; currentChunkRead_ = 0; }

static std::string trim(const std::string &s) {
  size_t a = 0; while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  size_t b = s.size(); while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
  return s.substr(a, b - a);
}

bool HttpRequestParser::parse(const std::string &data, HttpRequest &req) {
  if (state_ == S_DONE || state_ == S_ERROR) return req.complete;
  size_t hdrEnd = data.find("\r\n\r\n");
  if (state_ == S_REQUEST_LINE || state_ == S_HEADERS) {
  if (hdrEnd == std::string::npos) return false; // need more
  if (hdrEnd > 8192) { state_ = S_ERROR; consumed_ = hdrEnd; return false; }
    std::string headersPart = data.substr(0, hdrEnd);
    size_t pos = 0; bool first = true;
    while (pos < headersPart.size()) {
      size_t eol = headersPart.find("\r\n", pos);
      if (eol == std::string::npos) eol = headersPart.size();
      std::string line = headersPart.substr(pos, eol - pos);
      pos = eol + 2;
      if (first) {
        first = false;
    size_t m1 = line.find(' '); if (m1 == std::string::npos) { state_ = S_ERROR; consumed_ = eol; return false; }
    size_t m2 = line.find(' ', m1 + 1); if (m2 == std::string::npos) { state_ = S_ERROR; consumed_ = eol; return false; }
        req.method = line.substr(0, m1);
        req.uri = line.substr(m1 + 1, m2 - m1 - 1);
        req.version = line.substr(m2 + 1);
      } else {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
          HttpHeader h; h.name = trim(line.substr(0, colon)); h.value = trim(line.substr(colon + 1));
          req.headers.push_back(h);
          if (h.name == "Content-Length") {
            contentLength_ = (size_t)std::atoi(h.value.c_str());
          }
          if (h.name == "Transfer-Encoding" && h.value == "chunked") {
            chunked_ = true;
          }
        }
      }
    }
    state_ = S_BODY;
    headerEndOffset_ = hdrEnd + 4;
  }
  if (state_ == S_BODY) {
    if (chunked_) {
      // Decode incrementally
      size_t p = headerEndOffset_ + (consumed_ ? (consumed_ - headerEndOffset_) : 0);
      if (consumed_ < headerEndOffset_) consumed_ = headerEndOffset_; // ensure consumed covers header
      // Use consumed_ to track raw consumed bytes
      while (p < data.size() && chunkState_ != CHUNK_DONE && chunkState_ != CHUNK_TRAILER) {
        if (chunkState_ == CHUNK_SIZE) {
          size_t lineEnd = data.find("\r\n", p);
          if (lineEnd == std::string::npos) break; // need more
          std::string sz = data.substr(p, lineEnd - p);
          // hex size
          size_t val = 0; for (size_t i = 0; i < sz.size(); ++i) {
            char c = sz[i]; int d = -1;
            if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a'); else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A'); else { state_ = S_ERROR; return false; }
            val = (val << 4) + d;
          }
          currentChunkSize_ = val; currentChunkRead_ = 0; p = lineEnd + 2; consumed_ = p;
          if (currentChunkSize_ == 0) { chunkState_ = CHUNK_TRAILER; }
          else chunkState_ = CHUNK_DATA;
        } else if (chunkState_ == CHUNK_DATA) {
          size_t available = data.size() - p;
          size_t need = currentChunkSize_ - currentChunkRead_;
            size_t take = available < need ? available : need;
          req.body.append(data, p, take);
          p += take; currentChunkRead_ += take; consumed_ = p;
          if (currentChunkRead_ == currentChunkSize_) {
            if (p + 1 >= data.size()) break; // wait for CRLF
            if (data[p] == '\r' && data[p+1] == '\n') { p += 2; consumed_ = p; chunkState_ = CHUNK_SIZE; }
            else { state_ = S_ERROR; return false; }
          } else {
            break; // need more data
          }
        }
      }
      if (chunkState_ == CHUNK_TRAILER) {
        // Expect final CRLF
        if (data.size() >= p + 2) {
          if (data[p] == '\r' && data[p+1] == '\n') {
            p += 2; consumed_ = p; chunkState_ = CHUNK_DONE; state_ = S_DONE; req.complete = true; }
          else { state_ = S_ERROR; }
        }
      }
      if (chunkState_ == CHUNK_DONE) {
        return true;
      }
    } else {
      size_t bodyStart = headerEndOffset_;
      size_t have = data.size() - bodyStart;
      if (have < contentLength_) {
        // debug incomplete body
        if (contentLength_ > 0 && have > 0) {
          std::cerr << "[PARSER] waiting body have=" << have << " needed=" << contentLength_ << "\n";
        }
      }
      if (have >= contentLength_) {
        req.body = data.substr(bodyStart, contentLength_);
        consumed_ = bodyStart + contentLength_;
        state_ = S_DONE;
        req.complete = true;
      }
    }
  }
  return req.complete;
}
