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
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifndef EFD_SEMLIKE
#define EFD_SEMLIKE (1 << 0)
#endif

#ifndef __NR_eventfd2
#if defined(__x86_64__)
#define __NR_eventfd2 290
#elif defined(__i386__)
#define __NR_eventfd2 328
#else
#error Cannot detect your architecture!
#endif
#endif


static int eventfd2(int count, int flags) {
	return syscall(__NR_eventfd2, count, flags);
}

static void xsem_wait(int fd) {
	u_int64_t cntr;

	if (read(fd, &cntr, sizeof(cntr)) != sizeof(cntr)) {
		perror("reading eventfd");
		exit(1);
	}
	fprintf(stdout, "[%u] wait completed on %d: count=%llu\n",
		getpid(), fd, cntr);
}

static void xsem_post(int fd, int count) {
	u_int64_t cntr = count;

	if (write(fd, &cntr, sizeof(cntr)) != sizeof(cntr)) {
		perror("writing eventfd");
		exit(1);
	}
}

static void sem_player(int fd1, int fd2) {
	fprintf(stdout, "[%u] posting 1 on %d\n", getpid(), fd1);
	xsem_post(fd1, 1);

	fprintf(stdout, "[%u] waiting on %d\n", getpid(), fd2);
	xsem_wait(fd2);

	fprintf(stdout, "[%u] posting 1 on %d\n", getpid(), fd1);
	xsem_post(fd1, 1);

	fprintf(stdout, "[%u] waiting on %d\n", getpid(), fd2);
	xsem_wait(fd2);

	fprintf(stdout, "[%u] posting 5 on %d\n", getpid(), fd1);
	xsem_post(fd1, 5);

	fprintf(stdout, "[%u] waiting 5 times on %d\n", getpid(), fd2);
	xsem_wait(fd2);
	xsem_wait(fd2);
	xsem_wait(fd2);
	xsem_wait(fd2);
	xsem_wait(fd2);
}

static void usage(char const *prg) {
	fprintf(stderr, "use: %s [-h]\n", prg);
}

int main (int argc, char **argv) {
	int c, fd1, fd2, status;
	pid_t cpid_poster, cpid_waiter;

	while ((c = getopt(argc, argv, "h")) != -1) {
		switch (c) {
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if ((fd1 = eventfd2(0, EFD_SEMLIKE)) == -1 ||
	    (fd2 = eventfd2(0, EFD_SEMLIKE)) == -1) {
		perror("eventfd2");
		return 1;
	}
	if ((cpid_poster = fork()) == 0) {
		sem_player(fd1, fd2);
		exit(0);
	}
	if ((cpid_waiter = fork()) == 0) {
		sem_player(fd2, fd1);
		exit(0);
	}
	waitpid(cpid_poster, &status, 0);
	waitpid(cpid_waiter, &status, 0);

	return 0;
}

