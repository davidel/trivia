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
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include "zlib.h"


#define MIN_TESTIME (2 * 1000000)
#define INNER_CYCLES 32



#define CHECK_ERR(err, msg) do { \
	if (err != Z_OK) { \
		fprintf(stderr, "%s error: %d\n", msg, err); \
		exit(1); \
	} \
} while (0)



static unsigned long long mintt = MIN_TESTIME;
static uLong incycles = INNER_CYCLES;



static unsigned long long getustime(void) {
	struct timeval tm;

	gettimeofday(&tm, NULL);
	return tm.tv_sec * 1000000ULL + tm.tv_usec;
}

static void do_defl(Byte *cdata, uLong *clen,
		    Byte *udata, uLong uclen) {
	z_stream c_stream; /* compression stream */
	int err;

	c_stream.zalloc = (alloc_func) NULL;
	c_stream.zfree = (free_func) NULL;
	c_stream.opaque = (voidpf) NULL;

	err = deflateInit(&c_stream, Z_BEST_SPEED);
	CHECK_ERR(err, "deflateInit");

	c_stream.next_out = cdata;
	c_stream.avail_out = (uInt) *clen;

	/* At this point, udata is still mostly zeroes, so it should compress
	 * very well:
	 */
	c_stream.next_in = udata;
	c_stream.avail_in = (uInt) uclen;
	err = deflate(&c_stream, Z_FINISH);
	if (err != Z_STREAM_END) {
		fprintf(stderr, "whoops, got %d instead of Z_STREAM_END\n", err);
		exit(1);
	}

	err = deflateEnd(&c_stream);
	CHECK_ERR(err, "deflateEnd");

	*clen = c_stream.next_out - cdata;
}

static void do_infl(Byte *cdata, uLong clen,
		    Byte *udata, uLong *uclen) {
	int err;
	z_stream d_stream; /* decompression stream */

	d_stream.zalloc = (alloc_func) NULL;
	d_stream.zfree = (free_func) NULL;
	d_stream.opaque = (voidpf) NULL;

	d_stream.next_in  = cdata;
	d_stream.avail_in = (uInt) clen;

	err = inflateInit(&d_stream);
	CHECK_ERR(err, "inflateInit");

	d_stream.next_out = udata;            /* discard the output */
	d_stream.avail_out = (uInt) *uclen;
	err = inflate(&d_stream, Z_FULL_FLUSH);
	if (err != Z_STREAM_END) {
		fprintf(stderr, "deflate should report Z_STREAM_END\n");
		exit(1);
	}

	err = inflateEnd(&d_stream);
	CHECK_ERR(err, "inflateEnd");

	*uclen = d_stream.next_out - udata;
}

static int do_filebench(char const *fpath) {
	int fd, err = -1;
	uLong i, n, clen, ulen, size;
	Byte *ubuf, *cbuf, *tbuf;
	unsigned long long ts, te;
	void *addr;
	struct stat stb;

	if ((fd = open(fpath, O_RDONLY)) == -1 ||
	    fstat(fd, &stb)) {
		perror(fpath);
		close(fd);
		return -1;
	}
	size = stb.st_size;
	addr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (addr == (void *) -1L) {
		perror("mmap");
		return -1;
	}
	ulen = size;
	clen = size + 4096;
	ubuf = addr;
	if ((tbuf = malloc(ulen + clen)) == NULL) {
		perror("malloc");
		goto err_exit;
	}
	cbuf = tbuf + ulen;

	/* Warming up ... */
	do_defl(cbuf, &clen, ubuf, ulen);
	do_infl(cbuf, clen, tbuf, &ulen);
	if (ulen != size) {
		fprintf(stderr, "size mismatch %lu instead of %lu\n",
			(unsigned long) ulen, (unsigned long) size);
		goto err_exit;
	}
	if (memcmp(tbuf, ubuf, size)) {
		fprintf(stderr, "whoops! we did not get back the same data\n");
		goto err_exit;
	}

	/* Test ... */
	fprintf(stdout, "testing: %s\n", fpath);
	ts = getustime();
	n = 0;
	do {
		for (i = 0; i < incycles; i++) {
			ulen = size;
			do_infl(cbuf, clen, tbuf, &ulen);
		}
		n += i;
		te = getustime();
	} while (te - ts < mintt);

	fprintf(stdout, "\tus time / cycle = %llu\n", (te - ts) / n);
	err = 0;

err_exit:
	free(tbuf);
	munmap(addr, size);

	return err;
}

int main(int ac, char **av) {
	int i;

	for (i = 1; i < ac; i++)
		do_filebench(av[i]);

	return 0;
}

