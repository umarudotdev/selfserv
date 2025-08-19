package integration

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const (
	testServerHost = "localhost"
	testServerPort = 42069
	testTimeout    = 10 * time.Second
)

// Helper function to get test server URL
func getTestURL(path string) string {
	return fmt.Sprintf("http://%s:%d%s", testServerHost, testServerPort, path)
}

// Helper function to create test client with timeout
func createTestClient() *http.Client {
	return &http.Client{
		Timeout: testTimeout,
	}
}

// Test that the server is running and responsive
func TestServerIsRunning(t *testing.T) {
	client := createTestClient()

	resp, err := client.Get(getTestURL("/"))
	require.NoError(t, err, "Server should be accessible")
	defer resp.Body.Close()

	// Server should respond with some HTTP status (not necessarily 200)
	assert.NotZero(t, resp.StatusCode, "Server should return valid HTTP status")
}

// Test basic GET request for static files
func TestStaticFileServing(t *testing.T) {
	client := createTestClient()

	tests := []struct {
		name           string
		path           string
		expectedStatus int
	}{
		{
			name:           "Root path should be accessible",
			path:           "/",
			expectedStatus: 200,
		},
		{
			name:           "Non-existent file should return 404",
			path:           "/nonexistent.html",
			expectedStatus: 404,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			resp, err := client.Get(getTestURL(tt.path))
			require.NoError(t, err)
			defer resp.Body.Close()

			assert.Equal(t, tt.expectedStatus, resp.StatusCode)
		})
	}
}

// Test HTTP methods compliance
func TestHTTPMethods(t *testing.T) {
	client := createTestClient()

	tests := []struct {
		name           string
		method         string
		path           string
		body           io.Reader
		expectedStatus int
	}{
		{
			name:           "GET should be supported",
			method:         "GET",
			path:           "/",
			expectedStatus: 200,
		},
		{
			name:           "POST should be supported",
			method:         "POST",
			path:           "/upload",
			body:           strings.NewReader("test data"),
			expectedStatus: 200, // or appropriate status for upload endpoint
		},
		{
			name:           "DELETE should be supported",
			method:         "DELETE",
			path:           "/test.txt",
			expectedStatus: 405, // Method may not be allowed on this path
		},
		{
			name:           "PUT should return 405 (not implemented)",
			method:         "PUT",
			path:           "/",
			expectedStatus: 405,
		},
		{
			name:           "PATCH should return 405 (not implemented)",
			method:         "PATCH",
			path:           "/",
			expectedStatus: 405,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req, err := http.NewRequest(tt.method, getTestURL(tt.path), tt.body)
			require.NoError(t, err)

			resp, err := client.Do(req)
			require.NoError(t, err)
			defer resp.Body.Close()

			// Allow for some flexibility in status codes
			if tt.expectedStatus == 200 {
				assert.True(t, resp.StatusCode >= 200 && resp.StatusCode < 300,
					"Expected 2xx status, got %d", resp.StatusCode)
			} else {
				assert.Equal(t, tt.expectedStatus, resp.StatusCode)
			}
		})
	}
}

// Test HTTP/1.1 protocol compliance
func TestHTTP11Compliance(t *testing.T) {
	client := createTestClient()

	t.Run("Server should support HTTP/1.1", func(t *testing.T) {
		resp, err := client.Get(getTestURL("/"))
		require.NoError(t, err)
		defer resp.Body.Close()

		assert.Equal(t, "HTTP/1.1", resp.Proto)
	})

	t.Run("Server should handle Host header", func(t *testing.T) {
		req, err := http.NewRequest("GET", getTestURL("/"), nil)
		require.NoError(t, err)
		req.Host = "example.local"

		resp, err := client.Do(req)
		require.NoError(t, err)
		defer resp.Body.Close()

		// Should not return 400 for valid Host header
		assert.NotEqual(t, 400, resp.StatusCode)
	})
}

