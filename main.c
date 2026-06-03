/*
 * key2gestured — Android-style gesture scrolling for BlackBerry KEY2
 * Copyright (C) 2026 Marsh Jiang
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <limits.h>
#include <ctype.h>

#include <linux/input.h>
#include <libevdev/libevdev.h>

#include "state.h"
#include "uinput.h"

/*
 * Device paths
 */
#define DEFAULT_TOUCH_DEVICE   "/dev/input/by-path/platform-c175000.i2c-event"
#define FALLBACK_TOUCH_DEVICE  "/dev/input/event1"
#define DEFAULT_KEYBOARD_DEVICE "/dev/input/event7"
#define FALLBACK_KEYBOARD_DEVICE "/dev/input/event2"
#define TOUCH_DEVICE_NAME      "touch_keypad"
#define KEYD_KEYBOARD_NAME     "keyd virtual keyboard"
#define KEYBOARD_DEVICE_NAME   "stmpe_keypad"
#define CONFIG_PATH           "/etc/key2gestured/default.conf"
#define PID_PATH              "/run/key2gestured.pid"
#define READY_PATH            "/run/key2gestured.ready"
#define CONFIG_LINE_MAX       512
#define RELOAD_FIND_RETRIES   20
#define RELOAD_FIND_DELAY_US  100000
#define CGROUP_V2_PROCS       "/sys/fs/cgroup/system.slice/key2gestured.service/cgroup.procs"
#define CGROUP_V1_TASKS       "/sys/fs/cgroup/system.slice/key2gestured.service/tasks"

/*
 * Global state
 */
Finger fingers[MAX_FINGERS] = {0};
AppState state = STATE_IDLE;
int touch_fd = -1;
long long last_key_time_ms = 0;

static int current_slot = 0;
static int keyboard_fd = -1;
static struct libevdev *keyboard_dev = NULL;
static volatile sig_atomic_t running = 1;
static const char *touch_device_path = DEFAULT_TOUCH_DEVICE;
static const char *keyboard_device_path = DEFAULT_KEYBOARD_DEVICE;
static int touch_device_path_overridden = 0;
static int keyboard_device_path_overridden = 0;
static int scan_input_devices = 0;
static volatile sig_atomic_t reload_requested = 0;
static volatile sig_atomic_t shutdown_requested = 0;

/* Runtime tunables. Defaults are defined in state.h. */
static int cfg_typing_cooldown_ms = TYPING_COOLDOWN_MS;
static int cfg_scroll_threshold = SCROLL_THRESHOLD;
static int cfg_horizontal_scroll_threshold = HORIZONTAL_SCROLL_THRESHOLD;
static int cfg_gesture_timeout_ms = GESTURE_TIMEOUT_MS;
static float cfg_momentum_decay = MOMENTUM_DECAY;
static float cfg_momentum_min_velocity = MOMENTUM_MIN_VELOCITY;
static int cfg_momentum_interval_ms = MOMENTUM_INTERVAL_MS;
static int cfg_max_delta_per_event = MAX_DELTA_PER_EVENT;

typedef struct {
    int typing_cooldown_ms;
    int scroll_threshold;
    int horizontal_scroll_threshold;
    int gesture_timeout_ms;
    float momentum_decay;
    float momentum_min_velocity;
    int momentum_interval_ms;
    int max_delta_per_event;
    int scroll_scale_x;
    int scroll_scale_y;
} RuntimeConfig;

/* Momentum state */
static float momentum_vx = 0.0f;
static float momentum_vy = 0.0f;
static long long momentum_start_ms = 0;
static long long momentum_last_step = 0;

/* Gesture timing */
static long long gesture_start_ms = 0;

/* Touch injection state tracking */
static int touch_injected = 0;
static int total_dy = 0;
static int total_dx = 0;
static int prev_y = 0;
static int prev_x = 0;

/* Velocity tracking (for momentum) */
#define VELOCITY_SAMPLES 5
static float velocity_samples_x[VELOCITY_SAMPLES] = {0};
static float velocity_samples_y[VELOCITY_SAMPLES] = {0};
static int velocity_idx = 0;
static int velocity_count = 0;

/* ─── Time helpers ─── */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (long long)ts.tv_sec * 1000 +
           (long long)ts.tv_nsec / 1000000;
}

/* ─── Finger helpers ─── */

static void reset_finger(int slot)
{
    fingers[slot].active   = 0;
    fingers[slot].x        = 0;
    fingers[slot].y        = 0;
    fingers[slot].start_x  = 0;
    fingers[slot].start_y  = 0;
}

static void reset_all_fingers(void)
{
    for (int i = 0; i < MAX_FINGERS; i++)
        reset_finger(i);
}

static int slot_is_valid(int slot)
{
    return slot >= 0 && slot < MAX_FINGERS;
}

/* ─── Velocity tracking ─── */

static void record_velocity(int dx, int dy)
{
    velocity_samples_x[velocity_idx] = (float)dx;
    velocity_samples_y[velocity_idx] = (float)dy;
    velocity_idx = (velocity_idx + 1) % VELOCITY_SAMPLES;
    if (velocity_count < VELOCITY_SAMPLES)
        velocity_count++;
}

