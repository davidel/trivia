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
#include <string.h>
#include <ctype.h>

#define SYMOUT_NO_OFFSET (1 << 0)

static int find_symbol(FILE *nmfp, unsigned long long addr, char *csym, int n,
		       int flags) {
	unsigned long long saddr, caddr;
	char *taddr, *ttype, *tsym;
	char ln[1024];

	rewind(nmfp);
	saddr = 0;
	while (fgets(ln, sizeof(ln), nmfp) != NULL) {
		if (!isxdigit(ln[0]) ||
		    (taddr = strtok(ln, " \t\r\n")) == NULL ||
		    (ttype = strtok(NULL, " \t\r\n")) == NULL ||
		    (tsym = strtok(NULL, "\t\r\n")) == NULL)
			continue;
		caddr = strtoull(taddr, NULL, 16);
		if (caddr <= addr && caddr > saddr) {
			saddr = caddr;
			if (flags & SYMOUT_NO_OFFSET)
				snprintf(csym, n, "%s", tsym);
			else
				snprintf(csym, n, "%s (+ %llu)", tsym, addr - caddr);
		}
	}

	return saddr != 0;
}

static void usage(const char *prg) {
	fprintf(stderr, "use: %s -f NMFILE ADDR [-O] [-d OFFSET] ...\n", prg);
}

int main(int ac, char **av) {
	int i, flags = 0;
	unsigned long long addr;
	long long diff = 0;
	FILE *nmfp;
	const char *nmfile = NULL;
	char csym[512];

	while ((i = getopt(ac, av, "f:d:Oh")) >= 0) {
		switch (i) {
		case 'f':
			nmfile = optarg;
			break;
		case 'd':
			diff = strtoll(optarg, NULL, 16);
			break;
		case 'O':
			flags |= SYMOUT_NO_OFFSET;
			break;
		case '?':
		case 'h':
			usage(av[0]);
			return 1;
		}
	}
	if (nmfile == NULL || optind >= ac) {
		usage(av[0]);
		return 1;
	}
	if ((nmfp = fopen(nmfile, "r")) == NULL) {
		perror(nmfile);
		return 2;
	}
	for (i = optind; i < ac; i++) {
		addr = strtoull(av[i], NULL, 16);
		addr += diff;
		if (find_symbol(nmfp, addr, csym, sizeof(csym), flags))
			fprintf(stdout, "0x%llx\t%s\n", addr, csym);
		else
			fprintf(stdout, "0x%llx\t????\n", addr);
	}
	fclose(nmfp);

	return 0;
}

