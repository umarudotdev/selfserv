#pragma once
#include <string>
#include <vector>

struct HttpHeader { std::string name; std::string value; };

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
  void reset();
  bool parse(const std::string &data, HttpRequest &req);
  size_t consumed() const { return consumed_; }
  bool error() const { return state_ == S_ERROR; }
 private:
  enum State { S_REQUEST_LINE, S_HEADERS, S_BODY, S_DONE, S_ERROR } state_;
  size_t contentLength_;
  size_t consumed_;
  bool chunked_;
  size_t headerEndOffset_;
  // Chunked decoding state
  enum ChunkState { CHUNK_SIZE, CHUNK_DATA, CHUNK_CRLF, CHUNK_TRAILER, CHUNK_DONE } chunkState_;
  size_t currentChunkSize_;
  size_t currentChunkRead_;
};
