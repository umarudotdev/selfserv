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
docs/                  # Comprehensive documentation and examples
├── config/            # Configuration parser examples
├── http/              # HTTP parser implementation examples
├── server/            # Server core and RAII wrappers
├── integration/       # Go-based integration test framework
└── unit/              # Unit test patterns and examples
```

### Architecture Design Principles

#### Event-Driven, Single-Threaded Architecture
- **Single poll() loop**: All I/O operations go through one poll/select/epoll call
- **Non-blocking I/O**: Never block on read/write operations - rely on poll readiness
- **Event dispatching**: Single thread manages all connections simultaneously
- **State machines**: Each connection progresses through well-defined states
- **Resource efficiency**: Eliminates thread overhead and context switching

#### Connection State Machine
Connections progress through these states with compile-time safety:
```
ACCEPTED → REQ_HEADERS → REQ_BODY → HANDLE → RESP_BUILD → SENDING → KEEPALIVE_IDLE/CLOSING
```

#### Typestate Pattern Implementation
Use template-based state types to ensure compile-time correctness:
```cpp
template<typename State>
class Connection {
  // State-specific operations only available in correct states
  // Compile-time prevention of invalid state transitions
};
```

### Configuration System Architecture

#### NGINX-Inspired Configuration
- **Server blocks**: Virtual host support with server_name matching
- **Location blocks**: Route-based configuration with longest-prefix matching
- **Directives**: Timeout, size limits, CGI, upload configuration
- **POD structures**: Simple C++98-compatible config data structures

#### Two-Stage Parsing
1. **Lexical analysis**: Tokenize configuration file
2. **Recursive descent parser**: Build configuration object tree

### HTTP Protocol Implementation

#### Streaming Request Parser
- **Re-entrant state machine**: Handle fragmented incoming data
- **Incremental parsing**: Parse headers and body as data arrives
- **Content-Length and Chunked**: Support both transfer encoding methods
- **Header validation**: Case-insensitive header handling with size limits

#### Parser States
```cpp
enum ParserState {
  kStateRequestLine,  // Parse "METHOD /path HTTP/1.1"
  kStateHeaders,      // Parse header lines until \r\n\r\n
  kStateBody,         // Parse body based on Content-Length/chunked
  kStateDone,         // Request complete
  kStateError         // Parse error occurred
};
```

### Non-blocking I/O Architecture

#### Event Loop Structure
```cpp
while (running) {
  BuildPollFds();           // Prepare file descriptor array
  int ready = poll(fds);    // Wait for I/O events
  if (ready > 0) {
    ProcessListeningSocket(); // Handle new connections
    ProcessClientSockets();   // Handle client I/O
  }
  ProcessTimeouts();        // Check connection deadlines
}
```

#### Connection Lifecycle Management
- **Accept**: New connections from listening socket
- **Read**: Non-blocking receive with partial data handling
- **Parse**: Incremental HTTP request parsing
- **Route**: Match request to configuration handlers
- **Handle**: Execute static/CGI/upload/directory handlers
- **Write**: Non-blocking response transmission
- **Cleanup**: Resource deallocation and connection state cleanup

### Error Handling Patterns

#### Status-Based Error Handling (No Exceptions)
- **Error codes**: Consistent return value patterns
- **Error propagation**: Bubble errors up through call stack
- **Graceful degradation**: Continue serving other clients on errors
- **Client isolation**: Prevent one client error from affecting others

#### HTTP Error Response Generation
- **4xx Client errors**: Malformed requests, missing resources
- **5xx Server errors**: Internal failures, CGI timeouts
- **Custom error pages**: Configurable error response content
- **Proper status codes**: RFC-compliant status code usage

### Resource Management Patterns

#### RAII Implementation
- **FD wrapper**: Automatic file descriptor lifecycle management
```cpp
class FD {
  int m_fd;
  // Constructor acquires, destructor releases
  // Copy constructor uses dup() for safe sharing
};
```

- **Memory management**: Manual new/delete with RAII wrappers
- **Socket management**: Automatic cleanup on connection termination
- **Process management**: Child process lifecycle for CGI execution

#### Resource Ownership
- **Clear ownership**: Single owner responsible for cleanup
- **Transfer semantics**: Explicit ownership transfer patterns
- **Scope-based cleanup**: Resources tied to object lifetimes
- **No global state**: Avoid static/global class objects (initialization order issues)

## Critical Implementation Notes

- **No execve for server**: Only for CGI script execution
- **Error handling**: Use error codes, never exceptions; never check errno after read/write (poll-based flow)
- **Memory management**: Manual RAII with clear ownership; no global/static class objects
- **Constructor rules**: Keep simple (just initialize members), use explicit Init() for complex setup
- **Resource wrappers**: RAII classes for sockets, file descriptors, and memory allocation
- **Browser compatibility**: Test with real browsers, not just curl/telnet
- **Chunked encoding**: Support for both request and response chunking
- **File uploads**: Proper multipart/form-data handling

### CGI Implementation Patterns

#### CGI Process Management
- **Fork-exec pattern**: fork() child process, execve() CGI script
- **Pipe communication**: stdin/stdout redirection via pipes
- **Environment variables**: Proper CGI meta-variable setup
- **Non-blocking pipes**: CGI I/O integrated into main event loop
- **Timeout handling**: Terminate runaway CGI processes
- **Process cleanup**: Proper SIGCHLD handling and zombie prevention

#### CGI Environment Setup
```cpp
// Required CGI environment variables
setenv("REQUEST_METHOD", request.method.c_str(), 1);
setenv("PATH_INFO", pathInfo.c_str(), 1);
setenv("QUERY_STRING", queryString.c_str(), 1);
setenv("CONTENT_TYPE", contentType.c_str(), 1);
setenv("CONTENT_LENGTH", contentLength.c_str(), 1);
setenv("SERVER_NAME", serverName.c_str(), 1);
setenv("SERVER_PORT", serverPort.c_str(), 1);
// ... additional meta-variables
```

#### CGI Response Parsing
- **Document response**: Content-Type header + body content
- **Redirect response**: Location header handling
- **Local redirect**: Path-based internal redirects
- **Error handling**: CGI script failures and malformed output

### File Upload Implementation

#### Multipart/Form-Data Processing
- **Boundary detection**: Parse multipart boundary from Content-Type
- **Streaming parser**: Process uploads without buffering entire content
- **Part header parsing**: Content-Disposition, filename extraction
- **Direct-to-disk streaming**: Avoid memory exhaustion on large uploads
- **Upload limits**: Enforce client_max_body_size and individual file limits

#### Upload State Machine
```cpp
enum UploadState {
  kBoundarySearch,    // Looking for multipart boundary
  kPartHeaders,       // Parsing part headers
  kPartData,          // Streaming part content to disk
  kPartComplete,      // Part finished, look for next boundary
  kUploadComplete     // All parts processed
};
```

### Testing Strategy & Patterns

#### Unit Testing Framework
- **Criterion integration**: Optional test framework for advanced assertions
- **Fallback testing**: Basic assertions when Criterion unavailable
- **Isolated components**: Test parsers, config, utilities independently
- **Mock data**: Controlled test inputs for deterministic results

#### Unit Test Examples
```cpp
// HTTP parser testing
void test_parses_simple_get() {
  HttpRequestParser parser;
  HttpRequest request;
  std::string raw = "GET /index.html HTTP/1.1\r\n\r\n";
  bool complete = parser.Parse(raw, request);
  assert(complete);
  assert(request.method == "GET");
  assert(request.uri == "/index.html");
}
```

#### Integration Testing Strategy
- **Go-based test framework**: Comprehensive integration test suite
- **NGINX comparison**: Behavioral compatibility testing
- **Multi-client simulation**: Concurrent connection testing
- **Error condition testing**: Malformed requests, timeouts, resource limits
- **Browser compatibility**: Real browser testing scenarios

#### Integration Test Patterns
```go
func TestBasicGetRequest(t *testing.T) {
  // Start both webserv and nginx
  // Send identical requests to both
  // Compare responses for behavioral compatibility
}
```

### Performance Considerations

#### Memory Management
- **Minimize allocations**: Reuse buffers where possible
- **Streaming processing**: Avoid loading entire files into memory
- **Connection pooling**: Efficient connection state management
- **Buffer sizing**: Optimal read/write buffer sizes

#### Connection Scalability
- **poll() vs select()**: Use poll() to eliminate FD_SETSIZE limitations
- **Connection limits**: Configurable max_connections to prevent resource exhaustion
- **Keep-alive support**: HTTP/1.1 persistent connections
- **Pipeline support**: Multiple requests per connection

#### Timeout Management
- **Header timeout**: Time limit for receiving complete headers
- **Body timeout**: Time limit for receiving complete request body
- **Idle timeout**: Keep-alive connection idle time
- **CGI timeout**: Maximum CGI script execution time

### Common Pitfalls & Debugging

#### Non-blocking I/O Pitfalls
- **EAGAIN/EWOULDBLOCK**: Normal condition, not an error
- **Partial reads/writes**: Always handle incomplete I/O operations
- **Poll event interpretation**: Correctly handle POLLIN, POLLOUT, POLLERR
- **Connection state consistency**: Ensure state machine transitions are valid

#### CGI Implementation Pitfalls
- **Zombie processes**: Always wait() for child processes
- **Pipe deadlocks**: Non-blocking CGI I/O to prevent deadlocks
- **Environment leakage**: Clean CGI environment setup
- **Path traversal**: Validate CGI script paths for security

#### HTTP Protocol Pitfalls
- **Header case sensitivity**: HTTP headers are case-insensitive
- **Transfer encoding precedence**: Chunked takes precedence over Content-Length
- **Connection header**: Proper keep-alive vs close handling
- **Status code compliance**: Use appropriate HTTP status codes

#### Memory and Resource Pitfalls
- **File descriptor leaks**: Ensure proper FD cleanup on all code paths
- **Memory leaks**: Validate RAII patterns and manual memory management
- **Buffer overflows**: Bounds checking on all buffer operations
- **Resource exhaustion**: Proper limits and cleanup under high load

## Testing & Validation

### Unit Testing Strategy
- **Parser validation**: Test HTTP request parsing with various input formats
- **Configuration testing**: Validate config file parsing and error handling
- **RAII wrapper testing**: Ensure proper resource management
- **State machine testing**: Verify connection state transitions
- **Edge case testing**: Malformed inputs, boundary conditions

### Integration Testing Framework
- **Go-based test suite**: Comprehensive HTTP client testing
- **NGINX behavioral comparison**: Reference implementation validation
- **Multi-client simulation**: Concurrent connection handling
- **Browser compatibility testing**: Real-world browser scenarios
- **Performance benchmarking**: Load testing and resource monitoring

### Manual Testing Approaches
- **Telnet testing**: Raw HTTP protocol verification
- **curl testing**: Command-line HTTP client validation
- **Browser testing**: Chrome, Firefox, Safari compatibility
- **Error condition testing**: Network failures, timeouts, malformed requests

### Testing Tools & Commands
```bash
# Unit testing
make test                    # Run unit test suite
./build/test_parser         # Specific parser tests

