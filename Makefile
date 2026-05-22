# =============================================================
# Makefile — Real-Time Traffic Light Control System
# Anwar Atawna — Linux IPC Project
# =============================================================
#
# Targets:
#   make            — build all binaries (except opengl_ui)
#   make opengl_ui  — build OpenGL visualizer (needs freeglut3-dev)
#   make all_with_gl— build everything including opengl_ui
#   make run        — build + launch interactive mode
#   make run-auto   — build + launch automatic demo mode
#   make clean      — remove compiled binaries
#   make ipc-clean  — forcibly remove stale IPC objects (after crash)
#   make kill       — kill all running system processes
#
# Compiler flags:
#   -Wall -Wextra    : strict warnings
#   -g               : debug symbols for gdb
#   -O0              : no optimization (better gdb experience)
# =============================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -g -O0
LDFLAGS =

# Shared IPC library (compiled into every binary)
IPC_OBJS = ipc.c

# Core binaries (no extra libs needed)
CORE_BINS = main \
            controller \
            traffic_light \
            vehicle_detector \
            pedestrian \
            emergency \
            logger \
            safety_monitor

.PHONY: all all_with_gl clean ipc-clean run run-auto kill help

# Default: build all core binaries
all: $(CORE_BINS)

all_with_gl: all opengl_ui

# ---------------------------------------------------------------
# Core processes
# ---------------------------------------------------------------
main: main.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

controller: controller.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

traffic_light: traffic_light.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

vehicle_detector: vehicle_detector.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

pedestrian: pedestrian.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

emergency: emergency.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

logger: logger.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

safety_monitor: safety_monitor.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---------------------------------------------------------------
# OpenGL visualizer (requires: sudo apt-get install freeglut3-dev)
# ---------------------------------------------------------------
opengl_ui: opengl_ui.c $(IPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lGL -lGLU -lglut -lm

# ---------------------------------------------------------------
# Run targets
# ---------------------------------------------------------------
run: all
	@echo "[RUN] Starting in interactive mode..."
	./main

run-auto: all
	@echo "[RUN] Starting in automatic demo mode..."
	./main --auto

# ---------------------------------------------------------------
# Maintenance targets
# ---------------------------------------------------------------

# Remove compiled binaries
clean:
	rm -f $(CORE_BINS) opengl_ui
	@echo "Cleaned."

# Remove stale IPC objects (use after a crash / hard kill)
ipc-clean:
	@ipcrm -M 0x1234 2>/dev/null && echo "SHM removed"   || echo "SHM not found"
	@ipcrm -S 0x5678 2>/dev/null && echo "SEM removed"   || echo "SEM not found"
	@ipcrm -Q 0x9ABC 2>/dev/null && echo "MSGQ removed"  || echo "MSGQ not found"

# Kill any orphaned processes from a previous run
kill:
	@pkill -f '\./main$$'          2>/dev/null && echo "killed main"           || true
	@pkill -f '\./controller'      2>/dev/null && echo "killed controller"     || true
	@pkill -f '\./traffic_light'   2>/dev/null && echo "killed traffic_light"  || true
	@pkill -f '\./vehicle_detector'2>/dev/null && echo "killed vehicle_det"    || true
	@pkill -f '\./pedestrian'      2>/dev/null && echo "killed pedestrian"     || true
	@pkill -f '\./emergency'       2>/dev/null && echo "killed emergency"      || true
	@pkill -f '\./logger'          2>/dev/null && echo "killed logger"         || true
	@pkill -f '\./safety_monitor'  2>/dev/null && echo "killed safety_monitor" || true
	@pkill -f '\./opengl_ui'       2>/dev/null && echo "killed opengl_ui"      || true

help:
	@echo ""
	@echo "Usage:"
	@echo "  make              — build all core binaries"
	@echo "  make run          — interactive mode (type ENTER for pedestrian)"
	@echo "  make run-auto     — automatic demo (random events)"
	@echo "  make opengl_ui    — build OpenGL visualizer"
	@echo "  make clean        — remove binaries"
	@echo "  make ipc-clean    — remove stale IPC objects after crash"
	@echo "  make kill         — kill orphaned processes"
	@echo ""
