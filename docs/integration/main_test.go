package integration

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const (
	testHost = "localhost"
	testPort = 42069
	requestTimeout = 10 * time.Second
	serverStartupTimeout = 30 * time.Second
)

var (
	serverProcess *exec.Cmd
	testClient    *http.Client
)

// TestMain manages the server lifecycle for all tests
func TestMain(m *testing.M) {
	// Setup: Build and start the server
	if err := setupTestEnvironment(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to setup test environment: %v\n", err)
		os.Exit(1)
	}

	// Run all tests
	exitCode := m.Run()

	// Teardown: Stop the server and cleanup
	teardownTestEnvironment()

	os.Exit(exitCode)
}

// setupTestEnvironment builds the server and starts it for testing
func setupTestEnvironment() error {
	// Build the server
	if err := buildServer(); err != nil {
		return fmt.Errorf("failed to build server: %w", err)
	}

	// Start the server
	if err := startServer(); err != nil {
		return fmt.Errorf("failed to start server: %w", err)
	}

	// Create test client
	testClient = &http.Client{
		Timeout: requestTimeout,
	}

	return nil
}

// buildServer compiles the C++ webserv executable
func buildServer() error {
	fmt.Println("Building webserv server...")

	// Get project root directory
	projectRoot, err := filepath.Abs("../..")
	if err != nil {
		return fmt.Errorf("failed to get project root: %w", err)
	}

	// Run make debug to build the server
	cmd := exec.Command("make", "debug")
	cmd.Dir = projectRoot

	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("make failed: %w\nOutput: %s", err, output)
	}

	// Verify the binary exists
	binaryPath := filepath.Join(projectRoot, "build", "webserv")
	if _, err := os.Stat(binaryPath); os.IsNotExist(err) {
		return fmt.Errorf("webserv binary not found at %s", binaryPath)
	}

	fmt.Println("Server built successfully")
	return nil
}

// copyFile copies a file from src to dst
func copyFile(src, dst string) error {
	sourceFile, err := os.Open(src)
	if err != nil {
		return err
	}
	defer sourceFile.Close()

	destFile, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer destFile.Close()

	_, err = io.Copy(destFile, sourceFile)
	if err != nil {
		return err
	}

	// Make destination file executable
	return os.Chmod(dst, 0755)
}

// startServer launches the webserv process in the background
func startServer() error {
	fmt.Println("Starting test server...")

	// Get paths
	projectRoot, _ := filepath.Abs("../..")
	binaryPath := filepath.Join(projectRoot, "build", "webserv")

	// Copy binary to test-server directory like the bash script does
	testServerDir := "test-server"
	localBinaryPath := filepath.Join(testServerDir, "webserv")

	// Copy the binary
	if err := copyFile(binaryPath, localBinaryPath); err != nil {
		return fmt.Errorf("failed to copy binary: %w", err)
	}

	// Start the server process from test-server directory
	serverProcess = exec.Command("./webserv", "../test.conf")
	serverProcess.Dir = testServerDir

	// Redirect server output to file for debugging
	logFile, err := os.Create("test-server/server.log")
	if err != nil {
		return fmt.Errorf("failed to create log file: %w", err)
	}
	serverProcess.Stdout = logFile
	serverProcess.Stderr = logFile

	if err := serverProcess.Start(); err != nil {
		logFile.Close()
		return fmt.Errorf("failed to start server: %w", err)
	}

	// Wait for server to become ready
	if err := waitForServerReady(); err != nil {
		stopServer()
		return fmt.Errorf("server failed to become ready: %w", err)
	}

	fmt.Printf("Server started successfully (PID: %d)\n", serverProcess.Process.Pid)
	return nil
}

// waitForServerReady polls the server until it responds to requests
func waitForServerReady() error {
	fmt.Println("Waiting for server to be ready...")

	client := &http.Client{Timeout: 1 * time.Second}
	maxAttempts := 30

	for i := 0; i < maxAttempts; i++ {
		resp, err := client.Get(fmt.Sprintf("http://%s:%d/", testHost, testPort))
		if err == nil {
			resp.Body.Close()
			fmt.Println("Server is ready and responding")
			return nil
		}

		if i%5 == 0 {
			fmt.Printf("Waiting for server... (attempt %d/%d)\n", i+1, maxAttempts)
		}
		time.Sleep(1 * time.Second)
	}

	return fmt.Errorf("server did not become ready within %d seconds", maxAttempts)
}

