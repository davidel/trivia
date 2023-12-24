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


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>



#define MAX_CONNECT_ERRORS 4




static int tconnect(struct sockaddr_in const *addr) {
	int sfd;

	if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return -1;
	}
	if (connect(sfd, (struct sockaddr const *) addr, sizeof(*addr)) < 0) {
		perror("connect");
		close(sfd);
		return -1;
	}

	return sfd;
}

static void usage(char const *prg) {

	fprintf(stderr, "use: %s -s SERVER -p PORT -n NUMCONN [-R INITREQ] [-h]\n", prg);
}

int main(int ac, char **av) {
	int i, sfd, errors, port = -1, nconns = -1;
	char const *server = NULL, *req = NULL;
	struct hostent *he;
	struct in_addr inadr;
	struct sockaddr_in addr;

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-s") == 0) {
			if (++i < ac)
				server = av[i];
		} else if (strcmp(av[i], "-p") == 0) {
			if (++i < ac)
				port = atoi(av[i]);
		} else if (strcmp(av[i], "-n") == 0) {
			if (++i < ac)
				nconns = atoi(av[i]);
		} else if (strcmp(av[i], "-R") == 0) {
			if (++i < ac)
				req = av[i];
		} else if (strcmp(av[i], "-h") == 0) {
			usage(av[0]);
			return 1;
		}
	}
	if (server == NULL || port < 0 || nconns < 0) {
		usage(av[0]);
		return 1;
	}
	if (inet_aton(server, &inadr) == 0) {
		if ((he = gethostbyname(server)) == NULL) {
			fprintf(stderr, "unable to resolve: %s\n", server);
			return 2;
		}
		memcpy(&inadr.s_addr, he->h_addr_list[0], he->h_length);
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	memcpy(&addr.sin_addr, &inadr.s_addr, 4);
	addr.sin_port = htons((short int) port);
	for (i = 0; i < nconns; i++) {
		errors = 0;
		retry:
		if ((sfd = tconnect(&addr)) != -1) {
			if (req != NULL)
				write(sfd, req, strlen(req));
			errors = 0;
			printf("%d\n", i);
		} else {
			sleep(1);
			if (++errors < MAX_CONNECT_ERRORS)
				goto retry;
			break;
		}
	}
	fprintf(stdout, "%d connections created, press Ctrl-C to exit ...\n", i);
	for (;;)
		sleep(10);

	return 0;
}

