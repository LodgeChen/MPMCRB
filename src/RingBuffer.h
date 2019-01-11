#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct ring_buffer ring_buffer_t;

typedef struct ring_buffer_token
{
	const size_t	len;		/** length */
	uint8_t			data[];		/** data */
}ring_buffer_token_t;

typedef enum ring_buffer_flag
{
	ring_buffer_flag_overwrite			= 0x01 << 0x00,	/** overwrite exist data if no empty room. Default action is drop */
	ring_buffer_flag_discard			= 0x01 << 0x01,	/** discard operation */
	ring_buffer_flag_consume_on_error	= 0x01 << 0x02,	/** if user want to discard a consuming token but failed, force consume this token */
}ring_buffer_flag_t;

/**
* initialize a ring buffer on the buffer
* @param buffer		trunk of memory
* @param size		memory size
* @return			on success, return the handle of ring buffer. otherwise return NULL.
*/
ring_buffer_t* ring_buffer_init(void* buffer, size_t size);

/**
* exit ring buffer
* @param rb		ring buffer
* @return		0 on success, otherwise failed
*/
int ring_buffer_exit(ring_buffer_t* rb);

/**
* request a token to write.
* @param rb		ring buffer
* @param len	the data length you want to write
* @param flags	control flags. can be: `ring_buffer_flag_overwrite`
* @return		A token which can be write to. After write finish, you need to commit it ether as success or discard.
*/
ring_buffer_token_t* ring_buffer_reserve(ring_buffer_t* rb, size_t len, int flags);

/**
* request a token to consume.
* @param rb		ring buffer
* @return		A token which can be consume. After consume finish, you need to commit it ether as success or discard.
*/
ring_buffer_token_t* ring_buffer_consume(ring_buffer_t* rb);

/**
* commit a token as operation success or discard.
* @param rb		ring buffer
* @param token	a token to be commit
* @param flags	control flags. can be: `ring_buffer_flag_discard` or `ring_buffer_flag_consume_on_error`
*/
int ring_buffer_commit(ring_buffer_t* rb, ring_buffer_token_t* token, int flags);

/**
* the internal heap size for the ring buffer
* @return		size of heap
*/
size_t ring_buffer_heap_cost(void);

/**
* calculate the actual space the data will taken.
* @param len	length of data
* @return		the actual space will taken
*/
size_t ring_buffer_node_cost(size_t len);

#ifdef __cplusplus
}
#endif
#endif
