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

all: build

TARGET_BIN+=
TARGET_LIB+=
build: $(patsubst %,subdir_%,$(SUBDIRS)) $(TARGET_BIN) $(TARGET_LIB)

clean:
	rm -rf $(TARGET_BIN) $(TARGET_LIB) .obj
ifneq ($(SUBDIRS),)
	for a in $(SUBDIRS); do $(MAKE) -C $$a clean TOPDIR=$(TOPDIR)../; done
endif

#
# a generic "compile .o from .cpp" that puts the .o in the .obj/ dir
#
OBJ_from_SRC = $(patsubst %.cpp,.obj/%.o,$(1))

OBJ := $(call OBJ_from_SRC,$(SRC))

ifneq ($(TOPDIR),)
CFLAGS+=-I$(TOPDIR)
else
CFLAGS+=-I.
endif
export CFLAGS

define OBJ_template
 $(call OBJ_from_SRC,$(1)): $(1) $(HDR)
	@mkdir -p .obj
	$(CXX) $$$$CFLAGS -o $$@ -c $(1)
endef

$(foreach src,$(SRC),$(eval $(call OBJ_template,$(src))))


#
# a generic "link $(TARGET_BIN) from $(OBJ)"
#
ifneq ($(TARGET_BIN),)
$(TARGET_BIN): $(OBJ) $(LIBS)
	$(CXX) $$CFLAGS -o $@ $(OBJ) $(LDFLAGS)
endif

#
# a generic "link $(TARGET_LIB) from $(OBJ)"
#
ifneq ($(TARGET_LIB),)
$(TARGET_LIB): $(OBJ) $(SUBDIRS)
	$(AR) rcs $@ $(OBJ)
endif

define SUBDIR_template
 subdir_$(1):
	$$(MAKE) -C $(1) TOPDIR=$$(TOPDIR)../
endef
$(foreach subdir,$(SUBDIRS),$(eval $(call SUBDIR_template,$(subdir))))
