# Commute Traffic Display (M5StickC Plus2)

Shows the live arrival time and drive time between your work and home addresses
on the M5StickC Plus2 screen. It polls the Google Routes API on **weekdays**:
every 15 min from 8:00 AM to 2:00 PM, then every 5 min from 2:00 to 7:00 PM.
That's 85 checks a day (about 1,850–1,950 a month), well under the free 5,000 a month.

| Drive time | Background |
|---|---|
| under 40 min | 🟩 green |
| 40 – 50 min | 🟨 yellow |
| over 50 min | 🟥 red |

Outside the window the screen dims and shows "Off hours" plus the last reading.

**Buttons**
- **A** (big front button "M5"): fetch now. Works any time, and the colored result stays up for 5 minutes.
- **B** (right-side button): cycles demo screens (green → yellow → red → normal) so you can check the colors.

---

## 1. Google Maps API key (one-time)

1. Go to https://console.cloud.google.com/ and create a project (e.g. "commute-display").
2. **Billing → Link a billing account.** Google requires one even for free-tier use.
   This device makes about 19 calls a day (about 600 a month), which is well inside the
   Routes API's monthly free allowance, so expect a \$0 bill. If you want a hard cap,
   set a budget alert.
3. **APIs & Services → Library →** search **"Routes API"** → **Enable**.
4. **APIs & Services → Credentials → Create credentials → API key.**
5. Click the new key → **API restrictions → Restrict key → Routes API** → Save.
6. Test it from the PC:
   ```
   python tools/test_route.py YOUR_API_KEY
   ```
   It should print the current drive time.

## 2. Secrets file

Copy `include/secrets.example.h` to `include/secrets.h` (or run `tools/set_secrets.ps1`) and fill in:
```c
#define WIFI_SSID           "YourWiFi"      // 2.4 GHz only. ESP32 can't use 5 GHz
#define WIFI_PASSWORD       "password"
#define GOOGLE_MAPS_API_KEY "AIza..."
#define ORIGIN_ADDRESS      "123 Work St, City, State"
#define DESTINATION_ADDRESS "456 Home Ave, City, State"
```
`secrets.h` is git-ignored.

## 3. Build & flash

```
pip install platformio
cd traffic-display
python -m platformio run -t upload --upload-port COM4
python -m platformio device monitor -p COM4     # optional: view logs
```

## Customizing

All non-secret settings are in `include/config.h`: time windows, poll intervals, color
thresholds, brightness, orientation, and active days (`ACTIVE_DAYS_MASK = 0x3E` for weekdays only).

## Notes
- The Plus2 has a battery, but it's meant to stay plugged in. To power it off,
  hold the left side button for about 6 seconds. Press it once to turn it back on.
- TLS certificate checking is turned off (`setInsecure`) to keep the firmware simple.
  The connection is still encrypted.
