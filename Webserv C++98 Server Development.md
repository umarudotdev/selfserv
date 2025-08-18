

# **Webserv: An Architectural Blueprint for a C++98 Compliant, Non-Blocking HTTP/1.1 Server**

## **I. Architectural Foundations: The Non-Blocking, Event-Driven Core**

The fundamental architecture of a high-performance network server is dictated by its approach to concurrency and I/O management. For Webserv, the stringent constraints of C++98, a single-process model (barring CGI execution), and the demand for high concurrency necessitate an event-driven, non-blocking design. This model stands in contrast to traditional multi-process or multi-threaded architectures, offering superior scalability and resource efficiency under these specific limitations.

### **1.1. The Event-Driven Paradigm vs. Traditional Models**

Historically, network servers managed concurrency by dedicating a separate thread of execution to each client connection. The two primary models were:

* **Multi-Process Model:** Pioneered by early Unix servers, this model involves the master server process calling fork() to create a new child process for each incoming connection. While this provides excellent isolation between connections, the overhead of process creation and context switching becomes prohibitively expensive as the number of concurrent connections grows. For this project, the use of fork() is explicitly forbidden for any purpose other than launching CGI scripts, rendering this model non-compliant for general connection handling.  
* **Multi-Threaded Model:** This model replaces heavy processes with lighter-weight threads, one per connection. While this reduces the overhead of creation and context switching compared to the multi-process approach, it introduces significant complexity in the form of shared memory and the need for sophisticated synchronization mechanisms (mutexes, condition variables) to prevent race conditions. In the context of C++98, which lacks a standardized threading library, implementing a robust multi-threaded server would require platform-specific APIs and would add a layer of complexity that is elegantly avoided by the event-driven approach.

The **Event-Driven, Single-Threaded Model** chosen for Webserv circumvents these issues entirely. Instead of dedicating a thread of execution to each connection and blocking on I/O operations (like read() or write()), the server operates within a single main thread. This thread manages all connections simultaneously using a mechanism for I/O multiplexing. It issues non-blocking I/O calls and uses a notification system to learn when a socket is ready for a read or write operation. The core of this architecture is the **event loop**: a continuous cycle that waits for I/O events and dispatches them to the appropriate handlers. This design ensures that the server's single thread is never idle as long as there is work to be done, maximizing CPU and resource utilization.

### **1.2. I/O Multiplexing: Choosing the Right Tool (poll() vs. select())**

I/O multiplexing is the cornerstone of the event-driven model, allowing a single thread to monitor multiple file descriptors (sockets) for readiness.1 The POSIX standard provides several mechanisms for this, with

select() and poll() being the most relevant for C++98-compliant systems.

* **select():** This system call uses three file descriptor sets (fd\_set) to monitor for readability, writability, and exceptions. While universally available, select() has a critical and disqualifying limitation: the size of the fd\_set is determined by the compile-time constant FD\_SETSIZE, which is typically set to 1024\.2 This imposes a hard, unchangeable limit on the number of concurrent connections the server can handle. For a modern web server, a limit of 1024 connections is unreasonably low and makes  
  select() architecturally unsound for a scalable solution.  
* **poll():** This system call improves upon select() by removing the FD\_SETSIZE limitation. Instead of fixed-size bitmasks, poll() operates on a dynamically sized array of pollfd structures. Each structure contains a file descriptor, a bitmask of events to monitor (events), and a bitmask for the kernel to return the events that occurred (revents). The number of file descriptors poll() can monitor is limited only by available system memory and process limits, making it the superior and necessary choice for Webserv.

The main event loop will be structured around the poll() system call:

1. **Preparation:** A std::vector\<struct pollfd\> is constructed, containing an entry for each listening socket and each active client connection. The events field for each pollfd is set to request notifications for read events (POLLIN) and, if there is pending data to send, write events (POLLOUT).  
2. **Waiting:** The poll() function is called with the vector of pollfd structures and a timeout value. The call will block until an event occurs on one of the monitored file descriptors or the timeout expires.  
3. **Dispatching:** After poll() returns, the server iterates through the pollfd vector. For each entry where the revents field is non-zero, the server dispatches the event to the appropriate handler. This could be accepting a new connection on a listening socket, reading data from a client socket, or writing data to a client socket.

### **1.3. The Connection State Machine**

The decision to use non-blocking I/O fundamentally alters the programming model. System calls like read(), recv(), write(), and send() are not guaranteed to complete their work in a single invocation. When an operation cannot be completed immediately without blocking (e.g., reading from an empty socket buffer or writing to a full one), the call returns immediately with a special error code, and errno is set to EAGAIN or EWOULDBLOCK.3

This behavior means that a single HTTP transaction may be processed across dozens or even hundreds of iterations of the event loop. To manage this, the server must maintain the state for every single connection. A simple function call is insufficient; what is required is a formal **state machine**. Each client connection will be represented by a C++ object that encapsulates not only its file descriptor and I/O buffers but also its current state in the transaction lifecycle.

The choice of non-blocking I/O shifts the core complexity of the server's design. Instead of managing threads and locks, the central challenge becomes the meticulous management of state. The server must, for every active connection, remember precisely where it left off in the protocol exchange. This state dictates the next action to be taken when poll() signals that a file descriptor is ready. For example, if a socket is reported as readable and its connection object is in the PARSING\_HEADERS state, the server continues to append data to the header buffer. If it is in the READING\_BODY state, it appends data to the body buffer. The robustness and correctness of the entire server depend directly on the precise and comprehensive implementation of this state machine.

A robust connection lifecycle can be modeled with the following states:

