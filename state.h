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

#pragma once

/*
 * key2gestured state machine and types
 *
 * State transitions:
 *   IDLE → ONE_FINGER_PENDING (on touch down)
 *   ONE_FINGER_PENDING → SCROLL_ACTIVE (delta > threshold)
 *   SCROLL_ACTIVE → MOMENTUM (on touch up, if velocity > 0)
 *   SCROLL_ACTIVE → IDLE (on touch up, no velocity)
 *   MOMENTUM → IDLE (velocity decays to 0)
 */

#define MAX_FINGERS 10

/* Typing suppression window in ms */
#define TYPING_COOLDOWN_MS 100

/* Scroll activation threshold in device units */
#define SCROLL_THRESHOLD 18

/* Gesture timeout: must complete within 800ms */
#define GESTURE_TIMEOUT_MS 800

/* Momentum decay per iteration */
#define MOMENTUM_DECAY 0.92f

/* Minimum momentum velocity to continue */
#define MOMENTUM_MIN_VELOCITY 0.5f

/* Momentum injection interval in ms */
#define MOMENTUM_INTERVAL_MS 16

/* Maximum touch move delta per event to cap */
#define MAX_DELTA_PER_EVENT 50

typedef enum {
    GESTURE_NONE,
    GESTURE_UD, /* Up → Down */
    GESTURE_DU, /* Down → Up */
    GESTURE_LR, /* Left → Right */
    GESTURE_RL, /* Right → Left */
} GestureType;

typedef enum {
    STATE_IDLE,
    STATE_ONE_FINGER_PENDING,
    STATE_SCROLL_ACTIVE,
    STATE_MOMENTUM,
} AppState;

typedef struct {
    int active;
    int x;
    int y;
    int start_x;
    int start_y;
    int slot;
} Finger;

/* Global state */
extern Finger fingers[MAX_FINGERS];
extern AppState state;
extern int touch_fd;

/* Time tracking for typing suppression */
extern long long last_key_time_ms;
