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


#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>


#define MAX(a, b)	((a) > (b) ? (a): (b))

#define NT_MAX		400
#define NT_TOTAL	1200
#define MAX_MSEC	200

#define STACKSIZE	(4096 * 6)
#ifdef PTHREAD_STACK_MIN
#define THREAD_STACK_MIN	MAX(PTHREAD_STACK_MIN, STACKSIZE)
#else
#define THREAD_STACK_MIN	STACKSIZE
#endif


struct thread_ctx {
	struct thread_ctx *next;
	char *stk;
	int size;
	pthread_t thid;
};

static pthread_cond_t cnd = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static struct thread_ctx *tlst;
static struct thread_ctx tt[NT_MAX];

static void *thread_proc(void* ptr) {
	struct thread_ctx *th = (struct thread_ctx *) ptr;
	long r;

	fprintf(stdout, "start %p\n", th->stk);
	if ((char *) &r < th->stk || (char *) &r >= th->stk + th->size)
		fprintf(stderr, "ERR: stack not set: %p out of [%p, %p)\n",
			&r, th->stk, th->stk + th->size);
	r = rand() % 1000;
	usleep(r * MAX_MSEC);

	pthread_mutex_lock(&mtx);
	th->next = tlst;
	tlst = th;
	pthread_cond_signal(&cnd);
	pthread_mutex_unlock(&mtx);

	return NULL;
}

int main(int ac, char **av) {
	int completed, running = 0;
	int i, err, nt = 128, nt_total = NT_TOTAL, stk_size = THREAD_STACK_MIN;
	struct thread_ctx *th;
	pthread_attr_t pa;

	if (nt > NT_MAX)
		nt = NT_MAX;

	for (i = 0; i < nt; i++) {
		tt[i].size = stk_size;
		tt[i].stk = mmap(NULL, tt[i].size, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANON, -1, 0);
		if (tt[i].stk == MAP_FAILED) {
			perror("mmap");
			exit(1);
		}

		pthread_attr_init(&pa);
		if ((err = pthread_attr_setstack(&pa, tt[i].stk, tt[i].size)) != 0) {
			fprintf(stderr, "ERR: pthread_attr_setstack failed: %s\n",
				strerror(err));
			exit(1);
		}
		if ((err = pthread_create(&tt[i].thid, &pa, thread_proc, &tt[i])) != 0) {
			fprintf(stderr, "ERR: pthread_create failed: %s\n",
				strerror(err));
			exit(1);
		}
		pthread_attr_destroy(&pa);
		running++;
	}
	pthread_mutex_lock(&mtx);
	for (completed = 0; running > 0;) {
		pthread_cond_wait(&cnd, &mtx);
		while ((th = tlst) != NULL) {
			tlst = tlst->next;
			pthread_join(th->thid, NULL);

			fprintf(stdout, "unmap %p\n", th->stk);
			munmap(th->stk, th->size);

			completed++;
			running--;
			if (completed + running >= nt_total)
				continue;

			th->stk = mmap(NULL, th->size, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANON, -1, 0);
			if (th->stk == MAP_FAILED) {
				perror("mmap");
				exit(1);
			}

			pthread_attr_init(&pa);
			if ((err = pthread_attr_setstack(&pa, th->stk, th->size)) != 0) {
				fprintf(stderr, "ERR: pthread_attr_setstack failed: %s\n",
					strerror(err));
				exit(1);
			}
			if ((err = pthread_create(&th->thid, &pa, thread_proc, th)) != 0) {
				fprintf(stderr, "ERR: pthread_create failed: %s\n",
					strerror(err));
				exit(1);
			}
			pthread_attr_destroy(&pa);
			running++;
		}
	}
	pthread_mutex_unlock(&mtx);
	fprintf(stderr, "Done\n");

	return 0;
}

