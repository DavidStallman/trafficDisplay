"""Quick PC-side check of your Google Routes API key (no board needed).

Reads GOOGLE_MAPS_API_KEY, ORIGIN_ADDRESS, and DESTINATION_ADDRESS from
include/secrets.h by default, or pass the API key as a command-line argument.
"""
import json
import os
import pathlib
import re
import sys
import urllib.error
import urllib.request

secrets_path = pathlib.Path(__file__).resolve().parent.parent / "include" / "secrets.h"
defines = {}
if secrets_path.exists():
    defines = dict(re.findall(r'#define\s+(\w+)\s+"((?:[^"\\]|\\.)*)"', secrets_path.read_text()))

ORIGIN = os.environ.get("ORIGIN_ADDRESS") or defines.get("ORIGIN_ADDRESS", "")
DESTINATION = os.environ.get("DESTINATION_ADDRESS") or defines.get("DESTINATION_ADDRESS", "")
key = (
    sys.argv[1]
    if len(sys.argv) > 1
    else os.environ.get("GOOGLE_MAPS_API_KEY") or defines.get("GOOGLE_MAPS_API_KEY", "")
)
if not key or not ORIGIN or not DESTINATION:
    sys.exit("Populate include/secrets.h (or pass API key and set ORIGIN_ADDRESS / DESTINATION_ADDRESS).")

body = json.dumps({
    "origin": {"address": ORIGIN},
    "destination": {"address": DESTINATION},
    "travelMode": "DRIVE",
    "routingPreference": "TRAFFIC_AWARE",
    "units": "IMPERIAL",
}).encode()
req = urllib.request.Request(
    "https://routes.googleapis.com/directions/v2:computeRoutes",
    data=body,
    headers={
        "Content-Type": "application/json",
        "X-Goog-Api-Key": key,
        "X-Goog-FieldMask": "routes.duration,routes.staticDuration,routes.distanceMeters,routes.description",
    },
)
try:
    with urllib.request.urlopen(req, timeout=20) as resp:
        data = json.load(resp)
except urllib.error.HTTPError as e:
    sys.exit(f"HTTP {e.code}: {e.read().decode()}")

r = data["routes"][0]
dur = int(r["duration"].rstrip("s")) / 60
static = int(r.get("staticDuration", "0s").rstrip("s")) / 60
print(f"{ORIGIN} -> {DESTINATION}")
print(f"  With traffic : {dur:.0f} min")
print(f"  No traffic   : {static:.0f} min")
print(f"  Distance     : {r['distanceMeters'] / 1609.344:.1f} mi via {r.get('description', '?')}")
color = "GREEN" if dur < 40 else "YELLOW" if dur <= 50 else "RED"
print(f"  Screen color : {color}")
