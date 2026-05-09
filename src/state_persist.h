#ifndef NVSHARE_STATE_PERSIST_H
#define NVSHARE_STATE_PERSIST_H
#include "comm.h"

#define STATE_PATH "/var/run/nvshare/state.json"

struct loaded_state {
    int      lock_held;
    int      tq;
    int      scheduler_on;
    unsigned scheduling_round;
    int      n_clients;
    struct {
        uint64_t id;
        char     pod_name[POD_NAME_LEN_MAX];
        char     pod_namespace[POD_NAMESPACE_LEN_MAX];
    } clients[64];   /* simple cap; expand if a node hosts more clients */
};

/* Caller must hold global_mutex. */
void nvshare_state_persist(void);
int  nvshare_state_load(struct loaded_state *out);
#endif
