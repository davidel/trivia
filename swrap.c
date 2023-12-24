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


#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>


static void usage(char *prg)
{
	fprintf(stderr, "%s {-u UID, -U UNAME} {-g GID, -G GNAME} CMD [PARAM ...]\n", prg);
	exit(1);
}

int main(int ac, char **av)
{
	int i;
	uid_t uid = (uid_t) -1;
	gid_t gid = (gid_t) -1;
	char const *uname = NULL, *gname = NULL;

	while ((i = getopt(ac, av, "+u:U:g:G:h")) != EOF) {
		switch (i) {
		case 'u':
			uid = atoi(optarg);
			break;
		case 'U':
			uname = optarg;
			break;
		case 'g':
			gid = atoi(optarg);
			break;
		case 'G':
			gname = optarg;
			break;
		case 'h':
			usage(av[0]);
		}
	}
	if (uname != NULL) {
		struct passwd *pwn;

		if ((pwn = getpwnam(uname)) == NULL) {
			perror(uname);
			return 2;
		}
		uid = pwn->pw_uid;
	}
	if (gname != NULL) {
		struct group *grn;

		if ((grn = getgrnam(gname)) == NULL) {
			perror(gname);
			return 3;
		}
		gid = grn->gr_gid;
	}
	if (uid == (uid_t) -1 || gid == (gid_t) -1 || optind >= ac)
		usage(av[0]);


	if (setgid(gid)) {
		perror("Setting group");
		return 4;
	}
	if (setuid(uid)) {
		perror("Setting user");
		return 5;
	}

	execvp(av[optind], &av[optind]);
	perror(av[optind]);

	return 6;
}

