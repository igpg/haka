
#include <haka/tcp-stream.h>
#include <haka/tcp.h>
#include <haka/error.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>


/*
 * Stream interface declaration
 */

static bool tcp_stream_destroy(struct stream *s);
static size_t tcp_stream_read(struct stream *s, uint8 *data, size_t length);
static size_t tcp_stream_available(struct stream *s);
static size_t tcp_stream_insert(struct stream *s, const uint8 *data, size_t length);
static size_t tcp_stream_replace(struct stream *s, const uint8 *data, size_t length);
static size_t tcp_stream_erase(struct stream *s, size_t length);
static bool tcp_stream_mark(struct stream *s);
static bool tcp_stream_unmark(struct stream *s);
static bool tcp_stream_rewind(struct stream *s);

struct stream_ftable tcp_stream_ftable = {
	destroy: tcp_stream_destroy,
	read: tcp_stream_read,
	available: tcp_stream_available,
	insert: tcp_stream_insert,
	replace: tcp_stream_replace,
	erase: tcp_stream_erase,
	mark: tcp_stream_mark,
	unmark: tcp_stream_unmark,
	rewind: tcp_stream_rewind
};


/*
 * Stream structures
 */

enum tcp_modif_type {
	TCP_MODIF_INSERT,
	TCP_MODIF_ERASE
};

struct tcp_stream_chunk_modif {
	enum tcp_modif_type             type;
	size_t                          position;
	size_t                          length;
	struct tcp_stream_chunk_modif  *prev;
	struct tcp_stream_chunk_modif  *next;
	uint8                           data[0];
};

struct tcp_stream_chunk {
	struct tcp                     *tcp;
	size_t                          start_seq;
	size_t                          end_seq;
	int                             offset_seq;
	struct tcp_stream_chunk_modif  *modifs;
	struct tcp_stream_chunk        *next;
};

struct tcp_stream_position {
	size_t                          chunk_seq;         /* chunk position seq (without modifs) */
	size_t                          chunk_seq_modif;   /* chunk position seq (with modifs) */
	size_t                          current_seq_modif; /* current position seq (with modifs) */
	struct tcp_stream_chunk        *chunk;             /* chunk at the current stream position */
	size_t                          chunk_offset;      /* current offset in the current chunk (without modifs) */
	struct tcp_stream_chunk_modif  *modif;             /* current or previous modif */
	size_t                          modif_offset;      /* position in the modif or -1 */
};

struct tcp_stream {
	struct stream                   stream;
	bool                            seq_initialized;
	size_t                          start_seq;

	struct tcp_stream_chunk        *first;   /* first packet in the stream */
	int                             first_offset_seq;
	struct tcp_stream_chunk        *last;    /* last inserted packet */
	struct tcp_stream_chunk        *sent;    /* oldest sent packet not acked */
	struct tcp_stream_chunk        *last_sent;
	int                             sent_offset_seq;

	struct tcp_stream_position      current_position;
	struct tcp_stream_position      mark_position;
	struct tcp_stream_chunk_modif  *pending_modif;
};


/*
 * Stream position functions
 */

bool tcp_stream_position_isvalid(struct tcp_stream_position *pos)
{
	return (pos->current_seq_modif != (size_t)-1);
}

void tcp_stream_position_invalidate(struct tcp_stream_position *pos)
{
	pos->current_seq_modif = (size_t)-1;
	pos->chunk = NULL;
	pos->modif = NULL;
}

