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
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <math.h>


#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

#define TSPIN_LOCK_UNLOCKED	(tspinlock_t) { 0, 0 }
#define SPIN_LOCK_UNLOCKED	(spinlock_t) { 1 }

#define NAVG		16
#define MAX_SAMPLES	32
#define NR_ITERS	100
#define MAX_CPUS	128



#define rdtscll(val) do { \
	unsigned int __a,__d; \
	asm volatile("rdtsc" : "=a" (__a), "=d" (__d)); \
	(val) = ((unsigned long long) __a) | (((unsigned long long) __d) << 32); \
} while(0)


typedef unsigned short tspin_type;

typedef struct {
	tspin_type qhead;
	tspin_type qtail;
} __attribute__((__aligned__(128))) tspinlock_t;

typedef struct {
	unsigned int slock;
} __attribute__((__aligned__(128))) spinlock_t;

struct thread_ctx {
	int cpu;
	pthread_t tid;
	unsigned long long samples[MAX_SAMPLES];
	double avg, sig;
};

struct test_desc {
	char const *name;
	void *(*tproc)(void *);
};



static tspinlock_t tspin[MAX_CPUS];
static spinlock_t spin[MAX_CPUS];
static int sched_prio = 90;
static int sched_policy = SCHED_FIFO;
static int num_threads = 1;
static struct thread_ctx *tctx;
static int go;



static inline void cpu_relax(void) {

	asm volatile ("rep ; nop" : : : "memory");
}

static inline void tspin_lock_init(tspinlock_t *lock) {

	lock->qhead = lock->qtail = 0;
}

static inline void tspin_lock(tspinlock_t *lock) {
	tspin_type pos = 1;

	asm volatile ("lock ; xaddw %0, %1\n\t"
		      : "+r" (pos), "+m" (lock->qhead) : : "memory");
	while (unlikely(pos != lock->qtail))
		cpu_relax();
}

static inline void tspin_unlock(tspinlock_t *lock) {
	asm volatile ("addw $1, %0\n\t"
		      : "+m" (lock->qtail) : : "memory");
}

static inline void spin_lock_init(spinlock_t *lock) {

	lock->slock = 1;
}

static inline void spin_lock(spinlock_t *lock) {
	asm volatile ("1:\n\t"
		      "lock ; decl %0\n\t"
		      "jns 2f\n\t"
		      "3:\n\t"
		      "rep;nop\n\t"
		      "cmpl $0,%0\n\t"
		      "jle 3b\n\t"
		      "jmp 1b\n\t"
		      "2:\n\t" : "=m" (lock->slock) : : "memory");
}

static inline void spin_unlock(spinlock_t *lock) {
	asm volatile ("movl $1,%0\n\t" : "=m" (lock->slock) : : "memory");
}

static inline unsigned long long get_cycles(void) {
	unsigned long long ret;

	rdtscll(ret);
	return ret;
}

static int thread_setched(int cpu) {
	cpu_set_t mask;
	struct sched_param param;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);

	if (sched_setaffinity(0, sizeof(mask), &mask) == -1)
		perror("sched_setaffinity"), exit(1);
	memset(&param, 0, sizeof(param));
	param.sched_priority = sched_prio;
	if (sched_setscheduler(0, sched_policy, &param) == -1)
		perror("sched_setscheduler"), exit(1);

	return 0;
}

static int thread_setup(struct thread_ctx *ctx) {

	thread_setched(ctx->cpu);

	return 0;
}

static void *tspin_thread(void *arg) {
	struct thread_ctx *ctx = (struct thread_ctx *) arg;
	int i, j, k = ctx->cpu;
	unsigned long long ts, te;

	thread_setup(ctx);
	tspin_lock(&tspin[k]);
	while (!go)
		sched_yield();

	for (j = 0; j < MAX_SAMPLES; j++) {
		ts = get_cycles();
		for (i = 0; i < NR_ITERS; i++) {
			tspin_unlock(&tspin[k]);
			if (++k >= num_threads)
				k = 0;
			tspin_lock(&tspin[k]);
		}
		te = get_cycles();
		ctx->samples[j] = te - ts;
	}
	tspin_unlock(&tspin[k]);

	return NULL;
}

