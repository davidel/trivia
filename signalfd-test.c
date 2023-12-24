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
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

/*
 * This were good at the time of 2.6.22-rc3 ...
 */
#ifndef __NR_signalfd
#if defined(__x86_64__)
#define __NR_signalfd 282
#elif defined(__i386__)
#define __NR_signalfd 321
#else
#error Cannot detect your architecture!
#endif
#endif

#define SIZEOF_SIG (_NSIG / 8)
#define SIZEOF_SIGSET (SIZEOF_SIG > sizeof(sigset_t) ? sizeof(sigset_t): SIZEOF_SIG)
#define COUNTOF(a) (sizeof(a) / sizeof(a[0]))

#define TEST_SIG SIGUSR2
#define SIGNALFDS_X_EPOLLFD 10
#define SIGNALFD_CREATE_COUNT 1000000
#define FORK_PROC_COUNT 5000

struct signalfd_siginfo {
	u_int32_t ssi_signo;
	int32_t ssi_errno;
	int32_t ssi_code;
	u_int32_t ssi_pid;
	u_int32_t ssi_uid;
	int32_t ssi_fd;
	u_int32_t ssi_tid;
	u_int32_t ssi_band;
	u_int32_t ssi_overrun;
	u_int32_t ssi_trapno;
	int32_t ssi_status;
	int32_t ssi_int;
	u_int64_t ssi_ptr;
	u_int64_t ssi_utime;
	u_int64_t ssi_stime;
	u_int64_t ssi_addr;
	u_int8_t __pad[48];
};

struct epoll_signalfds {
	int efd;
	int n;
	int *sfds;
};

#if defined(USE_PTHREAD)

typedef pthread_t thread_id_t;

#else

typedef pid_t thread_id_t;

#endif


static int sfd;
static thread_id_t thid[8];
static unsigned long tids[8];


static int signalfd(int ufc, sigset_t const *mask, size_t sizemask)
{
	return syscall(__NR_signalfd, ufc, mask, sizemask);
}

static int xsignalfd(int ufc, sigset_t const *mask, size_t sizemask)
{
	int fd;

	if ((fd = signalfd(ufc, mask, sizemask)) == -1) {
		perror("creating signalfd");
		exit(1);
	}

	return fd;
}

static int xsignalfd_nb(int ufc, sigset_t const *mask, size_t sizemask)
{
	int fd;

	fd = xsignalfd(ufc, mask, sizemask);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	return fd;
}

static int xepollfd(void)
{
	int fd;

	if ((fd = epoll_create(1)) == -1) {
		perror("creating epoll fd");
		exit(1);
	}

	return fd;
}

static void xepoll_add(int efd, int fd, unsigned int events,
		       u_int64_t data)
{
	struct epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = events;
	ev.data.u64 = data;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		perror("adding to an epoll fd");
		exit(1);
	}
}

static void xepoll_wait_sigs(int efd, int timeo, int verbose)
{
	int i, n, fd, count;
	struct signalfd_siginfo info;
	struct epoll_event events[16];

	for (count = 0;
	     (n = epoll_wait(efd, events, COUNTOF(events), timeo)) > 0;
	     count += n) {
		for (i = 0; i < n; i++) {
			fd = (int) events[i].data.u64;
			if (read(fd, &info, sizeof(info)) != sizeof(info)) {
				if (verbose)
					fprintf(stderr,
						"[%u] signalfd %d ready, but signal "
						"has been stolen (this is OK)\n",
						getpid(), fd);
			} else {
				if (verbose)
					fprintf(stdout,
						"[%u] signal %d (%s) read from signalfd %d\n",
						getpid(), (int) info.ssi_signo,
						strsignal((int) info.ssi_signo), fd);
			}
		}
	}
	if (verbose && count == 0)
		fprintf(stdout, "[%u] timeout!\n", getpid());
}

