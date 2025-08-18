---
applyTo: "**"
---

# Selfserv - HTTP Server Development Guide

## Project Overview

This is a C++98 HTTP/1.1 server implementation (executable: `webserv`) following the 42 School webserv project requirements. The server must handle GET, POST, DELETE methods, serve static content, support CGI execution, and implement non-blocking I/O with poll/select/epoll.

## Architecture & Key Requirements

- **C++98 Standard**: Strict compliance required (`-std=c++98 -Wall -Wextra -Werror -pedantic`)
- **Non-blocking I/O**: Single poll/select/epoll loop for all operations (no read/write without polling)
- **Configuration-driven**: NGINX-like config file support for virtual hosts, routes, CGI
- **HTTP/1.1 Compliance**: Compare behavior with NGINX as reference implementation
- **CGI Support**: Execute scripts (PHP, Python) with proper environment variables and I/O handling

## Build System & Development Workflow

### Essential Make Targets

```bash
make debug          # Debug build with symbols (-g3)
make sanitizer      # Debug + AddressSanitizer + UBSan
make loose          # Build without -Werror for development
make run            # Build and execute server
make valgrind       # Memory leak detection
make format         # clang-format code styling
make norm           # norminette compliance check
make docs           # Generate Doxygen documentation
make index          # Generate compile_commands.json for LSP
```

### Docker Development

```bash
make docker.debug   # Run targets in containerized environment
# All development tools pre-installed: clang, valgrind, norminette
```

## Code Style & Standards

- **Google C++ Style Guide**: Base formatting rules (see `.clang-format`)
- **42 Norm**: Additional constraints via norminette
- **C++98 Constraints**: No exceptions, RTTI, auto, smart pointers, or STL containers with complex dtors
- **2-space indentation**: Spaces for C++, tabs for Makefiles
- **Include guards**: `#pragma once` preferred over traditional guards
- **Header organization**: Related header first, C system, C++ system, other libs, project headers
- **Explicit constructors**: Mark single-argument constructors as `explicit`
- **RAII patterns**: Manual resource management with constructor/destructor pairs

## Key Development Patterns

### File Organization

```
include/selfserv.h     # Main header with version defines
src/main.cpp           # Entry point with config file parsing
src/                   # Implementation files (currently minimal)
tests/                 # Unit tests (TODO implementation)
```

### Configuration Approach

- Take NGINX server blocks as inspiration
- Support multiple virtual hosts on same port
- Route-based configuration for methods, CGI, uploads
- Default error pages and custom error handling

### Non-blocking I/O Architecture

- Single event loop managing all file descriptors
- State machines for request parsing and response generation
- Connection lifecycle management with proper cleanup
- Client request timeouts and connection limits

### Error Handling Patterns

- Consistent error code return values (no exceptions in C++98)
- Input validation and boundary checking
- Graceful degradation for malformed requests
- Resource cleanup on error paths

### Resource Management Patterns

- RAII wrapper classes for file descriptors and sockets
- Clear ownership semantics for memory and handles
- Avoid static/global class objects (C++98 initialization order issues)
- Use POD types for configuration data

## Critical Implementation Notes

- **No execve for server**: Only for CGI script execution
- **Error handling**: Use error codes, never exceptions; never check errno after read/write (poll-based flow)
- **Memory management**: Manual RAII with clear ownership; no global/static class objects
- **Constructor rules**: Keep simple (just initialize members), use explicit Init() for complex setup
- **Resource wrappers**: RAII classes for sockets, file descriptors, and memory allocation
- **Browser compatibility**: Test with real browsers, not just curl/telnet
- **Chunked encoding**: Support for both request and response chunking
- **File uploads**: Proper multipart/form-data handling

## Testing & Validation

- Compare behavior with NGINX for edge cases
- Use telnet for manual protocol testing
- Stress testing for connection handling
- Memory leak verification with valgrind
- Multiple browser compatibility testing

## Development Environment

- VSCode integration via `compile_flags.txt` and `compile_commands.json`
- Doxygen documentation generation
- Git hooks for formatting and norm checking
- CI/CD via GitHub Actions (basic build verification)
