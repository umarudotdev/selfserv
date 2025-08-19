#!/usr/bin/env python3

import os
import sys
import cgi

# Print CGI headers
print("Content-Type: text/html")
print("Status: 200 OK")
print()  # Empty line to end headers

# Print HTML response
print("<!DOCTYPE html>")
print("<html>")
print("<head><title>CGI Test</title></head>")
print("<body>")
print("<h1>CGI Test Successful</h1>")
print(f"<p>Request Method: {os.environ.get('REQUEST_METHOD', 'Unknown')}</p>")
print(f"<p>Script Name: {os.environ.get('SCRIPT_NAME', 'Unknown')}</p>")
print(f"<p>Path Info: {os.environ.get('PATH_INFO', 'Unknown')}</p>")
print(f"<p>Query String: {os.environ.get('QUERY_STRING', 'None')}</p>")
print(f"<p>Content Length: {os.environ.get('CONTENT_LENGTH', '0')}</p>")
print(f"<p>Server Protocol: {os.environ.get('SERVER_PROTOCOL', 'Unknown')}</p>")

# If POST request, read and display body
if os.environ.get("REQUEST_METHOD") == "POST":
    content_length = int(os.environ.get("CONTENT_LENGTH", "0"))
    if content_length > 0:
        post_data = sys.stdin.read(content_length)
        print(f"<p>POST Data: {post_data}</p>")

print("</body>")
print("</html>")
