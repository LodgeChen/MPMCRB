#include "RingBuffer.h"

#define ALIGN_SIZE(size, align)	(((uintptr_t)(size) + ((uintptr_t)(align) - 1)) & ~((uintptr_t)(align) - 1))
#define ALIGN_PTR(ptr, align)	(void*)(ALIGN_SIZE(ptr, align))
#define CONTAINER_FOR(ptr, TYPE, member)	((TYPE*)((uint8_t*)(ptr) - (size_t)&((TYPE*)0)->member))

typedef enum ring_buffer_node_state
{
	writing,
	committed,
	reading,
}ring_buffer_node_state_t;

typedef struct ring_buffer_node
{
	struct ring_buffer_node_chain_pos
	{
		struct ring_buffer_node* p_forward;		/** next position */
		struct ring_buffer_node* p_backward;	/** previous position */
	}chain_pos;

	struct ring_buffer_node_chain_time
	{
		struct ring_buffer_node* p_newer;		/** newer node */
		struct ring_buffer_node* p_older;		/** older node */
	}chain_time;

	ring_buffer_node_state_t	state;			/** node state */
	ring_buffer_token_t			token;			/** user data */
}ring_buffer_node_t;

struct ring_buffer
{
	struct ring_buffer_cfg
	{
		uint8_t*			cache;				/** start of usable address */
		size_t				capacity;			/** length of usable address */
	}cfg;

	struct ring_buffer_counter
	{
		size_t				lost;				/** the number of lost elements form last consume */
	}counter;

	ring_buffer_node_t*		HEAD;				/** point to newest reading/writing/committed node */
	ring_buffer_node_t*		TAIL;				/** point to oldest reading/writing/committed node */
	ring_buffer_node_t*		oldest_reserve;		/** point to oldest writing/committed node */
};

/**
* calculate how many space a data actually cost
* @param len	data length
* @return		actual space
*/
inline static size_t _ring_buffer_node_cost(size_t len)
{
	return ALIGN_SIZE(sizeof(ring_buffer_node_t) + len, sizeof(void*));
}

inline static void _ring_buffer_reinit(ring_buffer_t* rb)
{
	rb->oldest_reserve = NULL;
	rb->HEAD = NULL;
	rb->TAIL = NULL;
}

/**
* create first node in ring buffer
* @param rb			ring buffer
* @param data_len	length of user data
* @param node_size	size of node
* @return			token
*/
inline static ring_buffer_token_t* _ring_buffer_reserve_empty(ring_buffer_t* rb, size_t data_len, size_t node_size)
{
	/* check capacity */
	if (node_size > rb->cfg.capacity)
	{
		return NULL;
	}

	/* initialize node */
	rb->HEAD = (ring_buffer_node_t*)rb->cfg.cache;
	rb->HEAD->state = writing;
	*(size_t*)&rb->HEAD->token.len = data_len;

	/* update chain_pos */
	rb->HEAD->chain_pos.p_forward = rb->HEAD;
	rb->HEAD->chain_pos.p_backward = rb->HEAD;

	/* update chain_time */
	rb->HEAD->chain_time.p_newer = NULL;
	rb->HEAD->chain_time.p_older = NULL;

	/* initialize other field */
	rb->TAIL = rb->HEAD;
	rb->oldest_reserve = rb->HEAD;

	return &rb->oldest_reserve->token;
}

inline static void _ring_buffer_update_time_for_new_node(ring_buffer_t* rb, ring_buffer_node_t* new_node)
{
	/* update chain_time */
	new_node->chain_time.p_newer = NULL;
	new_node->chain_time.p_older = rb->HEAD;
	new_node->chain_time.p_older->chain_time.p_newer = new_node;

	/* update HEAD */
	rb->HEAD = new_node;
}