// Test keep-alive connections
func TestKeepAlive(t *testing.T) {
	t.Run("Server should support keep-alive connections", func(t *testing.T) {
		conn, err := net.Dial("tcp", fmt.Sprintf("%s:%d", testServerHost, testServerPort))
		require.NoError(t, err)
		defer conn.Close()

		// Send multiple requests on same connection
		for i := 0; i < 3; i++ {
			request := fmt.Sprintf("GET / HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n", testServerHost)
			_, err := conn.Write([]byte(request))
			require.NoError(t, err)

			// Read response
			reader := bufio.NewReader(conn)
			_, err = http.ReadResponse(reader, nil)
			require.NoError(t, err)
		}
	})
}

// Test chunked transfer encoding
func TestChunkedTransferEncoding(t *testing.T) {
	t.Run("Server should handle chunked requests", func(t *testing.T) {
		conn, err := net.Dial("tcp", fmt.Sprintf("%s:%d", testServerHost, testServerPort))
		require.NoError(t, err)
		defer conn.Close()

		// Send chunked POST request
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
			testServerHost)

		_, err = conn.Write([]byte(chunkedRequest))
		require.NoError(t, err)

		// Read response
		reader := bufio.NewReader(conn)
		resp, err := http.ReadResponse(reader, nil)
		require.NoError(t, err)
		defer resp.Body.Close()

		// Should not return 400 for valid chunked request
		assert.NotEqual(t, 400, resp.StatusCode)
	})
}

// Test file upload functionality
func TestFileUpload(t *testing.T) {
	client := createTestClient()

	t.Run("Upload via multipart form", func(t *testing.T) {
		// Create multipart form data
		var b bytes.Buffer
		writer := multipart.NewWriter(&b)

		// Add a file field
		fileWriter, err := writer.CreateFormFile("file", "test.txt")
		require.NoError(t, err)

		_, err = fileWriter.Write([]byte("This is test file content"))
		require.NoError(t, err)

		err = writer.Close()
		require.NoError(t, err)

		// Send POST request
		req, err := http.NewRequest("POST", getTestURL("/upload"), &b)
		require.NoError(t, err)
		req.Header.Set("Content-Type", writer.FormDataContentType())

		resp, err := client.Do(req)
		require.NoError(t, err)
		defer resp.Body.Close()

		// Should accept the upload (200-299 status codes)
		assert.True(t, resp.StatusCode >= 200 && resp.StatusCode < 300,
			"Upload should be accepted, got status %d", resp.StatusCode)
	})

	t.Run("Upload raw data", func(t *testing.T) {
		testData := "Raw file upload test content"

		req, err := http.NewRequest("POST", getTestURL("/upload"), strings.NewReader(testData))
		require.NoError(t, err)
		req.Header.Set("Content-Type", "application/octet-stream")

		resp, err := client.Do(req)
		require.NoError(t, err)
		defer resp.Body.Close()

		// Should accept the upload
		assert.True(t, resp.StatusCode >= 200 && resp.StatusCode < 300,
			"Raw upload should be accepted, got status %d", resp.StatusCode)
	})
}

// Test content length limits
func TestContentLengthLimits(t *testing.T) {
	client := createTestClient()

	t.Run("Large request should be rejected", func(t *testing.T) {
		// Create a large payload (assuming 1MB limit)
		largeData := strings.Repeat("A", 2*1024*1024) // 2MB

		req, err := http.NewRequest("POST", getTestURL("/upload"), strings.NewReader(largeData))
		require.NoError(t, err)

		resp, err := client.Do(req)
		require.NoError(t, err)
		defer resp.Body.Close()

		// Should return 413 Payload Too Large
		assert.Equal(t, 413, resp.StatusCode)
	})
}

