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
 * The nvshare scheduler.
 */

#include <dirent.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "comm.h"
#define NVSHARE_COMPONENT "scheduler"
#include "common.h"
#include "metrics.h"
#include "utlist.h"
#include "state_persist.h"

#define NVSHARE_DEFAULT_TQ 30

int lock_held;
time_t lock_acquired_at; /* wall-clock time when current holder got LOCK_OK */
char active_req_id[17];  /* req_id of the current lock holder; survives dequeue */
int must_reset_timer;
pthread_cond_t timer_cv;
int scheduler_on;
int tq;
unsigned int scheduling_round = 0;

struct message out_msg = {0};

char nvscheduler_socket_path[NVSHARE_SOCK_PATH_MAX];

pthread_mutex_t global_mutex;

/* File descriptor for epoll */
int epoll_fd;

/* Necessary information for identifying an nvshare client */
struct nvshare_client {
	int fd; /* server-side socket for the persistent connection */
	uint64_t id; /* Unique */
	char pod_name[POD_NAME_LEN_MAX];
	char pod_namespace[POD_NAMESPACE_LEN_MAX];
	time_t last_heartbeat_at; /* wall-clock seconds of last HEARTBEAT (or REGISTER) */
	struct nvshare_client *next;
};

/* Holds the requests for the GPU lock, which we serve in an FCFS manner */
struct nvshare_request {
	struct nvshare_client *client;
	time_t received_at; /* wall-clock time when REQ_LOCK arrived */
	char req_id[17];    /* 16-hex-char request ID from the client + NUL */
	struct nvshare_request *next;
};

struct nvshare_client *clients = NULL;
struct nvshare_request *requests = NULL;

/*
 * Expected-clients list: entries added at startup from state.json.
 * When a REGISTER arrives with a matching id, we reuse that id instead
 * of generating a new one, binding the new FD to the known identity.
 * Entries are singly-linked and freed on match or at startup cleanup.
 */
struct expected_client {
    uint64_t id;
    char pod_name[POD_NAME_LEN_MAX];
    struct expected_client *next;
};
static struct expected_client *expected_clients = NULL;

/*
 * Wall-clock time at which we loaded state.  Used to implement the
 * 10-second grace window for a lock-held resume: if no client claims
 * the lock within 10 s, we drop it back to FREE.
 */
static time_t resumed_at = 0;

static void expected_clients_add(uint64_t id, const char *pod_name)
{
    struct expected_client *e = malloc(sizeof(*e));
    if (!e) { log_warn("expected_clients_add: malloc failed"); return; }
    e->id = id;
    strlcpy(e->pod_name, pod_name, sizeof(e->pod_name));
    e->next = expected_clients;
    expected_clients = e;
}

/*
 * Look up and remove an entry by id from expected_clients.
 * Returns the matched entry (caller must free) or NULL if not found.
 */
static struct expected_client *expected_clients_take(uint64_t id)
{
    struct expected_client *e, *prev = NULL;
    for (e = expected_clients; e; prev = e, e = e->next) {
        if (e->id == id) {
            if (prev) prev->next = e->next;
            else expected_clients = e->next;
            e->next = NULL;
            return e;
        }
    }
    return NULL;
}

/* Returns 1 if any connected client currently holds the lock token. */
static int any_client_with_active_lock(void)
{
    struct nvshare_request *r;
    if (!lock_held) return 0;
    /* The lock holder is always the head of the requests list. */
    r = requests;
    return (r != NULL && r->client != NULL);
}

void *timer_thr_fn(void *arg __attribute__((unused)));
void *heartbeat_sweep_thr_fn(void *arg __attribute__((unused)));

static void bcast_status(void);
static int send_message(struct nvshare_client *client, struct message *msg_p);
static int receive_message(struct nvshare_client *client, struct message *msg_p);
static void try_schedule(void);
static int register_client(struct nvshare_client *client, const struct message *in_msg);
static int has_registered(struct nvshare_client *client);
static void client_id_as_string(char *buf, size_t buflen, uint64_t id);
static void delete_client(struct nvshare_client *client);
static void insert_req(struct nvshare_client *client, const char *req_id);
static void remove_req(struct nvshare_client *client);

static int has_registered(struct nvshare_client *client)
{
	return (client->id != NVSHARE_UNREGISTERED_ID);
}

/*
 * Count registered clients.  Caller must hold global_mutex.
 */
