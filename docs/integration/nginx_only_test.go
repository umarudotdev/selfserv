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
	nginxTestHost = "localhost"
	nginxTestPort = 8080
	requestTimeout = 10 * time.Second
)

var (
	nginxInstance *NginxComparison
	nginxClient   *http.Client
)

// NginxComparison manages nginx instance for testing
type NginxComparison struct {
	workDir    string
	nginxPath  string
	configPath string
	pidFile    string
	running    bool
}

// NewNginxComparison creates a new nginx comparison instance
func NewNginxComparison(workDir string) *NginxComparison {
	return &NginxComparison{
		workDir:    workDir,
		nginxPath:  findNginxBinary(),
		configPath: filepath.Join(workDir, "nginx.conf"),
		pidFile:    filepath.Join(workDir, "nginx.pid"),
		running:    false,
	}
}

// findNginxBinary attempts to locate nginx binary
func findNginxBinary() string {
	candidates := []string{
		"/usr/sbin/nginx",
		"/usr/bin/nginx",
		"/usr/local/bin/nginx",
		"/opt/nginx/sbin/nginx",
	}

	for _, path := range candidates {
		if _, err := os.Stat(path); err == nil {
			return path
		}
	}

	// Try PATH
	if path, err := exec.LookPath("nginx"); err == nil {
		return path
	}

	return ""
}

// IsAvailable checks if nginx is available for testing
func (n *NginxComparison) IsAvailable() bool {
	return n.nginxPath != ""
}

// GenerateConfig creates nginx configuration for testing
func (n *NginxComparison) GenerateConfig() error {
	config := fmt.Sprintf(`
# Nginx configuration for HTTP server baseline testing
daemon off;
error_log stderr info;
pid %s;

events {
    worker_connections 1024;
    use epoll;
}

http {
    # Basic MIME types
    types {
        text/html                             html htm shtml;
        text/css                              css;
        text/xml                              xml;
        image/gif                             gif;
        image/jpeg                            jpeg jpg;
        image/png                             png;
        application/javascript                js;
        application/octet-stream              bin exe dll;
        application/octet-stream              deb;
        application/octet-stream              dmg;
    }

    default_type application/octet-stream;

    access_log off;
    sendfile on;
    keepalive_timeout 65;
    server_tokens off;

    server {
        listen %d default_server;
        server_name localhost example.local test.local _;

        # Match webserv test configuration
        client_max_body_size 1M;

        # Document root
        root %s;
        index index.html index.htm;

        # Root location
        location / {
            try_files $uri $uri/ =404;
        }

        # Upload endpoint - simulate upload behavior
        location /upload {
            limit_except POST {
                return 405 "Method Not Allowed\n";
            }
            return 200 "Upload successful\n";
            add_header Content-Type text/plain;
        }

        # Directory listing
        location /public/ {
            autoindex on;
            autoindex_exact_size off;
            autoindex_localtime on;
        }

        # Redirect test
        location = /old {
            return 302 /new-location;
        }

        # CGI simulation - return expected CGI output
        location /cgi-bin/ {
            return 200 "<html><head><title>CGI Test</title></head><body><h1>CGI Script Executed Successfully</h1><p>Request Method: $request_method</p><p>Server Protocol: $server_protocol</p></body></html>";
            add_header Content-Type text/html;
        }

        # API endpoint
        location /api {
            limit_except GET POST DELETE {
                return 405 "Method Not Allowed\n";
            }
            return 200 "API endpoint\n";
            add_header Content-Type text/plain;
        }

        # Error pages
        error_page 404 /custom_404.html;
        error_page 500 502 503 504 /custom_50x.html;

        location = /custom_404.html {
            internal;
            return 404 "Not Found\n";
            add_header Content-Type text/plain;
        }

        location = /custom_50x.html {
            internal;
            return 500 "Internal Server Error\n";
            add_header Content-Type text/plain;
        }
    }
}
`,
		n.pidFile,
		nginxTestPort,
		filepath.Join(n.workDir, "www"))

	return os.WriteFile(n.configPath, []byte(config), 0644)
}

// Start starts the nginx server
func (n *NginxComparison) Start() error {
	if !n.IsAvailable() {
		return fmt.Errorf("nginx binary not found")
	}

	if err := n.GenerateConfig(); err != nil {
		return fmt.Errorf("failed to generate nginx config: %w", err)
	}

	// Get absolute path for configuration
	absConfigPath, err := filepath.Abs(n.configPath)
	if err != nil {
		return fmt.Errorf("failed to get absolute config path: %w", err)
	}

	// Test the configuration first
	testCmd := exec.Command(n.nginxPath, "-t", "-c", absConfigPath)
	if output, err := testCmd.CombinedOutput(); err != nil {
		return fmt.Errorf("nginx config test failed: %w\nOutput: %s", err, string(output))
	}

	// Start nginx
	cmd := exec.Command(n.nginxPath, "-c", absConfigPath)

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("failed to start nginx: %w", err)
	}

	// Give nginx a moment to start
	time.Sleep(500 * time.Millisecond)

	// Check if it's responding
	for i := 0; i < 20; i++ {
		if n.isResponding() {
			n.running = true
			fmt.Printf("NGINX started successfully on port %d\n", nginxTestPort)
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}

	return fmt.Errorf("nginx failed to start responding within timeout")
}