* AWAITING\_REQUEST: The initial state after a connection is accepted. The server is waiting to read the first bytes of a new HTTP request.  
* PARSING\_HEADERS: The server is actively reading from the socket and parsing the request line and headers. The state persists until the CRLFCRLF sequence indicating the end of the headers is found.  
* READING\_BODY: If the headers indicate a message body (via Content-Length or Transfer-Encoding), the server transitions to this state to read the body from the socket.  
* HANDLING\_REQUEST: The request has been fully received and parsed. The server is now executing the business logic, such as locating a static file or preparing to launch a CGI script.  
* GENERATING\_RESPONSE: The handler has completed its task, and the server is constructing the HTTP response message (status line, headers, and body).  
* WRITING\_TO\_CGI: A CGI-specific state where the server is writing the request body to the standard input of the forked CGI process.  
* READING\_FROM\_CGI: A CGI-specific state where the server is reading the response from the standard output of the CGI process.  
* SENDING\_RESPONSE: The complete response is buffered and is being written to the client socket. This state persists until the entire response buffer has been sent.  
* CLOSING: The connection is marked for termination. The event loop will close its file descriptor and release all associated resources.

## **II. C++98 Idioms for Bulletproof Resource Management**

The project's mandate for absolute resource integrity—no memory leaks, no dangling file descriptors—under the constraints of C++98 and a ban on exceptions necessitates a rigorous and disciplined approach to resource management. The only viable strategy to achieve this level of robustness is the **Resource Acquisition Is Initialization (RAII)** programming idiom.

### **2.1. The RAII Philosophy in a Pre-C++11 World**

RAII is a core C++ concept that binds the lifetime of a resource to the lifetime of an object.5 The principle is simple yet powerful: a resource is acquired in an object's constructor, and it is released in the object's destructor. By creating these resource-managing objects on the stack (as automatic variables), their destructors are guaranteed by the language to be called when the object goes out of scope, regardless of how that scope is exited. This automates the cleanup process, making it deterministic and leak-proof.

In the C++98 standard, any class that directly manages ownership of a resource must adhere to the **Rule of Three**. This rule states that if a class defines any of the following, it should probably define all three to ensure correct behavior:

1. **Destructor:** To release the resource.  
2. **Copy Constructor:** To define what happens when a new object is created as a copy of an existing one.  
3. **Copy Assignment Operator:** To define what happens when an existing object is assigned the value of another.

Failing to correctly implement these can lead to catastrophic bugs. For example, a default copy constructor would simply copy the raw resource handle (like a file descriptor or a pointer), leading to two objects believing they own the same resource. When the first object is destroyed, it releases the resource, leaving the second object with a dangling handle. When the second object is destroyed, it attempts to release the already-released resource, resulting in a double-free or double-close error.

For Webserv, RAII is not merely a "best practice"; it is the foundational safety net that enables the server to meet its reliability requirements. It transforms resource management from a procedural task, prone to human error, into a declarative one, enforced by the compiler and the language's core mechanics.

### **2.2. A RAII Wrapper for File Descriptors (ManagedFD)**

Every socket and file handle obtained from the operating system is a file descriptor—an integer that must be explicitly closed via the close() system call. To manage these safely, a dedicated RAII wrapper is essential. The ManagedFD class will enforce unique ownership of a file descriptor.

The design philosophy for ManagedFD is to make it non-copyable and non-assignable. This is the safest and simplest way to manage unique resources like a client connection socket. By declaring the copy constructor and copy assignment operator as private and not providing an implementation, any attempt to copy a ManagedFD object will result in a compile-time error, preventing accidental ownership violations.

A minimal implementation skeleton for this class is as follows:

C++

\#**include** \<unistd.h\> // For close()

class ManagedFD {  
private:  
    int \_fd;

    // Prohibit copying and assignment by declaring them private  
    // and not defining them.  
    ManagedFD(const ManagedFD&);  
    ManagedFD& operator\=(const ManagedFD&);

public:  
    // Constructor acquires the resource.  
    // An explicit constructor prevents implicit conversions.  
    explicit ManagedFD(int fd \= \-1) : \_fd(fd) {}

    // Destructor releases the resource.  
    \~ManagedFD() {  
        if (\_fd\!= \-1) {  
            ::close(\_fd);  
        }  
    }

    // Getter for the raw file descriptor.  
    int get() const {  
        return \_fd;  
    }

    // Releases ownership of the FD, returning it to the caller.  
    // The ManagedFD object no longer owns the resource.  
    int release() {  
        int tmp \= \_fd;  
        \_fd \= \-1;  
        return tmp;  
    }  
};

### **2.3. A RAII Wrapper for Dynamic Memory (ScopedPtr)**

The project constraints forbid malloc()/free() and require that all dynamic memory allocated with new be managed via RAII. Since C++11's std::unique\_ptr and std::shared\_ptr are unavailable, a custom smart pointer class is required. The ScopedPtr\<T\> class will serve this purpose, implementing strict, non-transferable ownership semantics.

This class will be a template to work with any dynamically allocated type. Similar to ManagedFD, it will be made non-copyable and non-assignable to enforce that only one ScopedPtr can own a given piece of memory at any time.

C++

template \<typename T\>  
class ScopedPtr {  
private:  
    T\* \_ptr;

    ScopedPtr(const ScopedPtr&);  
    ScopedPtr& operator\=(const ScopedPtr&);

public:  
    explicit ScopedPtr(T\* p \= 0) : \_ptr(p) {}

    \~ScopedPtr() {  
        delete \_ptr;  
    }

    void reset(T\* p \= 0) {  
        delete \_ptr;  
        \_ptr \= p;  
    }

    T& operator\*() const {  
        return \*\_ptr;  
    }

    T\* operator\-\>() const {  
        return \_ptr;  
    }

    T\* get() const {  
        return \_ptr;  
    }  
};

This simple class provides the essential RAII guarantee for heap-allocated memory, ensuring that delete is automatically called when the ScopedPtr object goes out of scope. This prevents memory leaks in all execution paths, a critical component of the server's overall robustness.

## **III. The Configuration Engine: Parsing NGINX-like Directives**

