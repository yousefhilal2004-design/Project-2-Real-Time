/*
 * safety_monitor.c  —  Anwar Atawna
 * ------------------------------------
 * Independent safety watchdog process.
 *
 * Safety rules enforced (every 500 ms):
 *
 *   Rule 1 — No conflicting green lights:
 *     N-S and E-W directions must never both be GREEN simultaneously.
 *     This is the primary intersection safety rule.
 *
 *   Rule 2 — No pedestrian crossing during active vehicle green:
 *     pedestrian_active must not be 1 while any vehicle direction is GREEN.
 *
 *   Rule 3 — No GREEN→RED skip (must pass through YELLOW):
 *     Tracks the previous state of each light. If a direction goes
 *     directly from GREEN to RED, a violation is reported.
 *
 * IPC decisions:
 *   Read-only access to SHM (takes a snapshot under sem_lock).
 *   Writes safety_violation + safety_msg to SHM on violation.
 *   Sends MSG_LOG for every violation and periodic OK status.
 *   Never sends MSG_CMD — the controller retains sole authority.
 *
 * Periodic status: every 60 checks (30 s) sends a green "all OK" log
 * entry so there is always evidence the monitor is alive.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

static SharedData            *g_shm      = NULL;
static volatile sig_atomic_t  g_running  = 1;
static int                    g_prev[NUM_DIRECTIONS];
static int                    g_prev_left[NUM_DIRECTIONS];

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Report a safety violation to SHM + logger                          */
/* ------------------------------------------------------------------ */
static void report_violation(const char *msg) {
    sem_lock();
    g_shm->safety_violation = 1;
    strncpy(g_shm->safety_msg, msg, sizeof(g_shm->safety_msg) - 1);
    sem_unlock();

    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_LOG;
    m.source    = SRC_SAFETY;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "*** SAFETY VIOLATION: %s ***", msg);
    msg_send(&m);

    fprintf(stderr, "\x1b[1;31m[SAFE] VIOLATION: %s\x1b[0m\n", msg);
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Clear the violation flag once all rules pass                       */
/* ------------------------------------------------------------------ */
static void report_ok(void) {
    sem_lock();
    if (g_shm->safety_violation) {
        g_shm->safety_violation = 0;
        memset(g_shm->safety_msg, 0, sizeof(g_shm->safety_msg));
    }
    sem_unlock();
}

/* ------------------------------------------------------------------ */
/* Evaluate all safety rules against the current snapshot             */
/* ------------------------------------------------------------------ */
static void check_safety(const SharedData *s) {
    int ns      = (s->light[NORTH]      == GREEN || s->light[SOUTH]      == GREEN);
    int ew      = (s->light[EAST]       == GREEN || s->light[WEST]       == GREEN);
    int ns_left = (s->left_light[NORTH] == GREEN || s->left_light[SOUTH] == GREEN);
    int ew_left = (s->left_light[EAST]  == GREEN || s->left_light[WEST]  == GREEN);

    /* Rule 1a: conflicting straight greens */
    if (ns && ew) {
        report_violation("N-S and E-W both GREEN — conflicting straight lights!");
        return;
    }

    /* Rule 1b: conflicting left-turn arrows */
    if (ns_left && ew_left) {
        report_violation("N-S and E-W left-turn arrows both GREEN — conflict!");
        return;
    }

    /* Rule 1c: left-turn conflicts with opposing straight */
    if (ns_left && ew) {
        report_violation("N-S left-turn GREEN while E-W straight GREEN — conflict!");
        return;
    }
    if (ew_left && ns) {
        report_violation("E-W left-turn GREEN while N-S straight GREEN — conflict!");
        return;
    }

    /* Rule 2: pedestrian crossing while vehicles green */
    if (s->pedestrian_active && (ns || ew || ns_left || ew_left)) {
        report_violation("Pedestrian walk signal while vehicle lights GREEN!");
        return;
    }

    /* Rule 3a: straight signal GREEN→RED without YELLOW */
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        if (g_prev[d] == GREEN && s->light[d] == RED) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "%s straight went GREEN→RED without YELLOW!", dir_str(d));
            report_violation(buf);
            return;
        }
    }

    /* Rule 3b: left-turn arrow GREEN→RED without YELLOW */
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        if (g_prev_left[d] == GREEN && s->left_light[d] == RED) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "%s left-turn went GREEN→RED without YELLOW!", dir_str(d));
            report_violation(buf);
            return;
        }
    }

    report_ok();
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */
int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[SAFE] ipc_attach failed\n"); return 1; }

    printf("[SAFE] safety monitor started (pid=%d)\n", getpid());
    fflush(stdout);

    /* Announce startup */
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype = MSG_LOG; m.source = SRC_SAFETY; m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "Safety monitor started");
    msg_send(&m);

    /* Capture initial light states */
    sem_lock();
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        g_prev[d]      = g_shm->light[d];
        g_prev_left[d] = g_shm->left_light[d];
    }
    sem_unlock();

    int ticks = 0;

    while (g_running) {
        /* Take a snapshot under lock (minimise lock-hold time) */
        SharedData snap;
        sem_lock();
        snap = *g_shm;
        sem_unlock();

        if (snap.shutdown) break;

        check_safety(&snap);

        /* Periodic heartbeat log */
        ticks++;
        if (ticks % 60 == 0) {   /* 60 × 500 ms = 30 s */
            memset(&m, 0, sizeof(m));
            m.mtype = MSG_LOG; m.source = SRC_SAFETY; m.timestamp = time(NULL);
            snprintf(m.message, sizeof(m.message),
                     "Safety OK — phase=%s  N=%s S=%s E=%s W=%s",
                     phase_str(snap.current_phase),
                     light_str(snap.light[NORTH]),
                     light_str(snap.light[SOUTH]),
                     light_str(snap.light[EAST]),
                     light_str(snap.light[WEST]));
            msg_send(&m);
        }

        /* Save current states for next-cycle GREEN→RED check */
        for (int d = 0; d < NUM_DIRECTIONS; d++) {
            g_prev[d]      = snap.light[d];
            g_prev_left[d] = snap.left_light[d];
        }

        usleep(500 * 1000);   /* 500 ms check interval */
    }

    printf("[SAFE] safety monitor shutting down\n");
    ipc_detach(g_shm);
    return 0;
}
