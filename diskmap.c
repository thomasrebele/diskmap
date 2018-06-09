/*
Diskmap - hash map backed by a memory mapped file on disk
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

// have a look at README.md for more information

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdbool.h>

#define min(X,Y) (((X) < (Y)) ? (X) : (Y))
#define max(X,Y) (((X) > (Y)) ? (X) : (Y))

#ifndef DEBUG
#define DEBUG true
#endif

#define handle_error(msg) \
   do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define debug_print(fmt, ...) \
   do { if (DEBUG) fprintf(stderr, "   " fmt, __VA_ARGS__); } while (0)

int debug_multimap = 0;



//********************************************************************************
// memory management
//********************************************************************************

/** Store "pointers" to memory, as indices, starting from the header */
typedef uint64_t MEMPTR;
typedef uint64_t BLOCK_POS;

/** Macro for convenience, assumes memory handle called 'mem' */
#define MEMPTR(POS) ((void*)(mem->header) + (POS))
#define BLOCK(POS) ((struct mem_block*)((void*)(mem->header) + (POS)))

/** Header of the memory */
struct mem_header {
	BLOCK_POS next_free;
	size_t size;
};

/** An allocated part of memory */
struct mem_block {
	BLOCK_POS prev;
	BLOCK_POS next;
};

/** Handle used by clients */
struct mem {
	struct mem_header* header;
	int fd;
};

/** Setup memory header and first block */
void mem_init(struct mem *mem) {
	BLOCK_POS first = sizeof(struct mem);
	BLOCK_POS second = first + sizeof(struct mem_block);
	mem->header->next_free = second;
	BLOCK(first)->next = second;
	BLOCK(first)->prev = 0;
	BLOCK(second)->prev = first;
	BLOCK(second)->next = 0;
}

/** Create a memory mapping at the specified file */ 
struct mem* mem_create(char *file, int initial_size) {
	int fd;
	if((fd = open(file, O_RDWR | O_CREAT , (mode_t)0600)) == -1) handle_error("Error opening file for writing");
	if (lseek(fd, initial_size+1, SEEK_SET) == -1) handle_error("Error seeking");
	if (write(fd, "", 1) == -1) handle_error("Error writing");

	void* ptr = mmap(NULL, initial_size, PROT_WRITE, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) handle_error("Error mapping file to memory");
	struct mem *mem = malloc(sizeof(struct mem));
	mem->fd = fd;
	mem->header = ptr;
	mem->header->size = initial_size;
	mem_init(mem);
	return mem;
}

/** Write changes to disk */
void mem_sync(struct mem* mem) {
	if (msync(mem->header, mem->header->size, MS_SYNC) == -1) handle_error("Error syncing");
}

/** Remove memory mapping */
void mem_unmap(struct mem* mem) {
	if(munmap(mem->header, mem->header->size) == -1) handle_error("Error unmapping");
}

/** Close without writing to disk */
void mem_abandon(struct mem* mem) {
	mem_unmap(mem);
	struct stat fileInfo = {0};
	if (fstat(mem->fd, &fileInfo) == -1) handle_error("Error getting the file size");
	close(mem->fd);
	mem->header = NULL;
	mem->fd = 0;
}

/** Close everything */
void mem_close(struct mem* mem) {
	mem_sync(mem);
	mem_abandon(mem);
}

/** Make underlying file bigger */
void mem_resize(struct mem *mem, size_t size) {
	debug_print("resizing from %llu to %llu\n", mem->header->size, size);
	mem_sync(mem);
	mem_unmap(mem);
	if (lseek(mem->fd, size, SEEK_SET) == -1) handle_error("Error seeking");
	if (write(mem->fd, "", 1) == -1) handle_error("Error writing");
	void* old_ptr = mem->header;
	mem->header = mmap(old_ptr, size, PROT_WRITE, MAP_SHARED , mem->fd, 0);
	if(old_ptr != mem->header) {
		debug_print("mem_resize changed ptr from %p to %p!\n", old_ptr, mem->header);
		//exit(-1);
	}
	mem->header->size = size;
}