The behavior of Webserv is dictated entirely by a configuration file, the syntax of which is inspired by the widely-used NGINX web server.7 A robust and flexible parser is required to translate this human-readable format into an in-memory data structure that the server can use to make runtime decisions. This process is not merely about loading settings; it is about constructing a sophisticated routing table that directs incoming requests to the correct handlers based on host, port, and URI.

### **3.1. Defining the Configuration Grammar**

The configuration file is a text file composed of directives organized into hierarchical blocks or "contexts."

* **Syntax Rules:**  
  * **Simple Directive:** A key-value pair ending with a semicolon (e.g., worker\_processes 1;).  
  * **Block Directive (Context):** A name followed by a set of directives enclosed in curly braces (e.g., server {... }).  
  * **Comments:** Begin with a \# character and extend to the end of the line.  
  * **Whitespace:** Whitespace between tokens is generally ignored.  
* **Context Hierarchy:** The parser will support a nested structure:  
  * **server block:** Defines a virtual server that listens on a specific address and port combination and responds to specific hostnames.  
  * **location block:** Defined within a server block, it specifies rules for requests matching a particular URI pattern.  
* **Supported Directives:** The parser must recognize and process the following key directives:  
  * **listen \[host:\]port;**: Specifies the IP address and port on which the server will accept connections. A missing host implies listening on all interfaces.  
  * **server\_name name1 name2...;**: A space-separated list of hostnames that this server block will handle.10  
  * **error\_page code /uri;**: Maps an HTTP status code to a specific URI for a custom error page.  
  * **client\_max\_body\_size size;**: Sets the maximum permissible size for a client request body. The size can be specified with suffixes like k for kilobytes or m for megabytes.11  
  * **root path;**: Defines the root directory for serving files within a server or location context.13  
  * **index file1 file2...;**: A space-separated list of filenames to be served as the default page when a directory is requested.13  
  * **limit\_except method1... {... }**: Restricts which HTTP methods are allowed for a given location, applying enclosed allow or deny rules to all other methods.14  
  * **return code URL;**: Issues an HTTP redirection with the specified status code and target URL.18  
  * **autoindex on|off;**: Enables or disables the automatic generation of a directory listing if no index file is found.21  
  * **upload\_path path;**: A custom directive specifying the directory where uploaded files should be stored.  
  * **cgi\_pass.ext /path/to/interpreter;**: A custom directive to associate a file extension with a CGI interpreter executable.

### **3.2. Parser Implementation Strategy**

A two-stage approach—lexical analysis followed by parsing—is a classic and effective method for implementing a configuration parser without external dependencies.

1. **Lexer (Tokenizer):** The first stage reads the configuration file as a stream of characters and groups them into a sequence of meaningful tokens. For this grammar, the tokens would include identifiers (e.g., listen, server), strings (e.g., /var/www/html), numbers (e.g., 8080), and symbols (e.g., {, }, ;). This simplifies the subsequent parsing logic by abstracting away the details of whitespace and comments.  
2. **Parser (Recursive Descent):** The second stage takes the token stream from the lexer and builds a structured representation of the configuration. A recursive descent parser is an excellent fit for the hierarchical nature of the configuration file. The parser will have a set of mutually recursive functions, such as parse\_server\_block(), which in turn calls parse\_location\_block() and parse\_simple\_directive(). This method is intuitive and relatively easy to implement and debug.  
3. **Data Structure:** The output of the parser will be a tree of C++ objects that mirrors the file's structure. For example, a top-level Config object might hold a std::vector\<ServerConfig\>. Each ServerConfig object would store its settings (listen port, server names, etc.) and a std::vector\<LocationConfig\>. This object-oriented representation provides a clean and type-safe interface for the rest of the server to query configuration values.

### **3.3. Request Routing Logic**

The parsed configuration data structure serves as a real-time routing table for every incoming HTTP request. The routing process is a two-step procedure that precisely follows NGINX's logic.

1. **Virtual Host Matching:** When a new connection is established and the first request is parsed, the server inspects the Host header field. It iterates through all parsed ServerConfig objects, comparing the Host header value against each server\_name directive.  
   * The server first looks for an exact match.  
   * If no exact match is found, it checks for wildcard matches.  
   * If still no match is found, or if the request lacks a Host header, the request is routed to the **default server** for that host:port. The project specification dictates that the first server block defined for a given listen address acts as the default.  
2. **Location Matching:** After identifying the correct ServerConfig, the server must find the most appropriate LocationConfig within that server block to handle the request. This is done by matching the request's URI against the patterns in each location block. Following NGINX's algorithm, the server first checks for an exact match (e.g., location \= /path) and, if none is found, proceeds to find the longest prefix match (e.g., location /path/ would be chosen over location /).7 The rules defined in this most specific matching location block will then be applied to the request.

This structured approach ensures that every request is deterministically mapped to a specific set of configuration rules, forming the basis for all subsequent processing, from serving static files to executing CGI scripts.

## **IV. Deconstructing the Request: A Protocol-Compliant Parser**

The heart of an HTTP server is its ability to correctly and robustly parse incoming request messages. This parser must be strictly compliant with the grammar defined in RFC 2616 and resilient to malformed input.25 Furthermore, due to the non-blocking I/O architecture, the parser cannot assume it will receive the entire request at once. It must be designed as a re-entrant state machine, capable of processing data in arbitrary chunks as it arrives from the network.

### **4.1. Parsing the Request-Line**

The first line of any HTTP request is the Request-Line, which establishes the client's intent.

