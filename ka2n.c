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
#include <string.h>

#define KSYMS_FILE	"/proc/kallsyms"

static int find_symbol(const char *ksymfile, unsigned long long addr) {
	FILE *kfp;
	unsigned long long saddr, caddr;
	char ln[1024], csym[1024];

	if ((kfp = fopen(ksymfile, "r")) == NULL) {
		perror(ksymfile);
		return -1;
	}
	saddr = 0;
	while (fgets(ln, sizeof(ln), kfp) != NULL) {
		caddr = strtoull(ln, NULL, 16);
		if (caddr <= addr && caddr > saddr) {
			saddr = caddr;
			strcpy(csym, ln);
		}
	}
	fclose(kfp);
	if (saddr)
		fputs(csym, stdout);
	return 0;
}

int main(int ac, char **av) {
	int i;
	unsigned long long addr;
	const char *ksymfile = KSYMS_FILE;

	for (i = 1; i < ac; i++) {
		addr = strtoull(av[i], NULL, 16);
		find_symbol(ksymfile, addr);
	}

	return 0;
}

