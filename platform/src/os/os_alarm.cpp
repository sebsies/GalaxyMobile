// OS time base, alarms (the decrementer), and miscellaneous OS services.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "port/port.h"
#include "revolution/os.h"
#include "revolution/os/OSError.h"
#include "revolution/os/OSReset.h"
#include "revolution/os/OSResetSW.h"

// ---------------------------------------------------------------------------
// Time base.  OSTime counts timer ticks (bus clock / 4 = 60.75 MHz) since
// 2000-01-01 00:00:00, like the console after the IPL loads the RTC.
// ---------------------------------------------------------------------------
static constexpr int64_t kTimerClock = 243000000 / 4;
static int64_t sBootNs;
static int64_t sBootTicks;

extern "C" int64_t port_host_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

extern "C" void port_host_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ll);
    ts.tv_nsec = (long)(ns % 1000000000ll);
    nanosleep(&ts, nullptr);
}

static inline int64_t nsToTicks(int64_t ns) { return (ns / 4000) * 243 + ((ns % 4000) * 243) / 4000; }
static inline int64_t ticksToNs(int64_t ticks) { return (ticks / 243) * 4000 + ((ticks % 243) * 4000) / 243; }

static void initTimeBase() {
    sBootNs = port_host_time_ns();
    // Seconds from 2000-01-01 to now, in local time (the Wii RTC is local).
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    int64_t localNow = (int64_t)now + lt.tm_gmtoff;
    const int64_t kEpoch2000 = 946684800;  // 2000-01-01 in Unix time
    sBootTicks = (localNow - kEpoch2000) * kTimerClock;
}

static inline OSTime rawTime() { return sBootTicks + nsToTicks(port_host_time_ns() - sBootNs); }

extern "C" {

OSTime OSGetTime(void) {
    port_irq_poll();
    return rawTime();
}

OSTick OSGetTick(void) {
    port_irq_poll();
    return (OSTick)rawTime();
}

OSTime __OSGetSystemTime(void) { return rawTime(); }
OSTime __OSTimeToSystemTime(OSTime t) { return t; }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    int64_t totalUs = ticks / 243 * 4 + (ticks % 243) * 4 / 243;  // 60.75 ticks per microsecond
    int64_t secs = totalUs / 1000000;
    int64_t usecs = totalUs % 1000000;
    if (usecs < 0) {
        usecs += 1000000;
        secs -= 1;
    }
    time_t t = (time_t)(secs + 946684800);
    struct tm tmv;
    if (gmtime_r(&t, &tmv) == nullptr) {
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_mday = 1;
        tmv.tm_year = 100;
    }
    td->sec = tmv.tm_sec;
    td->min = tmv.tm_min;
    td->hour = tmv.tm_hour;
    td->mday = tmv.tm_mday;
    td->mon = tmv.tm_mon;
    td->year = tmv.tm_year + 1900;
    td->wday = tmv.tm_wday;
    td->yday = tmv.tm_yday;
    td->msec = (int)(usecs / 1000);
    td->usec = (int)(usecs % 1000);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Alarms.  The queue is owned by the CPU; a host timer thread raises the
// alarm interrupt when the earliest deadline passes.
// ---------------------------------------------------------------------------
static OSAlarm* sAlarmHead;
static std::mutex sTimerMutex;
static std::condition_variable sTimerCv;
static int64_t sTimerDeadline = INT64_MAX;  // in OSTime ticks, guarded by sTimerMutex

static void rearmTimer() {
    std::lock_guard<std::mutex> lk(sTimerMutex);
    sTimerDeadline = sAlarmHead ? sAlarmHead->fire : INT64_MAX;
    sTimerCv.notify_all();
}

static void timerThread() {
    std::unique_lock<std::mutex> lk(sTimerMutex);
    for (;;) {
        if (sTimerDeadline == INT64_MAX) {
            sTimerCv.wait(lk);
            continue;
        }
        int64_t now = rawTime();
        if (now >= sTimerDeadline) {
            sTimerDeadline = INT64_MAX;  // re-armed by the handler
            lk.unlock();
            port_irq_raise(PORT_IRQ_ALARM);
            lk.lock();
            continue;
        }
        int64_t waitNs = ticksToNs(sTimerDeadline - now);
        sTimerCv.wait_for(lk, std::chrono::nanoseconds(waitNs));
    }
}

static void insertAlarm(OSAlarm* alarm) {
    OSAlarm* prev = nullptr;
    OSAlarm* next = sAlarmHead;
    while (next && next->fire <= alarm->fire) {
        prev = next;
        next = next->next;
    }
    alarm->prev = prev;
    alarm->next = next;
    if (next) {
        next->prev = alarm;
    }
    if (prev) {
        prev->next = alarm;
    } else {
        sAlarmHead = alarm;
    }
}

static void removeAlarm(OSAlarm* alarm) {
    if (alarm->next) {
        alarm->next->prev = alarm->prev;
    }
    if (alarm->prev) {
        alarm->prev->next = alarm->next;
    } else if (sAlarmHead == alarm) {
        sAlarmHead = alarm->next;
    }
    alarm->prev = alarm->next = nullptr;
}

static void alarmIrq() {
    OSTime now = rawTime();
    while (sAlarmHead && sAlarmHead->fire <= now) {
        OSAlarm* alarm = sAlarmHead;
        removeAlarm(alarm);
        OSAlarmHandler handler = alarm->handler;
        if (alarm->period) {
            alarm->fire += alarm->period;
            if (alarm->fire <= now) {
                // Skip missed periods rather than firing a burst.
                OSTime missed = (now - alarm->fire) / alarm->period + 1;
                alarm->fire += missed * alarm->period;
            }
            insertAlarm(alarm);
        } else {
            alarm->handler = nullptr;
        }
        if (handler) {
            handler(alarm, OSGetCurrentContext());
        }
    }
    rearmTimer();
}

extern "C" {

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = nullptr;
    alarm->tag = 0;
    alarm->prev = alarm->next = nullptr;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = 0;
    alarm->handler = handler;
    alarm->fire = rawTime() + tick;
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = 0;
    alarm->handler = handler;
    alarm->fire = time;
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = period;
    alarm->start = start;
    alarm->handler = handler;
    OSTime now = rawTime();
    if (period > 0 && start < now) {
        alarm->fire = start + period * ((now - start) / period + 1);
    } else {
        alarm->fire = start;
    }
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSCancelAlarm(OSAlarm* alarm) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
        alarm->handler = nullptr;
        rearmTimer();
    }
    OSRestoreInterrupts(en);
}

