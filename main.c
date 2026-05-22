/*
 * main.c 
 * -----------------------
 * Master / supervisor process.
 *
 * Responsibilities:
 *   1. Create all IPC objects (SHM, semaphores, message queue).
 *   2. Fork + exec every child process in the correct startup order.
 *   3. Monitor children; on SIGINT/SIGTERM set shutdown flag, SIGTERM
 *      all children, wait for them, then destroy IPC.
 *
 * Startup order:
 *   logger     — must be running before anyone sends MSG_LOG.
 *   controller — sets initial light states before traffic_light attaches.
 *   everything else.
 *
 * Usage:
 *   ./main            interactive mode
 *   ./main --auto     fully automatic demo (recommended for demos)
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

static SharedData *g_shm   = NULL;
static pid_t       g_pids[32];
static int         g_npids = 0;
static volatile sig_atomic_t g_shutdown = 0;

static void on_signal(int sig) { (void)sig; g_shutdown = 1; }

static pid_t spawn(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(path, argv);
        perror(path);
        exit(1);
    }
    if (pid < 0) { perror("fork"); return -1; }
    g_pids[g_npids++] = pid;
    printf("[MAIN] started %-22s pid=%d\n", path, pid);
    fflush(stdout);
    return pid;
}

static void shutdown_all(void) {
    printf("\n[MAIN] shutdown — signalling all child processes...\n");
    fflush(stdout);

    if (g_shm) {
        sem_lock();
        g_shm->shutdown = 1;
        sem_unlock();
    }
    sleep(1);

    for (int i = 0; i < g_npids; i++)
        if (g_pids[i] > 0) kill(g_pids[i], SIGTERM);

    int status;
    pid_t pid;
    while ((pid = wait(&status)) > 0) {
        for (int i = 0; i < g_npids; i++) {
            if (g_pids[i] != pid) continue;
            if (WIFEXITED(status))
                printf("[MAIN] pid=%-6d exited   status=%d\n",
                       pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                printf("[MAIN] pid=%-6d killed   signal=%d\n",
                       pid, WTERMSIG(status));
            g_pids[i] = -1;
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    int auto_mode = (argc > 1 && strcmp(argv[1], "--auto") == 0);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGCHLD, SIG_DFL);

    printf("=================================================\n");
    printf("  Real-Time Traffic Light Control System\n");
    printf("  Anwar Atawna — Linux IPC Project\n");
    printf("=================================================\n");
    printf("[MAIN] mode : %s\n", auto_mode ? "AUTOMATIC" : "INTERACTIVE");
    printf("[MAIN] Ctrl+C to stop\n\n");
    fflush(stdout);

    g_shm = ipc_init();
    if (!g_shm) { fprintf(stderr, "[MAIN] ipc_init failed\n"); return 1; }

    sem_lock();
    memset(g_shm, 0, sizeof(SharedData));
    g_shm->running  = 1;
    g_shm->shutdown = 0;
    for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
    sem_unlock();

    { char *a[] = {"./logger",    NULL}; spawn("./logger", a); }
    usleep(150 * 1000);

    { char *a[] = {"./controller", NULL}; spawn("./controller", a); }
    usleep(200 * 1000);

    char *la[4][3] = {
        {"./traffic_light", "0", NULL},
        {"./traffic_light", "1", NULL},
        {"./traffic_light", "2", NULL},
        {"./traffic_light", "3", NULL},
    };
    for (int d = 0; d < NUM_DIRECTIONS; d++) spawn("./traffic_light", la[d]);

    { char *a[] = {"./vehicle_detector", NULL}; spawn("./vehicle_detector", a); }

    if (auto_mode) {
        char *a[] = {"./pedestrian", "--auto", NULL}; spawn("./pedestrian", a);
    } else {
        printf("[MAIN] Run  ./pedestrian  in a separate terminal to request crossings\n");
    }

    if (auto_mode) {
        char *a[] = {"./emergency", "--auto", NULL};  spawn("./emergency", a);
    } else {
        printf("[MAIN] Run  ./emergency   in a separate terminal to trigger emergencies\n");
    }

    { char *a[] = {"./safety_monitor", NULL}; spawn("./safety_monitor", a); }

    printf("\n[MAIN] %d processes running — system operational.\n\n", g_npids);
    fflush(stdout);

    while (!g_shutdown) {
        sleep(1);
        sem_lock();
        int running = g_shm->running;
        sem_unlock();
        if (!running) {
            printf("[MAIN] controller signalled stop\n");
            break;
        }
    }

    shutdown_all();

    printf("[MAIN] destroying IPC...\n");
    ipc_destroy();
    printf("[MAIN] done.\n");
    return 0;
}
