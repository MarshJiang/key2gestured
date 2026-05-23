# key2gestured — Android-style gesture scrolling for BlackBerry KEY2
# Copyright (C) 2026 Marsh Jiang
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

CC=gcc
CFLAGS=-Wall -O2 $(shell pkg-config --cflags libevdev)
LIBS=$(shell pkg-config --libs libevdev) -lm

all:
	$(CC) $(CFLAGS) main.c uinput.c -o key2gestured $(LIBS)

clean:
	rm -f key2gestured

install:
	cp key2gestured /usr/local/bin/
	cp key2gestured.service /etc/systemd/system/
	systemctl daemon-reload

.PHONY: all clean install
