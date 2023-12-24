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
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>


#define MIN_TEST_TIME 1000000ULL
#define LOOP_COUNT 200


typedef u_int8_t __u8;
typedef u_int32_t __u32;
typedef u_int64_t __u64;
#define unlikely(e) __builtin_expect(!!(e), 0)



static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);
	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

int memiszero(void const *p, unsigned long n) {
	unsigned int z = 0;
	void const *t;

	if (unlikely(n < 8))
		goto bytes_loop;
	t = (void const *) (((unsigned long) p + n) & ~7UL);
	n = ((unsigned long) p + n) & 7;
	switch ((unsigned long) p & 7) {
	case 1:
		z |= *(__u8 const *) p++;
	case 2:
		z |= *(__u8 const *) p++;
	case 3:
		z |= *(__u8 const *) p++;
	case 4:
		z |= *(__u8 const *) p++;
	case 5:
		z |= *(__u8 const *) p++;
	case 6:
		z |= *(__u8 const *) p++;
	case 7:
		z |= *(__u8 const *) p++;
		if (z)
			return 0;
	}
	for (; p < t; p += 8)
		if (*(__u64 const *) p)
			return 0;
	bytes_loop:
	switch (n) {
	case 7:
		z |= *(__u8 const *) p++;
	case 6:
		z |= *(__u8 const *) p++;
	case 5:
		z |= *(__u8 const *) p++;
	case 4:
		z |= *(__u8 const *) p++;
	case 3:
		z |= *(__u8 const *) p++;
	case 2:
		z |= *(__u8 const *) p++;
	case 1:
		z |= *(__u8 const *) p++;
	}
	return !z;
}

int memiszero_32(void const *p, unsigned long n) {
	unsigned int z = 0;
	void const *t;

	if (unlikely(n < 4))
		goto bytes_loop;
	t = (void const *) (((unsigned long) p + n) & ~3UL);
	n = ((unsigned long) p + n) & 3;
	switch ((unsigned long) p & 3) {
	case 1:
		z |= *(__u8 const *) p++;
	case 2:
		z |= *(__u8 const *) p++;
	case 3:
		z |= *(__u8 const *) p++;
		if (z)
			return 0;
	}
	for (; p < t; p += 4)
		if (*(__u32 const *) p)
			return 0;
	bytes_loop:
	switch (n) {
	case 3:
		z |= *(__u8 const *) p++;
	case 2:
		z |= *(__u8 const *) p++;
	case 1:
		z |= *(__u8 const *) p++;
	}
	return !z;
}

int memiszero_loop(void const *p, unsigned long n) {
	void const *t;

	t = p + n;
	for (; p < t; p++)
		if (*(__u8 const *) p)
			return 0;
	return 1;
}

int main(int ac, char **av) {
	int i, n;
	unsigned long size = 1024 * 128;
	char *data;
	unsigned long long ts, te, count;

	if (ac > 1)
		size = atol(av[1]);
	data = malloc(size);
	memset(data, 0, size);
	data[size - 1] = 1;

	ts = getustime();
	count = 0;
	do {
		for (i = n = 0; i < LOOP_COUNT; i++)
			n += memiszero_loop(data, size);
		count++;
		if (unlikely(n)) {
			fprintf(stderr, "memiszero_loop broken!\n");
			return 1;
		}
		te = getustime();
	} while (te - ts < MIN_TEST_TIME);
	fprintf(stdout, "loop:  %8llu us / loop\n", (te - ts) / count);

	ts = getustime();
	count = 0;
	do {
		for (i = n = 0; i < LOOP_COUNT; i++)
			n += memiszero_32(data, size);
		count++;
		if (unlikely(n)) {
			fprintf(stderr, "memiszero_loop broken!\n");
			return 1;
		}
		te = getustime();
	} while (te - ts < MIN_TEST_TIME);
	fprintf(stdout, "opt32: %8llu us / loop\n", (te - ts) / count);

	ts = getustime();
	count = 0;
	do {
		for (i = n = 0; i < LOOP_COUNT; i++)
			n += memiszero(data, size);
		count++;
		if (unlikely(n)) {
			fprintf(stderr, "memiszero_loop broken!\n");
			return 1;
		}
		te = getustime();
	} while (te - ts < MIN_TEST_TIME);
	fprintf(stdout, "opt64: %8llu us / loop\n", (te - ts) / count);

	return 0;
}

