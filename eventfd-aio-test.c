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
#include <sys/uio.h>
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
#ifndef __NR_eventfd
#if defined(__x86_64__)
#define __NR_eventfd 284
#elif defined(__i386__)
#define __NR_eventfd 323
#else
#error Cannot detect your architecture!
#endif
#endif

#define BUILD_BUG_IF(c) ((void) sizeof(char[1 - 2 * !!(c)]))

#define TESTFILE_SIZE (4096 * 5200)
#define IORTX_SIZE (1024 * 4)
#define NUM_EVENTS 128


typedef unsigned long aio_context_t;

enum {
	IOCB_CMD_PREAD = 0,
	IOCB_CMD_PWRITE = 1,
	IOCB_CMD_FSYNC = 2,
	IOCB_CMD_FDSYNC = 3,
	/* These two are experimental.
	 * IOCB_CMD_PREADX = 4,
	 * IOCB_CMD_POLL = 5,
	 */
	IOCB_CMD_NOOP = 6,
	IOCB_CMD_PREADV = 7,
	IOCB_CMD_PWRITEV = 8,
};

#if defined(__LITTLE_ENDIAN)
#define PADDED(x,y)	x, y
#elif defined(__BIG_ENDIAN)
#define PADDED(x,y)	y, x
#else
#error edit for your odd byteorder.
#endif

#define IOCB_FLAG_RESFD		(1 << 0)

/*
 * we always use a 64bit off_t when communicating
 * with userland.  its up to libraries to do the
 * proper padding and aio_error abstraction
 */
struct iocb {
	/* these are internal to the kernel/libc. */
	u_int64_t	aio_data;	/* data to be returned in event's data */
	u_int32_t	PADDED(aio_key, aio_reserved1);
	/* the kernel sets aio_key to the req # */

	/* common fields */
	u_int16_t	aio_lio_opcode;	/* see IOCB_CMD_ above */
	int16_t	aio_reqprio;
	u_int32_t	aio_fildes;

	u_int64_t	aio_buf;
	u_int64_t	aio_nbytes;
	int64_t	aio_offset;

	/* extra parameters */
	u_int64_t	aio_reserved2;	/* TODO: use this for a (struct sigevent *) */

	u_int32_t	aio_flags;
	/*
	 * If different from 0, this is an eventfd to deliver AIO results to
	 */
	u_int32_t	aio_resfd;
}; /* 64 bytes */

struct io_event {
	u_int64_t           data;           /* the data field from the iocb */
	u_int64_t           obj;            /* what iocb this event came from */
	int64_t           res;            /* result code for this event */
	int64_t           res2;           /* secondary result */
};


static void asyio_prep_preadv(struct iocb *iocb, int fd, struct iovec *iov,
			      int nr_segs, int64_t offset, int afd)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IOCB_CMD_PREADV;
	iocb->aio_reqprio = 0;
	iocb->aio_buf = (u_int64_t) iov;
	iocb->aio_nbytes = nr_segs;
	iocb->aio_offset = offset;
	iocb->aio_flags = IOCB_FLAG_RESFD;
	iocb->aio_resfd = afd;
}

static void asyio_prep_pwritev(struct iocb *iocb, int fd, struct iovec *iov,
			       int nr_segs, int64_t offset, int afd)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IOCB_CMD_PWRITEV;
	iocb->aio_reqprio = 0;
	iocb->aio_buf = (u_int64_t) iov;
	iocb->aio_nbytes = nr_segs;
	iocb->aio_offset = offset;
	iocb->aio_flags = IOCB_FLAG_RESFD;
	iocb->aio_resfd = afd;
}

static void asyio_prep_pread(struct iocb *iocb, int fd, void *buf,
			     int nr_segs, int64_t offset, int afd)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IOCB_CMD_PREAD;
	iocb->aio_reqprio = 0;
	iocb->aio_buf = (u_int64_t) buf;
	iocb->aio_nbytes = nr_segs;
	iocb->aio_offset = offset;
	iocb->aio_flags = IOCB_FLAG_RESFD;
	iocb->aio_resfd = afd;
}

static void asyio_prep_pwrite(struct iocb *iocb, int fd, void const *buf,
			      int nr_segs, int64_t offset, int afd)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IOCB_CMD_PWRITE;
	iocb->aio_reqprio = 0;
	iocb->aio_buf = (u_int64_t) buf;
	iocb->aio_nbytes = nr_segs;
	iocb->aio_offset = offset;
	iocb->aio_flags = IOCB_FLAG_RESFD;
	iocb->aio_resfd = afd;
}

static long io_setup(unsigned nr_reqs, aio_context_t *ctx) {
	return syscall(__NR_io_setup, nr_reqs, ctx);
}

static long io_destroy(aio_context_t ctx) {
	return syscall(__NR_io_destroy, ctx);
}

static long io_submit(aio_context_t ctx, long n, struct iocb **paiocb) {
	return syscall(__NR_io_submit, ctx, n, paiocb);
}

static long io_cancel(aio_context_t ctx, struct iocb *aiocb,
		      struct io_event *res) {
	return syscall(__NR_io_cancel, ctx, aiocb, res);
}

static long io_getevents(aio_context_t ctx, long min_nr, long nr,
			 struct io_event *events, struct timespec *tmo) {
	return syscall(__NR_io_getevents, ctx, min_nr, nr, events, tmo);
}

static int eventfd(int count) {
	return syscall(__NR_eventfd, count);
}

