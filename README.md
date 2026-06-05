# Milky-Spotify-Controller-and-Display
ESP32 based retro-futuristic desk display that shows Spotify now playing info in real time. Built around an ESP32 and a 3.12" OLED panel, styled after phosphor terminal displays from 1980s.
Required Spotify Premium

Features
  Live now playing — track name, artist, progress bar, and timestamp
  Scrolling marquee for long track titles
  Next in queue pulled from Spotify API
  Volume readout synced to Spotify device
  Auto token refresh 
  Spotify polling on core 0, display rendering on core 1
  WiFiManager config portal — connect to the ESP32 hotspot on first boot to enter WiFi credentials

Hardware
    PartDetailsESP32-WROOM-3238-pin dev board3.12" OLEDSSD1322 controller, 256×64, 8080 parallel interfaceClear acrylic sheet3mm, for Pepper's Ghost floating display effect3D printed enclosureMatte black PLA

    
Software
  Arduino IDE
  U8g2 — display driver
  WiFiManager — WiFi config portal
  ArduinoJson — JSON parsing
  Base64 — token encoding
  Spotify Web API — OAuth2 with refresh token flow

Setup
  Create a Spotify Developer app at developer.spotify.com
  Set redirect URI to http://127.0.0.1:8888/callback
  Run get_refresh_token.py to complete OAuth and get your refresh token
  Paste your Client ID, Client Secret, and Refresh Token into the sketch
  Flash to ESP32
  On first boot, connect to the SpotifyDesk WiFi hotspot and enter your home WiFi credentials