static int count_clients_locked(void)
{
	struct nvshare_client *c;
	int n = 0;
	LL_FOREACH(clients, c) {
		if (has_registered(c)) n++;
	}
	return n;
}

/* Print an nvshare client ID as a hex string */
static void client_id_as_string(char *buf, size_t buflen, uint64_t id)
{
	if (id == NVSHARE_UNREGISTERED_ID)
		strlcpy(buf, "<UNREGISTERED>", buflen);
	else snprintf(buf, buflen, "%016" PRIx64, id);
}

static void delete_client(struct nvshare_client *client)
{
	int cfd = client->fd;
	char id_str[HEX_STR_LEN(client->id)];
	struct nvshare_client *tmp, *c;


	client_id_as_string(id_str, sizeof(id_str), client->id);
	log_info("Removing client %s", id_str);
	remove_req(client);

	/* Remove from clients list */
	LL_FOREACH_SAFE(clients, c, tmp) {
		if (c->fd == client->fd) {
			LL_DELETE(clients, c);
			free(c);
		}
	}
	nvshare_state_persist(); /* callsite: delete_client */
	metrics_set_gauge("clients_connected", (double)count_clients_locked());

	true_or_exit(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cfd, NULL) == 0);
	/* See man close(2) for EINTR behavior on Linux */
	if (close(cfd) < 0 && errno != EINTR)
		log_fatal_errno("Failed to close FD %d", cfd);
}

static void insert_req(struct nvshare_client *client, const char *req_id)
{
	struct nvshare_request *r;
	LL_FOREACH(requests, r) {
		if (r->client->fd == client->fd) {
			log_warn("Client %016" PRIx64 " has already requested"
				 " the lock", r->client->id);
			return;
		}
	}
	true_or_exit(r = malloc(sizeof *r));
	r->next = NULL;
	r->client = client;
	r->received_at = time(NULL);
	memcpy(r->req_id, req_id, 16);
	r->req_id[16] = 0;
	LL_APPEND(requests, r);
}

static void remove_req(struct nvshare_client *client)
{
	struct nvshare_request *tmp, *r;
	if (requests != NULL) {
		/*
		 * This client was holding the GPU lock, as it was the head of
		 * the requests list.
		 */
		if (requests->client->fd == client->fd) {
			if (lock_acquired_at != 0) {
				double hold_s = difftime(time(NULL), lock_acquired_at);
				metrics_observe_seconds("lock_hold_seconds", hold_s);
				lock_acquired_at = 0;
			}
			lock_held = 0;
			nvshare_state_persist(); /* callsite: remove_req (lock release) */
		}
	}
	LL_FOREACH_SAFE(requests, r, tmp) {
		if (r->client->fd == client->fd) {
			LL_DELETE(requests, r);
			free(r);
		}
	}
}



static int register_client(struct nvshare_client *client, const struct message *in_msg)
{
	int ret;
	struct nvshare_client *c;
	uint64_t nvshare_client_id;
	struct expected_client *known = NULL;

	if (has_registered(client)) {
		log_warn("Client %016" PRIx64 " is already registered",
			 client->id);
		return -1;
	}

	/*
	 * If the connecting client presents a known id (from a previous
	 * scheduler incarnation), reuse that id and rebind this FD to it.
	 * This is the reconnect path after a scheduler restart.
	 */
	if (in_msg->id != NVSHARE_UNREGISTERED_ID && in_msg->id != 0)
		known = expected_clients_take(in_msg->id);

	if (known) {
		nvshare_client_id = known->id;
		free(known);
		log_info("register_client: reconnect — reusing id %016" PRIx64,
			 nvshare_client_id);
	} else {
		/* New client — generate a fresh id. */
again:
		nvshare_client_id = nvshare_generate_id();
		if (nvshare_client_id == NVSHARE_UNREGISTERED_ID) /* Tough luck */
			goto again;
		LL_FOREACH(clients, c) {
			if (c->id == nvshare_client_id) { /* ID clash */
				goto again;
			}
		}
	}

	/*
	 * Store the rest of the client information.
	 */
	client->id = nvshare_client_id;
	strlcpy(client->pod_name, in_msg->pod_name,
		sizeof(client->pod_name));
	strlcpy(client->pod_namespace, in_msg->pod_namespace,
		sizeof(client->pod_namespace));
	client->last_heartbeat_at = time(NULL);

	/*
	 * Inform the client of the current status of our current status, as
	 * well as the ID we generated for it.
	 *
	 * It will henceforth present this ID to interact with us.
	 */
	true_or_exit(snprintf(out_msg.data, 16+1, "%016" PRIx64, nvshare_client_id) == 16);
	out_msg.type = scheduler_on ? SCHED_ON : SCHED_OFF;
	if ((ret = send_message(client, &out_msg)) < 0)
		goto out_with_msg;
	nvshare_state_persist(); /* callsite: register_client */
	metrics_set_gauge("clients_connected", (double)count_clients_locked());

out_with_msg:
	/* out_msg is global, so make sure we've zeroed it out */
	memset(&out_msg.data, 0, sizeof(out_msg.data));

	return ret;
}


