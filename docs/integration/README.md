# Integration Testing for Webserv

This directory contains comprehensive integration tests for the webserv HTTP/1.1 server implementation. The test suite includes both standalone functional tests and comparison tests against nginx as a reference implementation.

## Test Categories

### 1. Standalone Tests (`webserv_test.go`)
- Basic HTTP functionality (GET, POST, DELETE)
- Static file serving
- File uploads
- CGI execution
- Error handling
- Keep-alive connections
- Chunked transfer encoding
- Virtual hosts
- Directory listing
- HTTP redirects

### 2. Stress Tests (`stress_test.go`)
- High concurrency load testing
- Memory stability testing
- Mixed workload testing
- Performance benchmarking

### 3. Nginx Comparison Tests (`nginx_comparison_test.go`)
- Protocol compliance verification
- Response format comparison
- Error handling comparison
- Performance benchmarking against nginx

## Prerequisites

### Required
- Go 1.21+ (for running tests)
- curl (for basic connectivity checks)

### Optional (for nginx comparison)
- nginx (install with `make nginx-install` or `sudo apt-get install nginx`)

## Running Tests

### Quick Test
```bash
make test-integration
```

### Full Test Suite (with stress tests)
```bash
make test-stress
```

### With Nginx Comparison
```bash
make test-nginx
```

### Complete Test Suite
```bash
make test-full
```

### Manual Test Execution
```bash
cd tests/integration
./run_tests.sh [OPTIONS]

Options:
  -b, --bench     Run benchmarks after tests
  -c, --cleanup   Only cleanup running servers
  -n, --nginx     Include nginx comparison tests
  -h, --help      Show help message
```

## Nginx Comparison Testing

The nginx comparison feature allows you to validate webserv behavior against nginx as a reference implementation. This helps ensure protocol compliance and identify behavioral differences.

### Setup Nginx
```bash
# Check if nginx is available
make nginx-check

# Install nginx (requires sudo)
make nginx-install
```

### What Gets Compared
- HTTP status codes
- Response headers (Content-Type, Content-Length, Connection, etc.)
- Protocol compliance (HTTP/1.1, keep-alive behavior)
- Error handling (404, 405, etc.)
- Performance characteristics

### Example Comparison Output
```
Comparison for GET /:
  Webserv status: 200
  Nginx status: 200
  Status match: true
  Content-Type header match: true
  Content-Length header match: true
  Notes: ["Server headers differ (expected): webserv='webserv/0.0.1', nginx='nginx/1.28.0'"]
```

## Test Configuration

The tests use a dedicated configuration file (`test.conf`) that sets up:
- Multiple server blocks for virtual host testing
- Various route configurations
- CGI endpoints
- Upload endpoints
- Error page configurations
- Timeout settings

## Test Environment Structure

```
tests/integration/
├── run_tests.sh              # Main test runner
├── nginx_setup.sh            # Nginx installation helper
├── test.conf                 # Webserv test configuration
├── go.mod                    # Go module definition
├── webserv_test.go          # Core functionality tests
├── stress_test.go           # Performance/stress tests
├── nginx_comparison_test.go  # Nginx comparison tests
├── nginx_test.go            # Nginx setup verification
└── test-server/             # Test server directory
    ├── www/                 # Static content
    │   ├── index.html
    │   ├── public/          # Directory listing test
    │   ├── cgi-bin/         # CGI scripts
    │   └── assets/          # Additional static files
    ├── uploads/             # Upload destination
    └── errors/              # Custom error pages
```

## Interpreting Test Results

### Successful Tests
- All basic functionality tests should pass
- Stress tests should show reasonable performance
- Nginx comparison tests may show differences (this is often expected)

### Expected Differences with Nginx
- Server identification headers
- Exact response formatting
- Some timing-dependent behaviors
- CGI implementation details

### Failure Investigation
1. Check server logs in `test-server/server.log`
2. Verify configuration in `test.conf`
3. Ensure all test files are present in `test-server/`
4. Check for port conflicts (42069 for webserv, 8080 for nginx)

## Adding New Tests

### Standalone Tests
Add new test functions to `webserv_test.go`:
```go
func TestNewFeature(t *testing.T) {
    client := createTestClient()
    // Test implementation
}
```

### Nginx Comparison Tests
Add new comparison tests to `nginx_comparison_test.go`:
```go
func TestNewComparison(t *testing.T) {
    nginx := NewNginxComparison("test-server")
    if !nginx.IsAvailable() {
        t.Skip("Nginx not available")
    }
    // Comparison implementation
}
```

## Troubleshooting

### "Server failed to start"
- Check if ports 42069/8080 are available
- Verify webserv binary exists in `build/webserv`
- Check configuration file syntax

### "Nginx not available"
- Install nginx: `make nginx-install`
- Check PATH: `which nginx`
- Verify permissions

### "Connection refused"
- Ensure server is starting properly
- Check firewall settings
- Verify test configuration

### Performance Issues
- Reduce concurrency in stress tests
- Check system resources (memory, CPU)
- Adjust timeout values

## Continuous Integration

For CI environments, use:
```bash
make test-ci  # Runs with timeout, skips stress tests
```

This provides basic validation without resource-intensive testing.