// stopServer gracefully stops the server process
func stopServer() {
	if serverProcess != nil && serverProcess.Process != nil {
		fmt.Println("Stopping server...")
		serverProcess.Process.Kill()
		serverProcess.Wait()
		fmt.Println("Server stopped")
	}
}

// teardownTestEnvironment cleans up resources
func teardownTestEnvironment() {
	stopServer()

	// Clean up any test files
	os.Remove("test-server/server.log")
	os.Remove("test-server/webserv")
}

// TestWebservAPI is the main table-driven test function that covers all HTTP server functionality
//
// This table-driven approach provides several benefits:
// 1. Scalability: Adding new test cases is as simple as adding entries to the test table
// 2. Maintainability: All test scenarios are defined in one place with clear structure
// 3. Consistency: All tests use the same helper functions and assertion patterns
// 4. Isolation: Each test case runs in its own subtest with `t.Run()`, enabling parallel execution
// 5. Clarity: Test names and expected outcomes are explicitly defined in the table structure
//
// The table structure makes it easy to see at a glance what scenarios are covered and what
// the expected behavior should be for each case. This is particularly valuable for HTTP servers
// where there are many combinations of methods, paths, headers, and body content to test.
func TestWebservAPI(t *testing.T) {
	// Test case structure defines all the parameters needed for each HTTP test scenario
	type testCase struct {
		name           string            // Human-readable test case name
		method         string            // HTTP method (GET, POST, DELETE, etc.)
		uri            string            // Request URI path
		requestBody    string            // Request body content
		headers        map[string]string // Additional HTTP headers to send
		expectedStatus int               // Expected HTTP status code
		expectedBody   string            // Expected substring in response body
		setup          func(t *testing.T) // Optional setup function to run before test
		cleanup        func(t *testing.T) // Optional cleanup function to run after test
	}

	// Test table covering all major HTTP server functionality
	// Each entry represents a complete test scenario with inputs and expected outputs
	testTable := []testCase{
		// Static file serving tests
		{
			name:           "GetStaticIndex",
			method:         "GET",
			uri:            "/",
			requestBody:    "",
			expectedStatus: 200,
			expectedBody:   "html", // Expecting HTML content in index
		},
		{
			name:           "GetStaticFile",
			method:         "GET",
			uri:            "/index.html",
			requestBody:    "",
			expectedStatus: 200,
			expectedBody:   "html",
		},
		{
			name:           "GetNotFound",
			method:         "GET",
			uri:            "/does/not/exist.txt",
			requestBody:    "",
			expectedStatus: 404,
			expectedBody:   "Not Found",
		},

		// File upload tests
		{
			name:           "PostUploadSuccess",
			method:         "POST",
			uri:            "/upload",
			requestBody:    "Test file upload content for webserv",
			headers:        map[string]string{"Content-Type": "application/octet-stream"},
			expectedStatus: 200, // or 201 Created
			expectedBody:   "", // Success response may vary
		},
		{
			name:           "PostUploadBodyTooLarge",
			method:         "POST",
			uri:            "/upload",
			requestBody:    strings.Repeat("A", 2*1024*1024), // 2MB - exceeds 1MB limit
			headers:        map[string]string{"Content-Type": "application/octet-stream"},
			expectedStatus: 413,
			expectedBody:   "Too Large",
		},

		// HTTP method validation tests
		{
			name:           "DeleteSupportedPath",
			method:         "DELETE",
			uri:            "/",
			requestBody:    "",
			expectedStatus: 200, // May vary based on implementation
			expectedBody:   "",
		},
		{
			name:           "MethodNotAllowedOnStaticFile",
			method:         "PATCH", // Unsupported method
			uri:            "/index.html",
			requestBody:    "some data",
			expectedStatus: 405,
			expectedBody:   "Method Not Allowed",
		},
		{
			name:           "PutMethodNotImplemented",
			method:         "PUT",
			uri:            "/",
			requestBody:    "put data",
			expectedStatus: 405,
			expectedBody:   "Method Not Allowed",
		},

		// CGI execution tests
		{
			name:           "GetCGIScript",
			method:         "GET",
			uri:            "/cgi-bin/test.py",
			requestBody:    "",
			expectedStatus: 200, // May be 404 if CGI not configured
			expectedBody:   "", // CGI output varies
		},
		{
			name:           "PostCGIScript",
			method:         "POST",
			uri:            "/cgi-bin/test.py",
			requestBody:    "name=webserv&test=true",
			headers:        map[string]string{"Content-Type": "application/x-www-form-urlencoded"},
			expectedStatus: 200, // May be 404 if CGI not configured
			expectedBody:   "",
		},

		// Directory listing tests
		{
			name:           "GetDirectoryListing",
			method:         "GET",
			uri:            "/public/",
			requestBody:    "",
			expectedStatus: 200, // May be 403 if autoindex disabled
			expectedBody:   "", // Directory listing format varies
		},

		// Redirect tests
		{
			name:           "GetRedirect",
			method:         "GET",
			uri:            "/old",
			requestBody:    "",
			expectedStatus: 302, // Or other 3xx redirect code
			expectedBody:   "",
		},

		// Error handling tests
		{
			name:           "GetForbiddenPath",
			method:         "GET",
			uri:            "/../etc/passwd", // Path traversal attempt
			requestBody:    "",
			expectedStatus: 403,
			expectedBody:   "Forbidden",
		},

		// Virtual host tests
		{
			name:           "GetWithHostHeader",
			method:         "GET",
			uri:            "/",
			requestBody:    "",
			headers:        map[string]string{"Host": "example.local"},
			expectedStatus: 200,
			expectedBody:   "",
		},

		// Protocol compliance tests
		{
			name:           "GetWithKeepAlive",
			method:         "GET",
			uri:            "/",
			requestBody:    "",
			headers:        map[string]string{"Connection": "keep-alive"},
			expectedStatus: 200,
			expectedBody:   "",
		},
	}

	// Execute each test case in its own subtest
	// This provides isolation, clear naming, and enables parallel execution
	for _, tc := range testTable {
		tc := tc // Capture loop variable for closure
		t.Run(tc.name, func(t *testing.T) {
			// Enable parallel execution for performance
			t.Parallel()

			// Run optional setup
			if tc.setup != nil {
				tc.setup(t)
			}

			// Run optional cleanup after test completes
			if tc.cleanup != nil {
				defer tc.cleanup(t)
			}

			// Execute the HTTP request and validate response
			statusCode, statusText, responseBody := sendRequest(t, tc.method, tc.uri, tc.requestBody, tc.headers)

			// Assert status code matches expectation
			if statusCode != tc.expectedStatus {
				t.Errorf("Expected status %d, got %d (%s)", tc.expectedStatus, statusCode, statusText)
			}

			// Assert response body contains expected content (if specified)
			if tc.expectedBody != "" && !strings.Contains(responseBody, tc.expectedBody) {
				t.Errorf("Expected response body to contain %q, got: %s", tc.expectedBody, responseBody)
			}
		})
	}
}

