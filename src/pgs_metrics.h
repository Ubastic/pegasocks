#ifndef _PGS_METRICS
#define _PGS_METRICS

#include "pgs_server_manager.h"
#include "pgs_session.h"
#include "pgs_codec.h"

typedef struct pgs_metrics_task_ctx_s pgs_metrics_task_ctx_t;

struct pgs_metrics_task_ctx_s {
	struct event_base *base;
	struct evdns_base *dns_base;
	pgs_server_manager_t *sm;
	int server_idx;
	pgs_logger_t *logger;
	pgs_session_outbound_t *outbound;
	struct timeval start_at;
};

void get_metrics_g204_connect(struct event_base *base, pgs_server_manager_t *sm,
			      int idx, pgs_logger_t *logger);

pgs_metrics_task_ctx_t *
pgs_metrics_task_ctx_new(struct event_base *base, pgs_server_manager_t *sm,
			 int idx, pgs_logger_t *logger,
			 pgs_session_outbound_t *outbound);
void pgs_metrics_task_ctx_free(pgs_metrics_task_ctx_t *ptr);

#endif
