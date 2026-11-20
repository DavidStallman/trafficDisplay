#pragma once
// ---------------------------------------------------------------------------
// User-tunable settings (non-secret). Secrets live in secrets.h.
// ---------------------------------------------------------------------------

// Route label (origin & destination addresses live in secrets.h)
#define ROUTE_LABEL         "Work > Home"

// Polling schedule, local time (24h). Each window polls on every
// INTERVAL-minute mark from START through END inclusive. A minute that falls
// in two windows (e.g. 14:00) is only checked once.
//   {startHour, startMin, endHour, endMin, intervalMin}
#define POLL_WINDOWS { \
    { 7, 0, 14, 0, 15},  /*  7:00 AM - 2:00 PM every 15 min */ \
    {14, 0, 17, 0,  5},  /*  2:00 PM - 4:00 PM every  5 min */ \
    {17, 0, 23, 0,  20},  /*  2:00 PM - 11:00 PM every  20 min */ \
}
// Checks/day with the above: 24 + 61 = 85  (~1,850-1,950 per month on weekdays)

// Days to poll: bit 0 = Sunday ... bit 6 = Saturday.
// 0x7F = every day, 0x3E = Monday-Friday only.
#define ACTIVE_DAYS_MASK    0x3E

// Color thresholds in minutes.
//   < GREEN_MAX_MIN            -> green
//   GREEN_MAX_MIN..YELLOW_MAX  -> yellow
//   > YELLOW_MAX_MIN           -> red
#define GREEN_MAX_MIN       45
#define YELLOW_MAX_MIN      55

// Time zone (POSIX TZ string): US Pacific with DST
#define TZ_INFO             "PST8PDT,M3.2.0,M11.1.0"

// Screen brightness (0-255)
#define BRIGHTNESS_ACTIVE   3    // ~1%
#define BRIGHTNESS_IDLE     2

// Screen orientation (landscape): 1 = USB port on the right, 3 = flipped 180 (USB on the left)
#define DISPLAY_ROTATION    3

// How long a manual (button A) refresh result stays on screen outside the window
#define MANUAL_SHOW_SECONDS 300

// If a fetch fails during the window, retry after this many seconds
#define RETRY_SECONDS       60
