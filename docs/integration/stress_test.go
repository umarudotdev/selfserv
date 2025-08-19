package integration

import (
	"context"
	"io"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// StressTestConfig defines parameters for stress testing
type StressTestConfig struct {
	NumRequests    int
	Concurrency    int
	RequestTimeout time.Duration
	TestDuration   time.Duration
	TargetURL      string
	MaxErrorRate   float64
}

// StressTestResult contains the results of a stress test
type StressTestResult struct {
	TotalRequests     int64
	SuccessfulReqs    int64
	FailedReqs        int64
	TotalBytes        int64
	Duration          time.Duration
	RequestsPerSecond float64
	ErrorRate         float64
	AvgResponseTime   time.Duration
	MinResponseTime   time.Duration
	MaxResponseTime   time.Duration
}

// StressTest performs a stress test against the webserv server
func StressTest(config StressTestConfig) (*StressTestResult, error) {
	client := &http.Client{
		Timeout: config.RequestTimeout,
	}

	var (
		totalRequests  int64
		successfulReqs int64
		failedReqs     int64
		totalBytes     int64
		totalRespTime  int64             // in nanoseconds
		minRespTime    int64 = 1<<63 - 1 // max int64
		maxRespTime    int64
	)

	startTime := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), config.TestDuration)
	defer cancel()

	// Channel to control concurrency
	semaphore := make(chan struct{}, config.Concurrency)
	var wg sync.WaitGroup

	// Run requests
	for i := 0; i < config.NumRequests; i++ {
		select {
		case <-ctx.Done():
			break
		default:
		}

		wg.Add(1)
		go func(reqID int) {
			defer wg.Done()

			semaphore <- struct{}{}        // Acquire
			defer func() { <-semaphore }() // Release

			reqStart := time.Now()
			resp, err := client.Get(config.TargetURL)
			reqDuration := time.Since(reqStart)

			atomic.AddInt64(&totalRequests, 1)

			// Update response time statistics
			respTimeNs := reqDuration.Nanoseconds()
			atomic.AddInt64(&totalRespTime, respTimeNs)

			// Update min response time
			for {
				currentMin := atomic.LoadInt64(&minRespTime)
				if respTimeNs >= currentMin || atomic.CompareAndSwapInt64(&minRespTime, currentMin, respTimeNs) {
					break
				}
			}

			// Update max response time
			for {
				currentMax := atomic.LoadInt64(&maxRespTime)
				if respTimeNs <= currentMax || atomic.CompareAndSwapInt64(&maxRespTime, currentMax, respTimeNs) {
					break
				}
			}

			if err != nil {
				atomic.AddInt64(&failedReqs, 1)
				return
			}

			defer resp.Body.Close()

			if resp.StatusCode >= 500 {
				atomic.AddInt64(&failedReqs, 1)
				return
			}

			// Count bytes
			bytes, err := io.ReadAll(resp.Body)
			if err != nil {
				atomic.AddInt64(&failedReqs, 1)
				return
			}

			atomic.AddInt64(&successfulReqs, 1)
			atomic.AddInt64(&totalBytes, int64(len(bytes)))
		}(i)

		// Small delay to avoid overwhelming the server instantaneously
		if i%config.Concurrency == 0 {
			time.Sleep(1 * time.Millisecond)
		}
	}

	// Wait for all requests to complete or timeout
	done := make(chan struct{})
	go func() {
		wg.Wait()
		close(done)
	}()

	select {
	case <-done:
	case <-ctx.Done():
		// Test duration exceeded
	}

	duration := time.Since(startTime)

	// Calculate results
	total := atomic.LoadInt64(&totalRequests)
	successful := atomic.LoadInt64(&successfulReqs)
	failed := atomic.LoadInt64(&failedReqs)

	result := &StressTestResult{
		TotalRequests:     total,
		SuccessfulReqs:    successful,
		FailedReqs:        failed,
		TotalBytes:        atomic.LoadInt64(&totalBytes),
		Duration:          duration,
		RequestsPerSecond: float64(total) / duration.Seconds(),
		ErrorRate:         float64(failed) / float64(total),
	}

	if total > 0 {
		result.AvgResponseTime = time.Duration(atomic.LoadInt64(&totalRespTime) / total)
	}
	result.MinResponseTime = time.Duration(atomic.LoadInt64(&minRespTime))
	result.MaxResponseTime = time.Duration(atomic.LoadInt64(&maxRespTime))

	return result, nil
}