/** Reserve a certain amount of memory */
uint64_t mem_alloc(struct mem *mem, size_t size) {
	BLOCK_POS free = mem->header->next_free;
	size_t needed = sizeof(struct mem_block) + size;
	while(!(BLOCK(free)->next - free > needed || BLOCK(free)->next == 0)) {
		free = BLOCK(free)->next;
	}
	BLOCK_POS prev = BLOCK(free)->prev;
	BLOCK_POS next = BLOCK(prev)->next;

	if(BLOCK(free)->next == 0) {
		next = free + needed;
		// align
		next = ((next >> 2)+1)<<2;
		if(next + sizeof(struct mem_block) >= mem->header->size) {
			size_t new_size = next + sizeof(struct mem_block);
			new_size = (size_t)(new_size * 1.5);
			new_size = ((new_size>>8)+1)<<8;
			mem_resize(mem, new_size);
		}
		BLOCK(free)->next = next;
		BLOCK(next)->next = 0;
	}
	mem->header->next_free = BLOCK(free)->next;

	BLOCK(free)->next = next;
	BLOCK(free)->prev = prev;
	BLOCK(prev)->next = free;
	BLOCK(next)->prev = free;

	return free + sizeof(struct mem_block);
}

/** Give ownership of memory back to memory management */
void mem_free(struct mem *mem, void *ptr) {
	// TODO: use index instead of pointer
	// TODO: merge continuous free blocks
	struct mem_block* block = ptr - sizeof(struct mem_block);
	struct mem_block* prev = (void*)mem + block->prev;
	struct mem_block* next = (void*)mem + block->next;
	prev->next = (void*)next - (void*)mem;
	next->prev = (void*)prev - (void*)mem;
	block->next = mem->header->next_free;
	mem->header->next_free = (void*)block - (void*)mem;
}

/** Write string to memory mapped file. Returns its position */
MEMPTR mem_insert_str(struct mem *mem, char *str) {
	size_t len = strlen(str) + 1;
	MEMPTR ptr = mem_alloc(mem, len);
	memcpy(MEMPTR(ptr), str, len);
	return ptr;
}

//********************************************************************************
// hash table
//********************************************************************************

// resolve ptr into mem; assumes hash table handle is 'table'
#define HTMEMPTR(PTR) (((char*)((struct mem*)table->mem)->header) + (PTR))
#define HTHEADER(TABLE) ((struct hash_table_header*)( HTMEMPTR((TABLE)->header_ptr)  ))

/* FNV-1a */
uint64_t hash(char *str){
	uint64_t v = 0xcbf29ce484222325;
	do {
		v = v ^ *str;
		v *= 0x100000001b3;
	} while(*str++ != '\0');
	return v == 0 ? 1 : v;
}

/** Handle for hash table. Resides in main memory. */
struct hash_table { // note: this struct only contains runtime configuration
	void* mem;                          // hande for memory-mapped file
	uint64_t header_ptr;                // ptr to hashmap (into mapped area)
};

/** Header for hash table. Resides in memory mapped file. Never moves. */
struct hash_table_header {
	size_t bucket_count;      // number of slots in hash table
	size_t bucket_size;       // size of one slot in bytes
	size_t filled;            // how many slots are occupied
	size_t max_dist;          // how many slots is an entry from its ideal position?
	uint64_t buckets_ptr;     // ptr to bucket array (into mapped area)
};

/** A bucket for storing a key and its hash (to speed up comparisons) */
struct hash_bucket {
	uint64_t hash; // hashcode of the key, 0 indicates empty bucket
	uint64_t keyptr; // index relative to underlying mem
};

