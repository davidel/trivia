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
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>




static void usage(char const *prg) {

	fprintf(stderr, "use: %s -s HOST -p PORT [-u UID] [-g GID] PATH [ARGS ...]\n", prg);
}

int main (int ac, char **av) {
	int i, port = -1, sfd, uid = -1, gid = -1, status;
	char const *host = NULL;
	pid_t pid;
	struct hostent *he;
	struct in_addr inadr;
	struct sockaddr_in addr;

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-p") == 0) {
			if (++i < ac)
				port = atoi(av[i]);
		} else if (strcmp(av[i], "-s") == 0) {
			if (++i < ac)
				host = av[i];
		} else if (strcmp(av[i], "-u") == 0) {
			if (++i < ac)
				uid = atoi(av[i]);
		} else if (strcmp(av[i], "-g") == 0) {
			if (++i < ac)
				gid = atoi(av[i]);
		} else
			break;
	}
	if (i == ac || port < 0 || host == NULL) {
		usage(av[0]);
		return 1;
	}
	if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return 2;
	}
	if (inet_aton(host, &inadr) == 0) {
		if ((he = gethostbyname(host)) == NULL) {
			fprintf(stderr, "unable to resolve: %s\n", host);
			return 3;
		}
		memcpy(&inadr.s_addr, he->h_addr_list[0], he->h_length);
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	memcpy(&addr.sin_addr, &inadr.s_addr, 4);
	addr.sin_port = htons((short int) port);
	if (connect(sfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("connect");
		close(sfd);
		return 4;
	}
	if ((pid = fork()) == 0) {
		if (gid >= 0 && setgid(gid)) {
			perror("setgid");
			return 5;
		}
		if (uid >= 0 && setuid(uid)) {
			perror("setuid");
			return 6;
		}
		dup2(sfd, 0);
		dup2(sfd, 1);
		dup2(sfd, 2);
		close(sfd);
		execvp(av[i], &av[i]);
		exit(7);
	}
	close(sfd);
	if (pid == -1) {
		perror("fork");
		return 8;
	}
	waitpid(pid, &status, 0);

	return 0;
}