static long waitsig(int sfd, int timeo)
{
	int n;
	struct pollfd pfd;
	struct signalfd_siginfo info;

	pfd.fd = sfd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, timeo) < 0) {
		perror("poll");
		return -1;
	}
	if ((pfd.revents & POLLIN) == 0) {
		fprintf(stdout, "no signals\n");
		return -1;
	}
	if ((n = read(sfd, &info, sizeof(info))) < 0) {
		perror("signal dequeue");
		return -1;
	} else if (n == 0) {
		fprintf(stdout, "task detached the sighand\n");
		return 0;
	}

	return info.ssi_signo;
}

static int gettid(void)
{
	return syscall(__NR_gettid);
}

static int tkill(unsigned long tid, int sig)
{
	return syscall(__NR_tkill, tid, sig);
}

#if defined(USE_PTHREAD)

static thread_id_t thread_new(void *(*proc)(void *), void *data)
{
	pthread_t tid;

	if (pthread_create(&tid, NULL, proc, data) != 0) {
		perror("pthread_create()");
		return 0;
	}

	return tid;
}

static int thread_wait(thread_id_t tid)
{
	if (pthread_join(tid, NULL)) {
		perror("pthread_wait()");
		return -1;
	}

	return 0;
}

static thread_id_t thread_id(void)
{
	return gettid();
}

#else

#define THREAD_STK_SIZE (1024 * 64)

static thread_id_t thread_new(void *(*proc)(void *), void *data)
{
	int tid;
	char *stk;

	stk = malloc(THREAD_STK_SIZE);
	if ((tid = clone((int (*)(void *)) proc, stk + THREAD_STK_SIZE - sizeof(long),
			 CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_VM | SIGCHLD,
			 data)) < 0) {
		perror("clone()");
		return 0;
	}

	return tid;
}

static int thread_wait(thread_id_t tid)
{
	if (waitpid(tid, NULL, __WALL) != tid) {
		perror("waitpid()");
		return -1;
	}

	return 0;
}

static thread_id_t thread_id(void)
{
	return gettid();
}

#endif

static thread_id_t xthread_new(void *(*proc)(void *), void *data)
{
	pthread_t tid;

	if ((tid = thread_new(proc, data)) == 0)
		exit(1);

	return tid;
}

static void dummy_sig(int sig)
{
	fprintf(stderr, "*** got REAL signal %d (%s)\n", sig, strsignal(sig));
}

static void *thproc(void *data)
{
	long thn = (long) data, sig;

	tids[thn] = thread_id();
	fprintf(stdout, "thread %ld tid is %lu pgrp=%d\n", thn, tids[thn], getpgrp());
	while ((sig = waitsig(sfd, -1)) > 0) {
		fprintf(stdout, "thread %ld got sig = %ld (%s)\n",
			thn, sig, strsignal(sig));
		if (sig == TEST_SIG)
			break;
	}
	fprintf(stdout, "thread %ld quit (sig = %ld)\n", thn, sig);
	kill(0, TEST_SIG);

	return NULL;
}

static void run_proc_in_child(void (*cproc)(void *), void *cdata,
			      void (*pproc)(void *), void *pdata)
{
	int status;
	pid_t pid;

	if ((pid = fork()) == 0) {
		(*cproc)(cdata);
		exit(0);
	} else if (pid == -1) {
		perror("creating child process");
		exit(1);
	}
	if (pproc != NULL)
		(*pproc)(pdata);
	if (waitpid(pid, &status, 0) != pid) {
		fprintf(stderr, "failed waiting for process %u\n", pid);
		exit(1);
	}
}

static int xcreate_epoll_signalfds(int n, int *sfds, int verbose)
{
	int i, efd, fd;
	sigset_t sset;

	efd = xepollfd();

	sigfillset(&sset);
	sigdelset(&sset, SIGINT);

	for (i = 0; i < n; i++) {
		fd = xsignalfd_nb(-1, &sset, SIZEOF_SIGSET);
		if (verbose)
			fprintf(stdout, "[%u] signalfd = %d\n", getpid(), fd);

		xepoll_add(efd, fd, EPOLLIN, fd);

		if (sfds != NULL)
			sfds[i] = fd;
	}

	return efd;
}