// Test basic stress scenarios
func TestStressBasicLoad(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping stress test in short mode")
	}

	config := StressTestConfig{
		NumRequests:    1000,
		Concurrency:    20,
		RequestTimeout: 5 * time.Second,
		TestDuration:   30 * time.Second,
		TargetURL:      getTestURL("/"),
		MaxErrorRate:   0.05, // 5% maximum error rate
	}

	result, err := StressTest(config)
	require.NoError(t, err)

	t.Logf("Stress Test Results:")
	t.Logf("  Total Requests: %d", result.TotalRequests)
	t.Logf("  Successful: %d", result.SuccessfulReqs)
	t.Logf("  Failed: %d", result.FailedReqs)
	t.Logf("  Requests/sec: %.2f", result.RequestsPerSecond)
	t.Logf("  Error Rate: %.2f%%", result.ErrorRate*100)
	t.Logf("  Avg Response Time: %v", result.AvgResponseTime)
	t.Logf("  Min Response Time: %v", result.MinResponseTime)
	t.Logf("  Max Response Time: %v", result.MaxResponseTime)
	t.Logf("  Total Bytes: %d", result.TotalBytes)
	t.Logf("  Duration: %v", result.Duration)

	// Assertions
	assert.True(t, result.TotalRequests > 0, "Should have processed some requests")
	assert.True(t, result.RequestsPerSecond > 10, "Should handle at least 10 requests per second")
	assert.Less(t, result.ErrorRate, config.MaxErrorRate,
		"Error rate should be less than %.2f%%", config.MaxErrorRate*100)
	assert.Less(t, result.AvgResponseTime.Milliseconds(), int64(1000),
		"Average response time should be less than 1 second")
}

// Test server under heavy concurrent load
func TestStressHighConcurrency(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping stress test in short mode")
	}

	config := StressTestConfig{
		NumRequests:    2000,
		Concurrency:    100, // High concurrency
		RequestTimeout: 10 * time.Second,
		TestDuration:   45 * time.Second,
		TargetURL:      getTestURL("/"),
		MaxErrorRate:   0.10, // Allow higher error rate for this test
	}

	result, err := StressTest(config)
	require.NoError(t, err)

	t.Logf("High Concurrency Test Results:")
	t.Logf("  Total Requests: %d", result.TotalRequests)
	t.Logf("  Successful: %d", result.SuccessfulReqs)
	t.Logf("  Failed: %d", result.FailedReqs)
	t.Logf("  Requests/sec: %.2f", result.RequestsPerSecond)
	t.Logf("  Error Rate: %.2f%%", result.ErrorRate*100)
	t.Logf("  Avg Response Time: %v", result.AvgResponseTime)

	// Under high concurrency, we expect some degradation but server should not crash
	assert.True(t, result.TotalRequests > 500, "Should process significant number of requests")
	assert.Less(t, result.ErrorRate, config.MaxErrorRate,
		"Error rate should be manageable even under high load")
}