static void *spin_thread(void *arg) {
	struct thread_ctx *ctx = (struct thread_ctx *) arg;
	int i, j, k = ctx->cpu;
	unsigned long long ts, te;

	thread_setup(ctx);
	spin_lock(&spin[k]);
	while (!go)
		sched_yield();

	for (j = 0; j < MAX_SAMPLES; j++) {
		ts = get_cycles();
		for (i = 0; i < NR_ITERS; i++) {
			spin_unlock(&spin[k]);
			if (++k >= num_threads)
				k = 0;
			spin_lock(&spin[k]);
		}
		te = get_cycles();
		ctx->samples[j] = te - ts;
	}
	spin_unlock(&spin[k]);

	return NULL;
}

static int cmp_ull(void const *p1, void const *p2) {
	unsigned long long const *d1 = p1, *d2 = p2;

	return *d1 > *d2 ? 1: *d1 < *d2 ? -1: 0;
}

int main(int ac, char **av) {
	int i, j, ncpus;
	struct test_desc tspin_desc = { "TICKLOCK", tspin_thread };
	struct test_desc spin_desc = { "SPINLOCK", spin_thread };
	struct test_desc *tdesc = &tspin_desc;
	unsigned long long ts, te, uscycles;
	struct timespec ts1, ts2;

	for (i = 1; i < ac; i++) {
		if (!strcmp(av[i], "-n")) {
			if (++i < ac)
				num_threads = atoi(av[i]);
		} else if (!strcmp(av[i], "-p")) {
			if (++i < ac)
				sched_prio = atoi(av[i]);
		} else if (!strcmp(av[i], "-s")) {
			tdesc = &spin_desc;
		} else if (!strcmp(av[i], "-F")) {
			sched_policy = SCHED_FIFO;
		} else if (!strcmp(av[i], "-O")) {
			sched_policy = SCHED_OTHER;
		} else if (!strcmp(av[i], "-R")) {
			sched_policy = SCHED_RR;
		}
	}
	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	if (num_threads > ncpus) {
		fprintf(stderr,
			"number of threads (%d) greater than number cpus (%d)\n"
			"\tdowngrading threads to %d\n", num_threads, ncpus, ncpus);
		num_threads = ncpus;
	}

	tctx = (struct thread_ctx *) malloc(num_threads * sizeof(struct thread_ctx));
	memset(tctx, 0, num_threads * sizeof(struct thread_ctx));

	for (i = 0; i < num_threads; i++) {
		tspin_lock_init(&tspin[i]);
		spin_lock_init(&spin[i]);
	}

	fprintf(stdout, "now testing: %s\n", tdesc->name);

	clock_getres(CLOCK_REALTIME, &ts1);
	fprintf(stdout, "timeres=%ld\n", ts1.tv_nsec / 1000);

	thread_setched(0);

	clock_gettime(CLOCK_REALTIME, &ts1);
	ts = get_cycles();
	sleep(1);
	clock_gettime(CLOCK_REALTIME, &ts2);
	te = get_cycles();
	uscycles = (te - ts) / ((ts2.tv_sec - ts1.tv_sec) * 1000000ULL +
				(ts2.tv_nsec - ts1.tv_nsec) / 1000);
	fprintf(stdout, "uscycles=%llu\n", uscycles);

	for (i = 0; i < num_threads; i++) {
		tctx[i].cpu = i;
		if (pthread_create(&tctx[i].tid, NULL, tdesc->tproc,
				   (void *) &tctx[i]) == -1)
			perror("pthread_create"), exit(1);
	}

	go = 1;
	sched_yield();

	for (i = 0; i < num_threads; i++) {
		double avg, sv, sig;

		if (pthread_join(tctx[i].tid, NULL) == -1)
			perror("pthread_join"), exit(1);

		qsort(tctx[i].samples, MAX_SAMPLES, sizeof(unsigned long long),
		      cmp_ull);
		for (j = 0, avg = 0; j < NAVG; j++)
			avg += tctx[i].samples[j + MAX_SAMPLES / 2 - NAVG / 2];
		avg /= NAVG;
		tctx[i].avg = avg;
		for (j = 0, sig = 0; j < NAVG; j++) {
			sv = avg - tctx[i].samples[j + MAX_SAMPLES / 2 - NAVG / 2];
			sig += sv * sv;
		}
		sig = sqrt(sig / NAVG);
		tctx[i].sig = sig;
		fprintf(stdout,
			"AVG[%d]: %lf cycles/loop\n"
			"SIG[%d]: %lf\n", i, avg, i, sig);
	}

	return 0;
}

