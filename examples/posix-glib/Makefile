PKG_CONFIG ?= pkg-config
ifeq ($(origin CC), default)
CC := cc
endif
CXX ?= c++
AR ?= ar
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUILD_DIR ?= build/posix_glib
OBJ_DIR := $(BUILD_DIR)/obj
EXE_NAME := libbounce_posix_glib_gtk3_example
EXE := $(BUILD_DIR)/$(EXE_NAME)
STATIC_LIB := $(BUILD_DIR)/libbounce-glib.a

CPPFLAGS ?= -I$(PROJECT_ROOT)/include -I$(PROJECT_ROOT) -DBOUNCE_POSIX_GLIB=1
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Werror -pedantic -pthread
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Werror -pedantic -pthread
LDFLAGS ?=
LDLIBS ?=
GLIB_CFLAGS := $(shell $(PKG_CONFIG) --cflags glib-2.0 gobject-2.0 gio-2.0)
GLIB_LIBS := $(shell $(PKG_CONFIG) --libs glib-2.0 gobject-2.0 gio-2.0)
GTK3_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
GTK3_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0)

LIBBOUNCE_SOURCES := \
	$(PROJECT_ROOT)/src/posix-glib/bounce_tls_posix_glib.c \
	$(PROJECT_ROOT)/src/posix-glib/bounce_posix_glib.c \
	$(PROJECT_ROOT)/src/posix-glib/bounce_posix_glib_timer.c
EXAMPLE_SOURCES := \
	$(PROJECT_ROOT)/examples/posix-glib/example_app.cpp \
	$(PROJECT_ROOT)/examples/posix-glib/main.cpp
LIBBOUNCE_OBJECTS := $(patsubst $(PROJECT_ROOT)/%.c,$(OBJ_DIR)/%.o,$(LIBBOUNCE_SOURCES))
EXAMPLE_OBJECTS := $(patsubst $(PROJECT_ROOT)/%.cpp,$(OBJ_DIR)/%.o,$(EXAMPLE_SOURCES))

.PHONY: all clean

all: $(EXE)

$(STATIC_LIB): $(LIBBOUNCE_OBJECTS) | $(BUILD_DIR)
	$(AR) rcs $@ $(LIBBOUNCE_OBJECTS)

$(EXE): $(STATIC_LIB) $(EXAMPLE_OBJECTS) | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $(EXAMPLE_OBJECTS) $(STATIC_LIB) $(LDLIBS) $(GTK3_LIBS)

$(OBJ_DIR)/%.o: $(PROJECT_ROOT)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(GLIB_CFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(PROJECT_ROOT)/%.cpp | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(GTK3_CFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