/** Gets hash bucket as a real pointer. Resizing mem or hash table invalidates this pointer. Buckets_ptr points to the bucket array */
struct hash_bucket* ht_bucket_rel(struct hash_table* table, size_t idx, uint64_t buckets_ptr) {
	struct hash_bucket* bucket = (void*)(HTMEMPTR(buckets_ptr) + idx*HTHEADER(table)->bucket_size);
	return bucket;
}

/** Gets hash bucket as a real pointer. Resizing mem or hash table invalidates this pointer */
struct hash_bucket* ht_bucket(struct hash_table* table, size_t idx) {
	return ht_bucket_rel(table, idx, HTHEADER(table)->buckets_ptr);
}

/** Get main memory addr of value (relative to bucket pointer). You must not write more data than requested by ht_init(...) */
void* ht_value_rel(struct hash_bucket* bucket) {
	return ((char*)bucket) + sizeof(struct hash_bucket);
}

/** Get main memory addr of value (by table and bucket index). You must not write more data than requested by ht_init(...) */
void* ht_value(struct hash_table* table, int64_t bucket_idx) {
	return ht_value_rel(ht_bucket(table, bucket_idx));
}

/** Init a hash table. Writes header and array of buckets to memory mapped file.
 * @param value_size amount of space reserved in each bucket for user-defined content */
struct hash_table ht_init(void* mem, size_t value_size) {
	struct hash_table result;
	struct hash_table *table = &result;
	table->mem = mem;
	table->header_ptr = mem_alloc(mem, sizeof(struct hash_table_header));

	struct hash_table_header* header = HTHEADER(table);
	header->bucket_size = sizeof(struct hash_bucket) + value_size;
	header->bucket_count = 2;
	header->filled = 0;
	header->max_dist = 0;

	// need temporary variable, because mem_alloc might change header
	uint64_t tmp = mem_alloc(mem, header->bucket_count * header->bucket_size);
	header = HTHEADER(table);
	header->buckets_ptr = tmp;
	for(int i=0; i<header->bucket_count; i++) {
		ht_bucket(table, i)->hash = 0;
	}
	return result;
}

/** Get index of first non-empty bucket, that follows bucket with index 'bucket_idx'
 * Returns -1 if none exists */
int64_t ht_next(struct hash_table* table, int64_t bucket_idx) {
	for(int i=bucket_idx + 1; i<HTHEADER(table)->bucket_count; i++) { \
		if(ht_bucket(table, i)->hash != 0) return i;
	}
	return -1;
}

/** Macros for iterating over all keys in hash table. Cannot be nested */
#define HTFOREACH(TABLE)       for(int i=ht_next((TABLE), -1); i >= 0; i = ht_next((TABLE), i)) 
// returns key of current bucket
#define HTFOREACH_KEY(TABLE)   MEMPTR(ht_bucket((TABLE), i)->keyptr)

/** Print statistics, for debugging */
void ht_print_stat(struct hash_table* table) {
	printf("---------------------------------------\n");
	struct hash_table_header* header = HTHEADER(table);
	printf("hashtable header idx %zu, (current addr %p), bucket size %zu, 'key size' %zu, max dist %zu\n", table->header_ptr, header, header->bucket_size, sizeof(struct hash_bucket), header->max_dist); 
	printf("bucket_count %zu, filled %zu, filled %g %%\n", header->bucket_count, header->filled, (float)header->filled  / header->bucket_count * 100);
}

/** Print statistics and content, for debugging. Do not use it for big hash tables. */
void ht_print(struct hash_table* table) {
	ht_print_stat(table);
	for(int i=0; i<HTHEADER(table)->bucket_count; i++) {
		struct hash_bucket* bucket = ht_bucket(table, i);
		printf("table ptr %p bucket %d hash %llx, addr %p", HTHEADER(table)->buckets_ptr, i, bucket->hash, &(bucket->hash));
		if(bucket->hash != 0) {
			printf(", key '%s'", HTMEMPTR(bucket->keyptr));
			size_t best = bucket->hash % HTHEADER(table)->bucket_count;
			printf(" best bucket %d", best);
			uint64_t value_size = HTHEADER(table)->bucket_size - sizeof(struct hash_bucket);
			if(value_size > 0) {
				printf(", value ");
				unsigned char* val = ht_value(table, i);
				for(int j=0; j<min(value_size, 10); j++) {
					printf(" %x", val[j]);
				}
			}
		}
		printf("\n");
	}
	printf("---------------------------------------\n");
}

