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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <float.h>
#include <errno.h>
#include <sched.h>


#define AUXSIZE 32

struct slreport {
	unsigned long missd;
	unsigned long long time;
};

struct sltask {
	char const *name;
	int nice;
	unsigned long bus, sus, wus;
	unsigned long jbus, jsus, jwus;
	int count;
	pid_t *pids;
	struct slreport *reps;
	int *pfds;
};

static double usloop;
static int stop_test;
static int debug;

static void sig_int(int sig) {

	++stop_test;
	signal(sig, sig_int);
}

static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);
	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

static void burn_loop(void) {
	unsigned long i;
	static unsigned long long aux[AUXSIZE];

	for (i = 0; i < 100; i++)
		aux[i % AUXSIZE] += i * 7 - 3;
}

static void burn(unsigned long usecs) {
	double t, us = usecs;

	for (t = 0; t < us; t += usloop)
		burn_loop();
}

static unsigned long myrand(const unsigned long *sval) {
	static unsigned long next;

	if (sval != NULL)
		next = *sval;
	else
		next = next * 1103515245UL + 12345;

	return next;
}

static int ilog2(unsigned long v) {
	int l;

	for (l = 0; v; v >>= 1, l++);
	return l;
}

static unsigned long jval(unsigned long v, unsigned long jitter) {
	unsigned long b, r;

	if (jitter > 0) {
		b = (v > jitter) ? v - jitter: 0;
		r = myrand(NULL);
		r >>= 8 * sizeof(unsigned long) - ilog2(2 * jitter) - 1;
		v = b + r % (2 * jitter);
	}
	return v;
}

static int next_task(struct sltask *tasks, int n) {

	if (n >= 0) {
		if ((tasks[n].pids = (pid_t *)
		     malloc(tasks[n].count *
			    sizeof(pid_t))) == NULL ||
		    (tasks[n].reps = (struct slreport *)
		     malloc(tasks[n].count *
			    sizeof(struct slreport))) == NULL ||
		    (tasks[n].pfds = (int *)
		     malloc(2 * tasks[n].count *
			    sizeof(int))) == NULL) {
			perror("malloc");
			exit(1);
		}
	}

	return n + 1;
}

static void calibrate_loop(void) {
	int i, j, loops, oldpolicy;
	unsigned long long ts, tn, tl;
	double ust;
	struct sched_param sp, osp;

	if ((oldpolicy = sched_getscheduler(0)) < 0) {
		perror("sched_getscheduler");
		exit(1);
	}
	if (sched_getparam(0, &osp)) {
		perror("sched_getparam");
		exit(1);
	}
	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = 99;
	if (sched_setscheduler(0, SCHED_RR, &sp)) {
		if (debug)
			perror("sched_setscheduler");
		loops = 8;
	} else
		loops = 2;
	usloop = DBL_MAX;
	for (i = 0; i < loops; i++) {
		tl = 0;
		ts = getustime();
		do {
			for (j = 0; j < 1024; j++)
				burn_loop();
			tl += j;
			tn = getustime();
		} while (tn - ts < 25000);
		ust = (double) (tn - ts) / tl;
		if (ust < usloop)
			usloop = ust;
	}
	sched_setscheduler(0, oldpolicy, &osp);
	if (debug)
		fprintf(stderr, "us per loop: %f\n", usloop);
}

static void usage(const char *prg) {

	fprintf(stderr,
		"use: %s [-T NAME] [-s USSLEEP] [-b USBURN] [-w USWAIT] [-n NICE]\n"
		"\t[-N NTASKS] [-B CPU] [-R NSECS] [-D] [-h]\n", prg);
	exit(1);
}

