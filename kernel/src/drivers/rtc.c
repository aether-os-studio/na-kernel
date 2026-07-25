#include <boot/boot.h>
#include <drivers/logger.h>
#include <drivers/rtc.h>

static rtc_device_t *default_rtc;
static spinlock_t rtc_lock = SPIN_INIT;

static bool rtc_is_leap_year(uint64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int rtc_days_in_month(uint64_t year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2 && rtc_is_leap_year(year))
        return 29;

    return days[month - 1];
}

bool rtc_time_valid(const rtc_time_t *tm) {
    uint64_t year;

    if (!tm)
        return false;

    year = (uint64_t)tm->tm_year;
    if (year < 1970 || tm->tm_mon < 1 || tm->tm_mon > 12 || tm->tm_mday < 1 ||
        tm->tm_hour < 0 || tm->tm_hour > 23 || tm->tm_min < 0 ||
        tm->tm_min > 59 || tm->tm_sec < 0 || tm->tm_sec > 59) {
        return false;
    }

    return tm->tm_mday <= rtc_days_in_month(year, tm->tm_mon);
}

uint64_t rtc_time_to_seconds(const rtc_time_t *tm) {
    uint64_t days = 0;
    uint64_t year;

    if (!rtc_time_valid(tm))
        return 0;

    for (year = 1970; year < (uint64_t)tm->tm_year; year++)
        days += rtc_is_leap_year(year) ? 366 : 365;

    for (int month = 1; month < tm->tm_mon; month++)
        days += (uint64_t)rtc_days_in_month((uint64_t)tm->tm_year, month);

    days += (uint64_t)tm->tm_mday - 1;

    return days * 86400ULL + (uint64_t)tm->tm_hour * 3600ULL +
           (uint64_t)tm->tm_min * 60ULL + (uint64_t)tm->tm_sec;
}

void rtc_seconds_to_time(uint64_t seconds, rtc_time_t *tm) {
    uint64_t days;
    uint64_t rem;
    uint64_t year = 1970;
    int month = 1;

    if (!tm)
        return;

    days = seconds / 86400ULL;
    rem = seconds % 86400ULL;

    tm->tm_hour = (int)(rem / 3600ULL);
    rem %= 3600ULL;
    tm->tm_min = (int)(rem / 60ULL);
    tm->tm_sec = (int)(rem % 60ULL);

    while (true) {
        uint64_t year_days = rtc_is_leap_year(year) ? 366 : 365;
        if (days < year_days)
            break;
        days -= year_days;
        year++;
    }

    while (true) {
        int month_days = rtc_days_in_month(year, month);
        if (days < (uint64_t)month_days)
            break;
        days -= (uint64_t)month_days;
        month++;
    }

    tm->tm_year = (int)year;
    tm->tm_mon = month;
    tm->tm_mday = (int)days + 1;
}

int rtc_register_device(rtc_device_t *rtc) {
    if (!rtc || !rtc->ops || !rtc->ops->read_time)
        return -EINVAL;

    spin_lock(&rtc_lock);
    if (!default_rtc)
        default_rtc = rtc;
    spin_unlock(&rtc_lock);

    printk("rtc: registered %s\n", rtc->name ? rtc->name : "unknown");
    return 0;
}

rtc_device_t *rtc_get_default(void) {
    rtc_device_t *rtc;

    spin_lock(&rtc_lock);
    rtc = default_rtc;
    spin_unlock(&rtc_lock);

    return rtc;
}

int rtc_read_time(rtc_time_t *tm) {
    rtc_device_t *rtc = rtc_get_default();

    if (!rtc || !rtc->ops->read_time)
        return -ENODEV;

    return rtc->ops->read_time(rtc, tm);
}

int rtc_set_time(const rtc_time_t *tm) {
    rtc_device_t *rtc = rtc_get_default();

    if (!rtc || !rtc->ops->set_time)
        return -ENODEV;

    return rtc->ops->set_time(rtc, tm);
}

int rtc_read_alarm(rtc_alarm_t *alarm) {
    rtc_device_t *rtc = rtc_get_default();

    if (!rtc || !rtc->ops->read_alarm)
        return -ENODEV;

    return rtc->ops->read_alarm(rtc, alarm);
}

int rtc_set_alarm(const rtc_alarm_t *alarm) {
    rtc_device_t *rtc = rtc_get_default();

    if (!rtc || !rtc->ops->set_alarm)
        return -ENODEV;

    return rtc->ops->set_alarm(rtc, alarm);
}

int rtc_alarm_enable_irq(bool enabled) {
    rtc_device_t *rtc = rtc_get_default();

    if (!rtc || !rtc->ops->alarm_enable_irq)
        return -ENODEV;

    return rtc->ops->alarm_enable_irq(rtc, enabled);
}

int rtc_read_realtime(rtc_realtime_t *time) {
    rtc_device_t *rtc;
    uint64_t mono_ns;
    int ret;

    if (!time)
        return -EINVAL;

    rtc = rtc_get_default();
    if (rtc && rtc->ops->read_realtime) {
        ret = rtc->ops->read_realtime(rtc, time);
        if (ret == 0)
            return 0;
    }

    /*
     * A calendar-only RTC has one-second resolution.  Combining a freshly
     * sampled RTC second with an unrelated monotonic fractional second can
     * move CLOCK_REALTIME backwards whenever the two counters roll over at
     * slightly different instants.  The boot protocol timestamp and the
     * monotonic clock form one stable epoch instead.  RTCs which expose an
     * atomic high-resolution read remain handled by read_realtime above.
     */
    mono_ns = nano_time();
    time->sec = boot_get_boottime() + mono_ns / 1000000000ULL;
    time->nsec = (uint32_t)(mono_ns % 1000000000ULL);
    return 0;
}

void rtc_handle_alarm_irq(void) {}