static float average_velocity_x(void)
{
    if (velocity_count == 0)
        return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < velocity_count; i++)
        sum += velocity_samples_x[i];
    return sum / velocity_count;
}

static float average_velocity_y(void)
{
    if (velocity_count == 0)
        return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < velocity_count; i++)
        sum += velocity_samples_y[i];
    return sum / velocity_count;
}

static void clear_velocity(void)
{
    velocity_idx = 0;
    velocity_count = 0;
    for (int i = 0; i < VELOCITY_SAMPLES; i++) {
        velocity_samples_x[i] = 0.0f;
        velocity_samples_y[i] = 0.0f;
    }
}

static void end_injected_touch(void)
{
    if (touch_injected) {
        touch_inject_up();
        touch_injected = 0;
    }
}

static void cancel_scroll_state(const char *reason)
{
    if (!shutdown_requested)
        end_injected_touch();
    state = STATE_IDLE;
    reset_all_fingers();
    clear_velocity();
    printf("%s\n", reason);
}

static int open_named_input_device(const char *name, const char **matched_path)
{
    DIR *dir;
    struct dirent *entry;
    int found_fd = -1;

    dir = opendir("/dev/input");
    if (!dir) {
        perror("opendir /dev/input");
        return -1;
    }

    while (running && (entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct libevdev *dev = NULL;
        int fd;
        const char *dev_name;

        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        if (libevdev_new_from_fd(fd, &dev) < 0) {
            close(fd);
            continue;
        }

        dev_name = libevdev_get_name(dev);
        if (dev_name && strcmp(dev_name, name) == 0) {
            char *saved_path = strdup(path);

            libevdev_free(dev);
            if (saved_path)
                *matched_path = saved_path;
            found_fd = fd;
            break;
        }

        libevdev_free(dev);
        close(fd);
    }

    closedir(dir);
    return found_fd;
}

static int open_input_path(const char *path)
{
    int fd;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        perror(path);

    return fd;
}

static int open_input_path_if_named(const char *path, const char *name,
                                    const char **matched_path)
{
    struct libevdev *dev = NULL;
    int fd;
    const char *dev_name;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (libevdev_new_from_fd(fd, &dev) < 0) {
        close(fd);
        return -1;
    }

    dev_name = libevdev_get_name(dev);
    if (dev_name && strcmp(dev_name, name) == 0) {
        libevdev_free(dev);
        *matched_path = path;
        return fd;
    }

    libevdev_free(dev);
    close(fd);
    return -1;
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        text++;

    if (*text == '\0')
        return text;

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return text;
}

static int parse_int_value(const char *name, const char *text,
                           int *value, int min, int max)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' ||
        parsed < min || parsed > max) {
        fprintf(stderr, "Ignoring invalid %s=%s\n", name, text);
        return -1;
    }

    *value = (int)parsed;
    return 0;
}

static int parse_float_value(const char *name, const char *text,
                             float *value, float min, float max)
{
    char *end = NULL;
    float parsed;

    errno = 0;
    parsed = strtof(text, &end);
    if (errno || end == text || *end != '\0' || !isfinite(parsed) ||
        parsed < min || parsed > max) {
        fprintf(stderr, "Ignoring invalid %s=%s\n", name, text);
        return -1;
    }

    *value = parsed;
    return 0;
}

static void load_env_int(const char *name, int *value, int min, int max)
{
    const char *text = getenv(name);

    if (!text || text[0] == '\0')
        return;

    parse_int_value(name, text, value, min, max);
}

static void load_env_float(const char *name, float *value,
                           float min, float max)
{
    const char *text = getenv(name);

    if (!text || text[0] == '\0')
        return;

    parse_float_value(name, text, value, min, max);
}

static RuntimeConfig default_runtime_config(void)
{
    RuntimeConfig config;

    config.typing_cooldown_ms = TYPING_COOLDOWN_MS;
    config.scroll_threshold = SCROLL_THRESHOLD;
    config.horizontal_scroll_threshold = HORIZONTAL_SCROLL_THRESHOLD;
    config.gesture_timeout_ms = GESTURE_TIMEOUT_MS;
    config.momentum_decay = MOMENTUM_DECAY;
    config.momentum_min_velocity = MOMENTUM_MIN_VELOCITY;
    config.momentum_interval_ms = MOMENTUM_INTERVAL_MS;
    config.max_delta_per_event = MAX_DELTA_PER_EVENT;
    config.scroll_scale_x = DEFAULT_SCROLL_SCALE_X;
    config.scroll_scale_y = DEFAULT_SCROLL_SCALE_Y;

    return config;
}

static void apply_runtime_config(const RuntimeConfig *config)
{
    cfg_typing_cooldown_ms = config->typing_cooldown_ms;
    cfg_scroll_threshold = config->scroll_threshold;
    cfg_horizontal_scroll_threshold = config->horizontal_scroll_threshold;
    cfg_gesture_timeout_ms = config->gesture_timeout_ms;
    cfg_momentum_decay = config->momentum_decay;
    cfg_momentum_min_velocity = config->momentum_min_velocity;
    cfg_momentum_interval_ms = config->momentum_interval_ms;
    cfg_max_delta_per_event = config->max_delta_per_event;
    touch_set_scroll_scale(config->scroll_scale_x, config->scroll_scale_y);
}

