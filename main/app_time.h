/**
 * @file app_time.h
 * @brief Wall-clock time: SNTP when STA has IP; hook for future external RTC.
 *
 * Display time priority (@ref app_time_local_tm):
 * 1) External RTC when registered and read succeeds (primary; includes while STA is up)
 * 2) libc local time only if SNTP has synced and RTC is unavailable / invalid (e.g. OS flag)
 */
#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Call once after @c net_wifi_init (needs default event loop + netif). */
void app_time_init(void);

/** True after SNTP sync callback (libc time is suitable for calibrating RTC, not primary for UI). */
bool app_time_wall_clock_synced(void);

/**
 * Fill @p out with local civil time (respects TZ env).
 * @return false if no valid source (no RTC and SNTP not synced since last STA drop).
 */
bool app_time_local_tm(struct tm *out);

/**
 * Optional external RTC (e.g. I2C). When non-NULL, @ref app_time_local_tm tries this first.
 * Return false if RTC unread / not fitted yet.
 */
typedef bool (*app_time_rtc_read_fn)(struct tm *out_local);
void app_time_register_rtc_reader(app_time_rtc_read_fn fn);

/** Called each time SNTP reports a successful sync (e.g. start periodic RTC calibration). */
typedef void (*app_time_wall_sync_fn)(void);
void app_time_register_wall_sync_handler(app_time_wall_sync_fn fn);

/** Called when STA drops / SNTP is stopped — stop background calibration. */
typedef void (*app_time_net_lost_fn)(void);
void app_time_register_net_lost_handler(app_time_net_lost_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* APP_TIME_H */
