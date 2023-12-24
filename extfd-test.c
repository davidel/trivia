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
#include <sys/socket.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define SYS_SOCKET2	18

/*
 * This were good at the time of 2.6.22-rc3
 */
#ifndef __NR_nonseqfd
#if defined(__x86_64__)
#define __NR_nonseqfd	284
#elif defined(__i386__)
#define __NR_nonseqfd	324
#else
#error Cannot detect your architecture!
#endif
#endif

#ifndef O_NONSEQFD
#define O_NONSEQFD	02000000
#endif

#define SCARG(v)	((unsigned int) (v))

static int nonseqfd(int fd, int flags) {

	return syscall(__NR_nonseqfd, fd, flags);
}

#if defined(__x86_64__)
/*
 * This was good at the time of 2.6.22-rc3
 */
#define __NR_socket2            285

static int socket2(int domain, int type, int protocol, int flags) {

	return syscall(__NR_socket2, domain, type, protocol, flags);
}

#elif defined(__i386__)

static int socket2(int domain, int type, int protocol, int flags) {
	unsigned long args[8] = {
		SCARG(domain),
		SCARG(type),
		SCARG(protocol),
		SCARG(flags)
	};

	return syscall(__NR_socketcall, SYS_SOCKET2, args);
}

#else
#error Cannot detect your architecture!
#endif

static void test_fdmap_grow(int fd) {
	int i, xfd;
	pid_t pid;
	char path[128];
	char *av[8];

	for (i = 0; i < 800; i++) {
		if ((xfd = nonseqfd(fd, 0)) == -1) {
			perror("nonseqfd");
			exit(1);
		}
		printf("xfd = %d\n", xfd);
	}
	sprintf(path, "/proc/%u/fd/", getpid());
	av[0] = "ls";
	av[1] = "-l";
	av[2] = path;
	av[3] = NULL;
	if ((pid = fork()) == 0) {
		execvp(av[0], av);
		exit(1);
	}
	waitpid(pid, NULL, 0);

	exit(0);
}

int main(int ac, char **av) {
	int xfd, nfd;
	pid_t pid;
	const char *xfdstr = "This comes from xfd!!\n";
	const char *xfdstrc = "This comes from xfd (child)!!\n";
	const char *nfdstr = "This comes from nfd!!\n";

	if ((xfd = nonseqfd(1, 0)) == -1) {
		perror("dup2(FD_UNSEQ_ALLOC)");
		return 1;
	}
	fprintf(stdout, "xfd = %d\n", xfd);

	write(xfd, xfdstr, strlen(xfdstr));

	if (fork() == 0) {
		write(xfd, xfdstrc, strlen(xfdstrc));
		close(xfd);
		exit(0);
	}
	fprintf(stdout, "testing dup() of a nonseq fd ...\n");
	if ((nfd = dup(xfd)) == -1) {
		perror("dup(xfd)");
		return 1;
	}
	fprintf(stdout, "nfd = %d (dup of %d)\n", nfd, xfd);
	write(nfd, nfdstr, strlen(nfdstr));
	close(nfd);

	fprintf(stdout, "testing dup2() over an allocated nonseq fd ...\n");
	if ((nfd = dup2(1, xfd)) == -1) {
		perror("dup2(1, xfd)");
		return 1;
	}
	fprintf(stdout, "nfd = %d (dup2 of %d)\n", nfd, xfd);
	write(nfd, nfdstr, strlen(nfdstr));
	close(nfd);
	close(xfd);

	fprintf(stdout, "testing socket2(O_NONSEQFD) ...\n");
	if ((xfd = socket2(PF_INET, SOCK_STREAM, 0, O_NONSEQFD)) == -1) {
		perror("socket2(O_NONSEQFD)");
		return 1;
	}
	fprintf(stdout, "sockfd = %d\n", xfd);
	close(xfd);

	fprintf(stdout, "testing open(/dev/null, O_NONSEQFD) ...\n");
	if ((xfd = open("/dev/null", O_RDWR | O_NONSEQFD)) == -1) {
		perror("open(/dev/null, O_NONSEQFD)");
		return 1;
	}
	fprintf(stdout, "xfd = %d\n", xfd);
	if (write(xfd, "hello?!", 7) != 7) {
		perror("/dev/null xfd");
		return 1;
	}
	close(xfd);

	fprintf(stdout, "press enter ...");
	getchar();

	fprintf(stdout, "running test_fdmap_grow()\n");
	if ((pid = fork()) == 0)
		test_fdmap_grow(1);
	waitpid(pid, NULL, 0);


	return 0;
}

