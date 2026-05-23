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

#include <linux/uinput.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include "uinput.h"

/*
 * Virtual touchscreen device setup.
 *
 * Creates a uinput device with INPUT_PROP_DIRECT (touchscreen),
 * EV_ABS (MT position tracking), and BTN_TOUCH.
 *
 * Physics-based coordinate parameters.
 *
 * touch_keypad physical specs (from libinput / evtest):
 *   Physical size: 51mm × 25mm
 *   Resolution:    21 units/mm
 *   ABS_X range:   0..1080 (51mm × 21 ≈ 1071)
 *   ABS_Y range:   0..525  (25mm × 21 = 525)
 *
 * The 25mm height is very short — user's full swipe covers only
 * 525 units. SCROLL_SCALE_Y amplifies this to produce natural
 * touch scrolling on the compositor.
 */

#define ABS_X_MAX 2000
#define ABS_Y_MAX 3000

/* Touch injection starts at virtual device center */
#define TOUCH_CENTER_X (ABS_X_MAX / 2)
#define TOUCH_CENTER_Y (ABS_Y_MAX / 2)

/*
 * Scale keyboard touch deltas.
 *
 * Real Y range:       0..525  (25mm @ 21 units/mm)
 * Virtual Y range:    0..3000
 * With SCROLL_SCALE_Y = 3:
 *   Full swipe: 525 × 3 = 1575 units
 *   Range utilization: 1575/3000 ≈ 53% per direction
 *   From center (1500): 1500 ± 1575 → clamped at [0..3000]
 *   ≈ 97% total range utilization
 *
 * X axis: 51mm width is generous, no amplification needed.
 */
#define SCROLL_SCALE_X 1
#define SCROLL_SCALE_Y 3

static int uinput_fd = -1;
static int touch_active = 0;
static int touch_x = TOUCH_CENTER_X;
static int touch_y = TOUCH_CENTER_Y;
static int tracking_id = 0;

static void emit(int fd, int type, int code, int val)
{
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = type;
    ie.code = code;
    ie.value = val;
    write(fd, &ie, sizeof(ie));
}

static void emit_sync(int fd)
{
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

int setup_uinput_touch(void)
{
    struct uinput_user_dev uud;
    int fd;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    /* EV_KEY: BTN_TOUCH */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
        perror("UI_SET_EVBIT EV_KEY");
    if (ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0)
        perror("UI_SET_KEYBIT BTN_TOUCH");

    /* EV_ABS: MT tracking */
    if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
        perror("UI_SET_EVBIT EV_ABS");

    /* ABS_MT_SLOT */
    if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT) < 0)
        perror("UI_SET_ABSBIT ABS_MT_SLOT");

    /* ABS_MT_TRACKING_ID */
    if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) < 0)
        perror("UI_SET_ABSBIT ABS_MT_TRACKING_ID");

    /* ABS_MT_POSITION_X */
    if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) < 0)
        perror("UI_SET_ABSBIT ABS_MT_POSITION_X");

    /* ABS_MT_POSITION_Y */
    if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) < 0)
        perror("UI_SET_ABSBIT ABS_MT_POSITION_Y");

    /* INPUT_PROP_DIRECT: hint to compositor this is a touchscreen */
    if (ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0)
        perror("UI_SET_PROPBIT INPUT_PROP_DIRECT");

    memset(&uud, 0, sizeof(uud));
    snprintf(uud.name, UINPUT_MAX_NAME_SIZE, "key2gestured-touch");

    uud.id.bustype = BUS_USB;
    uud.id.vendor  = 0x1234;
    uud.id.product = 0x5679;
    uud.id.version = 1;

    /* Set ABS ranges */
    uud.absmin[ABS_MT_SLOT]        = 0;
    uud.absmax[ABS_MT_SLOT]        = 9;
    uud.absmin[ABS_MT_TRACKING_ID] = 0;
    uud.absmax[ABS_MT_TRACKING_ID] = 0xffff;
    uud.absmin[ABS_MT_POSITION_X]  = 0;
    uud.absmax[ABS_MT_POSITION_X]  = ABS_X_MAX;
    uud.absmin[ABS_MT_POSITION_Y]  = 0;
    uud.absmax[ABS_MT_POSITION_Y]  = ABS_Y_MAX;

    if (write(fd, &uud, sizeof(uud)) < 0) {
        perror("write uinput_user_dev");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    /* Give udev/libinput time to see the new device */
    sleep(1);

    printf("  Device: key2gestured-touch\n");
    printf("  ABS range: X[0-%d] Y[0-%d]\n", ABS_X_MAX, ABS_Y_MAX);
    printf("  Touch center: (%d, %d)\n", TOUCH_CENTER_X, TOUCH_CENTER_Y);
    printf("  Scroll scale: X=%d Y=%d\n", SCROLL_SCALE_X, SCROLL_SCALE_Y);

    uinput_fd = fd;
    return fd;
}

void touch_inject_down(void)
{
    if (touch_active)
        return;

    touch_x = TOUCH_CENTER_X;
    touch_y = TOUCH_CENTER_Y;
    tracking_id++;

    /* Slot 0 */
    emit(uinput_fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, tracking_id);
    emit(uinput_fd, EV_ABS, ABS_MT_POSITION_X, touch_x);
    emit(uinput_fd, EV_ABS, ABS_MT_POSITION_Y, touch_y);
    emit(uinput_fd, EV_KEY, BTN_TOUCH, 1);
    emit_sync(uinput_fd);

    touch_active = 1;
}

void touch_inject_move(int dx, int dy)
{
    if (!touch_active)
        return;

    touch_x += dx * SCROLL_SCALE_X;
    touch_y += dy * SCROLL_SCALE_Y;

    /* Clamp to valid range */
    if (touch_x < 0) touch_x = 0;
    if (touch_x > ABS_X_MAX) touch_x = ABS_X_MAX;
    if (touch_y < 0) touch_y = 0;
    if (touch_y > ABS_Y_MAX) touch_y = ABS_Y_MAX;

    emit(uinput_fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(uinput_fd, EV_ABS, ABS_MT_POSITION_X, touch_x);
    emit(uinput_fd, EV_ABS, ABS_MT_POSITION_Y, touch_y);
    emit_sync(uinput_fd);
}

void touch_inject_up(void)
{
    if (!touch_active)
        return;

    emit(uinput_fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(uinput_fd, EV_KEY, BTN_TOUCH, 0);
    emit_sync(uinput_fd);

    touch_active = 0;
}

void touch_close(void)
{
    if (touch_active)
        touch_inject_up();

    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        uinput_fd = -1;
    }
}