* **Format:** As defined in RFC 2616, Section 5.1, the format is Method SP Request-URI SP HTTP-Version CRLF.25  
* **Implementation:** The server will buffer incoming data from the client socket until it encounters the first CRLF sequence. This buffered line is then parsed by splitting it on the single space (SP) delimiters.  
* **Validation:** Each component must be rigorously validated:  
  * **Method:** The method token must be one of the implemented methods: GET, POST, or DELETE. If an unsupported or unrecognized method is received, the server must respond with 501 Not Implemented. If the method is recognized but not allowed for the target resource (as determined by the configuration), the response must be 405 Method Not Allowed.  
  * **HTTP-Version:** The version string must be validated. For this server, HTTP/1.1 is expected. If a request is received with a version the server does not support (e.g., HTTP/2.0), it must respond with 505 HTTP Version Not Supported.  
  * **Request-URI:** The URI must be parsed and sanitized. A critical security measure is to normalize the path (e.g., resolving . and .. components) and ensure that the resulting path does not escape the configured document root, preventing path traversal attacks.

### **4.2. Parsing Headers**

Following the Request-Line, the client sends a series of header fields.

* **Format:** Each header follows the format field-name ":" \[ field-value \] CRLF. The header section is terminated by an empty line, consisting of only a CRLF.25  
* **Implementation:** The parser continues to read from the socket line by line. Each line is split at the first colon (:) into a key and a value. Per RFC 2616, header field names are case-insensitive, so they should be converted to a canonical form (e.g., lowercase) for consistent storage and lookup. The field value may be preceded by linear white space (LWS), which must be trimmed. Multiple headers with the same field name are permissible and should be combined into a single comma-separated list. The process terminates upon reading an empty line.

### **4.3. Handling the Message Body**

The presence and length of a message body are determined by the Content-Length and Transfer-Encoding headers.25 The parser must handle both mechanisms.

* **Content-Length:** If this header is present, its value indicates the exact size of the message body in bytes. The server must read precisely this number of bytes from the socket. The parsed length must be checked against the client\_max\_body\_size limit defined in the configuration. If the length exceeds the limit, the server must immediately stop reading and respond with 413 Request Entity Too Large.  
* **Transfer-Encoding: chunked:** This encoding is used when the total size of the body is not known in advance. It requires a more complex parsing logic detailed in RFC 2616, Section 3.6.1.25 The server must implement the following decoding algorithm:  
  1. Read a line from the socket. This line contains the size of the next chunk, encoded as a hexadecimal string.  
  2. Parse the hexadecimal value. If the size is zero, this signifies the end of the message body. The parser then proceeds to read any optional trailer headers until a final empty CRLF line is received.  
  3. If the size is greater than zero, the parser reads exactly that number of bytes from the socket. This data constitutes the chunk.  
  4. Immediately following the chunk data, a CRLF sequence must be read and discarded.  
  5. The process repeats from step 1 until the zero-sized chunk is received.

The design of the parser as a re-entrant state machine is not an optional refinement but a fundamental requirement of the non-blocking architecture. Data can arrive from a TCP socket in fragments of any size. A single read() call might yield half of a request line, multiple complete headers, or the end of the headers and the beginning of the body. A simple procedural parser would fail. Instead, the parser must be implemented as an object that maintains its current state (e.g., PARSING\_REQUEST\_LINE, PARSING\_HEADERS, PARSING\_CHUNK\_SIZE, PARSING\_CHUNK\_DATA) and an internal buffer. Each time new data is available from the socket, it is appended to the buffer, and the state machine is run to consume as much of the buffer as possible, transitioning between states as it completes each part of the HTTP message. This design ensures that the server can correctly assemble a full request from an unpredictable stream of incoming data packets.

## **V. Crafting the Response: From Static Files to Dynamic Content**

Once a request has been fully parsed and routed, the server's next task is to generate and transmit a protocol-compliant HTTP/1.1 response. This process involves constructing a status line, appropriate headers, and a message body, tailored to the specific request handler—be it serving a static file, generating a directory listing, or issuing a redirection.

### **5.1. The Anatomy of an HTTP Response**

A valid HTTP response is a structured message composed of three parts, as specified in RFC 2616, Section 6\.25

* **Status-Line:** The first line of the response, with the format HTTP-Version SP Status-Code SP Reason-Phrase CRLF.  
  * The HTTP-Version will be HTTP/1.1.  
  * The Status-Code is a 3-digit integer indicating the result of the request (e.g., 200, 404, 500).  
  * The Reason-Phrase is a short, human-readable text corresponding to the status code (e.g., "OK", "Not Found"). The server will include a utility to map standard status codes to their recommended reason phrases.25  
* **Response Headers:** A series of key-value pairs providing metadata about the response. Several headers are essential for correct operation:  
  * Date: The time and date of message generation, in RFC 1123 format (e.g., Tue, 15 Nov 1994 08:12:31 GMT). This header is mandatory in all responses except for certain 1xx and 5xx statuses.25  
  * Server: A header identifying the server software (e.g., Server: webserv/1.0).  
  * Content-Length: The size of the response body in bytes. It is required for any response containing a body unless chunked encoding is used.  
  * Content-Type: The MIME type of the response body (e.g., text/html, image/jpeg), which is critical for the browser to render the content correctly.  
* **Message Body:** The actual content being returned to the client, such as an HTML page or an image file. This part is optional and its presence is determined by the request method and response status code. For instance, responses to HEAD requests and responses with 204 No Content or 304 Not Modified status codes MUST NOT include a message body.25

### **5.2. Serving Static Content**

The most common function of a web server is to serve static files from the local filesystem.

* **Path Resolution and Security:** The handler constructs the full filesystem path by combining the root directive from the matched location block with the sanitized request URI. A critical security step is to ensure that this resolved path is still within the confines of the specified root directory. This prevents path traversal attacks where a malicious URI like /../../etc/passwd could be used to access sensitive system files.  
* **File Handling and Response Generation:** The server attempts to open the file for reading.  
  * **Success:** If the file is opened successfully, a 200 OK response is generated. The Content-Length header is set to the size of the file, and the Content-Type header is determined by mapping the file's extension to a known list of MIME types (e.g., .html \-\> text/html, .jpg \-\> image/jpeg). The file's contents are then read into the response body buffer.  
  * **Error Handling:** If the file does not exist, a 404 Not Found response is generated. If the file exists but the server process lacks read permissions, a 403 Forbidden response is sent. For these error conditions, the server will serve a default error page or a custom one if specified by the error\_page directive in the configuration.

