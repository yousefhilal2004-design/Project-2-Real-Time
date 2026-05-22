/*
 * controller.c  —  Anwar Atawna
 * ------------------------------
 * The "brain" of the traffic light system.
 *
 * Full cycle (protected left-turn first):
 *   PHASE_NS_LEFT_GREEN  → PHASE_NS_LEFT_YELLOW  → PHASE_ALL_RED_3 →
 *   PHASE_NS_GREEN       → PHASE_NS_YELLOW        → PHASE_ALL_RED_1 →
 *   PHASE_EW_LEFT_GREEN  → PHASE_EW_LEFT_YELLOW  → PHASE_ALL_RED_4 →
 *   PHASE_EW_GREEN       → PHASE_EW_YELLOW        → PHASE_ALL_RED_2 → (repeat)
 *
 * Each direction carries two independent signals:
 *   light[d]      — straight-through / right-turn signal
 *   left_light[d] — protected left-turn arrow signal
 *
 * Interrupts (highest-priority first):
 *   EMERGENCY  — preempts any phase; clears via YELLOW+ALL-RED.
 *   PEDESTRIAN — preempts any green phase (straight or left).
 *
 * IPC:
 *   SHM  : controller is the ONLY writer of light[] and left_light[].
 *   MSG  : CMD messages carry both value (straight) and left_value.
 *   LOG  : every phase transition logged.
 *
 * Timing resolution: 100 ms — check_msgs() is called 10× per second so
 * emergency/pedestrian events are detected within 100 ms regardless of
 * which phase is active.
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

static SharedData            *g_shm    = NULL;
static volatile sig_atomic_t  g_running = 1;

static int g_want_pedestrian = 0;
static int g_want_emergency  = 0;
static int g_emergency_dir   = -1;
static int g_pedestrian_dir  = 0;

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Logging helper                                                       */
/* ------------------------------------------------------------------ */
static void log_event(const char *fmt, ...) {
    Message m;
    va_list ap;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_LOG;
    m.source    = SRC_CONTROLLER;
    m.timestamp = time(NULL);
    va_start(ap, fmt);
    vsnprintf(m.message, sizeof(m.message), fmt, ap);
    va_end(ap);
    msg_send(&m);
    printf("[CTRL] %s\n", m.message);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* apply_phase: write both light[] and left_light[] to SHM, then      */
/*              broadcast CMD messages to all four traffic_light procs */
/* ------------------------------------------------------------------ */
static void apply_phase(int phase) {
    sem_lock();
    g_shm->current_phase    = phase;
    g_shm->phase_start_time = time(NULL);
    g_shm->last_update      = time(NULL);

    for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->left_light[d] = RED;

    switch (phase) {

    case PHASE_NS_LEFT_GREEN:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->left_light[NORTH] = GREEN; g_shm->left_light[SOUTH] = GREEN;
        g_shm->pedestrian_active = 0;     g_shm->emergency_mode    = 0;
        break;

    case PHASE_NS_LEFT_YELLOW:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->left_light[NORTH] = YELLOW; g_shm->left_light[SOUTH] = YELLOW;
        break;

    case PHASE_NS_GREEN:
        g_shm->light[NORTH] = GREEN;  g_shm->light[SOUTH] = GREEN;
        g_shm->light[EAST]  = RED;    g_shm->light[WEST]  = RED;
        g_shm->pedestrian_active = 0; g_shm->emergency_mode = 0;
        break;

    case PHASE_NS_YELLOW:
        g_shm->light[NORTH] = YELLOW; g_shm->light[SOUTH] = YELLOW;
        g_shm->light[EAST]  = RED;    g_shm->light[WEST]  = RED;
        break;

    case PHASE_ALL_RED_1:
    case PHASE_ALL_RED_2:
    case PHASE_ALL_RED_3:
    case PHASE_ALL_RED_4:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        break;

    case PHASE_EW_LEFT_GREEN:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->left_light[EAST] = GREEN; g_shm->left_light[WEST] = GREEN;
        g_shm->pedestrian_active = 0;    g_shm->emergency_mode   = 0;
        break;

    case PHASE_EW_LEFT_YELLOW:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->left_light[EAST] = YELLOW; g_shm->left_light[WEST] = YELLOW;
        break;

    case PHASE_EW_GREEN:
        g_shm->light[NORTH] = RED;    g_shm->light[SOUTH] = RED;
        g_shm->light[EAST]  = GREEN;  g_shm->light[WEST]  = GREEN;
        g_shm->pedestrian_active = 0; g_shm->emergency_mode = 0;
        break;

    case PHASE_EW_YELLOW:
        g_shm->light[NORTH] = RED;    g_shm->light[SOUTH] = RED;
        g_shm->light[EAST]  = YELLOW; g_shm->light[WEST]  = YELLOW;
        break;

    case PHASE_PEDESTRIAN:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->pedestrian_active  = 1;
        g_shm->pedestrian_request = 0;
        g_shm->emergency_mode     = 0;
        break;

    case PHASE_EMERGENCY:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->light[g_emergency_dir] = GREEN;
        g_shm->emergency_mode         = 1;
        g_shm->emergency_direction    = g_emergency_dir;
        g_shm->pedestrian_active      = 0;
        g_shm->pedestrian_request     = 0;
        break;
    }

    int lights[NUM_DIRECTIONS], lefts[NUM_DIRECTIONS];
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        lights[d] = g_shm->light[d];
        lefts[d]  = g_shm->left_light[d];
    }
    sem_unlock();

    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        Message cmd;
        build_cmd_message(&cmd, d, lights[d], lefts[d]);
        msg_send(&cmd);
    }

    log_event("Phase → %s", phase_str(phase));
}

