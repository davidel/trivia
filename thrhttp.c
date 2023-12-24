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


#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <resolv.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>

#define BSTREAM_BUFSIZE (1024 * 4)

enum tx_modes {
	TX_SENDFILE,
	TX_MMAP
};

struct bstream {
	int fd;
	size_t ridx, bcnt;
	char buf[BSTREAM_BUFSIZE];
};

static int stopsvr;
static char const *rootfs = ".";
static int oflags;
static int txmode = TX_MMAP;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long conns, reqs, tbytes;

static struct bstream *bstream_open(int fd)
{
	struct bstream *bstr;

	if ((bstr = (struct bstream *) malloc(sizeof(struct bstream))) == NULL) {
		perror("malloc");
		return NULL;
	}
	bstr->fd = fd;
	bstr->ridx = bstr->bcnt = 0;

	return bstr;
}

static void bstream_close(struct bstream *bstr)
{
	close(bstr->fd);
	free(bstr);
}

static size_t bstream_refil(struct bstream *bstr)
{
	bstr->ridx = 0;
	bstr->bcnt = recv(bstr->fd, bstr->buf, BSTREAM_BUFSIZE, 0);

	return bstr->bcnt;
}

static size_t bstream_readsome(struct bstream *bstr, void *buf, size_t n)
{
	size_t cnt;

	if (bstr->ridx < bstr->bcnt) {
		if ((cnt = bstr->bcnt - bstr->ridx) > n)
			cnt = n;
		memcpy(buf, bstr->buf + bstr->ridx, cnt);
		bstr->ridx += cnt;
	} else
		cnt = recv(bstr->fd, buf, n, 0);

	return cnt;
}

static size_t bstream_read(struct bstream *bstr, void *buf, size_t n)
{
	size_t cnt, acnt;

	for (cnt = 0; cnt < n;) {
		if ((acnt = bstream_readsome(bstr, buf, n - cnt)) <= 0)
			break;
		cnt += acnt;
		buf = (char *) buf + acnt;
	}

	return cnt;
}

static char *bstream_readln(struct bstream *bstr, size_t *lnsize)
{
	size_t lsize = 0, nlsize, cnt;
	char *ln = NULL, *nln;
	char const *eol = NULL;

	for (; eol == NULL;) {
		if (bstr->ridx == bstr->bcnt &&
		    bstream_refil(bstr) <= 0)
			break;
		if ((eol = (char *) memchr(bstr->buf + bstr->ridx, '\n',
					   bstr->bcnt - bstr->ridx)) != NULL)
			cnt = (size_t) (eol - (bstr->buf + bstr->ridx)) + 1;
		else
			cnt = bstr->bcnt - bstr->ridx;
		nlsize = lsize + cnt;
		if ((nln = (char *) realloc(ln, nlsize + 1)) == NULL) {
			perror("realloc");
			free(ln);
			return NULL;
		}
		bstream_readsome(bstr, nln + lsize, cnt);
		ln = nln;
		lsize = nlsize;
	}
	if (ln != NULL) {
		ln[lsize] = '\0';
		*lnsize = lsize;
	}

	return ln;
}

static size_t bstream_write(struct bstream *bstr, void const *buf, size_t n)
{
	size_t cnt, acnt;

	for (cnt = 0; cnt < n;) {
		if ((acnt = send(bstr->fd, buf, n - cnt, 0)) < 0) {
			perror("send");
			return -1;
		}
		cnt += acnt;
		buf = (char const *) buf + acnt;
	}

	return cnt;
}

static size_t bstream_printf(struct bstream *bstr, char const *fmt, ...)
{
	size_t cnt;
	char *wstr = NULL;
	va_list args;

	va_start(args, fmt);
	cnt = vasprintf(&wstr, fmt, args);
	va_end(args);
	if (wstr != NULL) {
		cnt = bstream_write(bstr, wstr, cnt);
		free(wstr);
	}

	return cnt;
}

static int sendfile_tx(int fd, struct bstream *bstr, struct stat const *stb)
{
	off_t off = 0;

	if (sendfile(bstr->fd, fd, &off, stb->st_size) != stb->st_size) {
		perror("sendfile");
		return -1;
	}

	return 0;
}

static int mmap_tx(int fd, struct bstream *bstr, struct stat const *stb)
{
	void *addr;
	size_t txcnt;

	if ((addr = mmap(NULL, stb->st_size, PROT_READ, MAP_PRIVATE,
			 fd, 0)) == (void *) -1) {
		perror("mmap");
		return -1;
	}
	txcnt = bstream_write(bstr, addr, stb->st_size);
	munmap(addr, stb->st_size);

	return txcnt == stb->st_size ? 0: -1;
}

