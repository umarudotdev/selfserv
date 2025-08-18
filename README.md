# SelfServ HTTP/1.1 Server

## Overview

SelfServ is a single-process, single-thread, non‑blocking HTTP/1.1 server written in portable C++98 and developed as an educational / 42‑style project. It implements core mandatory features plus quality-of-life extensions while staying within constraints (no external net libs, only poll(), fork() only for CGI).

## Key Features

- Multiple listening sockets & virtual hosts (Host header routing)
- HTTP/1.1 keep‑alive & basic pipelining
- Methods: GET, POST, DELETE
- Static file serving, index files, directory listing (autoindex)
- Configurable per-route root, methods, redirect, CGI, uploads
- Upload handling (raw + basic multipart parsing & disk save)
- Body size limit enforcement
- Chunked transfer decoding (request bodies)
- Route‑level redirects (302) and custom error pages
- CGI execution by extension (non‑blocking pipes, timeout, env vars)
- Per‑vhost header/body/idle/CGI timeouts
- Minimal logging to stderr

## Notable Implementation Points

- One poll() loop drives all client sockets and CGI pipe fds.
- Explicit ClientConnection state machine phases (ACCEPTED, HEADERS, BODY, HANDLE, RESPOND, IDLE, CLOSING).
- Incremental parser retains buffer for potential pipelining; consumed() tells how many bytes to discard.
- CGI responses parsed for Status / headers; keep‑alive respected.
- Error pages loaded from configurable directory; fallback text if missing.

## Build & Run

```
make debug            # build debug binary (./webserv)
./webserv             # uses conf/selfserv.conf by default
# or specify another config
./webserv my.conf
```

## Example Request Flow

1. Client connects; poll() registers fd.
2. Read loop accumulates request; parser signals completion.
3. Virtual host selected via Host header; route matched longest prefix.
4. Handler decides: redirect / static / upload / directory / CGI / delete.
5. Response buffered then written non‑blocking; connection either recycled (keep‑alive) or closed.

## Configuration Summary

See `en.subject.md` Appendix A.2 for a minimal annotated example. A fuller sample lives in `conf/selfserv.conf`.

## Status Codes Implemented

200, 204, 302, 400, 403, 404, 405, 408, 413, 500, 501 (fallback), 504 (CGI timeout).

## CGI Support

Triggered when route has `cgi_ext` matching requested file. Optional `cgi_bin` specifies interpreter. Environment includes REQUEST*METHOD, SCRIPT_FILENAME, SCRIPT_NAME, PATH_INFO, QUERY_STRING, CONTENT_LENGTH, CONTENT_TYPE, GATEWAY_INTERFACE, SERVER_PROTOCOL, REDIRECT_STATUS, SERVER_NAME, SERVER_PORT, and HTTP*\* headers. Timeout produces 504.

## Security Notes

- Basic `..` traversal guard; no full realpath canonicalization yet.
- Uploaded filenames sanitized (strip path & control chars).
- CGI working directory changed to script's directory.

## Limitations / Future Work

- Multipart and CGI bodies fully buffered (not streamed).
- No TLS (scope limitation).
- Minimal logging & no access log rotation.
- Limited MIME type mapping (extend in code easily).

## Testing

Parser unit tests (Criterion if available; otherwise simple main). Add your own integration tests hitting server endpoints with curl or a script.

## License

MIT (see LICENSE)

## Author

Project scaffold & implementation via AI pair‑programming session (2025). Contributions welcome.
