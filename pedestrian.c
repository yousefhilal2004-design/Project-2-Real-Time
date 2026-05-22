/*
 * pedestrian.c  —  Anwar Atawna
 * --------------------------------
 * Pedestrian crossing request process.
 *
 * Two operating modes:
 *   interactive (default): press ENTER then choose direction 0-3.
 *   automatic   (--auto) : sends a random-direction request every 20-60 seconds.
 *
 * Run in its own terminal:
 *   ./pedestrian
 *
 * IPC:
 *   MSG_PEDESTRIAN: sends crossing request (with direction) to the controller.
 *   SHM pedestrian_request / pedestrian_active / pedestrian_direction: set here.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

static SharedData            *g_shm    = NULL;
static volatile sig_atomic_t  g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void send_request(int dir) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_PEDESTRIAN;
    m.source    = SRC_PEDESTRIAN;
    m.direction = dir;
    m.value     = 1;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message),
             "Pedestrian crossing request [%s]", dir_str(dir));
    msg_send(&m);

    sem_lock();
    g_shm->pedestrian_request   = 1;
    g_shm->pedestrian_direction = dir;
    sem_unlock();
}

/* select() with timeout: returns 1 if stdin has data within ms milliseconds */
static int stdin_ready(int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

int main(int argc, char *argv[]) {
    int auto_mode = (argc > 1 && strcmp(argv[1], "--auto") == 0);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[PED] ipc_attach failed\n"); return 1; }

    printf("[PED] pedestrian process started (pid=%d)\n", getpid());
    fflush(stdout);

    int pending    = 0;
    int walk_shown = 0;

    if (auto_mode) {
        printf("[PED] auto mode — random direction requests every 20-60 s\n");
        fflush(stdout);

        while (g_running) {
            sem_lock();
            int sd     = g_shm->shutdown;
            int active = g_shm->pedestrian_active;
            int req    = g_shm->pedestrian_request;
            int pdir   = g_shm->pedestrian_direction;
            sem_unlock();
            if (sd) break;

            if (pending && !req && !active) {
                pending    = 0;
                walk_shown = 0;
                printf("[PED] crossing complete\n");
                fflush(stdout);
            }
            if (active && !walk_shown) {
                printf("[PED] ** WALK SIGNAL ACTIVE for %s crossing **\n",
                       dir_str(pdir));
                fflush(stdout);
                walk_shown = 1;
            }

            if (!pending) {
                int wait = 20 + rand() % 40;
                printf("[PED] next automatic request in %d seconds\n", wait);
                fflush(stdout);
                for (int t = 0; t < wait && g_running; t++) {
                    sleep(1);
                    sem_lock(); int sd2 = g_shm->shutdown; sem_unlock();
                    if (sd2) break;
                }
                if (!g_running) break;

                int dir = rand() % NUM_DIRECTIONS;
                printf("[PED] sending automatic request for %s crossing!\n",
                       dir_str(dir));
                fflush(stdout);
                send_request(dir);
                pending = 1;
            } else {
                usleep(500 * 1000);
            }
        }

    } else {
        /* Interactive mode — press ENTER, then choose direction 0-3. */
        printf("[PED] Pedestrian mode\n");
        printf("[PED] Press ENTER to request crossing, then choose direction (Ctrl+C to quit)\n\n");
        fflush(stdout);

        while (g_running) {
            sem_lock();
            int sd     = g_shm->shutdown;
            int active = g_shm->pedestrian_active;
            int req    = g_shm->pedestrian_request;
            int pdir   = g_shm->pedestrian_direction;
            sem_unlock();
            if (sd) break;

            /* Detect walk-signal activation */
            if (active && !walk_shown) {
                printf("\n[PED] ** WALK SIGNAL ACTIVE for %s crossing **\n",
                       dir_str(pdir));
                fflush(stdout);
                walk_shown = 1;
            }

            /* Detect crossing complete */
            if (pending && !req && !active) {
                pending    = 0;
                walk_shown = 0;
                printf("[PED] Crossing complete.\n\n");
                fflush(stdout);
            }

            if (!pending) {
                /* Step 1: wait for ENTER */
                printf("Press ENTER to request crossing: ");
                fflush(stdout);

                int got_enter = 0;
                while (g_running && !got_enter) {
                    sem_lock(); int sd2 = g_shm->shutdown; sem_unlock();
                    if (sd2) { g_running = 0; break; }
                    if (stdin_ready(300)) {
                        char buf[32];
                        if (!fgets(buf, sizeof(buf), stdin)) {
                            g_running = 0; break;
                        }
                        got_enter = 1;
                    }
                }
                if (!g_running) break;

                /* Step 2: choose direction */
                printf("Choose direction: 0=NORTH  1=SOUTH  2=EAST  3=WEST\n");
                printf("Direction (0-3): ");
                fflush(stdout);

                int dir = -1;
                while (g_running && dir < 0) {
                    sem_lock(); int sd2 = g_shm->shutdown; sem_unlock();
                    if (sd2) { g_running = 0; break; }
                    if (stdin_ready(300)) {
                        char dbuf[32];
                        if (!fgets(dbuf, sizeof(dbuf), stdin)) {
                            g_running = 0; break;
                        }
                        int d;
                        if (sscanf(dbuf, "%d", &d) == 1 &&
                            d >= 0 && d < NUM_DIRECTIONS) {
                            dir = d;
                        } else {
                            printf("[PED] Invalid — use 0=N  1=S  2=E  3=W\n");
                            printf("Direction (0-3): ");
                            fflush(stdout);
                        }
                    }
                }
                if (dir < 0 || !g_running) break;

                send_request(dir);
                pending = 1;
                printf("[PED] Request sent for %s crossing — waiting for walk signal...\n",
                       dir_str(dir));
                fflush(stdout);

            } else {
                usleep(300 * 1000);
            }
        }
    }

    printf("[PED] pedestrian process shutting down\n");
    ipc_detach(g_shm);
    return 0;
}
