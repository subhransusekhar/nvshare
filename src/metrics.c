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
 * Prometheus text-format /metrics endpoint for nvshare-scheduler.
 *
 * Five metrics (M0):
 *   nvshare_lock_acquisitions_total  counter
 *   nvshare_early_releases_total     counter
 *   nvshare_clients_connected        gauge
 *   nvshare_lock_wait_seconds        histogram (le=0.1,1,10,60,+Inf)
 *   nvshare_lock_hold_seconds        histogram (same buckets)
 *
 * HTTP/1.0 server on a dedicated pthread — no library dependency.
 * Atomic counters; histogram sum kept in millis to avoid float atomics.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "metrics.h"

/* ------------------------------------------------------------------ */
/* Metric storage                                                       */
/* ------------------------------------------------------------------ */

static _Atomic uint64_t lock_acquisitions = 0;
static _Atomic uint64_t early_releases    = 0;
static _Atomic int64_t  clients_connected = 0;

/* Histograms: 4 finite buckets + +Inf, plus count and sum-in-millis. */
#define HIST_NBUCKETS 4
static const double bucket_le[HIST_NBUCKETS] = {0.1, 1.0, 10.0, 60.0};

struct hist {
    _Atomic uint64_t bucket[HIST_NBUCKETS + 1]; /* [0..3]=finite, [4]=+Inf */
    _Atomic uint64_t count;
    _Atomic int64_t  sum_millis;                 /* avoids float atomics */
};

static struct hist h_wait; /* nvshare_lock_wait_seconds */
static struct hist h_hold; /* nvshare_lock_hold_seconds */

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void metrics_init(void)
{
    atomic_store(&lock_acquisitions, 0);
    atomic_store(&early_releases, 0);
    atomic_store(&clients_connected, 0);

    for (int i = 0; i <= HIST_NBUCKETS; i++) {
        atomic_store(&h_wait.bucket[i], 0);
        atomic_store(&h_hold.bucket[i], 0);
    }
    atomic_store(&h_wait.count, 0);
    atomic_store(&h_wait.sum_millis, 0);
    atomic_store(&h_hold.count, 0);
    atomic_store(&h_hold.sum_millis, 0);
}

void metrics_inc_counter(const char *n)
{
    if (!strcmp(n, "lock_acquisitions_total"))
        atomic_fetch_add(&lock_acquisitions, 1);
    else if (!strcmp(n, "early_releases_total"))
        atomic_fetch_add(&early_releases, 1);
}

void metrics_observe_seconds(const char *n, double s)
{
    struct hist *h;

    if (!strcmp(n, "lock_wait_seconds"))
        h = &h_wait;
    else if (!strcmp(n, "lock_hold_seconds"))
        h = &h_hold;
    else
        return;

    atomic_fetch_add(&h->count, 1);
    atomic_fetch_add(&h->sum_millis, (int64_t)(s * 1000.0));

    /*
     * Cumulative bucket semantics (Prometheus convention):
     * a sample falls into every bucket whose le >= s.
     * The +Inf bucket receives every sample unconditionally.
     */
    for (int i = 0; i < HIST_NBUCKETS; i++) {
        if (s <= bucket_le[i])
            atomic_fetch_add(&h->bucket[i], 1);
    }
    atomic_fetch_add(&h->bucket[HIST_NBUCKETS], 1); /* +Inf */
}

void metrics_set_gauge(const char *n, double v)
{
    if (!strcmp(n, "clients_connected"))
        atomic_store(&clients_connected, (int64_t)v);
}

/* ------------------------------------------------------------------ */
/* Prometheus text-format renderer                                      */
/* ------------------------------------------------------------------ */

static int render_hist(char *buf, size_t cap, int off,
                       const char *name, struct hist *h)
{
    int n = off;
    static const char *le_str[HIST_NBUCKETS] = {"0.1", "1", "10", "60"};

    n += snprintf(buf + n, cap - n,
                  "# TYPE nvshare_%s histogram\n", name);

    for (int i = 0; i < HIST_NBUCKETS; i++) {
        n += snprintf(buf + n, cap - n,
                      "nvshare_%s_bucket{le=\"%s\"} %llu\n",
                      name, le_str[i],
                      (unsigned long long)atomic_load(&h->bucket[i]));
    }
    n += snprintf(buf + n, cap - n,
                  "nvshare_%s_bucket{le=\"+Inf\"} %llu\n",
                  name,
                  (unsigned long long)atomic_load(&h->bucket[HIST_NBUCKETS]));

    {
        int64_t sm = atomic_load(&h->sum_millis);
        double  sd = (double)sm / 1000.0;
        n += snprintf(buf + n, cap - n,
                      "nvshare_%s_sum %.3f\n", name, sd);
    }
    n += snprintf(buf + n, cap - n,
                  "nvshare_%s_count %llu\n",
                  name,
                  (unsigned long long)atomic_load(&h->count));

    return n;
}

static int render(char *buf, size_t cap)
{
    int n = 0;

    n += snprintf(buf + n, cap - n,
                  "# TYPE nvshare_lock_acquisitions_total counter\n"
                  "nvshare_lock_acquisitions_total %llu\n",
                  (unsigned long long)atomic_load(&lock_acquisitions));

    n += snprintf(buf + n, cap - n,
                  "# TYPE nvshare_early_releases_total counter\n"
                  "nvshare_early_releases_total %llu\n",
                  (unsigned long long)atomic_load(&early_releases));

    n += snprintf(buf + n, cap - n,
                  "# TYPE nvshare_clients_connected gauge\n"
                  "nvshare_clients_connected %lld\n",
                  (long long)atomic_load(&clients_connected));

    n = render_hist(buf, cap, n, "lock_wait_seconds", &h_wait);
    n = render_hist(buf, cap, n, "lock_hold_seconds", &h_hold);

    return n;
}

/* ------------------------------------------------------------------ */
/* Trivial HTTP/1.0 server                                              */
/* ------------------------------------------------------------------ */

static void *http_thread(void *arg)
{
    int port = (int)(intptr_t)arg;
    int srv, opt = 1;
    struct sockaddr_in addr;

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;

    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv);
        return NULL;
    }
    if (listen(srv, 8) < 0) {
        close(srv);
        return NULL;
    }

    for (;;) {
        char body[8192];
        char header[128];
        int  conn, bodylen, hdrlen;

        conn = accept(srv, NULL, NULL);
        if (conn < 0) continue;

        /* Drain the request — we don't care about the path. */
        {
            char req[512];
            (void)recv(conn, req, sizeof(req) - 1, 0);
        }

        bodylen = render(body, sizeof(body));
        hdrlen  = snprintf(header, sizeof(header),
                           "HTTP/1.0 200 OK\r\n"
                           "Content-Type: text/plain; version=0.0.4\r\n"
                           "Content-Length: %d\r\n"
                           "\r\n",
                           bodylen);

        (void)write(conn, header, (size_t)hdrlen);
        (void)write(conn, body,   (size_t)bodylen);
        close(conn);
    }

    return NULL;
}

void metrics_start_http_server(int port)
{
    pthread_t t;
    metrics_init();
    pthread_create(&t, NULL, http_thread, (void *)(intptr_t)port);
    pthread_detach(t);
}
