#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEST_DIR="$SCRIPT_DIR"
SERVER_DIR="$TEST_DIR/test-server"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "${GREEN}[TEST]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Build the server if needed
build_server() {
    log "Building webserv server..."
    cd "$PROJECT_ROOT"
    make debug
    if [ ! -f "./build/webserv" ]; then
        error "Failed to build webserv binary"
        exit 1
    fi
    log "Server built successfully"
}

# Start test server
start_server() {
    log "Starting test server..."
    cd "$SERVER_DIR"

    # Copy server binary to test directory
    cp "$PROJECT_ROOT/build/webserv" ./

    # Start server in background
    ./webserv ../test.conf > server.log 2>&1 &
    SERVER_PID=$!

    # Wait for server to start
    log "Waiting for server to start (PID: $SERVER_PID)..."
    sleep 2

    # Check if server is running
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        error "Server failed to start"
        if [ -f server.log ]; then
            cat server.log
        fi
        exit 1
    fi

    # Test if server is responding
    for i in {1..10}; do
        if curl -s --connect-timeout 1 http://localhost:42069/ > /dev/null 2>&1; then
            log "Server is responding on port 42069"
            return 0
        fi
        log "Waiting for server to be ready... (attempt $i/10)"
        sleep 1
    done

    error "Server is not responding after 10 seconds"
    stop_server
    exit 1
}

# Stop test server
stop_server() {
    log "Stopping test server..."
    if [ ! -z "$SERVER_PID" ] && kill -0 $SERVER_PID 2>/dev/null; then
        kill $SERVER_PID
        wait $SERVER_PID 2>/dev/null || true
        log "Server stopped"
    fi
}

# Run Go tests
run_tests() {
    log "Running integration tests..."
    cd "$TEST_DIR"

    # Initialize Go module if needed
    if [ ! -f go.sum ]; then
        go mod tidy
    fi

    # Run tests
    if go test -v -timeout 30s ./...; then
        log "All tests passed!"
        return 0
    else
        error "Some tests failed"
        return 1
    fi
}

# Run benchmarks
run_benchmarks() {
    log "Running benchmarks..."
    cd "$TEST_DIR"
    go test -bench=. -benchmem ./...
}

# Run nginx comparison tests
run_nginx_comparison() {
    log "Running nginx comparison tests..."
    cd "$TEST_DIR"
    
    # Check if nginx is available
    if ! command -v nginx >/dev/null 2>&1 && [ ! -f /usr/sbin/nginx ] && [ ! -f /usr/bin/nginx ]; then
        warn "Nginx not found, skipping comparison tests"
        warn "Install nginx to enable comparison testing: sudo apt-get install nginx"
        return 0
    fi
    
    # Run only nginx comparison tests
    go test -v -run "TestComparison|TestProtocolCompliance|TestErrorHandling" ./...
    
    if [ $? -eq 0 ]; then
        log "Nginx comparison tests completed successfully!"
    else
        warn "Some nginx comparison tests failed (this may be expected due to implementation differences)"
    fi
}

# Main execution
main() {
    local run_benchmarks=false
    local cleanup_only=false
    local include_nginx=false
    
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -b|--bench)
                run_benchmarks=true
                shift
                ;;
            -c|--cleanup)
                cleanup_only=true
                shift
                ;;
            -n|--nginx)
                include_nginx=true
                shift
                ;;
            -h|--help)
                echo "Usage: $0 [OPTIONS]"
                echo "Options:"
                echo "  -b, --bench     Run benchmarks after tests"
                echo "  -c, --cleanup   Only cleanup running servers"
                echo "  -n, --nginx     Include nginx comparison tests"
                echo "  -h, --help      Show this help message"
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                exit 1
                ;;
        esac
    done    # Cleanup function
    cleanup() {
        log "Cleaning up..."
        stop_server
        # Kill any remaining webserv processes on test ports
        pkill -f "webserv.*test.conf" 2>/dev/null || true
        # Clean up test files
        rm -f "$SERVER_DIR/webserv" "$SERVER_DIR/server.log"
    }

    # Set up cleanup trap
    trap cleanup EXIT

    if [ "$cleanup_only" = true ]; then
        cleanup
        exit 0
    fi

    # Create necessary directories
    mkdir -p "$SERVER_DIR/uploads"

    # Build and start server
    build_server
    start_server

    # Run tests
    if run_tests; then
        if [ "$run_benchmarks" = true ]; then
            run_benchmarks
        fi
        if [ "$include_nginx" = true ]; then
            run_nginx_comparison
        fi
        log "Integration testing completed successfully!"
        exit 0
    else
        error "Integration testing failed!"
        # Show server logs for debugging
        if [ -f "$SERVER_DIR/server.log" ]; then
            warn "Server logs:"
            cat "$SERVER_DIR/server.log"
        fi
        exit 1
    fi
}

# Run main function
main "$@"
