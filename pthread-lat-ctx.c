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
#include <sys/syscall.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>

#define MIN_TEST_TIME_US 2000000

struct pipe_duplex {
	int p1[2];
	int p2[2];
};

static struct pipe_duplex pd;
static unsigned long long swcount;
static unsigned long long thtimes[2];

static pid_t gettid(void)
{
	return (pid_t) syscall(SYS_gettid);
}

static void xpipe(int *pfds)
{
	if (pipe(pfds) != 0) {
		perror("Creating a pipe");
		exit(1);
	}
}

static void xsched_setaffinity(pid_t pid, unsigned int cpusetsize,
			       cpu_set_t *mask)
{
	if (sched_setaffinity(pid, cpusetsize, mask) != 0) {
		perror("Setting thread CPU affinity");
		exit(1);
	}
}

static void xpthread_create(pthread_t *ptid, pthread_attr_t const *attr,
			    void *(*thproc)(void *), void *arg)
{
	if (pthread_create(ptid, attr, thproc, arg) != 0) {
		perror("Creating new thread");
		exit(1);
	}
}

static unsigned long long getustime(void)
{
	struct timeval tm;

	gettimeofday(&tm, NULL);

	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

static double get_base_switch_time(int *pfd, unsigned long long *pcount)
{
	unsigned long i;
	unsigned long long ts, te, count;
	char buf[1];

	count = 0;
	ts = getustime();
	do {
		for (i = 0; i < 10000; i++) {
			if (write(pfd[1], "w", 1) != 1 ||
			    read(pfd[0], buf, 1) != 1)
				;
		}
		count += i;
		te = getustime();
	} while (te - ts < MIN_TEST_TIME_US);
	*pcount = count;

	return (double) (te - ts) / (double) count;
}

static void pin_to_cpu(int cpu)
{
	cpu_set_t cset;

	CPU_ZERO(&cset);
	CPU_SET(cpu, &cset);
	xsched_setaffinity(gettid(), sizeof(cset), &cset);
}

static void *tproc(void *data)
{
	int thid = (int) (long) data;
	unsigned long long count, ts;
	char buf[1];

	pin_to_cpu(0);
	ts = getustime();
	if (thid == 0) {
		for (count = swcount; count != 0; count--) {
			if (write(pd.p1[1], "w", 1) != 1 ||
			    read(pd.p2[0], buf, 1) != 1)
				;
		}
	} else {
		for (count = swcount; count != 0; count--) {
			if (write(pd.p2[1], "w", 1) != 1 ||
			    read(pd.p1[0], buf, 1) != 1)
				;
		}
	}
	thtimes[thid] = getustime() - ts;

	return NULL;
}

int main(int ac, char **av)
{
	int c;
	unsigned long long count;
	double bxtime, txtime;
	pthread_t tids[2];

	while ((c = getopt(ac, av, "h")) != -1) {
		switch (c) {
		case 'h':

			break;
		default:
			fprintf(stderr, "Illegal argument \"%c\"\n", c);
			exit(1);
		}
	}

	xpipe(pd.p1);
	xpipe(pd.p2);

	pin_to_cpu(0);
	bxtime = get_base_switch_time(pd.p1, &count);
	swcount = count;

	fprintf(stdout, "BASE   = %.4lfus\n", bxtime);
	fprintf(stdout, "COUNT  = %llu\n", count);

	xpthread_create(&tids[0], NULL, tproc, (void *) 0L);
	xpthread_create(&tids[1], NULL, tproc, (void *) 1L);

	pthread_join(tids[0], NULL);
	pthread_join(tids[1], NULL);

	txtime = (double) ((thtimes[0] + thtimes[1]) / 2) / (double) count;
	fprintf(stdout, "THREAD = %.4lfus\n", txtime);
	fprintf(stdout, "CTXUS  = %.4lfus\n", txtime - bxtime);

	return 0;
}