static void load_runtime_config_from_env(void)
{
    int scroll_scale_x = DEFAULT_SCROLL_SCALE_X;
    int scroll_scale_y = DEFAULT_SCROLL_SCALE_Y;

    load_env_int("KEY2GESTURED_TYPING_COOLDOWN_MS",
                 &cfg_typing_cooldown_ms, 0, 2000);
    load_env_int("KEY2GESTURED_SCROLL_THRESHOLD",
                 &cfg_scroll_threshold, 1, 500);
    load_env_int("KEY2GESTURED_HORIZONTAL_SCROLL_THRESHOLD",
                 &cfg_horizontal_scroll_threshold, 1, 1000);
    load_env_int("KEY2GESTURED_GESTURE_TIMEOUT_MS",
                 &cfg_gesture_timeout_ms, 50, 5000);
    load_env_float("KEY2GESTURED_MOMENTUM_DECAY",
                   &cfg_momentum_decay, 0.50f, 0.99f);
    load_env_float("KEY2GESTURED_MOMENTUM_MIN_VELOCITY",
                   &cfg_momentum_min_velocity, 0.0f, 50.0f);
    load_env_int("KEY2GESTURED_MOMENTUM_INTERVAL_MS",
                 &cfg_momentum_interval_ms, 1, 100);
    load_env_int("KEY2GESTURED_MAX_DELTA_PER_EVENT",
                 &cfg_max_delta_per_event, 1, 500);
    load_env_int("KEY2GESTURED_SCROLL_SCALE_X",
                 &scroll_scale_x, 1, 16);
    load_env_int("KEY2GESTURED_SCROLL_SCALE_Y",
                 &scroll_scale_y, 1, 16);

    touch_set_scroll_scale(scroll_scale_x, scroll_scale_y);
}

static int apply_config_pair(RuntimeConfig *config,
                             const char *key, const char *value,
                             int line_no)
{
    if (strcmp(key, "KEY2GESTURED_TYPING_COOLDOWN_MS") == 0)
        return parse_int_value(key, value, &config->typing_cooldown_ms,
                               0, 2000);
    if (strcmp(key, "KEY2GESTURED_SCROLL_THRESHOLD") == 0)
        return parse_int_value(key, value, &config->scroll_threshold,
                               1, 500);
    if (strcmp(key, "KEY2GESTURED_HORIZONTAL_SCROLL_THRESHOLD") == 0)
        return parse_int_value(key, value,
                               &config->horizontal_scroll_threshold,
                               1, 1000);
    if (strcmp(key, "KEY2GESTURED_GESTURE_TIMEOUT_MS") == 0)
        return parse_int_value(key, value, &config->gesture_timeout_ms,
                               50, 5000);
    if (strcmp(key, "KEY2GESTURED_MOMENTUM_DECAY") == 0)
        return parse_float_value(key, value, &config->momentum_decay,
                                 0.50f, 0.99f);
    if (strcmp(key, "KEY2GESTURED_MOMENTUM_MIN_VELOCITY") == 0)
        return parse_float_value(key, value,
                                 &config->momentum_min_velocity,
                                 0.0f, 50.0f);
    if (strcmp(key, "KEY2GESTURED_MOMENTUM_INTERVAL_MS") == 0)
        return parse_int_value(key, value,
                               &config->momentum_interval_ms,
                               1, 100);
    if (strcmp(key, "KEY2GESTURED_MAX_DELTA_PER_EVENT") == 0)
        return parse_int_value(key, value, &config->max_delta_per_event,
                               1, 500);
    if (strcmp(key, "KEY2GESTURED_SCROLL_SCALE_X") == 0)
        return parse_int_value(key, value, &config->scroll_scale_x, 1, 16);
    if (strcmp(key, "KEY2GESTURED_SCROLL_SCALE_Y") == 0)
        return parse_int_value(key, value, &config->scroll_scale_y, 1, 16);

    if (strcmp(key, "KEY2GESTURED_TOUCH_DEVICE") == 0 ||
        strcmp(key, "KEY2GESTURED_KEYBOARD_DEVICE") == 0 ||
        strcmp(key, "KEY2GESTURED_SCAN_INPUTS") == 0) {
        printf("[CONFIG] %s changes require restart\n", key);
        return 0;
    }

    fprintf(stderr, "Ignoring unknown config key %s on line %d\n",
            key, line_no);
    return 0;
}