/* ------------------------------------------------------------------ */
/* check_msgs: drain pending requests (non-blocking)                   */
/* ------------------------------------------------------------------ */
static void check_msgs(void) {
    Message m;

    while (msg_recv(&m, MSG_PEDESTRIAN, 0) > 0) {
        if (!g_want_pedestrian && !g_want_emergency) {
            g_want_pedestrian = 1;
            g_pedestrian_dir  = m.direction;
            log_event("Pedestrian request [%s] received — will serve at next opportunity",
                      dir_str(m.direction));
            sem_lock();
            g_shm->pedestrian_request   = 1;
            g_shm->pedestrian_direction = m.direction;
            sem_unlock();
        }
    }

    while (msg_recv(&m, MSG_EMERGENCY, 0) > 0) {
        g_want_emergency = 1;
        g_emergency_dir  = m.direction;
        log_event("EMERGENCY vehicle from %s — preempting cycle!",
                  dir_str(m.direction));
    }
}

/* ------------------------------------------------------------------ */
/* Adaptive green duration (straight phases only)                      */
/* ------------------------------------------------------------------ */
static int green_duration(int d1, int d2) {
    sem_lock();
    int n = g_shm->vehicle_count[d1] + g_shm->vehicle_count[d2];
    sem_unlock();
    int t = GREEN_DURATION + n * GREEN_TIME_PER_VEHICLE;
    if (t < MIN_GREEN_DURATION) t = MIN_GREEN_DURATION;
    if (t > MAX_GREEN_DURATION) t = MAX_GREEN_DURATION;
    return t;
}

/* ------------------------------------------------------------------ */
/* tick: sleep 100 ms — basic timing unit                             */
/* ------------------------------------------------------------------ */
#define TICKS_PER_SEC  10
#define TICK_US        (1000000 / TICKS_PER_SEC)   /* 100 000 µs */

/* sleep_secs: interruptible sleep in 100 ms steps, polling messages  */
/* Returns 1 if interrupted by emergency, 0 otherwise.                */
static int sleep_secs_em(int secs) {
    for (int t = 0; t < secs * TICKS_PER_SEC && g_running; t++) {
        usleep(TICK_US);
        check_msgs();
        if (g_want_emergency) return 1;
    }
    return 0;
}

/* plain sleep (no check_msgs, used inside safe_to_allred)            */
static void sleep_plain(int secs) {
    for (int t = 0; t < secs * TICKS_PER_SEC && g_running; t++)
        usleep(TICK_US);
}

