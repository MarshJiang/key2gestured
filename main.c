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

#include <linux/input.h>
#include <libevdev/libevdev.h>

#include "state.h"
#include "uinput.h"

/*
 * Device paths
 */
#define TOUCH_DEVICE   "/dev/input/by-path/platform-c175000.i2c-event"
#define KEYBOARD_DEVICE "/dev/input/event2"

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

/* ─── Velocity tracking ─── */

static void record_velocity(int dx, int dy)
{
    velocity_samples_x[velocity_idx] = (float)dx;
    velocity_samples_y[velocity_idx] = (float)dy;
    velocity_idx = (velocity_idx + 1) % VELOCITY_SAMPLES;
}

static float average_velocity_x(void)
{
    float sum = 0.0f;
    for (int i = 0; i < VELOCITY_SAMPLES; i++)
        sum += velocity_samples_x[i];
    return sum / VELOCITY_SAMPLES;
}

static float average_velocity_y(void)
{
    float sum = 0.0f;
    for (int i = 0; i < VELOCITY_SAMPLES; i++)
        sum += velocity_samples_y[i];
    return sum / VELOCITY_SAMPLES;
}

static void clear_velocity(void)
{
    velocity_idx = 0;
    for (int i = 0; i < VELOCITY_SAMPLES; i++) {
        velocity_samples_x[i] = 0.0f;
        velocity_samples_y[i] = 0.0f;
    }
}

/* ─── Typing suppression ─── */

static int is_typing_active(void)
{
    long long now = now_ms();
    return (now - last_key_time_ms) < TYPING_COOLDOWN_MS;
}

