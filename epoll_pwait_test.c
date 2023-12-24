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


#include <linux/unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <sys/epoll.h>



#define SIZEOF_SIGSET (_NSIG / 8)



#define __sys_epoll_pwait(epfd, pevents, maxevents, timeout, sigmask, sigsetsize) \
			_syscall6(int, epoll_pwait, int, epfd, struct epoll_event *, pevents, \
					  int, maxevents, int, timeout, const sigset_t *, sigmask, size_t, sigsetsize)


__sys_epoll_pwait(epfd, pevents, maxevents, timeout, sigmask, sigsetsize);



static void sig_handler(int sig) {

}

int main(int ac, char **av) {
	int epfd, error, status, fails = 0;
	pid_t cpid;
	sigset_t sigmask, curmask;
	struct epoll_event evt;
	struct sigaction sa;
	int pfds[2];

	if ((epfd = epoll_create(1)) == -1) {
		perror("epoll_create(1)");
		return 1;
	}
	if (pipe(pfds) < 0) {
		perror("pipe()");
		return 1;
	}

	memset(&evt, 0, sizeof(evt));
	evt.events = EPOLLIN;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, pfds[0], &evt) < 0) {
		perror("epoll_ctl(EPOLL_CTL_ADD)");
		return 1;
	}
	fprintf(stdout, "Testing from wrong epfd type            : ");
	if ((error = epoll_pwait(pfds[0], &evt, 1, 0, NULL, 0)) != -1)
		fprintf(stdout, "FAIL (%d returned instead of -1)\n", error), fails++;
	else if (errno != EINVAL)
		fprintf(stdout, "FAIL (errno is %d instead of EINVAL)\n", errno), fails++;
	else
		fprintf(stdout, "OK\n");

	fprintf(stdout, "Testing from closed epfd type           : ");
	close(17);
	if ((error = epoll_pwait(17, &evt, 1, 0, NULL, 0)) != -1)
		fprintf(stdout, "FAIL (%d returned instead of -1)\n", error), fails++;
	else if (errno != EBADF)
		fprintf(stdout, "FAIL (errno is %d instead of EBADF)\n", errno), fails++;
	else
		fprintf(stdout, "OK\n");

	fprintf(stdout, "Testing numevents <= 0                  : ");
	if ((error = epoll_pwait(epfd, &evt, 0, 0, NULL, 0)) != -1)
		fprintf(stdout, "FAIL (%d returned instead of -1)\n", error), fails++;
	else if (errno != EINVAL)
		fprintf(stdout, "FAIL (errno is %d instead of EINVAL)\n", errno), fails++;
	else
		fprintf(stdout, "OK\n");

	fprintf(stdout, "Testing numevents too big               : ");
	if ((error = epoll_pwait(epfd, &evt, INT_MAX, 0, NULL, 0)) != -1)
		fprintf(stdout, "FAIL (%d returned instead of -1)\n", error), fails++;
	else if (errno != EINVAL)
		fprintf(stdout, "FAIL (errno is %d instead of EINVAL)\n", errno), fails++;
	else
		fprintf(stdout, "OK\n");

	fprintf(stdout, "Testing wrong sigmask size              : ");
	if ((error = epoll_pwait(epfd, &evt, 1, 0, &sigmask, SIZEOF_SIGSET / 2)) != -1)
		fprintf(stdout, "FAIL (%d returned instead of -1)\n", error), fails++;
	else if (errno != EINVAL)
		fprintf(stdout, "FAIL (errno is %d instead of EINVAL)\n", errno), fails++;
	else
		fprintf(stdout, "OK\n");


	fprintf(stdout, "Testing getevents when none available   : ");
	if ((error = epoll_pwait(epfd, &evt, 1, 0, NULL, 0)) > 0)
		fprintf(stdout, "FAIL (%d returned instead of 0)\n", error), fails++;
	else
		fprintf(stdout, "OK\n");

	write(pfds[1], "w", 1);
	fprintf(stdout, "Testing getevents when one available    : ");
	if ((error = epoll_pwait(epfd, &evt, 1, 0, NULL, 0)) != 1)
		fprintf(stdout, "FAIL (%d returned instead of 1)\n", error), fails++;
	else {
		fprintf(stdout, "OK\n");
		read(pfds[0], &error, 1);
	}


	memset(&sa, 0, sizeof(sa));
	sa.sa_restorer = NULL;
	sa.sa_handler = sig_handler;
	sigfillset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGUSR1, &sa, NULL)) {
		perror("sigaction");
		return 1;
	}
	if (sigprocmask(SIG_SETMASK, NULL, &curmask)) {
		perror("sigprocmask(SIG_SETMASK, NULL, &curmask)");
		return 1;
	}

	fprintf(stdout, "Testing signal when blocked             : ");
	sigaddset(&curmask, SIGUSR1);
	if ((cpid = fork()) == (pid_t) -1) {
		perror("fork");
		return 1;
	} else if (cpid == 0) {
		usleep(200000);
		kill(getppid(), SIGUSR1);
		exit(0);
	}
	if ((error = epoll_pwait(epfd, &evt, 1, 1000, &curmask, SIZEOF_SIGSET)) != 0)
		fprintf(stdout, "FAIL (%d returned instead of 0 - errno = %d)\n",
			error, errno), fails++;
	else
		fprintf(stdout, "OK\n");
	waitpid(cpid, &status, 0);

	fprintf(stdout, "Testing signal when non blocked         : ");
	sigdelset(&curmask, SIGUSR1);
	if ((cpid = fork()) == (pid_t) -1) {
		perror("fork");
		return 1;
	} else if (cpid == 0) {
		usleep(200000);
		kill(getppid(), SIGUSR1);
		exit(0);
	}
	if ((error = epoll_pwait(epfd, &evt, 1, 1000, &curmask, SIZEOF_SIGSET)) != -1)
		fprintf(stdout, "FAIL (%d returned instead of -1)\n", error), fails++;
	else if (errno != EINTR)
		fprintf(stdout, "FAIL (errno is %d instead of EINTR)\n", errno), fails++;
	else
		fprintf(stdout, "OK\n");
	waitpid(cpid, &status, 0);


	return fails ? 2: 0;
}

