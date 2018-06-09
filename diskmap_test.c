/*
Diskmap - a hash map backed by a memory mapped file on disk
Copyright (C) 2018  Thomas Rebele

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>

#define main deactivated_main
#include "diskmap.c"
#undef main

bool ht_check(struct hash_table* table) {
	size_t filled = 0;
	HTFOREACH(table) {
		filled++;
	}
	if(HTHEADER(table)->filled != filled) {
		ht_print(table);
	}
	assert(HTHEADER(table)->filled == filled);
}

const char *bit_rep[16] = {
    [ 0] = "0000", [ 1] = "0001", [ 2] = "0010", [ 3] = "0011",
    [ 4] = "0100", [ 5] = "0101", [ 6] = "0110", [ 7] = "0111",
    [ 8] = "1000", [ 9] = "1001", [10] = "1010", [11] = "1011",
    [12] = "1100", [13] = "1101", [14] = "1110", [15] = "1111",
};

void print_byte(uint8_t byte) {
    printf("%s%s", bit_rep[byte >> 4], bit_rep[byte & 0x0F]);
}

#define MAKEKEY(i)  char key[100]; strcpy(key, "key"); sprintf(&key[3], "%d", i);
#define MAKEVAL(i)  char val[100]; sprintf(val, "%s", key); strcpy(&val[strlen(key)], "val"); sprintf(&val[strlen(key)+3], "%d", i);

/** Insert n strings into hashmap, and check whether there are n strings in the hashmap */
int test1() {
	struct mem* mem = mem_create("/tmp/diskmap_test", 4000);
	struct hash_table tab = ht_init(mem, 0);
	struct hash_table* table = &tab;

	time_t start = time(NULL);

	int n = 5000000;
	for(int i=0; i<n; i++) {
		MAKEKEY(i);
		if(i%100000 == 0) {
			printf("test1 calling insert for '%s'", key);
			printf(" #inserts / s: %.2f\n", i/((double)(time(NULL) - start)));
		}
		if(i%1000000 == 0) {
			ht_print_stat(table);
		}
		ht_insert_str(table, key);
	}

	assert(HTHEADER(table)->filled == n);

	// check
	for(int i=0; i<n; i++) {
		MAKEKEY(i);
		int pos = ht_lookup(table, key);
		if(pos < 0) {
			ht_print(table);
			printf("key not found: '%s'\n", key);
		}
		assert(pos >= 0);
	}
	
	printf("********************************************************************************\n");
	printf("*** test1 successful (n = %d)\n", n);
	printf("********************************************************************************\n");
	mem_abandon(mem);
}

/** Insert n strings into a multimap, and for the i-th string insert i values.
 * Thus enters O(n^2) key-value pairs. */
int test2() {
	struct mem* mem = mem_create("/tmp/diskmap_test", 65536);
	struct hash_table tab = ht_init(mem, sizeof(MEMPTR));
	struct hash_table* table = &tab;

	time_t start = time(NULL);

	int n = 3000, count = 0;
	for(int i=1; i<=n; i++) {
		MAKEKEY(i);

		/*if(strcmp(key, "key10") == 0)*/ debug_multimap = 1;
		for(int j=0; j<i; j++) {
			MAKEVAL(j);
			multimap_insert_key_val(table, key, val);
			count++;

			if(count%100000 == 0) {
				printf("test1 calling insert for '%s' value '%s'", key, val);
				printf(" #inserts / s: %.2f\n", count/((double)(time(NULL) - start)));
			}
		}
	}

	assert(HTHEADER(table)->filled == n);

	// check
	for(int i=1; i<=n; i++) {
		MAKEKEY(i);
		int64_t bucket_idx = ht_lookup(table, key);
		uint64_t *htval = ht_value(table, bucket_idx);
		struct hash_table tab = multimap_get(mem, htval);
		int filled = HTHEADER(&tab)->filled;
		if(filled != i) {
			printf("some entry missing for key %s\n", key);
			ht_print(&tab);
		}

		assert(HTHEADER(&tab)->filled == i);
	}

	printf("********************************************************************************\n");
	printf("*** test2 successful (n = %d)\n", n);
	printf("********************************************************************************\n");
	mem_abandon(mem);
}

int key_buckets() {
//	// print last bits of hash for some keys
//	for(int i=0; i<1000; i++) {
//		char key[100];
//		strcpy(key, "key");
//		sprintf(&key[3], "%d", i);
//		printf("'%s' hash ", key);
//		print_byte(hash(key)%256);
//		printf("\n");
//	}
}

int main(int argc, char *argv[]) {
	test1();
	test2();
	printf("all tests done, exiting\n");
	return 0;
}



