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


#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <stdarg.h>

#define CRLF_STR "\r\n"
#define CMD_LF "\r"

static int debug;

static int xgsm_swap_nibble(int v) {
	return  ((v % 10) << 4) | (v / 10);
}

static char *xgsm_code_number(const char *nbr, int *rsize) {
	int n, v;
	char *resp;

	if ((resp = (char *) malloc(strlen(nbr) / 2 + 2)) == NULL)
		return NULL;
	for (n = 0; *nbr != '\0';) {
		if (!isdigit(*nbr)) {
			fprintf(stderr, "error: non digit in number\n");
			free(resp);
			return NULL;
		}
		v = *nbr - '0';
		nbr++;
		if (*nbr != '\0') {
			if (!isdigit(*nbr)) {
				fprintf(stderr, "error: non digit in number\n");
				free(resp);
				return NULL;
			}
			v |= (*nbr - '0') << 4;
			nbr++;
		} else
			v |= 0xf0;
		resp[n++] = (char) v;
	}
	*rsize = n;

	return resp;
}

static int xgsm_get_timestamp(char *tsbuf, int n) {
	int gmtoff;
	time_t t;
	struct tm tm;

	if (n < 7) {
		fprintf(stderr, "error: timestamp buffer too small (need at least 7 bytes)\n");
		return -1;
	}
	time(&t);
	if (localtime_r(&t, &tm) == NULL) {
		perror("getting local time");
		return -1;
	}
	tsbuf[0] = (char) xgsm_swap_nibble(tm.tm_year);
	tsbuf[1] = (char) xgsm_swap_nibble(tm.tm_mon);
	tsbuf[2] = (char) xgsm_swap_nibble(tm.tm_mday);
	tsbuf[3] = (char) xgsm_swap_nibble(tm.tm_hour);
	tsbuf[4] = (char) xgsm_swap_nibble(tm.tm_min);
	tsbuf[5] = (char) xgsm_swap_nibble(tm.tm_sec);

	/*
	 * One step is 15 minutes.
	 */
	if ((gmtoff = (int) tm.tm_gmtoff / (15 * 60)) < 0)
		tsbuf[6] = (char) xgsm_swap_nibble(-gmtoff) | 0x80;
	else
		tsbuf[6] = (char) xgsm_swap_nibble(gmtoff);

	return 7;
}

static char *xgsm_pack7(const char *data, int *rsize) {
	int n, i, b, s, m;
	char *resp;

	if ((resp = (char *) malloc(strlen(data) + 1)) == NULL)
		return NULL;
	for (i = n = 0; data[i] != '\0'; i++) {
		if (data[i] & 0x80) {
			fprintf(stderr, "error: non 7bit data\n");
			free(resp);
			return NULL;
		}
		if ((s = i % 8) < 7) {
			b = s + 1;
			m = (1 << (s + 1)) - 1;
			resp[n++] = (data[i] >> s) | ((data[i + 1] & m) << (7 - s));
		}
	}
	*rsize = n;

	return resp;
}

static int xgsm_get_vt(int mins) {
	int v;

	if ((v = mins / (60 * 24 * 7)) >= 5)
		return 192 + v;
	if ((v = mins / (60 * 24)) >= 2)
		return 166 + v;
	if ((v = mins / 30) >= (12 * 60 / 30))
		return 143 + v - (12 * 60 / 30);

	return mins / 5;
}

static char *xgsm_create_PDU(const char *nbr, const char *scnbr, const char *msg,
			     int *rsize) {
	int n, size, tosca = 129, toda = 129;
	char *resp, *encd;

	size = 256 + strlen(nbr) + 2 * strlen(msg);
	if (scnbr != NULL)
		size += 2 + strlen(scnbr);
	if ((resp = (char *) malloc(size)) == NULL)
		return NULL;
	n = 0;
	if (scnbr != NULL) {
		if (*scnbr == '+') {
			scnbr++;
			tosca = 145;
		}
		if ((encd = xgsm_code_number(scnbr, &size)) == NULL) {
			free(resp);
			return NULL;
		}
		resp[n++] = 1 + (char) size;
		resp[n++] = (char) tosca;
		memcpy(resp + n, encd, size);
		n += size;
		free(encd);
	} else
		resp[n++] = 0x00;

	if (*nbr == '+') {
		nbr++;
		toda = 145;
	}
	if ((encd = xgsm_code_number(nbr, &size)) == NULL) {
		free(resp);
		return NULL;
	}
	resp[n++] = 0x11;
	resp[n++] = 0x00;	// MSG-REF
	resp[n++] = (char) strlen(nbr);	// Size of recipient number
	resp[n++] = (char) toda;	// Type of recipient number
	memcpy(resp + n, encd, size);
	n += size;
	free(encd);
	resp[n++] = 0x00;	// PID
	resp[n++] = 0x00;	// DCS
	resp[n++] = (char) xgsm_get_vt(1 * 60 * 24);	// VT
	resp[n++] = (char) strlen(msg);

#if 0
	for (; *msg != '\0'; msg++)
		resp[n++] = *msg;
#else
	encd = xgsm_pack7(msg, &size);
	memcpy(resp + n, encd, size);
        n += size;
	free(encd);
#endif

	*rsize = n;

	return resp;
}