static struct tcp_stream_chunk_modif *tcp_stream_position_modif(struct tcp_stream_position *pos,
		size_t *modif_offset, struct tcp_stream_chunk_modif **prev, struct tcp_stream_chunk_modif **next)
{
	if (pos->modif && pos->modif->position == pos->chunk_offset) {
		if ((pos->modif->type == TCP_MODIF_INSERT &&
			pos->modif_offset >= pos->modif->length) ||
			(pos->modif->type == TCP_MODIF_ERASE &&
			pos->modif_offset != 0)) {
			if (prev) *prev = pos->modif;
			if (next) *next = pos->modif->next;
			return NULL;
		}
		else {
			*modif_offset = pos->modif_offset;
			if (prev) *prev = pos->modif->prev;
			if (next) *next = pos->modif->next;
			return pos->modif;
		}
	}
	else {
		if (pos->modif) {
			if (prev) *prev = pos->modif;
			if (next) *next = pos->modif->next;
		}
		else {
			if (prev) *prev = NULL;
			if (next) *next = pos->chunk ? pos->chunk->modifs : NULL;
		}
		return NULL;
	}
}

static void tcp_stream_position_update_modif(struct tcp_stream_position *pos)
{
	struct tcp_stream_chunk_modif *next_modif = NULL;

	if (pos->modif) {
		if (pos->chunk_offset == pos->modif->position) {
			if (pos->modif->type == TCP_MODIF_INSERT &&
				pos->modif_offset >= pos->modif->length) {
				next_modif = pos->modif->next;
			}
			else if (pos->modif->type == TCP_MODIF_ERASE &&
				pos->modif_offset != 0) {
				next_modif = pos->modif->next;
			}
		}
		else {
			next_modif = pos->modif->next;
		}
	}
	else {
		next_modif = pos->chunk ? pos->chunk->modifs : NULL;
	}

	assert(!next_modif || next_modif->position >= pos->chunk_offset);

	if (next_modif && next_modif->position == pos->chunk_offset) {
		pos->modif = next_modif;
		pos->modif_offset = 0;
	}
}

static bool tcp_stream_position_chunk_at_end(struct tcp_stream_position *pos)
{
	struct tcp_stream_chunk_modif *modif;

	if (pos->chunk->start_seq + pos->chunk_offset != pos->chunk->end_seq) {
		return false;
	}

	modif = pos->modif;
	if (!modif) modif = pos->chunk->modifs;

	if (!modif) {
		return true;
	}
	else if (modif->next) {
		assert(modif->next->position >= pos->chunk_offset);
		return false;
	}
	else {
		assert(modif->position <= pos->chunk_offset);
		return modif->position != pos->chunk_offset ||
				pos->modif_offset >= modif->length;
	}
}

static bool tcp_stream_position_next_chunk(struct tcp_stream *tcp_s,
		struct tcp_stream_position *pos)
{
	assert(pos->chunk);

	if (!pos->chunk->next || pos->chunk->next->start_seq == pos->chunk->end_seq) {
		pos->chunk = pos->chunk->next;
		pos->chunk_seq += pos->chunk_offset;
		assert(!pos->chunk || pos->chunk->start_seq == pos->chunk_seq);
		pos->chunk_offset = 0;
		pos->modif = NULL;
		pos->modif_offset = (size_t)-1;
		assert(!tcp_s->pending_modif);
		return true;
	}
	else {
		return false;
	}
}

static bool tcp_stream_position_chunk_is_before(struct tcp_stream_position *pos,
		struct tcp_stream_chunk *chunk)
{
	return (pos->chunk != chunk) && (pos->chunk_seq + pos->chunk_offset >= chunk->end_seq);
}

static bool tcp_stream_position_advance(struct tcp_stream *tcp_s,
		struct tcp_stream_position *pos)
{
	if (!pos->chunk) {
		if (!tcp_s->first || tcp_s->first->start_seq != pos->chunk_seq) {
			if (!pos->modif) {
				if (tcp_s->pending_modif) {
					pos->modif = tcp_s->pending_modif;
					pos->modif_offset = 0;
				}
				else {
					return false;
				}
			}
		}
		else {
			pos->chunk = tcp_s->first;
			pos->chunk_offset = 0;
			pos->chunk_seq_modif = pos->chunk->start_seq + tcp_s->first_offset_seq;
			assert(pos->chunk_seq == pos->chunk->start_seq);

			if (pos->modif) {
				assert(pos->modif == pos->chunk->modifs);
				assert(pos->current_seq_modif == pos->chunk_seq_modif + pos->modif_offset);
			}
			else {
				pos->modif = NULL;
				pos->modif_offset = (size_t)-1;
				assert(pos->current_seq_modif == pos->chunk_seq_modif);
			}
		}
	}

