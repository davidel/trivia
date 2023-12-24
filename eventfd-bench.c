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
#include <sys/syscall.h>
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

/*
 * This were good at the time of 2.6.21-rc5.mm4 ...
 */
#ifndef __NR_eventfd
#if defined(__x86_64__)
#define __NR_eventfd 283
#elif defined(__i386__)
#define __NR_eventfd 323
#else
#error Cannot detect your architecture!
#endif
#endif



static int count, writes, fired;
static int *evfds;
static int num_evfds, num_active, num_writes;
static int epfd;
static struct epoll_event *events;
static double samps[RUNTIMES];



static int eventfd(int count) {

	return syscall(__NR_eventfd, count);
}

static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);

	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

static void read_cb(int fd, int idx) {
	int widx = idx + num_active + 1;
	u_int64_t cntr;

	if (read(fd, &cntr, sizeof(cntr)) == sizeof(cntr))
		count++;
	else
		fprintf(stderr, "false read event: fd=%d idx=%d\n", fd, idx);
	if (writes) {
		if (widx >= num_evfds)
			widx -= num_evfds;
		cntr = 1;
		write(evfds[widx], &cntr, sizeof(cntr));
		writes--;
		fired++;
	}
}

static int run_once(long *work, unsigned long long *tr) {
	int i, res;
	unsigned long long ts, te;
	u_int64_t cntr = 1;

	fired = 0;
	for (i = 0; i < num_active; i++, fired++)
		write(evfds[i], &cntr, sizeof(cntr));

	count = 0;
	writes = num_writes;

	ts = getustime();
	do {
		res = epoll_wait(epfd, events, num_evfds, 0);
		for (i = 0; i < res; i++)
			read_cb(evfds[events[i].data.u32], events[i].data.u32);
	} while (count != fired);
	te = getustime();

	*tr = te - ts;
	*work = count;

	return 0;
}

static int cmp_double(void const *p1, void const *p2) {
	double const *d1 = p1, *d2 = p2;

	return *d1 > *d2 ? 1: *d1 < *d2 ? -1: 0;
}

static void usage(char const *prg) {

	fprintf(stderr, "use: %s [-n NUMevfds] [-a NUMACTIVE] [-w NUMWRITES] [-q] [-h]\n", prg);
}

int main (int argc, char **argv) {
	struct rlimit rl;
	int i, c, quiet = 0;
	long work;
	unsigned long long tr;
	double avg, sig, sv;
	struct epoll_event ev;
	extern char *optarg;

	num_evfds = 200;
	num_active = 1;
	num_writes = 50000;
	while ((c = getopt(argc, argv, "n:a:w:qh")) != -1) {
		switch (c) {
		case 'n':
			num_evfds = atoi(optarg);
			break;
		case 'a':
			num_active = atoi(optarg);
			break;
		case 'w':
			num_writes = atoi(optarg);
			break;
		case 'q':
			quiet++;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (num_active >= num_evfds)
		num_active = num_evfds - 1;

	rl.rlim_cur = rl.rlim_max = num_evfds * 2 + 50;
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
		perror("setrlimit");
		return 2;
	}

	events = calloc(num_evfds, sizeof(struct epoll_event));
	evfds = calloc(num_evfds, sizeof(int));
	if (events == NULL || evfds == NULL) {
		perror("malloc");
		return 3;
	}

	if ((epfd = epoll_create(num_evfds)) == -1) {
		perror("epoll_create");
		return 4;
	}

	for (i = 0; i < num_evfds; i++) {
		if ((evfds[i] = eventfd(0)) == -1) {
			perror("eventfd");
			return 5;
		}
		fcntl(evfds[i], F_SETFL, fcntl(evfds[i], F_GETFL) | O_NONBLOCK);
	}

	for (i = 0; i < num_evfds; i++) {
		ev.events = EPOLLIN | EPOLLET;
		ev.data.u32 = i;
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, evfds[i], &ev) < 0) {
			perror("epoll_ctl");
			return 6;
		}
	}
	if (!quiet)
		fprintf(stdout, "us/event (lower is better):\n");
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
		"AVG: %lf us/event\n"
		"SIG: %lf\n", avg, sig);

	return 0;
}

