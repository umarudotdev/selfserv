package integration

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const (
	nginxPort     = 8080
	nginxConfPath = "nginx.conf"
	nginxPidFile  = "nginx.pid"
)

// NginxComparison manages nginx instance for comparison testing
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
		configPath: filepath.Join(workDir, nginxConfPath),
		pidFile:    filepath.Join(workDir, nginxPidFile),
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

// GenerateConfig creates nginx configuration for comparison testing
func (n *NginxComparison) GenerateConfig() error {
	// Create a minimal nginx config that should work in most environments
	config := fmt.Sprintf(`
# Minimal nginx configuration for webserv comparison testing
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

        # Upload endpoint - return method not allowed for now
        location /upload {
            limit_except POST {
                return 405;
            }
            return 200 "Upload successful";
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

        # CGI simulation
        location /cgi-bin/ {
            return 200 "CGI Test Successful";
            add_header Content-Type text/html;
        }

        # API endpoint
        location /api {
            limit_except GET POST DELETE {
                return 405;
            }
            return 200 "API endpoint";
            add_header Content-Type text/plain;
        }

        # Error pages
        error_page 404 /custom_404.html;
        error_page 500 502 503 504 /custom_50x.html;

        location = /custom_404.html {
            internal;
        }

        location = /custom_50x.html {
            internal;
        }
    }
}
`,
		n.pidFile,
		nginxPort,
		filepath.Join(n.workDir, "www"))

	return os.WriteFile(n.configPath, []byte(config), 0644)
}

// Start starts the nginx server
func (n *NginxComparison) Start() error {
	if !n.IsAvailable() {
		return fmt.Errorf("nginx binary not found")
	}

	if err := n.GenerateConfig(); err != nil {
		return fmt.Errorf("failed to generate nginx config: %v", err)
	}

	// Get absolute path for configuration
	absConfigPath, err := filepath.Abs(n.configPath)
	if err != nil {
		return fmt.Errorf("failed to get absolute config path: %v", err)
	}

	// Test the configuration first
	testCmd := exec.Command(n.nginxPath, "-t", "-c", absConfigPath)
	if output, err := testCmd.CombinedOutput(); err != nil {
		return fmt.Errorf("nginx config test failed: %v\nOutput: %s", err, string(output))
	}

	// Start nginx
	cmd := exec.Command(n.nginxPath, "-c", absConfigPath)

	// Capture stderr for debugging
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return fmt.Errorf("failed to create stderr pipe: %v", err)
	}

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("failed to start nginx: %v", err)
	}

	// Give nginx a moment to start
	time.Sleep(500 * time.Millisecond)

	// Check if process is still running
	if cmd.Process != nil {
		// Process started, now check if it's responding
		for i := 0; i < 20; i++ { // Increased attempts
			if n.isResponding() {
				n.running = true
				return nil
			}
			time.Sleep(100 * time.Millisecond)
		}
	}

	// If we get here, nginx didn't start properly
	// Try to read any error output
	stderrData, _ := io.ReadAll(stderr)
	if len(stderrData) > 0 {
		return fmt.Errorf("nginx failed to start responding. Error output: %s", string(stderrData))
	}

	return fmt.Errorf("nginx failed to start responding within timeout")
}

// Stop stops the nginx server
func (n *NginxComparison) Stop() error {
	if !n.running {
		return nil
	}

	// Get absolute path for configuration
	absConfigPath, _ := filepath.Abs(n.configPath)

	// Try graceful shutdown first
	if err := exec.Command(n.nginxPath, "-c", absConfigPath, "-s", "quit").Run(); err != nil {
		// Force stop if graceful shutdown fails
		exec.Command(n.nginxPath, "-c", absConfigPath, "-s", "stop").Run()
	}

	n.running = false

	// Clean up files
	os.Remove(n.configPath)
	os.Remove(n.pidFile)

	return nil
}

// isResponding checks if nginx is responding to requests
func (n *NginxComparison) isResponding() bool {
	client := &http.Client{Timeout: 1 * time.Second}
	resp, err := client.Get(fmt.Sprintf("http://localhost:%d/", nginxPort))
	if err != nil {
		return false
	}
	resp.Body.Close()
	return true
}

// GetURL returns nginx URL for given path
func (n *NginxComparison) GetURL(path string) string {
	return fmt.Sprintf("http://localhost:%d%s", nginxPort, path)
}

// CompareResponse compares webserv response with nginx response
type ResponseComparison struct {
	Path           string
	Method         string
	WebservStatus  int
	NginxStatus    int
	StatusMatch    bool
	HeaderMatches  map[string]bool
	BodyMatch      bool
	WebservBody    string
	NginxBody      string
	Notes          []string
}