	while (true) {
		struct tcp_stream_chunk_modif *cur_modif;
		size_t modif_offset;

		tcp_stream_position_update_modif(pos);

		if (pos->chunk) {
			if (tcp_stream_position_chunk_at_end(pos)) {
				if (!pos->chunk->next || !tcp_stream_position_next_chunk(tcp_s, pos)) {
					return false;
				}
			}
			else {
				assert(pos->chunk->start_seq + pos->chunk_offset <= pos->chunk->end_seq);
			}
		}
		else if (pos->modif) {
			/* Pending modif case */
			assert(pos->modif == tcp_s->pending_modif);
			assert(pos->modif->next == NULL);

			if (pos->modif_offset >= pos->modif->length) {
				return false;
			}
		}
		else {
			return false;
		}

		cur_modif = tcp_stream_position_modif(pos, &modif_offset, NULL, NULL);
		if (cur_modif) {
			if (cur_modif->type == TCP_MODIF_ERASE) {
				assert(pos->chunk);
				pos->chunk_offset += cur_modif->length;
				assert(pos->chunk->start_seq+pos->chunk_offset <= pos->chunk->end_seq);
				pos->modif = cur_modif;
				pos->modif_offset = 1;
			}
			else {
				break;
			}
		}
		else {
			break;
		}
	}

	return true;
}

static size_t tcp_stream_position_read_step(struct tcp_stream *tcp_s,
		struct tcp_stream_position *pos, uint8 *data, size_t length)
{
	struct tcp_stream_chunk_modif *next_modif = NULL;
	struct tcp_stream_chunk_modif *current_modif = NULL;
	size_t modif_offset;

	if (!tcp_stream_position_advance(tcp_s, pos)) {
		return (size_t)-1;
	}

	current_modif = tcp_stream_position_modif(pos, &modif_offset, NULL, &next_modif);
	if (current_modif) {
		pos->modif = current_modif;
		pos->modif_offset = modif_offset;

		assert(current_modif->type == TCP_MODIF_INSERT);
		assert(pos->modif_offset < current_modif->length);

		const size_t maxlength = MIN(length, current_modif->length - pos->modif_offset);
		if (data) memcpy(data, current_modif->data + pos->modif_offset, maxlength);
		pos->modif_offset += maxlength;
		pos->current_seq_modif += maxlength;
		return maxlength;
	}
	else if (pos->chunk) {
		size_t maxlength;

		if (next_modif) {
			maxlength = MIN(next_modif->position - pos->chunk_offset, length);
		}
		else {
			maxlength = MIN((pos->chunk->end_seq - pos->chunk->start_seq) - pos->chunk_offset, length);
		}

		if (data) memcpy(data, tcp_get_payload(pos->chunk->tcp) + pos->chunk_offset, maxlength);
		pos->chunk_offset += maxlength;
		pos->current_seq_modif += maxlength;
		return maxlength;
	}
	else {
		return (size_t)-1;
	}
}

static size_t tcp_stream_position_read(struct tcp_stream *tcp_s,
		struct tcp_stream_position *pos, uint8 *data, size_t length)
{
	size_t left_len = length;

	while (left_len > 0) {
		const size_t len = tcp_stream_position_read_step(tcp_s, pos, data, left_len);
		if (len == (size_t)-1) {
			break;
		}

		left_len -= len;
		if (data) data += len;
	}

	return length - left_len;
}

static size_t tcp_stream_position_skip_available(struct tcp_stream *tcp_s,
		struct tcp_stream_position *pos)
{
	size_t chunk_length, length, total_length  = 0;

