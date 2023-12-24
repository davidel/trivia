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


#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static unsigned long long get_mstime(void)
{
        struct timeval tm;

        gettimeofday(&tm, NULL);

        return tm.tv_sec * 1000ULL + tm.tv_usec / 1000;
}

static FILE *xfopen(char const *path, char const *mode)
{
	FILE *file;

	if ((file = fopen(path, mode)) == NULL) {
		perror(path);
		exit(2);
	}

	return file;
}

static int is_rfc822_atext(int c)
{
	return isalpha(c) || isdigit(c) || strchr("!#$%&'*+-/=?^_`{|}~", c) != NULL;
}

static char const *is_dot_atom(char const *str, char const *top)
{
	for (;;) {
		char const *atm = str;

		for (; str < top && is_rfc822_atext(*str); str++);
		if (atm == str)
			return NULL;
		if (str == top || *str != '.')
			break;
		str++;
	}

	return str;
}

static char const *parse_hostname(char const *hname, char const *top)
{
	return is_dot_atom(hname, top);
}

static int is_valid_host(char const *hname, char const *top)
{
	char const *eoh;

	if ((eoh = parse_hostname(hname, top)) == NULL)
		return 0;

	return eoh == top;
}

static int is_valid_addr_spec(char const *addr, char const *top)
{
	char const *caddr;

	if ((caddr = is_dot_atom(addr, top)) == NULL)
		return 0;
	if (caddr == top)
		return 1;
	if (*caddr++ != '@')
		return 0;

	return is_valid_host(caddr, top);
}

static int is_split_msg(char const *ln)
{
	unsigned int d, h, m, s, y;
	char addr[256], wday[8], ymon[8];

	return sscanf(ln, "From %255s %3s %3s %u %u:%u:%u %u",
		      addr, wday, ymon, &d, &h, &m, &s, &y) == 8 &&
		is_valid_addr_spec(addr, addr + strlen(addr));
}

static void usage(char *prg)
{
	fprintf(stderr, "use: %s [-i MBPATH] [-d OUTDIR] [-X]\n", prg);
	exit(1);
}

int main(int argc, char *argv[])
{
	int i, lnum, add_xfile = 0;
	unsigned long long mstm;
	FILE *ofil, *ifil = stdin;
	char const *ipath = NULL, *outdir = ".";
	char hname[256], lnbuf[4096];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			if (++i < argc)
				outdir = argv[i];
		} else if (strcmp(argv[i], "-i") == 0) {
			if (++i < argc)
				ipath = argv[i];
		} else if (strcmp(argv[i], "-X") == 0)
			add_xfile = 1;
		else
			usage(argv[0]);
	}
	if (ipath != NULL)
		ifil = xfopen(ipath, "r");
	gethostname(hname, sizeof(hname));
	for (ofil = NULL, lnum = 1; fgets(lnbuf, sizeof(lnbuf) - 1, ifil);
	     lnum++) {
		if (strncmp(lnbuf, "From ", 5) == 0 && is_split_msg(lnbuf)) {
			if (ofil != NULL)
				fclose(ofil);
			mstm = get_mstime();
			snprintf(lnbuf, sizeof(lnbuf), "%s/%llu.%d.%u.%s",
				 outdir, mstm, lnum, getpid(), hname);
			ofil = xfopen(lnbuf, "w");
			if (add_xfile)
				fprintf(ofil, "X-SplitMB-File: %llu.%d.%u.%s\n",
					mstm, lnum, getpid(), hname);
		} else if (ofil != NULL)
			fputs(lnbuf, ofil);
	}
	if (ofil != NULL)
		fclose(ofil);
	if (ifil != stdin)
		fclose(ifil);

	return 0;
}