// declare method used in insert
void ht_resize(struct hash_table* table);

/** Search bucket index of key, retun -1 if not existing */
int64_t ht_lookup(struct hash_table* table, char* key) {
	uint64_t h = hash(key);
	size_t pos = h % HTHEADER(table)->bucket_count, dist = 0;
	struct hash_bucket* bucket;
	do {
		bucket = ht_bucket(table, pos);
		// only need to check 'max_dist'-many buckets
		if(bucket->hash == 0 || dist > HTHEADER(table)->max_dist) return -1;
		if(bucket->hash == h) {
			int cmp = strcmp(key, HTMEMPTR(bucket->keyptr));
			if(cmp == 0) return pos;
		}
		if(++pos == HTHEADER(table)->bucket_count) pos = 0; // wrap at end of table
		dist++;
	} while(1);
	return pos;
}

// adopted from https://www.sebastiansylvan.com/post/robin-hood-hashing-should-be-your-default-hash-table-implementation/
/** You probably want to use ht_insert(...). Inserts the key into the hash table. Makes table bigger if necessary. 
 * @param key position of key string stored in memory mapped file
 * Returns bucket index. */
int64_t ht_insert_intern(struct hash_table* table, uint64_t key) {
	// check whether we need to make the hashtable bigger
	int max_filled = min(floor(0.9 * HTHEADER(table)->bucket_count), HTHEADER(table)->bucket_count-1);
	if(HTHEADER(table)->filled >= max_filled) {
		ht_resize(table);
	}
	HTHEADER(table)->filled++;

	// robin hood insert
	int64_t result = -1;
	struct hash_bucket* to_insert = calloc(1, HTHEADER(table)->bucket_size);
	struct hash_bucket* tmp = calloc(1, HTHEADER(table)->bucket_size);
	to_insert->hash = hash(HTMEMPTR(key));
	to_insert->keyptr = key;
	int pos = to_insert->hash % HTHEADER(table)->bucket_count;
	size_t insert_dist = 0;
	do {
		// search empty bucket
		struct hash_bucket* bucket = ht_bucket(table, pos);
		if(bucket->hash == 0) {
			// insert new entry
			memcpy(bucket, to_insert, HTHEADER(table)->bucket_size);
			if(result < 0) result = pos;
			HTHEADER(table)->max_dist = max(HTHEADER(table)->max_dist, insert_dist);
			break;
		}
		else {
			// steal from the rich
			int exist_dist = ((pos - bucket->hash) % HTHEADER(table)->bucket_count);
			if(insert_dist > exist_dist) {
				// copy everything, hash, key, and value (if existing)
				memcpy(tmp, bucket, HTHEADER(table)->bucket_size);
				memcpy(bucket, to_insert, HTHEADER(table)->bucket_size);
				memcpy(to_insert, tmp, HTHEADER(table)->bucket_size);
				HTHEADER(table)->max_dist = max(HTHEADER(table)->max_dist, insert_dist);
				insert_dist = exist_dist;
				if(result < 0) result = pos;
			}
		}
		insert_dist++; pos++;
		if(pos == HTHEADER(table)->bucket_count) pos = 0;
	} while(1);
	return result;
}