// Stop stops the nginx server
func (n *NginxComparison) Stop() error {
	if !n.running {
		return nil
	}

	// Try graceful shutdown first
	if err := exec.Command(n.nginxPath, "-c", n.configPath, "-s", "quit").Run(); err != nil {
		// Force stop if graceful shutdown fails
		exec.Command(n.nginxPath, "-c", n.configPath, "-s", "stop").Run()
	}

	n.running = false
	fmt.Println("NGINX stopped")

	// Clean up files
	os.Remove(n.configPath)
	os.Remove(n.pidFile)

	return nil
}

// isResponding checks if nginx is responding to requests
func (n *NginxComparison) isResponding() bool {
	client := &http.Client{Timeout: 1 * time.Second}
	resp, err := client.Get(fmt.Sprintf("http://%s:%d/", nginxTestHost, nginxTestPort))
	if err != nil {
		return false
	}
	resp.Body.Close()
	return true
}

// GetURL returns nginx URL for given path
func (n *NginxComparison) GetURL(path string) string {
	return fmt.Sprintf("http://%s:%d%s", nginxTestHost, nginxTestPort, path)
}

// TestMain manages the NGINX lifecycle for all tests
func TestMain(m *testing.M) {
	// Setup: Start NGINX
	if err := setupNginxEnvironment(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to setup NGINX environment: %v\n", err)
		os.Exit(1)
	}

	// Run all tests
	exitCode := m.Run()

	// Teardown: Stop NGINX
	teardownNginxEnvironment()

	os.Exit(exitCode)
}

// setupNginxEnvironment starts NGINX for testing
func setupNginxEnvironment() error {
	nginxInstance = NewNginxComparison("test-server")

	if !nginxInstance.IsAvailable() {
		return fmt.Errorf("nginx not available for testing")
	}

	if err := nginxInstance.Start(); err != nil {
		return fmt.Errorf("failed to start nginx: %w", err)
	}

	nginxClient = &http.Client{
		Timeout: requestTimeout,
	}

	return nil
}

// teardownNginxEnvironment cleans up NGINX
func teardownNginxEnvironment() {
	if nginxInstance != nil {
		nginxInstance.Stop()
	}
}

