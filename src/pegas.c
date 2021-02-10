#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include <pthread.h>

#if defined(UNIX)
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/un.h>
#include <netinet/in.h>
#elif defined(WIN32)
#include <winsock2.h>
#define F_OK 0

int opterr = 1, /* if error message should be printed */
	optind = 1, /* index into parent argv vector */
	optopt, /* character checked for validity */
	optreset; /* reset getopt */
char *optarg; /* argument associated with option */

#define BADCH (int)'?'
#define BADARG (int)':'
#define EMSG ""

/*
* getopt --
*      Parse argc/argv argument vector.
*/
int getopt(int nargc, char *const nargv[], const char *ostr)
{
	static char *place = EMSG; /* option letter processing */
	const char *oli; /* option letter list index */

	if (optreset || !*place) { /* update scanning pointer */
		optreset = 0;
		if (optind >= nargc || *(place = nargv[optind]) != '-') {
			place = EMSG;
			return (-1);
		}
		if (place[1] && *++place == '-') { /* found "--" */
			++optind;
			place = EMSG;
			return (-1);
		}
	} /* option letter okay? */
	if ((optopt = (int)*place++) == (int)':' ||
	    !(oli = strchr(ostr, optopt))) {
		/*
      * if the user didn't specify '-' as an option,
      * assume it means -1.
      */
		if (optopt == (int)'-')
			return (-1);
		if (!*place)
			++optind;
		if (opterr && *ostr != ':')
			(void)printf("illegal option -- %c\n", optopt);
		return (BADCH);
	}
	if (*++oli != ':') { /* don't need argument */
		optarg = NULL;
		if (!*place)
			++optind;
	} else { /* need an argument */
		if (*place) /* no white space */
			optarg = place;
		else if (nargc <= ++optind) { /* no arg */
			place = EMSG;
			if (*ostr == ':')
				return (BADARG);
			if (opterr)
				(void)printf(
					"option requires an argument -- %c\n",
					optopt);
			return (BADCH);
		} else /* white space */
			optarg = nargv[optind];
		place = EMSG;
		++optind;
	}
	return (optopt); /* dump back option letter */
}
#endif

#include "pgs_local_server.h"
#include "pgs_config.h"
#include "pgs_server_manager.h"
#include "pgs_helper_thread.h"
#include "pgs_applet.h"

#define MAX_LOG_MPSC_SIZE 64
#define MAX_STATS_MPSC_SIZE 64

static void spawn_workers(pthread_t *threads, int server_threads,
			  pgs_local_server_ctx_t *ctx)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	// Local server threads
	for (int i = 0; i < server_threads; i++) {
		pthread_create(&threads[i], &attr, start_local_server,
			       (void *)ctx);
	}

	pthread_attr_destroy(&attr);
}

static void kill_workers(pthread_t *threads, int server_threads)
{
	for (int i = 0; i < server_threads; i++) {
		pthread_kill(threads[i], SIGINT);
	}
}

static int init_local_server_fd(const pgs_config_t *config, int *fd)
{
	int err = 0;
	struct sockaddr_in sin;
	int port = config->local_port;

	memset(&sin, 0, sizeof(sin));

	sin.sin_family = AF_INET;
	err = inet_pton(AF_INET, config->local_address, &sin.sin_addr);
	if (err <= 0) {
		if (err == 0)
			pgs_config_error(config, "Not in presentation format");
		else
			perror("inet_pton");
		exit(EXIT_FAILURE);
	}
	sin.sin_port = htons(port);

	*fd = socket(AF_INET, SOCK_STREAM, 0);
	int reuse_port = 1;

#if defined(UNIX)
	err = setsockopt(*fd, SOL_SOCKET, SO_REUSEPORT,
			 (const void *)&reuse_port, sizeof(int));
#elif defined(WIN32)
	err = setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR,
			 (const void *)&reuse_port, sizeof(int));
#endif

	if (err < 0) {
		perror("setsockopt");
		return err;
	}

#if defined(UNIX)
	int flag = fcntl(*fd, F_GETFL, 0);
	fcntl(*fd, F_SETFL, flag | O_NONBLOCK);
#elif defined(WIN32)
	u_long mode = 1; // 1 to enable non-blocking socket
	ioctlsocket(fd, FIONBIO, &mode);
#endif

	

	err = bind(*fd, (struct sockaddr *)&sin, sizeof(sin));

	if (err < 0) {
		perror("bind");
		return err;
	}
	return err;
}

