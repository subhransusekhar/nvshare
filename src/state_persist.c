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
 * Hand-rolled JSON parser for the state written by nvshare_state_persist().
 *
 * The schema is fixed and fields are written in a known order:
 *   version, lock_held, tq, scheduler_on, scheduling_round, clients[]
 *
 * Parsing strategy: advance a char* cursor through the buffer, using
 * strstr() to locate each key, then sscanf / strtoul to extract values.
 * No recursive descent needed — the structure has only one level of nesting
 * (the clients array contains flat objects).
 *
 * Returns 0 on success, -1 on any parse error or missing file.
 * On -1, *out is zeroed.
 */
int nvshare_state_load(struct loaded_state *out)
{
    FILE *f;
    char buf[65536]; /* ample for 64 clients */
    size_t n;
    char *p, *q;
    int version;
    int i;

    memset(out, 0, sizeof(*out));

    f = fopen(STATE_PATH, "r");
    if (f == NULL) {
        /* Missing file is normal on first start */
        return -1;
    }

    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return -1;
    buf[n] = '\0';

    /* --- version --- */
    p = strstr(buf, "\"version\"");
    if (!p) goto bad;
    p = strchr(p, ':');
    if (!p) goto bad;
    if (sscanf(p + 1, " %d", &version) != 1) goto bad;
    if (version != 1) {
        log_warn("state_persist: unsupported state version %d", version);
        goto bad;
    }

    /* --- lock_held --- */
    p = strstr(buf, "\"lock_held\"");
    if (!p) goto bad;
    p = strchr(p, ':');
    if (!p) goto bad;
    if (sscanf(p + 1, " %d", &out->lock_held) != 1) goto bad;

    /* --- tq --- */
    p = strstr(buf, "\"tq\"");
    if (!p) goto bad;
    p = strchr(p, ':');
    if (!p) goto bad;
    if (sscanf(p + 1, " %d", &out->tq) != 1) goto bad;

    /* --- scheduler_on --- */
    p = strstr(buf, "\"scheduler_on\"");
    if (!p) goto bad;
    p = strchr(p, ':');
    if (!p) goto bad;
    if (sscanf(p + 1, " %d", &out->scheduler_on) != 1) goto bad;

    /* --- scheduling_round --- */
    p = strstr(buf, "\"scheduling_round\"");
    if (!p) goto bad;
    p = strchr(p, ':');
    if (!p) goto bad;
    if (sscanf(p + 1, " %u", &out->scheduling_round) != 1) goto bad;

    /* --- clients array --- */
    p = strstr(buf, "\"clients\"");
    if (!p) goto bad;
    p = strchr(p, '[');
    if (!p) goto bad;

    i = 0;
    while (i < 64) {
        /* Find next object opening brace within the array */
        q = strchr(p + 1, '{');
        if (!q) break; /* end of array */

        /* Check we haven't wandered outside the array */
        {
            char *arr_end = strchr(p + 1, ']');
            if (arr_end && arr_end < q) break;
        }
        p = q;

        /* id — stored as decimal uint64 in the JSON */
        q = strstr(p, "\"id\"");
        if (!q) break;
        q = strchr(q, ':');
        if (!q) break;
        {
            unsigned long long tmp_id;
            if (sscanf(q + 1, " %llu", &tmp_id) != 1) goto bad;
            out->clients[i].id = (uint64_t)tmp_id;
        }

        /* pod_name */
        q = strstr(p, "\"pod_name\"");
        if (!q) break;
        q = strchr(q, ':');
        if (!q) break;
        q = strchr(q, '"');
        if (!q) break;
        q++; /* skip opening quote */
        {
            char *end = strchr(q, '"');
            if (!end) goto bad;
            size_t len = (size_t)(end - q);
            if (len >= POD_NAME_LEN_MAX) len = POD_NAME_LEN_MAX - 1;
            memcpy(out->clients[i].pod_name, q, len);
            out->clients[i].pod_name[len] = '\0';
            p = end; /* advance cursor past pod_name value */
        }

        /* pod_namespace */
        q = strstr(p, "\"pod_namespace\"");
        if (!q) break;
        q = strchr(q, ':');
        if (!q) break;
        q = strchr(q, '"');
        if (!q) break;
        q++; /* skip opening quote */
        {
            char *end = strchr(q, '"');
            if (!end) goto bad;
            size_t len = (size_t)(end - q);
            if (len >= POD_NAMESPACE_LEN_MAX) len = POD_NAMESPACE_LEN_MAX - 1;
            memcpy(out->clients[i].pod_namespace, q, len);
            out->clients[i].pod_namespace[len] = '\0';
            p = end;
        }

        i++;
    }

    out->n_clients = i;
    if (out->n_clients == 64)
        log_warn("state_persist: loaded max 64 clients; any beyond that were truncated");

    log_debug("state_persist: loaded version=%d lock_held=%d tq=%d"
              " scheduler_on=%d scheduling_round=%u n_clients=%d",
              version, out->lock_held, out->tq, out->scheduler_on,
              out->scheduling_round, out->n_clients);
    return 0;

bad:
    log_warn("state_persist: failed to parse %s — starting fresh", STATE_PATH);
    memset(out, 0, sizeof(*out));
    return -1;
}
