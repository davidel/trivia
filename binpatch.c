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

static void usage(const char* prg)
{
	fprintf(stderr,
            "Use: %s -i INPUT_FILE [-o OUTPUT_FILE] [-d] MATCH_BYTE .. -- PATCH_BYTE ..\n",
            prg);
}

int main(int ac, const char** av)
{
	int i;
	int dry_run = 0;
	const char* in_file_path = NULL;
	const char* out_file_path = NULL;
	FILE* fin;
	FILE* fout = stdout;
	size_t match_pos = 0;
	size_t match_size = 0;
	char match_buf[4096];
	size_t patch_size = 0;
	char patch_buf[4096];
	size_t m = 0;
	size_t match_count = 0;
	long matches[32];

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-i") == 0) {
			if (++i < ac)
				in_file_path = av[i];
		} else if (strcmp(av[i], "-o") == 0) {
			if (++i < ac)
				out_file_path = av[i];
		} else if (strcmp(av[i], "-d") == 0)
			dry_run++;
		else
			break;
	}
	if (in_file_path == NULL) {
		usage(av[0]);
		return 1;
	}
	for (; i < ac && strcmp(av[i], "--") != 0 && match_size < sizeof(match_buf); i++)
		match_buf[match_size++] = (char) strtol(av[i], NULL, 0);
	for (++i; i < ac && patch_size < sizeof(patch_buf); i++)
		patch_buf[patch_size++] = (char) strtol(av[i], NULL, 0);
	if (match_size != patch_size) {
		fprintf(stderr, "Match and patch size must match!\n");
		return 2;
	}

	if ((fin = fopen(in_file_path, "rb")) == NULL) {
		perror(in_file_path);
		return 3;
	}

	while (match_count < 32 && (i = fgetc(fin)) != EOF) {
		if (match_buf[match_pos] == (char) i) {
			if (match_pos == 0)
				matches[match_count] = ftell(fin) - 1;
			match_pos++;
			if (match_pos == match_size) {
				match_pos = 0;
				fprintf(stderr, "match[%ld] @ %ld\n", match_count,
                        matches[match_count]);
				match_count++;
				continue;
			}
		} else if (match_pos > 0) {
			match_pos = 0;
			fseek(fin, matches[match_count] + 1, SEEK_SET);
		}
	}
	if (!dry_run) {
		fprintf(stderr, "Patch binary file [y/n]? ");
		if (fgetc(stdin) != 'y')
			return 5;

        if (out_file_path != NULL && (fout = fopen(out_file_path, "w+b")) == NULL) {
            perror(out_file_path);
            return 4;
        }
		rewind(fin);
		while ((i = fgetc(fin)) != EOF) {
			long off = ftell(fin) - 1;

			if (m < match_count && off == matches[m]) {
				size_t j;

				for (j = 0; j < patch_size; j++)
					fputc(patch_buf[j], fout);
				fseek(fin, j - 1, SEEK_CUR);
				m++;
			} else
				fputc(i, fout);
		}
        if (fout != stdout)
            fclose(fout);
	}
	fclose(fin);

	return 0;
}

