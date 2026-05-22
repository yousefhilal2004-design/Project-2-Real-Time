/*
 * ipc.h  —  Anwar Atawna
 * ----------------------
 * Public interface for all IPC operations:
 *   - Shared memory lifecycle
 *   - Semaphore P/V (sem_lock / sem_unlock)
 *   - System-V message queue (send / receive)
 *   - Utility helpers used by every process
 *
 * IPC design summary:
 *   SHM  : Shared state (lights, counts, flags) — single source of truth.
 *   SEM  : Binary mutex protecting SHM writes; second sem guards log file.
 *   MSGQ : Event bus — commands, logs, vehicle/ped/emergency events.
 *          Each message type has a unique mtype so msgrcv() filters
 *          at the kernel level (no application-level message routing).
 */

#ifndef IPC_H
#define IPC_H

#include "shared.h"
#include <time.h>

/* ------------------------------------------------------------------ */
/* Message structure                                                    */
/*                                                                      */
/* mtype  : kernel uses this for per-type dequeue (MSG_CMD_FOR, etc.) */
/* source : SRC_* constant identifying the sending process             */
/* direction : NORTH/SOUTH/EAST/WEST or -1 if not applicable          */
/* value  : light state, count, or boolean flag depending on mtype    */
/* priority: 0=normal, 1=high (used by emergency messages)            */
/* ------------------------------------------------------------------ */
typedef struct {
    long   mtype;
    int    source;
    int    direction;
    int    value;        /* straight/right signal state (RED/YELLOW/GREEN) */
    int    left_value;   /* left-turn arrow state       (RED/YELLOW/GREEN) */
    int    priority;
    char   message[128];
    time_t timestamp;
} Message;

/* ------------------------------------------------------------------ */
/* Shared memory + semaphore lifecycle                                  */
/* ------------------------------------------------------------------ */

/* ipc_init  : create SHM, semaphores, message queue; zero SHM.
 *             Call ONCE from the master process (main).               */
SharedData *ipc_init(void);

/* ipc_attach: attach to EXISTING IPC objects (no IPC_CREAT).
 *             Call from every child process.                          */
SharedData *ipc_attach(void);

/* ipc_detach: detach this process from SHM (does not destroy).       */
void ipc_detach(SharedData *shm);

/* ipc_destroy: detach + remove SHM, semaphores, message queue.
 *              Call ONCE from the master process on shutdown.         */
void ipc_destroy(void);

/* ------------------------------------------------------------------ */
/* Semaphore operations  (always use these — never call semop() raw)  */
/*                                                                      */
/* sem_lock   = P(SEM_MAIN)  — decrement, block if 0                  */
/* sem_unlock = V(SEM_MAIN)  — increment, wake waiting processes      */
/*                                                                      */
/* SEM_UNDO flag: kernel auto-undoes the lock if the holding process  */
/* crashes — prevents permanent deadlock on unexpected exit.          */
/* ------------------------------------------------------------------ */
void sem_lock(void);
void sem_unlock(void);

/* ------------------------------------------------------------------ */
/* Message queue operations                                             */
/* ------------------------------------------------------------------ */

/* msg_send  : non-blocking send (IPC_NOWAIT).
 *             Returns 0 on success, -1 on error.                     */
int msg_send(const Message *msg);

/* msg_recv  : receive a message of type mtype.
 *             block=1 → block until message available (blocking).    *
 *             block=0 → return immediately (IPC_NOWAIT).             *
 *             Returns bytes received (>0), 0 if no msg, -1 on error. */
int msg_recv(Message *msg, long mtype, int block);

/* msg_destroy: remove the message queue (called by ipc_destroy).     */
void msg_destroy(void);

/* ------------------------------------------------------------------ */
/* Utility helpers                                                      */
/* ------------------------------------------------------------------ */
const char *light_str(int state);       /* RED→"RED", etc.            */
const char *dir_str(int dir);           /* NORTH→"NORTH", etc.        */
const char *phase_str(int phase);       /* PHASE_NS_GREEN→"N-S GREEN" */

/* build_cmd_message: fill a Message ready to send to a traffic_light.
 * new_state  = straight/right signal (RED/YELLOW/GREEN)
 * left_state = left-turn arrow      (RED/YELLOW/GREEN) */
void build_cmd_message(Message *m, int direction, int new_state, int left_state);

#endif /* IPC_H */
