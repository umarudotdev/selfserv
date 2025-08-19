#include "http/HttpRequest.hpp"

#include <cctype>
#include <cstdlib>
#include <iostream>

HttpRequestParser::HttpRequestParser()
    : m_state(kStateRequestLine),
      m_contentLength(0),
      m_consumed(0),
      m_chunked(false),
      m_headerEndOffset(0),
      m_chunkState(kChunkSize),
      m_currentChunkSize(0),
      m_currentChunkRead(0) {}

void HttpRequestParser::Reset() {
  m_state = kStateRequestLine;
  m_contentLength = 0;
  m_consumed = 0;
  m_chunked = false;
  m_headerEndOffset = 0;
  m_chunkState = kChunkSize;
  m_currentChunkSize = 0;
  m_currentChunkRead = 0;
}

static std::string trim(const std::string &s) {
  size_t a = 0;
  while (a < s.size() &&
         (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
    ++a;
  size_t b = s.size();
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                   s[b - 1] == '\n'))
    --b;
  return s.substr(a, b - a);
}

bool HttpRequestParser::Parse(const std::string &data, HttpRequest &req) {
  if (m_state == kStateDone || m_state == kStateError) return req.complete;
  size_t hdrEnd = data.find("\r\n\r\n");
  if (m_state == kStateRequestLine || m_state == kStateHeaders) {
    if (hdrEnd == std::string::npos) return false;  // need more
    if (hdrEnd > 8192) {
      m_state = kStateError;
      m_consumed = hdrEnd;
      return false;
    }
    std::string headersPart = data.substr(0, hdrEnd);
    size_t pos = 0;
    bool first = true;
    while (pos < headersPart.size()) {
      size_t eol = headersPart.find("\r\n", pos);
      if (eol == std::string::npos) eol = headersPart.size();
      std::string line = headersPart.substr(pos, eol - pos);
      pos = eol + 2;
      if (first) {
        first = false;
        size_t m1 = line.find(' ');
        if (m1 == std::string::npos) {
          m_state = kStateError;
          m_consumed = eol;
          return false;
        }
        size_t m2 = line.find(' ', m1 + 1);
        if (m2 == std::string::npos) {
          m_state = kStateError;
          m_consumed = eol;
          return false;
        }
        req.method = line.substr(0, m1);
        req.uri = line.substr(m1 + 1, m2 - m1 - 1);
        req.version = line.substr(m2 + 1);
      } else {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
          HttpHeader h;
          h.name = trim(line.substr(0, colon));
          h.value = trim(line.substr(colon + 1));
          req.headers.push_back(h);
          if (h.name == "Content-Length") {
            m_contentLength = (size_t)std::atoi(h.value.c_str());
          }
          if (h.name == "Transfer-Encoding" && h.value == "chunked") {
            m_chunked = true;
          }
        }
      }
    }
    m_state = kStateBody;
    m_headerEndOffset = hdrEnd + 4;
  }
  if (m_state == kStateBody) {
    if (m_chunked) {
      // Decode incrementally
      size_t p = m_headerEndOffset +
                 (m_consumed ? (m_consumed - m_headerEndOffset) : 0);
      if (m_consumed < m_headerEndOffset)
        m_consumed = m_headerEndOffset;  // ensure consumed covers header
      // Use m_consumed to track raw consumed bytes
      while (p < data.size() && m_chunkState != kChunkDone &&
             m_chunkState != kChunkTrailer) {
        if (m_chunkState == kChunkSize) {
          size_t lineEnd = data.find("\r\n", p);
          if (lineEnd == std::string::npos) break;  // need more
          std::string sz = data.substr(p, lineEnd - p);
          // hex size
          size_t val = 0;
          for (size_t i = 0; i < sz.size(); ++i) {
            char c = sz[i];
            int d = -1;
            if (c >= '0' && c <= '9')
              d = c - '0';
            else if (c >= 'a' && c <= 'f')
              d = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F')
              d = 10 + (c - 'A');
            else {
              m_state = kStateError;
              return false;
            }
            val = (val << 4) + d;
          }
          m_currentChunkSize = val;
          m_currentChunkRead = 0;
          p = lineEnd + 2;
          m_consumed = p;
          if (m_currentChunkSize == 0) {
            m_chunkState = kChunkTrailer;
          } else
            m_chunkState = kChunkData;
        } else if (m_chunkState == kChunkData) {
          size_t available = data.size() - p;
          size_t need = m_currentChunkSize - m_currentChunkRead;
          size_t take = available < need ? available : need;
          req.body.append(data, p, take);
          p += take;
          m_currentChunkRead += take;
          m_consumed = p;
          if (m_currentChunkRead == m_currentChunkSize) {
            if (p + 1 >= data.size()) break;  // wait for CRLF
            if (data[p] == '\r' && data[p + 1] == '\n') {
              p += 2;
              m_consumed = p;
              m_chunkState = kChunkSize;
            } else {
              m_state = kStateError;
              return false;
            }
          } else {
            break;  // need more data
          }
        }
      }
      if (m_chunkState == kChunkTrailer) {
        // Expect final CRLF
        if (data.size() >= p + 2) {
          if (data[p] == '\r' && data[p + 1] == '\n') {
            p += 2;
            m_consumed = p;
            m_chunkState = kChunkDone;
            m_state = kStateDone;
            req.complete = true;
          } else {
            m_state = kStateError;
          }
        }
      }
      if (m_chunkState == kChunkDone) {
        return true;
      }
    } else {
      size_t bodyStart = m_headerEndOffset;
      size_t have = data.size() - bodyStart;
      if (have < m_contentLength) {
        // debug incomplete body
        if (m_contentLength > 0 && have > 0) {
          std::cerr << "[PARSER] waiting body have=" << have
                    << " needed=" << m_contentLength << "\n";
        }
      }
      if (have >= m_contentLength) {
        req.body = data.substr(bodyStart, m_contentLength);
        m_consumed = bodyStart + m_contentLength;
        m_state = kStateDone;
        req.complete = true;
      }
    }
  }
  return req.complete;
}