static int load_runtime_config_from_file(const char *path,
                                         RuntimeConfig *config)
{
    FILE *file;
    char line[CONFIG_LINE_MAX];
    int line_no = 0;
    int errors = 0;

    file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT)
            return 0;

        perror(path);
        return -1;
    }

    while (fgets(line, sizeof(line), file)) {
        char *text;
        char *eq;
        char *key;
        char *value;

        line_no++;
        text = trim(line);
        if (text[0] == '\0' || text[0] == '#')
            continue;

        eq = strchr(text, '=');
        if (!eq) {
            fprintf(stderr, "Ignoring malformed config line %d\n", line_no);
            errors++;
            continue;
        }

        *eq = '\0';
        key = trim(text);
        value = trim(eq + 1);

        if (value[0] == '"' || value[0] == '\'') {
            char quote = value[0];
            size_t len = strlen(value);

            if (len >= 2 && value[len - 1] == quote) {
                value[len - 1] = '\0';
                value++;
            }
        }

        if (apply_config_pair(config, key, value, line_no) < 0)
            errors++;
    }

    if (ferror(file)) {
        perror(path);
        errors++;
    }

    fclose(file);
    return errors ? -1 : 0;
}

static int load_runtime_config_file(const char *action)
{
    RuntimeConfig config = default_runtime_config();

    if (load_runtime_config_from_file(CONFIG_PATH, &config) < 0) {
        fprintf(stderr, "[CONFIG] %s failed; keeping current settings\n",
                action);
        return -1;
    }

    apply_runtime_config(&config);
    printf("[CONFIG] %s %s\n", action, CONFIG_PATH);
    return 0;
}

static int reload_runtime_config(void)
{
    if (load_runtime_config_file("Reload") < 0)
        return -1;

    return 0;
}

static int write_pid_file(void)
{
    FILE *file;

    file = fopen(PID_PATH, "w");
    if (!file) {
        perror(PID_PATH);
        return -1;
    }

    fprintf(file, "%ld\n", (long)getpid());
    if (fclose(file) < 0) {
        perror(PID_PATH);
        return -1;
    }

    return 0;
}

static void remove_runtime_files(void)
{
    unlink(READY_PATH);
    unlink(PID_PATH);
}

static int write_ready_file(void)
{
    FILE *file;

    file = fopen(READY_PATH, "w");
    if (!file) {
        perror(READY_PATH);
        return -1;
    }

    fprintf(file, "%ld\n", (long)getpid());
    if (fclose(file) < 0) {
        perror(READY_PATH);
        return -1;
    }

    return 0;
}

static int read_pid_file(pid_t *pid)
{
    FILE *file;
    long parsed;

    file = fopen(PID_PATH, "r");
    if (!file)
        return -1;

    if (fscanf(file, "%ld", &parsed) != 1 || parsed <= 0) {
        fprintf(stderr, "Invalid pid file: %s\n", PID_PATH);
        fclose(file);
        return -1;
    }

    fclose(file);
    *pid = (pid_t)parsed;
    return 0;
}

static int read_ready_file(pid_t *pid)
{
    FILE *file;
    long parsed;

    file = fopen(READY_PATH, "r");
    if (!file)
        return -1;

    if (fscanf(file, "%ld", &parsed) != 1 || parsed <= 0) {
        fprintf(stderr, "Invalid ready file: %s\n", READY_PATH);
        fclose(file);
        return -1;
    }

    fclose(file);
    *pid = (pid_t)parsed;
    return 0;
}

static int read_process_comm(pid_t pid, char *comm, size_t comm_size)
{
    char path[64];
    FILE *file;

    snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    file = fopen(path, "r");
    if (!file)
        return -1;

    if (!fgets(comm, comm_size, file)) {
        fclose(file);
        return -1;
    }

    fclose(file);
    comm[strcspn(comm, "\n")] = '\0';
    return 0;
}

static int process_cmdline_contains(pid_t pid, const char *needle)
{
    char path[64];
    char cmdline[PATH_MAX];
    FILE *file;
    size_t len;

    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    file = fopen(path, "r");
    if (!file)
        return 0;

    len = fread(cmdline, 1, sizeof(cmdline) - 1, file);
    fclose(file);

    if (len == 0)
        return 0;

    cmdline[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (cmdline[i] == '\0')
            cmdline[i] = ' ';
    }

    return strstr(cmdline, needle) != NULL;
}

static int pid_is_daemon(pid_t pid)
{
    char comm[64];

    if (pid <= 0 || pid == getpid())
        return 0;

    if (kill(pid, 0) < 0)
        return 0;

    if (read_process_comm(pid, comm, sizeof(comm)) == 0 &&
        strcmp(comm, "key2gestured") == 0)
        return 1;

    return process_cmdline_contains(pid, "/usr/local/bin/key2gestured");
}

