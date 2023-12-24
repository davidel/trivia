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
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sched.h>
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

#define GET_CPUCTX() (thcpu_ctx + xget_thread_ctx()->cpu)

enum tx_modes {
	TX_SENDFILE,
	TX_MMAP
};

struct bstream {
	int fd;
	size_t ridx, bcnt;
	char buf[BSTREAM_BUFSIZE];
};

struct per_cpu_ctx {
	pthread_mutex_t mtx;
	pthread_cond_t cnd;
	pthread_cond_t dqcnd;
	unsigned long long tbytes, reqs, conns;
	int nthreads;
	pthread_t *threads;
	int qsize, rqpos, wqpos, qcount, qwait;
	int *squeue;
} __attribute__ ((aligned (64)));

struct thread_ctx {
	int cpu;
};

static int stopsvr;
static char const *rootfs = ".";
static int oflags;
static int txmode = TX_MMAP;
static int avail_cpus, num_cpus;
static int sh_pipe[2];
static int svrfd;
static struct per_cpu_ctx *thcpu_ctx;
static pthread_attr_t def_thattr;
static pthread_key_t thtls_key;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void xpthread_create(pthread_t *ptid, pthread_attr_t const *attr,
			    void *(*thproc)(void *), void *arg)
{
	if (pthread_create(ptid, attr, thproc, arg) != 0) {
		perror("Creating new thread");
		exit(1);
	}
}

static void xpthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
	if (pthread_key_create(key, dtor) != 0) {
		perror("Creating a TLS key");
		exit(1);
	}
}

static void xpthread_setspecific(pthread_key_t key, void *data)
{
	if (pthread_setspecific(key, data) != 0) {
		perror("Setting a TLS key");
		exit(1);
	}
}

static struct thread_ctx *xget_thread_ctx(void)
{
	void *data;

	if (__builtin_expect((data = pthread_getspecific(thtls_key)) == NULL,
			     0)) {
		perror("Getting thread context from TLS");
		exit(1);
	}

	return (struct thread_ctx *) data;
}

static void xpthread_mutex_init(pthread_mutex_t *pmtx,
				pthread_mutexattr_t const *attr)
{
	if (pthread_mutex_init(pmtx, attr) != 0) {
		perror("Initializing a mutex");
		exit(1);
	}
}

static void xpthread_cond_init(pthread_cond_t *cnd,
			       pthread_condattr_t const *attr)
{
	if (pthread_cond_init(cnd, attr) != 0) {
		perror("Initializing a conditional variable");
		exit(1);
	}
}

static void xsched_setaffinity(pid_t pid, unsigned int cpusetsize,
			       cpu_set_t *mask)
{
	if (sched_setaffinity(pid, cpusetsize, mask) != 0) {
		perror("Setting thread CPU affinity");
		exit(1);
	}
}

static void *xmmap(void *addr, size_t length, int prot, int flags,
		   int fd, off_t offset)
{
	void *ptr;

	if ((ptr = mmap(addr, length, prot, flags, fd,
			offset)) == (void *) -1L) {
		perror("Creating memory map");
		exit(1);
	}

	return ptr;
}

static void xpipe(int *pfds)
{
	if (pipe(pfds) != 0) {
		perror("Creating a pipe");
		exit(1);
	}
}

static int xsocket(int domain, int type, int protocol)
{
	int sfd;

	if ((sfd = socket(domain, type, protocol)) == -1) {
		perror("Creating socket");
		exit(1);
	}

	return sfd;
}

static void xbind(int sfd, struct sockaddr const *addr, socklen_t addrlen)
{
	if (bind(sfd, addr, addrlen) == -1) {
		perror("Binding socket");
		exit(1);
	}
}

static void *xmalloc(size_t size)
{
	void *data;

	if ((data = malloc(size)) == NULL) {
		perror("Allocating memory block");
		exit(1);
	}

	return data;
}

static void *xrealloc(void *odata, size_t size)
{
	void *data;

	if ((data = realloc(odata, size)) == NULL) {
		perror("Re-allocating memory block");
		exit(1);
	}

	return data;
}

static int xvasprintf(char **bptr, char const *fmt, va_list args)
{
	int error;

	error = vasprintf(bptr, fmt, args);
	if (error < 0) {
		perror("Generating formatted buffer");
		exit(1);
	}

	return error;
}

static int xasprintf(char **bptr, char const *fmt, ...)
{
	int error;
	va_list args;

	va_start(args, fmt);
	error = xvasprintf(bptr, fmt, args);
	va_end(args);

	return error;
}