### **5.3. Handling Directory Requests**

When a request URI points to a directory, the server's behavior is governed by the index and autoindex directives.

* **Index File Resolution:** The server first checks for the existence of any files listed in the index directive (e.g., index.html, index.htm) within the requested directory. It checks for them in the specified order. If an index file is found, the server serves that file with a 200 OK response, effectively treating the request as a request for that file.13  
* **Automatic Directory Listing:** If no configured index file is found and the autoindex directive for the matched location is set to on, the server must dynamically generate a directory listing.21 This involves reading the directory's contents (files and subdirectories) and creating an HTML page. This page will contain a list of hyperlinks, allowing the user to navigate the filesystem through their browser. If  
  autoindex is off (the default), and no index file is found, the server will return a 404 Not Found or 403 Forbidden error.

### **5.4. HTTP Redirections**

The configuration may specify that certain URIs should be redirected to another location.

* **Implementation:** If the request matches a location block containing a return directive, the server's normal request processing is halted.18 It immediately constructs a redirection response. The status code will be the one specified in the directive (typically  
  301 Moved Permanently or 302 Found), and the Location header will be set to the target URL specified in the directive. The response will typically have no message body, though a small HTML body with a link to the new location is recommended for compatibility with older clients.

## **VI. Executing Dynamic Logic: The Common Gateway Interface (CGI)**

To serve dynamic content, Webserv must implement the Common Gateway Interface (CGI), a standard protocol that allows a web server to execute external scripts and programs. The implementation must be fully compliant with RFC 3875 (CGI/1.1) and, critically, must integrate seamlessly with the server's non-blocking, event-driven architecture.27 This involves careful process management (

fork, execve) and non-blocking inter-process communication using pipes.

### **6.1. The CGI Execution Flow**

The interaction between the server and a CGI script is a well-defined sequence of operations:

1. **Request Identification:** The server identifies a request as a CGI request based on the configuration (e.g., a cgi\_pass directive matching the file extension of the request URI).  
2. **Inter-Process Communication Setup:** Before creating the CGI process, the server must establish a way to communicate with it. Two pipes are created using the pipe() system call:  
   * server\_to\_cgi: Used by the server to send the HTTP request body to the script's standard input.  
   * cgi\_to\_server: Used by the script to send its response (via standard output) back to the server.  
3. **Process Creation:** The server calls fork() to create a new child process. This is the only context in which fork() is permitted.  
4. **Child Process Execution:** The code path within the child process is solely dedicated to preparing for and executing the CGI script.  
   * It uses dup2() to redirect its standard input (STDIN\_FILENO) to the read end of the server\_to\_cgi pipe and its standard output (STDOUT\_FILENO) to the write end of the cgi\_to\_server pipe. This effectively connects the script's standard I/O to the server.  
   * It changes its working directory to the directory of the script to ensure correct relative path resolution within the script.  
   * It prepares the environment variables as specified by the CGI standard (see Section 6.2).  
   * Finally, it calls execve() to replace its own process image with that of the CGI interpreter or script (e.g., /usr/bin/python with the script path as an argument). If execve() succeeds, the child process is now running the CGI script; if it fails, the child process exits with an error status.  
5. **Parent Process Management:** The main server process continues its execution path.  
   * It closes the unused ends of the two pipes (the write end of server\_to\_cgi and the read end of cgi\_to\_server).  
   * It sets the remaining pipe file descriptors to non-blocking mode using fcntl(). This is crucial to prevent the server from blocking while writing to or reading from the CGI process.  
   * These new, non-blocking file descriptors are added to the main poll() monitoring set. The connection's state is updated to reflect that it is now in a CGI-handling phase, ready to shuttle data between the client socket and the CGI pipes.  
   * The server must also monitor the child process for termination using waitpid() with the WNOHANG option to prevent creating zombie processes.

### **6.2. Crafting the CGI Environment (Meta-Variables)**

The CGI specification defines a contract for how a server communicates request metadata to a script. This contract is fulfilled by setting a specific set of environment variables before calling execve().30 The script reads these variables to understand the context of the request. The accuracy of these variables is paramount for correct CGI script execution. The following table outlines the essential meta-variables and their corresponding sources from the HTTP request.

| Meta-Variable | Source in HTTP Request | RFC 3875 Reference |
| :---- | :---- | :---- |
| GATEWAY\_INTERFACE | Static string: "CGI/1.1" | Section 4.1.4 |
| SERVER\_PROTOCOL | From the request line (e.g., "HTTP/1.1") | Section 4.1.14 |
| REQUEST\_METHOD | From the request line (e.g., "GET", "POST") | Section 4.1.12 |
| SCRIPT\_NAME | The virtual path to the script itself | Section 4.1.13 |
| PATH\_INFO | The part of the URI path after the script name | Section 4.1.5 |
| QUERY\_STRING | The part of the URI after the '?' | Section 4.1.7 |
| CONTENT\_LENGTH | From the Content-Length header (for POST) | Section 4.1.2 |
| CONTENT\_TYPE | From the Content-Type header (for POST) | Section 4.1.3 |
| REMOTE\_ADDR | IP address of the client | Section 4.1.8 |
| SERVER\_NAME | Hostname from the Host header or config | Section 4.1.14 |
| SERVER\_PORT | Port number the connection was accepted on | Section 4.1.15 |
| HTTP\_\* | All other request headers, prefixed with HTTP\_, with hyphens converted to underscores and in uppercase (e.g., User-Agent becomes HTTP\_USER\_AGENT) | Section 4.1.18 |

This mapping forms the core specification for the CGI handler. It defines the precise data transformation that must occur between the parsed HTTP request object and the environment of the forked CGI process.

### **6.3. Non-Blocking I/O with CGI Pipes**