/** Make bucket array twice as big, and copy all existing keys (with their values) */
void ht_resize(struct hash_table* table) {
	size_t old_count = HTHEADER(table)->bucket_count;
	HTHEADER(table)->bucket_count *= 2;
	size_t size = HTHEADER(table)->bucket_count * HTHEADER(table)->bucket_size;

	uint64_t old_ptr = HTHEADER(table)->buckets_ptr;
	HTHEADER(table)->filled = 0;
	HTHEADER(table)->max_dist = 0;

	// temporary variable necessary, as alloc invalidates pointer used by HTHEADER(table)
	uint64_t tmp = mem_alloc(table->mem, size);
	HTHEADER(table)->buckets_ptr = tmp;
	memset(HTMEMPTR(HTHEADER(table)->buckets_ptr), 0, size);
	uint64_t value_size = HTHEADER(table)->bucket_size - sizeof(struct hash_bucket);
	for(int i=0; i<old_count; i++) {
		struct hash_bucket* old_bucket = ht_bucket_rel(table, i, old_ptr);
		if(old_bucket->hash != 0) {
			int64_t bucket_idx = ht_insert_intern(table, old_bucket->keyptr);
			memcpy(ht_value(table, bucket_idx), ht_value_rel(old_bucket), value_size);
		}
	}
	memset(HTMEMPTR(old_ptr), 0xff, old_count * HTHEADER(table)->bucket_size);
}

/** Insert string into hashtable. Returns bucket index */
int64_t ht_insert_str(struct hash_table* table, char* key) {
	// check whether key already exists
	int64_t pos = ht_lookup(table, key);
	if(pos >= 0) pos;

	// internalize string
	uint64_t keyptr = mem_insert_str(table->mem, key);
	return ht_insert_intern(table, keyptr);
}

/** Insert a key value pair into multi-map hash table. Requires ht_init(..., sizeof(MEMPTR)) */
void multimap_insert_key_val(struct hash_table* table, char* key, char* val) {
	int64_t pos = ht_lookup(table, key);
	// key didn't exist yet, create hashtable
	struct hash_table tab;
	if(pos < 0) {
		pos = ht_insert_str(table, key);
		tab = ht_init(table->mem, 0);
		uint64_t* val = ht_value(table, pos);
		val[0] = tab.header_ptr;
	}
	else {
		uint64_t* val = ht_value(table, pos);
		tab.mem = table->mem;
		tab.header_ptr = val[0];
	}
	ht_insert_str(&tab, val);
}

/** Get a multimap, which was stored in the value of another hashmap */
struct hash_table multimap_get(struct mem* mem, void* ptr) {
	struct hash_table tab;
	tab.mem = mem;
	tab.header_ptr = ((uint64_t*)ptr)[0];
	return tab;
}


//********************************************************************************
// main
//********************************************************************************


int main(int argc, char *argv[])
{
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	if(argc != 2) {
		printf("usage: %s <file>\n", argv[0]);
		return -1;
	}

	printf("create a disk map with an initial size of 420 bytes\n");
	struct mem* mem = mem_create(argv[1], 420);

	struct hash_table tab = ht_init(mem, sizeof(MEMPTR));
	struct hash_table* table = &tab;

	printf("inserting values\n");
	multimap_insert_key_val(table, "key0", "key0val0");
	multimap_insert_key_val(table, "key0", "key0val1");
	multimap_insert_key_val(table, "key0", "key0val2");
	multimap_insert_key_val(table, "key1", "key1val0");
	multimap_insert_key_val(table, "key1", "key1val1");
	multimap_insert_key_val(table, "key2", "key2val0");

	printf("reading values\n");
	HTFOREACH(table) {
		printf("key %s\n", HTFOREACH_KEY(table));
		struct hash_table tab = multimap_get(mem, ht_value(table, i));
		HTFOREACH(&tab) {
			printf("\t val %s\n", HTFOREACH_KEY(&tab));
		}
	}

	MEMPTR ptr = mem_alloc(mem, 20);
	sprintf(MEMPTR(ptr), "%s", "END OF USED MEM");

	mem_close(mem);
	printf("done\n");
	return 0;
}
