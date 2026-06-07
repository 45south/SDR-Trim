# Makefile for SDR Trim + SDR Trim GUI
# Supports Linux, macOS, and Windows (MSYS2/MinGW64)
#
# Targets:
#   make              — build sdrtrim and sdrtrimgui (default on Windows)
#   make sdrtrim      — build CLI tool only
#   make sdrtrimgui   — build GUI only (Windows/MinGW only)
#   make install      — install sdrtrim to $(PREFIX)/bin (Linux/macOS only)
#   make clean        — remove build output

CC     = gcc
CFLAGS = -Wall -Wextra -O2
LIBS   = -lm

ifeq ($(OS),Windows_NT)

TARGET     = sdrtrim.exe
GUI_TARGET = sdrtrimgui.exe
GUI_RES    = sdrtrimgui.res
GUI_LIBS   = -lcomctl32 -lcomdlg32 -lshell32 -mwindows -municode

.PHONY: all sdrtrim sdrtrimgui clean

all: $(TARGET) $(GUI_TARGET)

sdrtrim: $(TARGET)

sdrtrimgui: $(GUI_TARGET)

$(TARGET): sdrtrim.c
	$(CC) $(CFLAGS) -o $@ sdrtrim.c $(LIBS)

$(GUI_RES): sdrtrimgui.rc sdrtrimgui.manifest
	windres sdrtrimgui.rc -O coff -o $(GUI_RES)

$(GUI_TARGET): sdrtrimgui.c $(GUI_RES)
	$(CC) $(CFLAGS) -o $@ sdrtrimgui.c $(GUI_RES) $(GUI_LIBS)

clean:
	rm -f $(TARGET) $(GUI_TARGET) $(GUI_RES)

else

TARGET  = sdrtrim
PREFIX ?= /usr/local

.PHONY: all sdrtrim install clean

all: $(TARGET)

sdrtrim: $(TARGET)

$(TARGET): sdrtrim.c
	$(CC) $(CFLAGS) -o $@ sdrtrim.c $(LIBS)

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

endif