The greatest challenge in CGI implementation within an event-driven server is managing the data flow between the client, the server, and the CGI process without blocking the main event loop.

* **Data Flow Management:** The connection's state machine is extended with CGI-specific states (WRITING\_TO\_CGI, READING\_FROM\_CGI). The poll() call now monitors the client socket and both CGI pipe ends.  
  * If the client socket is readable and the request has a body, the server reads data and buffers it for the CGI script.  
  * If the server\_to\_cgi pipe is writable (POLLOUT), the server writes a chunk of the buffered request body to the script.  
  * If the cgi\_to\_server pipe is readable (POLLIN), the server reads a chunk of the script's output and buffers it for the client.  
  * If the client socket is writable (POLLOUT), the server writes a chunk of the buffered CGI response to the client.  
* **CGI Response Parsing:** The output from a CGI script is not raw data; it is a "CGI response" which can be either a **document response** or a **redirect response**.  
  * **Document Response:** The script's output begins with HTTP-like headers (e.g., Content-Type: text/html, Status: 201 Created), followed by a blank line, and then the response body. The server must parse these headers to construct the final HTTP response for the client. If no Status header is provided, a 200 OK is assumed.  
  * **Redirect Response:** If the script outputs a Location header, the server must generate a redirection response (e.g., 302 Found) to the client.

The server acts as a non-blocking intermediary, orchestrating the flow of data between the client and the CGI process, all within the single-threaded event loop.

## **VII. Handling Complex Payloads: File Uploads via multipart/form-data**

A modern web server must be capable of handling file uploads, which are typically sent in POST requests using the multipart/form-data content type. This encoding, defined in RFC 7578, allows multiple pieces of data, including files, to be sent in a single request body.31 Parsing this format requires specialized logic that can operate efficiently on a stream of data without buffering entire large files in memory.

### **7.1. Understanding multipart/form-data**

The structure of a multipart/form-data body is analogous to a MIME message:

* **Boundary:** The Content-Type header of the HTTP request specifies the media type as multipart/form-data and includes a mandatory boundary parameter. This boundary is a unique string that is used to separate the different parts of the message body.31  
* **Parts:** The body consists of one or more parts. Each part begins with a boundary delimiter (-- followed by the boundary string and a CRLF). The entire message is terminated with a final boundary delimiter that has two additional hyphens at the end (--boundary--CRLF).  
* **Part Headers:** Each part has its own set of headers, separated from the part's content by a blank line (CRLFCRLF). The most important header is Content-Disposition.  
* **Content-Disposition Header:** For multipart/form-data, this header must have a value of form-data. It also includes a name parameter, which corresponds to the name of the HTML form field. For file uploads, it will also include a filename parameter, providing the original name of the uploaded file on the client's machine.31 An optional  
  Content-Type header may also be present within a part to specify the MIME type of the uploaded file.

### **7.2. A Streaming Multipart Parser**

Because file uploads can be very large, it is infeasible to buffer the entire request body in memory. The parser must be designed to process the incoming data as a stream, writing file content directly to a temporary location on disk. This requires a sophisticated state machine that can identify boundaries and headers while streaming the payload.

The parsing algorithm proceeds as follows:

1. **Initialization:** The parser extracts the boundary string from the Content-Type header of the main HTTP request.  
2. **Boundary Detection:** The parser reads data from the client socket into an internal buffer. It continuously scans this buffer for the boundary delimiter string.  
3. **Part Header Parsing:** Upon finding a boundary, the parser transitions to a state where it reads and parses the headers for that specific part. It reads line by line until it finds the blank line that signals the end of the part's headers.  
4. **File Handling:** From the parsed Content-Disposition header, the parser determines if the part represents a file (by checking for the filename parameter). If it is a file, it generates a unique temporary filename and opens a file for writing in the directory specified by the upload\_path configuration directive.  
5. **Data Streaming:** The parser transitions to a data streaming state. It reads subsequent data from the socket and writes it directly to the opened temporary file. This continues until the parser detects the next boundary string in the input stream. This is the most critical step, as it involves carefully managing the internal buffer to ensure that the start of the next boundary is not accidentally written to the current file.  
6. **Finalization:** When the next boundary is found, the temporary file is closed. The parser then repeats the process from step 3 for the next part. The entire process concludes when the final boundary delimiter (with trailing hyphens) is found.

This implementation represents a dual state machine. The main connection manager, which handles the non-blocking socket I/O, acts as the outer machine. It feeds incoming data chunks to the multipart parser, which is the inner state machine. The multipart parser maintains its own state (e.g., SEEKING\_BOUNDARY, PARSING\_PART\_HEADERS, STREAMING\_PART\_DATA) to correctly interpret the structured multipart stream and manage the file I/O. This separation of concerns is key to creating a clean, robust, and memory-efficient file upload handler.

## **VIII. Systemic Resilience: A Strategy for Error Handling Without Exceptions**

A primary requirement for Webserv is that it must not crash. In modern C++, exceptions are the standard mechanism for handling and propagating errors. However, under the C++98 constraint and a potential project-level decision to disable exceptions for performance or determinism, a rigorous and disciplined alternative is required. This strategy must reliably detect errors, propagate them cleanly, and allow the server to degrade gracefully without terminating.

### **8.1. The Problem with C-Style Error Handling**

The traditional C approach to error handling involves checking the return value of every function and, for system calls, inspecting the global errno variable when a failure is indicated.33 While functional, this method has significant drawbacks:

* **Code Obfuscation:** Error-checking logic (if (result \== \-1) {... }) becomes interleaved with the main application logic, leading to deeply nested conditionals that are difficult to read, reason about, and maintain.  
* **Error-Prone:** It is easy for a developer to forget to check a return value. Such an omission can lead to the program continuing in an undefined state with corrupted data, often leading to a crash far from the original point of failure.  
* **Ambiguity of EAGAIN/EWOULDBLOCK:** In a non-blocking server, errno being set to EAGAIN or EWOULDBLOCK after a read() or write() call is not a true error. It is an expected, normal condition indicating that the operation cannot complete at this moment.3 The error handling mechanism must clearly and easily distinguish these expected conditions from genuine, fatal errors (e.g.,  
  EBADF for an invalid file descriptor).