# Integration testing
cd docs/integration && go test  # Full integration suite
go test -run TestNginxComparison # NGINX comparison tests

# Manual testing
curl -v http://localhost:8080/   # Basic GET request
telnet localhost 8080            # Raw protocol testing

# Performance testing
ab -n 1000 -c 10 http://localhost:8080/  # Apache bench
wrk -t4 -c100 -d30s http://localhost:8080/ # Modern HTTP benchmarking
```

### Memory and Resource Validation
- **Valgrind integration**: Memory leak detection and analysis
- **AddressSanitizer**: Runtime memory error detection
- **File descriptor monitoring**: Ensure proper FD cleanup
- **Resource limit testing**: Behavior under resource constraints

## Development Environment

- VSCode integration via `compile_flags.txt` and `compile_commands.json`
- Doxygen documentation generation
- Git hooks for formatting and norm checking
- CI/CD via GitHub Actions (basic build verification)

## Google C++ Style Guide Adaptation (Project-Specific Cheatsheet)

Only the practically enforceable subset is listed here; prefer these defaults when generating or editing code.

### Headers & Includes

- One header per translation unit; small test mains may omit a header.
- Always use `#pragma once` (already project convention) instead of manual guards.
- Order includes in `.cpp` files: corresponding header first, then C system (`<sys/...>` / `<cstdio>`), C++ standard (`<string>` etc.), third-party (none here), project headers. Keep groups separated by a blank line, each group alphabetized.
- Minimize includes in headers; forward declare classes/structs when pointer or reference suffices (avoid unnecessary recompiles).
- Do not rely on transitive includes; explicitly include what you use in each `.cpp`.