static char *xgsm_hexdata(const char *data, int n) {
	char *resp, *p;
	const unsigned char *udata;

	if ((resp = (char *) malloc(2 * n + 1)) == NULL)
		return NULL;
	for (p = resp, udata = (const unsigned char *) data;
	     n > 0; n--, p += 2, udata++)
		sprintf(p, "%02x", *udata);
	*p = '\0';

	return resp;
}

static char *xgsm_create_hexPDU(const char *nbr, const char *scnbr,
				const char *msg) {
	int size;
	char *pdu, *hpdu;

	if ((pdu = xgsm_create_PDU(nbr, scnbr, msg, &size)) == NULL)
		return NULL;
	hpdu = xgsm_hexdata(pdu, size);
	free(pdu);

	return hpdu;
}

static void xgsm_hexdump(FILE *f, const char *data, int n) {
	int i, j, cnt;
	const unsigned char *udata;

	for (i = 0, udata = (const unsigned char *) data;
	     i < n; i += 16) {
		if ((cnt = n - i) > 16)
			cnt = 16;
		for (j = 0; j < cnt; j++) {
			if (j == 8)
				fputs(" ", f);
			fprintf(f, " %02x", udata[i + j]);
		}
		for (; j < 16; j++) {
			if (j == 8)
				fputs(" ", f);
			fprintf(f, "   ");
		}
		fputs(" |", f);

		for (j = 0; j < cnt; j++)
			if (isprint(data[i + j]))
				fprintf(f, "%c", udata[i + j]);
			else
				fputs(".", f);
		for (; j < 16; j++)
			fprintf(f, " ");

		fputs("|\n", f);
	}
}

static void xgsm_usage(char const *prg) {
	fprintf(stderr, "use: %s -d DEV -m MSG -n NBR [-u] [-h]\n", prg);
}

static int xgsm_read(int fd, char *buf, int n, int timeo) {
	int error;
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	if ((error = poll(&pfd, 1, timeo)) <= 0)
		return -1;

	return pfd.revents & POLLIN ? read(fd, buf, n): 0;
}

static void xgsm_flushin(int fd, int timeo) {
	int n;
	char buf[64];

	fputs("`` FLUSH\n", stderr);
	while ((n = xgsm_read(fd, buf, sizeof(buf), timeo)) > 0)
		xgsm_hexdump(stderr, buf, n);
	fputs("``\n", stderr);
}

static int xgsm_write(int fd, const char *data, int n, int timeo) {
	int error;
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;

	if ((error = poll(&pfd, 1, timeo)) <= 0 ||
	    (pfd.revents & (POLLHUP | POLLERR)) != 0)
		return -1;

	return pfd.revents & POLLOUT ? write(fd, data, n): 0;
}

static int xgsm_setnobuf(int fd) {
	struct termios ttysts;

	if (tcgetattr(fd, &ttysts)) {
		perror("tcgetattr");
		return -1;
	}

	/*
	 * Disable canonical mode, and set buffer size to 1 byte.
	 */
	ttysts.c_lflag &= ~ICANON;
	ttysts.c_cc[VTIME] = 0;
	ttysts.c_cc[VMIN] = 1;
	if (tcsetattr(fd, TCSANOW, &ttysts)) {
		perror("tcgetattr");
		return -1;
	}

	return 0;
}

static int xgsm_eoc(const char *resp, int n) {
	const char *sol;

	if (n <= 2 || memcmp(resp + n - 2, CRLF_STR, 2) != 0)
		return 0;
	/*
	 * Find the beginning of the previous line ...
	 */
	for (sol = resp + n - 3; sol >= resp && *sol != '\n'; sol--);
	if (sol < resp)
		return 0;

	return strncasecmp(sol, "\nOK" CRLF_STR, 5) == 0 ||
		strncasecmp(sol, "\nERROR" CRLF_STR, 8) == 0 ||
		strncasecmp(sol, "\nNO CARRIER" CRLF_STR, 13) == 0 ||
		strncasecmp(sol, "\n+CMS ERROR:", 12) == 0 ||
		strncasecmp(sol, "\n+CME ERROR:", 12) == 0;
}