/**
* perform overwrite
*/
inline static ring_buffer_token_t* _ring_buffer_reserve_overwrite(ring_buffer_t* rb, size_t data_len, size_t node_size)
{
	/* overwrite only perform on committed nodes */
	if (rb->oldest_reserve == NULL || rb->oldest_reserve->state != committed)
	{
		return NULL;
	}

	/* if there is only one node in ring buffer, then check if the whole buffer can hold the new node */
	if (rb->oldest_reserve->chain_pos.p_forward == rb->oldest_reserve && rb->oldest_reserve->chain_pos.p_backward == rb->oldest_reserve)
	{
		if (rb->cfg.capacity >= node_size)
		{
			rb->counter.lost++;
			_ring_buffer_reinit(rb);
			return _ring_buffer_reserve_empty(rb, data_len, node_size);
		}
		return NULL;
	}

	/* we need to calculate if continuous committed node is large enough to hold new data */
	size_t sum_size = 0;
	size_t lost_node = 1;
	ring_buffer_node_t* node_start = rb->oldest_reserve;
	ring_buffer_node_t* node_end = node_start;

	while (1)
	{
		sum_size += _ring_buffer_node_cost(node_end->token.len);
		if (!(
			sum_size < node_size	/* overwrite minimum nodes */
			&& node_end->chain_pos.p_forward->state == committed	/* only overwrite committed node */
			&& node_end->chain_pos.p_forward == node_end->chain_time.p_newer	/* node must both physical and time continuous */
			&& node_end->chain_pos.p_forward > node_end	/* cannot interrupt by array boundary */
			))
		{
			break;
		}
		node_end = node_end->chain_pos.p_forward;
		lost_node++;
	}

	/* if requirement cannot meet, then overwrite failed */
	if (sum_size < node_size)
	{
		return NULL;
	}

	/*
	* here [node_start, node_end] will be overwrite,
	* oldest_reserve need to move forward.
	*/
	rb->oldest_reserve = node_end->chain_time.p_newer;

	/* update chain_pos */
	node_start->chain_pos.p_forward = node_end->chain_pos.p_forward;
	node_end->chain_pos.p_forward->chain_pos.p_backward = node_start;

	/* update chain_time */
	if (node_start->chain_time.p_older != NULL)
	{
		node_start->chain_time.p_older->chain_time.p_newer = node_end->chain_time.p_newer;
	}
	node_end->chain_time.p_newer->chain_time.p_older = node_start->chain_time.p_older;

	_ring_buffer_update_time_for_new_node(rb, node_start);
	rb->counter.lost += lost_node;

	/* update length */
	*(size_t*)&node_start->token.len = data_len;

	return &node_start->token;
}

inline static void _ring_buffer_insert_new_node(ring_buffer_t* rb, ring_buffer_node_t* new_node, size_t data_len)
{
	/* initialize token */
	new_node->state = writing;
	*(size_t*)&new_node->token.len = data_len;

	/* update chain_pos */
	new_node->chain_pos.p_forward = rb->HEAD->chain_pos.p_forward;
	new_node->chain_pos.p_backward = rb->HEAD;
	new_node->chain_pos.p_forward->chain_pos.p_backward = new_node;
	new_node->chain_pos.p_backward->chain_pos.p_forward = new_node;

	_ring_buffer_update_time_for_new_node(rb, new_node);
}

inline static ring_buffer_token_t* _ring_buffer_reserve_none_empty(ring_buffer_t* rb, size_t data_len, size_t node_size, int flags)
{
	/* calculate possible node position on the right */
	ring_buffer_node_t* next_possible_node = (ring_buffer_node_t*)((uint8_t*)rb->HEAD + _ring_buffer_node_cost(rb->HEAD->token.len));

	/* if there exists node on the right, then try to make token */
	if (rb->HEAD->chain_pos.p_forward > rb->HEAD)
	{
		if ((size_t)((uint8_t*)rb->HEAD->chain_pos.p_forward - (uint8_t*)next_possible_node) >= node_size)
		{
			_ring_buffer_insert_new_node(rb, next_possible_node, data_len);
			return &next_possible_node->token;
		}

		return (flags & ring_buffer_flag_overwrite) ?
			_ring_buffer_reserve_overwrite(rb, data_len, node_size) : NULL;
	}

	/* if higher area has enough space, make token */
	if ((rb->cfg.capacity - ((uint8_t*)next_possible_node - rb->cfg.cache)) >= node_size)
	{
		_ring_buffer_insert_new_node(rb, next_possible_node, data_len);

		return &next_possible_node->token;
	}

	/* if area on the most left cache is enough, make token */
	if ((size_t)((uint8_t*)rb->HEAD->chain_pos.p_forward - rb->cfg.cache) >= node_size)
	{
		next_possible_node = (ring_buffer_node_t*)rb->cfg.cache;
		_ring_buffer_insert_new_node(rb, next_possible_node, data_len);

		return &next_possible_node->token;
	}

	/* in other condition, overwrite if needed */
	return (flags & ring_buffer_flag_overwrite) ?
		_ring_buffer_reserve_overwrite(rb, data_len, node_size) : NULL;
}