// sendRequest is a test helper that handles HTTP request sending and response parsing
// This encapsulates the repetitive logic of creating requests, handling connections,
// and parsing responses, while providing clear error reporting through *testing.T
func sendRequest(t *testing.T, method, uri, body string, headers map[string]string) (statusCode int, statusText, responseBody string) {
	t.Helper() // Mark this as a test helper for better error location reporting

	// Create HTTP request
	var bodyReader io.Reader
	if body != "" {
		bodyReader = strings.NewReader(body)
	}

	url := fmt.Sprintf("http://%s:%d%s", testHost, testPort, uri)
	req, err := http.NewRequest(method, url, bodyReader)
	if err != nil {
		t.Fatalf("Failed to create request: %v", err)
	}

	// Add custom headers
	for key, value := range headers {
		req.Header.Set(key, value)
	}

	// Send request using test client
	resp, err := testClient.Do(req)
	if err != nil {
		t.Fatalf("Failed to send request: %v", err)
	}
	defer resp.Body.Close()

	// Read response body
	bodyBytes, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("Failed to read response body: %v", err)
	}

	return resp.StatusCode, resp.Status, string(bodyBytes)
}

// TestWebservMultipartUpload tests multipart form uploads using table-driven approach
// This is separated because multipart requires special request body construction
func TestWebservMultipartUpload(t *testing.T) {
	type multipartTestCase struct {
		name           string
		fieldName      string
		fileName       string
		fileContent    string
		expectedStatus int
		expectedBody   string
	}

	testTable := []multipartTestCase{
		{
			name:           "MultipartUploadSuccess",
			fieldName:      "file",
			fileName:       "test.txt",
			fileContent:    "This is test file content for multipart upload",
			expectedStatus: 200, // May be 201 Created
			expectedBody:   "",
		},
		{
			name:           "MultipartUploadLargeFile",
			fieldName:      "file",
			fileName:       "large.txt",
			fileContent:    strings.Repeat("Large file content ", 1000), // ~20KB
			expectedStatus: 200,
			expectedBody:   "",
		},
	}

	for _, tc := range testTable {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			// Create multipart form data
			var buf bytes.Buffer
			writer := multipart.NewWriter(&buf)

			fileWriter, err := writer.CreateFormFile(tc.fieldName, tc.fileName)
			if err != nil {
				t.Fatalf("Failed to create form file: %v", err)
			}

			_, err = fileWriter.Write([]byte(tc.fileContent))
			if err != nil {
				t.Fatalf("Failed to write file content: %v", err)
			}

			err = writer.Close()
			if err != nil {
				t.Fatalf("Failed to close multipart writer: %v", err)
			}

			// Send multipart request
			req, err := http.NewRequest("POST", fmt.Sprintf("http://%s:%d/upload", testHost, testPort), &buf)
			if err != nil {
				t.Fatalf("Failed to create request: %v", err)
			}
			req.Header.Set("Content-Type", writer.FormDataContentType())

			resp, err := testClient.Do(req)
			if err != nil {
				t.Fatalf("Failed to send request: %v", err)
			}
			defer resp.Body.Close()

			responseBody, _ := io.ReadAll(resp.Body)

			// Assert status code
			if resp.StatusCode != tc.expectedStatus {
				t.Errorf("Expected status %d, got %d", tc.expectedStatus, resp.StatusCode)
			}

			// Assert response body if specified
			if tc.expectedBody != "" && !strings.Contains(string(responseBody), tc.expectedBody) {
				t.Errorf("Expected response body to contain %q, got: %s", tc.expectedBody, responseBody)
			}
		})
	}
}