### **8.2. A Unified Error Handling Strategy**

To overcome these challenges, Webserv will adopt a unified strategy that combines return codes with simple status objects, ensuring that errors are handled consistently and explicitly throughout the codebase.

* **Status Enum:** Functions that can fail will return a status enumeration instead of a simple integer. This makes the code self-documenting.  
  C++  
  enum OperationStatus {  
      OK,  
      ERROR,  
      WOULD\_BLOCK // For non-blocking I/O  
  };

* **System Call Wrappers:** All direct system calls (socket, bind, read, poll, etc.) will be wrapped in thin utility functions. These wrappers will perform the underlying system call, check its return value and errno, and translate the result into the OperationStatus enum. This centralizes the messy details of errno checking and isolates the main application logic from platform-specific error codes.  
* **Output Parameters for Return Values:** For functions that need to return both a status and a value (e.g., the number of bytes read), the value will be passed back through a reference or pointer argument.  
  C++  
  // Instead of: int bytes \= read(fd,...); if (bytes \== \-1)...  
  // Use:  
  OperationStatus result \= safe\_read(fd, buffer, \&bytes\_read);  
  if (result \== ERROR) { /\* handle fatal error \*/ }

This approach forces the calling code to acknowledge the return status, making it much harder to ignore an error, while clearly separating the "happy path" from error handling logic.

### **8.3. Graceful Degradation**

The server's response to an error depends on its category. The goal is always to handle the error in a way that isolates the fault to a single connection, allowing the server to remain operational for all other clients.

* **Client Errors (4xx):** These are errors caused by invalid client input, such as a malformed request line, a request for a non-existent resource, or an attempt to use a disallowed method. The server's response is to:  
  1. Log the error for diagnostic purposes.  
  2. Generate and send an appropriate 4xx HTTP status code (e.g., 400 Bad Request, 404 Not Found).  
  3. If the error is recoverable (e.g., a single malformed request on a persistent connection), reset the connection state to AWAITING\_REQUEST. If the error is fatal to the connection (e.g., a framing error), close the connection.  
* **Server Errors (5xx):** These are internal failures that prevent the server from fulfilling a valid request. Examples include failing to fork() a CGI process due to resource limits, being unable to open a file due to running out of file descriptors, or encountering a critical logic bug. The server's response must be to:  
  1. Log a detailed diagnostic message, including the errno and context.  
  2. Attempt to send a 500 Internal Server Error or 503 Service Unavailable response to the client.  
  3. Immediately close the connection and release all associated resources to prevent the error state from propagating.

This strategy positions the main event loop as the ultimate safety net. Individual connection handlers will execute operations and, upon encountering a fatal error, will propagate an ERROR status up to the main loop. The main loop, which has a global view of all server resources, is then responsible for the final, authoritative cleanup of the failed connection. It will ensure the socket is closed, all buffers are deallocated, any associated CGI processes are terminated, and the file descriptor is removed from the poll() set. This architectural design ensures that an unrecoverable error in one connection is cleanly isolated and handled, allowing the server to continue processing requests for all other clients, thereby achieving the highest standard of resilience.

#### **Works cited**