inline static int _ring_buffer_commit_for_write_confirm(ring_buffer_t* rb, ring_buffer_node_t* node)
{
	node->state = committed;
	return 0;
}

inline static void _ring_buffer_remove_node_chain_pos(ring_buffer_node_t* node)
{
	node->chain_pos.p_backward->chain_pos.p_forward = node->chain_pos.p_forward;
	node->chain_pos.p_forward->chain_pos.p_backward = node->chain_pos.p_backward;
}

inline static void _ring_buffer_remove_tail(ring_buffer_t* rb)
{
	/* update chain_pos */
	_ring_buffer_remove_node_chain_pos(rb->TAIL);

	/* update chain_time */
	rb->TAIL->chain_time.p_newer->chain_time.p_older = NULL;

	ring_buffer_node_t* next_node = rb->TAIL->chain_time.p_newer;
	if (rb->oldest_reserve == rb->TAIL)
	{
		rb->oldest_reserve = next_node;
	}
	rb->TAIL = next_node;
}

inline static void _ring_buffer_remove_head(ring_buffer_t* rb)
{
	/* update chain_pos */
	_ring_buffer_remove_node_chain_pos(rb->HEAD);

	/* update chain_time */
	rb->HEAD->chain_time.p_older->chain_time.p_newer = NULL;
	if (rb->oldest_reserve == rb->HEAD)
	{
		rb->oldest_reserve = NULL;
	}
	rb->HEAD = rb->HEAD->chain_time.p_older;
}

/**
* completely remove a node from ring buffer
* @param rb	ring buffer
* @param node	node to be delete
*/
inline static void _ring_buffer_delete_node(ring_buffer_t* rb, ring_buffer_node_t* node)
{
	/* only node in ring buffer */
	if (node->chain_pos.p_backward == node && node->chain_pos.p_forward == node)
	{
		_ring_buffer_reinit(rb);
		return ;
	}

	/*
	* node is the oldest node.
	* @warning
	* use node->chain_time.p_older to avoid memory access,
	* beacuse if node->chain_time.p_older == NULL, then TAIL == node
	*/
	if (node->chain_time.p_older == NULL)
	{
		_ring_buffer_remove_tail(rb);
		return ;
	}

	/*
	* node is the newest node
	* @warning
	* use node->chain_time.p_newer to avoid memory access,
	* beacuse if node->chain_time.p_newer == NULL, then HEAD == node
	*/
	if (node->chain_time.p_newer == NULL)
	{
		_ring_buffer_remove_head(rb);
		return ;
	}

	_ring_buffer_remove_node_chain_pos(node);
	/* in other condition, just take care about `oldest_reserve` */
	node->chain_time.p_older->chain_time.p_newer = node->chain_time.p_newer;
	node->chain_time.p_newer->chain_time.p_older = node->chain_time.p_older;
	if (rb->oldest_reserve == node)
	{
		rb->oldest_reserve = node->chain_time.p_newer;
	}

	return ;
}

inline static int _ring_buffer_commit_for_write_discard(ring_buffer_t* rb, ring_buffer_node_t* node)
{
	_ring_buffer_delete_node(rb, node);
	return 0;
}

inline static int _ring_buffer_commit_for_write(ring_buffer_t* rb, ring_buffer_node_t* node, int flags)
{
	return (flags & ring_buffer_flag_discard) ?
		_ring_buffer_commit_for_write_discard(rb, node) :
		_ring_buffer_commit_for_write_confirm(rb, node);
}

inline static int _ring_buffer_commit_for_consume_confirm(ring_buffer_t* rb, ring_buffer_node_t* node)
{
	_ring_buffer_delete_node(rb, node);
	return 0;
}

