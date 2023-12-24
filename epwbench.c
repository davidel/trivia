/*    Copyright 2023 Davide Libenzi
 * 
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 * 
 *        http://www.apache.org/licenses/LICENSE-2.0
 * 
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 * 
 */


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>

#include <sys/epoll.h>


#define RUNTIMES 16
#define NAVG 6
#define NBATCH 16
#define MIN_TESTTIME (250ULL * 1000ULL)




static int *pipes;
static int num_pipes;
static int epfd;
static struct epoll_event *events;
static double samps[RUNTIMES];




static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);

	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

static int run_once(long *work, unsigned long long *tr) {
	int i, res;
	unsigned long long ts;

	*work = 0;
	ts = getustime();
	do {
		for (i = 0; i < NBATCH; i++) {
			res = epoll_wait(epfd, events, num_pipes, 0);
			if (res != num_pipes)
				fprintf(stderr, "error: got %d events instead of %d\n",
					res, num_pipes);
		}
		*work += i;
		*tr = getustime() - ts;
	} while (*tr < MIN_TESTTIME);

	return 0;
}

static int cmp_double(void const *p1, void const *p2) {
	double const *d1 = p1, *d2 = p2;

	return *d1 > *d2 ? 1: *d1 < *d2 ? -1: 0;
}

static void usage(char const *prg) {

	fprintf(stderr, "use: %s [-n NUMEVENTS] [-q] [-h]\n", prg);
}

int main (int argc, char **argv) {
	struct rlimit rl;
	int i, c, quiet = 0;
	long work;
	unsigned long long tr;
	double avg, sig, sv;
	int *cp;
	struct epoll_event ev;
	extern char *optarg;

	num_pipes = 200;
	while ((c = getopt(argc, argv, "n:qh")) != -1) {
		switch (c) {
		case 'n':
			num_pipes = atoi(optarg);
			break;
		case 'q':
			quiet++;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	rl.rlim_cur = rl.rlim_max = num_pipes * 2 + 50;
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
		perror("setrlimit");
		return 2;
	}

	events = calloc(num_pipes, sizeof(struct epoll_event));
	pipes = calloc(num_pipes * 2, sizeof(int));
	if (events == NULL || pipes == NULL) {
		perror("malloc");
		return 3;
	}

	if ((epfd = epoll_create(num_pipes)) == -1) {
		perror("epoll_create");
		return 4;
	}

	for (cp = pipes, i = 0; i < num_pipes; i++, cp += 2) {
		if (pipe(cp) == -1) {
			perror("pipe");
			return 5;
		}
	}
	for (cp = pipes, i = 0; i < num_pipes; i++, cp += 2) {
		ev.events = EPOLLIN;
		ev.data.u32 = i;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, cp[0], &ev) < 0) {
			perror("epoll_ctl");
			return 6;
		}
	}
	for (cp = pipes, i = 0; i < num_pipes; i++, cp += 2)
		write(cp[1], "e", 1);

	for (i = 0; i < RUNTIMES; i++) {
		run_once(&work, &tr);
		if (!work)
			return 7;
		samps[i] = (double) tr / (double) work;
		if (!quiet)
			fprintf(stdout, "%lf\n", samps[i]);
	}

	qsort(samps, RUNTIMES, sizeof(double), cmp_double);
	for (i = 0, avg = 0; i < NAVG; i++)
		avg += samps[i + RUNTIMES / 2 - NAVG / 2];
	avg /= NAVG;
	for (i = 0, sig = 0; i < NAVG; i++) {
		sv = avg - samps[i + RUNTIMES / 2 - NAVG / 2];
		sig += sv * sv;
	}
	sig = sqrt(sig / NAVG);

	fprintf(stdout,
		"AVG: %lf\n"
		"SIG: %lf\n", avg, sig);

	return 0;
}