	while (true) {
		if (!tcp_stream_position_advance(tcp_s, pos)) {
			break;
		}

		if (pos->chunk) {
			chunk_length = pos->chunk->end_seq - pos->chunk->start_seq + pos->chunk->offset_seq;
			length = chunk_length - (pos->current_seq_modif - pos->chunk_seq_modif);
			pos->chunk_offset = pos->chunk->end_seq - pos->chunk->start_seq;

			if (!pos->modif) pos->modif = pos->chunk->modifs;

			if (pos->modif) {
				while (pos->modif->next) {
					pos->modif = pos->modif->next;

					switch (pos->modif->type) {
					case TCP_MODIF_INSERT:
						pos->modif_offset = pos->modif->length;
						break;

					case TCP_MODIF_ERASE:
						pos->modif_offset = 1;
						break;

					default:
						assert(0);
					}
				}
			}
			else {
				pos->modif_offset = (size_t)-1;
			}
		}
		else if (pos->modif) {
			assert(pos->modif->type == TCP_MODIF_INSERT);
			length = pos->modif->length - pos->modif_offset;
			pos->modif_offset = pos->modif->length;
		}
		else {
			break;
		}

		pos->current_seq_modif += length;
		total_length += length;
	}

	return total_length;
}

static void tcp_stream_position_try_advance_chunk(struct tcp_stream *tcp_s,
		struct tcp_stream_position *pos, struct tcp_stream_chunk *chunk)
{
	if (pos->chunk && pos->chunk == chunk) {
		if (tcp_stream_position_chunk_at_end(pos)) {
			tcp_stream_position_next_chunk(tcp_s, pos);
		}
	}
}

static bool tcp_stream_position_isbefore(struct tcp_stream_position *pos1,
		struct tcp_stream_position *pos2)
{
	return (pos1->current_seq_modif <= pos2->current_seq_modif);
}


/*
 * Stream functions
 */

#define TCP_STREAM(s) struct tcp_stream *tcp_s = (struct tcp_stream *)(s); \
	assert((s)->ftable == &tcp_stream_ftable);

struct stream *tcp_stream_create()
{
	struct tcp_stream *tcp_s = malloc(sizeof(struct tcp_stream));
	if (!tcp_s) {
		error(L"memory error");
		return NULL;
	}

	memset(tcp_s, 0, sizeof(struct tcp_stream));

	tcp_stream_position_invalidate(&tcp_s->mark_position);

	tcp_s->stream.ftable = &tcp_stream_ftable;
	return &tcp_s->stream;
}

static bool tcp_stream_destroy(struct stream *s)
{
	struct tcp_stream_chunk *tmp, *iter;
	TCP_STREAM(s);

	iter = tcp_s->first;
	while (iter) {
		assert(iter->tcp);
		tcp_release(iter->tcp);

		tmp = iter;
		iter = iter->next;
		free(tmp);
	}

	iter = tcp_s->sent;
	while (iter) {
		tmp = iter;
		iter = iter->next;
		free(tmp);
	}

	free(tcp_s->pending_modif);

	free(s);
	return true;
}

