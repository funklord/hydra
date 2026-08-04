import http.server, socketserver, sys, os, time
os.chdir(sys.argv[2])
class H(http.server.SimpleHTTPRequestHandler):
	  def log_message(self, *a): pass
	  def do_GET(self):
		    p = self.translate_path(self.path)
		    if not os.path.isfile(p): return super().do_GET()
		    size = os.path.getsize(p)
		    self.send_response(200)
		    self.send_header("Content-Length", str(size))
		    self.send_header("Content-Type", "video/mp4")
		    self.send_header("Accept-Ranges", "bytes")
		    self.end_headers()
				# dribble it out so the progress bar is visible on screen
		    with open(p, "rb") as f:
			      while True:
				        b = f.read(60000)
				        if not b: break
				        try: self.wfile.write(b)
				        except Exception: return
				        time.sleep(0.25)
socketserver.TCPServer.allow_reuse_address = True
socketserver.TCPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