static void epoll_test_1(void *data)
{
	int efd;

	fprintf(stdout, "[%u] epoll test 1 (many signalfds inside epoll)\n",
		getpid());
	efd = xcreate_epoll_signalfds(SIGNALFDS_X_EPOLLFD, NULL, 1);

	fprintf(stdout, "sending SIGUSR1\n");
	kill(0, SIGUSR1);

	xepoll_wait_sigs(efd, 250, 1);
}

static void epoll_parent_proc_2(void *data)
{
	int sig = (int) (long) data;

	usleep(100000);
	fprintf(stdout, "[%u] sending %d (%s)\n", getpid(), sig,
		strsignal(sig));
	kill(0, sig);
}

static void epoll_child_proc_2(void *data)
{
	int efd = (int) (long) data;

	xepoll_wait_sigs(efd, 250, 1);
}

static void epoll_test_2(void *data)
{
	int efd;

	fprintf(stdout, "[%u] epoll test 2 (many signalfds inside epoll, fork,\n"
		"\tsend signal from parent)\n", getpid());
	efd = xcreate_epoll_signalfds(SIGNALFDS_X_EPOLLFD, NULL, 1);

	run_proc_in_child(epoll_child_proc_2, (void *) (long) efd,
			  epoll_parent_proc_2, (void *) SIGUSR1);

	xepoll_wait_sigs(efd, 250, 1);
}

static void epoll_child_proc_3(void *data)
{
	int efd = (int) (long) data, sig = SIGUSR1;

	usleep(100000);
	fprintf(stdout, "[%u] sending %d (%s)\n", getpid(), sig,
		strsignal(sig));
	kill(0, sig);

	xepoll_wait_sigs(efd, 250, 1);
}

static void epoll_test_3(void *data)
{
	int efd;

	fprintf(stdout, "[%u] epoll test 3 (many signalfds inside epoll, fork,\n"
		"\tsend signal from child)\n", getpid());
	efd = xcreate_epoll_signalfds(SIGNALFDS_X_EPOLLFD, NULL, 1);

	run_proc_in_child(epoll_child_proc_3, (void *) (long) efd,
			  NULL, NULL);

	xepoll_wait_sigs(efd, 250, 1);
}

static void epoll_child_proc_4(void *data)
{
	struct epoll_signalfds *esf = (struct epoll_signalfds *) data;
	int i;

	fprintf(stdout, "[%u] child waiting\n", getpid());
	usleep(500000);

	xepoll_wait_sigs(esf->efd, 250, 1);

	for (i = 0; i < esf->n; i++)
		epoll_ctl(esf->efd, EPOLL_CTL_DEL, esf->sfds[i], NULL);
}

static void epoll_parent_proc_4(void *data)
{
	struct epoll_signalfds *esf = (struct epoll_signalfds *) data;
	int sig = SIGUSR1;

	fprintf(stdout, "[%u] parent sending %d (%s)\n", getpid(), sig,
		strsignal(sig));
	kill(0, sig);

	exit(0);
}

static void epoll_test_4(void *data)
{
	static struct epoll_signalfds esf;
	static int sfds[SIGNALFDS_X_EPOLLFD];

	esf.n = SIGNALFDS_X_EPOLLFD;
	esf.sfds = sfds;

	fprintf(stdout, "[%u] epoll test 4 (many signalfds inside epoll, fork,\n"
		"\tsend signal from parent, parent exit before child)\n",
		getpid());
	esf.efd = xcreate_epoll_signalfds(esf.n, esf.sfds, 1);

	run_proc_in_child(epoll_child_proc_4, &esf,
			  epoll_parent_proc_4, &esf);
}

static void stress_child_proc(void *data)
{
	int efd;

	efd = xcreate_epoll_signalfds(SIGNALFDS_X_EPOLLFD, NULL, 0);

	kill(getpid(), SIGUSR1);
	xepoll_wait_sigs(efd, 0, 0);
}