bool tcp_stream_push(struct stream *s, struct tcp *tcp)
{
	struct tcp_stream_chunk *chunk;
	TCP_STREAM(s);

	if (tcp_get_flags_syn(tcp)) {
		if (!tcp_s->seq_initialized) {
			tcp_s->start_seq = tcp_get_seq(tcp)+1;
			tcp_s->seq_initialized = true;
			return true;
		}
		else {
			return true;
		}
	}

	if (!tcp_s->seq_initialized) {
		error(L"invalid stream");
		return false;
	}

	chunk = malloc(sizeof(struct tcp_stream_chunk));
	if (!chunk) {
		error(L"memory error");
		return false;
	}

	chunk->tcp = tcp;
	chunk->start_seq = tcp_get_seq(tcp);
	if (chunk->start_seq < tcp_s->start_seq) {
		error(L"invalid sequence number: %u < %u", chunk->start_seq, tcp_s->start_seq);
		free(chunk);
		return false;
	}

	chunk->start_seq -= tcp_s->start_seq;
	chunk->end_seq = chunk->start_seq + tcp_get_payload_length(tcp);
	chunk->offset_seq = 0;
	chunk->modifs = NULL;

	if (chunk->start_seq < tcp_s->current_position.chunk_seq + tcp_s->current_position.chunk_offset ||
		chunk->end_seq < tcp_s->current_position.chunk_seq + tcp_s->current_position.chunk_offset) {
		error(L"retransmit packet (unsupported)");
		free(chunk);
		return false;
	}

	/* Search for insert point */
	if (!tcp_s->last) {
		tcp_s->first = chunk;
		tcp_s->last = chunk;
		chunk->next = NULL;
	}
	else {
		struct tcp_stream_chunk **iter;

		if (tcp_s->last->start_seq < chunk->start_seq) {
			iter = &tcp_s->last;
		}
		else {
			iter = &tcp_s->first;
		}

		do {
			iter = &((*iter)->next);
		}
		while (*iter && (*iter)->start_seq < chunk->start_seq);

		if (*iter) {
			if (chunk->end_seq <= (*iter)->start_seq) {
				chunk->next = (*iter)->next;
			}
			else {
				error(L"retransmit packet (unsupported)");
				free(chunk);
				return false;
			}
		}
		else {
			chunk->next = NULL;
		}

		*iter = chunk;

		if (tcp_s->pending_modif) {
			assert(!chunk->modifs);
			assert(!tcp_s->pending_modif->next && !tcp_s->pending_modif->prev);
			assert(tcp_s->pending_modif->position == 0);
			chunk->modifs = tcp_s->pending_modif;
			chunk->offset_seq += chunk->modifs->length;
			tcp_s->pending_modif = NULL;
		}

		tcp_s->last = chunk;
	}

	return true;
}

static void tcp_stream_chunk_release(struct tcp_stream_chunk *chunk)
{
	struct tcp_stream_chunk_modif *iter = chunk->modifs;
	while (iter) {
		struct tcp_stream_chunk_modif *modif = iter;
		iter = iter->next;
		free(modif);
	}

	chunk->modifs = NULL;
}

struct tcp *tcp_stream_pop(struct stream *s)
{
	struct tcp *tcp;
	struct tcp_stream_chunk *chunk;
	struct tcp_stream_position *pos;
	TCP_STREAM(s);

	chunk = tcp_s->first;

	assert(!tcp_stream_position_isvalid(&tcp_s->mark_position) ||
			tcp_stream_position_isbefore(&tcp_s->mark_position, &tcp_s->current_position));

	pos = &tcp_s->current_position;
	tcp_stream_position_advance(tcp_s, pos);

	if (tcp_stream_position_isvalid(&tcp_s->mark_position)) {
		tcp_stream_position_try_advance_chunk(tcp_s, pos, chunk);

		pos = &tcp_s->mark_position;
		tcp_stream_position_advance(tcp_s, pos);
		tcp_stream_position_try_advance_chunk(tcp_s, pos, chunk);
	}
	else {
		/* Force a skip of all available data */
		tcp_stream_position_skip_available(tcp_s, pos);
		tcp_stream_position_try_advance_chunk(tcp_s, pos, chunk);
	}

