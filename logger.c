/*
 * logger.c  —  Anwar Atawna
 * ---------------------------
 * Centralised logging service.  Every other process sends MSG_LOG here.
 *
 * Behaviour:
 *   - Non-blocking receive loop with 50 ms sleep when the queue is empty.
 *   - Writes timestamped entries to LOG_FILE (appended).
 *   - Prints colour-coded entries to the terminal.
 *   - On shutdown, drains all remaining messages before exiting.
 *
 * IPC decisions:
 *   MSG_LOG: this is the ONLY message type the logger receives.
 *   Pure consumer — never writes to SHM, never sends messages.
 *   Non-blocking receive + usleep(50 ms) is chosen over blocking
 *   receive so the shutdown flag in SHM is checked regularly.
 *
 * Colour coding:
 *   CTRL → cyan   EMRG → red     PED → yellow
 *   VDET → green  SAFE → magenta LIGHT → blue
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

/* ANSI colour codes */
#define C_RESET   "\x1b[0m"
#define C_RED     "\x1b[31m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_BLUE    "\x1b[34m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN    "\x1b[36m"
#define C_BOLD    "\x1b[1m"

static FILE                  *g_logfile = NULL;
static SharedData            *g_shm     = NULL;
static volatile sig_atomic_t  g_running  = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Map source id → colour string and short name                        */
/* ------------------------------------------------------------------ */
static const char *src_color(int src) {
    if (src == SRC_CONTROLLER)  return C_CYAN;
    if (src == SRC_EMERGENCY)   return C_RED   C_BOLD;
    if (src == SRC_PEDESTRIAN)  return C_YELLOW;
    if (src == SRC_VEHICLE_DET) return C_GREEN;
    if (src == SRC_SAFETY)      return C_MAGENTA C_BOLD;
    if (src >= SRC_TRAFFIC_LIGHT && src < SRC_TRAFFIC_LIGHT + NUM_DIRECTIONS)
        return C_BLUE;
    return C_RESET;
}

static const char *src_name(int src) {
    switch (src) {
    case SRC_CONTROLLER:  return "CTRL";
    case SRC_VEHICLE_DET: return "VDET";
    case SRC_PEDESTRIAN:  return "PED ";
    case SRC_EMERGENCY:   return "EMRG";
    case SRC_LOGGER:      return "LOG ";
    case SRC_SAFETY:      return "SAFE";
    default:
        if (src >= SRC_TRAFFIC_LIGHT &&
            src <  SRC_TRAFFIC_LIGHT + NUM_DIRECTIONS) {
            static char b[8];
            snprintf(b, sizeof(b), "LGT%c",
                     "NSEW"[src - SRC_TRAFFIC_LIGHT]);
            return b;
        }
        return "????";
    }
}

/* ------------------------------------------------------------------ */
/* Write one log entry to file and terminal                            */
/* ------------------------------------------------------------------ */
static void write_entry(const Message *m) {
    struct tm *tm_info = localtime(&m->timestamp);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);

    /* File — plain text */
    if (g_logfile) {
        fprintf(g_logfile, "[%s][%s] %s\n", ts, src_name(m->source), m->message);
        fflush(g_logfile);
    }

    /* Terminal — coloured */
    printf("%s[%s][%s]%s %s\n",
           src_color(m->source), ts, src_name(m->source), C_RESET,
           m->message);
    fflush(stdout);
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */
int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[LOG] ipc_attach failed\n"); return 1; }

    g_logfile = fopen(LOG_FILE, "a");
    if (!g_logfile)
        perror("[LOG] fopen — continuing without file logging");

    printf("[LOG] logger started (pid=%d)  log=%s\n", getpid(), LOG_FILE);
    fflush(stdout);

    if (g_logfile) {
        fprintf(g_logfile, "\n=== session start  ts=%ld ===\n", (long)time(NULL));
        fflush(g_logfile);
    }

    while (g_running) {
        Message m;
        int n = msg_recv(&m, MSG_LOG, 0);   /* non-blocking */

        if (n > 0) {
            write_entry(&m);
        } else {
            /* No message — check shutdown flag then sleep briefly */
            if (n < 0 && errno != ENOMSG) {
                if (errno == EIDRM || errno == EINVAL) break;  /* queue gone */
            }

            sem_lock();
            int sd = g_shm->shutdown;
            sem_unlock();

            if (sd) {
                /* Drain any remaining messages before exiting */
                while (msg_recv(&m, MSG_LOG, 0) > 0) write_entry(&m);
                break;
            }

            usleep(50 * 1000);   /* 50 ms — low CPU, still responsive */
        }
    }

    if (g_logfile) {
        fprintf(g_logfile, "=== session end    ts=%ld ===\n\n", (long)time(NULL));
        fclose(g_logfile);
    }

    printf("[LOG] logger shutting down\n");
    ipc_detach(g_shm);
    return 0;
}
