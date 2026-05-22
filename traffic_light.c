/*
 * traffic_light.c  —  Anwar Atawna
 * ----------------------------------
 * Represents one traffic light (one process per direction).
 * Usage:  ./traffic_light <direction>    0=NORTH  1=SOUTH  2=EAST  3=WEST
 *
 * Responsibilities:
 *   - Receive state-change commands from the controller.
 *   - Verify that SHM matches the commanded state (integrity check).
 *   - Send MSG_CONFIRM back to the controller.
 *   - Send MSG_LOG on every state change.
 *   - Poll SHM periodically to catch any missed commands.
 *
 * IPC decisions:
 *   MSG_CMD_FOR(direction): each light listens ONLY on its own mtype
 *   (11=NORTH, 12=SOUTH, 13=EAST, 14=WEST).  msgrcv() filters at the
 *   kernel level — no application-level routing, no race between lights.
 *
 *   SHM is the authoritative state; the command message is a notification.
 *   If the message and SHM disagree we log the discrepancy and trust SHM.
 *
 *   Poll interval: 100 ms — responsive without burning CPU.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

static int                    g_dir       = -1;
static SharedData            *g_shm       = NULL;
static int                    g_last      = -1;   /* last known straight state */
static int                    g_last_left = -1;   /* last known left-turn state */
static volatile sig_atomic_t  g_running   =  1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Send a log message to the logger process                            */
/* ------------------------------------------------------------------ */
static void send_log(const char *text) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_LOG;
    m.source    = SRC_TRAFFIC_LIGHT + g_dir;
    m.direction = g_dir;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "[LIGHT-%s] %s",
             dir_str(g_dir), text);
    msg_send(&m);
}

/* ------------------------------------------------------------------ */
/* Send confirmation back to controller                                */
/* ------------------------------------------------------------------ */
static void send_confirm(int state) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_CONFIRM;
    m.source    = SRC_TRAFFIC_LIGHT + g_dir;
    m.direction = g_dir;
    m.value     = state;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "CONFIRM %s → %s",
             dir_str(g_dir), light_str(state));
    msg_send(&m);
}

/* ------------------------------------------------------------------ */
/* Handle a command from the controller                                */
/* ------------------------------------------------------------------ */
static void handle_command(const Message *cmd) {
    int requested = cmd->value;

    if (requested != RED && requested != YELLOW && requested != GREEN) {
        fprintf(stderr, "[LIGHT-%s] bad command value %d\n",
                dir_str(g_dir), requested);
        return;
    }

    /* Controller writes SHM before sending the message */
    sem_lock();
    int actual      = g_shm->light[g_dir];
    int left_actual = g_shm->left_light[g_dir];
    sem_unlock();

    if (actual != requested) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "CMD/SHM mismatch: cmd=%s shm=%s — using SHM",
                 light_str(requested), light_str(actual));
        fprintf(stderr, "[LIGHT-%s] %s\n", dir_str(g_dir), buf);
        send_log(buf);
        requested = actual;
    }

    if (requested == g_last && left_actual == g_last_left)
        return;   /* no change */

    printf("[LIGHT-%s]  straight:%s→%s  left:%s→%s\n",
           dir_str(g_dir),
           (g_last      == -1) ? "INIT" : light_str(g_last),
           light_str(requested),
           (g_last_left == -1) ? "INIT" : light_str(g_last_left),
           light_str(left_actual));
    fflush(stdout);

    char buf[128];
    snprintf(buf, sizeof(buf), "straight→%s  left→%s",
             light_str(requested), light_str(left_actual));
    send_log(buf);
    send_confirm(requested);
    g_last      = requested;
    g_last_left = left_actual;
}

/* ------------------------------------------------------------------ */
/* Poll SHM — catches state changes if a command message was lost      */
/* ------------------------------------------------------------------ */
static void poll_shm(void) {
    sem_lock();
    int current      = g_shm->light[g_dir];
    int left_current = g_shm->left_light[g_dir];
    int shutdown     = g_shm->shutdown;
    sem_unlock();

    if (shutdown) { g_running = 0; return; }

    if (g_last == -1) {
        g_last      = current;
        g_last_left = left_current;
        printf("[LIGHT-%s] initial: straight=%s left=%s\n",
               dir_str(g_dir), light_str(current), light_str(left_current));
        fflush(stdout);
        return;
    }

    if (current != g_last || left_current != g_last_left) {
        printf("[LIGHT-%s] (SHM update) straight:%s→%s  left:%s→%s\n",
               dir_str(g_dir),
               light_str(g_last),      light_str(current),
               light_str(g_last_left), light_str(left_current));
        fflush(stdout);
        char buf[128];
        snprintf(buf, sizeof(buf), "SHM update: straight→%s left→%s",
                 light_str(current), light_str(left_current));
        send_log(buf);
        g_last      = current;
        g_last_left = left_current;
    }
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <direction 0..3>\n", argv[0]);
        return 1;
    }
    g_dir = atoi(argv[1]);
    if (g_dir < 0 || g_dir >= NUM_DIRECTIONS) {
        fprintf(stderr, "Invalid direction %d (must be 0-3)\n", g_dir);
        return 1;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    g_shm = ipc_attach();
    if (!g_shm) {
        fprintf(stderr, "[LIGHT-%s] ipc_attach failed\n", dir_str(g_dir));
        return 1;
    }

    printf("[LIGHT-%s] started (pid=%d)\n", dir_str(g_dir), getpid());
    fflush(stdout);

    char buf[64];
    snprintf(buf, sizeof(buf), "process started");
    send_log(buf);

    /* Listen on the message type reserved for this direction */
    long my_mtype = MSG_CMD_FOR(g_dir);

    while (g_running) {
        Message cmd;
        int n = msg_recv(&cmd, my_mtype, 0);   /* non-blocking */
        if (n > 0) {
            handle_command(&cmd);
        } else if (n < 0 && errno != ENOMSG) {
            if (errno == EIDRM || errno == EINVAL) break;  /* queue gone */
            perror("[LIGHT] msg_recv");
        }

        poll_shm();
        usleep(100 * 1000);   /* 100 ms */
    }

    printf("[LIGHT-%s] shutting down\n", dir_str(g_dir));
    send_log("process shutting down");
    ipc_detach(g_shm);
    return 0;
}
