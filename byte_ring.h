#ifndef __BYTE_RING_H__
#define __BYTE_RING_H__

#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <unistd.h>

//#define BR_STDIO_DEBUG
//#define BR_ASSERT_ACTIVE
//#define BR_SHRED_OLD_DATA

// this data structure is a ring buffer, that is organized into 'lines'
//			a line is a byte array, but easier to type
// the data structure is told how many lines are required for storage
//			as well as how long a line can be at maximum
// the structure will not swap to a new a line unless seek is called, or
//			if writing more data to the current line will exceed the line length
//			in which case the programmer is left to figure the line's validity

// the API exposes the ability to query a line with arbitrary functions
// the function can return any int, and the br_is_ready function will hand
//			the result back to the caller
// the br_pop function will use the value to determine whether or not to
//			TRUNCATE, IGNORE, or POP the data into a provided buffer,
//			see BR_READY_FOR_POP enum
// the API exposes all line lengths, including the line currently being written
// this helps the programmer ensure that a line is valid at any time

typedef struct byte_ring byte_ring_t;

// this enum states what a br_ready_for_pop function should return
// where BR_TRUNCATE means delete the current line
// BR_NOT_READY means don't alter the ring
// and BR_READY means pop this line then seek the next one
typedef enum
{
	BR_TRUNCATE = -1,
	BR_NOT_READY,
	BR_READY,
} BR_READY_FOR_POP;

// return 1 for yes this thing can be popped, 0, for no, -1 for overwrite
typedef int (*br_ready_for_pop)(const uint8_t*, size_t);

// the behavior for a ring to follow when the buffer is full
// BR_OVERWRITE_OLDEST will only overwrite the oldest line
// BR_OVERWRITE_NEWEST will only overwrite the last received line
// BR_OVERWRITE_REFUSAL will deny more data until something is popped
typedef enum
{
	BR_OVERWRITE_OLDEST = 0,
	BR_OVERWRITE_NEWEST,
	BR_OVERWRITE_REFUSAL,
}
BR_AUTO_OVERWRITE_BEHAVIOR;

// === housekeeping ===
// returns a dynamically allocated ring
//		with dynamically allocated memory for the backing store
byte_ring_t* br_create_full_alloc(size_t n_lines, size_t len_lines,
		BR_AUTO_OVERWRITE_BEHAVIOR behavior);

// returns a dynamically allocated ring
//		that uses a given backing store buffer
byte_ring_t* br_create_alloc_static_backing_store(size_t n_lines, size_t len_lines,
		BR_AUTO_OVERWRITE_BEHAVIOR behavior, uint8_t* backing_store);

// returns a ring s.t. one internal feature is allocated, the rest is not
int br_alloc_full_static(byte_ring_t* ring, size_t n_lines, size_t len_lines,
	BR_AUTO_OVERWRITE_BEHAVIOR behavior, uint8_t* backing_store);

// frees any memory that was allocated to a ring
void br_destroy_internals(byte_ring_t* br);
// frees all memory for a full alloc ring
void br_destroy(byte_ring_t** br);

// === observer ===
// dumps hex of the ring's backing store
#ifdef BR_STDIO_DEBUG
void br_dump_contents(byte_ring_t* ring);
// prints some information about this ring during runtime
void br_print_configuration(byte_ring_t* ring);
#endif

// get the length of the current read line
size_t br_peek_read_size(byte_ring_t* ring);
// get the data of the current read line
const uint8_t* br_peek_read_data(byte_ring_t* ring);
// get the length of the current write line
size_t br_peek_write_size(byte_ring_t* ring);
// get the data of the current write line
const uint8_t* br_peek_write_data(byte_ring_t* ring);
// gives f access (like br_peek) but with the stored length as well
int br_is_ready(byte_ring_t* ring, br_ready_for_pop f);

// === mutators ===
// write a new byte wrt behavior
bool br_push(byte_ring_t* ring, uint8_t byte);
// seek next line wrt behavior
bool br_seek(byte_ring_t* ring);
// invalidate all data in this ring
void br_clear(byte_ring_t* br);
// manually request to move the write head forward, returns true if it succeeded, false if it did not (overwriting when behavior says not to overwrite)
bool br_advance_write_head(byte_ring_t* ring);
// fills the remainder of the current write line with byte
bool br_cinch(byte_ring_t* ring, uint8_t byte);
bool br_cinch00(byte_ring_t* ring);
bool br_cinchFF(byte_ring_t* ring);

// f returns BR_TRUNCATE		=> br_seek and return -1
// f returns BR_NOTE_READY  => return 0
// f returns BR_READY				=> br_seek and returns number of bytes copied to dst
ssize_t br_pop(byte_ring_t* ring, uint8_t* dst, br_ready_for_pop f);

#endif