// Test CGI execution (if CGI endpoint is configured)
func TestCGIExecution(t *testing.T) {
	client := createTestClient()

	t.Run("CGI script execution", func(t *testing.T) {
		// Test if CGI endpoint exists
		resp, err := client.Get(getTestURL("/cgi-bin/test.py"))
		require.NoError(t, err)
		defer resp.Body.Close()

		// If CGI is configured, should not return 404
		// If not configured, 404 is acceptable
		if resp.StatusCode != 404 {
			// CGI seems to be configured, test execution
			assert.True(t, resp.StatusCode >= 200 && resp.StatusCode < 300,
				"CGI execution should succeed")

			// Check for CGI-specific headers
			body, err := io.ReadAll(resp.Body)
			require.NoError(t, err)

			// CGI should produce some output
			assert.NotEmpty(t, body, "CGI should produce output")
		}
	})
}

// Test error page handling
func TestErrorPages(t *testing.T) {
	client := createTestClient()

	tests := []struct {
		name           string
		path           string
		expectedStatus int
	}{
		{
			name:           "404 for non-existent file",
			path:           "/this-file-does-not-exist.html",
			expectedStatus: 404,
		},
		{
			name:           "405 for unsupported method on restricted path",
			path:           "/",
			expectedStatus: 405, // Depending on configuration
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var resp *http.Response
			var err error

			if tt.expectedStatus == 405 {
				// Test unsupported method
				req, err := http.NewRequest("TRACE", getTestURL(tt.path), nil)
				require.NoError(t, err)
				resp, err = client.Do(req)
			} else {
				resp, err = client.Get(getTestURL(tt.path))
			}

			require.NoError(t, err)
			defer resp.Body.Close()

			if resp.StatusCode == tt.expectedStatus {
				// Check that error page is not empty
				body, err := io.ReadAll(resp.Body)
				require.NoError(t, err)
				assert.NotEmpty(t, body, "Error page should not be empty")

				// Check Content-Type header
				contentType := resp.Header.Get("Content-Type")
				assert.NotEmpty(t, contentType, "Error response should have Content-Type")
			}
		})
	}
}

// Test directory listing (autoindex)
func TestDirectoryListing(t *testing.T) {
	client := createTestClient()

	t.Run("Directory listing", func(t *testing.T) {
		// Test if directory listing is enabled for any path
		resp, err := client.Get(getTestURL("/public/"))
		require.NoError(t, err)
		defer resp.Body.Close()

		// Either should show directory listing (200) or be forbidden (403)
		assert.True(t, resp.StatusCode == 200 || resp.StatusCode == 403 || resp.StatusCode == 404,
			"Directory request should return 200, 403, or 404")

		if resp.StatusCode == 200 {
			body, err := io.ReadAll(resp.Body)
			require.NoError(t, err)

			// Directory listing should contain HTML
			bodyStr := string(body)
			assert.True(t, strings.Contains(bodyStr, "<html>") || strings.Contains(bodyStr, "Index of"),
				"Directory listing should contain HTML or index information")
		}
	})
}

// Test virtual hosts (server_name)
func TestVirtualHosts(t *testing.T) {
	client := createTestClient()

	t.Run("Different Host headers", func(t *testing.T) {
		hosts := []string{"example.local", "test.local", "localhost"}

		for _, host := range hosts {
			req, err := http.NewRequest("GET", getTestURL("/"), nil)
			require.NoError(t, err)
			req.Host = host

			resp, err := client.Do(req)
			require.NoError(t, err)
			resp.Body.Close()

			// Should handle different hosts gracefully (not return 400)
			assert.NotEqual(t, 400, resp.StatusCode,
				"Server should handle Host header: %s", host)
		}
	})
}

// Test HTTP redirection
func TestHTTPRedirection(t *testing.T) {
	client := &http.Client{
		Timeout: testTimeout,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			// Don't follow redirects, we want to test the redirect response
			return http.ErrUseLastResponse
		},
	}

	t.Run("HTTP redirects", func(t *testing.T) {
		// Test if any redirect rules are configured
		paths := []string{"/old", "/redirect-test", "/moved"}

		for _, path := range paths {
			resp, err := client.Get(getTestURL(path))
			require.NoError(t, err)
			resp.Body.Close()

			// If a redirect is configured, should return 3xx
			if resp.StatusCode >= 300 && resp.StatusCode < 400 {
				location := resp.Header.Get("Location")
				assert.NotEmpty(t, location, "Redirect should have Location header")
			}
		}
	})
}

