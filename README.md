# key2gestured

Android-style gesture scrolling daemon for the BlackBerry KEY2 keyboard touch
surface on Wayland mobile Linux (Droidian / Phosh / Phoc).

## Architecture

```
touch_keypad (/dev/input/event1, INPUT_PROP_DIRECT touchscreen)
    ↓ libevdev_grab (exclusive access)
Gesture state machine (IDLE → PENDING → SCROLL_ACTIVE → MOMENTUM → IDLE)
    ↓
uinput virtual touchscreen device (INPUT_PROP_DIRECT + EV_ABS + BTN_TOUCH)
    ↓ touch down → move → up drag injection
Phoc / wlroots (recognized as touch gesture → smooth scrolling)
    ↓
GTK / WebKit (natural scrolling, no cursor)
```

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| uinput EV_ABS touch injection | Touch devices never produce a cursor |
| INPUT_PROP_DIRECT | Hints to compositor this is a touchscreen, not a pointer |
| Fixed injection coordinates (screen center) | Avoids accidental UI clicks from coordinate mapping |
| Drag-only (no tap) | Drag = scroll in GTK/WebKit; tap = click |
| Typing suppression (100ms cooldown) | Prevents accidental scroll while typing on physical keyboard |
| Momentum / kinetic scrolling | Android-like fling behavior via velocity tracking + decay |

## State Machine

```
IDLE ──touch down──→ ONE_FINGER_PENDING
ONE_FINGER_PENDING ──delta > threshold──→ SCROLL_ACTIVE
ONE_FINGER_PENDING ──timeout/typing──→ IDLE
SCROLL_ACTIVE ──touch up, has velocity──→ MOMENTUM
SCROLL_ACTIVE ──touch up, no velocity──→ IDLE
SCROLL_ACTIVE ──typing detected──→ IDLE
MOMENTUM ──velocity decays──→ IDLE
MOMENTUM ──new touch──→ SCROLL_ACTIVE
```

## Build

### Dependencies

- `gcc`
- `libevdev-dev`
- `libc6-dev` (for `linux/uinput.h`)

### Compile

```sh
make
```

For Droidian KEY2 changes, build and test on the device that owns the real
input devices. A typical foreground test loop is:

```sh
sudo systemctl stop key2gestured
make clean && make
sudo ./key2gestured
```

To include `/etc/key2gestured/default.conf` in a foreground test:

```sh
sudo sh -c 'set -a; [ -f /etc/key2gestured/default.conf ] && . /etc/key2gestured/default.conf; exec ./key2gestured'
```

In another shell, watch service logs after installing:

```sh
journalctl -u key2gestured -f
```

### Install

```sh
sudo make install
sudo systemctl enable --now key2gestured
```

The systemd unit orders itself after `keyd.service` and
`systemd-udev-settle.service` so the auto-detected input devices are likely to
exist before the daemon starts. If keyd is not installed, key2gestured falls
back to the physical `stmpe_keypad` device for typing suppression.

## Configuration

Runtime configuration lives at `/etc/key2gestured/default.conf`. The file uses
systemd `EnvironmentFile` syntax:

```sh
KEY2GESTURED_SCROLL_THRESHOLD=18
KEY2GESTURED_HORIZONTAL_SCROLL_THRESHOLD=36
KEY2GESTURED_TYPING_COOLDOWN_MS=100
KEY2GESTURED_GESTURE_TIMEOUT_MS=800
KEY2GESTURED_MOMENTUM_DECAY=0.92
KEY2GESTURED_MOMENTUM_MIN_VELOCITY=0.5
KEY2GESTURED_MOMENTUM_INTERVAL_MS=16
KEY2GESTURED_MAX_DELTA_PER_EVENT=50
KEY2GESTURED_SCROLL_SCALE_X=1
KEY2GESTURED_SCROLL_SCALE_Y=3
```

`make install` installs [`key2gestured.default.conf`](key2gestured.default.conf)
to `/etc/key2gestured/default.conf` only if the target file does not already
exist.

Edit [`uinput.c`](uinput.c) only to change the virtual touch coordinate range:

- `ABS_X_MAX` / `ABS_Y_MAX` — virtual touch coordinate range

By default, key2gestured scans `/dev/input/event*` by libevdev device name:

- touch source: `touch_keypad`
- typing suppression: `keyd virtual keyboard`, falling back to `stmpe_keypad`

Device paths can also be overridden in `/etc/key2gestured/default.conf`:

```sh
KEY2GESTURED_TOUCH_DEVICE=/dev/input/by-path/platform-c175000.i2c-event
KEY2GESTURED_KEYBOARD_DEVICE=/dev/input/event7
```

## Device Paths

| Device | Path | Role |
|--------|------|------|
| `touch_keypad` | auto-detected by name, fallback `/dev/input/by-path/platform-c175000.i2c-event` | Gesture source |
| `keyd virtual keyboard` | auto-detected by name | Typing suppression after keyd remapping |
| `stmpe_keypad` | auto-detected by name, fallback `/dev/input/event2` | Typing suppression without keyd |

## Files

| File | Purpose |
|------|---------|
| [`main.c`](main.c) | Event loop, state machine, gesture logic, momentum |
| [`uinput.c`](uinput.c) | Virtual touch device creation and event injection |
| [`uinput.h`](uinput.h) | Touch injection API |
| [`state.h`](state.h) | Types, enums, and tunable parameters |
| [`Makefile`](Makefile) | Build system |
| [`key2gestured.service`](key2gestured.service) | systemd service unit |
| [`key2gestured.default.conf`](key2gestured.default.conf) | Example runtime configuration |

## What This Is NOT

- ❌ Mouse emulator
- ❌ Wheel emulator
- ❌ Desktop touchpad daemon
- ❌ Key remapper
- ❌ X11 tool

## What This IS

- ✅ Android-style gesture scrolling
- ✅ Cursor-free touch injection
- ✅ Mobile-first Wayland UX
- ✅ Typing-aware suppression
- ✅ Momentum / kinetic scrolling
- ✅ Low-latency event processing

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
