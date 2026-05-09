/*
 * Copyright (c) 2023 Georgios Alexopoulos
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *
 * Atomic state persistence for the nvshare scheduler.
 *
 * On every state transition, writes a JSON snapshot to STATE_PATH using a
 * write-to-tmp + fsync + rename pattern so a crash mid-write never corrupts
 * the on-disk state.  Issue #10 fills in nvshare_state_load().
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include "state_persist.h"
#include "common.h"

/*
 * Scheduler globals; defined in scheduler.c.
 * Caller must hold global_mutex before calling nvshare_state_persist().
 */
extern int  lock_held;
extern int  tq;
extern int  scheduler_on;
extern unsigned int scheduling_round;

/* Forward-declare the client struct so we can walk the linked list. */
struct nvshare_client {
    int fd;
    uint64_t id;
    char pod_name[POD_NAME_LEN_MAX];
    char pod_namespace[POD_NAMESPACE_LEN_MAX];
    struct nvshare_client *next;
};
extern struct nvshare_client *clients;

extern pthread_mutex_t global_mutex;

/*
 * Atomically persist scheduler state to STATE_PATH.
 * Caller MUST hold global_mutex.
 */
void nvshare_state_persist(void)
{
    char tmp[256];
    int fd;
    FILE *f;
    int first;
    struct nvshare_client *c;

    snprintf(tmp, sizeof(tmp), "%s.tmp", STATE_PATH);

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        log_warn("state_persist: open(%s): %m", tmp);
        return;
    }

    f = fdopen(fd, "w");
    if (f == NULL) {
        log_warn("state_persist: fdopen: %m");
        close(fd);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"lock_held\": %d,\n", lock_held);
    fprintf(f, "  \"tq\": %d,\n", tq);
    fprintf(f, "  \"scheduler_on\": %d,\n", scheduler_on);
    fprintf(f, "  \"scheduling_round\": %u,\n", scheduling_round);
    fprintf(f, "  \"clients\": [");

    first = 1;
    for (c = clients; c != NULL; c = c->next) {
        fprintf(f, "%s\n    {\"id\": %llu, \"pod_name\": \"%s\","
                " \"pod_namespace\": \"%s\"}",
                first ? "" : ",",
                (unsigned long long)c->id,
                c->pod_name,
                c->pod_namespace);
        first = 0;
    }
    fprintf(f, "%s  ]\n}\n", first ? "" : "\n");

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp, STATE_PATH) != 0) {
        log_warn("state_persist: rename(%s -> %s): %m", tmp, STATE_PATH);
        unlink(tmp);
        return;
    }

    log_debug("state_persist: lock_held=%d scheduler_on=%d tq=%d"
              " scheduling_round=%u",
              lock_held, scheduler_on, tq, scheduling_round);
}

/*
 * Loader is the implementation of issue #10; stub kept here so the
 * header signature is satisfied and #10 only needs to fill in the parser.
 */
int nvshare_state_load(struct loaded_state *out)
{
    /* See issue #10 for the parser. */
    (void)out;
    return -1;
}