static void *signalfd_create_thproc(void *data)
{
	int i, fd, n = (int) (long) data;
	sigset_t sset;

	fprintf(stdout, "[%u] signalfd creator enter\n", getpid());

	sigfillset(&sset);
	sigdelset(&sset, SIGINT);

	for (i = 0; i < n; i++) {
		fd = xsignalfd(-1, &sset, SIZEOF_SIGSET);
		close(fd);
	}

	fprintf(stdout, "[%u] signalfd creator exit\n", getpid());

	return NULL;
}

static void *forker_thproc(void *data)
{
	int i, n = (int) (long) data;

	fprintf(stdout, "[%u] forker enter\n", getpid());

	for (i = 0; i < n; i++)
		run_proc_in_child(stress_child_proc, NULL, NULL, NULL);

	fprintf(stdout, "[%u] forker exit\n", getpid());

	return NULL;
}

static void signalfd_stress_fork(void *data)
{
	int efd;
	thread_id_t ths, thf;

	fprintf(stdout, "[%u] signalfd plus fork multi-thread stress test ...\n",
		getpid());

	efd = xcreate_epoll_signalfds(SIGNALFDS_X_EPOLLFD, NULL, 1);

	ths = xthread_new(signalfd_create_thproc,
			  (void *) (long) SIGNALFD_CREATE_COUNT);
	thf = xthread_new(forker_thproc, (void *) (long) FORK_PROC_COUNT);

	thread_wait(thf);
	thread_wait(ths);
}