	if (chunk && tcp_stream_position_chunk_is_before(pos, chunk)) {
		tcp = chunk->tcp;

		if (chunk->modifs) {
			/*
			 * Apply modifs to tcp packet
			 */
			const size_t new_size = chunk->end_seq - chunk->start_seq + chunk->offset_seq;
			struct tcp_stream_position pos;
			uint8 *buffer, *payload;
			size_t size;

			pos.chunk = chunk;
			pos.chunk_offset = 0;
			pos.modif = NULL;
			pos.modif_offset = 0;
			pos.chunk_seq = chunk->start_seq;
			pos.chunk_seq_modif = chunk->start_seq + tcp_s->first_offset_seq;
			pos.current_seq_modif = pos.chunk_seq_modif;

			buffer = malloc(new_size);
			if (!buffer) {
				error(L"memory error");
				return NULL;
			}

			size = tcp_stream_position_read(tcp_s, &pos, buffer, new_size);
			assert(size == new_size);

			payload = tcp_resize_payload(tcp, new_size);
			if (!payload) {
				error(L"memory error");
				free(buffer);
				return NULL;
			}

			memcpy(payload, buffer, new_size);
			free(buffer);
		}

		tcp_set_seq(tcp, tcp_get_seq(tcp) + tcp_s->first_offset_seq);

		tcp_s->first_offset_seq += tcp_s->first->offset_seq;
		tcp_s->first = tcp_s->first->next;

		if (tcp_s->last == chunk) {
			assert(chunk->next == NULL);
			tcp_s->last = NULL;
		}

		chunk->next = NULL;
		if (tcp_s->last_sent) {
			assert(tcp_s->last_sent->end_seq == chunk->start_seq);
			tcp_s->last_sent->next = chunk;
			tcp_s->last_sent = chunk;
		}
		else {
			tcp_s->last_sent = chunk;
			tcp_s->sent = chunk;
		}
		chunk->tcp = NULL;

		tcp_stream_chunk_release(chunk);
		return tcp;
	}

	return NULL;
}

void tcp_stream_ack(struct stream *s, struct tcp *tcp)
{
	uint32 ack = tcp_get_ack_seq(tcp);
	uint32 seq, new_seq;
	struct tcp_stream_chunk *iter;
	TCP_STREAM(s);

	iter = tcp_s->sent;
	if (!iter) {
		return;
	}

	ack -= tcp_s->start_seq;
	seq = tcp_s->sent_offset_seq + tcp_s->sent->start_seq;
	new_seq = tcp_s->sent->start_seq;

	while (iter) {
		if (iter->end_seq + iter->offset_seq > ack) {
			break;
		}

		seq += (iter->end_seq - iter->start_seq) + iter->offset_seq;
		new_seq = iter->end_seq;
		if (ack <= seq) {
			break;
		}

		assert(!iter->next || iter->next->start_seq == iter->end_seq);

		iter = iter->next;
	}

	tcp_set_ack_seq(tcp, new_seq + tcp_s->start_seq);
}

static size_t tcp_stream_read(struct stream *s, uint8 *data, size_t length)
{
	TCP_STREAM(s);
	return tcp_stream_position_read(tcp_s, &tcp_s->current_position, data, length);
}

static size_t tcp_stream_available(struct stream *s)
{
	struct tcp_stream_position position;
	TCP_STREAM(s);

	position = tcp_s->current_position;
	return tcp_stream_position_skip_available(tcp_s, &position);
}

static struct tcp_stream_chunk_modif *tcp_stream_create_insert_modif(struct tcp_stream *tcp_s,
		struct tcp_stream_chunk_modif *prev, struct tcp_stream_chunk_modif *next,
		const uint8 *data, size_t length)
{
	struct tcp_stream_chunk_modif *new_modif = NULL;
	struct tcp_stream_position *pos;

	pos = &tcp_s->current_position;

	new_modif = malloc(sizeof(struct tcp_stream_chunk_modif) + length);
	if (!new_modif) {
		error(L"memory error");
		return NULL;
	}

	new_modif->type = TCP_MODIF_INSERT;
	new_modif->position = pos->chunk_offset;
	new_modif->length = length;
	memcpy(new_modif->data, data, length);

	if (prev) {
		new_modif->prev = prev;
		new_modif->next = prev->next;
		if (new_modif->next) new_modif->next->prev = new_modif;
		prev->next = new_modif;
	}
	else {
		new_modif->next = next;
		new_modif->prev = NULL;
		if (pos->chunk) pos->chunk->modifs = new_modif;
	}

	if (pos->chunk) pos->chunk->offset_seq += length;

	pos->modif = new_modif;
	pos->modif_offset = length;
	pos->current_seq_modif += length;

	return new_modif;
}

