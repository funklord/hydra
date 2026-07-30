# Serves the request's own headers as the body, so a completed download *is*
# the record of what the server saw.
import http.server, socketserver, sys, json
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = json.dumps({k.lower(): v for k, v in self.headers.items()},
                          indent=0).encode()
        self.send_response(206 if "Range" in self.headers else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
socketserver.TCPServer.allow_reuse_address = True
socketserver.TCPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