// CompareResponses compares webserv and nginx responses for a given request
func CompareResponses(webservURL, nginxURL, method string, body io.Reader) (*ResponseComparison, error) {
	client := &http.Client{Timeout: 5 * time.Second}

	// Make request to webserv
	webservReq, err := http.NewRequest(method, webservURL, body)
	if err != nil {
		return nil, err
	}

	webservResp, err := client.Do(webservReq)
	if err != nil {
		return nil, fmt.Errorf("webserv request failed: %v", err)
	}
	defer webservResp.Body.Close()

	webservBody, err := io.ReadAll(webservResp.Body)
	if err != nil {
		return nil, err
	}

	// Make request to nginx (reset body if needed)
	var nginxBody []byte
	var nginxResp *http.Response

	if body != nil {
		// For POST requests, we need a fresh body
		// This is a limitation - in real tests we'd need to prepare the body twice
		nginxReq, err := http.NewRequest(method, nginxURL, strings.NewReader(""))
		if err != nil {
			return nil, err
		}
		nginxResp, err = client.Do(nginxReq)
	} else {
		nginxReq, err := http.NewRequest(method, nginxURL, nil)
		if err != nil {
			return nil, err
		}
		nginxResp, err = client.Do(nginxReq)
	}

	if err != nil {
		return nil, fmt.Errorf("nginx request failed: %v", err)
	}
	defer nginxResp.Body.Close()

	nginxBody, err = io.ReadAll(nginxResp.Body)
	if err != nil {
		return nil, err
	}

	// Compare responses
	comparison := &ResponseComparison{
		Path:           webservURL,
		Method:         method,
		WebservStatus:  webservResp.StatusCode,
		NginxStatus:    nginxResp.StatusCode,
		StatusMatch:    webservResp.StatusCode == nginxResp.StatusCode,
		HeaderMatches:  make(map[string]bool),
		WebservBody:    string(webservBody),
		NginxBody:      string(nginxBody),
		Notes:          []string{},
	}

	// Compare important headers
	importantHeaders := []string{"Content-Type", "Content-Length", "Connection", "Server"}
	for _, header := range importantHeaders {
		webservVal := webservResp.Header.Get(header)
		nginxVal := nginxResp.Header.Get(header)

		// Special handling for Server header (expected to be different)
		if header == "Server" {
			comparison.HeaderMatches[header] = true // Always pass server header comparison
			comparison.Notes = append(comparison.Notes,
				fmt.Sprintf("Server headers differ (expected): webserv='%s', nginx='%s'", webservVal, nginxVal))
		} else {
			comparison.HeaderMatches[header] = webservVal == nginxVal
			if webservVal != nginxVal {
				comparison.Notes = append(comparison.Notes,
					fmt.Sprintf("%s header differs: webserv='%s', nginx='%s'", header, webservVal, nginxVal))
			}
		}
	}

	// Simple body comparison (exact match not always expected due to different server implementations)
	comparison.BodyMatch = strings.TrimSpace(string(webservBody)) == strings.TrimSpace(string(nginxBody))

	return comparison, nil
}

// Test webserv against nginx for basic functionality
func TestComparisonWithNginx(t *testing.T) {
	nginx := NewNginxComparison("test-server")

	if !nginx.IsAvailable() {
		t.Skip("Nginx not available for comparison testing")
	}

	// Start nginx
	require.NoError(t, nginx.Start(), "Failed to start nginx for comparison")
	defer nginx.Stop()

	tests := []struct {
		name         string
		path         string
		method       string
		expectSimilar bool
	}{
		{
			name:         "Root GET request",
			path:         "/",
			method:       "GET",
			expectSimilar: true,
		},
		{
			name:         "404 Not Found",
			path:         "/nonexistent.html",
			method:       "GET",
			expectSimilar: true,
		},
		{
			name:         "Method not allowed",
			path:         "/",
			method:       "PATCH",
			expectSimilar: true,
		},
		{
			name:         "Redirect test",
			path:         "/old",
			method:       "GET",
			expectSimilar: true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			webservURL := getTestURL(tt.path)
			nginxURL := nginx.GetURL(tt.path)

			comparison, err := CompareResponses(webservURL, nginxURL, tt.method, nil)
			require.NoError(t, err)

			t.Logf("Comparison for %s %s:", tt.method, tt.path)
			t.Logf("  Webserv status: %d", comparison.WebservStatus)
			t.Logf("  Nginx status: %d", comparison.NginxStatus)
			t.Logf("  Status match: %v", comparison.StatusMatch)

			for header, match := range comparison.HeaderMatches {
				t.Logf("  %s header match: %v", header, match)
			}

			if len(comparison.Notes) > 0 {
				t.Logf("  Notes: %v", comparison.Notes)
			}

			if tt.expectSimilar {
				assert.True(t, comparison.StatusMatch,
					"Status codes should match for %s %s", tt.method, tt.path)
			}

			// Check that webserv returns valid HTTP responses
			assert.True(t, comparison.WebservStatus >= 100 && comparison.WebservStatus < 600,
				"Webserv should return valid HTTP status code")
		})
	}
}