int main(int ac, char **av)
{
	int i, sfd2, sigs;
	long lsig;
	pid_t pid;
	struct signalfd_siginfo info;
	sigset_t sset, oset;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	signal(TEST_SIG, dummy_sig);
	sigfillset(&sset);
	sigdelset(&sset, SIGINT);
	sigprocmask(SIG_BLOCK, &sset, &oset);
	sfd = xsignalfd(-1, &sset, SIZEOF_SIGSET);
	fprintf(stdout, "signalfd = %d\n", sfd);

	fprintf(stdout, "creating child (SIGCHLD test) ...\n");
	if ((pid = fork()) == 0) {
		fprintf(stdout, "child exit\n");
		exit(0);
	}
	fprintf(stdout, "waiting  SIGCHLD ...\n");
	lsig = waitsig(sfd, 1000);
	fprintf(stdout, "got sig = %ld (%s)\n\n", lsig, strsignal(lsig));

	fprintf(stdout, "creating child (child send SIGUSR1 test) ...\n");
	if ((pid = fork()) == 0) {
		fprintf(stdout, "child sends SIGUSR1\n");
		kill(getppid(), SIGUSR1);
		exit(0);
	}
	fprintf(stdout, "waiting signal ...\n");
	lsig = waitsig(sfd, 1000);
	fprintf(stdout, "got sig = %ld (%s) - expect %d (%s)\n",
		lsig, strsignal(lsig), SIGUSR1, strsignal(SIGUSR1));
	fprintf(stdout, "waiting signal ...\n");
	lsig = waitsig(sfd, 1000);
	fprintf(stdout, "got sig = %ld (%s) - expect %d (%s)\n",
		lsig, strsignal(lsig), SIGCHLD, strsignal(SIGCHLD));
	fputs("\n", stdout);

	fprintf(stdout, "creating child (parent send SIGUSR1 test) ...\n");
	if ((pid = fork()) == 0) {
		fprintf(stdout, "child waiting signal ...\n");
		lsig = waitsig(sfd, 1000);
		fprintf(stdout, "child got sig = %ld (%s) - expect %d (%s)\n",
			lsig, strsignal(lsig), SIGUSR1, strsignal(SIGUSR1));

		exit(0);
	}
	fprintf(stdout, "parent sends SIGUSR1\n");
	kill(pid, SIGUSR1);
	usleep(250000);
	fprintf(stdout, "waiting signal ...\n");
	lsig = waitsig(sfd, 1000);
	fprintf(stdout, "got sig = %ld (%s) - expect %d (%s)\n\n",
		lsig, strsignal(lsig), SIGCHLD, strsignal(SIGCHLD));

	fprintf(stdout, "setting new mask ...\n");
	sigfillset(&sset);
	sigdelset(&sset, SIGUSR1);
	sfd = xsignalfd(sfd, &sset, SIZEOF_SIGSET);
	fprintf(stdout, "new signalfd = %d\n", sfd);
	fprintf(stdout, "sending SIGUSR1\n");
	kill(0, SIGUSR1);
	fprintf(stdout, "waiting SIGUSR1 ...\n");
	if ((lsig = waitsig(sfd, 0)) > 0)
		fprintf(stdout, "whooops! got sig = %ld (%s)\n", lsig, strsignal(lsig));
	else
		fprintf(stdout, "no signal, correct\n");
	fputs("\n", stdout);

	fprintf(stdout, "creating new signalfd (multiple fd receive test) ...\n");
	sigfillset(&sset);
	sfd = xsignalfd(sfd, &sset, SIZEOF_SIGSET);
	fprintf(stdout, "new signalfd = %d\n", sfd);
	if ((sfd2 = signalfd(-1, &sset, SIZEOF_SIGSET)) == -1) {
		perror("signalfd");
		return 1;
	}
	fprintf(stdout, "signalfd2 = %d\n", sfd2);
	fprintf(stdout, "parent sends SIGUSR1\n");
	kill(0, SIGUSR1);
	sigs = 0;
	if ((lsig = waitsig(sfd, 0)) > 0)
		sigs++;
	fprintf(stdout, "1st fd got sig = %ld (%s)\n", lsig, strsignal(lsig));
	if ((lsig = waitsig(sfd2, 0)) > 0)
		sigs++;
	fprintf(stdout, "2nd fd got sig = %ld (%s)\n", lsig, strsignal(lsig));
	if (sigs > 1)
		fprintf(stdout, "whooops! got 2 sigs instead of one!\n");
	fputs("\n", stdout);
	close(sfd2);

	fprintf(stdout, "multi-thread test ...\n");
	for (i = 0; i < 8; i++) {
		thid[i] = xthread_new(thproc, (void *) (long) i);
		fprintf(stdout, "thread %d is %ld (%p)\n", i, (long) thid[i],
			(void *) (long) thid[i]);
	}
	sleep(1);

	fprintf(stdout, "sending signal %d (%s) pgrp=%d ...\n",
		TEST_SIG, strsignal(TEST_SIG), getpgrp());
	kill(0, TEST_SIG);
	for (i = 0; i < 8; i++) {
		fprintf(stdout, "waiting for thread %d\n", i);
		thread_wait(thid[i]);
	}

	while ((i = waitsig(sfd, 0)) > 0)
		fprintf(stdout, "flushing signal %d (%s)\n", i, strsignal(i));

	fprintf(stdout, "setting O_NONBLOCK (non blocking read test) ...\n");
	fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL, 0) | O_NONBLOCK);
	if (read(sfd, &info, sizeof(info)) > 0)
		fprintf(stdout, "whooops! read signal when should have not\n\n");
	else if (errno != EAGAIN)
		fprintf(stdout, "whooops! bad errno value (%d = '%s')!\n\n",
			errno, strerror(errno));
	else
		fprintf(stdout, "success\n\n");

	fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL, 0) & ~O_NONBLOCK);
	close(sfd);

	run_proc_in_child(epoll_test_1, NULL, NULL, NULL);

	run_proc_in_child(epoll_test_2, NULL, NULL, NULL);

	run_proc_in_child(epoll_test_3, NULL, NULL, NULL);

	run_proc_in_child(epoll_test_4, NULL, NULL, NULL);
	sleep(2);

	run_proc_in_child(signalfd_stress_fork, NULL, NULL, NULL);

	return 0;
}

