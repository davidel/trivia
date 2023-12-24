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


#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>


/*
 * This were good at the time of 2.6.21-rc5.mm4 ...
 */
#ifndef __NR_timerfd
#if defined(__x86_64__)
#define __NR_timerfd 282
#elif defined(__i386__)
#define __NR_timerfd 322
#else
#error Cannot detect your architecture!
#endif
#endif



/* Definitions from include/linux/timerfd.h */
#define TFD_TIMER_ABSTIME (1 << 0)



struct tmr_type {
	int id;
	char const *name;
};


unsigned long long getustime(int clockid) {
	struct timespec tp;

	if (clock_gettime((clockid_t) clockid, &tp)) {
		perror("clock_gettime");
		return 0;
	}

	return 1000000ULL * tp.tv_sec + tp.tv_nsec / 1000;
}

void set_timespec(struct timespec *tmr, unsigned long long ustime) {

	tmr->tv_sec = (time_t) (ustime / 1000000ULL);
	tmr->tv_nsec = (long) (1000ULL * (ustime % 1000000ULL));
}

int timerfd(int ufc, int clockid, int flags, struct itimerspec *utmr) {

	return syscall(__NR_timerfd, ufc, clockid, flags, utmr);
}

long waittmr(int tfd, int timeo) {
	u_int32_t ticks;
	struct pollfd pfd;

	pfd.fd = tfd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, timeo) < 0) {
		perror("poll");
		return -1;
	}
	if ((pfd.revents & POLLIN) == 0) {
		fprintf(stdout, "no ticks happened\n");
		return -1;
	}
	if (read(tfd, &ticks, sizeof(ticks)) != sizeof(ticks)) {
		perror("timerfd read");
		return -1;
	}

	return ticks;
}

int main(int ac, char **av) {
	int i, tfd, tfd2;
	long ticks;
	unsigned long long tnow, ttmr;
	u_int32_t uticks;
	struct itimerspec tmr;
	struct tmr_type clks[] = {
		{ CLOCK_MONOTONIC, "CLOCK MONOTONIC" },
		{ CLOCK_REALTIME, "CLOCK REALTIME" },
	};

	for (i = 0; i < sizeof(clks) / sizeof(clks[0]); i++) {
		fprintf(stdout, "\n\n---------------------------------------\n");
		fprintf(stdout, "| testing %s\n", clks[i].name);
		fprintf(stdout, "---------------------------------------\n\n");

		fprintf(stdout, "relative timer test (at 500 ms) ...\n");
		set_timespec(&tmr.it_value, 500 * 1000);
		set_timespec(&tmr.it_interval, 0);
		tnow = getustime(clks[i].id);
		if ((tfd = timerfd(-1, clks[i].id, 0,
				   &tmr)) == -1) {
			perror("timerfd");
			return 1;
		}
		fprintf(stdout, "timerfd = %d\n", tfd);

		fprintf(stdout, "wating timer ...\n");
		ticks = waittmr(tfd, -1);
		ttmr = getustime(clks[i].id);
		if (ticks <= 0)
			fprintf(stdout, "whooops! no timer showed up!\n");
		else
			fprintf(stdout, "got timer ticks (%ld) after %llu ms\n",
				ticks, (ttmr - tnow) / 1000);


		fprintf(stdout, "absolute timer test (at 500 ms) ...\n");
		tnow = getustime(clks[i].id);
		set_timespec(&tmr.it_value, tnow + 500 * 1000);
		set_timespec(&tmr.it_interval, 0);
		if ((tfd = timerfd(tfd, clks[i].id, TFD_TIMER_ABSTIME,
				   &tmr)) == -1) {
			perror("timerfd");
			return 1;
		}
		fprintf(stdout, "timerfd = %d\n", tfd);

		fprintf(stdout, "wating timer ...\n");
		ticks = waittmr(tfd, -1);
		ttmr = getustime(clks[i].id);
		if (ticks <= 0)
			fprintf(stdout, "whooops! no timer showed up!\n");
		else
			fprintf(stdout, "got timer ticks (%ld) after %llu ms\n",
				ticks, (ttmr - tnow) / 1000);

		fprintf(stdout, "sequential timer test (100 ms clock) ...\n");
		tnow = getustime(clks[i].id);
		set_timespec(&tmr.it_value, tnow + 100 * 1000);
		set_timespec(&tmr.it_interval, 100 * 1000);
		if ((tfd = timerfd(tfd, clks[i].id, TFD_TIMER_ABSTIME,
				   &tmr)) == -1) {
			perror("timerfd");
			return 1;
		}
		fprintf(stdout, "timerfd = %d\n", tfd);

		fprintf(stdout, "sleeping 2 seconds ...\n");
		sleep(2);

		fprintf(stdout, "wating timer ...\n");
		ticks = waittmr(tfd, -1);
		ttmr = getustime(clks[i].id);
		if (ticks <= 0)
			fprintf(stdout, "whooops! no timer showed up!\n");
		else
			fprintf(stdout, "got timer ticks (%ld) after %llu ms\n",
				ticks, (ttmr - tnow) / 1000);


		fprintf(stdout, "O_NONBLOCK test ...\n");
		tnow = getustime(clks[i].id);
		set_timespec(&tmr.it_value, 100 * 1000);
		set_timespec(&tmr.it_interval, 0);
		if ((tfd = timerfd(tfd, clks[i].id, 0,
				   &tmr)) == -1) {
			perror("timerfd");
			return 1;
		}
		fprintf(stdout, "timerfd = %d\n", tfd);

		fprintf(stdout, "wating timer (flush the single tick) ...\n");
		ticks = waittmr(tfd, -1);
		ttmr = getustime(clks[i].id);
		if (ticks <= 0)
			fprintf(stdout, "whooops! no timer showed up!\n");
		else
			fprintf(stdout, "got timer ticks (%ld) after %llu ms\n",
				ticks, (ttmr - tnow) / 1000);

		fcntl(tfd, F_SETFL, fcntl(tfd, F_GETFL, 0) | O_NONBLOCK);

		if (read(tfd, &uticks, sizeof(uticks)) > 0)
			fprintf(stdout, "whooops! timer ticks not zero when should have been\n");
		else if (errno != EAGAIN)
			fprintf(stdout, "whooops! bad errno value (%d = '%s')!\n",
				errno, strerror(errno));
		else
			fprintf(stdout, "success\n");

		fcntl(tfd, F_SETFL, fcntl(tfd, F_GETFL, 0) & ~O_NONBLOCK);

		close(tfd);
	}

	return 0;
}