static char *xgsm_readresp(int fd, int *rsize) {
	int rdbytes, n, size;
	char *resp, *nresp;

	size = 128;
	if ((resp = (char *) malloc(size)) == NULL) {
		perror("allocating response buffer");
		return NULL;
	}
	for (rdbytes = 0;;) {
		if (rdbytes + 1 >= size) {
			size = size * 2;
			if ((nresp = (char *) realloc(resp, size)) == NULL) {
				perror("allocating response buffer");
				free(resp);
				return NULL;
			}
			resp = nresp;
		}
		if ((n = read(fd, resp + rdbytes, size - rdbytes - 1)) < 0) {
			perror("reading serial device");
			free(resp);
			return NULL;
		}
		if (debug > 10)
			xgsm_hexdump(stderr, resp + rdbytes, n);
		rdbytes += n;
		if (xgsm_eoc(resp, rdbytes))
			break;
	}
	resp[rdbytes] = '\0';
	*rsize = rdbytes;

	fputs("<< RECV\n", stderr);
	xgsm_hexdump(stderr, resp, rdbytes);
	fputs("<<\n", stderr);

	xgsm_flushin(fd, 500);

	return resp;
}

static char *xgsm_sendcmd(int fd, const char *cmd, int len, int *rsize) {
	if (len < 0)
		len = strlen(cmd);
	fputs(">> SEND\n", stderr);
	xgsm_hexdump(stderr, cmd, len);
	fputs(">>\n", stderr);
	if (xgsm_write(fd, cmd, len, -1) != len) {
		perror("writing serial device");
		return NULL;
	}

	return xgsm_readresp(fd, rsize);
}

static char *xgsm_vsendcmd(int fd, int *rsize, const char *cfmt, ...) {
	int n;
	char *cmd, *resp;
	va_list args;

	va_start(args, cfmt);
	n = vasprintf(&cmd, cfmt, args);
	va_end(args);
	if (n < 0) {
		perror("building SMS command");
		return NULL;
	}
	resp = xgsm_sendcmd(fd, cmd, n, rsize);
	free(cmd);

	return resp;
}

static int xgsm_want_reply(int fd, const char *reply, int timeo) {
	int len, cnt, n, i;
	char buf[32];

	for (len = strlen(reply); len > 0;) {
		if ((cnt = len) > sizeof(buf))
			cnt = sizeof(buf);
		if ((n = xgsm_read(fd, buf, cnt, timeo)) <= 0) {
			fprintf(stderr, "end of data while reading requested reply ('%s')",
				reply);
			return -1;
		}
		if (debug > 10)
			xgsm_hexdump(stderr, buf, n);
		for (i = 0; i < n; i++, reply++)
			if (toupper(*reply) != toupper(buf[i])) {
				fprintf(stderr, "mismatch in wanted reply: '%c' != '%c'",
					toupper(*reply), toupper(buf[i]));
				return 0;
			}
		len -= n;
	}

	return 1;
}

static char *xgsm_send_txt_sms(int fd, const char *nbr, const char *msg, int *rsize) {
	int n;
	char *rmsg;

	if ((n = asprintf(&rmsg, "AT+CMGS=\"%s\",145" CMD_LF, nbr)) < 0)
		return NULL;
	fputs(">> SEND SMS CMD\n", stderr);
	xgsm_hexdump(stderr, rmsg, n);
	fputs(">>\n", stderr);
	if (xgsm_write(fd, rmsg, n, -1) != n) {
		perror("writing serial device");
		free(rmsg);
		return NULL;
	}
	free(rmsg);

	fputs(">> MODEM WAIT REPLY\n", stderr);
	if (xgsm_want_reply(fd, CRLF_STR "> ", 2000) <= 0)
		return NULL;

	if ((n = asprintf(&rmsg, "%s\x1a", msg)) < 0)
		return NULL;
	fputs(">> SEND SMS DATA\n", stderr);
	xgsm_hexdump(stderr, rmsg, n);
	fputs(">>\n", stderr);
	if (xgsm_write(fd, rmsg, n, -1) != n) {
		perror("writing serial device");
		free(rmsg);
		return NULL;
	}
	free(rmsg);

	return xgsm_readresp(fd, rsize);
}

static int xgsm_CMGS_PDU_length(const char *hpdu) {
	unsigned int l;

	if (sscanf(hpdu, "%2x", &l) != 1)
		return -1;

	return strlen(hpdu) / 2 - l - 1;
}