static void bcast_status(void)
{
	struct nvshare_client *tmp, *c;
	LL_FOREACH_SAFE(clients, c, tmp) {
		if (!has_registered(c)) continue;

		out_msg.type = scheduler_on ? SCHED_ON : SCHED_OFF;
		if (send_message(c, &out_msg) < 0)
			delete_client(c);
	}
}


/*
 * Send a given message to a given client.
 *
 * We are particularly strict and consider the client dead if we encounter any
 * (even possibly recoverable if we were more lenient) error.
 */
static int send_message(struct nvshare_client *client, struct message *msg_p)
{
	ssize_t ret;
	char id_str[HEX_STR_LEN(client->id)];

	client_id_as_string(id_str, sizeof(id_str), client->id);

	ret = nvshare_send_noblock(client->fd, msg_p, sizeof(*msg_p));

	if (ret >= 0 && (size_t)ret < sizeof(*msg_p))/* Partial send */
		return -1;
	else if (ret < 0) {
		if (errno == EAGAIN ||
		    errno == EWOULDBLOCK ||
		    errno == ECONNRESET ||
		    errno == EPIPE) { /* Recoverable errors, but we're strict */
			log_info("Failed to send message to client %s",
				 id_str);
			return -1;
		} else log_fatal("nvshare_send_noblock() failed unrecoverably");
	} else { /* ret == 0 */
		log_info("Sent %s to client %s",
		         message_type_string[msg_p->type], id_str);
	}
	return 0;
}

/*
 * Send a given message to a given client.
 *
 * We are particularly strict and consider the client dead if we encounter any
 * (even possibly recoverable if we were more lenient) error.
 */
static int receive_message(struct nvshare_client *client, struct message *msg_p)
{
	ssize_t ret;
	char id_str[HEX_STR_LEN(client->id)];

	client_id_as_string(id_str, sizeof(id_str), client->id);

	ret = nvshare_receive_noblock(client->fd, msg_p, sizeof(*msg_p));

	if (ret == 0) { /* Client closed the other end of the connection */
		errno = ENOTCONN;
		log_debug("Client %s has closed the connection", id_str);
		return -1;
	} else if (ret > 0 && (size_t)ret < sizeof(*msg_p)) { /* Partial receive */
		return -1;
	} else if (ret < 0) {
		if (errno == EAGAIN ||
		    errno == EWOULDBLOCK ||
		    errno == ECONNRESET ||
		    errno == EPIPE) {
			log_info("Failed to receive message from client %s",
				 id_str);
			return -1;
		} else log_fatal("nvshare_receive_noblock() failed unrecoverably");
	}
	return 0;
}

/*
 * Try to assign the GPU lock to a client in the requests list in FCFS order.
 *
 * Return only on successful assignment of GPU lock to a client or if the
 * requests list is empty.
 */