### Namespaces & Scope

- Use anonymous namespaces only in `.cpp` files for internal linkage helpers; never in headers.
- Do not add `using namespace` at global scope. If needed, use specific `using ::std::size_t;` inside a function body only.
- Keep helper free functions in an anonymous namespace instead of making them `static` (both are fine; anonymous namespace preferred already used).

### Classes & Structs

- Constructors: Only initialize members; move multi-step or failure-prone logic to an `Init()` method returning a bool/error code (already followed for `Server::init`).
- Avoid hidden work in destructors; ensure they are noexcept and lightweight.
- Prefer POD-style config structs (already done) and RAII wrappers for resources (`FD`).
- Single-argument constructors must be marked `explicit` (add if new ones introduced).

### Functions

- Keep functions short and focused (< ~40 lines); refactor large branches into helpers (e.g., split upload/CGI/directory code if it grows further).
- Parameter ordering: inputs first (values / const refs), then outputs (non-const pointers / refs).
- Inline only trivial (≤10 lines, no loops/switch) accessors or small helpers; leave logic in `.cpp`.

### Variables

- Declare variables in the narrowest scope possible; initialize upon declaration.
- Avoid file-scope mutable objects; only POD static globals allowed (we currently have none besides constants/macros).
- Use `const` for values not modified after initialization.

