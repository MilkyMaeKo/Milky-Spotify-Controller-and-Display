# Milky-Spotify-Controller-and-Display
ESP32 based retro-futuristic desk display that shows Spotify now playing info in real time. Built around an ESP32 and a 3.12" OLED panel, styled after phosphor terminal displays from 1980s.
Required Spotify Premium	

**Features**<br />
  Live now playing — track name, artist, progress bar, and timestamp<br />
  Scrolling marquee for long track titles	<br />
  Next in queue pulled from Spotify API	<br />
  Volume readout synced to Spotify device	<br />
  Auto token refresh 	<br />
  Spotify polling on core 0, display rendering on core 1	<br />
  WiFiManager config portal — connect to the ESP32 hotspot on first boot to enter WiFi credentials	<br />

**Hardware**	<br />
    PartDetailsESP32-WROOM-3238-pin dev board3.12" <br />
    OLEDSSD1322 controller, 256×64, 8080 parallel interface  <br />
    3D printed enclosureMatte black PLA	<br />

    
**Software**	<br />
  U8g2 — display driver	<br />
  WiFiManager — WiFi config portal	<br />
  ArduinoJson — JSON parsing	<br />
  Base64 — token encoding	<br />
  Spotify Web API — OAuth2 with refresh token flow	<br />

**Setup**	<br />
  Create a Spotify Developer app at developer.spotify.com	<br />
  Set redirect URI to http://127.0.0.1:8888/callback	<br />
  Run get_refresh_token.py to complete OAuth and get your refresh token	<br />
  Paste your Client ID, Client Secret, and Refresh Token into the sketch	<br />
  Flash to ESP32	<br />
  On first boot, connect to the SpotifyDesk WiFi hotspot and enter your home WiFi credentials	<br />