static void try_schedule(void)
{
	int ret;

	/*
	 * Lock-held grace window: if we resumed from state.json with
	 * lock_held=1 but no client has re-REGISTERed to claim that lock
	 * within 10 seconds, drop lock_held back to FREE so the scheduler
	 * doesn't stall indefinitely.
	 */
	if (lock_held && resumed_at != 0 && !any_client_with_active_lock()) {
		if (time(NULL) - resumed_at > 10) {
			log_warn("try_schedule: lock_held grace window expired;"
				 " no client reclaimed lock — dropping to FREE");
			lock_held = 0;
			resumed_at = 0;
			nvshare_state_persist(); /* callsite: grace-window expiry */
		}
	}

try_again:
	if (requests == NULL) {
		log_debug("try_schedule() called with no pending requests");
		return;
	} else {
		out_msg.type = LOCK_OK;
		/* Echo req_id back to client in LOCK_OK data field */
		memcpy(out_msg.data, requests->req_id, 16);
		out_msg.data[16] = 0;
		out_msg.data[17] = 0;
		out_msg.data[18] = 0;
		out_msg.data[19] = 0;
		/* Stash req_id so timer_thr_fn can use it after the request is dequeued */
		memcpy(active_req_id, requests->req_id, 17);
		/* FCFS, use head of requests list */
		ret = send_message(requests->client, &out_msg);
		if (ret < 0) { /* Client's dead to us */
			memset(out_msg.data, 0, sizeof(out_msg.data));
			delete_client(requests->client);
			goto try_again;
		}
		memset(out_msg.data, 0, sizeof(out_msg.data));
		scheduling_round++;
		lock_held = 1;
		lock_acquired_at = time(NULL);
		{
			double wait_s = (requests->received_at != 0)
			    ? difftime(lock_acquired_at, requests->received_at)
			    : 0.0;
			metrics_inc_counter("lock_acquisitions_total");
			metrics_observe_seconds("lock_wait_seconds", wait_s);
		}
		log_event("info", "lock_granted", "req_id=%s pod=%s wait_ms=%ld",
			  active_req_id, requests->client->pod_name, (long)-1);
		nvshare_state_persist(); /* callsite: try_schedule (lock grant) */
		must_reset_timer = 1;
		pthread_cond_broadcast(&timer_cv);
	}
}


/*
 * The timer thread's sole responsibility is to implement the Time Quantum (TQ)
 * notion of nvshare.
 *
 * When a client obtains the GPU lock, the timer resets.
 *
 * When TQ elapses, it sends a DROP_LOCK message to the client that holds the
 * lock.
 */

void *timer_thr_fn(void *arg __attribute__((unused)))
{
	struct message t_msg = {0};
	unsigned int round_at_start;
	struct timespec timer_end_ts = {0, 0};
	int ret;
	int drop_lock_sent = 0;

	t_msg.id = 1337; /* Nobody checks this */
	t_msg.type = DROP_LOCK;

	true_or_exit(pthread_mutex_lock(&global_mutex) == 0);
	while (1) {
		must_reset_timer = 0;
		round_at_start = scheduling_round;
		true_or_exit(clock_gettime(CLOCK_REALTIME, &timer_end_ts) == 0);
		timer_end_ts.tv_sec += tq;
remainder:
		ret = pthread_cond_timedwait(&timer_cv, &global_mutex, &timer_end_ts);
		/* Wake up with global_mutex held, can do whatever we want */
		if (ret == ETIMEDOUT) { /* TQ elapsed */
			log_debug("TQ elapsed");
			if (!lock_held) continue; /* Life is meaningless :( */
			if (drop_lock_sent) continue; /* Send it only once */
			/*
			 * We use round_at_stat and scheduling_round to enable
			 * us to uniquely order (and by extent identify) every
			 * binding of the GPU lock to a client.
			 *
			 * This ensures we can avoid race conditions in which
			 * the timer wakes up and the lock has changed hands,
			 * thus erroneously sending a DROP_LOCK to the wrong
			 * client.
			 */
			if (round_at_start != scheduling_round) {
				drop_lock_sent = 0;
				continue;
			}
			/*
			 * Strict handling of clients. If something goes wrong,
			 * clean them up.
			 */
			/* Echo req_id in DROP_LOCK so client can correlate */
			memcpy(t_msg.data, active_req_id, 16);
			t_msg.data[16] = 0;
			t_msg.data[17] = 0;
			t_msg.data[18] = 0;
			t_msg.data[19] = 0;
			if (send_message(requests->client, &t_msg) < 0) {
				memset(t_msg.data, 0, sizeof(t_msg.data));
				delete_client(requests->client);
				try_schedule();
				drop_lock_sent = 0;
			} else { /* All good */
				log_event("info", "drop_lock",
					  "req_id=%s pod=%s reason=%s",
					  active_req_id,
					  requests->client->pod_name,
					  "tq_expired");
				memset(t_msg.data, 0, sizeof(t_msg.data));
				drop_lock_sent = 1;
			}
		} else if (ret != 0) { /* Unrecoverable error */
			errno = ret;
			log_fatal("pthread_cond_timedwait()");
		} else { /* ret == 0, someone signaled the condvar */
			if (must_reset_timer) {
				drop_lock_sent = 0;
				continue;
			} else { /* Spurious wakeup */
				goto remainder;
			}
		}
	}
}