int main(int ac, char **av) {
	int i, k, nt, n = -1, cpu_bind = -1, status, runfor = 0;
	unsigned long j, bus, sus, wus;
	unsigned long long ts, tn, tl, totcpu;
	const char *jptr;
	struct sltask *tasks, *ctask;
	cpu_set_t mask;
	struct slreport rep;

	if ((tasks = (struct sltask *)
	     malloc(ac * sizeof(struct sltask))) == NULL) {
		perror("malloc");
		return 1;
	}
	memset(tasks, 0, ac * sizeof(struct sltask));
	while ((i = getopt(ac, av, "T:b:s:w:n:N:B:R:Dh")) != -1) {
		switch (i) {
		case 'T':
			n = next_task(tasks, n);
			tasks[n].name = optarg;
			tasks[n].count = 1;
			break;
		case 'b':
			tasks[n].bus = atol(optarg);
			if ((jptr = strchr(optarg, ':')) != NULL)
				tasks[n].jbus = atol(jptr + 1);
			break;
		case 's':
			tasks[n].sus = atol(optarg);
			if ((jptr = strchr(optarg, ':')) != NULL)
				tasks[n].jsus = atol(jptr + 1);
			break;
		case 'w':
			tasks[n].wus = atol(optarg);
			if ((jptr = strchr(optarg, ':')) != NULL)
				tasks[n].jwus = atol(jptr + 1);
			break;
		case 'n':
			tasks[n].nice = atoi(optarg);
			break;
		case 'N':
			tasks[n].count = atoi(optarg);
			break;
		case 'R':
			runfor = atoi(optarg);
			break;
		case 'B':
			cpu_bind = atol(optarg);
			break;
		case 'D':
			debug = 1;
			break;
		case 'h':
			usage(av[0]);
		}
	}
	if (n < 0)
		usage(av[0]);
	n = next_task(tasks, n);
	if (cpu_bind >= 0) {
		CPU_ZERO(&mask);
		CPU_SET(cpu_bind, &mask);
		if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
			perror("sched_setaffinity");
			return 1;
		}
	}
	calibrate_loop();
	signal(SIGINT, sig_int);
	signal(SIGALRM, sig_int);
	for (i = 0; i < n; i++) {
		ctask = &tasks[i];
		for (k = 0; k < ctask->count; k++) {
			nt = k;
			if (pipe(&ctask->pfds[2 * nt])) {
				perror("pipe");
				return 1;
			}
			if ((ctask->pids[k] = fork()) == 0)
				goto child;
		}
	}
	for (i = 0, totcpu = 0; i < n; i++) {
		ctask = &tasks[i];
		for (k = 0; k < ctask->count; k++) {
			if (waitpid(ctask->pids[k], &status, 0) != ctask->pids[k]) {
				perror("waitpid");
				return 1;
			}
			if (read(ctask->pfds[2 * k], &ctask->reps[k],
				 sizeof(struct slreport)) != sizeof(struct slreport)) {
				perror("pipe read");
				return 1;
			}
			totcpu += ctask->reps[k].time;
		}
	}
	fprintf(stdout, "Total Burnt Time: %llu ms\n", totcpu / 1000);
	if (totcpu) {
		for (i = 0; i < n; i++) {
			ctask = &tasks[i];
			for (k = 0; k < ctask->count; k++) {
				fprintf(stdout, "%s[%3d]{%3d} %llu ms (%.2f%%); missed %lu\n",
					ctask->name, k, ctask->nice, ctask->reps[k].time / 1000,
					100.0 * (double) ctask->reps[k].time / (double) totcpu,
					ctask->reps[k].missd);
			}
		}
	}

	return 0;

	child:
	if (debug)
		fprintf(stderr, "[%6lu] %s\n", (unsigned long) getpid(), ctask->name);
	if (runfor > 0)
		alarm(runfor);
	if (setpriority(PRIO_PROCESS, 0, ctask->nice))
		perror("setpriority");
	rep.missd = 0;
	rep.time = 0;
	for (; !stop_test;) {
		ts = getustime();
		if (ctask->bus) {
			bus = jval(ctask->bus, ctask->jbus);
			burn(bus);
			rep.time += bus;
		}
		if (ctask->sus) {
			sus = jval(ctask->sus, ctask->jsus);
			usleep(sus);
		}
		if (ctask->wus) {
			wus = jval(ctask->wus, ctask->jwus);
			tn = getustime();
			if (ts + wus > tn)
				usleep(ts + wus - tn);
			else {
				rep.missd++;
				if (debug)
					fprintf(stderr, "missed deadline: %llu us\n",
						tn - (ts + wus));
			}
		}
	}
	write(ctask->pfds[2 * nt + 1], &rep, sizeof(rep));

	return 0;
}

