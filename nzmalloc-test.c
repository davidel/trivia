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
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#define DEFAULT_SIZE	(sysconf(_SC_PAGESIZE) * 64)

static int page_is_zero(void *page, unsigned int pgsize) {
	unsigned long *p, *top;

	p = page;
	top = page + pgsize;
	/*
	 * Write-fault the page before, otherwise you get ZERO_PAGE plus
	 * copy_page() behaviour ...
	 */
	*p++ = 1;
	for (; p < top; p++)
		if (*p)
			return 0;
	((unsigned long *) page)[1] = 2;

	return 1;
}

static int test_malloc(unsigned int size) {
	unsigned int pgsize, nzcount;
	char *addr, *p, *top;

	pgsize = sysconf(_SC_PAGESIZE);
	size = ((size + pgsize - 1) / pgsize) * pgsize;
	if ((addr = malloc(size)) == NULL) {
		perror("malloc");
		return -1;
	}
	for (p = addr, top = addr + size, nzcount = 0; p < top; p += pgsize) {
		if (!page_is_zero(p, pgsize))
			nzcount++;
	}
	free(addr);
	fprintf(stdout, "mapping had %u non-zero pages\n", nzcount);

	return 0;
}

int main(int ac, char **av) {
	int i;
	unsigned int size = DEFAULT_SIZE;

	while ((i = getopt(ac, av, "s:h")) != -1) {
		switch (i) {
		case 's':
			size = atoi(optarg);
			break;
		case 'h':

			break;
		}
	}
	test_malloc(size);

	return 0;
}

