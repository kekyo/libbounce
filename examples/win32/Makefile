ifeq ($(origin CC), default)
CC := x86_64-w64-mingw32-gcc-win32
endif
CXX := $(subst gcc,g++,$(CC))
AR ?= ar
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC_BASENAME := $(notdir $(CC))
BUILD_DIR ?= build/$(CC_BASENAME)
OBJ_DIR := $(BUILD_DIR)/obj
EXE_NAME := libbounce_win32_example.exe
EXE := $(BUILD_DIR)/$(EXE_NAME)
STATIC_LIB := $(BUILD_DIR)/libbounce.a

CPPFLAGS ?= -I$(PROJECT_ROOT)/include -I$(PROJECT_ROOT)
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Werror -pedantic
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Werror -pedantic -fcoroutines
LDFLAGS ?=
LDLIBS ?=
PLATFORM_LDLIBS := -Wl,-Bstatic -Wl,-Bdynamic -static-libgcc -static-libstdc++ -luser32

LIBBOUNCE_SOURCES := \
	$(PROJECT_ROOT)/src/win32/bounce_tls_win32.c \
	$(PROJECT_ROOT)/src/win32/bounce_win32.c \
	$(PROJECT_ROOT)/src/win32/bounce_win32_timer.c
EXAMPLE_SOURCES := \
	$(PROJECT_ROOT)/examples/win32/example_app.cpp \
	$(PROJECT_ROOT)/examples/win32/main.cpp
LIBBOUNCE_OBJECTS := $(patsubst $(PROJECT_ROOT)/%.c,$(OBJ_DIR)/%.o,$(LIBBOUNCE_SOURCES))
EXAMPLE_OBJECTS := $(patsubst $(PROJECT_ROOT)/%.cpp,$(OBJ_DIR)/%.o,$(EXAMPLE_SOURCES))

.PHONY: all clean

all: $(EXE)

$(STATIC_LIB): $(LIBBOUNCE_OBJECTS) | $(BUILD_DIR)
	$(AR) rcs $@ $(LIBBOUNCE_OBJECTS)

$(EXE): $(STATIC_LIB) $(EXAMPLE_OBJECTS) | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -mwindows -o $@ $(EXAMPLE_OBJECTS) $(STATIC_LIB) $(LDLIBS) $(PLATFORM_LDLIBS)

$(OBJ_DIR)/%.o: $(PROJECT_ROOT)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(PROJECT_ROOT)/%.cpp | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
