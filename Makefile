# Copyright (c) 2014 David Hubbard
#
# This program is free software: you can redistribute it and/or modify it under the terms of
# the GNU Affero General Public License version 3, as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU Affero General Public License version 3 for more details.
#
# You should have received a copy of the GNU Affero General Public License version 3 along with
# this program.  If not, see <http://www.gnu.org/licenses/>.

.PHONY: all build clean test

TARGET_BIN=sez

SRC+=main.cpp
SRC+=iface.cpp
SRC+=socket.cpp
SRC+=tuner.cpp
SRC+=mpgts.cpp
SRC+=mpgatsc.cpp

HDR+=iface.h
HDR+=socket.h
HDR+=tuner.h
HDR+=mpgts.h
HDR+=mpgatsc.h

LIBS+=-lpthread

CFLAGS+=-g -Wall -Wextra -Wundef -fno-exceptions -fno-rtti -pipe -Os
ifeq (1, 0)
CFLAGS+=-fomit-frame-pointer
endif
include build/init_cflags.mk

LDFLAGS+=$(LIBS)

all: build

test:
	$(MAKE) -C test test TOPDIR=$(TOPDIR)../

TOPDIR+=
include $(TOPDIR)build/build.mk