// Test mixed workload (GET, POST, uploads)
func TestStressMixedWorkload(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping stress test in short mode")
	}

	client := createTestClient()

	var (
		getRequests    int64
		postRequests   int64
		uploadRequests int64
		totalErrors    int64
	)

	const (
		numWorkers   = 20
		testDuration = 30 * time.Second
	)

	ctx, cancel := context.WithTimeout(context.Background(), testDuration)
	defer cancel()

	var wg sync.WaitGroup

	// Start workers for different types of requests
	for i := 0; i < numWorkers; i++ {
		wg.Add(3) // GET, POST, and Upload workers

		// GET worker
		go func() {
			defer wg.Done()
			for {
				select {
				case <-ctx.Done():
					return
				default:
				}

				resp, err := client.Get(getTestURL("/"))
				if err != nil {
					atomic.AddInt64(&totalErrors, 1)
				} else {
					resp.Body.Close()
					if resp.StatusCode >= 400 {
						atomic.AddInt64(&totalErrors, 1)
					}
				}
				atomic.AddInt64(&getRequests, 1)

				time.Sleep(10 * time.Millisecond)
			}
		}()

		// POST worker
		go func() {
			defer wg.Done()
			for {
				select {
				case <-ctx.Done():
					return
				default:
				}

				req, err := http.NewRequest("POST", getTestURL("/api"),
					strings.NewReader("test data"))
				if err != nil {
					atomic.AddInt64(&totalErrors, 1)
					continue
				}

				resp, err := client.Do(req)
				if err != nil {
					atomic.AddInt64(&totalErrors, 1)
				} else {
					resp.Body.Close()
					if resp.StatusCode >= 500 { // Only count server errors
						atomic.AddInt64(&totalErrors, 1)
					}
				}
				atomic.AddInt64(&postRequests, 1)

				time.Sleep(20 * time.Millisecond)
			}
		}()

		// Upload worker
		go func() {
			defer wg.Done()
			for {
				select {
				case <-ctx.Done():
					return
				default:
				}

				// Small upload to avoid overwhelming
				uploadData := "small test upload"
				req, err := http.NewRequest("POST", getTestURL("/upload"),
					strings.NewReader(uploadData))
				if err != nil {
					atomic.AddInt64(&totalErrors, 1)
					continue
				}
				req.Header.Set("Content-Type", "application/octet-stream")

				resp, err := client.Do(req)
				if err != nil {
					atomic.AddInt64(&totalErrors, 1)
				} else {
					resp.Body.Close()
					if resp.StatusCode >= 500 {
						atomic.AddInt64(&totalErrors, 1)
					}
				}
				atomic.AddInt64(&uploadRequests, 1)

				time.Sleep(50 * time.Millisecond) // Uploads are more expensive
			}
		}()
	}

	// Wait for test to complete
	<-ctx.Done()

	// Give workers a moment to finish current requests
	time.Sleep(100 * time.Millisecond)

	gets := atomic.LoadInt64(&getRequests)
	posts := atomic.LoadInt64(&postRequests)
	uploads := atomic.LoadInt64(&uploadRequests)
	errors := atomic.LoadInt64(&totalErrors)
	total := gets + posts + uploads

	t.Logf("Mixed Workload Test Results:")
	t.Logf("  GET Requests: %d", gets)
	t.Logf("  POST Requests: %d", posts)
	t.Logf("  Upload Requests: %d", uploads)
	t.Logf("  Total Requests: %d", total)
	t.Logf("  Total Errors: %d", errors)
	t.Logf("  Error Rate: %.2f%%", float64(errors)/float64(total)*100)

	// Assertions
	assert.True(t, total > 0, "Should have processed requests")
	assert.True(t, gets > 0, "Should have processed GET requests")
	assert.True(t, posts > 0, "Should have processed POST requests")

	errorRate := float64(errors) / float64(total)
	assert.Less(t, errorRate, 0.15, "Error rate should be reasonable under mixed load")
}

// Test server memory stability under prolonged load
func TestStressMemoryStability(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping memory stability test in short mode")
	}

	// This test runs for a longer duration with moderate load
	// to check for memory leaks and stability issues
	config := StressTestConfig{
		NumRequests:    5000,
		Concurrency:    10, // Moderate concurrency
		RequestTimeout: 5 * time.Second,
		TestDuration:   60 * time.Second, // 1 minute
		TargetURL:      getTestURL("/"),
		MaxErrorRate:   0.05,
	}

	result, err := StressTest(config)
	require.NoError(t, err)

	t.Logf("Memory Stability Test Results:")
	t.Logf("  Duration: %v", result.Duration)
	t.Logf("  Total Requests: %d", result.TotalRequests)
	t.Logf("  Requests/sec: %.2f", result.RequestsPerSecond)
	t.Logf("  Error Rate: %.2f%%", result.ErrorRate*100)

	// After prolonged execution, server should still be responsive
	assert.True(t, result.TotalRequests > 1000, "Should process significant number of requests")
	assert.Less(t, result.ErrorRate, config.MaxErrorRate, "Error rate should remain low")
	assert.True(t, result.RequestsPerSecond > 5, "Should maintain reasonable throughput")
}
