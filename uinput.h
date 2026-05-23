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
 * uinput touch injection interface
 *
 * Creates a uinput virtual touchscreen device (INPUT_PROP_DIRECT)
 * for injecting touch drag events that produce cursor-free scrolling
 * in Phoc/wlroots.
 *
 * Coordinate space: 0-2000 X, 0-3000 Y (phone-like aspect ratio)
 * Touch starts at screen center to avoid accidental UI activation.
 */

int setup_uinput_touch(void);

void touch_inject_down(void);
void touch_inject_move(int dx, int dy);
void touch_inject_up(void);
void touch_close(void);
