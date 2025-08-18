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