static int find_running_daemon(pid_t *pid)
{
    DIR *dir;
    struct dirent *entry;
    pid_t self = getpid();

    dir = opendir("/proc");
    if (!dir) {
        perror("opendir /proc");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *end = NULL;
        long parsed;
        errno = 0;
        parsed = strtol(entry->d_name, &end, 10);
        if (errno || end == entry->d_name || *end != '\0' || parsed <= 0)
            continue;

        if ((pid_t)parsed == self)
            continue;

        if (pid_is_daemon((pid_t)parsed)) {
            *pid = (pid_t)parsed;
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return -1;
}

static int read_cgroup_pid_file(const char *path, pid_t *pid)
{
    FILE *file;
    long parsed;
    pid_t self = getpid();

    file = fopen(path, "r");
    if (!file)
        return -1;

    while (fscanf(file, "%ld", &parsed) == 1) {
        if (parsed <= 0 || (pid_t)parsed == self)
            continue;

        if (kill((pid_t)parsed, 0) == 0) {
            fclose(file);
            *pid = (pid_t)parsed;
            return 0;
        }
    }

    fclose(file);
    return -1;
}

static int find_daemon_from_cgroup(pid_t *pid)
{
    if (read_cgroup_pid_file(CGROUP_V2_PROCS, pid) == 0)
        return 0;

    if (read_cgroup_pid_file(CGROUP_V1_TASKS, pid) == 0)
        return 0;

    return -1;
}

static int find_daemon_once(pid_t *pid)
{
    if (read_pid_file(pid) == 0 && pid_is_daemon(*pid))
        return 0;

    if (find_daemon_from_cgroup(pid) == 0 && pid_is_daemon(*pid))
        return 0;

    if (find_running_daemon(pid) == 0)
        return 0;

    return -1;
}

static int send_reload_signal(void)
{
    pid_t pid;
    int daemon_seen = 0;

    for (int i = 0; i < RELOAD_FIND_RETRIES; i++) {
        if (read_ready_file(&pid) == 0 && pid_is_daemon(pid)) {
            if (kill(pid, SIGHUP) == 0) {
                printf("Reload signal sent to key2gestured (pid %ld)\n",
                       (long)pid);
                return 0;
            }

            perror("kill SIGHUP");
            return 1;
        }

        if (!daemon_seen && find_daemon_once(&pid) == 0)
            daemon_seen = 1;

        usleep(RELOAD_FIND_DELAY_US);
    }

    if (daemon_seen) {
        fprintf(stderr,
                "key2gestured is running but not ready for reload yet. "
                "Wait for the Ready log line, then retry.\n");
        return 1;
    }

    fprintf(stderr,
            "Could not find running key2gestured daemon. "
            "Check `systemctl status key2gestured`.\n");
    return 1;
}

static int send_stop_signal(void)
{
    pid_t pid;

    if (read_ready_file(&pid) == 0 && pid_is_daemon(pid)) {
        if (kill(pid, SIGTERM) == 0) {
            printf("Stop signal sent to key2gestured (pid %ld)\n",
                   (long)pid);
            return 0;
        }

        if (errno != ESRCH) {
            perror("kill SIGTERM");
            return 1;
        }
    }

    if (read_pid_file(&pid) == 0 && pid_is_daemon(pid)) {
        if (kill(pid, SIGTERM) == 0) {
            printf("Stop signal sent to key2gestured (pid %ld)\n",
                   (long)pid);
            return 0;
        }

        if (errno != ESRCH) {
            perror("kill SIGTERM");
            return 1;
        }
    }

    remove_runtime_files();
    printf("key2gestured is not running\n");
    return 0;
}

/* ─── Typing suppression ─── */

static int is_typing_active(void)
{
    long long now = now_ms();
    return (now - last_key_time_ms) < cfg_typing_cooldown_ms;
}

static int setup_keyboard(void)
{
    struct libevdev *dev = NULL;
    int fd;

    if (keyboard_device_path_overridden) {
        fd = open_input_path(keyboard_device_path);
    } else {
        fd = open_input_path_if_named(DEFAULT_KEYBOARD_DEVICE,
                                      KEYD_KEYBOARD_NAME,
                                      &keyboard_device_path);
        if (fd < 0) {
            fd = open_input_path_if_named(FALLBACK_KEYBOARD_DEVICE,
                                          KEYBOARD_DEVICE_NAME,
                                          &keyboard_device_path);
        }
        if (fd < 0 && scan_input_devices) {
            printf("Scanning input devices for keyboard source\n");
            fd = open_named_input_device(KEYD_KEYBOARD_NAME,
                                         &keyboard_device_path);
        }
        if (fd < 0 && scan_input_devices)
            fd = open_named_input_device(KEYBOARD_DEVICE_NAME,
                                         &keyboard_device_path);
        if (fd < 0)
            fd = open_input_path(keyboard_device_path);
    }

    if (fd < 0) {
        perror("open keyboard device");
        return -1;
    }

    if (libevdev_new_from_fd(fd, &dev) < 0) {
        fprintf(stderr, "Failed to init libevdev for keyboard\n");
        close(fd);
        return -1;
    }

    printf("Keyboard device: %s\n", libevdev_get_name(dev));
    printf("  Path: %s\n", keyboard_device_path);
    keyboard_fd = fd;
    keyboard_dev = dev;
    return 0;
}

static void process_keyboard_events(void)
{
    if (!keyboard_dev)
        return;

    while (1) {
        struct input_event ev;
        int rc = libevdev_next_event(keyboard_dev,
                                     LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == -EAGAIN)
            break;
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS)
            continue;

        /* Track any key press (type 1, value 1) */
        if (ev.type == EV_KEY && ev.value == 1) {
            last_key_time_ms = now_ms();
        }
    }
}

/* ─── Gesture / scroll handling ─── */

static void inject_scroll_delta(int dx, int dy)
{
    if (dy > cfg_max_delta_per_event)
        dy = cfg_max_delta_per_event;
    else if (dy < -cfg_max_delta_per_event)
        dy = -cfg_max_delta_per_event;

    if (dx > cfg_max_delta_per_event)
        dx = cfg_max_delta_per_event;
    else if (dx < -cfg_max_delta_per_event)
        dx = -cfg_max_delta_per_event;

    record_velocity(dx, dy);
    touch_inject_move(dx, dy);
    touch_injected = 1;

    total_dy += dy;
    total_dx += dx;
}

static void enter_scroll_active(int new_x, int new_y, int dx, int dy)
{
    printf("[SCROLL] Enter SCROLL_ACTIVE (dy=%d, dx=%d)\n", dy, dx);

    state = STATE_SCROLL_ACTIVE;
    total_dy = 0;
    total_dx = 0;
    prev_y = new_y;
    prev_x = new_x;
    clear_velocity();

    /* Inject touch down at screen center */
    touch_inject_down();
    touch_injected = 1;

    inject_scroll_delta(dx, dy);
}

static void exit_scroll_active(void)
{
    if (state == STATE_SCROLL_ACTIVE) {
        /* Capture final velocity for momentum */
        momentum_vx = average_velocity_x();
        momentum_vy = average_velocity_y();
        float speed = sqrtf(momentum_vx * momentum_vx +
                            momentum_vy * momentum_vy);

        if (speed > cfg_momentum_min_velocity) {
            printf("[MOMENTUM] Starting (vx=%.1f, vy=%.1f, speed=%.1f)\n",
                   momentum_vx, momentum_vy, speed);
            state = STATE_MOMENTUM;
            momentum_start_ms = now_ms();
            momentum_last_step = 0;
        } else {
            /* No significant velocity, end touch */
            end_injected_touch();
            state = STATE_IDLE;
            clear_velocity();
            printf("[IDLE] Scroll ended, no momentum\n");
        }
    }
}

static void process_scroll_motion(int new_y, int new_x)
{
    if (!fingers[0].active)
        return;

    int dy = new_y - fingers[0].start_y;
    int dx = new_x - fingers[0].start_x;

    if (state == STATE_ONE_FINGER_PENDING) {
        /* Check if we exceed threshold to activate scrolling */
        int abs_dy = abs(dy);
        int abs_dx = abs(dx);

        if (abs_dy > cfg_scroll_threshold ||
            abs_dx > cfg_horizontal_scroll_threshold) {
            enter_scroll_active(new_x, new_y, dx, dy);
        } else {
            /* Check gesture timeout */
            long long elapsed = now_ms() - gesture_start_ms;
            if (elapsed > cfg_gesture_timeout_ms) {
                state = STATE_IDLE;
                reset_all_fingers();
            }
        }
        return;
    }

    if (state != STATE_SCROLL_ACTIVE)
        return;

    /*
     * In SCROLL_ACTIVE, inject touch moves.
     *
     * We send delta from the previous injected position,
     * not from the start position. This gives smooth
     * continuous scrolling.
     */
    int move_dy = new_y - prev_y;
    int move_dx = new_x - prev_x;

    inject_scroll_delta(move_dx, move_dy);
    prev_y = new_y;
    prev_x = new_x;
}

/* ─── Momentum simulation ─── */

static void process_momentum(void)
{
    long long now = now_ms();
    long long elapsed = now - momentum_start_ms;
    long long expected_steps = elapsed / cfg_momentum_interval_ms;

    long long steps = expected_steps - momentum_last_step;

    if (steps <= 0)
        return;

    for (long long s = 0; s < steps; s++) {
        momentum_vx *= cfg_momentum_decay;
        momentum_vy *= cfg_momentum_decay;

        float speed = sqrtf(momentum_vx * momentum_vx +
                            momentum_vy * momentum_vy);

        if (speed < cfg_momentum_min_velocity) {
            /* Momentum exhausted */
            cancel_scroll_state("[IDLE] Momentum decayed");
            return;
        }

        /* Inject decaying move */
        int mvx = (int)momentum_vx;
        int mvy = (int)momentum_vy;

        if (mvx == 0 && mvy == 0) {
            /* Too small to inject, but still has velocity */
            continue;
        }

        touch_inject_move(mvx, mvy);
        touch_injected = 1;
    }

    momentum_last_step = expected_steps;
}

/* ─── Touch event processing ─── */

static void process_touch_event(struct input_event *ev)
{
    /* Typing suppression */
    if (is_typing_active()) {
        /* Suppress gesture activation */
        if (state == STATE_IDLE || state == STATE_ONE_FINGER_PENDING)
            return;

        /* If scroll active, end it immediately on keyboard activity. */
        if (state == STATE_SCROLL_ACTIVE || state == STATE_MOMENTUM) {
            cancel_scroll_state("[SUPPRESS] Scroll cancelled by typing");
            return;
        }
    }

    switch (ev->code) {

    case ABS_MT_SLOT:
        if (!slot_is_valid(ev->value)) {
            fprintf(stderr, "Ignoring invalid touch slot %d\n", ev->value);
            current_slot = -1;
            break;
        }
        current_slot = ev->value;
        break;

    case ABS_MT_TRACKING_ID:
        if (!slot_is_valid(current_slot))
            break;

        if (ev->value < 0) {
            /* Finger lifted */
            if (current_slot == 0) {
                exit_scroll_active();
            }
            fingers[current_slot].active = 0;
        } else {
            /* Finger touched */
            if (state == STATE_MOMENTUM) {
                /* New touch during momentum: cancel momentum */
                cancel_scroll_state("[MOMENTUM] Cancelled by new touch");
            }

            fingers[current_slot].active = 1;
            fingers[current_slot].x = 0;
            fingers[current_slot].y = 0;
            fingers[current_slot].start_x = 0;
            fingers[current_slot].start_y = 0;

            /* Only track finger 0 for scrolling */
            if (current_slot == 0 && state == STATE_IDLE) {
                state = STATE_ONE_FINGER_PENDING;
                gesture_start_ms = now_ms();
            }
        }
        break;

    case ABS_MT_POSITION_X:
        if (!slot_is_valid(current_slot))
            break;

        fingers[current_slot].x = ev->value;
        if (fingers[current_slot].active &&
            fingers[current_slot].start_x == 0)
            fingers[current_slot].start_x = ev->value;
        break;

    case ABS_MT_POSITION_Y:
        if (!slot_is_valid(current_slot))
            break;

        fingers[current_slot].y = ev->value;
        if (fingers[current_slot].active &&
            fingers[current_slot].start_y == 0)
            fingers[current_slot].start_y = ev->value;
        break;
    }

    /* Process scroll for finger 0 on position update */
    if (state == STATE_ONE_FINGER_PENDING ||
        state == STATE_SCROLL_ACTIVE) {
        if (fingers[0].active &&
            fingers[0].start_x != 0 &&
            fingers[0].start_y != 0) {
            process_scroll_motion(fingers[0].y, fingers[0].x);
        }
    }
}

/* ─── Main loop ─── */

static void event_loop(struct libevdev *touch_dev, int touch_fd_raw)
{
    struct pollfd fds[2];
    int nfds = 1;

    fds[0].fd = touch_fd_raw;
    fds[0].events = POLLIN;

    if (keyboard_fd >= 0) {
        fds[1].fd = keyboard_fd;
        fds[1].events = POLLIN;
        nfds = 2;
    }

    while (running) {
        int poll_timeout = cfg_momentum_interval_ms < 8 ?
                           cfg_momentum_interval_ms : 8;
        int pret = poll(fds, nfds, poll_timeout);
        int poll_errno = errno;

        if (pret < 0) {
            if (poll_errno == EINTR) {
                if (reload_requested) {
                    reload_requested = 0;
                    reload_runtime_config();
                }
                continue;
            }

            errno = poll_errno;
            perror("poll");
            break;
        }

        if (reload_requested) {
            reload_requested = 0;
            reload_runtime_config();
        }

        /* Process keyboard events first (typing suppression) */
        if (nfds > 1 && (fds[1].revents & POLLIN)) {
            process_keyboard_events();
            if (is_typing_active() &&
                (state == STATE_SCROLL_ACTIVE || state == STATE_MOMENTUM)) {
                cancel_scroll_state("[SUPPRESS] Scroll cancelled by typing");
            }
        }

        /* Process touch events */
        if (fds[0].revents & POLLIN) {
            while (1) {
                struct input_event ev;
                int rc = libevdev_next_event(touch_dev,
                                             LIBEVDEV_READ_FLAG_NORMAL, &ev);
                if (rc == -EAGAIN)
                    break;
                if (rc != LIBEVDEV_READ_STATUS_SUCCESS)
                    continue;

                /* We only care about ABS events from touch device */
                if (ev.type == EV_ABS) {
                    process_touch_event(&ev);
                }
            }
        }

        /* Handle momentum state on timeout */
        if (state == STATE_MOMENTUM) {
            process_momentum();
        }

        if (state == STATE_ONE_FINGER_PENDING) {
            long long elapsed = now_ms() - gesture_start_ms;

            if (!fingers[0].active || elapsed > cfg_gesture_timeout_ms) {
                state = STATE_IDLE;
                reset_all_fingers();
                clear_velocity();
            }
        }
    }

    if (!shutdown_requested)
        end_injected_touch();
}

/* ─── Entry point ─── */

static void handle_signal(int signum)
{
    if (signum == SIGHUP) {
        reload_requested = 1;
    } else {
        shutdown_requested = 1;
        running = 0;
    }
}

static int setup_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0) {
        perror("sigaction SIGINT");
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction SIGTERM");
        return -1;
    }
    if (sigaction(SIGHUP, &action, NULL) < 0) {
        perror("sigaction SIGHUP");
        return -1;
    }

    return 0;
}