static struct bstream *bstream_open(int fd)
{
	struct bstream *bstr;

	bstr = (struct bstream *) xmalloc(sizeof(struct bstream));
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
	size_t n;

	if (bstr->bcnt > 0 && bstr->ridx > 0)
		memmove(bstr->buf, bstr->buf + bstr->ridx, bstr->bcnt);
	bstr->ridx = 0;
	if ((n = recv(bstr->fd, bstr->buf + bstr->bcnt,
		      BSTREAM_BUFSIZE - bstr->bcnt, 0)) > 0)
		bstr->bcnt += n;

	return n;
}

static size_t bstream_readsome(struct bstream *bstr, void *buf, size_t n)
{
	size_t cnt;

	if ((cnt = bstr->bcnt) > 0) {
		if (cnt > n)
			cnt = n;
		memcpy(buf, bstr->buf + bstr->ridx, cnt);
		bstr->ridx += cnt;
		bstr->bcnt -= cnt;
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
	size_t lsize;
	char *eol;

	if ((eol = (char *) memchr(bstr->buf + bstr->ridx, '\n',
				   bstr->bcnt)) == NULL) {
		if (bstream_refil(bstr) <= 0 ||
		    (eol = (char *) memchr(bstr->buf + bstr->ridx, '\n',
					   bstr->bcnt)) == NULL)
			return NULL;
	}
	lsize = (size_t) (eol - (bstr->buf + bstr->ridx)) + 1;
	*eol = '\0';
	bstr->bcnt -= lsize;
	bstr->ridx += lsize;

	*lnsize = lsize;

	return bstr->buf + bstr->ridx - lsize;
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
	cnt = xvasprintf(&wstr, fmt, args);
	va_end(args);

	cnt = bstream_write(bstr, wstr, cnt);
	free(wstr);

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

	addr = xmmap(NULL, stb->st_size, PROT_READ, MAP_PRIVATE, fd, 0);
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
	struct per_cpu_ctx *pcx;
	struct stat stbuf;

	pcx = GET_CPUCTX();

	/*
	 * Ok, this is a dumb server, don't expect protection against '..'
	 * root path back-tracking tricks ;)
	 */
	xasprintf(&path, "%s/%s", rootfs, *doc == '/' ? doc + 1: doc);
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

	pthread_mutex_lock(&pcx->mtx);
	pcx->tbytes += stbuf.st_size;
	pthread_mutex_unlock(&pcx->mtx);

	return 0;
}



static int send_mem(struct bstream *bstr, long size, char const *ver,
		    char const *cclose)
{
	size_t csize, n;
	long msent;
	struct per_cpu_ctx *pcx;
	static char mbuf[1024 * 8];

	pcx = GET_CPUCTX();

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

	pthread_mutex_lock(&pcx->mtx);
	pcx->tbytes += msent;
	pthread_mutex_unlock(&pcx->mtx);

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

static int process_session(int cfd)
{
	int cclose, chunked;
	size_t lsize, clen;
	struct per_cpu_ctx *pcx;
	struct bstream *bstr;
	char *meth, *doc, *ver, *ln, *auxptr;
	char req[2048];

	/*
	 * This could be easily be passed from the top, but I want to test
	 * the pthread TLS access performance, since in big projects, passing
	 * down the pointers might make the interface ugly.
	 */
	pcx = GET_CPUCTX();

	bstr = bstream_open(cfd);
	do {
		if ((ln = bstream_readln(bstr, &lsize)) == NULL)
			break;
		strncpy(req, ln, sizeof(req));
		if ((meth = strtok_r(req, " ", &auxptr)) == NULL ||
		    (doc = strtok_r(NULL, " ", &auxptr)) == NULL ||
		    (ver = strtok_r(NULL, " \r", &auxptr)) == NULL ||
		    strcasecmp(meth, "GET") != 0) {
		bad_request:
			bstream_printf(bstr,
				       "HTTP/1.1 400 Bad request\r\n"
				       "Connection: close\r\n"
				       "Content-Length: 0\r\n"
				       "\r\n");
			break;
		}
		pthread_mutex_lock(&pcx->mtx);
		pcx->reqs++;
		pthread_mutex_unlock(&pcx->mtx);
		cclose = strcasecmp(ver, "HTTP/1.1") != 0;
		for (clen = 0, chunked = 0;;) {
			if ((ln = bstream_readln(bstr, &lsize)) == NULL)
				break;
			if (strcmp(ln, "\r") == 0)
				break;
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
		}
		/*
		 * Sorry, really stupid HTTP server here. Neither GET payload nor
		 * chunked encoding allowed.
		 */
		if (clen || chunked)
			goto bad_request;
		send_url(bstr, doc, ver, cclose ? "close": "keep-alive");
	} while (!stopsvr && !cclose);
	bstream_close(bstr);

	return 0;
}

static int dequeue_client_session(struct per_cpu_ctx *pcx)
{
	int cfd = -1;

	pthread_mutex_lock(&pcx->mtx);
	while (!stopsvr && pcx->qcount == 0)
		pthread_cond_wait(&pcx->cnd, &pcx->mtx);
	if (pcx->qcount > 0) {
		cfd = pcx->squeue[pcx->rqpos];
		pcx->rqpos = (pcx->rqpos + 1) % pcx->qsize;
		pcx->qcount--;
		if (pcx->qwait > 0)
			pthread_cond_signal(&pcx->dqcnd);
	}
	pthread_mutex_unlock(&pcx->mtx);

	return cfd;
}

static void queue_client_session(struct per_cpu_ctx *pcx, int cfd)
{
	pthread_mutex_lock(&pcx->mtx);
	while (!stopsvr && pcx->qcount >= pcx->qsize) {
		pcx->qwait++;
		pthread_cond_wait(&pcx->dqcnd, &pcx->mtx);
		pcx->qwait--;
	}
	if (pcx->qcount < pcx->qsize) {
		pcx->qcount++;
		pcx->squeue[pcx->wqpos] = cfd;
		pcx->wqpos = (pcx->wqpos + 1) % pcx->qsize;
		pthread_cond_signal(&pcx->cnd);
	}
	pthread_mutex_unlock(&pcx->mtx);
}

static pid_t gettid(void)
{
	return (pid_t) syscall(SYS_gettid);
}

static struct thread_ctx *setup_thread_ctx(int cpu)
{
	struct thread_ctx *tcx;
	cpu_set_t cset;

	CPU_ZERO(&cset);
	CPU_SET(cpu, &cset);
	xsched_setaffinity(gettid(), sizeof(cset), &cset);

	tcx = (struct thread_ctx *) xmalloc(sizeof(struct thread_ctx));
	tcx->cpu = cpu;

	xpthread_setspecific(thtls_key, tcx);

	return tcx;
}

static void *service_thproc(void *data)
{
	int cfd, cpu = (int) (long) data;
	struct per_cpu_ctx *pcx;
	struct thread_ctx *tcx;

	pcx = thcpu_ctx + cpu;
	tcx = setup_thread_ctx(cpu);

	while ((cfd = dequeue_client_session(pcx)) != -1)
		process_session(cfd);

	return NULL;
}

static int accept_session(struct sockaddr_in *caddr)
{
	int cfd;
	socklen_t alen;
	struct pollfd pfds[2];

	for (;;) {
		pfds[0].fd = svrfd;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		pfds[1].fd = sh_pipe[0];
		pfds[1].events = POLLIN;
		pfds[1].revents = 0;
		if (poll(pfds, 2, -1) <= 0 || pfds[1].revents & POLLIN)
			break;
		if (pfds[0].revents & POLLIN) {
			alen = sizeof(*caddr);
			if ((cfd = accept(svrfd, (struct sockaddr *) caddr,
					  &alen)) == -1) {
				if (errno != EAGAIN) {
					perror("accept");
					break;
				}
				continue;
			}

			return cfd;
		}
	}

	return -1;
}

static void *acceptor_thproc(void *data)
{
	int cfd, cpu = (int) (long) data;
	struct per_cpu_ctx *pcx;
	struct thread_ctx *tcx;
	struct sockaddr_in caddr;
	struct linger ling = { 0, 0 };

	pcx = thcpu_ctx + cpu;
	tcx = setup_thread_ctx(cpu);

	while (!stopsvr) {
		if ((cfd = accept_session(&caddr)) < 0)
			break;
		setsockopt(cfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));

		pthread_mutex_lock(&pcx->mtx);
		pcx->conns++;
		pthread_mutex_unlock(&pcx->mtx);

		queue_client_session(pcx, cfd);
	}

	return NULL;
}

static void thtls_dtor(void *data)
{
	free(data);
}

static void init_per_cpu_ctx(struct per_cpu_ctx *pcx, int cpu, int nthreads,
			     int qsize)
{
	int i;

	memset(pcx, 0, sizeof(*pcx));
	xpthread_mutex_init(&pcx->mtx, NULL);
	xpthread_cond_init(&pcx->cnd, NULL);
	xpthread_cond_init(&pcx->dqcnd, NULL);

	pcx->nthreads = nthreads + 1;
	pcx->threads = (pthread_t *) xmalloc(pcx->nthreads * sizeof(pthread_t));

	pcx->qsize = qsize;
	pcx->squeue = (int *) xmalloc(qsize * sizeof(int));

	for (i = 0; i < nthreads; i++)
		xpthread_create(pcx->threads + i, &def_thattr, service_thproc,
				(void *) (long) cpu);

	xpthread_create(pcx->threads + i, &def_thattr, acceptor_thproc,
			(void *) (long) cpu);
}

static void usage(char const *prg)
{
	fprintf(stderr,
		"Use: %s [-h,--help] [-p,--port PORTNO] [-L,--listen LISBKLOG]\n"
		"\t[-r,--root ROOTFS] [-S,--sendfile] [-k,--stksize SIZE]\n"
		"\t[-T,--num-threads NUM] [-Q,--queue-size SIZE] [-R,--res-cpu NCPU]\n", prg);
}

static void sig_int(int sig)
{
	++stopsvr;
	write(sh_pipe[1], &sig, sizeof(int));
}

int main(int ac, char **av)
{
	int i, error, port = 80, lbklog = 1024, one = 1,
		stksize = 0, nthreads = 16, qsize = 32, rescpu = 0;
	unsigned long long conns, tbytes, reqs;
	struct sockaddr_in saddr;
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
		} else if (strcmp(av[i], "--res-cpu") == 0 ||
			   strcmp(av[i], "-R") == 0) {
			if (++i < ac)
				rescpu = atoi(av[i]);
		} else if (strcmp(av[i], "--num-threads") == 0 ||
			   strcmp(av[i], "-T") == 0) {
			if (++i < ac)
				nthreads = atoi(av[i]);
		} else if (strcmp(av[i], "--queue-size") == 0 ||
			   strcmp(av[i], "-Q") == 0) {
			if (++i < ac)
				qsize = atoi(av[i]);
		} else if (strcmp(av[i], "--help") == 0||
			   strcmp(av[i], "-h") == 0) {
			usage(av[0]);
			return 1;
		}
	}

	signal(SIGINT, sig_int);
	signal(SIGPIPE, SIG_IGN);
	siginterrupt(SIGINT, 1);

	pthread_attr_init(&def_thattr);
	if (stksize > 0 && (error = pthread_attr_setstacksize(&def_thattr, stksize)) != 0) {
		fprintf(stderr, "Failed to set stack size: %s\n", strerror(error));
		return 2;
	}

	xpipe(sh_pipe);

	avail_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if ((num_cpus = avail_cpus - rescpu) <= 0)
		num_cpus = 1;

	fprintf(stdout,
		"Number of CPU(s)            : %d\n"
		"Number of used CPU(s)       : %d\n"
		"Number of Thread(s) per CPU : %d\n",
		avail_cpus, num_cpus, nthreads);

	svrfd = xsocket(AF_INET, SOCK_STREAM, 0);
	fcntl(svrfd, F_SETFL, fcntl(svrfd, F_GETFL, 0) | O_NONBLOCK);
	setsockopt(svrfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(svrfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons((short int) port);
	saddr.sin_addr.s_addr = INADDR_ANY;
	xbind(svrfd, (struct sockaddr *) &saddr, sizeof(saddr));
	listen(svrfd, lbklog);

	xpthread_key_create(&thtls_key, thtls_dtor);
	thcpu_ctx = (struct per_cpu_ctx *)
		xmalloc(num_cpus * sizeof(struct per_cpu_ctx));
	for (i = 0; i < num_cpus; i++)
		init_per_cpu_ctx(thcpu_ctx + i, i, nthreads, qsize);

	for (;;) {
		struct pollfd pfd;

		pfd.fd = sh_pipe[0];
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, -1) > 0 && pfd.revents & POLLIN)
			break;
	}

	close(svrfd);

	for (i = 0; i < num_cpus; i++)
		pthread_cond_broadcast(&thcpu_ctx[i].cnd);
	tbytes = reqs = conns = 0;
	for (i = 0; i < num_cpus; i++) {
		int j;

		for (j = 0; j < thcpu_ctx[i].nthreads; j++)
			pthread_join(thcpu_ctx[i].threads[j], NULL);

		tbytes += thcpu_ctx[i].tbytes;
		reqs += thcpu_ctx[i].reqs;
		conns += thcpu_ctx[i].conns;
	}

	fprintf(stdout,
		"Connections .....: %llu\n"
		"Requests ........: %llu\n"
		"Total Bytes .....: %llu\n", conns, reqs, tbytes);

	return 0;
}