static char *xgsm_send_sms(int fd, const char *nbr, const char *scnbr,
			   const char *msg, int *rsize) {
	int n;
	char *hpdu, *rmsg;

	if ((hpdu = xgsm_create_hexPDU(nbr, scnbr, msg)) == NULL)
		return NULL;

	fprintf(stderr, "!! PDU DATA: %s\n", hpdu);

	if ((n = asprintf(&rmsg, "AT+CMGS=%d" CMD_LF, xgsm_CMGS_PDU_length(hpdu))) < 0)
		return NULL;
	fputs(">> SEND SMS CMD\n", stderr);
	xgsm_hexdump(stderr, rmsg, n);
	fputs(">>\n", stderr);
	if (xgsm_write(fd, rmsg, n, -1) != n) {
		perror("writing serial device");
		free(rmsg);
		free(hpdu);
		return NULL;
	}
	free(rmsg);

	fputs(">> MODEM WAIT REPLY\n", stderr);
	if (xgsm_want_reply(fd, CRLF_STR "> ", 2000) <= 0) {
		free(hpdu);
		return NULL;
	}

	n = asprintf(&rmsg, "%s\x1a", hpdu);
	free(hpdu);
	if (n < 0)
		return NULL;
	fputs(">> SEND SMS DATA\n", stderr);
	xgsm_hexdump(stderr, rmsg, n);
	fputs(">>\n", stderr);
	if (xgsm_write(fd, rmsg, n, -1) != n) {
		perror("writing serial device");
		free(rmsg);
		return NULL;
	}
	free(rmsg);

	return xgsm_readresp(fd, rsize);
}

int main(int ac, char **av) {
	int p, fd, rsize, canon = 1, verbose = 0, mlist = 0, mdel = -1, txt = 0;
	char *resp;
	const char *dev = NULL, *scnbr = NULL, *msg = NULL, *nbr = NULL;

	while ((p = getopt(ac, av, "d:m:n:uUhvlR:tC:D:")) != -1) {
		switch (p) {
		case 'd':
			dev = optarg;
			break;

		case 'u':
			xgsm_setnobuf(0);
			break;

		case 'U':
			canon = 0;
			break;

		case 'v':
			verbose = 1;
			break;

		case 'l':
			mlist = 1;
			break;

		case 't':
			txt = 1;
			break;

		case 'R':
			mdel = atoi(optarg);
			break;

		case 'D':
			debug = atoi(optarg);
			break;

		case 'C':
			scnbr = optarg;
			break;

		case 'm':
			msg = optarg;
			break;

		case 'n':
			nbr = optarg;
			break;

		case 'h':
		default:
			xgsm_usage(av[0]);
			break;
		}
	}
	if (dev == NULL || msg == NULL || nbr == NULL) {
		xgsm_usage(av[0]);
		return 1;
	}
	if ((fd = open(dev, O_RDWR)) == -1) {
		perror(dev);
		return 2;
	}
	if (!canon)
		xgsm_setnobuf(fd);

	xgsm_flushin(fd, 500);

	resp = xgsm_sendcmd(fd, "AT&F" CMD_LF, -1, &rsize);
	free(resp);

	resp = xgsm_sendcmd(fd, "ATE0" CMD_LF, -1, &rsize);
	free(resp);

	resp = xgsm_sendcmd(fd, "AT+CMEE=1" CMD_LF, -1, &rsize);
	free(resp);

	if (verbose) {
		resp = xgsm_sendcmd(fd, "AT+CFUN=1" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CPIN?" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CSQ" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+COPS?" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+COPS=0" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+COPN" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT^SMONC" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT^MONP" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CREG?" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CREG=1" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CGMI" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CGMM" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CGMR" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CGSN" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CSCS=?" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CIMI" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CCID" CMD_LF, -1, &rsize);
		free(resp);
		resp = xgsm_sendcmd(fd, "AT+CNUM" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CSCA?" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CPMS?" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CSMS=1" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CSMP?" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CLIP=1" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CNMI=0,0,0,0,1" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_sendcmd(fd, "AT+CGSMS=1" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_vsendcmd(fd, &rsize, "AT&V" CMD_LF, txt);
		free(resp);
	}

	resp = xgsm_sendcmd(fd, "AT^SM20=0,0" CMD_LF, -1, &rsize);
	free(resp);

	resp = xgsm_vsendcmd(fd, &rsize, "AT+CMGF=%d" CMD_LF, txt);
	free(resp);

	if (txt) {
		resp = xgsm_sendcmd(fd, "AT+CSMP=17,169,0,0" CMD_LF, -1, &rsize);
		free(resp);

		resp = xgsm_send_txt_sms(fd, nbr, msg, &rsize);
		free(resp);
	} else {
		resp = xgsm_send_sms(fd, nbr, scnbr, msg, &rsize);
		free(resp);
	}

	resp = xgsm_sendcmd(fd, "AT+CEER" CMD_LF, -1, &rsize);
	free(resp);

	if (mlist) {
		resp = xgsm_sendcmd(fd, "AT+CMGL=\"ALL\"" CMD_LF, -1, &rsize);
		free(resp);
	}

	if (mdel > 0) {
		resp = xgsm_vsendcmd(fd, &rsize, "AT+CMGD=%d" CMD_LF, mdel);
		free(resp);
	}

	close(fd);

	return 0;
}

