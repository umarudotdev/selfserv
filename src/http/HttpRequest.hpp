#pragma once

#include <string>
#include <vector>

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpRequest {
  std::string method;
  std::string uri;
  std::string version;
  std::vector<HttpHeader> headers;
  std::string body;
  bool complete;

  HttpRequest() : complete(false) {}
};

class HttpRequestParser {
 public:
  HttpRequestParser();

  void Reset();
  bool Parse(const std::string &data, HttpRequest &request);
  size_t Consumed() const { return m_consumed; }
  bool Error() const { return m_state == kStateError; }

 private:
  enum State {
    kStateRequestLine,
    kStateHeaders,
    kStateBody,
    kStateDone,
    kStateError
  } m_state;

  size_t m_contentLength;
  size_t m_consumed;
  bool m_chunked;
  size_t m_headerEndOffset;

  // Chunked decoding state
  enum ChunkState {
    kChunkSize,
    kChunkData,
    kChunkCrlf,
    kChunkTrailer,
    kChunkDone
  } m_chunkState;

  size_t m_currentChunkSize;
  size_t m_currentChunkRead;
};
