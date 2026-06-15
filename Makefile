# Makefile for SDR Trim (sdrtrim.exe)
# Build: make clean && make
#
# Requires MSYS2/MinGW-w64 on Windows
#
# Targets:
#   make        — build sdrtrim.exe
#   make clean  — remove build output

CC     = gcc

# -O3 enables auto-vectorisation of the DDC inner loop (SSE2/AVX)
# -msse2 ensures SSE2 on 32-bit builds; no-op on 64-bit
CFLAGS = -Wall -Wextra -O3 -msse2

LIBS   = -lcomctl32 -lcomdlg32 -lshell32 -mwindows -municode -lm

TARGET = sdrtrim.exe
RES    = sdrtrim.res

.PHONY: all clean

all: $(TARGET)

$(RES): sdrtrim.rc sdrtrim.manifest
	windres sdrtrim.rc -O coff -o $(RES)

$(TARGET): sdrtrim.c $(RES)
	$(CC) $(CFLAGS) -o $@ sdrtrim.c $(RES) $(LIBS)

clean:
	rm -f $(TARGET) $(RES)
