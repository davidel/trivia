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
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#define STD_POLL_TIMEO (1 * 1000)

static void xtty_usage(char const *prg) {

	fprintf(stderr,
		"use: %s --dev DEV [--at-mode] [--unbuf] [--logfile LOGFILE] [--quiet] [--help]\n",
		prg);
}

static char *xtty_tr(char *str, int n, char const *imap,
		     char const *omap, int mn) {
	int i;
	char const *tmp;

	for (i = 0; i < n; i++)
		if ((tmp = (char *) memchr(imap, str[i], mn)) != NULL)
			str[i] = omap[tmp - imap];

	return str;
}

static int xtty_setnobuf(int fd) {
	struct termios ttysts;

	if (tcgetattr(fd, &ttysts)) {
		perror("tcgetattr");
		return -1;
	}

	/*
	 * Disable canonical mode, and set buffer size to 1 byte.
	 */
	ttysts.c_lflag &= ~ICANON;
	ttysts.c_cc[VTIME] = 0;
	ttysts.c_cc[VMIN] = 1;
	if (tcsetattr(fd, TCSANOW, &ttysts)) {
		perror("tcgetattr");
		return -1;
	}

	return 0;
}

int main(int ac, char **av) {
	int i, fd, rdbytes, timeo = STD_POLL_TIMEO, atmode = 0;
	int cinfd = fileno(stdin), coutfd = fileno(stdout), logfd = -1;
	char const *dev = NULL;
	struct pollfd pfds[2];
	char buf[2048];

	for (i = 1; i < ac; i++) {
		if (!strcmp(av[i], "--dev")) {
			if (++i < ac)
				dev = av[i];
		}
		else if (!strcmp(av[i], "--at-mode"))
			atmode++;
		else if (!strcmp(av[i], "--unbuf"))
			xtty_setnobuf(0);
		else if (!strcmp(av[i], "--quiet"))
			coutfd = -1;
		else if (!strcmp(av[i], "--logfile")) {
			if (++i < ac &&
			    (logfd = open(av[i], O_WRONLY | O_CREAT | O_APPEND)) == -1) {
				perror(av[i]);
				return 1;
			}
		} else if (!strcmp(av[i], "--help")) {
			xtty_usage(av[0]);
			return 2;
		}
	}
	if (dev == NULL) {
		xtty_usage(av[0]);
		return 3;
	}
	if ((fd = open(dev, O_RDWR)) == -1) {
		perror(dev);
		return 4;
	}
	for (;;) {
		pfds[0].fd = fd;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		pfds[1].fd = cinfd;
		pfds[1].events = POLLIN;
		pfds[1].revents = 0;
		poll(pfds, 2, timeo);
		if (pfds[0].revents & POLLIN) {
			if ((rdbytes = read(pfds[0].fd, buf,
					    sizeof(buf))) > 0) {
				if (coutfd != -1)
					write(coutfd, buf, rdbytes);
				if (logfd != -1)
					write(logfd, buf, rdbytes);
			} else if (rdbytes == 0)
				break;
		}
		if (pfds[1].revents & POLLIN) {
			if ((rdbytes = read(pfds[1].fd, buf,
					    sizeof(buf))) > 0) {
				if (atmode)
					xtty_tr(buf, rdbytes, "\n", "\r", 1);
				write(fd, buf, rdbytes);
			}
			else if (rdbytes == 0)
				break;
		}
	}
	close(fd);
	if (logfd != -1)
		close(logfd);

	return 0;
}