### Error Handling & Returns

- No exceptions (C++98 project rule). Propagate errors via return codes / bool and fallback responses.
- Avoid using `errno` after non-blocking read/write per subject; rely on poll readiness and return values only.

### Formatting & Style

- Indentation: 2 spaces (no tabs) for C++; tabs limited to Makefile.
- Line length: keep ≤ 100 chars when practical; wrap long literals carefully.
- Braces on same line for function/class definitions; always brace multi-line conditionals and loops.
- Space after keywords (`if (` / `for (`) and around binary operators.

### Ownership & RAII

- Continue wrapping file descriptors/sockets; no raw owning pointer new/delete in code generation (prefer stack objects or RAII wrappers). Smart pointers disallowed (C++98 + constraints), so if dynamic allocation required, provide manual delete in destructor.

### Naming

- Types & classes: `PascalCase`.
- Functions / methods: `PascalCase` (e.g., `BuildPollFds`, `HandleReadable`). Future additions should follow this; existing camelCase functions may be migrated gradually.
- Variables (general local scope): `camelCase`.
- Member variables: `m_camelCase` (prefix `m_`).
- Static (class) members: `s_camelCase` (prefix `s_`).
- Global / namespace‑scope (non-const) variables (avoid when possible): `g_camelCase` (prefix `g_`).
- Constants: prefer `kCamelCase` for `const` objects or ALL_CAPS for macro constants / required preprocessor symbols.
- Enum types: `PascalCase`; enumerators `kCamelCase` (or ALL_CAPS if matching existing pattern, but prefer `kCamelCase`).
- Macro-like constants: only when necessary; ALL_CAPS with underscores.
- Avoid Hungarian notation beyond the sanctioned `m_`, `s_`, `g_` ownership/scope hints.
- Boolean variables: affirmative (`isReady`, `hasBody`) — with prefixes where applicable (`m_isReady`).

Additional naming specifics:

- Filenames: lowercase_with_underscores where practical; tolerate existing mixed style, do not churn history.
- Headers vs sources: match stem (`FooBar.hpp` / `FooBar.cpp`).
- Namespaces: short lowercase (e.g., `selfserv::parser`); avoid deep nesting.
- Template parameters: `T`, `U`, or descriptive `TypeName`.
- Test cases: snake_case or CamelCase; prefer snake_case for clarity (`parse_chunked_body_success`).
- Abbreviations: only if widely recognized (`fd`, `cgi`); otherwise spell out (`connection`).
- Migration note: existing functions in camelCase need not be bulk‑renamed immediately; apply new style for all new/modified APIs and gradually refactor when touching code.

### Ordering Conventions

- File layout (`.cpp`): (1) related header first, (2) C system headers, (3) C++ standard headers, (4) third‑party (none), (5) project headers, then `using` declarations (rare), then anonymous namespace helpers, then function / method definitions.
- Header layout: (1) `#pragma once`, (2) forward declarations, (3) public includes strictly required, (4) declarations (types, classes, functions), (5) inline trivial definitions.
- Class member order: (1) public type aliases / enums, (2) public static consts, (3) public constructors/destructor (ctor, dtor, deleted copy if any), (4) public methods, (5) protected section (mirror order), (6) private section: helpers first, then data members last. Keep data members grouped logically (e.g., config, state, buffers) with blank lines.
- Overloads / related functions: group together with no unrelated code between them.
- Function parameter ordering: inputs (value / const ref) first, then outputs (non‑const pointers/refs) — already noted; keep this consistent across overloads.
- Initialization list ordering: match declaration order of members in the class to avoid warnings and improve readability.
- Route / config parsing functions: keep directive handlers alphabetized or grouped (timeouts, paths, CGI) to minimize merge conflicts.
- Tests: order test cases from simplest (happy path) to more complex / edge cases; name consistently.
- Enum ordering: logical progression (state machine steps or severity). Add comment if out‑of‑sequence for dependency reasons.
- Switch case ordering: natural ordering (enum declaration order or most common cases first if performance oriented) — prefer enum order for readability unless profiling dictates otherwise.

### Comments & TODOs

- File header: brief purpose line if adding new module.
- Function comment only when intent isn't obvious from signature; prefer self-documenting names.
- Use `// TODO(user): description` for deferred work.

### Forbidden / Avoid

- No global non-POD objects or static initialization of complex types.
- No RTTI, exceptions, `<typeinfo>`, dynamic_cast, or templates introducing complexity unless justified.
- No unchecked recursion for request handling (risk of stack issues).

### Testing

- Add lightweight unit tests beside existing parser tests; prefer deterministic inputs without network when possible.

### When In Doubt

- Mirror existing local style present in adjacent code and keep changes minimal & consistent.
