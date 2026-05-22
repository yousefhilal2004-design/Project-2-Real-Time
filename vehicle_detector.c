/*
 * vehicle_detector.c  —  Anwar Atawna
 * -------------------------------------
 * Simulates vehicle arrival and departure at each approach.
 *
 * Behaviour:
 *   Every 0.5-3 seconds, picks a random direction and either:
 *     - Adds a vehicle to the queue (if light is RED or YELLOW), or
 *     - Removes a vehicle from the queue (if light is GREEN — car passes).
 *
 *   Sends MSG_VEHICLE to the message queue so the controller can
 *   observe traffic density in real time.
 *
 * IPC decisions:
 *   SHM write (vehicle_count): under sem_lock() to prevent a race with
 *   the controller, which reads these counts for adaptive timing.
 *   MSG_VEHICLE: best-effort (IPC_NOWAIT) — we never want the vehicle
 *   detector to block on a full queue.
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

static SharedData            *g_shm    = NULL;
static volatile sig_atomic_t  g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void send_log(const char *text) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_LOG;
    m.source    = SRC_VEHICLE_DET;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "[VDET] %s", text);
    msg_send(&m);
}

static void send_vehicle_event(int dir, int delta, int count) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_VEHICLE;
    m.source    = SRC_VEHICLE_DET;
    m.direction = dir;
    m.value     = count;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "%s %s (queue=%d)",
             (delta > 0) ? "vehicle arrived at" : "vehicle passed at",
             dir_str(dir), count);
    msg_send(&m);
}

int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[VDET] ipc_attach failed\n"); return 1; }

    printf("[VDET] vehicle detector started (pid=%d)\n", getpid());
    send_log("vehicle detector started");

    while (g_running) {
        sem_lock();
        int shutdown = g_shm->shutdown;
        sem_unlock();
        if (shutdown) break;

        /* Pick a random direction */
        int dir = rand() % NUM_DIRECTIONS;

        sem_lock();
        int count = g_shm->vehicle_count[dir];
        int light  = g_shm->light[dir];
        sem_unlock();

        int delta = 0;

        if (count >= MAX_VEHICLES_PER_DIR) {
            /* Queue saturated — one vehicle gives up and turns away */
            delta = -1;
        } else if (count == 0) {
            /* Empty queue — 33 % chance of new arrival */
            delta = (rand() % 3 == 0) ? 1 : 0;
        } else if (light == GREEN) {
            /* Green light: 60 % chance a car clears the intersection */
            delta = (rand() % 10 < 6) ? -1 : 0;
        } else {
            /* Red or yellow: 30 % chance of a new arrival */
            delta = (rand() % 10 < 3) ? 1 : 0;
        }

        if (delta != 0) {
            sem_lock();
            g_shm->vehicle_count[dir] += delta;
            if (g_shm->vehicle_count[dir] < 0) g_shm->vehicle_count[dir] = 0;
            int new_count = g_shm->vehicle_count[dir];
            sem_unlock();

            send_vehicle_event(dir, delta, new_count);

            printf("[VDET] %-5s  %s  queue=%-2d  light=%s\n",
                   dir_str(dir),
                   (delta > 0) ? "arrival  " : "departure",
                   new_count,
                   light_str(light));
            fflush(stdout);
        }

        /* Random inter-event interval: 500 ms – 3 s */
        usleep((500 + rand() % 2500) * 1000);
    }

    printf("[VDET] shutting down\n");
    send_log("vehicle detector shutting down");
    ipc_detach(g_shm);
    return 0;
}
