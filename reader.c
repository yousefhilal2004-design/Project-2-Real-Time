/*
 * reader.c  —  Anwar Atawna
 * -------------------------
 * Diagnostic tool: attaches to SHM and prints a snapshot every second.
 * Useful for checking the system state without the OpenGL UI.
 *
 * Compile: gcc -Wall reader.c ipc.c -o reader
 * Run:     ./reader      (the main system must already be running)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

int main(void) {
    SharedData *shm = ipc_attach();
    if (!shm) {
        fprintf(stderr, "[READER] ipc_attach failed — start ./main first\n");
        return 1;
    }
    printf("[READER] attached to SHM (press Ctrl+C to stop)\n\n");

    for (int i = 0; ; i++) {
        SharedData snap;

        /* Copy the whole struct under lock, then print after releasing */
        sem_lock();
        snap = *shm;
        sem_unlock();

        if (snap.shutdown) {
            printf("[READER] system shutdown detected\n");
            break;
        }

        printf("=== snapshot #%d ===\n", i + 1);
        printf("  Phase    : %s  (time_remaining=%ds)\n",
               phase_str(snap.current_phase), snap.phase_time_remaining);
        printf("  Lights   : N=%-7s S=%-7s E=%-7s W=%s\n",
               light_str(snap.light[NORTH]), light_str(snap.light[SOUTH]),
               light_str(snap.light[EAST]),  light_str(snap.light[WEST]));
        printf("  Vehicles : N=%d  S=%d  E=%d  W=%d\n",
               snap.vehicle_count[NORTH], snap.vehicle_count[SOUTH],
               snap.vehicle_count[EAST],  snap.vehicle_count[WEST]);
        printf("  Pedestrian: request=%d  active=%d\n",
               snap.pedestrian_request, snap.pedestrian_active);
        printf("  Emergency : mode=%d  dir=%s\n",
               snap.emergency_mode,
               snap.emergency_mode ? dir_str(snap.emergency_direction) : "N/A");
        if (snap.safety_violation)
            printf("  ** SAFETY VIOLATION: %s **\n", snap.safety_msg);
        printf("\n");
        fflush(stdout);

        sleep(1);
    }

    ipc_detach(shm);
    printf("[READER] done.\n");
    return 0;
}
