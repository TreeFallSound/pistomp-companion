#ifndef __JB_LOG_HPP__
#define __JB_LOG_HPP__

// os_log shim. Subsystem "com.jackbridge"; categories split so the HAL plugin
// (which logs to the same subsystem) and operators can grep cleanly:
//   log show --predicate 'subsystem == "com.jackbridge"'
//   log show --predicate 'subsystem == "com.jackbridge" && category == "jack"'
//
// Stick to format-string literals — os_log redacts dynamic strings as <private>
// by default. Use %{public}s when you really mean it.

#include <os/log.h>

inline os_log_t jb_log_daemon() {
    static os_log_t l = os_log_create("com.jackbridge", "daemon");
    return l;
}

inline os_log_t jb_log_driver() {
    static os_log_t l = os_log_create("com.jackbridge", "driver");
    return l;
}

inline os_log_t jb_log_shm() {
    static os_log_t l = os_log_create("com.jackbridge", "shm");
    return l;
}

inline os_log_t jb_log_jack() {
    static os_log_t l = os_log_create("com.jackbridge", "jack");
    return l;
}

#define JB_LOG_ERR(log, fmt, ...)    os_log_error((log), fmt, ##__VA_ARGS__)
#define JB_LOG_INFO(log, fmt, ...)   os_log_info((log), fmt, ##__VA_ARGS__)
#define JB_LOG_DEBUG(log, fmt, ...)  os_log_debug((log), fmt, ##__VA_ARGS__)
#define JB_LOG_DEFAULT(log, fmt, ...) os_log((log), fmt, ##__VA_ARGS__)

#endif