static int set_cork(int fd, int v)
{
	return setsockopt(fd, SOL_TCP, TCP_CORK, &v, sizeof(v));
}

static int send_doc(struct bstream *bstr, char const *doc, char const *ver,
		    char const *cclose)
{
	int fd, error = -1;
	char *path = NULL;
	struct stat stbuf;

	/*
	 * Ok, this is a dumb server, don't expect protection against '..'
	 * root path back-tracking tricks ;)
	 */
	asprintf(&path, "%s/%s", rootfs, *doc == '/' ? doc + 1: doc);
	if (path == NULL) {
		perror("malloc");
		bstream_printf(bstr,
			       "%s 500 Internal server error\r\n"
			       "Connection: %s\r\n"
			       "Content-Length: 0\r\n"
			       "\r\n", ver, cclose);
		return -1;
	}
	if ((fd = open(path, oflags | O_RDONLY)) == -1 ||
	    fstat(fd, &stbuf)) {
		perror(path);
		close(fd);
		free(path);
		bstream_printf(bstr,
			       "%s 404 Not found\r\n"
			       "Connection: %s\r\n"
			       "Content-Length: 0\r\n"
			       "\r\n", ver, cclose);
		return -1;
	}
	free(path);
	set_cork(bstr->fd, 1);
	bstream_printf(bstr,
		       "%s 200 OK\r\n"
		       "Connection: %s\r\n"
		       "Content-Length: %ld\r\n"
		       "\r\n", ver, cclose, (long) stbuf.st_size);
	if (txmode == TX_SENDFILE)
		error = sendfile_tx(fd, bstr, &stbuf);
	else if (txmode == TX_MMAP)
		error = mmap_tx(fd, bstr, &stbuf);
	close(fd);
	set_cork(bstr->fd, 0);
	if (error < 0)
		return error;

	pthread_mutex_lock(&mtx);
	tbytes += stbuf.st_size;
	pthread_mutex_unlock(&mtx);

	return 0;
}

static int send_mem(struct bstream *bstr, long size, char const *ver,
		    char const *cclose)
{
	size_t csize, n;
	long msent;
	static char mbuf[1024 * 8];

	set_cork(bstr->fd, 1);
	bstream_printf(bstr,
		       "%s 200 OK\r\n"
		       "Connection: %s\r\n"
		       "Content-Length: %ld\r\n"
		       "\r\n", ver, cclose, size);
	for (msent = 0; msent < size;) {
		csize = (size - msent) > sizeof(mbuf) ?
			sizeof(mbuf): (size_t) (size - msent);
		if ((n = bstream_write(bstr, mbuf, csize)) > 0)
			msent += n;
		if (n != csize)
			break;
	}
	set_cork(bstr->fd, 0);

	pthread_mutex_lock(&mtx);
	tbytes += msent;
	pthread_mutex_unlock(&mtx);

	return msent == size ? 0: -1;
}

static int send_url(struct bstream *bstr, char const *doc, char const *ver,
		    char const *cclose)
{
	int error;

	if (strncmp(doc, "/mem-", 5) == 0)
		error = send_mem(bstr, atol(doc + 5), ver, cclose);
	else
		error = send_doc(bstr, doc, ver, cclose);

	return error;
}

