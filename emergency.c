/*
 * emergency.c  
 * --------------------------------
 * Emergency vehicle detection process.
 *
 * Two operating modes:
 *   interactive (default): type a direction number (0-3) and press ENTER.
 *   automatic   (--auto) : random emergency every 45-120 seconds.
 *
 * Run in its own terminal:
 *   ./emergency
 *
 * IPC:
 *   Sets SHM emergency_mode immediately so the safety monitor sees it.
 *   Sends MSG_EMERGENCY so the controller preempts the cycle (≤ 100 ms).
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

static void trigger_emergency(int dir) {
    sem_lock();
    g_shm->emergency_mode      = 1;
    g_shm->emergency_direction = dir;
    sem_unlock();

    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_EMERGENCY;
    m.source    = SRC_EMERGENCY;
    m.direction = dir;
    m.value     = 1;
    m.priority  = 1;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message),
             "EMERGENCY vehicle approaching from %s!", dir_str(dir));
    msg_send(&m);

    Message log_m;
    memset(&log_m, 0, sizeof(log_m));
    log_m.mtype     = MSG_LOG;
    log_m.source    = SRC_EMERGENCY;
    log_m.timestamp = time(NULL);
    snprintf(log_m.message, sizeof(log_m.message),
             "Emergency vehicle from %s — preempting traffic", dir_str(dir));
    msg_send(&log_m);

    printf("[EMRG] *** EMERGENCY from %-5s — controller notified ***\n",
           dir_str(dir));
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    int auto_mode = (argc > 1 && strcmp(argv[1], "--auto") == 0);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[EMRG] ipc_attach failed\n"); return 1; }

    printf("[EMRG] emergency process started (pid=%d)\n", getpid());
    fflush(stdout);

    if (auto_mode) {
        printf("[EMRG] auto mode — random emergencies every 45-120 s\n");
        fflush(stdout);
        while (g_running) {
            sem_lock(); int sd = g_shm->shutdown; sem_unlock();
            if (sd) break;

            int wait = 45 + rand() % 75;
            printf("[EMRG] next emergency in %d seconds...\n", wait);
            fflush(stdout);

            for (int t = 0; t < wait && g_running; t++) {
                sleep(1);
                sem_lock(); int sd2 = g_shm->shutdown; sem_unlock();
                if (sd2) { g_running = 0; break; }
            }
            if (!g_running) break;

            trigger_emergency(rand() % NUM_DIRECTIONS);
            sleep(EMERGENCY_DURATION + ALL_RED_DURATION + 5);
        }
    } else {
        printf("[EMRG] Emergency mode  —  0=NORTH  1=SOUTH  2=EAST  3=WEST\n");
        printf("[EMRG] Type a direction number and press ENTER (Ctrl+C to quit)\n\n");
        fflush(stdout);

        while (g_running) {
            sem_lock(); int sd = g_shm->shutdown; sem_unlock();
            if (sd) break;

            printf("Direction (0-3): ");
            fflush(stdout);

            char buf[32];
            if (!fgets(buf, sizeof(buf), stdin)) break;

            int dir;
            if (sscanf(buf, "%d", &dir) != 1) continue;
            if (dir < 0 || dir >= NUM_DIRECTIONS) {
                printf("[EMRG] Invalid — use 0=N  1=S  2=E  3=W\n");
                fflush(stdout);
                continue;
            }
            trigger_emergency(dir);
        }
    }

    printf("[EMRG] emergency process shutting down\n");
    ipc_detach(g_shm);
    return 0;
}
