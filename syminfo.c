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
#include <string.h>
#define __USE_GNU
#include <dlfcn.h>


static void usage(char const *prg) {

	fprintf(stderr, "use: %s [-h] [-l LIBPATH] SYMBOL ...\n", prg);
}

int main(int ac, char **av) {
	int i;
	void *lib = RTLD_NEXT, *symaddr;
	Dl_info syminf;

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-l") == 0) {
			i++;
			if (i < ac &&
			    (lib = dlopen(av[i], RTLD_NOW)) == NULL) {
				perror(av[i]);
				return 2;
			}
		} else if (strcmp(av[i], "-h") == 0) {
			usage(av[0]);
			return 1;
		} else {
			break;
		}
	}
	fprintf(stdout, "%16s%16s%24s%16s%16s%16s\n",
		"SYM", "ADDR", "FILE", "BASE", "RSYM", "RADDR");

	for (; i < ac; i++) {
		symaddr = dlsym(lib, av[i]);
		fprintf(stdout, "%16s%16p", av[i], symaddr);
		if (dladdr(symaddr, &syminf)) {
			fprintf(stdout, "%24s%16p%16s%16p",
				syminf.dli_fname, syminf.dli_fbase,
				syminf.dli_sname, syminf.dli_saddr);
		}
		fprintf(stdout, "\n");
	}

	return 0;
}