static long waitasync(int afd, int timeo) {
	struct pollfd pfd;

	pfd.fd = afd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, timeo) < 0) {
		perror("poll");
		return -1;
	}
	if ((pfd.revents & POLLIN) == 0) {
		fprintf(stderr, "no results completed\n");
		return 0;
	}

	return 1;
}

static long test_read(aio_context_t ctx, int fd, long range, int afd) {
	long i, n, r, j;
	u_int64_t eval;
	struct iocb **piocb;
	struct iocb *iocb;
	struct timespec tmo;
	static struct io_event events[NUM_EVENTS];
	static char buf[IORTX_SIZE];

	n = range / IORTX_SIZE;
	iocb = malloc(n * sizeof(struct iocb));
	piocb = malloc(n * sizeof(struct iocb *));
	if (!iocb || !piocb) {
		perror("iocb alloc");
		return -1;
	}
	for (i = 0; i < n; i++) {
		piocb[i] = &iocb[i];
		asyio_prep_pread(&iocb[i], fd, buf, sizeof(buf),
				 (n - i - 1) * IORTX_SIZE, afd);
		iocb[i].aio_data = (u_int64_t) i + 1;
	}
	fprintf(stdout, "submitting read requests (%ld) ...\n", n);
	if ((r = io_submit(ctx, n, piocb)) <= 0) {
		perror("io_submit");
		return -1;
	}
	fprintf(stdout, "submitted %ld requests\n", r);
	for (i = 0; i < n;) {
		fprintf(stdout, "waiting ... "), fflush(stdout);
		waitasync(afd, -1);
		eval = 0;
		if (read(afd, &eval, sizeof(eval)) != sizeof(eval))
			perror("read");
		fprintf(stdout, "done! %llu\n", (unsigned long long) eval);
		while (eval > 0) {
			tmo.tv_sec = 0;
			tmo.tv_nsec = 0;
			r = io_getevents(ctx, 1, eval > NUM_EVENTS ? NUM_EVENTS: (long) eval,
					 events, &tmo);
			if (r > 0) {
				for (j = 0; j < r; j++) {

				}
				i += r;
				eval -= r;
				fprintf(stdout, "test_write got %ld/%ld results so far\n",
					i, n);
			}
		}
	}
	free(iocb);
	free(piocb);

	return n;
}

static long test_write(aio_context_t ctx, int fd, long range, int afd) {
	long i, n, r, j;
	u_int64_t eval;
	struct iocb **piocb;
	struct iocb *iocb;
	struct timespec tmo;
	static struct io_event events[NUM_EVENTS];
	static char buf[IORTX_SIZE];

	for (i = 0; i < IORTX_SIZE; i++)
		buf[i] = i & 0xff;
	n = range / IORTX_SIZE;
	iocb = malloc(n * sizeof(struct iocb));
	piocb = malloc(n * sizeof(struct iocb *));
	if (!iocb || !piocb) {
		perror("iocb alloc");
		return -1;
	}
	for (i = 0; i < n; i++) {
		piocb[i] = &iocb[i];
		asyio_prep_pwrite(&iocb[i], fd, buf, sizeof(buf),
				  (n - i - 1) * IORTX_SIZE, afd);
		iocb[i].aio_data = (u_int64_t) i + 1;
	}
	fprintf(stdout, "submitting write requests (%ld) ...\n", n);
	if ((r = io_submit(ctx, n, piocb)) <= 0) {
		perror("io_submit");
		return -1;
	}
	fprintf(stdout, "submitted %ld requests\n", r);
	for (i = 0; i < n;) {
		fprintf(stdout, "waiting ... "), fflush(stdout);
		waitasync(afd, -1);
		eval = 0;
		if (read(afd, &eval, sizeof(eval)) != sizeof(eval))
			perror("read");
		fprintf(stdout, "done! %llu\n", (unsigned long long) eval);
		while (eval > 0) {
			tmo.tv_sec = 0;
			tmo.tv_nsec = 0;
			r = io_getevents(ctx, 1, eval > NUM_EVENTS ? NUM_EVENTS: (long) eval,
					 events, &tmo);
			if (r > 0) {
				for (j = 0; j < r; j++) {

				}
				i += r;
				eval -= r;
				fprintf(stdout, "test_write got %ld/%ld results so far\n",
					i, n);
			}
		}
	}
	free(iocb);
	free(piocb);

	return n;
}

int main(int ac, char **av) {
	int afd, fd;
	aio_context_t ctx = 0;
	char const *testfn = "/tmp/eventfd-aio-test.data";

	BUILD_BUG_IF(sizeof(struct iocb) != 64);

	fprintf(stdout, "creating an eventfd ...\n");
	if ((afd = eventfd(0)) == -1) {
		perror("eventfd");
		return 2;
	}
	fprintf(stdout, "done! eventfd = %d\n", afd);
	if (io_setup(TESTFILE_SIZE / IORTX_SIZE + 256, &ctx)) {
		perror("io_setup");
		return 3;
	}
	if ((fd = open(testfn, O_RDWR | O_CREAT, 0644)) == -1) {
		perror(testfn);
		return 4;
	}
	ftruncate(fd, TESTFILE_SIZE);

	fcntl(afd, F_SETFL, fcntl(afd, F_GETFL, 0) | O_NONBLOCK);

	test_write(ctx, fd, TESTFILE_SIZE, afd);
	test_read(ctx, fd, TESTFILE_SIZE, afd);

	io_destroy(ctx);
	close(fd);
	close(afd);
	remove(testfn);

	return 0;
}

