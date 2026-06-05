import requests
import webbrowser
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

CLIENT_ID     = ""
CLIENT_SECRET = ""
REDIRECT_URI  = "http://127.0.0.1:8888/callback"
SCOPES        = "user-read-currently-playing user-read-playback-state user-modify-playback-state"

# 1: open Spotify login in browser
params = {
    "client_id":     CLIENT_ID,
    "response_type": "code",
    "redirect_uri":  REDIRECT_URI,
    "scope":         SCOPES,
}
auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(params)
print("Opening Spotify login in browser...")
webbrowser.open(auth_url)

# 2: catch the callback code
auth_code = None

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        global auth_code
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        if "code" in params:
            auth_code = params["code"][0]
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"Got it! You can close this tab.")
    def log_message(self, *args):
        pass  # suppress server logs

print("Waiting for Spotify to redirect back...")
server = HTTPServer(("127.0.0.1", 8888), Handler)
server.handle_request()

if not auth_code:
    print("No code received, something went wrong.")
    exit(1)

# 3: exchange code for tokens
response = requests.post(
    "https://accounts.spotify.com/api/token",
    data={
        "grant_type":   "authorization_code",
        "code":         auth_code,
        "redirect_uri": REDIRECT_URI,
    },
    auth=(CLIENT_ID, CLIENT_SECRET),
)

tokens = response.json()

if "refresh_token" not in tokens:
    print("Error:", tokens)
    exit(1)

print("\n==============================================")
print("SUCCESS. Copy these into your ESP32 sketch:")
print("==============================================")
print(f"CLIENT_ID:     {CLIENT_ID}")
print(f"CLIENT_SECRET: {CLIENT_SECRET}")
print(f"REFRESH_TOKEN: {tokens['refresh_token']}")
print("==============================================\n")