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
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/wait.h>

#define EPWAIT_TIMEO	(1 * 1000)
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif

#define EPOLL_MAX_CHAIN	100L

#define EPOLL_TF_LOOP (1 << 0)

struct epoll_test_cfg {
	long size;
	long flags;
};

static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);

	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

static int xepoll_create(int n) {
	int epfd;

	if ((epfd = epoll_create(n)) == -1) {
		perror("epoll_create");
		exit(2);
	}

	return epfd;
}

static void xepoll_ctl(int epfd, int cmd, int fd, struct epoll_event *evt) {
	if (epoll_ctl(epfd, cmd, fd, evt) < 0) {
		perror("epoll_ctl");
		exit(3);
	}
}

static void xpipe(int *fds) {
	if (pipe(fds)) {
		perror("pipe");
		exit(4);
	}
}

static pid_t xfork(void) {
	pid_t pid;

	if ((pid = fork()) == (pid_t) -1) {
		perror("pipe");
		exit(5);
	}

	return pid;
}

static int run_forked_proc(int (*proc)(void *), void *data) {
	int status;
	pid_t pid;

	if ((pid = xfork()) == 0)
		exit((*proc)(data));
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}

	return WIFEXITED(status) ? WEXITSTATUS(status): -2;
}

static int check_events(int fd, int timeo) {
	struct pollfd pfd;

	fprintf(stdout, "Checking events for fd %d\n", fd);
	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = POLLIN | POLLOUT;
	if (poll(&pfd, 1, timeo) < 0) {
		perror("poll()");
		return 0;
	}
	if (pfd.revents & POLLIN)
		fprintf(stdout, "\tPOLLIN\n");
	if (pfd.revents & POLLOUT)
		fprintf(stdout, "\tPOLLOUT\n");
	if (pfd.revents & POLLERR)
		fprintf(stdout, "\tPOLLERR\n");
	if (pfd.revents & POLLHUP)
		fprintf(stdout, "\tPOLLHUP\n");
	if (pfd.revents & POLLRDHUP)
		fprintf(stdout, "\tPOLLRDHUP\n");

	return pfd.revents;
}

static int epoll_test_tty(void *data) {
	int epfd, ifd = fileno(stdin), res;
	struct epoll_event evt;

	if (check_events(ifd, 0) != POLLOUT) {
		fprintf(stderr, "Something is cooking on STDIN (%d)\n", ifd);
		return 1;
	}
	epfd = xepoll_create(1);
	fprintf(stdout, "Created epoll fd (%d)\n", epfd);
	memset(&evt, 0, sizeof(evt));
	evt.events = EPOLLIN;
	xepoll_ctl(epfd, EPOLL_CTL_ADD, ifd, &evt);
	if (check_events(epfd, 0) & POLLIN) {
		res = epoll_wait(epfd, &evt, 1, 0);
		if (res == 0) {
			fprintf(stderr, "Epoll fd (%d) is ready when it shouldn't!\n",
				epfd);
			return 2;
		}
	}

	return 0;
}

static int epoll_test_tty2(void *data) {
	int epfd, ifd = fileno(stdin), res, status;
	pid_t pid;
	unsigned long long ts, te;
	struct epoll_event evt;

	epfd = xepoll_create(1);
	fprintf(stdout, "Created epoll fd (%d)\n", epfd);
	memset(&evt, 0, sizeof(evt));
	evt.events = EPOLLIN;
	xepoll_ctl(epfd, EPOLL_CTL_ADD, ifd, &evt);
	if ((pid = xfork()) == 0) {
		/*
		 * Wait for teh parent to fall into epoll_wait().
		 */
		sleep(1);
		fprintf(stdout, "Child stuff!\n");
		fflush(stdout);
		exit(0);
	}
	/*
	 * We shouln't be woken up by the child writing TTY. We measure
	 * the time spent in epoll_wait() for that.
	 */
	ts = getustime();
	res = epoll_wait(epfd, &evt, 1, 2500);
	te = getustime();
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return 1;
	}
	if (res != 0) {
		fprintf(stderr, "Epoll fd (%d) is ready when it shouldn't!\n",
			epfd);
		return 2;
	}
	/*
	 * We use epoll_wait() with 2500ms timeout. The spurious wakeup,
	 * if present, should come right after 1000ms. Give some tollerance
	 * and use 2000ms as threshold between the eventual wakeup and the
	 * timeout value.
	 */
	if (te - ts < 2000000ULL) {
		fprintf(stderr, "Epoll fd (%d) got a spurious wakeup, no good!\n",
			epfd);
		return 3;
	}

	return 0;
}

static int epoll_wakeup_chain(void *data) {
	struct epoll_test_cfg *tcfg = data;
	int i, res, epfd, bfd, nfd, pfds[2];
	pid_t pid;
	struct epoll_event evt;

	memset(&evt, 0, sizeof(evt));
	evt.events = EPOLLIN;

	epfd = bfd = xepoll_create(1);

	for (i = 0; i < tcfg->size; i++) {
		nfd = xepoll_create(1);
		xepoll_ctl(bfd, EPOLL_CTL_ADD, nfd, &evt);
		bfd = nfd;
	}
	xpipe(pfds);
	if (tcfg->flags & EPOLL_TF_LOOP)
	{
		xepoll_ctl(bfd, EPOLL_CTL_ADD, epfd, &evt);
		/*
		 * If we're testing for loop, we want that the wakeup
		 * triggered by the write to the pipe done in the child
		 * process, triggers a fake event. So we add the pipe
		 * read size with EPOLLOUT events. This will trigger
		 * an addition to the ready-list, but no real events
		 * will be there. The the epoll kernel code will proceed
		 * in calling f_op->poll() of the epfd, triggering the
		 * loop we want to test.
		 */
		evt.events = EPOLLOUT;
	}
	xepoll_ctl(bfd, EPOLL_CTL_ADD, pfds[0], &evt);

	/*
	 * The pipe write must come after the poll(2) call inside
	 * check_events(). This tests the nested wakeup code in
	 * fs/eventpoll.c:ep_poll_safewake()
	 * By having the check_events() (hence poll(2)) happens first,
	 * we have poll wait queue filled up, and the write(2) in the
	 * child will trigger the wakeup chain.
	 */
	if ((pid = xfork()) == 0) {
		sleep(1);
		write(pfds[1], "w", 1);
		exit(0);
	}

	res = check_events(epfd, 2000) & POLLIN;

	if (waitpid(pid, NULL, 0) != pid) {
		perror("waitpid");
		return -1;
	}

	return res;
}