static int startup_should_stop(void)
{
    if (!running) {
        remove_runtime_files();
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct libevdev *touch_dev = NULL;
    const char *env_touch_device;
    const char *env_keyboard_device;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc > 1) {
        if (strcmp(argv[1], "reload") == 0)
            return send_reload_signal();
        if (strcmp(argv[1], "stop") == 0)
            return send_stop_signal();

        fprintf(stderr, "Usage: %s [reload|stop]\n", argv[0]);
        return 2;
    }

    if (setup_signal_handlers() < 0)
        return 1;

    env_touch_device = getenv("KEY2GESTURED_TOUCH_DEVICE");
    if (env_touch_device && env_touch_device[0] != '\0') {
        touch_device_path = env_touch_device;
        touch_device_path_overridden = 1;
    }

    env_keyboard_device = getenv("KEY2GESTURED_KEYBOARD_DEVICE");
    if (env_keyboard_device && env_keyboard_device[0] != '\0') {
        keyboard_device_path = env_keyboard_device;
        keyboard_device_path_overridden = 1;
    }

    load_env_int("KEY2GESTURED_SCAN_INPUTS", &scan_input_devices, 0, 1);

    load_runtime_config_from_env();
    load_runtime_config_file("Loaded");

    printf("key2gestured v0.2 — Touch injection scrolling daemon\n");
    printf("====================================================\n\n");

    unlink(READY_PATH);

    if (startup_should_stop())
        return 0;

    if (write_pid_file() < 0)
        return 1;

    if (startup_should_stop())
        return 0;

    /* Open touch device */
    int fd;

    if (touch_device_path_overridden) {
        fd = open_input_path(touch_device_path);
    } else {
        fd = open_input_path_if_named(DEFAULT_TOUCH_DEVICE,
                                      TOUCH_DEVICE_NAME,
                                      &touch_device_path);
        if (fd < 0) {
            printf("Default touch path %s not usable; trying fallback\n",
                   DEFAULT_TOUCH_DEVICE);
            fd = open_input_path_if_named(FALLBACK_TOUCH_DEVICE,
                                          TOUCH_DEVICE_NAME,
                                          &touch_device_path);
        }
        if (fd < 0 && scan_input_devices) {
            printf("Scanning input devices for touch source\n");
            fd = open_named_input_device(TOUCH_DEVICE_NAME,
                                         &touch_device_path);
        }
        if (fd < 0)
            fd = open_input_path(touch_device_path);
    }

    if (startup_should_stop())
        return 0;

    if (fd < 0) {
        perror("open touch device");
        remove_runtime_files();
        return 1;
    }

    if (libevdev_new_from_fd(fd, &touch_dev) < 0) {
        fprintf(stderr, "Failed to init libevdev for touch\n");
        close(fd);
        remove_runtime_files();
        return 1;
    }

    if (startup_should_stop())
        return 0;

    printf("Touch device: %s\n", libevdev_get_name(touch_dev));
    printf("  Path: %s\n", touch_device_path);

    /* Grab the touch device exclusively */
    if (libevdev_grab(touch_dev, LIBEVDEV_GRAB) < 0) {
        fprintf(stderr, "Failed to grab touch device\n");
    } else {
        printf("  Grab: OK (exclusive access)\n");
    }

    if (startup_should_stop())
        return 0;

    /* Open keyboard for typing suppression */
    printf("\nKeyboard:\n");
    setup_keyboard();

    if (startup_should_stop())
        return 0;

    /* Setup uinput touch injection */
    printf("\nTouch injection:\n");
    touch_fd = setup_uinput_touch();
    if (touch_fd < 0) {
        fprintf(stderr, "Failed to create uinput touch device\n");
        libevdev_grab(touch_dev, LIBEVDEV_UNGRAB);
        libevdev_free(touch_dev);
        close(fd);
        remove_runtime_files();
        return 1;
    }

    if (startup_should_stop())
        return 0;

    printf("\nState machine:\n");
    printf("  Threshold: %d units\n", cfg_scroll_threshold);
    printf("  Horizontal threshold: %d units\n",
           cfg_horizontal_scroll_threshold);
    printf("  Typing cooldown: %d ms\n", cfg_typing_cooldown_ms);
    printf("  Gesture timeout: %d ms\n", cfg_gesture_timeout_ms);
    printf("  Momentum decay: %.2f\n", cfg_momentum_decay);
    printf("  Momentum min velocity: %.2f\n", cfg_momentum_min_velocity);
    printf("  Momentum interval: %d ms\n", cfg_momentum_interval_ms);
    printf("  Max delta per event: %d units\n", cfg_max_delta_per_event);
    printf("\nReady. Touch the keyboard touch surface to scroll.\n");
    printf("------------------------------------------\n");

    if (write_ready_file() < 0) {
        remove_runtime_files();
        return 1;
    }

    event_loop(touch_dev, fd);

    if (shutdown_requested) {
        remove_runtime_files();
        return 0;
    }

    /* Cleanup */
    touch_close();
    if (keyboard_dev) {
        libevdev_free(keyboard_dev);
        close(keyboard_fd);
    }
    libevdev_grab(touch_dev, LIBEVDEV_UNGRAB);
    libevdev_free(touch_dev);
    close(fd);
    remove_runtime_files();

    return 0;
}