/**
* discard a consumed token.
* the only condition a consumed token can be discard is no one consume newer token
*/
inline static int _ring_buffer_commit_for_consume_discard(ring_buffer_t* rb, ring_buffer_node_t* node, int flags)
{
	/* if exist a newer consumer, should fail */
	if (node->chain_time.p_newer != NULL && node->chain_time.p_newer->state == reading)
	{
		return (flags & ring_buffer_flag_consume_on_error) ?
			_ring_buffer_commit_for_consume_confirm(rb, node) : -1;
	}

	/* modify state */
	node->state = committed;

	/* if no newer node, then oldest_reserve should point to this node */
	if (node->chain_time.p_newer == NULL)
	{
		rb->oldest_reserve = node;
		return 0;
	}

	/* if node is just older than oldest_reserve, then oldest_reserve should move back */
	if (rb->oldest_reserve != NULL && rb->oldest_reserve->chain_time.p_older == node)
	{
		rb->oldeest_reserve = node;
		return 0;
	}
	
	return 0;
}

inline static int _ring_buffer_commit_for_consume(ring_buffer_t* rb, ring_buffer_node_t* node, int flags)
{
	return (flags & ring_buffer_flag_discard) ?
		_ring_buffer_commit_for_consume_discard(rb, node, flags) :
		_ring_buffer_commit_for_consume_confirm(rb, node);
}

size_t ring_buffer_heap_cost(void)
{
	/* need to align with machine size */
	return ALIGN_SIZE(sizeof(struct ring_buffer), sizeof(void*));
}

size_t ring_buffer_node_cost(size_t len)
{
	return _ring_buffer_node_cost(len);
}

ring_buffer_t* ring_buffer_init(void* buffer, size_t size)
{
	ring_buffer_t* rb = ALIGN_PTR(buffer, sizeof(void*));

	/* ring buffer must start from aligned address */
	const size_t leading_align_size = (uint8_t*)rb - (uint8_t*)buffer;

	/* check if buffer has enough space */
	if (leading_align_size + ring_buffer_heap_cost() >= size)
	{/* out of range */
		return NULL;
	}

	/* setup necessary field */
	rb->cfg.cache = (uint8_t*)rb + ring_buffer_heap_cost();
	rb->cfg.capacity = size - leading_align_size - ring_buffer_heap_cost();
	rb->counter.lost = 0;

	/* initialize */
	_ring_buffer_reinit(rb);

	return rb;
}

int ring_buffer_exit(ring_buffer_t* rb)
{
	(void)rb;
	// do nothing
	return 0;
}

ring_buffer_token_t* ring_buffer_reserve(ring_buffer_t* rb, size_t len, int flags)
{
	/* node must aligned */
	const size_t node_size = _ring_buffer_node_cost(len);

	/* empty ring buffer */
	if (rb->TAIL == NULL)
	{
		return _ring_buffer_reserve_empty(rb, len, node_size);
	}

	/* non empty ring buffer */
	return _ring_buffer_reserve_none_empty(rb, len, node_size, flags);
}

ring_buffer_token_t* ring_buffer_consume(ring_buffer_t* rb, size_t* lost)
{
	if (rb->oldest_reserve == NULL || rb->oldest_reserve->state != committed)
	{
		return NULL;
	}

	if(lost != NULL)
	{
		*lost = rb->counter.lost;
	}
	rb->counter.lost = 0;

	ring_buffer_node_t* token_node = rb->oldest_reserve;
	rb->oldest_reserve = rb->oldest_reserve->chain_time.p_newer;

	token_node->state = reading;
	return &token_node->token;
}

int ring_buffer_commit(ring_buffer_t* rb, ring_buffer_token_t* token, int flags)
{
	ring_buffer_node_t* node = CONTAINER_FOR(token, ring_buffer_node_t, token);

	return node->state == writing ?
		_ring_buffer_commit_for_write(rb, node, flags) :
		_ring_buffer_commit_for_consume(rb, node, flags);
}

int ring_buffer_foreach(ring_buffer_t* rb,
	int (*cb)(ring_buffer_token_t* token, int state, void* arg), void* arg)
{
	int counter = 0;
	ring_buffer_node_t* node = rb->TAIL;
	for (; node != NULL; counter++)
	{
		if (cb(&node->token, node->state, arg) < 0)
		{
			break;
		}

		node = node->chain_time.p_newer;
	}

	return counter;
}
