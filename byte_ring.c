#include "byte_ring.h"
#include <string.h>
#include <stdio.h>

#ifdef BR_ASSERT_ACTIVE
#	include <assert.h>
#endif

#define BR_BACKING_STORE_ALLOC			(1 << 3)
#define BR_STRUCT_ALLOC					(1 << 4)
#define BR_SIZEMAP_ALLOC				(1 << 5)

#define BR_BEHAVIOR_FLAGS_MASK			(BR_OVERWRITE_OLDEST	|	BR_OVERWRITE_NEWEST	|	BR_OVERWRITE_REFUSAL)
#define BR_ALLOC_FLAGS_MASK				(BR_BACKING_STORE_ALLOC	|	BR_STRUCT_ALLOC		|	BR_SIZEMAP_ALLOC)
#define BR_IMMUTABLE_FLAGS_MASK			(BR_ALLOC_FLAGS_MASK	|	BR_BEHAVIOR_FLAGS_MASK)
#define BR_EVENT_FLAGS_MASK				(~BR_IMMUTABLE_FLAGS_MASK)

/*
// the ring automatically wrapped a line when a line became full
BR_FLAG_LINE_WRAPPED	=		(1 << 8),
*/

struct byte_ring
{
	uint32_t								bit_flags;
	bool (*push_function)(struct byte_ring*, uint8_t);
	size_t									number_lines;
	size_t									line_length;
	uint8_t*								backing_store;
	size_t									backing_store_size;
	size_t*									size_map;

	uint8_t*								write;
	uint8_t*								read;
};

inline static uint32_t _br_get_flags(byte_ring_t* ring)
{
	return (ring->bit_flags);
}

inline static void _br_set_flags(byte_ring_t* ring, uint32_t flags)
{
	ring->bit_flags = flags;
}

inline static void _br_add_flags(byte_ring_t* ring, uint32_t flags)
{
	ring->bit_flags |= flags;
}

inline static void _br_clear_flag(byte_ring_t* ring, uint32_t flag)
{
	uint32_t flags = _br_get_flags(ring) & ~(flag);
	_br_set_flags(ring, flags);
}

inline static void _br_clear_event_flags(byte_ring_t* ring)
{
	uint32_t flags = _br_get_flags(ring) & BR_IMMUTABLE_FLAGS_MASK;
	_br_set_flags(ring, flags);
}

inline static bool _br_flag_is_set(byte_ring_t* ring, BR_EVENT_FLAGS event_flag)
{
	return (0 != (_br_get_flags(ring) & event_flag));
}

inline static size_t br_get_backing_store_size(byte_ring_t* ring)
{
	return (ring->backing_store_size);
}

inline static uint8_t* br_get_first_line(byte_ring_t* ring)
{
	return (ring->backing_store);
}

inline static uint8_t* br_get_final_line(byte_ring_t* ring)
{
	// take the first line, go to the last byte, move back by a line's length
	return (br_get_first_line(ring) + br_get_backing_store_size(ring) - (ring->line_length));
}

inline static uint8_t* br_get_specific_line(byte_ring_t* ring, size_t index)
{
	return (br_get_first_line(ring) + (index * ring->line_length));
}

inline static size_t br_get_line_index(byte_ring_t* ring, uint8_t* head)
{
	return ((size_t) (head - br_get_first_line(ring)));
}

inline static uint8_t* br_get_next_line(byte_ring_t* ring, uint8_t* head)
{
	const uint8_t* final = br_get_final_line(ring);
	uint8_t* function_value = NULL;
	
	// if this line is at the end, go back to the beginning
	if(final == head) { function_value = br_get_first_line(ring); }
		
	// if this line is not at the end, move forward by a line length from where it currently is
	if(final != head) {	function_value = head + ring->line_length; }
	
	return (function_value);
}

inline static size_t br_get_size(byte_ring_t* ring, uint8_t* head)
{
	size_t index = br_get_line_index(ring, head);
	return ((ring->size_map)[index]);
}

inline static void br_set_size(byte_ring_t* ring, size_t index, size_t size)
{
	(ring->size_map)[index] = size;
}

inline static bool br_write_line_is_full(byte_ring_t* ring)
{
	bool function_value = false;
	if(ring->line_length <= br_get_size(ring, ring->write))
	{
		function_value = true;
		_br_add_flags(ring, BR_FLAG_LINE_WRAPPED);
	}
	
	return (function_value);
}