static void *thproc(void *data)
{
	int cfd = (int) (long) data, cclose, chunked;
	size_t lsize, clen;
	struct bstream *bstr;
	char *req, *meth, *doc, *ver, *ln, *auxptr;

	if ((bstr = bstream_open(cfd)) == NULL) {
		close(cfd);
		return NULL;
	}
	do {
		if ((req = bstream_readln(bstr, &lsize)) == NULL)
			break;
		if ((meth = strtok_r(req, " ", &auxptr)) == NULL ||
		    (doc = strtok_r(NULL, " ", &auxptr)) == NULL ||
		    (ver = strtok_r(NULL, " \r\n", &auxptr)) == NULL ||
		    strcasecmp(meth, "GET") != 0) {
		bad_request:
			free(req);
			bstream_printf(bstr,
				       "HTTP/1.1 400 Bad request\r\n"
				       "Connection: close\r\n"
				       "Content-Length: 0\r\n"
				       "\r\n");
			break;
		}
		pthread_mutex_lock(&mtx);
		reqs++;
		pthread_mutex_unlock(&mtx);
		cclose = strcasecmp(ver, "HTTP/1.1") != 0;
		for (clen = 0, chunked = 0;;) {
			if ((ln = bstream_readln(bstr, &lsize)) == NULL)
				break;
			if (strcmp(ln, "\r\n") == 0) {
				free(ln);
				break;
			}
			if (strncasecmp(ln, "Content-Length:", 15) == 0) {
				for (auxptr = ln + 15; *auxptr == ' '; auxptr++);
				clen = atoi(auxptr);
			} else if (strncasecmp(ln, "Connection:", 11) == 0) {
				for (auxptr = ln + 11; *auxptr == ' '; auxptr++);
				cclose = strncasecmp(auxptr, "close", 5) == 0;
			} else if (strncasecmp(ln, "Transfer-Encoding:", 18) == 0) {
				for (auxptr = ln + 18; *auxptr == ' '; auxptr++);
				chunked = strncasecmp(auxptr, "chunked", 7) == 0;
			}
			free(ln);
		}
		/*
		 * Sorry, really stupid HTTP server here. Neither GET payload nor
		 * chunked encoding allowed.
		 */
		if (clen || chunked)
			goto bad_request;
		send_url(bstr, doc, ver, cclose ? "close": "keep-alive");
		free(req);
	} while (!stopsvr && !cclose);
	bstream_close(bstr);

	return NULL;
}

static void usage(char const *prg)
{
	fprintf(stderr,
		"Use: %s [-h,--help] [-p,--port PORTNO] [-L,--listen LISBKLOG]\n"
		"\t[-r,--root ROOTFS] [-S,--sendfile] [-k,--stksize SIZE]\n", prg);
}

static void sig_int(int sig)
{
	++stopsvr;
}

int main(int ac, char **av)
{
	int i, error, sfd, cfd, port = 80, lbklog = 1024, one = 1, stksize = 0;
	socklen_t alen;
	pthread_t thid;
	struct sockaddr_in saddr, caddr;
	pthread_attr_t thattr;
	struct linger ling = { 0, 0 };

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "--port") == 0 ||
		    strcmp(av[i], "-p") == 0) {
			if (++i < ac)
				port = atoi(av[i]);
		} else if (strcmp(av[i], "--listen") == 0 ||
			   strcmp(av[i], "-L") == 0) {
			if (++i < ac)
				lbklog = atoi(av[i]);
		} else if (strcmp(av[i], "--root") == 0 ||
			   strcmp(av[i], "-r") == 0) {
			if (++i < ac)
				rootfs = av[i];
		} else if (strcmp(av[i], "-N") == 0 ||
			   strcmp(av[i], "--no-atime") == 0) {
			oflags |= O_NOATIME;
		} else if (strcmp(av[i], "-S") == 0 ||
			   strcmp(av[i], "--sendfile") == 0) {
			txmode = TX_SENDFILE;
		} else if (strcmp(av[i], "--stksize") == 0 ||
			   strcmp(av[i], "-k") == 0) {
			if (++i < ac)
				stksize = atoi(av[i]);
		} else if (strcmp(av[i], "--help") == 0||
			   strcmp(av[i], "-h") == 0) {
			usage(av[0]);
			return 1;
		}
	}
	signal(SIGINT, sig_int);
	signal(SIGPIPE, SIG_IGN);
	siginterrupt(SIGINT, 1);

	pthread_attr_init(&thattr);
	pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_DETACHED);
	if (stksize > 0 && (error = pthread_attr_setstacksize(&thattr, stksize)) != 0) {
		fprintf(stderr, "Failed to set stack size: %s\n", strerror(error));
		return 2;
	}

	if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return 3;
	}
	setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(sfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons((short int) port);
	saddr.sin_addr.s_addr = INADDR_ANY;
	if (bind(sfd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1) {
		perror("bind");
		close(sfd);
		return 4;
	}
	listen(sfd, lbklog);

	while (!stopsvr) {
		alen = sizeof(caddr);
		if ((cfd = accept(sfd, (struct sockaddr *) &caddr, &alen)) == -1) {
			perror("accept");
			continue;
		}
		setsockopt(cfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
		pthread_mutex_lock(&mtx);
		conns++;
		pthread_mutex_unlock(&mtx);
		if (pthread_create(&thid, &thattr, thproc,
				   (void *) (long) cfd) != 0) {
			perror("pthread_create");
			close(cfd);
			continue;
		}
	}
	close(sfd);

	fprintf(stdout,
		"Connections .....: %llu\n"
		"Requests ........: %llu\n"
		"Total Bytes .....: %llu\n", conns, reqs, tbytes);

	return 0;
}

