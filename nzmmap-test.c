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

#ifndef MAP_NOZERO
#define MAP_NOZERO	0x04000000
#endif

#define DEFAULT_SIZE	(sysconf(_SC_PAGESIZE) * 32)

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

static int test_mmap(unsigned int size, unsigned int mmflags) {
	unsigned int pgsize, nzcount;
	char *addr, *p, *top;

	pgsize = sysconf(_SC_PAGESIZE);
	size = ((size + pgsize - 1) / pgsize) * pgsize;
	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, mmflags, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	for (p = addr, top = addr + size, nzcount = 0; p < top; p += pgsize) {
		if (!page_is_zero(p, pgsize))
			nzcount++;
	}
	munmap(addr, size);
	fprintf(stdout, "mapping had %u non-zero pages\n", nzcount);

	return 0;
}

int main(int ac, char **av) {
	int i;
	unsigned int size = DEFAULT_SIZE, mmflags = MAP_ANONYMOUS | MAP_PRIVATE;

	while ((i = getopt(ac, av, "s:nh")) != -1) {
		switch (i) {
		case 's':
			size = atoi(optarg);
			break;
		case 'n':
			mmflags |= MAP_NOZERO;
			break;
		case 'h':

			break;
		}
	}
	test_mmap(size, mmflags);

	return 0;
}