static size_t tcp_stream_update_insert_modif(struct tcp_stream *tcp_s,
		struct tcp_stream_chunk_modif *cur_modif, const uint8 *data,
		size_t length, size_t modif_offset)
{
	struct tcp_stream_chunk_modif *new_modif = NULL;
	struct tcp_stream_position *pos;

	assert(cur_modif->type == TCP_MODIF_INSERT);
	pos = &tcp_s->current_position;

	new_modif = malloc(sizeof(struct tcp_stream_chunk_modif) + length + cur_modif->length);
	if (!new_modif) {
		error(L"memory error");
		return -1;
	}

	new_modif->type = TCP_MODIF_INSERT;
	new_modif->position = cur_modif->position;
	new_modif->length = cur_modif->length + length;

	memcpy(new_modif->data, cur_modif->data, modif_offset);
	memcpy(new_modif->data + modif_offset, data, length);
	memcpy(new_modif->data + modif_offset + length,
			cur_modif->data + modif_offset,
			cur_modif->length - modif_offset);

	new_modif->next = cur_modif->next;
	new_modif->prev = cur_modif->prev;
	if (cur_modif->next) cur_modif->next->prev = new_modif;
	if (cur_modif->prev) cur_modif->prev->next = new_modif;

	if (pos->chunk) pos->chunk->offset_seq += length;
	free(cur_modif);

	pos->modif = new_modif;
	pos->modif_offset = modif_offset + length;
	pos->current_seq_modif += length;

	return length;
}

static size_t tcp_stream_insert(struct stream *s, const uint8 *data, size_t length)
{
	struct tcp_stream_chunk_modif *prev_modif = NULL;
	struct tcp_stream_chunk_modif *next_modif = NULL;
	struct tcp_stream_chunk_modif *cur_modif = NULL;
	struct tcp_stream_position *pos;
	size_t modif_offset;
	TCP_STREAM(s);

	pos = &tcp_s->current_position;
	tcp_stream_position_advance(tcp_s, pos);

	if (!pos->chunk) {
		if (tcp_s->pending_modif) {
			return tcp_stream_update_insert_modif(tcp_s, tcp_s->pending_modif,
					data, length, modif_offset);
		}
		else {
			tcp_s->pending_modif = tcp_stream_create_insert_modif(tcp_s, NULL,
					NULL, data, length);
			if (!tcp_s->pending_modif) {
				return -1;
			}
			return length;
		}
	}
	else {
		cur_modif = tcp_stream_position_modif(pos, &modif_offset, &prev_modif, &next_modif);
		if (cur_modif) {
			return tcp_stream_update_insert_modif(tcp_s, cur_modif, data, length,
					modif_offset);
		}
		else {
			cur_modif = tcp_stream_create_insert_modif(tcp_s, prev_modif, next_modif, data, length);
			if (!cur_modif) {
				return -1;
			}
			return length;
		}
	}
}

static size_t tcp_stream_replace(struct stream *s, const uint8 *data, size_t length)
{
	const size_t ret = tcp_stream_insert(s, data, length);
	tcp_stream_erase(s, length);
	return ret;
}

