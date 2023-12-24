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
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define MAIL_DATA_TAG	"<<MAIL-DATA>>"
#define MAIL_FROM	"MAIL FROM:"

#ifdef _MSC_VER
#define strncasecmp	strnicmp
#define strcasecmp	stricmp
#endif	/* #ifdef _MSC_VER */


char *extract_address(char const *str, char *addr, int asize) {
	int alen;
	char const *pat, *base, *end;

	if (!(pat = strchr(str, '@')))
		return NULL;
	for (base = pat - 1; base >= str && !strchr(":,;<> \t\r\n\"'", *base); base--);
	base++;
	for (end = pat + 1; *end != '\0' && !strchr(":,;<> \t\r\n\"'", *end); end++);
	alen = (int) (end - base);
	if (alen > asize)
		alen = asize - 1;
	strncpy(addr, base, alen);
	addr[alen] = '\0';
	return addr;
}


int main(int argc, char * argv[]) {
	int i, rcode = 1, uxmode = 0, mbox = 0;
	time_t tcur = time(NULL);
	FILE *pfin = stdin, *pfout = stdout;
	char *fnin = NULL, *fnout = NULL;
	char buffer[2048], from[256] = "", astime[128];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--input") == 0) {
			if (++i < argc)
				fnin = argv[i];
			continue;
		} else if (strcmp(argv[i], "--output") == 0) {
			if (++i < argc)
				fnout = argv[i];
			continue;
		} else if (strcmp(argv[i], "--unix") == 0) {
			++uxmode;
			continue;
		} else if (strcmp(argv[i], "--mbox") == 0) {
			++mbox;
			continue;
		}
	}
	if (fnin && !(pfin = fopen(fnin, "rb")))
		pfin = stdin;
	if (fnout && !(pfout = fopen(fnout, "wb")))
		pfout = stdout;
	while (fgets(buffer, sizeof(buffer) - 1, pfin) &&
	       strncmp(buffer, MAIL_DATA_TAG, sizeof(MAIL_DATA_TAG) - 1))
		if (mbox && !strncasecmp(buffer, MAIL_FROM, sizeof(MAIL_FROM) - 1))
			extract_address(buffer, from, sizeof(from) - 1);
	if (feof(pfin))
		goto clean_exit;
	if (mbox) {
		rcode = 2;
		if (!strlen(from))
			goto clean_exit;
		strcpy(astime, ctime(&tcur));
		astime[strlen(astime) - 1] = '\0';
		if (!uxmode)
			fprintf(pfout, "From %s %s\r\n", from, astime);
		else
			fprintf(pfout, "From %s %s\n", from, astime);
	}
	rcode = 3;
	if (!uxmode) {
		do {
			unsigned int rsize = fread(buffer, 1, sizeof(buffer), pfin);

			if (rsize && fwrite(buffer, 1, rsize, pfout) != rsize)
				goto clean_exit;
		} while (!feof(pfin));
	} else {
		while (fgets(buffer, sizeof(buffer) - 1, pfin)) {
			if ((i = strlen(buffer)) >= 2 && buffer[i - 1] == '\n' &&
			    buffer[i - 2] == '\r')
				buffer[--i - 1] = '\n';
			if (i && fwrite(buffer, 1, i, pfout) != i)
				goto clean_exit;
		}
	}
	fflush(pfout);
	rcode = 0;
clean_exit:
	if (pfin != stdin) fclose(pfin);
	if (pfout != stdout) fclose(pfout);
	return rcode;
}