inline static bool br_head1_will_point_to_head2(byte_ring_t* ring, uint8_t* restrict head1, uint8_t* restrict head2)
{
	bool function_value = false;
	if(br_get_next_line(ring, head1) == head2) { function_value = true; }
	return (function_value);
}

// if this function returns true, the byte_ring is full
inline static bool br_write_will_point_to_read(byte_ring_t* ring)
{
	bool clobber = br_head1_will_point_to_head2(ring, ring->write, ring->read);
	if(true == clobber) { _br_add_flags(ring, BR_FLAG_RING_FULL); }
	return (clobber);
}

// if this function returns true, the byte_ring is empty
inline static bool br_read_will_point_to_write(byte_ring_t* ring)
{
	bool clobber = br_head1_will_point_to_head2(ring, ring->read, ring->write);
	if(true == clobber) { _br_add_flags(ring, BR_FLAG_RING_EMPTY); }
	return (clobber);
}

static void br_check_truths(byte_ring_t* ring)
{
	#ifdef BR_ASSERT_ACTIVE
	assert(NULL != ring);
	assert(NULL != ring->backing_store);
	assert(NULL != ring->size_map);
	assert(NULL != ring->write);
	assert(NULL != ring->read);
	assert(NULL != ring->push_function);
	assert(0		!= br_get_backing_store_size(ring));
	assert(0 == (ring->read == ring->write));
	#endif
	
	#ifndef BR_ASSERT_ACTIVE
	#endif
}

inline static void br_reset_write_head(byte_ring_t* ring)
{
	size_t index = br_get_line_index(ring, ring->write);
	br_set_size(ring, index, 0);
}

inline static void br_reset_read_head(byte_ring_t* ring)
{
	size_t index = br_get_line_index(ring, ring->read);
	br_set_size(ring, index, 0);
	
#	ifdef BR_SHRED_OLD_DATA
	// really unnecessary to do thrice, but that assumes this data structures goes into volatile memory
	// instead of someone's hacked up non-volatile memory mappings
	// but also dealing with swap files that might get thrown onto a disk temporarily
	memset(ring->read, 0xA5, ring->line_length * sizeof(uint8_t));
	memset(ring->read, 0x5A, ring->line_length * sizeof(uint8_t));
	memset(ring->read, 0, ring->line_length * sizeof(uint8_t));
#	endif
}

inline static void br_write_byte(byte_ring_t* ring, uint8_t byte)
{
	size_t size = br_get_size(ring, ring->write);
	size_t index = br_get_line_index(ring, ring->write);
	
	*(ring->write + size) = byte;
	
	br_set_size(ring, index, size + sizeof(byte));
}

inline static void br_move_read_line_forward(byte_ring_t* ring)
{
	br_reset_read_head(ring);
	ring->read = br_get_next_line(ring, ring->read);
	br_check_truths(ring);
}

inline static void br_move_write_line_forward(byte_ring_t* ring)
{
	// store the size of the written data into the size map
	// before moving to the next line
	size_t index = br_get_line_index(ring, ring->write);
	br_set_size(ring, index, br_peek_write_size(ring));

	ring->write = br_get_next_line(ring, ring->write);
	br_reset_write_head(ring);
	br_check_truths(ring);
}

inline static void br_increment_write(byte_ring_t* ring, bool force)
{
	bool clobber = br_read_will_point_to_write(ring);
	if(true == clobber && false == force) { goto function_exit; }
	if(true == clobber) { br_move_read_line_forward(ring); }
	br_move_write_line_forward(ring);
function_exit:
	br_check_truths(ring);
}

inline static void br_increment_read(byte_ring_t* ring, bool force)
{
	bool clobber = br_read_will_point_to_write(ring);
	if(true == clobber && false == force) { goto function_exit; }
	if(true == clobber) { br_move_write_line_forward(ring); }
	br_move_read_line_forward(ring);
function_exit:
	br_check_truths(ring);
}

// return true == push_success, always returns true
static bool br_push_overwrite_oldest(byte_ring_t* ring, uint8_t byte)
{
	bool clobber		= br_write_will_point_to_read(ring);
	bool full				=	br_write_line_is_full(ring);
	bool overwrite	= clobber && full;

	if(true == overwrite)
	{
		br_move_read_line_forward(ring);
		_br_add_flags(ring, BR_FLAG_OVERWRITE);
	}

	if(true == full)
	{
		br_move_write_line_forward(ring);
	}

	br_write_byte(ring, byte);
	br_check_truths(ring);
	return (true);
}

