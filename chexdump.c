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



int main(int ac, char **av) {
	int i = 0, c, n = 16, t = 4;

	fprintf(stdout, "{\n%*s", t, " ");
	while ((c = fgetc(stdin)) != EOF) {
		if (i && (i % n) == 0)
			fprintf(stdout, "\n%*s", t, " ");
		fprintf(stdout, " 0x%02x,", c);
		i++;
	}
	fprintf(stdout, "\n");
	fprintf(stdout, "};\n");

	return 0;
}