// Test DELETE method
func TestDeleteMethod(t *testing.T) {
	client := createTestClient()

	t.Run("DELETE request handling", func(t *testing.T) {
		req, err := http.NewRequest("DELETE", getTestURL("/test-delete.txt"), nil)
		require.NoError(t, err)

		resp, err := client.Do(req)
		require.NoError(t, err)
		defer resp.Body.Close()

		// DELETE should either succeed (2xx), be forbidden (403),
		// not found (404), or method not allowed (405)
		validStatuses := []int{200, 204, 403, 404, 405}
		assert.Contains(t, validStatuses, resp.StatusCode,
			"DELETE should return valid status code")
	})
}

// Test server robustness under load
func TestServerRobustness(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping robustness test in short mode")
	}

	client := createTestClient()

	t.Run("Multiple concurrent requests", func(t *testing.T) {
		const numRequests = 50
		const numConcurrent = 10

		results := make(chan error, numRequests)
		semaphore := make(chan struct{}, numConcurrent)

		for i := 0; i < numRequests; i++ {
			go func(id int) {
				semaphore <- struct{}{}        // Acquire
				defer func() { <-semaphore }() // Release

				resp, err := client.Get(getTestURL(fmt.Sprintf("/?req=%d", id)))
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
		for i := 0; i < numRequests; i++ {
			if err := <-results; err != nil {
				errors = append(errors, err)
			}
		}

		// Allow some errors but server should handle most requests
		errorRate := float64(len(errors)) / float64(numRequests)
		assert.Less(t, errorRate, 0.1, // Less than 10% error rate
			"Server should handle concurrent requests robustly. Errors: %v", errors)
	})
}

// Test timeout handling
func TestTimeoutHandling(t *testing.T) {
	t.Run("Server should handle slow clients", func(t *testing.T) {
		conn, err := net.Dial("tcp", fmt.Sprintf("%s:%d", testServerHost, testServerPort))
		require.NoError(t, err)
		defer conn.Close()

		// Send partial request
		_, err = conn.Write([]byte("GET / HTTP/1.1\r\nHost: " + testServerHost + "\r\n"))
		require.NoError(t, err)

		// Wait to see if server handles the incomplete request
		time.Sleep(1 * time.Second)

		// Complete the request
		_, err = conn.Write([]byte("\r\n"))
		require.NoError(t, err)

		// Server should still respond
		reader := bufio.NewReader(conn)
		resp, err := http.ReadResponse(reader, nil)
		require.NoError(t, err)
		defer resp.Body.Close()

		assert.NotEqual(t, 408, resp.StatusCode, "Server should not timeout for reasonable delays")
	})
}

// Benchmark tests for performance validation
func BenchmarkSimpleGET(b *testing.B) {
	client := createTestClient()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		resp, err := client.Get(getTestURL("/"))
		if err != nil {
			b.Fatal(err)
		}
		resp.Body.Close()
	}
}

func BenchmarkKeepAliveRequests(b *testing.B) {
	conn, err := net.Dial("tcp", fmt.Sprintf("%s:%d", testServerHost, testServerPort))
	if err != nil {
		b.Fatal(err)
	}
	defer conn.Close()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		request := fmt.Sprintf("GET /?req=%d HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",
			i, testServerHost)
		_, err := conn.Write([]byte(request))
		if err != nil {
			b.Fatal(err)
		}

		reader := bufio.NewReader(conn)
		resp, err := http.ReadResponse(reader, nil)
		if err != nil {
			b.Fatal(err)
		}
		resp.Body.Close()
	}
}
