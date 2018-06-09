
# diskmap

Diskmap is a hash map backed by a memory mapped file on disk

## Requirements

* C compiler
* Linux kernel; 

  diskmap might work on other kernels that support `mmap` (with `MAP_SHARED`)

## Features
* hash set of strings
* hash map possible
* map string to set of strings (multi-map)
* robin hood hashing

## Try it

This project is a simple C library that implements an on-disk hash map in about 500 lines of C code.
For an example how to use it, see the function `main()` in the file `diskmap.c`, or the file `diskmap_test.c`.

* diskmap.c: It shows how to insert key-value pairs into a multi-map.
  You can run it with the command
      
      gcc diskmap.c && ./a.out cache

* diskmap_test.c: It runs two tests. The first inserts several million entries into a hash map, and check whether the hash map contains them all.
  The second runs a similar test with a multi-map. It also measures the insertion speed.
  You can run it with the command
      
      gcc diskmap_test.c && ./a.out cache

  My laptop achieves around 450000 insertions per second, with an i3-7100, 8 GB RAM, and 237 GB SSD.

## Implementation
- the basis of diskmap is a memory mapped file, with memory management (called mem)
- the address space is managed similarly to malloc/free in C
- strings are stored in memory mapped file, too. No deduplication is performed.
- next layer is a hash set. Adding new elements automatically increases size 
  of bucket array / memory mapped file, and invalidates all previous pointers.
  Addresses are therefore stored as positions of the memory mapped file
- the hash set can easily be turned into a hashmap, by giving the client 
  some space in each bucket. The client can then store a fixed-size value for each key
- the multi-map stores a pointer for each entry in such a hash-map. This pointer refers to
  a hash set that contains all values for one key

## Limitations / TODOs
* mem_free does not combine freed blocks
* use mem_free in hash set (currently it always requests more space on the disk)

## License

This project is licensed under the AGPL, version 3 ([LICENSE](LICENSE) or https://www.gnu.org/licenses/agpl-3.0.en.html).
If you need a different license, please contact me.

