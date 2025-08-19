package integration

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TestNginxSetup verifies that nginx comparison setup works
func TestNginxSetup(t *testing.T) {
	nginx := NewNginxComparison("test-server")
	
	if !nginx.IsAvailable() {
		t.Skip("Nginx not available - run 'make nginx-install' or install manually")
	}
	
	t.Run("nginx binary detection", func(t *testing.T) {
		assert.NotEmpty(t, nginx.nginxPath, "Should detect nginx binary")
		t.Logf("Nginx binary found at: %s", nginx.nginxPath)
	})
	
	t.Run("nginx config generation", func(t *testing.T) {
		err := nginx.GenerateConfig()
		require.NoError(t, err, "Should generate nginx config")
		
		// Check that config file was created
		assert.FileExists(t, nginx.configPath, "Config file should be created")
		
		// Clean up
		nginx.Stop()
	})
	
	t.Run("nginx start and stop", func(t *testing.T) {
		err := nginx.Start()
		require.NoError(t, err, "Should start nginx successfully")
		
		assert.True(t, nginx.running, "Nginx should be marked as running")
		assert.True(t, nginx.isResponding(), "Nginx should be responding to requests")
		
		err = nginx.Stop()
		assert.NoError(t, err, "Should stop nginx cleanly")
		assert.False(t, nginx.running, "Nginx should be marked as stopped")
	})
}

// TestSimpleComparison tests basic comparison functionality
func TestSimpleComparison(t *testing.T) {
	nginx := NewNginxComparison("test-server")
	
	if !nginx.IsAvailable() {
		t.Skip("Nginx not available for comparison testing")
	}
	
	// Start nginx for comparison
	require.NoError(t, nginx.Start())
	defer nginx.Stop()
	
	t.Run("compare basic GET request", func(t *testing.T) {
		webservURL := getTestURL("/")
		nginxURL := nginx.GetURL("/")
		
		comparison, err := CompareResponses(webservURL, nginxURL, "GET", nil)
		require.NoError(t, err)
		
		t.Logf("Comparison results:")
		t.Logf("  Webserv status: %d", comparison.WebservStatus)
		t.Logf("  Nginx status: %d", comparison.NginxStatus)
		t.Logf("  Status match: %v", comparison.StatusMatch)
		t.Logf("  Body match: %v", comparison.BodyMatch)
		
		// Both should return valid HTTP status codes
		assert.True(t, comparison.WebservStatus >= 100 && comparison.WebservStatus < 600,
			"Webserv should return valid HTTP status")
		assert.True(t, comparison.NginxStatus >= 100 && comparison.NginxStatus < 600,
			"Nginx should return valid HTTP status")
		
		// Log any differences for analysis
		if len(comparison.Notes) > 0 {
			t.Logf("Differences noted: %v", comparison.Notes)
		}
	})
}