static int setup_keyboard(void)
{
    struct libevdev *dev = NULL;
    int fd;

    fd = open(KEYBOARD_DEVICE, O_RDONLY | O_NONBLOCK);
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

static void enter_scroll_active(int dy, int dx)
{
    printf("[SCROLL] Enter SCROLL_ACTIVE (dy=%d, dx=%d)\n", dy, dx);

    state = STATE_SCROLL_ACTIVE;
    total_dy = 0;
    total_dx = 0;
    prev_y = 0;
    prev_x = 0;
    touch_injected = 0;

    /* Inject touch down at screen center */
    touch_inject_down();
}

static void exit_scroll_active(void)
{
    if (state == STATE_SCROLL_ACTIVE) {
        /* Capture final velocity for momentum */
        momentum_vx = average_velocity_x();
        momentum_vy = average_velocity_y();
        float speed = sqrtf(momentum_vx * momentum_vx +
                            momentum_vy * momentum_vy);

        if (speed > MOMENTUM_MIN_VELOCITY) {
            printf("[MOMENTUM] Starting (vx=%.1f, vy=%.1f, speed=%.1f)\n",
                   momentum_vx, momentum_vy, speed);
            state = STATE_MOMENTUM;
            momentum_start_ms = now_ms();
            momentum_last_step = 0;
        } else {
            /* No significant velocity, end touch */
            if (touch_injected) {
                touch_inject_up();
                touch_injected = 0;
            }
            state = STATE_IDLE;
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

        if (abs_dy > SCROLL_THRESHOLD || abs_dx > SCROLL_THRESHOLD) {
            enter_scroll_active(dy, dx);
        } else {
            /* Check gesture timeout */
            long long elapsed = now_ms() - gesture_start_ms;
            if (elapsed > GESTURE_TIMEOUT_MS) {
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

    /* Cap to prevent massive jumps */
    if (move_dy > MAX_DELTA_PER_EVENT)
        move_dy = MAX_DELTA_PER_EVENT;
    else if (move_dy < -MAX_DELTA_PER_EVENT)
        move_dy = -MAX_DELTA_PER_EVENT;

    if (move_dx > MAX_DELTA_PER_EVENT)
        move_dx = MAX_DELTA_PER_EVENT;
    else if (move_dx < -MAX_DELTA_PER_EVENT)
        move_dx = -MAX_DELTA_PER_EVENT;

    /* Record velocity for momentum */
    record_velocity(move_dx, move_dy);

    /* Inject the touch move */
    touch_inject_move(move_dx, move_dy);
    touch_injected = 1;

    total_dy += move_dy;
    total_dx += move_dx;
    prev_y = new_y;
    prev_x = new_x;
}

/* ─── Momentum simulation ─── */

static void process_momentum(void)
{
    long long now = now_ms();
    long long elapsed = now - momentum_start_ms;
    long long expected_steps = elapsed / MOMENTUM_INTERVAL_MS;

    long long steps = expected_steps - momentum_last_step;

    if (steps <= 0)
        return;

    for (long long s = 0; s < steps; s++) {
        momentum_vx *= MOMENTUM_DECAY;
        momentum_vy *= MOMENTUM_DECAY;

        float speed = sqrtf(momentum_vx * momentum_vx +
                            momentum_vy * momentum_vy);

        if (speed < MOMENTUM_MIN_VELOCITY) {
            /* Momentum exhausted */
            if (touch_injected) {
                touch_inject_up();
                touch_injected = 0;
            }
            state = STATE_IDLE;
            reset_all_fingers();
            clear_velocity();
            printf("[IDLE] Momentum decayed\n");
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

        /* If scroll active, end it immediately on keyboard activity */
        if (state == STATE_SCROLL_ACTIVE) {
            if (touch_injected) {
                touch_inject_up();
                touch_injected = 0;
            }
            state = STATE_IDLE;
            reset_all_fingers();
            clear_velocity();
            printf("[SUPPRESS] Scroll cancelled by typing\n");
            return;
        }
    }

    switch (ev->code) {

    case ABS_MT_SLOT:
        current_slot = ev->value;
        break;

    case ABS_MT_TRACKING_ID:
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
                if (touch_injected) {
                    touch_inject_up();
                    touch_injected = 0;
                }
                state = STATE_IDLE;
                reset_all_fingers();
                clear_velocity();
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
        fingers[current_slot].x = ev->value;
        if (fingers[current_slot].active &&
            fingers[current_slot].start_x == 0)
            fingers[current_slot].start_x = ev->value;
        break;

    case ABS_MT_POSITION_Y:
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

    while (1) {
        int pret = poll(fds, nfds, 8); /* ~8ms timeout for momentum */

        if (pret < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* Process keyboard events first (typing suppression) */
        if (nfds > 1 && (fds[1].revents & POLLIN)) {
            process_keyboard_events();
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

        /* End gesture if finger lifted and we're still pending */
        if (state == STATE_ONE_FINGER_PENDING &&
            !fingers[0].active) {
            state = STATE_IDLE;
        }
    }
}

/* ─── Entry point ─── */

int main(void)
{
    struct libevdev *touch_dev = NULL;

    printf("key2gestured v0.2 — Touch injection scrolling daemon\n");
    printf("====================================================\n\n");

    /* Open touch device */
    int fd = open(TOUCH_DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open touch device");
        return 1;
    }

    if (libevdev_new_from_fd(fd, &touch_dev) < 0) {
        fprintf(stderr, "Failed to init libevdev for touch\n");
        close(fd);
        return 1;
    }

    printf("Touch device: %s\n", libevdev_get_name(touch_dev));
    printf("  Path: %s\n", TOUCH_DEVICE);

    /* Grab the touch device exclusively */
    if (libevdev_grab(touch_dev, LIBEVDEV_GRAB) < 0) {
        fprintf(stderr, "Failed to grab touch device\n");
    } else {
        printf("  Grab: OK (exclusive access)\n");
    }

    /* Open keyboard for typing suppression */
    printf("\nKeyboard:\n");
    setup_keyboard();

    /* Setup uinput touch injection */
    printf("\nTouch injection:\n");
    touch_fd = setup_uinput_touch();
    if (touch_fd < 0) {
        fprintf(stderr, "Failed to create uinput touch device\n");
        libevdev_grab(touch_dev, LIBEVDEV_UNGRAB);
        libevdev_free(touch_dev);
        close(fd);
        return 1;
    }

    printf("\nState machine:\n");
    printf("  Threshold: %d units\n", SCROLL_THRESHOLD);
    printf("  Typing cooldown: %d ms\n", TYPING_COOLDOWN_MS);
    printf("  Gesture timeout: %d ms\n", GESTURE_TIMEOUT_MS);
    printf("  Momentum decay: %.2f\n", MOMENTUM_DECAY);
    printf("\nReady. Touch the keyboard touch surface to scroll.\n");
    printf("------------------------------------------\n");

    event_loop(touch_dev, fd);

    /* Cleanup */
    touch_close();
    if (keyboard_dev) {
        libevdev_free(keyboard_dev);
        close(keyboard_fd);
    }
    libevdev_grab(touch_dev, LIBEVDEV_UNGRAB);
    libevdev_free(touch_dev);
    close(fd);

    return 0;
}