// TestWebservProtocolCompliance tests HTTP/1.1 protocol features using table-driven approach
func TestWebservProtocolCompliance(t *testing.T) {
	type protocolTestCase struct {
		name          string
		testFunc      func(t *testing.T)
		description   string
	}

	testTable := []protocolTestCase{
		{
			name:        "HTTP11Protocol",
			description: "Server responds with HTTP/1.1 protocol",
			testFunc: func(t *testing.T) {
				resp, err := testClient.Get(fmt.Sprintf("http://%s:%d/", testHost, testPort))
				if err != nil {
					t.Fatalf("Failed to send request: %v", err)
				}
				defer resp.Body.Close()

				if resp.Proto != "HTTP/1.1" {
					t.Errorf("Expected HTTP/1.1, got %s", resp.Proto)
				}
			},
		},
		{
			name:        "KeepAliveConnections",
			description: "Server supports keep-alive connections",
			testFunc: func(t *testing.T) {
				conn, err := net.Dial("tcp", fmt.Sprintf("%s:%d", testHost, testPort))
				if err != nil {
					t.Fatalf("Failed to connect: %v", err)
				}
				defer conn.Close()

				// Send multiple requests on same connection
				for i := 0; i < 3; i++ {
					request := fmt.Sprintf("GET /?req=%d HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n", i, testHost)
					_, err := conn.Write([]byte(request))
					if err != nil {
						t.Fatalf("Failed to write request %d: %v", i, err)
					}

					reader := bufio.NewReader(conn)
					_, err = http.ReadResponse(reader, nil)
					if err != nil {
						t.Fatalf("Failed to read response %d: %v", i, err)
					}
				}
			},
		},
		{
			name:        "ChunkedTransferEncoding",
			description: "Server handles chunked transfer encoding",
			testFunc: func(t *testing.T) {
				conn, err := net.Dial("tcp", fmt.Sprintf("%s:%d", testHost, testPort))
				if err != nil {
					t.Fatalf("Failed to connect: %v", err)
				}
				defer conn.Close()

				chunkedRequest := fmt.Sprintf(
					"POST /upload HTTP/1.1\r\n"+
						"Host: %s\r\n"+
						"Transfer-Encoding: chunked\r\n"+
						"Content-Type: text/plain\r\n"+
						"\r\n"+
						"7\r\n"+
						"Mozilla\r\n"+
						"9\r\n"+
						"Developer\r\n"+
						"7\r\n"+
						"Network\r\n"+
						"0\r\n"+
						"\r\n",
					testHost)

				_, err = conn.Write([]byte(chunkedRequest))
				if err != nil {
					t.Fatalf("Failed to write chunked request: %v", err)
				}

				reader := bufio.NewReader(conn)
				resp, err := http.ReadResponse(reader, nil)
				if err != nil {
					t.Fatalf("Failed to read response: %v", err)
				}
				defer resp.Body.Close()

				// Should not return 400 for valid chunked request
				if resp.StatusCode == 400 {
					t.Errorf("Server returned 400 for valid chunked request")
				}
			},
		},
	}

	for _, tc := range testTable {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			tc.testFunc(t)
		})
	}
}

