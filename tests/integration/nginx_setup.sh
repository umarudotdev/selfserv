#!/bin/bash

# Nginx setup script for webserv comparison testing

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "${GREEN}[NGINX]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if nginx is available
check_nginx() {
    log "Checking for nginx installation..."
    
    if command -v nginx >/dev/null 2>&1; then
        NGINX_PATH=$(command -v nginx)
        log "Found nginx at: $NGINX_PATH"
        nginx -v
        return 0
    fi
    
    # Check common installation paths
    for path in /usr/sbin/nginx /usr/bin/nginx /usr/local/bin/nginx; do
        if [ -f "$path" ]; then
            log "Found nginx at: $path"
            $path -v
            return 0
        fi
    done
    
    return 1
}

# Install nginx (requires sudo)
install_nginx() {
    log "Attempting to install nginx..."
    
    if [ "$EUID" -ne 0 ]; then
        error "Root privileges required to install nginx"
        error "Run: sudo $0 --install"
        error "Or manually install: sudo apt-get install nginx"
        exit 1
    fi
    
    # Detect package manager and install
    if command -v apt-get >/dev/null 2>&1; then
        log "Installing nginx via apt-get..."
        apt-get update
        apt-get install -y nginx
    elif command -v yum >/dev/null 2>&1; then
        log "Installing nginx via yum..."
        yum install -y nginx
    elif command -v brew >/dev/null 2>&1; then
        log "Installing nginx via brew..."
        brew install nginx
    else
        error "No supported package manager found"
        error "Please install nginx manually"
        exit 1
    fi
    
    log "Nginx installation completed"
}

# Main function
main() {
    case "${1:-check}" in
        --install)
            if ! check_nginx; then
                install_nginx
            else
                log "Nginx is already installed"
            fi
            ;;
        --check)
            if check_nginx; then
                log "Nginx is available for comparison testing"
                exit 0
            else
                warn "Nginx not found"
                warn "Install with: $0 --install"
                warn "Or manually: sudo apt-get install nginx"
                exit 1
            fi
            ;;
        --help)
            echo "Nginx setup script for webserv comparison testing"
            echo ""
            echo "Usage: $0 [OPTION]"
            echo ""
            echo "Options:"
            echo "  --check     Check if nginx is available (default)"
            echo "  --install   Install nginx (requires sudo)"
            echo "  --help      Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                  # Check nginx availability"
            echo "  sudo $0 --install   # Install nginx"
            ;;
        *)
            check_nginx
            ;;
    esac
}

main "$@"