/*
 * Heartbeat sweep thread.
 *
 * Runs every 10 seconds.  Any registered client that has not sent a HEARTBEAT
 * (or REGISTER) within the last 30 seconds is considered a zombie and is
 * cleaned up via delete_client(), which removes the client from the list,
 * releases any lock it held, and persists scheduler state.
 */
void *heartbeat_sweep_thr_fn(void *arg __attribute__((unused)))
{
	struct nvshare_client *c, *next;
	time_t now;

	while (1) {
		sleep(10);

		now = time(NULL);
		true_or_exit(pthread_mutex_lock(&global_mutex) == 0);
		for (c = clients; c; c = next) {
			next = c->next;
			if (!has_registered(c)) continue;
			if (now - c->last_heartbeat_at > 30) {
				log_warn("[scheduler] HEARTBEAT timeout: dropping client %016" PRIx64,
					 c->id);
				log_event("warn", "heartbeat_timeout",
					  "id=%016" PRIx64 " age_s=%d",
					  c->id, (int)(now - c->last_heartbeat_at));
				delete_client(c);
				if (!lock_held && scheduler_on)
					try_schedule();
			}
		}
		true_or_exit(pthread_mutex_unlock(&global_mutex) == 0);
	}
	return NULL;
}


static void process_msg(struct nvshare_client *client, const struct message *in_msg)
{
	int newtq;
	char id_str[HEX_STR_LEN(client->id)];
	char *endptr;

	client_id_as_string(id_str, sizeof(id_str), client->id);

	switch (in_msg->type) {
	case REGISTER:
		log_info("Received %s",
			   message_type_string[in_msg->type]);

		if (register_client(client, in_msg) < 0) delete_client(client);
		else log_info("Registered client %016" PRIx64 " with Pod"
			      " name = %s, Pod namespace = %s", client->id,
			      client->pod_name, client->pod_namespace);
		break;

	case SCHED_ON: /* nvsharectl */
		log_info("Received %s from %s",
		   	 message_type_string[in_msg->type], id_str);

		/*
		 * Ensure status actually changed before broadcasting,
		 * otherwise it is a no-op.
		 */
		if (!scheduler_on) {
			scheduler_on = 1;
			log_info("Scheduler turned ON, broadcasting it...");
			bcast_status();
			nvshare_state_persist(); /* callsite: process_msg SCHED_ON */
		}
		break;

	case SCHED_OFF: /* nvsharectl */
		log_info("Received %s from %s",
			 message_type_string[in_msg->type], id_str);

		if (scheduler_on) {
			log_info("Scheduler turned OFF, broadcasting it...");
			scheduler_on = 0;
			bcast_status();
			/*
			 * When the scheduler is OFF, every client thinks they
			 * have the lock, so the requests list instantaneously
			 * becomes invalid. Empty it.
			 */
			struct nvshare_request *tmp, *r;
			LL_FOREACH_SAFE(requests, r, tmp) {
				LL_DELETE(requests, r);
				free(r);
			}
			lock_held = 0;
			nvshare_state_persist(); /* callsite: process_msg SCHED_OFF */
		}
		break;

	case SET_TQ: /* nvsharectl */
		log_info("Received %s from %s",
			 message_type_string[in_msg->type], id_str);

		errno = 0;
		newtq = (int)strtoll(in_msg->data, &endptr, 0);
        	if (in_msg->data != endptr && *endptr == '\0' && errno == 0) {
			tq = newtq;
			nvshare_state_persist(); /* callsite: process_msg SET_TQ */
			must_reset_timer = 1;
			pthread_cond_broadcast(&timer_cv); /* Reset timer on TQ change */
			log_info("New TQ = %d", tq);
		}
		else log_info("Failed to parse new TQ from message");
		break;

	case REQ_LOCK: /* client */
		log_info("Received %s from %s",
			 message_type_string[in_msg->type], id_str);
		{
			/* Extract the 16-hex req_id from the message data field */
			char req_id[17];
			memcpy(req_id, in_msg->data, 16);
			req_id[16] = 0;
			log_event("info", "req_lock_received", "req_id=%s pod=%s",
				  req_id, client->pod_name);

			if (has_registered(client)) {
				if (scheduler_on) {
					insert_req(client, req_id);
					if (!lock_held) try_schedule();
				}
			} else { /* The client is not registered. Slam the door. */
				delete_client(client);
			}
		}
		break;

	case LOCK_RELEASED: /* From client */
		log_info("Received %s from %s",
			 message_type_string[in_msg->type], id_str);
		{
			/* Client echoes back the req_id it received; use it for correlation */
			char req_id[17];
			memcpy(req_id, in_msg->data, 16);
			req_id[16] = 0;
			log_event("info", "lock_released", "req_id=%s pod=%s hold_ms=%ld",
				  req_id, client->pod_name, (long)-1);

			if (has_registered(client)) {
				/*
				 * When the scheduler is OFF, LOCK_RELEASED messages
				 * are meaningless. Mostly a sanity check.
				 */
				if (scheduler_on) {
					/* Early release: client voluntarily returned lock before TQ. */
					metrics_inc_counter("early_releases_total");
					remove_req(client);
					if (!lock_held) try_schedule();
				}
			} else { /* The client is not registered. Slam the door. */
				delete_client(client);
			}
		}
		break;

	case HEARTBEAT: /* client liveness ping — one-way, no reply */
		log_debug("Received %s from %s",
			  message_type_string[in_msg->type], id_str);

		client->last_heartbeat_at = time(NULL);
		break;

	default: /* Unknown message type */
		log_info("Received message of unknown type %d"
			 " from %s", (int)in_msg->type, id_str);
		break;
	}
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	pthread_t timer_tid;
	pthread_t sweep_tid;
	struct nvshare_client *client;
	int ret, err, lsock, rsock, num_fds;
	char *debug_val;
	struct message in_msg = {0};
	struct epoll_event event, events[EPOLL_MAX_EVENTS];

	debug_val = getenv(ENV_NVSHARE_DEBUG);
	if (debug_val != NULL) {
		__debug = 1;
		log_info("nvshare-scheduler started in debug mode");
	} else log_info("nvshare-scheduler started in normal mode");

	/*
	 * Permissions are 711:
	 * - RWX (7) for owner (root because we are under /var/run/)
	 * - X (1) for group (to connect to the socket)
	 * - X (1) for others (to connect to the socket)
	 *
	 * In general, directory permissions work as follows:
	 *
	 * - `r`: Can read the names of files in the directory
	 * - `w`:
	 *    + if also `x`: can create, rename, delete files in the directory
	 *    + if not `x`: nothing
	 * - `x`:
	 *    1. Can access files (read/write to them if individual file
	 *       permissions permit) in the directory
	 *    2. Can read meta-information (permissions, timestamps, size)
	 *       about a file if the file name is known
	 */
	err = mkdir(NVSHARE_SOCK_DIR, S_IRWXU | S_IXGRP | S_IXOTH);
	if (err != 0 && errno != EEXIST)
		log_fatal("Could not create scheduler socket directory %s",
			  NVSHARE_SOCK_DIR);

	/*
	 * Unconditionally call chmod(), as it is not affected by umask, to
	 * ensure that the directory has the correct (aforementioned) 711
	 * permissions.
	 */
	if (chmod(NVSHARE_SOCK_DIR, S_IRWXU | S_IXGRP | S_IXOTH) != 0)
		log_fatal("chmod() failed for %s", NVSHARE_SOCK_DIR);

	/* TODO: Enable setting this dynamically through an envvar/conffile */
	scheduler_on = 1;
	/* TODO: Enable setting this dynamically through an envvar/conffile */
	tq = NVSHARE_DEFAULT_TQ;

	/* Seed srand() for generating client IDs */
	srand((unsigned int)(time(NULL)));

	true_or_exit(pthread_mutex_init(&global_mutex, NULL) == 0);
	true_or_exit(pthread_cond_init(&timer_cv, NULL) == 0);

	if (nvshare_get_scheduler_path(nvscheduler_socket_path) != 0)
		log_fatal("nvshare_get_scheduler_path() failed!");

	/* Spawn the timer thread */
	true_or_exit(pthread_create(&timer_tid, NULL, timer_thr_fn, NULL) == 0);

	/* Spawn the heartbeat sweep thread */
	true_or_exit(pthread_create(&sweep_tid, NULL, heartbeat_sweep_thr_fn, NULL) == 0);

	/* Set up fd for epoll */
	true_or_exit((epoll_fd = epoll_create(1)) >= 0);

	/* Start listening */
	true_or_exit(nvshare_bind_and_listen(&lsock, nvscheduler_socket_path) == 0);

	/* Start Prometheus metrics HTTP server on port 9601 */
	metrics_start_http_server(9601);

	/* Use the 'fd' field of epoll_data for the listening socket events */
	event.data.fd = lsock;
	event.events = EPOLLIN;
	true_or_exit(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, lsock, &event) == 0);

	/*
	 * According to man unix(7):
	 * On Linux, connecting to a stream socket object requires write
	 * permission on that socket; [...]
	 *
	 * We also need execute permission for the socket directory, to access
	 * the socket file that lies therein.
	 *
	 * Therefore, the minimal permissions for the socket file are 722.
	 */
	if (chmod(nvscheduler_socket_path, S_IRWXU | S_IWGRP| S_IWOTH) != 0)
		log_fatal("chmod() failed for %s", nvscheduler_socket_path);

	out_msg.id = 7331;

	/*
	 * Attempt to resume from persisted state (issue #10).
	 * Must happen after the listen socket is up so that clients
	 * re-connecting immediately after the scheduler restarts are
	 * accepted into the epoll loop.
	 */
	{
		struct loaded_state ls = {0};
		if (nvshare_state_load(&ls) == 0) {
			lock_held        = ls.lock_held;
			tq               = ls.tq;
			scheduler_on     = ls.scheduler_on;
			scheduling_round = ls.scheduling_round;
			for (int i = 0; i < ls.n_clients; i++)
				expected_clients_add(ls.clients[i].id,
						     ls.clients[i].pod_name);
			if (lock_held)
				resumed_at = time(NULL);
			log_debug("scheduler: resumed from state: lock_held=%d"
				  " clients_expected=%d", lock_held, ls.n_clients);
			log_info("scheduler: resumed from state.json — %d client(s) expected",
				 ls.n_clients);
			log_event("info", "scheduler_resumed",
				  "lock_held=%d clients_expected=%d",
				  lock_held, ls.n_clients);
		}
	}

	log_info("nvshare-scheduler listening on %s",
		 nvscheduler_socket_path);

	for (;;) {
		num_fds = RETRY_INTR(epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, -1));

		/* Since we use an infinite timeout, a non-zero return value
		 * indicates an error
		 */
		if (num_fds < 0) log_fatal("epoll_wait() failed");

		/* ret > 0, we got an event */

		true_or_exit(pthread_mutex_lock(&global_mutex) == 0);

		for (int i = 0; i < num_fds; i++) {
			if (events[i].data.fd == lsock) {
				/* New connection. */
				ret = nvshare_accept(events[i].data.fd, &rsock);
				if (ret == 0) { /* OK */
					/* 1. Set up the client struct */
					client = malloc(sizeof(*client));
					client->fd = rsock;
					client->id = NVSHARE_UNREGISTERED_ID;
					client->next = NULL;

					/*
					 * 2. Add new rsock to the epoll
					 *    interest list.
					 *
					 * The epoll_data field of epoll_event
					 * is a union, and we can only use one
					 * field at a time.
					 *
					 * In this case, use void *ptr.
					 *
					 * See man epoll_ctl(2).
					 * */
					event.data.ptr = client;
					event.events = EPOLLIN;
					if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, rsock, &event) < 0) {
						log_warn("Couldn't add %d to the epoll interest list", rsock);
						close(rsock);
						free(client);
					} else LL_APPEND(clients, client); /* OK */
				} else if (errno != ECONNABORTED && errno != EAGAIN && errno != EWOULDBLOCK)
					log_fatal("accept() failed non-transiently");

			} else { /* Some event other than new connection */
				client = (struct nvshare_client *)events[i].data.ptr;

				/* Check for incoming messages */
				if (events[i].events & EPOLLIN) {
					ret = receive_message(client, &in_msg);
					if (ret < 0) {
						delete_client(client);
						if (!lock_held && scheduler_on) try_schedule();
					}
					else process_msg(client, &in_msg); /* OK */

				}
				/*
				 * Check for errors after checking for messages,
				 * as nvsharectl sends a message and immediately
				 * closes a connection.
				 */
				else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
					delete_client(client);
					if (!lock_held && scheduler_on)
						try_schedule();
				}

			}
		}
		true_or_exit(pthread_mutex_unlock(&global_mutex) == 0);
	}

	/* Control should never reach here */
	return -1;
}

