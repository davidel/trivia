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
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>



#define SEXECP_PORT 8199




static void usage(char const *prg) {

	fprintf(stderr, "use: %s [-p PORT] [-u UID] [-g GID] PATH [ARGS ...]\n", prg);
}

int main (int ac, char **av) {
	int i, port = SEXECP_PORT, sfd, cfd, uid = -1, gid = -1, one = 1;
	pid_t pid;
	socklen_t alen;
	struct sockaddr_in addr, caddr;

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-p") == 0) {
			if (++i < ac)
				port = atoi(av[i]);
		} else if (strcmp(av[i], "-u") == 0) {
			if (++i < ac)
				uid = atoi(av[i]);
		} else if (strcmp(av[i], "-g") == 0) {
			if (++i < ac)
				gid = atoi(av[i]);
		} else
			break;
	}
	if (i == ac) {
		usage(av[0]);
		return 1;
	}
	if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return 2;
	}
#ifdef SO_REUSEADDR
	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (char *) &one,
		       sizeof(one)) < 0) {
		perror("setsockopt(SO_REUSEADDR)");
		return 3;
	}
#endif
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);
	if (bind(sfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		return 4;
	}
	listen(sfd, 128);

	for (;;) {
		alen = sizeof(caddr);
		if ((cfd = accept(sfd, (struct sockaddr *) &caddr, &alen)) == -1) {
			perror("accept");
			break;
		}
		if ((pid = fork()) != 0) {
			if (pid == -1)
				perror("fork");
			close(cfd);
			continue;
		}
		if (gid >= 0 && setgid(gid)) {
			perror("setgid");
			return 4;
		}
		if (uid >= 0 && setuid(uid)) {
			perror("setuid");
			return 5;
		}
		dup2(cfd, 0);
		dup2(cfd, 1);
		dup2(cfd, 2);
		close(cfd);
		execvp(av[i], &av[i]);
		exit(5);
	}

	return 0;
}