/* ------------------------------------------------------------------ */
/* safe_to_allred: enforce GREEN→YELLOW→RED rule for all phase types   */
/* ------------------------------------------------------------------ */
static void safe_to_allred(int cur_phase) {
    switch (cur_phase) {
    case PHASE_NS_LEFT_GREEN:
        apply_phase(PHASE_NS_LEFT_YELLOW);
        sleep_plain(YELLOW_DURATION);
        apply_phase(PHASE_ALL_RED_3);
        sleep_plain(ALL_RED_DURATION);
        break;
    case PHASE_NS_LEFT_YELLOW:
        apply_phase(PHASE_ALL_RED_3);
        sleep_plain(ALL_RED_DURATION);
        break;
    case PHASE_NS_GREEN:
        apply_phase(PHASE_NS_YELLOW);
        sleep_plain(YELLOW_DURATION);
        apply_phase(PHASE_ALL_RED_1);
        sleep_plain(ALL_RED_DURATION);
        break;
    case PHASE_NS_YELLOW:
        apply_phase(PHASE_ALL_RED_1);
        sleep_plain(ALL_RED_DURATION);
        break;
    case PHASE_EW_LEFT_GREEN:
        apply_phase(PHASE_EW_LEFT_YELLOW);
        sleep_plain(YELLOW_DURATION);
        apply_phase(PHASE_ALL_RED_4);
        sleep_plain(ALL_RED_DURATION);
        break;
    case PHASE_EW_LEFT_YELLOW:
        apply_phase(PHASE_ALL_RED_4);
        sleep_plain(ALL_RED_DURATION);
        break;
    case PHASE_EW_GREEN:
        apply_phase(PHASE_EW_YELLOW);
        sleep_plain(YELLOW_DURATION);
        apply_phase(PHASE_ALL_RED_2);
        sleep_plain(ALL_RED_DURATION);
        break;
    case PHASE_EW_YELLOW:
        apply_phase(PHASE_ALL_RED_2);
        sleep_plain(ALL_RED_DURATION);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Handle emergency: safe clear → emergency green → resume            */
/* ------------------------------------------------------------------ */
static void handle_emergency(int *cur) {
    g_want_emergency = 0;
    log_event("Handling EMERGENCY [%s] — clearing intersection",
              dir_str(g_emergency_dir));

    safe_to_allred(*cur);
    if (!g_running) return;

    apply_phase(PHASE_EMERGENCY);
    *cur = PHASE_EMERGENCY;

    /* Hold emergency green — check msgs every 100 ms */
    for (int t = 0; t < EMERGENCY_DURATION * TICKS_PER_SEC && g_running; t++) {
        usleep(TICK_US);
        check_msgs();
    }

    /* Emergency direction must pass through YELLOW before RED */
    sem_lock();
    g_shm->light[g_emergency_dir] = YELLOW;
    g_shm->last_update = time(NULL);
    sem_unlock();
    {   Message cmd;
        build_cmd_message(&cmd, g_emergency_dir, YELLOW, RED);
        msg_send(&cmd); }
    log_event("Emergency direction %s → YELLOW (safety transition)",
              dir_str(g_emergency_dir));
    sleep_plain(YELLOW_DURATION);

    apply_phase(PHASE_ALL_RED_1);
    sleep_plain(ALL_RED_DURATION);
    apply_phase(PHASE_NS_LEFT_GREEN);
    *cur = PHASE_NS_LEFT_GREEN;
    log_event("Emergency cleared — resuming normal cycle");
}

/* ------------------------------------------------------------------ */
/* Handle pedestrian: safe clear → walk signal → resume               */
/* If an emergency fires mid-crossing: preempt, then resume remaining */
/* ------------------------------------------------------------------ */
static void handle_pedestrian(int *cur) {
    g_want_pedestrian = 0;
    int ped_dir = g_pedestrian_dir;
    log_event("Handling PEDESTRIAN [%s] — clearing intersection", dir_str(ped_dir));

    safe_to_allred(*cur);
    if (!g_running) return;

    apply_phase(PHASE_PEDESTRIAN);
    *cur = PHASE_PEDESTRIAN;

    int ticks_left = PEDESTRIAN_DURATION * TICKS_PER_SEC;

    while (ticks_left > 0 && g_running) {
        usleep(TICK_US);
        check_msgs();

        if (g_want_emergency) {
            int secs_left = ticks_left / TICKS_PER_SEC;
            log_event("Emergency interrupts pedestrian crossing — %ds remaining",
                      secs_left);

            /* Emergency takes priority — handle it fully */
            handle_emergency(cur);

            if (secs_left > 0 && g_running) {
                /* Restore pedestrian direction (emergency phase cleared it) */
                sem_lock();
                g_shm->pedestrian_direction = ped_dir;
                sem_unlock();

                log_event("Resuming PEDESTRIAN [%s] — %ds remaining",
                          dir_str(ped_dir), secs_left);
                apply_phase(PHASE_PEDESTRIAN);
                *cur       = PHASE_PEDESTRIAN;
                ticks_left = secs_left * TICKS_PER_SEC;
            } else {
                ticks_left = 0;
            }
            continue;
        }

        ticks_left--;
    }

    apply_phase(PHASE_ALL_RED_1);
    sleep_plain(ALL_RED_DURATION);
    apply_phase(PHASE_NS_LEFT_GREEN);
    *cur = PHASE_NS_LEFT_GREEN;
    log_event("Pedestrian crossing complete — resuming normal cycle");
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */
int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[CTRL] ipc_attach failed\n"); return 1; }

    sem_lock();
    g_shm->controller_pid = getpid();
    g_shm->running        = 1;
    sem_unlock();

    printf("[CTRL] controller started (pid=%d)\n", getpid());
    log_event("Controller started — beginning traffic cycle");

    apply_phase(PHASE_NS_LEFT_GREEN);
    int cur = PHASE_NS_LEFT_GREEN;

    while (g_running) {
        sem_lock(); int shutdown = g_shm->shutdown; sem_unlock();
        if (shutdown) break;

        if (g_want_emergency) { handle_emergency(&cur); continue; }

        if (g_want_pedestrian &&
            (cur == PHASE_NS_LEFT_GREEN || cur == PHASE_EW_LEFT_GREEN ||
             cur == PHASE_NS_GREEN      || cur == PHASE_EW_GREEN)) {
            handle_pedestrian(&cur);
            continue;
        }

        switch (cur) {

        /* ---- N-S LEFT-TURN ---- */
        case PHASE_NS_LEFT_GREEN: {
            int dur = LEFT_GREEN_DURATION;
            log_event("N-S LEFT-GREEN  dur=%ds", dur);
            for (int t = 0; t < dur * TICKS_PER_SEC && g_running; t++) {
                usleep(TICK_US);
                check_msgs();
                if (g_want_emergency || g_want_pedestrian) break;
                if (t % TICKS_PER_SEC == 0) {
                    sem_lock();
                    g_shm->phase_time_remaining = dur - t / TICKS_PER_SEC;
                    sem_unlock();
                }
            }
            if (!g_want_emergency && !g_want_pedestrian) {
                apply_phase(PHASE_NS_LEFT_YELLOW); cur = PHASE_NS_LEFT_YELLOW;
            }
            break;
        }

        case PHASE_NS_LEFT_YELLOW:
            if (sleep_secs_em(YELLOW_DURATION)) break;
            apply_phase(PHASE_ALL_RED_3); cur = PHASE_ALL_RED_3;
            break;

        case PHASE_ALL_RED_3:
            if (sleep_secs_em(ALL_RED_DURATION)) break;
            apply_phase(PHASE_NS_GREEN); cur = PHASE_NS_GREEN;
            break;

        /* ---- N-S STRAIGHT ---- */
        case PHASE_NS_GREEN: {
            int dur = green_duration(NORTH, SOUTH);
            log_event("N-S GREEN  dur=%ds  (N:%d S:%d vehicles)", dur,
                      g_shm->vehicle_count[NORTH], g_shm->vehicle_count[SOUTH]);
            for (int t = 0; t < dur * TICKS_PER_SEC && g_running; t++) {
                usleep(TICK_US);
                check_msgs();
                if (g_want_emergency || g_want_pedestrian) break;
                if (t % TICKS_PER_SEC == 0) {
                    sem_lock();
                    g_shm->phase_time_remaining = dur - t / TICKS_PER_SEC;
                    sem_unlock();
                }
            }
            if (!g_want_emergency && !g_want_pedestrian) {
                apply_phase(PHASE_NS_YELLOW); cur = PHASE_NS_YELLOW;
            }
            break;
        }

        case PHASE_NS_YELLOW:
            if (sleep_secs_em(YELLOW_DURATION)) break;
            apply_phase(PHASE_ALL_RED_1); cur = PHASE_ALL_RED_1;
            break;

        case PHASE_ALL_RED_1:
            if (sleep_secs_em(ALL_RED_DURATION)) break;
            apply_phase(PHASE_EW_LEFT_GREEN); cur = PHASE_EW_LEFT_GREEN;
            break;

        /* ---- E-W LEFT-TURN ---- */
        case PHASE_EW_LEFT_GREEN: {
            int dur = LEFT_GREEN_DURATION;
            log_event("E-W LEFT-GREEN  dur=%ds", dur);
            for (int t = 0; t < dur * TICKS_PER_SEC && g_running; t++) {
                usleep(TICK_US);
                check_msgs();
                if (g_want_emergency || g_want_pedestrian) break;
                if (t % TICKS_PER_SEC == 0) {
                    sem_lock();
                    g_shm->phase_time_remaining = dur - t / TICKS_PER_SEC;
                    sem_unlock();
                }
            }
            if (!g_want_emergency && !g_want_pedestrian) {
                apply_phase(PHASE_EW_LEFT_YELLOW); cur = PHASE_EW_LEFT_YELLOW;
            }
            break;
        }

        case PHASE_EW_LEFT_YELLOW:
            if (sleep_secs_em(YELLOW_DURATION)) break;
            apply_phase(PHASE_ALL_RED_4); cur = PHASE_ALL_RED_4;
            break;

        case PHASE_ALL_RED_4:
            if (sleep_secs_em(ALL_RED_DURATION)) break;
            apply_phase(PHASE_EW_GREEN); cur = PHASE_EW_GREEN;
            break;

        /* ---- E-W STRAIGHT ---- */
        case PHASE_EW_GREEN: {
            int dur = green_duration(EAST, WEST);
            log_event("E-W GREEN  dur=%ds  (E:%d W:%d vehicles)", dur,
                      g_shm->vehicle_count[EAST], g_shm->vehicle_count[WEST]);
            for (int t = 0; t < dur * TICKS_PER_SEC && g_running; t++) {
                usleep(TICK_US);
                check_msgs();
                if (g_want_emergency || g_want_pedestrian) break;
                if (t % TICKS_PER_SEC == 0) {
                    sem_lock();
                    g_shm->phase_time_remaining = dur - t / TICKS_PER_SEC;
                    sem_unlock();
                }
            }
            if (!g_want_emergency && !g_want_pedestrian) {
                apply_phase(PHASE_EW_YELLOW); cur = PHASE_EW_YELLOW;
            }
            break;
        }

        case PHASE_EW_YELLOW:
            if (sleep_secs_em(YELLOW_DURATION)) break;
            apply_phase(PHASE_ALL_RED_2); cur = PHASE_ALL_RED_2;
            break;

        case PHASE_ALL_RED_2:
            if (sleep_secs_em(ALL_RED_DURATION)) break;
            apply_phase(PHASE_NS_LEFT_GREEN); cur = PHASE_NS_LEFT_GREEN;
            break;

        default:
            log_event("Unexpected phase %d — recovering", cur);
            apply_phase(PHASE_ALL_RED_1); cur = PHASE_ALL_RED_1;
            break;
        }
    }

    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        Message cmd;
        sem_lock();
        g_shm->light[d]      = RED;
        g_shm->left_light[d] = RED;
        sem_unlock();
        build_cmd_message(&cmd, d, RED, RED);
        msg_send(&cmd);
    }
    sem_lock(); g_shm->running = 0; sem_unlock();

    log_event("Controller shut down — all lights RED");
    ipc_detach(g_shm);
    return 0;
}