// return true == push_success, always returns true
static bool br_push_overwrite_newest(byte_ring_t* ring, uint8_t byte)
{
	bool clobber		= br_write_will_point_to_read(ring);
	bool full				=	br_write_line_is_full(ring);
	bool overwrite	= clobber && full;

	if(true == overwrite)
	{
		br_reset_write_head(ring);
		_br_add_flags(ring, BR_FLAG_OVERWRITE);
	}

	if((false == overwrite) && (true == full))
	{
		br_move_write_line_forward(ring);
	}

	br_write_byte(ring, byte);
	br_check_truths(ring);
	return (true);
}

// returns true == push_success, when ring was not full
// returns false == no mutation, when ring was full
static bool br_push_refuse_overwrite(byte_ring_t* ring, uint8_t byte)
{
	bool clobber				= br_write_will_point_to_read(ring);
	bool full						=	br_write_line_is_full(ring);
	bool overwrite			= clobber && full;

	if(true == overwrite)
	{
		goto fail_early;
	}
		
	if(true == full)
	{
		br_move_write_line_forward(ring);
	}
	
	br_write_byte(ring, byte);
fail_early:
	br_check_truths(ring);
	return (false == overwrite);
}

byte_ring_t* br_create_full_alloc(size_t n_lines, size_t len_lines,
		BR_BEHAVIOR_FLAGS behavior_flag)
{
	size_t size								= len_lines * n_lines;
	byte_ring_t* ring					= (byte_ring_t*) malloc(sizeof(byte_ring_t));
	if(NULL == ring) { goto fail_early; }

	uint8_t* backing_store		= (uint8_t*) malloc(size);
	if(NULL == backing_store)
	{
		free(ring);
		ring = NULL;
		goto fail_early;
	}

	size_t* size_map					= (size_t*) malloc(sizeof(size_t) * n_lines);
	if(NULL == size_map)
	{
		free(ring);
		free(backing_store);
		ring = NULL;
		goto fail_early;
	}

	ring->size_map						=	size_map;
	ring->backing_store				= backing_store;
	ring->backing_store_size	= size;
	ring->number_lines				= n_lines;
	ring->line_length					= len_lines;
	
	_br_set_flags(ring, 0);
	_br_add_flags(ring, BR_BACKING_STORE_ALLOC | BR_STRUCT_ALLOC | BR_SIZEMAP_ALLOC | behavior_flag);

	switch(behavior_flag)
	{
		case BR_OVERWRITE_OLDEST:
			ring->push_function = br_push_overwrite_oldest;
			break;

		case BR_OVERWRITE_NEWEST:
			ring->push_function = br_push_overwrite_newest;
			break;

		case BR_OVERWRITE_REFUSAL:
			ring->push_function = br_push_refuse_overwrite;
			break;

		default:
			break;
	}

	br_clear(ring);
fail_early:
	return ring;
}

byte_ring_t* br_create_alloc_static_backing_store(size_t n_lines,
		size_t len_lines, BR_BEHAVIOR_FLAGS behavior_flag,
		uint8_t* backing_store)
{
	byte_ring_t* ring					= (byte_ring_t*) malloc(sizeof(byte_ring_t));
	if(NULL == ring) { goto fail_early; }

	size_t* size_map					= (size_t*) malloc(sizeof(size_t) * n_lines);
	if(NULL == size_map)
	{
		free(ring);
		ring = NULL;
		goto fail_early;
	}

	ring->size_map						=	size_map;
	ring->backing_store				= backing_store;
	ring->backing_store_size	= n_lines * len_lines;
	ring->number_lines				= n_lines;
	ring->line_length					= len_lines;
	
	_br_set_flags(ring, 0);
	_br_add_flags(ring, BR_STRUCT_ALLOC | BR_SIZEMAP_ALLOC | behavior_flag);

	switch(behavior_flag)
	{
		case BR_OVERWRITE_OLDEST:
			ring->push_function = br_push_overwrite_oldest;
			break;

		case BR_OVERWRITE_NEWEST:
			ring->push_function = br_push_overwrite_newest;
			break;

		case BR_OVERWRITE_REFUSAL:
			ring->push_function = br_push_refuse_overwrite;
			break;

		default:
			break;
	}

	br_clear(ring);
fail_early:
	return ring;
}

