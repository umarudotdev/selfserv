// Unit tests for HttpRequestParser
#include <cstddef>
#include <cstdint>
#include <string>
#include <iostream>
#include "http/HttpRequest.hpp"

#ifdef __has_include
#  if __has_include(<criterion/criterion.h>)
#    define HAVE_CRITERION 1
#  endif
#endif

#ifdef HAVE_CRITERION
#include <criterion/criterion.h>
#include <criterion/logging.h>
#endif

static void test_parses_simple_get_impl() {
  HttpRequestParser p; HttpRequest req;
  std::string raw = "GET /index.html HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
  bool done = p.parse(raw, req);
#ifdef HAVE_CRITERION
  cr_assert(done, "Parser should complete for simple GET");
  cr_assert(strcmp(req.method.c_str(), "GET") == 0);
  cr_assert(strcmp(req.uri.c_str(), "/index.html") == 0);
  cr_assert(strcmp(req.version.c_str(), "HTTP/1.1") == 0);
  cr_assert(req.headers.size() == 2);
#else
  if (!done) std::cerr << "FAIL simple_get: parser incomplete" << std::endl;
#endif
}

static void test_parses_content_length_body_impl() {
  HttpRequestParser p; HttpRequest req;
  std::string raw = "POST /upload HTTP/1.1\r\nHost: example.com\r\nContent-Length: 11\r\n\r\nhello world";
  bool done = p.parse(raw, req);
#ifdef HAVE_CRITERION
  cr_assert(done);
  cr_assert(strcmp(req.method.c_str(), "POST") == 0);
  cr_assert(strcmp(req.body.c_str(), "hello world") == 0);
#else
  if (!done || req.body != "hello world") std::cerr << "FAIL content_length" << std::endl;
#endif
}

static void test_parses_chunked_body_impl() {
  HttpRequestParser p; HttpRequest req;
  std::string part1 = "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n";
  bool early = p.parse(part1, req);
  std::string combined = part1 + "5\r\npedia\r\n0\r\n\r\n";
  bool done = p.parse(combined, req); // supply full buffer (parser uses offsets)
#ifdef HAVE_CRITERION
  cr_assert(!early);
  cr_assert(done);
  cr_assert(strcmp(req.body.c_str(), "Wikipedia") == 0);
#else
  if (early || !done || req.body != "Wikipedia") std::cerr << "FAIL chunked" << std::endl;
#endif
}

#ifdef HAVE_CRITERION
Test(HttpRequestParser, parses_simple_get) { test_parses_simple_get_impl(); }
Test(HttpRequestParser, parses_content_length_body) { test_parses_content_length_body_impl(); }
Test(HttpRequestParser, parses_chunked_body) { test_parses_chunked_body_impl(); }
#else
int main() {
  test_parses_simple_get_impl();
  test_parses_content_length_body_impl();
  test_parses_chunked_body_impl();
  return 0;
}
#endif
