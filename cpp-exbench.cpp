/*
 *  cpp-exbench by Davide Libenzi (C++ exception handling micro-bench)
 *  Copyright (C) 2007  Davide Libenzi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>



#define MIN_TEST_TIME 1000000ULL
#define LOOP_COUNT 10000
#define NTLOOP_COUNT 100000



unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);
	return (unsigned long long) tm.tv_sec * 1000000ULL + (unsigned long long) tm.tv_usec;
}

void thrower(int n) {

	throw(n);
}

void non_thrower(int n) {

}

int main (int argc, char **argv) {
	int i;
	unsigned long long ts, te, count = 0, caught = 0;

	ts = getustime();
	do {
		for (i = 0; i < LOOP_COUNT; i++) {
			try {
				thrower(i);
			}
			catch (int n) {
				caught++;
			}
		}
		count++;
		te = getustime();
	} while (te - ts < MIN_TEST_TIME);
	if (caught != count * LOOP_COUNT)
		fprintf(stderr, "exceptions not caught\n");
	fprintf(stdout, "thrower:     %llu us / loop\n", (te - ts) / count);

	count = 0;
	ts = getustime();
	do {
		for (i = 0; i < NTLOOP_COUNT; i++) {
			try {
				non_thrower(i);
			}
			catch (int n) {

			}
		}
		count++;
		te = getustime();
	} while (te - ts < MIN_TEST_TIME);
	fprintf(stdout, "non_thrower: %llu us / loop\n", (te - ts) / count);

	return 0;
}