int br_alloc_full_static(byte_ring_t* ring, size_t n_lines, size_t len_lines,
		BR_BEHAVIOR_FLAGS behavior_flag, uint8_t* backing_store)
{
	int function_value				=	-1;
	ring->backing_store				= backing_store;
	ring->backing_store_size	= n_lines * len_lines;
	ring->number_lines				= n_lines;
	ring->line_length					= len_lines;
	
	_br_set_flags(ring, 0);
	_br_add_flags(ring, BR_SIZEMAP_ALLOC | behavior_flag);

	size_t* size_map					= (size_t*) malloc(sizeof(size_t) * n_lines);
	if(NULL == size_map)
	{
		goto function_exit;
	}

	ring->size_map						=	size_map;
	switch(behavior_flag)
	{
		case BR_OVERWRITE_OLDEST:
			ring->push_function = br_push_overwrite_oldest;
			break;

		case BR_OVERWRITE_NEWEST:
			ring->push_function = br_push_overwrite_newest;
			break;

		case BR_OVERWRITE_REFUSAL:
			ring->push_function = br_push_refuse_overwrite;
			break;

		default:
			break;
	}

	br_clear(ring);
	function_value = 0;
function_exit:
	return function_value;
}

void br_destroy_internals(byte_ring_t* ring)
{
	uint32_t alloc_map = _br_get_flags(ring) & BR_ALLOC_FLAGS_MASK;
	uint8_t* backing_store = br_get_first_line(ring);

	if(BR_BACKING_STORE_ALLOC & alloc_map)
	{
		free(backing_store);
		ring->backing_store = NULL;
	}

	if(BR_SIZEMAP_ALLOC & alloc_map)
	{
		free(ring->size_map);
		ring->size_map = NULL;
	}
}

void br_destroy(byte_ring_t** ring)
{
	br_destroy_internals(*ring);

	uint32_t alloc_map = _br_get_flags(*ring) & BR_ALLOC_FLAGS_MASK;
	if(BR_STRUCT_ALLOC & alloc_map) { free(*ring); *ring = NULL; }
}

#ifdef BR_STDIO_DEBUG
void br_dump_contents(byte_ring_t* ring)
{
	uint8_t* line = br_get_first_line(ring);

	while(line <= br_get_final_line(ring))
	{
		for(size_t i = 0; i < ring->line_length; i++)
		{
			printf("%.2X", line[i]);
		}
		puts("");

		if(br_get_final_line(ring) == line)
		{
			break;
		}

		line = br_get_next_line(ring, line);
	}
}

void br_print_configuration(byte_ring_t* ring)
{
	size_t bs_size = br_get_backing_store_size(ring);
	printf("number lines: %zu\n", ring->number_lines);
	printf("line length: %zu\n", ring->line_length);
	printf("backing store size: %zu\n", bs_size);
	printf("allocation map: %u\n", ring->alloc_map);
	
	printf("overwrite mode: ");
	switch(ring->behavior)
	{
		case BR_OVERWRITE_REFUSAL:
		printf("refused\n");
		break;
		case BR_OVERWRITE_NEWEST:
		printf("newest\n");
		break;
		case BR_OVERWRITE_OLDEST:
		printf("oldest\n");
		break;
	}

	puts("");

	size_t counter = 0;
	uint8_t* line = br_get_first_line(ring);
	size_t size = br_get_size(ring, line);
	do
	{
		printf("line: %zu @ %p, size: %zu\n", counter, line, size);
		++ counter;
		line = br_get_next_line(ring, line);
		size = br_get_size(ring, line);
	}
	while(counter < ring->number_lines);
}
#endif

size_t br_peek_read_size(byte_ring_t* ring)
{
	return br_get_size(ring, ring->read);
}

inline const uint8_t* br_peek_read_data(byte_ring_t* ring)
{
	const uint8_t* peek = ring->read;
	return peek;
}

inline size_t br_peek_write_size(byte_ring_t* ring)
{
	return br_get_size(ring, ring->write);
}

inline const uint8_t* br_peek_write_data(byte_ring_t* ring)
{
	const uint8_t* peek = ring->write;
	return peek;
}

int br_is_ready(byte_ring_t* ring, br_ready_for_pop f)
{
	return f(br_peek_read_data(ring), br_get_size(ring, ring->read));
}

bool br_push(byte_ring_t* ring, uint8_t byte)
{
	return (ring->push_function(ring, byte));
}

