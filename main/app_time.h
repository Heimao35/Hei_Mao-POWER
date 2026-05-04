/**
 * @file app_time.h
 * @brief Wall-clock time: SNTP when STA has IP; hook for future external RTC.
 *
 * Priority when reading local civil time (once RTC is integrated):
 * 1) Optional RTC reader if registered and returns success
 * 2) libc time after SNTP has synchronized at least once while associated
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

/** True after SNTP sync callback (libc time is trustworthy for display). */
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

#ifdef __cplusplus
}
#endif

#endif /* APP_TIME_H */