// sendNginxRequest is a test helper for sending requests to NGINX
func sendNginxRequest(t *testing.T, method, uri, body string, headers map[string]string) (statusCode int, statusText, responseBody string) {
	t.Helper()

	var bodyReader io.Reader
	if body != "" {
		bodyReader = strings.NewReader(body)
	}

	url := nginxInstance.GetURL(uri)
	req, err := http.NewRequest(method, url, bodyReader)
	if err != nil {
		t.Fatalf("Failed to create request: %v", err)
	}

	// Add custom headers
	for key, value := range headers {
		req.Header.Set(key, value)
	}

	// Send request using nginx client
	resp, err := nginxClient.Do(req)
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

// TestNginxBaseline tests NGINX baseline behavior for HTTP server functionality
//
// This table-driven test establishes the expected behavior baseline that our webserv
// implementation should match. By testing against NGINX, we validate:
// 1. Our test scenarios are realistic and achievable
// 2. Expected status codes and responses are correct
// 3. HTTP/1.1 protocol compliance expectations
func TestNginxBaseline(t *testing.T) {
	type testCase struct {
		name           string
		method         string
		uri            string
		requestBody    string
		headers        map[string]string
		expectedStatus int
		expectedBody   string
		description    string
	}

	testTable := []testCase{
		// Static file serving tests
		{
			name:           "GetStaticIndex",
			method:         "GET",
			uri:            "/",
			requestBody:    "",
			expectedStatus: 200,
			expectedBody:   "html",
			description:    "NGINX serves static index file with 200 OK",
		},
		{
			name:           "GetStaticFile",
			method:         "GET",
			uri:            "/index.html",
			requestBody:    "",
			expectedStatus: 200,
			expectedBody:   "html",
			description:    "NGINX serves specific static file with 200 OK",
		},
		{
			name:           "GetNotFound",
			method:         "GET",
			uri:            "/does/not/exist.txt",
			requestBody:    "",
			expectedStatus: 404,
			expectedBody:   "Not Found",
			description:    "NGINX returns 404 for non-existent files",
		},

		// File upload simulation tests
		{
			name:           "PostUploadSuccess",
			method:         "POST",
			uri:            "/upload",
			requestBody:    "Test file upload content",
			headers:        map[string]string{"Content-Type": "application/octet-stream"},
			expectedStatus: 200,
			expectedBody:   "Upload successful",
			description:    "NGINX simulates successful upload with 200 OK",
		},

		// HTTP method validation tests
		{
			name:           "MethodNotAllowedOnUpload",
			method:         "GET",
			uri:            "/upload",
			requestBody:    "",
			expectedStatus: 405,
			expectedBody:   "Method Not Allowed",
			description:    "NGINX returns 405 for GET on upload endpoint",
		},
		{
			name:           "PutMethodNotImplemented",
			method:         "PUT",
			uri:            "/",
			requestBody:    "put data",
			expectedStatus: 405,
			expectedBody:   "",
			description:    "NGINX returns 405 for unsupported PUT method",
		},

		// CGI simulation tests
		{
			name:           "GetCGIScript",
			method:         "GET",
			uri:            "/cgi-bin/test.py",
			requestBody:    "",
			expectedStatus: 200,
			expectedBody:   "CGI Script Executed Successfully",
			description:    "NGINX simulates CGI execution with proper response",
		},
		{
			name:           "PostCGIScript",
			method:         "POST",
			uri:            "/cgi-bin/test.py",
			requestBody:    "name=test&value=data",
			headers:        map[string]string{"Content-Type": "application/x-www-form-urlencoded"},
			expectedStatus: 200,
			expectedBody:   "CGI Script Executed Successfully",
			description:    "NGINX handles POST to CGI endpoint",
		},

		// Directory listing tests
		{
			name:           "GetDirectoryListing",
			method:         "GET",
			uri:            "/public/",
			requestBody:    "",
			expectedStatus: 200,
			expectedBody:   "", // Directory listing format varies
			description:    "NGINX provides directory listing when enabled",
		},

		// Redirect tests
		{
			name:           "GetRedirect",
			method:         "GET",
			uri:            "/old",
			requestBody:    "",
			expectedStatus: 302,
			expectedBody:   "",
			description:    "NGINX performs 302 redirect as configured",
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
			description:    "NGINX handles virtual host headers correctly",
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
			description:    "NGINX supports HTTP/1.1 keep-alive connections",
		},
	}

	// Execute each test case to establish NGINX baseline behavior
	for _, tc := range testTable {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			t.Logf("Testing NGINX baseline: %s", tc.description)

			statusCode, statusText, responseBody := sendNginxRequest(t, tc.method, tc.uri, tc.requestBody, tc.headers)

			t.Logf("NGINX Response: %d %s", statusCode, statusText)
			if len(responseBody) > 0 && len(responseBody) < 200 {
				t.Logf("Response body: %q", responseBody)
			}

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

// TestNginxMultipartUpload tests NGINX multipart handling baseline
func TestNginxMultipartUpload(t *testing.T) {
	type multipartTestCase struct {
		name           string
		fieldName      string
		fileName       string
		fileContent    string
		expectedStatus int
		expectedBody   string
		description    string
	}

	testTable := []multipartTestCase{
		{
			name:           "MultipartUploadToUploadEndpoint",
			fieldName:      "file",
			fileName:       "test.txt",
			fileContent:    "This is test file content for multipart upload",
			expectedStatus: 200,
			expectedBody:   "Upload successful",
			description:    "NGINX handles multipart upload to configured endpoint",
		},
	}

	for _, tc := range testTable {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			t.Logf("Testing NGINX multipart baseline: %s", tc.description)

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

			// Send multipart request to NGINX
			req, err := http.NewRequest("POST", nginxInstance.GetURL("/upload"), &buf)
			if err != nil {
				t.Fatalf("Failed to create request: %v", err)
			}
			req.Header.Set("Content-Type", writer.FormDataContentType())

			resp, err := nginxClient.Do(req)
			if err != nil {
				t.Fatalf("Failed to send request: %v", err)
			}
			defer resp.Body.Close()

			responseBody, _ := io.ReadAll(resp.Body)

			t.Logf("NGINX multipart response: %d %s", resp.StatusCode, resp.Status)
			t.Logf("Response body: %q", string(responseBody))

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

// TestNginxProtocolCompliance tests NGINX HTTP/1.1 protocol compliance baseline
func TestNginxProtocolCompliance(t *testing.T) {
	type protocolTestCase struct {
		name        string
		testFunc    func(t *testing.T)
		description string
	}

	testTable := []protocolTestCase{
		{
			name:        "HTTP11Protocol",
			description: "NGINX responds with HTTP/1.1 protocol",
			testFunc: func(t *testing.T) {
				resp, err := nginxClient.Get(nginxInstance.GetURL("/"))
				if err != nil {
					t.Fatalf("Failed to send request: %v", err)
				}
				defer resp.Body.Close()

				t.Logf("NGINX protocol version: %s", resp.Proto)

				if resp.Proto != "HTTP/1.1" {
					t.Errorf("Expected HTTP/1.1, got %s", resp.Proto)
				}
			},
		},
		{
			name:        "KeepAliveConnections",
			description: "NGINX supports keep-alive connections",
			testFunc: func(t *testing.T) {
				conn, err := net.Dial("tcp", fmt.Sprintf("%s:%d", nginxTestHost, nginxTestPort))
				if err != nil {
					t.Fatalf("Failed to connect: %v", err)
				}
				defer conn.Close()

				// Send multiple requests on same connection
				for i := 0; i < 3; i++ {
					request := fmt.Sprintf("GET /?req=%d HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n", i, nginxTestHost)
					_, err := conn.Write([]byte(request))
					if err != nil {
						t.Fatalf("Failed to write request %d: %v", i, err)
					}

					reader := bufio.NewReader(conn)
					resp, err := http.ReadResponse(reader, nil)
					if err != nil {
						t.Fatalf("Failed to read response %d: %v", i, err)
					}
					resp.Body.Close()

					t.Logf("Request %d: %d %s", i, resp.StatusCode, resp.Status)
				}

				t.Log("NGINX successfully handled multiple keep-alive requests")
			},
		},
		{
			name:        "ResponseHeaders",
			description: "NGINX includes standard HTTP headers",
			testFunc: func(t *testing.T) {
				resp, err := nginxClient.Get(nginxInstance.GetURL("/"))
				if err != nil {
					t.Fatalf("Failed to send request: %v", err)
				}
				defer resp.Body.Close()

				// Check essential headers
				essentialHeaders := []string{"Content-Type", "Content-Length"}
				for _, header := range essentialHeaders {
					value := resp.Header.Get(header)
					t.Logf("NGINX %s header: %s", header, value)
					if value == "" {
						t.Errorf("NGINX should include %s header", header)
					}
				}

				// Check Date header
				dateHeader := resp.Header.Get("Date")
				t.Logf("NGINX Date header: %s", dateHeader)
				if dateHeader == "" {
					t.Error("NGINX should include Date header")
				}
			},
		},
	}

	for _, tc := range testTable {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			t.Logf("Testing NGINX protocol compliance: %s", tc.description)
			tc.testFunc(t)
		})
	}
}

// TestNginxPerformanceBaseline establishes performance baseline with NGINX
func TestNginxPerformanceBaseline(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping performance tests in short mode")
	}

	type performanceTestCase struct {
		name         string
		numRequests  int
		numConcurrent int
		description  string
	}

	testTable := []performanceTestCase{
		{
			name:         "LightLoad",
			numRequests:  20,
			numConcurrent: 5,
			description:  "NGINX handles light concurrent load",
		},
		{
			name:         "ModerateLoad",
			numRequests:  50,
			numConcurrent: 10,
			description:  "NGINX handles moderate concurrent load",
		},
	}

	for _, tc := range testTable {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Logf("Testing NGINX performance baseline: %s", tc.description)

			results := make(chan error, tc.numRequests)
			semaphore := make(chan struct{}, tc.numConcurrent)

			startTime := time.Now()

			// Launch concurrent requests
			for i := 0; i < tc.numRequests; i++ {
				go func(id int) {
					semaphore <- struct{}{}        // Acquire
					defer func() { <-semaphore }() // Release

					resp, err := nginxClient.Get(nginxInstance.GetURL(fmt.Sprintf("/?req=%d", id)))
					if err != nil {
						results <- err
						return
					}
					resp.Body.Close()

					if resp.StatusCode >= 400 {
						results <- fmt.Errorf("request %d returned error: %d", id, resp.StatusCode)
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

			duration := time.Since(startTime)

			successRate := float64(tc.numRequests-len(errors)) / float64(tc.numRequests) * 100
			requestsPerSecond := float64(tc.numRequests) / duration.Seconds()

			t.Logf("NGINX Performance Results:")
			t.Logf("  Total requests: %d", tc.numRequests)
			t.Logf("  Concurrent: %d", tc.numConcurrent)
			t.Logf("  Duration: %v", duration)
			t.Logf("  Success rate: %.1f%%", successRate)
			t.Logf("  Requests/sec: %.1f", requestsPerSecond)
			t.Logf("  Errors: %d", len(errors))

			if len(errors) > 0 {
				t.Logf("  Sample errors: %v", errors[:min(3, len(errors))])
			}

			// NGINX should handle all requests successfully
			if successRate < 99.0 {
				t.Errorf("NGINX success rate %.1f%% is below expected 99%%", successRate)
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