// TestWebservStressAndRobustness tests server behavior under load using table-driven scenarios
func TestWebservStressAndRobustness(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping stress tests in short mode")
	}

	type stressTestCase struct {
		name         string
		numRequests  int
		numConcurrent int
		maxErrorRate float64
		description  string
	}

	testTable := []stressTestCase{
		{
			name:         "LightLoad",
			numRequests:  20,
			numConcurrent: 5,
			maxErrorRate:  0.1, // 10% error rate tolerance
			description:  "Server handles light concurrent load",
		},
		{
			name:         "ModerateLoad",
			numRequests:  50,
			numConcurrent: 10,
			maxErrorRate:  0.15, // 15% error rate tolerance
			description:  "Server handles moderate concurrent load",
		},
		{
			name:         "HeavyLoad",
			numRequests:  100,
			numConcurrent: 20,
			maxErrorRate:  0.2, // 20% error rate tolerance
			description:  "Server handles heavy concurrent load",
		},
	}

	for _, tc := range testTable {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			results := make(chan error, tc.numRequests)
			semaphore := make(chan struct{}, tc.numConcurrent)

			// Launch concurrent requests
			for i := 0; i < tc.numRequests; i++ {
				go func(id int) {
					semaphore <- struct{}{}        // Acquire
					defer func() { <-semaphore }() // Release

					resp, err := testClient.Get(fmt.Sprintf("http://%s:%d/?req=%d", testHost, testPort, id))
					if err != nil {
						results <- err
						return
					}
					resp.Body.Close()

					if resp.StatusCode >= 500 {
						results <- fmt.Errorf("request %d returned server error: %d", id, resp.StatusCode)
						return
					}

					results <- nil
				}(i)
			}

			// Collect results
			var errors []error
			for i := 0; i < tc.numRequests; i++ {
				if err := <-results; err != nil {
					errors = append(errors, err)
				}
			}

			// Check error rate
			errorRate := float64(len(errors)) / float64(tc.numRequests)
			if errorRate > tc.maxErrorRate {
				t.Errorf("Error rate %.2f exceeds maximum %.2f. Errors: %v", errorRate, tc.maxErrorRate, errors[:min(5, len(errors))])
			}
		})
	}
}

// Helper function for Go versions that don't have built-in min function
func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
