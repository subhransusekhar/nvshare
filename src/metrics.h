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
 * Prometheus metrics — public API.
 */

#ifndef NVSHARE_METRICS_H
#define NVSHARE_METRICS_H

void metrics_init(void);
void metrics_inc_counter(const char *name);
void metrics_observe_seconds(const char *name, double seconds);
void metrics_set_gauge(const char *name, double value);
void metrics_start_http_server(int port);

#endif /* NVSHARE_METRICS_H */