void OSCancelAlarms(u32 tag) {
    BOOL en = OSDisableInterrupts();
    for (OSAlarm* a = sAlarmHead; a;) {
        OSAlarm* n = a->next;
        if (a->tag == tag) {
            removeAlarm(a);
            a->handler = nullptr;
        }
        a = n;
    }
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { alarm->tag = tag; }
void OSSetAlarmUserData(OSAlarm* alarm, void* data) { alarm->userData = data; }
void* OSGetAlarmUserData(const OSAlarm* alarm) { return alarm->userData; }

}  // extern "C"

// ---------------------------------------------------------------------------
// Reporting / fatal errors
// ---------------------------------------------------------------------------
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "PetariVR"
#endif

// Set by the headless runner so logs also reach the adb shell.
extern "C" int port_log_to_stderr = 0;

// The app's own copy of its log (port_log_file): the system log keeps only a
// few minutes of a busy headset, too little to look into a session after it.
static std::mutex sLogFileLock;
static FILE* sLogFile = nullptr;
static long sLogFileBytes = 0;
static const long kLogFileMax = 16 << 20;

extern "C" void port_log_file(const char* path) {
    // The previous session's log is kept beside it as <path>.prev.
    char prev[512];
    snprintf(prev, sizeof(prev), "%s.prev", path);
    rename(path, prev);
    std::lock_guard<std::mutex> lock(sLogFileLock);
    sLogFile = fopen(path, "w");
    if (sLogFile) {
        setvbuf(sLogFile, nullptr, _IOLBF, 1024);  // each line reaches the file at once, crash or not
    }
}

static void logToFile(const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(sLogFileLock);
    if (!sLogFile || sLogFileBytes > kLogFileMax) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = fprintf(sLogFile, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    n += vfprintf(sLogFile, fmt, args);
    fputc('\n', sLogFile);
    sLogFileBytes += n > 0 ? n + 1 : 1;
}

extern "C" void port_vlog(const char* fmt, va_list args) {
    {
        va_list copy;
        va_copy(copy, args);
        logToFile(fmt, copy);
        va_end(copy);
    }
#ifdef __ANDROID__
    if (port_log_to_stderr) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
        va_end(copy);
    }
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, args);
#else
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
#endif
}

extern "C" void port_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    port_vlog(fmt, args);
    va_end(args);
}

extern "C" void port_fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    {
        va_list copy;
        va_copy(copy, args);
        logToFile(fmt, copy);
        va_end(copy);
    }
#ifdef __ANDROID__
    if (port_log_to_stderr) {
        va_list copy;
        va_copy(copy, args);
        fputs("FATAL: ", stderr);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
        va_end(copy);
    }
    __android_log_vprint(ANDROID_LOG_FATAL, LOG_TAG, fmt, args);
#else
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
#endif
    va_end(args);
    abort();
}

extern "C" {

void OSVReport(const char* fmt, va_list args) {
    // Game messages are Shift-JIS; log them verbatim.
    char buf[1024];
    port_vsnprintf(buf, sizeof(buf), fmt, args);  // game format strings (MSL semantics)
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
    if (n) {
        port_log("[OS] %s", buf);
    }
}

void OSReport(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    OSVReport(fmt, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    port_vsnprintf(buf, sizeof(buf), fmt, args);  // game format strings (MSL semantics)
    va_end(args);
    port_fatal("OSPanic %s:%d: %s", file, line, buf);
}

void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    port_fatal("OSFatal: %s", msg);
}

// Error handlers (exceptions never reach the game on the host).
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error;
    (void)handler;
    return nullptr;
}

void OSRegisterVersion(const char* version) { port_log("OSRegisterVersion: %s", version); }

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) { (void)info; }

OSPowerCallback OSSetPowerCallback(OSPowerCallback cb) {
    (void)cb;
    return nullptr;
}

BOOL OSGetResetButtonState(void) { return FALSE; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
void OSRestart(u32 code) { port_fatal("OSRestart(%u)", code); }
void OSRebootSystem(void) { port_fatal("OSRebootSystem"); }
void OSReturnToMenu(void) { port_fatal("OSReturnToMenu"); }
void OSShutdownSystem(void) { port_fatal("OSShutdownSystem"); }
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)forceMenu;
    port_fatal("OSResetSystem(%d, %u)", reset, resetCode);
}

void OSInit(void) {}

}  // extern "C"

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
extern "C" void port_os_time_init(void) {
    initTimeBase();
    port_irq_set_handler(PORT_IRQ_ALARM, alarmIrq);
    std::thread(timerThread).detach();
}
