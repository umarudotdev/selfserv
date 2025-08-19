# Webserv Implementation Guide

## Table of Contents

1. [Project Overview](#project-overview)
2. [Architecture Design](#architecture-design)
3. [Core Components](#core-components)
4. [Implementation Strategy](#implementation-strategy)
5. [HTTP Protocol Implementation](#http-protocol-implementation)
6. [Configuration System](#configuration-system)
7. [Error Handling](#error-handling)
8. [Testing Strategy](#testing-strategy)
9. [Performance Considerations](#performance-considerations)
10. [Common Pitfalls](#common-pitfalls)

## Project Overview

### What You're Building

A production-ready HTTP/1.1 server that can:

- Handle multiple simultaneous connections using a single thread
- Serve static files and execute CGI scripts
- Support file uploads and downloads
- Implement virtual hosts and complex routing
- Handle keep-alive connections and HTTP pipelining
- Provide robust error handling and timeout management

### Key Constraints

- **C++98 Standard**: No modern C++ features (auto, lambdas, smart pointers)
- **Single Poll Loop**: All I/O must go through one poll()/select()/epoll() call
- **Non-blocking I/O**: Never block on read/write operations
- **No External Libraries**: Pure C++ implementation only
- **No Fork Except CGI**: Process management limited to CGI execution

## Architecture Design

### High-Level Architecture

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   Config File   │    │   Main Loop     │    │   HTTP Parser   │
│   Parser        │    │   (poll/epoll)  │    │   State Machine │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 │
                    ┌─────────────────┐
                    │     Server      │
                    │   Controller    │
                    └─────────────────┘
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│ Connection  │    │    Route    │    │     CGI     │
│  Manager    │    │   Handler   │    │   Executor  │
└─────────────┘    └─────────────┘    └─────────────┘
```

### Core Design Patterns

1. **Event-Driven Architecture**: Single event loop handles all I/O
2. **State Machine**: Each connection progresses through defined states
3. **Typestate Pattern**: Compile-time state transition validation
4. **Strategy Pattern**: Different handlers for different route types
5. **RAII**: Resource management through constructors/destructors
6. **Factory Pattern**: Connection and parser creation

## Core Components

### Typestate Pattern Implementation

The typestate pattern ensures compile-time correctness of state transitions and prevents invalid operations in wrong states. Here's how to implement it for HTTP connections:

#### Connection State Types

```cpp
// Forward declarations for state types
struct AcceptedState;
struct HeaderState;
struct BodyState;
struct HandleState;
struct RespondState;
struct IdleState;
struct ClosingState;

// Base connection template with state parameter
template<typename State>
class Connection {
private:
  FD m_fd;
  std::string m_readBuf;
  std::string m_writeBuf;
  HttpRequest m_request;
  HttpRequestParser m_parser;
  unsigned long m_lastActivityMs;

  // State-specific data can be added via template specialization

  // Only allow construction by state transition methods
  friend class Connection<AcceptedState>;
  friend class Connection<HeaderState>;
  friend class Connection<BodyState>;
  friend class Connection<HandleState>;
  friend class Connection<RespondState>;
  friend class Connection<IdleState>;
  friend class Connection<ClosingState>;

public:
  explicit Connection(int fd) : m_fd(fd), m_lastActivityMs(getCurrentTimeMs()) {
    setNonBlocking(fd);
  }

  // Copy constructor for state transitions
  template<typename OtherState>
  Connection(const Connection<OtherState>& other)
    : m_fd(other.m_fd), m_readBuf(other.m_readBuf),
      m_writeBuf(other.m_writeBuf), m_request(other.m_request),
      m_parser(other.m_parser), m_lastActivityMs(other.m_lastActivityMs) {}

  int GetFd() const { return m_fd.Get(); }
  unsigned long GetLastActivity() const { return m_lastActivityMs; }
  void UpdateActivity() { m_lastActivityMs = getCurrentTimeMs(); }

  // Common operations available in all states
  bool HasTimedOut(unsigned long timeoutMs) const {
    return (getCurrentTimeMs() - m_lastActivityMs) > timeoutMs;
  }
};

// Type aliases for different connection states
using AcceptedConnection = Connection<AcceptedState>;
using HeaderConnection = Connection<HeaderState>;
using BodyConnection = Connection<BodyState>;
using HandleConnection = Connection<HandleState>;
using RespondConnection = Connection<RespondState>;
using IdleConnection = Connection<IdleState>;
using ClosingConnection = Connection<ClosingState>;
```

#### State-Specific Operations

```cpp
// Template specializations for state-specific operations

// AcceptedState: Just accepted, waiting for first data
template<>
class Connection<AcceptedState> {
  // ... base implementation ...

public:
  // Only operation allowed: start reading headers
  std::pair<HeaderConnection, bool> StartReading(const std::string& data) {
    m_readBuf += data;
    UpdateActivity();

    // Try to parse request line
    size_t lineEnd = m_readBuf.find("\r\n");
    if (lineEnd != std::string::npos) {
      // Transition to header parsing state
      return std::make_pair(HeaderConnection(*this), true);
    }

    // Not enough data yet, stay in accepted state
    return std::make_pair(HeaderConnection(*this), false);
  }
};

// HeaderState: Reading HTTP headers
template<>
class Connection<HeaderState> {
  // ... base implementation ...

public:
  // Parse headers and potentially transition to body or handle state
  std::pair<BodyConnection, bool> ParseHeaders(const std::string& data) {
    m_readBuf += data;
    UpdateActivity();

    HttpRequest tempRequest;
    if (m_parser.Parse(m_readBuf, tempRequest)) {
      m_request = tempRequest;

      // Check if we need to read body
      if (m_request.method == "POST" && hasContentLength(m_request)) {
        return std::make_pair(BodyConnection(*this), true);
      } else {
        // No body needed, ready to handle
        return std::make_pair(BodyConnection(*this), true);
      }
    }

    return std::make_pair(BodyConnection(*this), false);
  }

  ClosingConnection TimeoutError() {
    // Generate 408 Request Timeout
    return ClosingConnection(*this);
  }
};

// BodyState: Reading request body
template<>
class Connection<BodyState> {
  // ... base implementation ...

public:
  std::pair<HandleConnection, bool> ReadBody(const std::string& data) {
    m_readBuf += data;
    UpdateActivity();

    if (m_parser.Parse(m_readBuf, m_request) && m_request.complete) {
      return std::make_pair(HandleConnection(*this), true);
    }

    return std::make_pair(HandleConnection(*this), false);
  }

  ClosingConnection BodyTooLarge() {
    // Generate 413 Payload Too Large
    return ClosingConnection(*this);
  }

  ClosingConnection TimeoutError() {
    // Generate 408 Request Timeout
    return ClosingConnection(*this);
  }
};

// HandleState: Processing request (static files, CGI, etc.)
template<>
class Connection<HandleState> {
  RouteContext m_routeContext;  // State-specific data

public:
  RespondConnection ServeStaticFile(const std::string& path) {
    // File serving logic
    std::string body;
    if (readFile(path, body)) {
      m_writeBuf = buildResponse(200, "OK", body, guessContentType(path), true);
    } else {
      m_writeBuf = buildResponse(404, "Not Found", "File not found", "text/plain", false);
    }

    return RespondConnection(*this);
  }

  RespondConnection HandleUpload() {
    // Upload handling logic
    // ...
    return RespondConnection(*this);
  }

  HandleConnection StartCgi(const RouteConfig& route) {
    // Start CGI process, but stay in handle state until CGI completes
    // ...
    return HandleConnection(*this);
  }

  RespondConnection CgiCompleted(const std::string& cgiOutput) {
    // Process CGI output and build response
    // ...
    return RespondConnection(*this);
  }

  ClosingConnection ServerError() {
    // Generate 500 Internal Server Error
    return ClosingConnection(*this);
  }
};

// RespondState: Sending response to client
template<>
class Connection<RespondState> {
  // ... base implementation ...

public:
  std::pair<RespondConnection, bool> WriteResponse() {
    if (m_writeBuf.empty()) {
      return std::make_pair(RespondConnection(*this), true); // Done writing
    }

    ssize_t n = send(m_fd.Get(), m_writeBuf.data(), m_writeBuf.size(), 0);
    if (n > 0) {
      m_writeBuf.erase(0, n);
      UpdateActivity();
    }

    return std::make_pair(RespondConnection(*this), m_writeBuf.empty());
  }

  IdleConnection CompleteResponse() {
    // Response sent, transition to keep-alive idle state
    // Reset parser for potential pipelining
    size_t consumed = m_parser.Consumed();
    m_readBuf.erase(0, consumed);
    m_parser.Reset();
    m_request = HttpRequest();

    return IdleConnection(*this);
  }

  ClosingConnection CloseAfterResponse() {
    // Response sent, close connection
    return ClosingConnection(*this);
  }
};

// IdleState: Keep-alive idle, waiting for next request
template<>
class Connection<IdleState> {
  // ... base implementation ...

public:
  HeaderConnection NewRequest(const std::string& data) {
    // New request data arrived, start parsing
    m_readBuf += data;
    return HeaderConnection(*this);
  }

  ClosingConnection IdleTimeout() {
    // Idle timeout reached, close gracefully
    return ClosingConnection(*this);
  }
};

// ClosingState: Connection being closed
template<>
class Connection<ClosingState> {
  // ... base implementation ...

public:
  // No transitions out of closing state
  void Close() {
    // Perform final cleanup
    // Connection will be removed from server's connection map
  }
};
```

#### Server Integration with Typestate

```cpp
class Server {
private:
  // Use variant to store connections in different states
  typedef boost::variant<
    AcceptedConnection,
    HeaderConnection,
    BodyConnection,
    HandleConnection,
    RespondConnection,
    IdleConnection,
    ClosingConnection
  > ConnectionVariant;

  std::map<int, ConnectionVariant> m_connections;

  // Visitor for handling different connection states
  struct EventVisitor : public boost::static_visitor<> {
    Server* server;
    int fd;
    short revents;

    EventVisitor(Server* s, int f, short r) : server(s), fd(f), revents(r) {}

    void operator()(AcceptedConnection& conn) {
      if (revents & POLLIN) {
        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
          auto result = conn.StartReading(std::string(buffer, n));
          if (result.second) {
            server->m_connections[fd] = result.first;
          }
        }
      }
    }

    void operator()(HeaderConnection& conn) {
      if (revents & POLLIN) {
        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
          auto result = conn.ParseHeaders(std::string(buffer, n));
          if (result.second) {
            server->m_connections[fd] = result.first;
          }
        }
      }
    }

    void operator()(BodyConnection& conn) {
      if (revents & POLLIN) {
        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
          auto result = conn.ReadBody(std::string(buffer, n));
          if (result.second) {
            // Transition to handle state and immediately process
            HandleConnection handleConn = result.first;
            server->m_connections[fd] = server->ProcessRequest(handleConn);
          }
        }
      }
    }

    void operator()(RespondConnection& conn) {
      if (revents & POLLOUT) {
        auto result = conn.WriteResponse();
        if (result.second) {
          // Response complete, decide next state
          if (server->ShouldKeepAlive(conn)) {
            server->m_connections[fd] = conn.CompleteResponse();
          } else {
            server->m_connections[fd] = conn.CloseAfterResponse();
          }
        }
      }
    }

    void operator()(IdleConnection& conn) {
      if (revents & POLLIN) {
        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
          server->m_connections[fd] = conn.NewRequest(std::string(buffer, n));
        }
      }
    }

    void operator()(ClosingConnection& conn) {
      conn.Close();
      server->m_connections.erase(fd);
    }
  };

public:
  void ProcessEvents() {
    for (size_t i = 0; i < m_pfds.size(); ++i) {
      if (m_pfds[i].revents) {
        int fd = m_pfds[i].fd;

        if (IsListeningSocket(fd)) {
          AcceptNewConnection(fd);
        } else {
          auto it = m_connections.find(fd);
          if (it != m_connections.end()) {
            EventVisitor visitor(this, fd, m_pfds[i].revents);
            boost::apply_visitor(visitor, it->second);
          }
        }
      }
    }
  }

private:
  RespondConnection ProcessRequest(const HandleConnection& conn) {
    // Route the request and generate appropriate response
    const RouteConfig* route = FindRoute(conn.GetRequest());

    if (route->redirect.empty()) {
      return conn.ServeStaticFile(route->root + conn.GetRequest().uri);
    } else if (conn.GetRequest().method == "POST" && route->uploadsEnabled) {
      return conn.HandleUpload();
    } else if (ShouldUseCgi(*route, conn.GetRequest().uri)) {
      // For CGI, we might need to stay in handle state longer
      HandleConnection cgiConn = conn.StartCgi(*route);
      // ... CGI processing ...
      return cgiConn.CgiCompleted(cgiOutput);
    } else {
      return conn.ServeStaticFile(route->root + conn.GetRequest().uri);
    }
  }
};
```

#### Benefits of Typestate Pattern

1. **Compile-time Safety**: Invalid state transitions are caught at compile time
2. **Clear API**: Each state exposes only valid operations
3. **Self-Documenting**: State types make the protocol flow obvious
4. **Reduced Bugs**: Impossible to forget state checks or perform invalid operations
5. **Better Testing**: Each state can be unit tested independently

#### C++98 Adaptation

Since we're constrained to C++98, we can adapt this pattern:

```cpp
// C++98 version without templates (using inheritance instead)
class ConnectionBase {
protected:
  FD m_fd;
  std::string m_readBuf;
  std::string m_writeBuf;
  HttpRequest m_request;
  // ... common data

public:
  virtual ~ConnectionBase() {}
  virtual ConnectionBase* HandleEvent(short revents) = 0;
  virtual const char* GetStateName() const = 0;
};

class AcceptedConnection : public ConnectionBase {
public:
  virtual ConnectionBase* HandleEvent(short revents) {
    if (revents & POLLIN) {
      // Read data and potentially return HeaderConnection
      return new HeaderConnection(*this);
    }
    return this;
  }

  virtual const char* GetStateName() const { return "Accepted"; }
};

// Similar implementations for other states...
```

#### Typestate vs Traditional State Machine

**Traditional Approach (Current Implementation):**

```cpp
enum Phase { kPhaseAccepted, kPhaseHeaders, /* ... */ };

void HandleReadable(ClientConnection& conn) {
  switch (conn.m_phase) {
    case kPhaseAccepted:
      // Could accidentally access m_request before it's ready
      ProcessHeaders(conn);  // Runtime error possible
      break;
    case kPhaseHeaders:
      // Manual state validation needed
      if (conn.m_headersComplete) {
        conn.m_phase = kPhaseBody;
      }
      break;
  }
}
```

**Typestate Approach:**

```cpp
// Compile-time guarantees prevent invalid operations
HeaderConnection ProcessAccepted(AcceptedConnection& conn) {
  // conn.GetRequest() won't compile - request not ready yet
  return conn.StartReading(data);  // Only valid operation
}

RespondConnection ProcessHeaders(HeaderConnection& conn) {
  // Now we can safely access request data
  HttpRequest req = conn.GetRequest();  // Guaranteed to be valid
  return conn.ServeStaticFile(path);
}
```

#### Implementation Recommendation

For the 42 School webserv project, I recommend starting with the traditional state machine approach for these reasons:

1. **Simpler Learning Curve**: Focus on HTTP protocol and networking first
2. **C++98 Constraints**: Full typestate pattern benefits require modern C++ features
3. **Project Scope**: The pattern adds complexity that may not be justified for this project size
4. **Time Constraints**: Implementation time might be better spent on core features

However, if you want to explore advanced C++ patterns and have extra time, implementing a simplified version of typestate can be a great learning exercise and will make your code more robust.

### 1. Server Class

**Responsibilities:**

- Manage listening sockets
- Accept new connections
- Coordinate event loop
- Handle connection lifecycle

**Key Methods:**

```cpp
class Server {
  bool Init();                    // Set up listening sockets
  bool PollOnce(int timeoutMs);   // Single poll iteration
  void ProcessEvents();           // Handle ready file descriptors
  void Shutdown();                // Clean shutdown
};
```

### 2. ClientConnection Structure

**State Management:**

```cpp
enum Phase {
  kPhaseAccepted,   // Just accepted, no data yet
  kPhaseHeaders,    // Reading HTTP headers
  kPhaseBody,       // Reading HTTP body
  kPhaseHandle,     // Processing request (CGI execution)
  kPhaseRespond,    // Sending response
  kPhaseIdle,       // Keep-alive idle state
  kPhaseClosing     // Connection being closed
};
```

**Connection Data:**

- File descriptor (wrapped in RAII FD class)
- Read/write buffers
- HTTP request parser state
- Timing information
- CGI execution context

### 3. HTTP Request Parser

**Incremental Parsing:**

```cpp
class HttpRequestParser {
  bool Parse(const std::string &data, HttpRequest &request);
  size_t Consumed() const;  // For pipelining support
  void Reset();             // Prepare for next request
};
```

**Parser States:**

- Request line parsing (METHOD URI VERSION)
- Header parsing (Name: Value pairs)
- Body parsing (Content-Length or chunked)
- Validation and completion

### 4. Configuration System

**Hierarchical Structure:**

```cpp
struct Config {
  std::vector<ServerConfig> servers;
};

struct ServerConfig {
  std::string host;
  int port;
  std::vector<std::string> serverNames;
  std::vector<RouteConfig> routes;
  // Timeouts, limits, error pages...
};

struct RouteConfig {
  std::string path;
  std::string root;
  std::vector<std::string> methods;
  bool uploadsEnabled;
  std::string cgiExtension;
  // Route-specific configuration...
};
```

## Implementation Strategy

### Phase 1: Basic HTTP Server

1. **Set up project structure**

   ```
   src/
   ├── main.cpp
   ├── server/
   │   ├── Server.hpp
   │   ├── Server.cpp
   │   └── FD.hpp
   ├── http/
   │   ├── HttpRequest.hpp
   │   └── HttpRequest.cpp
   └── config/
       ├── Config.hpp
       └── Config.cpp
   ```

2. **Implement RAII file descriptor wrapper**

   ```cpp
   class FD {
     int m_fd;
   public:
     explicit FD(int fd = -1) : m_fd(fd) {}
     ~FD() { if (m_fd >= 0) close(m_fd); }
     int Get() const { return m_fd; }
     // Copy/assignment with dup()
   };
   ```

3. **Create basic server loop**

   ```cpp
   bool Server::PollOnce(int timeoutMs) {
     BuildPollFds(m_pfds);
     int ready = poll(m_pfds.data(), m_pfds.size(), timeoutMs);
     if (ready > 0) ProcessEvents();
     return ready >= 0;
   }
   ```

4. **Handle new connections**
   ```cpp
   void Server::AcceptNew(int listenFd) {
     int clientFd = accept(listenFd, NULL, NULL);
     if (clientFd >= 0) {
       setNonBlocking(clientFd);
       m_clients[clientFd] = ClientConnection();
       m_clients[clientFd].m_fd.Reset(clientFd);
     }
   }
   ```

### Phase 2: HTTP Protocol Implementation

1. **Implement request line parsing**

   ```cpp
   // Parse "GET /path HTTP/1.1"
   size_t space1 = line.find(' ');
   size_t space2 = line.find(' ', space1 + 1);
   request.method = line.substr(0, space1);
   request.uri = line.substr(space1 + 1, space2 - space1 - 1);
   request.version = line.substr(space2 + 1);
   ```

2. **Implement header parsing**

   ```cpp
   size_t colon = line.find(':');
   if (colon != std::string::npos) {
     HttpHeader header;
     header.name = trim(line.substr(0, colon));
     header.value = trim(line.substr(colon + 1));
     request.headers.push_back(header);
   }
   ```

3. **Handle Content-Length and Transfer-Encoding**
   ```cpp
   if (header.name == "Content-Length") {
     m_contentLength = atoi(header.value.c_str());
   } else if (header.name == "Transfer-Encoding" &&
              header.value == "chunked") {
     m_chunked = true;
   }
   ```

### Phase 3: Response Generation

1. **Static file serving**

   ```cpp
   std::string buildResponse(int code, const std::string& reason,
                           const std::string& body,
                           const std::string& contentType,
                           bool keepAlive) {
     std::ostringstream response;
     response << "HTTP/1.1 " << code << " " << reason << "\r\n";
     response << "Content-Type: " << contentType << "\r\n";
     response << "Content-Length: " << body.size() << "\r\n";
     response << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
     response << "\r\n" << body;
     return response.str();
   }
   ```

2. **Error page handling**
   ```cpp
   std::string loadErrorPageBody(const ServerConfig& sc, int code,
                               const std::string& fallback) {
     if (!sc.errorPageRoot.empty()) {
       std::string path = sc.errorPageRoot + "/" + toString(code) + ".html";
       std::string content;
       if (readFile(path, content)) return content;
     }
     return fallback;
   }
   ```

### Phase 4: Advanced Features

1. **Virtual host selection**

   ```cpp
   int selectServer(const Config& config, const std::string& host, int port) {
     for (size_t i = 0; i < config.servers.size(); ++i) {
       if (config.servers[i].port == port) {
         for (size_t j = 0; j < config.servers[i].serverNames.size(); ++j) {
           if (strcasecmp(host.c_str(),
                         config.servers[i].serverNames[j].c_str()) == 0) {
             return i;
           }
         }
       }
     }
     // Return first server for this port as default
     for (size_t i = 0; i < config.servers.size(); ++i) {
       if (config.servers[i].port == port) return i;
     }
     return 0;
   }
   ```

2. **Route matching**
   ```cpp
   const RouteConfig* findRoute(const ServerConfig& server,
                               const std::string& uri) {
     const RouteConfig* bestMatch = NULL;
     size_t bestLength = 0;

     for (size_t i = 0; i < server.routes.size(); ++i) {
       const std::string& path = server.routes[i].path;
       if (uri.substr(0, path.length()) == path &&
           path.length() > bestLength) {
         bestMatch = &server.routes[i];
         bestLength = path.length();
       }
     }
     return bestMatch;
   }
   ```

### Phase 5: CGI Implementation

1. **CGI execution setup**

   ```cpp
   bool MaybeStartCgi(ClientConnection& conn, const RouteConfig& route,
                     const std::string& filePath) {
     int inPipe[2], outPipe[2];
     if (pipe(inPipe) < 0 || pipe(outPipe) < 0) return false;

     pid_t pid = fork();
     if (pid == 0) {
       // Child process
       dup2(inPipe[0], 0);   // stdin
       dup2(outPipe[1], 1);  // stdout
       close(inPipe[1]);
       close(outPipe[0]);

       // Set up environment
       setenv("REQUEST_METHOD", conn.m_request.method.c_str(), 1);
       setenv("CONTENT_LENGTH", toString(conn.m_request.body.size()).c_str(), 1);

       execl(route.cgiInterpreter.c_str(),
             route.cgiInterpreter.c_str(),
             filePath.c_str(), NULL);
       exit(1);
     }

     // Parent process
     close(inPipe[0]);
     close(outPipe[1]);
     setNonBlocking(inPipe[1]);
     setNonBlocking(outPipe[0]);

     conn.m_cgiInFd = inPipe[1];
     conn.m_cgiOutFd = outPipe[0];
     conn.m_cgiPid = pid;
     conn.m_cgiActive = true;

     return true;
   }
   ```

2. **CGI I/O handling**
   ```cpp
   bool DriveCgiIO(ClientConnection& conn) {
     // Write request body to CGI stdin
     if (conn.m_cgiInFd >= 0 && conn.m_cgiWriteOffset < conn.m_request.body.size()) {
       ssize_t n = write(conn.m_cgiInFd,
                        conn.m_request.body.data() + conn.m_cgiWriteOffset,
                        conn.m_request.body.size() - conn.m_cgiWriteOffset);
       if (n > 0) conn.m_cgiWriteOffset += n;

       if (conn.m_cgiWriteOffset >= conn.m_request.body.size()) {
         close(conn.m_cgiInFd);
         conn.m_cgiInFd = -1;
       }
     }

     // Read CGI stdout
     if (conn.m_cgiOutFd >= 0) {
       char buffer[4096];
       ssize_t n = read(conn.m_cgiOutFd, buffer, sizeof(buffer));
       if (n > 0) {
         conn.m_cgiBuffer.append(buffer, n);
       }
     }

     return true;
   }
   ```

## HTTP Protocol Implementation

### Request Parsing State Machine

```cpp
enum ParserState {
  kStateRequestLine,  // Parsing "METHOD URI VERSION"
  kStateHeaders,      // Parsing "Name: Value" pairs
  kStateBody,         // Reading request body
  kStateDone,         // Request complete
  kStateError         // Parse error occurred
};
```

### Chunked Transfer Encoding

```cpp
enum ChunkState {
  kChunkSize,     // Reading hex chunk size
  kChunkData,     // Reading chunk data
  kChunkCrlf,     // Reading chunk trailing CRLF
  kChunkTrailer,  // Reading final trailer headers
  kChunkDone      // All chunks processed
};
```

### Keep-Alive Connection Management

1. **Connection header parsing**

   ```cpp
   bool shouldKeepAlive(const HttpRequest& request) {
     std::string connection = getHeader(request, "Connection");
     if (request.version == "HTTP/1.1") {
       return connection != "close";  // Default keep-alive
     } else {
       return connection == "keep-alive";  // Explicit opt-in
     }
   }
   ```

2. **Pipeline support**
   ```cpp
   void handleWritable(ClientConnection& conn) {
     // Send response
     ssize_t n = send(conn.m_fd.Get(), conn.m_writeBuf.data(),
                     conn.m_writeBuf.size(), 0);
     if (n > 0) conn.m_writeBuf.erase(0, n);

     if (conn.m_writeBuf.empty()) {
       if (conn.m_keepAlive) {
         // Prepare for next request
         size_t consumed = conn.m_parser.Consumed();
         conn.m_readBuf.erase(0, consumed);
         conn.m_parser.Reset();
         conn.m_phase = kPhaseIdle;
       } else {
         // Close connection
         CloseConnection(conn.m_fd.Get());
       }
     }
   }
   ```

## Configuration System

### Configuration File Format

```conf
# Example configuration
server 0.0.0.0 8080
  server_name example.com www.example.com
  client_max_body_size 1048576
  error_page_root ./errors

  route / ./www
    methods GET POST
    index index.html
    autoindex on

  route /upload ./www
    methods POST
    uploadsEnabled on
    upload_path ./uploads

  route /cgi-bin ./www/cgi
    methods GET POST
    cgi_ext .py
    cgi_bin /usr/bin/python3
```

### Configuration Parser

```cpp
class ConfigParser {
  bool parseServerBlock(std::istream& input, ServerConfig& server);
  bool parseRouteBlock(std::istream& input, RouteConfig& route);
  bool parseKeyValue(const std::string& line,
                    std::string& key, std::string& value);
};
```

## Error Handling

### Error Categories

1. **Client Errors (4xx)**

   - 400 Bad Request: Malformed HTTP
   - 403 Forbidden: No access permission
   - 404 Not Found: Resource doesn't exist
   - 405 Method Not Allowed: Method not supported on route
   - 408 Request Timeout: Headers/body timeout
   - 413 Payload Too Large: Body size limit exceeded

2. **Server Errors (5xx)**
   - 500 Internal Server Error: General server error
   - 501 Not Implemented: Unsupported feature
   - 504 Gateway Timeout: CGI timeout

### Error Response Generation

```cpp
void sendErrorResponse(ClientConnection& conn, int code,
                      const std::string& reason) {
  const ServerConfig& sc = m_config.servers[conn.m_serverIndex];
  std::string body = loadErrorPageBody(sc, code, reason + "\n");

  conn.m_writeBuf = buildResponse(code, reason, body, "text/html", false);
  conn.m_phase = kPhaseRespond;
  conn.m_keepAlive = false;  // Close after error
  conn.m_wantWrite = true;
}
```

## Testing Strategy

### Unit Testing

1. **HTTP Parser Tests**

   ```cpp
   void testBasicRequest() {
     HttpRequestParser parser;
     HttpRequest request;
     std::string data = "GET /test HTTP/1.1\r\nHost: example.com\r\n\r\n";

     assert(parser.Parse(data, request));
     assert(request.method == "GET");
     assert(request.uri == "/test");
     assert(request.version == "HTTP/1.1");
   }
   ```

2. **Configuration Tests**
   ```cpp
   void testConfigParsing() {
     std::istringstream config("server 127.0.0.1 8080\n"
                              "  server_name test.local\n");
     Config cfg;
     assert(parseConfig(config, cfg));
     assert(cfg.servers.size() == 1);
     assert(cfg.servers[0].port == 8080);
   }
   ```

### Integration Testing

1. **Static File Serving**

   ```bash
   curl -v http://localhost:8080/index.html
   ```

2. **File Upload**

   ```bash
   curl -X POST -F "file=@test.txt" http://localhost:8080/upload
   ```

3. **CGI Execution**
   ```bash
   curl -X POST -d "name=test" http://localhost:8080/cgi-bin/script.py
   ```

### Stress Testing

```cpp
// Connection limit test
for (int i = 0; i < 1000; ++i) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  connect(sock, serverAddr, sizeof(serverAddr));
  // Keep connections open to test limits
}
```

## Performance Considerations

### Memory Management

1. **Buffer Size Limits**

   ```cpp
   static const size_t MAX_REQUEST_SIZE = 8192;    // Headers
   static const size_t MAX_BODY_SIZE = 1048576;    // Default body limit
   static const size_t READ_BUFFER_SIZE = 4096;    // Read chunk size
   ```

2. **Connection Pooling**
   ```cpp
   std::map<int, ClientConnection> m_clients;  // Active connections
   static const int MAX_CONNECTIONS = 1024;    // Connection limit
   ```

### Timeout Management

```cpp
int ComputePollTimeout() const {
  unsigned long now = getCurrentTimeMs();
  unsigned long nextTimeout = now + 30000;  // Default 30s

  for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
    unsigned long deadline = it->second.m_lastActivityMs + getTimeoutMs(it->second);
    if (deadline < nextTimeout) nextTimeout = deadline;
  }

  return std::max(0, (int)(nextTimeout - now));
}
```

## Common Pitfalls

### 1. Blocking I/O

**Problem**: Using read/write without poll() check

```cpp
// WRONG
char buffer[1024];
int n = read(fd, buffer, sizeof(buffer));  // May block!
```

**Solution**: Always check poll() readiness

```cpp
// CORRECT
if (pollfd.revents & POLLIN) {
  char buffer[1024];
  int n = read(fd, buffer, sizeof(buffer));
  if (n > 0) /* process data */;
}
```

### 2. Invalid State Access

**Problem**: Accessing request data before it's ready

```cpp
// WRONG - Traditional state machine
void HandleConnection(ClientConnection& conn) {
  if (conn.m_phase == kPhaseAccepted) {
    // BUG: request not parsed yet!
    std::string method = conn.m_request.method;  // Undefined behavior
  }
}
```

**Solution**: Typestate pattern prevents this at compile time

```cpp
// CORRECT - Typestate approach
RespondConnection ProcessHeaders(HeaderConnection& conn) {
  // This compiles because HeaderConnection guarantees request is ready
  std::string method = conn.GetRequest().method;  // Safe!
  return conn.ServeStaticFile(path);
}
```

### 3. Forgotten State Transitions

**Problem**: Forgetting to update connection phase

```cpp
// WRONG
void HandleReadable(ClientConnection& conn) {
  if (conn.m_phase == kPhaseHeaders) {
    if (parseHeaders(conn)) {
      // BUG: Forgot to update phase!
      // conn.m_phase = kPhaseBody;  // Missing!
      processBody(conn);  // Will fail
    }
  }
}
```

**Solution**: Typestate forces explicit transitions

```cpp
// CORRECT
BodyConnection result = headerConn.ParseHeaders(data);
// Compiler ensures we handle the transition
```

### 4. HTTP Parsing Edge Cases

**Problem**: Not handling partial requests

```cpp
// WRONG
if (buffer.find("\r\n\r\n") != std::string::npos) {
  // Process complete request
}
```

**Solution**: Incremental parsing with state

```cpp
// CORRECT
while (hasMoreData()) {
  switch (m_state) {
    case kStateRequestLine:
      if (parseRequestLine()) m_state = kStateHeaders;
      break;
    // ... other states
  }
}
```

### 5. Resource Leaks

**Problem**: Manual resource management

```cpp
// WRONG - potential leak
int fd = open(filename, O_RDONLY);
if (error) return;  // fd leaked!
close(fd);
```

**Solution**: RAII wrappers

```cpp
// CORRECT
FD fd(open(filename, O_RDONLY));
if (!fd.Valid()) return;
// fd automatically closed in destructor
```

### 6. CGI Security Issues

**Problem**: Unsanitized environment

```cpp
// WRONG
setenv("PATH_INFO", request.uri.c_str(), 1);  // Injection risk!
```

**Solution**: Input validation and sanitization

```cpp
// CORRECT
std::string pathInfo = sanitizePath(request.uri);
if (isValidPath(pathInfo)) {
  setenv("PATH_INFO", pathInfo.c_str(), 1);
}
```

## Implementation Checklist

### Core Requirements ✓

- [ ] Single poll() loop for all I/O
- [ ] Non-blocking file descriptors
- [ ] HTTP/1.1 GET, POST, DELETE methods
- [ ] Static file serving with index files
- [ ] Directory listing (autoindex)
- [ ] File upload handling
- [ ] CGI execution with timeout
- [ ] Virtual hosts (server_name)
- [ ] Route-based configuration
- [ ] Keep-alive connections
- [ ] Error pages (default + custom)
- [ ] Request body size limits
- [ ] Multiple listening ports

### Advanced Features ✓

- [ ] Chunked transfer encoding
- [ ] HTTP pipelining
- [ ] Request/response timeouts
- [ ] Multipart form data parsing
- [ ] URL redirection
- [ ] Custom error pages
- [ ] Access logging
- [ ] Signal handling

### Quality Assurance ✓

- [ ] Memory leak testing (valgrind)
- [ ] Stress testing (siege, ab)
- [ ] Browser compatibility testing
- [ ] RFC compliance testing
- [ ] Edge case handling
- [ ] Error condition testing
- [ ] Performance profiling

This implementation guide provides a comprehensive roadmap for building a robust HTTP/1.1 server that meets all mandatory requirements while maintaining code quality and performance standards.