static size_t tcp_stream_erase(struct stream *s, size_t length)
{
	struct tcp_stream_chunk_modif *prev_modif = NULL;
	struct tcp_stream_chunk_modif *next_modif = NULL;
	struct tcp_stream_chunk_modif *cur_modif = NULL;
	struct tcp_stream_position *pos;
	size_t modif_offset, erase_length;
	TCP_STREAM(s);

	pos = &tcp_s->current_position;

	if (!tcp_stream_position_advance(tcp_s, pos)) {
		return 0;
	}

	cur_modif = tcp_stream_position_modif(pos, &modif_offset, &prev_modif, &next_modif);
	if (cur_modif) {
		assert(cur_modif->type == TCP_MODIF_INSERT);
		struct tcp_stream_chunk_modif *new_modif = NULL;
		size_t max_erase;

		max_erase = cur_modif->length - pos->modif_offset;
		erase_length = MIN(max_erase, length);

		if (cur_modif->length == erase_length) {
			/* Remove modif */
			if (cur_modif->next) cur_modif->next->prev = cur_modif->prev;
			if (cur_modif->prev) cur_modif->prev->next = cur_modif->next;

			if (!prev_modif) {
				assert(pos->chunk->modifs == cur_modif);
				pos->chunk->modifs = cur_modif->next;
			}

			if (pos->modif == cur_modif) {
				pos->modif = prev_modif;
				pos->modif_offset = (size_t)-1;
			}
		}
		else {
			/* Modify an existing modif */
			new_modif = malloc(sizeof(struct tcp_stream_chunk_modif) + cur_modif->length - erase_length);
			if (!new_modif) {
				error(L"memory error");
				return -1;
			}

			new_modif->type = TCP_MODIF_INSERT;
			new_modif->position = cur_modif->position;
			new_modif->length = cur_modif->length - erase_length;

			memcpy(new_modif->data, cur_modif->data, modif_offset);
			memcpy(new_modif->data + modif_offset,
					cur_modif->data + modif_offset + erase_length,
					cur_modif->length - modif_offset - erase_length);

			new_modif->next = cur_modif->next;
			new_modif->prev = cur_modif->prev;
			if (cur_modif->next) cur_modif->next->prev = new_modif;
			if (cur_modif->prev) cur_modif->prev->next = new_modif;

			if (!prev_modif) {
				assert(pos->chunk->modifs == cur_modif);
				pos->chunk->modifs = new_modif;
			}

			if (pos->modif == cur_modif) {
				pos->modif = new_modif;
			}
		}

		pos->chunk->offset_seq -= erase_length;
		free(cur_modif);
	}
	else {
		size_t max_erase;

		if (next_modif) {
			max_erase = next_modif->position - pos->chunk_offset;
		}
		else {
			max_erase = (pos->chunk->end_seq - pos->chunk->start_seq) - pos->chunk_offset;
		}

		erase_length = MIN(max_erase, length);

		/* Create a new modif */
		cur_modif = malloc(sizeof(struct tcp_stream_chunk_modif));
		if (!cur_modif) {
			error(L"memory error");
			return -1;
		}

		cur_modif->type = TCP_MODIF_ERASE;
		cur_modif->position = pos->chunk_offset;
		cur_modif->length = erase_length;

		if (prev_modif) {
			cur_modif->prev = prev_modif;
			cur_modif->next = prev_modif->next;
			if (cur_modif->next) cur_modif->next->prev = cur_modif;
			prev_modif->next = cur_modif;
		}
		else {
			cur_modif->next = next_modif;
			cur_modif->prev = NULL;
			pos->chunk->modifs = cur_modif;
		}

		pos->chunk->offset_seq -= erase_length;

		pos->modif = cur_modif;
		pos->modif_offset = 1;
		pos->chunk_offset += erase_length;
	}

	if (erase_length > 0 && erase_length < length) {
		return erase_length + tcp_stream_erase(s, length-erase_length);
	}
	else {
		return erase_length;
	}
}

static bool tcp_stream_mark(struct stream *s)
{
	TCP_STREAM(s);

	tcp_s->mark_position = tcp_s->current_position;
	return true;
}

static bool tcp_stream_unmark(struct stream *s)
{
	TCP_STREAM(s);

	if (tcp_stream_position_isvalid(&tcp_s->mark_position)) {
		tcp_stream_position_invalidate(&tcp_s->mark_position);
		return true;
	}
	else {
		error(L"stream was not marked");
		return false;
	}
}

static bool tcp_stream_rewind(struct stream *s)
{
	TCP_STREAM(s);

	if (tcp_stream_position_isvalid(&tcp_s->mark_position)) {
		tcp_s->current_position = tcp_s->mark_position;
		tcp_stream_position_invalidate(&tcp_s->mark_position);
		return true;
	}
	else {
		error(L"stream was not marked");
		return false;
	}
}