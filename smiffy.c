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
#include <sys/time.h>
#include <sys/resource.h>

#ifndef MAP_NOZERO
#define MAP_NOZERO	0x04000000
#endif

#define DEFAULT_SIZE_PAGES	128

static unsigned int pgsize;

static int page_check(void *page) {
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

	return 1;
}

static int test_mmap(unsigned int size) {
	unsigned int nzcount;
	char *addr, *p, *top;

	size = ((size + pgsize - 1) / pgsize) * pgsize;
	addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE | MAP_NOZERO, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	for (p = addr, top = addr + size, nzcount = 0; p < top; p += pgsize) {
		if (!page_check(p))
			nzcount++;
	}
	munmap(addr, size);
	if (nzcount)
		fprintf(stderr, "mapping had %u non-zero pages\n", nzcount);

	return 0;
}

int main(int ac, char **av) {
	int i, sltimeo = 1000000, nice = 0;
	unsigned int size;

	pgsize = sysconf(_SC_PAGESIZE);
	size = pgsize * DEFAULT_SIZE_PAGES;
	while ((i = getopt(ac, av, "s:p:N:h")) != -1) {
		switch (i) {
		case 's':
			size = atoi(optarg) * pgsize;
			break;
		case 'p':
			sltimeo = atoi(optarg);
			break;
		case 'N':
			nice = atoi(optarg);
			break;
		case 'h':

			break;
		}
	}
	if (setpriority(PRIO_PROCESS, 0, nice))
		perror("setpriority");
	for (;;) {
		test_mmap(size);
		usleep(sltimeo);
	}

	return 0;
}