static int init_control_fd(const pgs_config_t *config, int *fd)
{
	int err = 0;
	if (config->control_port) {
		// tcp port
		struct sockaddr_in sin;
		int port = config->control_port;

		memset(&sin, 0, sizeof(sin));

		sin.sin_family = AF_INET;
		err = inet_pton(AF_INET, config->local_address, &sin.sin_addr);
		if (err <= 0) {
			if (err == 0)
				pgs_config_error(config,
						 "Not in presentation format");
			else
				perror("inet_pton");
			exit(EXIT_FAILURE);
		}
		sin.sin_port = htons(port);

		*fd = socket(AF_INET, SOCK_STREAM, 0);

#if defined(UNIX)
		int flag = fcntl(*fd, F_GETFL, 0);
		fcntl(*fd, F_SETFL, flag | O_NONBLOCK);
#elif defined(WIN32)
		u_long mode = 1; // 1 to enable non-blocking socket
		ioctlsocket(fd, FIONBIO, &mode);
#endif

		err = bind(*fd, (struct sockaddr *)&sin, sizeof(sin));
		if (err < 0) {
			perror("bind");
			return err;
		}
	} else if (config->control_file) {
#if defined(UNIX)
		// unix socket
		struct sockaddr_un server;
		*fd = socket(AF_UNIX, SOCK_STREAM, 0);
		server.sun_family = AF_UNIX;
		strcpy(server.sun_path, config->control_file);
		int flag = fcntl(*fd, F_GETFL, 0);
		fcntl(*fd, F_SETFL, flag | O_NONBLOCK);
		unlink(config->control_file);
		err = bind(*fd, (struct sockaddr *)&server,
			   sizeof(struct sockaddr_un));
		if (err < 0) {
			perror("bind");
			return err;
		}
#endif
	}

	return err;
}

int main(int argc, char **argv)
{
#if defined(UNIX)
	signal(SIGPIPE, SIG_IGN);

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
#endif

#ifdef DEBUG_EVENT
	event_enable_debug_logging(EVENT_DBG_ALL);
#endif

	// default settings
	int server_threads = 4;
	char *config_path = NULL;

	// parse opt
	int opt = 0;
	while ((opt = getopt(argc, argv, "c:t:")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 't':
			server_threads = atoi(optarg);
			break;
		}
	}

	// get config path
	char full_config_path[512] = { 0 };
	char config_home[512] = { 0 };
	if (!config_path) {
		const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
		const char *home = getenv("HOME");
		if (!xdg_config_home || strlen(xdg_config_home) == 0) {
			sprintf(config_home, "%s/.config", home);
		} else {
			strcpy(config_home, xdg_config_home);
		}
		sprintf(full_config_path, "%s/.pegasrc", config_home);
		if (access(full_config_path, F_OK) == -1) {
			sprintf(full_config_path, "%s/pegas/config",
				config_home);
			if (access(full_config_path, F_OK) == -1) {
				fprintf(stderr, "config is required");
				return -1;
			}
		}
		config_path = full_config_path;
	}

	// load config
	pgs_config_t *config = pgs_config_load(config_path);
	if (config == NULL) {
		fprintf(stderr, "invalid config");
		return -1;
	}

	pgs_config_info(config, "worker threads: %d, config: %s",
			server_threads, config_path);

	int server_fd, ctrl_fd;
	if (init_local_server_fd(config, &server_fd) < 0) {
		perror("failed to init local server");
		return -1;
	}
	if (init_control_fd(config, &ctrl_fd) < 0) {
		perror("failed to init local server");
		return -1;
	}

	// mpsc with 64 message slots
	pgs_mpsc_t *mpsc = pgs_mpsc_new(MAX_LOG_MPSC_SIZE);
	pgs_mpsc_t *statsq = pgs_mpsc_new(MAX_STATS_MPSC_SIZE);
	// logger for logger server
	pgs_logger_t *logger =
		pgs_logger_new(mpsc, config->log_level, config->log_isatty);

	pgs_server_manager_t *sm = pgs_server_manager_new(
		statsq, config->servers, config->servers_count);

	pgs_local_server_ctx_t ctx = { server_fd, mpsc, config, sm };

	pgs_helper_thread_arg_t helper_thread_arg = { sm, logger, config,
						      ctrl_fd };

	// Spawn threads
#if defined(UNIX)
	pthread_t threads[server_threads + 1];
#elif defined(WIN32)
	pthread_t threads[4 + 1];
#endif
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	// Start helper thread
	pthread_create(&threads[0], &attr, pgs_helper_thread_start,
		       (void *)&helper_thread_arg);

	// Local server threads
	spawn_workers(threads + 1, server_threads, &ctx);

#ifdef WITH_APPLET
	pgs_tray_context_t tray_ctx = { logger,	       sm,
					threads + 1,   server_threads,
					spawn_workers, kill_workers,
					NULL };
	pgs_tray_start(&tray_ctx);
#endif

	// block on helper thread
	pthread_join(threads[0], NULL);

	pthread_attr_destroy(&attr);

	pgs_logger_free(logger);
	pgs_mpsc_free(mpsc);
	pgs_config_free(config);

	return 0;
}