// Test protocol compliance comparison
func TestProtocolComplianceComparison(t *testing.T) {
	nginx := NewNginxComparison("test-server")

	if !nginx.IsAvailable() {
		t.Skip("Nginx not available for protocol compliance testing")
	}

	require.NoError(t, nginx.Start())
	defer nginx.Stop()

	t.Run("HTTP/1.1 keep-alive behavior", func(t *testing.T) {
		// Test keep-alive connections
		webservConn, err := net.Dial("tcp", fmt.Sprintf("localhost:%d", testServerPort))
		require.NoError(t, err)
		defer webservConn.Close()

		nginxConn, err := net.Dial("tcp", fmt.Sprintf("localhost:%d", nginxPort))
		require.NoError(t, err)
		defer nginxConn.Close()

		// Send same request to both servers
		request := "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"

		// Webserv
		_, err = webservConn.Write([]byte(request))
		require.NoError(t, err)

		webservReader := bufio.NewReader(webservConn)
		webservResp, err := http.ReadResponse(webservReader, nil)
		require.NoError(t, err)
		webservResp.Body.Close()

		// Nginx
		_, err = nginxConn.Write([]byte(request))
		require.NoError(t, err)

		nginxReader := bufio.NewReader(nginxConn)
		nginxResp, err := http.ReadResponse(nginxReader, nil)
		require.NoError(t, err)
		nginxResp.Body.Close()

		t.Logf("Webserv Connection header: %s", webservResp.Header.Get("Connection"))
		t.Logf("Nginx Connection header: %s", nginxResp.Header.Get("Connection"))

		// Both should handle keep-alive similarly
		assert.Equal(t, "HTTP/1.1", webservResp.Proto)
		assert.Equal(t, "HTTP/1.1", nginxResp.Proto)
	})

	t.Run("Response header compliance", func(t *testing.T) {
		webservResp, err := http.Get(getTestURL("/"))
		require.NoError(t, err)
		defer webservResp.Body.Close()

		nginxResp, err := http.Get(nginx.GetURL("/"))
		require.NoError(t, err)
		defer nginxResp.Body.Close()

		// Check that webserv includes essential HTTP headers
		essentialHeaders := []string{"Content-Type", "Content-Length"}
		for _, header := range essentialHeaders {
			assert.NotEmpty(t, webservResp.Header.Get(header),
				"Webserv should include %s header", header)
		}

		// Compare Date header format (both should include it)
		webservDate := webservResp.Header.Get("Date")
		nginxDate := nginxResp.Header.Get("Date")

		assert.NotEmpty(t, webservDate, "Webserv should include Date header")
		assert.NotEmpty(t, nginxDate, "Nginx should include Date header")

		t.Logf("Webserv Date: %s", webservDate)
		t.Logf("Nginx Date: %s", nginxDate)
	})
}

// Test error handling comparison
func TestErrorHandlingComparison(t *testing.T) {
	nginx := NewNginxComparison("test-server")

	if !nginx.IsAvailable() {
		t.Skip("Nginx not available for error handling testing")
	}

	require.NoError(t, nginx.Start())
	defer nginx.Stop()

	errorTests := []struct {
		name       string
		path       string
		method     string
		expectCode int
	}{
		{
			name:       "404 for non-existent file",
			path:       "/does-not-exist.html",
			method:     "GET",
			expectCode: 404,
		},
		{
			name:       "405 for unsupported method",
			path:       "/",
			method:     "TRACE",
			expectCode: 405,
		},
	}

	for _, tt := range errorTests {
		t.Run(tt.name, func(t *testing.T) {
			webservURL := getTestURL(tt.path)
			nginxURL := nginx.GetURL(tt.path)

			comparison, err := CompareResponses(webservURL, nginxURL, tt.method, nil)
			require.NoError(t, err)

			t.Logf("Error handling comparison for %s %s:", tt.method, tt.path)
			t.Logf("  Webserv: %d", comparison.WebservStatus)
			t.Logf("  Nginx: %d", comparison.NginxStatus)

			// Both servers should return the expected error code
			assert.Equal(t, tt.expectCode, comparison.WebservStatus,
				"Webserv should return %d for %s %s", tt.expectCode, tt.method, tt.path)

			// Status codes should match (or be reasonable alternatives)
			if comparison.WebservStatus != comparison.NginxStatus {
				t.Logf("Status codes differ - this may be acceptable depending on implementation")
			}
		})
	}
}

// Benchmark comparison between webserv and nginx
func BenchmarkWebservVsNginx(b *testing.B) {
	nginx := NewNginxComparison("test-server")

	if !nginx.IsAvailable() {
		b.Skip("Nginx not available for benchmarking")
	}

	require.NoError(b, nginx.Start())
	defer nginx.Stop()

	client := &http.Client{Timeout: 5 * time.Second}

	b.Run("Webserv", func(b *testing.B) {
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			resp, err := client.Get(getTestURL("/"))
			if err != nil {
				b.Fatal(err)
			}
			resp.Body.Close()
		}
	})

	b.Run("Nginx", func(b *testing.B) {
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			resp, err := client.Get(nginx.GetURL("/"))
			if err != nil {
				b.Fatal(err)
			}
			resp.Body.Close()
		}
	})
}