1. Chapter 6\. I/O Multiplexing: The select and poll Functions \- Shichao's Notes, accessed August 18, 2025, [https://notes.shichao.io/unp/ch6/](https://notes.shichao.io/unp/ch6/)  
2. 60B Over 10K: Measuring select() vs poll() vs epoll() for non ..., accessed August 18, 2025, [https://medium.com/@seantywork/60b-over-10k-measuring-select-vs-poll-vs-epoll-for-non-blocking-tcp-sockets-38d64f23319f](https://medium.com/@seantywork/60b-over-10k-measuring-select-vs-poll-vs-epoll-for-non-blocking-tcp-sockets-38d64f23319f)  
3. Non-blocking I/O in C++ clarification : r/cpp\_questions \- Reddit, accessed August 18, 2025, [https://www.reddit.com/r/cpp\_questions/comments/5s77f3/nonblocking\_io\_in\_c\_clarification/](https://www.reddit.com/r/cpp_questions/comments/5s77f3/nonblocking_io_in_c_clarification/)  
4. How do I set a socket to be non-blocking? \- Jim Fisher, accessed August 18, 2025, [https://jameshfisher.com/2017/04/05/set\_socket\_nonblocking/](https://jameshfisher.com/2017/04/05/set_socket_nonblocking/)  
5. Resource acquisition is initialization \- Wikipedia, accessed August 18, 2025, [https://en.wikipedia.org/wiki/Resource\_acquisition\_is\_initialization](https://en.wikipedia.org/wiki/Resource_acquisition_is_initialization)  
6. Resource Acquisition is Initialization (RAII) in C++ \- W3computing.com, accessed August 18, 2025, [https://www.w3computing.com/articles/resource-acquisition-is-initialization-raii-in-cpp/](https://www.w3computing.com/articles/resource-acquisition-is-initialization-raii-in-cpp/)  
7. Beginner's Guide \- nginx, accessed August 18, 2025, [http://nginx.org/en/docs/beginners\_guide.html](http://nginx.org/en/docs/beginners_guide.html)  
8. What language are nginx conf files? \- Stack Overflow, accessed August 18, 2025, [https://stackoverflow.com/questions/2936260/what-language-are-nginx-conf-files](https://stackoverflow.com/questions/2936260/what-language-are-nginx-conf-files)  
9. Create NGINX Plus and NGINX Configuration Files, accessed August 18, 2025, [https://docs.nginx.com/nginx/admin-guide/basic-functionality/managing-configuration-files/](https://docs.nginx.com/nginx/admin-guide/basic-functionality/managing-configuration-files/)  
10. Server names \- nginx, accessed August 18, 2025, [http://nginx.org/en/docs/http/server\_names.html](http://nginx.org/en/docs/http/server_names.html)  
11. Configuration file measurement units \- nginx, accessed August 18, 2025, [http://nginx.org/en/docs/syntax.html](http://nginx.org/en/docs/syntax.html)  
12. Limit File Upload Size in NGINX \- Rackspace Technology, accessed August 18, 2025, [https://docs.rackspace.com/docs/limit-file-upload-size-in-nginx](https://docs.rackspace.com/docs/limit-file-upload-size-in-nginx)  
13. Serve Static Content | NGINX Documentation, accessed August 18, 2025, [https://docs.nginx.com/nginx/admin-guide/web-server/serving-static-content/](https://docs.nginx.com/nginx/admin-guide/web-server/serving-static-content/)  
14. limit\_except, accessed August 18, 2025, [https://docs.asprain.cn/NginxHTTPServer/part0143.html](https://docs.asprain.cn/NginxHTTPServer/part0143.html)  
15. K000139614: The limit\_except directive breaks if conditions in a location block, accessed August 18, 2025, [https://my.f5.com/manage/s/article/K000139614](https://my.f5.com/manage/s/article/K000139614)  
16. Module ngx\_http\_access\_module \- nginx, accessed August 18, 2025, [http://nginx.org/en/docs/http/ngx\_http\_access\_module.html](http://nginx.org/en/docs/http/ngx_http_access_module.html)  
17. Using limit\_except to deny all except GET, HEAD and POST \- Server Fault, accessed August 18, 2025, [https://serverfault.com/questions/905708/using-limit-except-to-deny-all-except-get-head-and-post](https://serverfault.com/questions/905708/using-limit-except-to-deny-all-except-get-head-and-post)  
18. Nginx Rewrite URL Rules Examples \- DigitalOcean, accessed August 18, 2025, [https://www.digitalocean.com/community/tutorials/nginx-rewrite-url-rules](https://www.digitalocean.com/community/tutorials/nginx-rewrite-url-rules)  
19. Rewrite Rules in Nginx \- EngineYard, accessed August 18, 2025, [https://www.engineyard.com/blog/rewrite-rules-nginx/](https://www.engineyard.com/blog/rewrite-rules-nginx/)  
20. Module ngx\_http\_rewrite\_module \- nginx, accessed August 18, 2025, [http://nginx.org/en/docs/http/ngx\_http\_rewrite\_module.html](http://nginx.org/en/docs/http/ngx_http_rewrite_module.html)  
21. Module ngx\_http\_autoindex\_module \- nginx, accessed August 18, 2025, [http://nginx.org/en/docs/http/ngx\_http\_autoindex\_module.html](http://nginx.org/en/docs/http/ngx_http_autoindex_module.html)  
22. Enabling the Nginx Directory Index Listing \- KeyCDN Support, accessed August 18, 2025, [https://www.keycdn.com/support/nginx-directory-index](https://www.keycdn.com/support/nginx-directory-index)  
23. Nginx Location Directive Explained \- KeyCDN Support, accessed August 18, 2025, [https://www.keycdn.com/support/nginx-location-directive](https://www.keycdn.com/support/nginx-location-directive)  
24. Understanding Nginx Server and Location Block Selection Algorithms \- DigitalOcean, accessed August 18, 2025, [https://www.digitalocean.com/community/tutorials/understanding-nginx-server-and-location-block-selection-algorithms](https://www.digitalocean.com/community/tutorials/understanding-nginx-server-and-location-block-selection-algorithms)  
25. rfc2616.txt  
26. Creating Redirects with Nginx (Temporary and Permanent) | RunCloud Docs, accessed August 18, 2025, [https://runcloud.io/docs/creating-nginx-redirects](https://runcloud.io/docs/creating-nginx-redirects)  
27. CGI: RFC 3875 And How The World Wide Web Works \- YouTube, accessed August 18, 2025, [https://www.youtube.com/watch?v=3fk7PO5IRos](https://www.youtube.com/watch?v=3fk7PO5IRos)  
28. Common Gateway Interface \- Wikipedia, accessed August 18, 2025, [https://en.wikipedia.org/wiki/Common\_Gateway\_Interface](https://en.wikipedia.org/wiki/Common_Gateway_Interface)  
29. What is a Common Getaway Interface (CGI)? \- CooliceHost.com, accessed August 18, 2025, [https://coolicehost.com/billing/knowledgebase/article/101/what-is-a-common-getaway-interface-cgi/](https://coolicehost.com/billing/knowledgebase/article/101/what-is-a-common-getaway-interface-cgi/)  
30. Information on RFC 3875 » RFC Editor, accessed August 18, 2025, [https://www.rfc-editor.org/info/rfc3875](https://www.rfc-editor.org/info/rfc3875)  
31. RFC 7578 \- Returning Values from Forms: multipart/form-data, accessed August 18, 2025, [https://datatracker.ietf.org/doc/html/rfc7578](https://datatracker.ietf.org/doc/html/rfc7578)  
32. Multipart Form Data \- Unifi User Documentation, accessed August 18, 2025, [https://docs.sharelogic.com/unifi/troubleshooting/development/multipart-form-data](https://docs.sharelogic.com/unifi/troubleshooting/development/multipart-form-data)  
33. Exception Handling in C Without C++ \- On Time Informatik GmbH, accessed August 18, 2025, [http://www.on-time.com/ddj0011.htm](http://www.on-time.com/ddj0011.htm)  
34. How do I change a TCP socket to be non-blocking? \- Stack Overflow, accessed August 18, 2025, [https://stackoverflow.com/questions/1543466/how-do-i-change-a-tcp-socket-to-be-non-blocking](https://stackoverflow.com/questions/1543466/how-do-i-change-a-tcp-socket-to-be-non-blocking)