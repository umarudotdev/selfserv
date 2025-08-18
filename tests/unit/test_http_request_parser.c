#include <criterion/criterion.h>
#include <criterion/logging.h>

#include <string>
extern "C" {
#include <stddef.h>
}
#include "http/HttpRequest.hpp"

Test(HttpRequestParser, parses_simple_get) {
  HttpRequestParser p;
  HttpRequest req;
  std::string raw =
      "GET /index.html HTTP/1.1\r\nHost: example.com\r\nConnection: "
      "close\r\n\r\n";
  bool done = p.parse(raw, req);
  cr_assert(done);
  cr_assert(eq(str, req.method.c_str(), "GET"));
  cr_assert(eq(str, req.uri.c_str(), "/index.html"));
  cr_assert(eq(str, req.version.c_str(), "HTTP/1.1"));
  cr_assert(req.headers.size() == 2);
}

Test(HttpRequestParser, parses_content_length_body) {
  HttpRequestParser p;
  HttpRequest req;
  std::string raw =
      "POST /upload HTTP/1.1\r\nHost: example.com\r\nContent-Length: "
      "11\r\n\r\nhello world";
  bool done = p.parse(raw, req);
  cr_assert(done);
  cr_assert(eq(str, req.method.c_str(), "POST"));
  cr_assert(eq(str, req.body.c_str(), "hello world"));
}

Test(HttpRequestParser, parses_chunked_body) {
  HttpRequestParser p;
  HttpRequest req;
  std::string part1 =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: "
      "chunked\r\n\r\n4\r\nWiki\r\n";
  cr_assert(!p.parse(part1, req));
  std::string part2 = "5\r\npedia\r\n0\r\n\r\n";
  bool done =
      p.parse(part1 + part2, req);  // supply full buffer (parser uses offset)
  cr_assert(done);
  cr_assert(eq(str, req.body.c_str(), "Wikipedia"));
}