static int epoll_poll_chain(void *data) {
	struct epoll_test_cfg *tcfg = data;
	int i, res, epfd, bfd, nfd, pfds[2];
	pid_t pid;
	struct epoll_event evt;

	memset(&evt, 0, sizeof(evt));
	evt.events = EPOLLIN;

	epfd = bfd = xepoll_create(1);

	for (i = 0; i < tcfg->size; i++) {
		nfd = xepoll_create(1);
		xepoll_ctl(bfd, EPOLL_CTL_ADD, nfd, &evt);
		bfd = nfd;
	}
	xpipe(pfds);
	if (tcfg->flags & EPOLL_TF_LOOP)
	{
		xepoll_ctl(bfd, EPOLL_CTL_ADD, epfd, &evt);
		/*
		 * If we're testing for loop, we want that the wakeup
		 * triggered by the write to the pipe done in the child
		 * process, triggers a fake event. So we add the pipe
		 * read size with EPOLLOUT events. This will trigger
		 * an addition to the ready-list, but no real events
		 * will be there. The the epoll kernel code will proceed
		 * in calling f_op->poll() of the epfd, triggering the
		 * loop we want to test.
		 */
		evt.events = EPOLLOUT;
	}
	xepoll_ctl(bfd, EPOLL_CTL_ADD, pfds[0], &evt);

	/*
	 * The pipe write mush come before the poll(2) call inside
	 * check_events(). This tests the nested f_op->poll calls code in
	 * fs/eventpoll.c:ep_eventpoll_poll()
	 * By having the pipe write(2) happen first, we make the kernel
	 * epoll code to load the ready lists, and the following poll(2)
	 * done inside check_events() will test nested poll code in
	 * ep_eventpoll_poll().
	 */
	if ((pid = xfork()) == 0) {
		write(pfds[1], "w", 1);
		exit(0);
	}
	sleep(1);
	res = check_events(epfd, 1000) & POLLIN;

	if (waitpid(pid, NULL, 0) != pid) {
		perror("waitpid");
		return -1;
	}

	return res;
}

int main(int ac, char **av) {
	int error;
	struct epoll_test_cfg tcfg;

	fprintf(stdout, "\n********** Testing TTY events\n");
	error = run_forked_proc(epoll_test_tty, NULL);
	fprintf(stdout, error == 0 ?
		"********** OK\n": "********** FAIL (%d)\n", error);

	fprintf(stdout, "\n********** Testing TTY spurious wakeups\n");
	error = run_forked_proc(epoll_test_tty2, NULL);
	fprintf(stdout, error == 0 ?
		"********** OK\n": "********** FAIL (%d)\n", error);

	tcfg.size = 3;
	tcfg.flags = 0;
	fprintf(stdout, "\n********** Testing short wakeup chain\n");
	error = run_forked_proc(epoll_wakeup_chain, &tcfg);
	fprintf(stdout, error == POLLIN ?
		"********** OK\n": "********** FAIL (%d)\n", error);

	tcfg.size = EPOLL_MAX_CHAIN;
	tcfg.flags = 0;
	fprintf(stdout, "\n********** Testing long wakeup chain (HOLD ON)\n");
	error = run_forked_proc(epoll_wakeup_chain, &tcfg);
	fprintf(stdout, error == 0 ?
		"********** OK\n": "********** FAIL (%d)\n", error);

	tcfg.size = 3;
	tcfg.flags = 0;
	fprintf(stdout, "\n********** Testing short poll chain\n");
	error = run_forked_proc(epoll_poll_chain, &tcfg);
	fprintf(stdout, error == POLLIN ?
		"********** OK\n": "********** FAIL (%d)\n", error);

	tcfg.size = EPOLL_MAX_CHAIN;
	tcfg.flags = 0;
	fprintf(stdout, "\n********** Testing long poll chain (HOLD ON)\n");
	error = run_forked_proc(epoll_poll_chain, &tcfg);
	fprintf(stdout, error == 0 ?
		"********** OK\n": "********** FAIL (%d)\n", error);

	tcfg.size = 3;
	tcfg.flags = EPOLL_TF_LOOP;
	fprintf(stdout, "\n********** Testing loopy wakeup chain (HOLD ON)\n");
	error = run_forked_proc(epoll_wakeup_chain, &tcfg);
	fprintf(stdout, error == 0 ?
		"********** OK\n": "********** FAIL (%d)\n", error);

	tcfg.size = 3;
	tcfg.flags = EPOLL_TF_LOOP;
	fprintf(stdout, "\n********** Testing loopy poll chain (HOLD ON)\n");
	error = run_forked_proc(epoll_poll_chain, &tcfg);
	fprintf(stdout, error == 0 ?
		"********** OK\n": "********** FAIL (%d)\n", error);


	return 0;
}

