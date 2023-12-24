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
#include <sys/signal.h>
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
#include <pthread.h>

#define DEVZERO_FILE	"/dev/zero"
#define DEF_BUFSIZE	1024
#define DEF_READSIZE	(1024 * 1024)

#define RUN_SHARED	(1 << 0)
#define RUN_UNSHARED	(1 << 1)


static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void *thproc(void *data) {
	pthread_mutex_lock(&mtx);
	pthread_mutex_unlock(&mtx);
	return (void *) 0;
}

static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);
	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

static long read_test(const char *zfname, int fd, void *buf, int bufsize,
		      long size) {
	long rsize, cnt;

	lseek(fd, 0, SEEK_SET);
	for (rsize = 0; rsize < size;) {
		if ((cnt = size - rsize) > (long) bufsize)
			cnt = bufsize;
		if (read(fd, buf, (size_t) cnt) != cnt) {
			perror(zfname);
			break;
		}
		rsize += cnt;
	}

	return rsize;
}

static void usage(const char *prg) {
	fprintf(stderr, "use: %s [-f TESTFILE] [-b BLKSIZE] [-r READSIZE] [-U] [-S] [-h]\n", prg);
}

int main (int ac, char **av) {
	int c, fd, bufsize = DEF_BUFSIZE, mode = RUN_SHARED | RUN_UNSHARED;
	long size = DEF_READSIZE;
	unsigned long long ts, te;
	pthread_t tid;
	const char *zfname = DEVZERO_FILE;
	void *buf;

	while ((c = getopt(ac, av, "f:b:r:USh")) != -1) {
		switch (c) {
		case 'f':
			zfname = optarg;
			break;
		case 'b':
			bufsize = atoi(optarg);
			break;
		case 'r':
			size = atol(optarg);
			break;
		case 'U':
			mode &= ~RUN_UNSHARED;
			break;
		case 'S':
			mode &= ~RUN_SHARED;
			break;
		default:
			usage(av[0]);
			return 1;
		}
	}
	if ((buf = malloc(bufsize)) == NULL) {
		perror("read buffer");
		return 2;
	}
	if ((fd = open(zfname, O_RDONLY)) == -1) {
		perror(zfname);
		return 2;
	}
	if ((mode & (RUN_SHARED | RUN_UNSHARED)) == (RUN_SHARED | RUN_UNSHARED)) {
		fprintf(stdout, "warming up ...\n");
		read_test(zfname, fd, buf, bufsize, size);
	}
	if (mode & RUN_UNSHARED) {
		fprintf(stdout, "testing non-shared ...\n");
		ts = getustime();
		read_test(zfname, fd, buf, bufsize, size);
		te = getustime();
		fprintf(stdout, "time = %lf ms\n", ((double) (te - ts)) / 1000.0);
	}
	if (mode & RUN_SHARED) {
		pthread_mutex_lock(&mtx);
		if (pthread_create(&tid, NULL, thproc, NULL) != 0) {
			perror("pthread_create()");
			return 2;
		}

		fprintf(stdout, "testing shared ...\n");
		ts = getustime();
		read_test(zfname, fd, buf, bufsize, size);
		te = getustime();
		fprintf(stdout, "time = %lf ms\n", ((double) (te - ts)) / 1000.0);
		pthread_mutex_unlock(&mtx);
	}

	close(fd);

	return 0;
}