//bool br_cinch(byte_ring_t* ring, uint8_t byte)
//{
//	size_t size = br_get_size(ring, ring->write);
//	//size_t index = br_get_size_map_index(ring, ring->write);
//	
//	if((ring->line_length - size - sizeof(byte) > ring->line_length)
//		|| (ring->line_length - size - sizeof(byte) < 0))
//	{
//		goto function_exit;
//	}
//	
//	// block overwrite with byte for however many is left, without the last one
//	memset(ring->write + size, (int) byte, (ring->line_length - size - sizeof(byte)) * sizeof(byte));
//	
//	// don't adjust the counter, since this is dummy data
//	// leave a space for the final push
//	//br_set_size(ring, index, ring->line_length - 1);
//	
//	// then try to increment head according to behavior with the final byte being pushed
//function_exit:
//	return ring->push_function(ring, byte);
//}
//
//bool br_cinch00(byte_ring_t* ring)
//{
//	return br_cinch(ring, 0x00);
//}
//
//bool br_cinchFF(byte_ring_t* ring)
//{
//	return br_cinch(ring, 0xFF);
//}

bool br_advance_write_head(byte_ring_t* ring)
{
	bool clobber			= br_write_will_point_to_read(ring);
	bool full				= br_write_line_is_full(ring);
	bool overwrite			= clobber && full;
	bool function_value		= false;
	
	if(false == overwrite)
	{
		br_move_write_line_forward(ring);
		function_value = true;
	}
		
	if(true == overwrite)
	{
		switch(_br_get_flags(ring) & BR_BEHAVIOR_FLAGS_MASK)
		{
				case BR_OVERWRITE_OLDEST:
					{
						br_move_read_line_forward(ring);
						br_move_write_line_forward(ring);
						_br_add_flags(ring, BR_FLAG_OVERWRITE);
						function_value = true;
					}
				break;
				
				case BR_OVERWRITE_NEWEST:
					{
						br_reset_write_head(ring);
						_br_add_flags(ring, BR_FLAG_OVERWRITE);
						function_value = true;
					}
				break;
				
				case BR_OVERWRITE_REFUSAL:
					{
						function_value = false;
					}
				break;
		}
	}	
	
	_br_add_flags(ring, BR_FLAG_DATA_READY);
	return (function_value);
}

bool br_seek(byte_ring_t* ring)
{
	bool function_value = false;
	br_reset_read_head(ring);
	bool clobber = br_read_will_point_to_write(ring);
	
	if(false == clobber)
	{
		br_move_read_line_forward(ring);
		function_value = true;
	}
	
	return (function_value);
}

void br_clear(byte_ring_t* ring)
{
	size_t size = br_get_backing_store_size(ring);
	uint8_t* backing_store = br_get_first_line(ring);
	memset(backing_store, 0, size * sizeof(uint8_t));
	memset(ring->size_map, 0, sizeof(size_t) * (ring->number_lines));

	ring->read	= br_get_final_line(ring);
	ring->write	= br_get_first_line(ring);
	br_reset_read_head(ring);
	br_reset_write_head(ring);

	_br_clear_event_flags(ring);
	br_check_truths(ring);
}

void br_set_flag(byte_ring_t* ring, BR_EVENT_FLAGS event_flag)
{
	uint32_t new_flags = _br_get_flags(ring) | (event_flag & BR_EVENT_FLAGS_MASK);
	_br_add_flags(ring, new_flags);
}

void br_clear_flag(byte_ring_t* ring, BR_EVENT_FLAGS event_flag)
{
	_br_clear_flag(ring, event_flag & BR_EVENT_FLAGS_MASK);
}

bool br_flag_is_set(byte_ring_t* ring, BR_EVENT_FLAGS event_flag)
{
	uint32_t flags = _br_get_flags(ring);
	return (0 != (event_flag & flags));
}

ssize_t br_pop(byte_ring_t* ring, uint8_t* dst, br_ready_for_pop f)
{
	size_t size = br_get_size(ring, ring->read);
	ssize_t function_value = 0;
	int action = f(br_peek_read_data(ring), size);
	
	if(BR_NOT_READY == action)
	{
		goto function_exit;
	}

	if(BR_TRUNCATE == action)
	{
		br_seek(ring);
		function_value = -1;
		goto function_exit;
	}

	if(BR_READY == action)
	{
		memcpy(dst, ring->read, size);
		function_value = size;
		br_seek(ring);
		goto function_exit;
	}

function_exit:
	return function_value;
}
