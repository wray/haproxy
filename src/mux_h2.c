/*
 * HTTP/2 mux-demux for connections
 *
 * Copyright 2017 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <common/cfgparse.h>
#include <common/config.h>
#include <common/h2.h>
#include <common/hpack-dec.h>
#include <common/hpack-enc.h>
#include <common/hpack-tbl.h>
#include <common/net_helper.h>
#include <proto/connection.h>
#include <proto/h1.h>
#include <proto/stream.h>
#include <types/session.h>
#include <eb32tree.h>


/* dummy streams returned for idle and closed states */
static const struct h2s *h2_closed_stream;
static const struct h2s *h2_idle_stream;

/* the h2c connection pool */
static struct pool_head *pool_head_h2c;
/* the h2s stream pool */
static struct pool_head *pool_head_h2s;

/* Connection flags (32 bit), in h2c->flags */
#define H2_CF_NONE              0x00000000

/* Flags indicating why writing to the mux is blocked. */
#define H2_CF_MUX_MALLOC        0x00000001  // mux blocked on lack of connection's mux buffer
#define H2_CF_MUX_MFULL         0x00000002  // mux blocked on connection's mux buffer full
#define H2_CF_MUX_BLOCK_ANY     0x00000003  // aggregate of the mux flags above

/* Flags indicating why writing to the demux is blocked.
 * The first two ones directly affect the ability for the mux to receive data
 * from the connection. The other ones affect the mux's ability to demux
 * received data.
 */
#define H2_CF_DEM_DALLOC        0x00000004  // demux blocked on lack of connection's demux buffer
#define H2_CF_DEM_DFULL         0x00000008  // demux blocked on connection's demux buffer full

#define H2_CF_DEM_MBUSY         0x00000010  // demux blocked on connection's mux side busy
#define H2_CF_DEM_MROOM         0x00000020  // demux blocked on lack of room in mux buffer
#define H2_CF_DEM_SALLOC        0x00000040  // demux blocked on lack of stream's request buffer
#define H2_CF_DEM_SFULL         0x00000080  // demux blocked on stream request buffer full
#define H2_CF_DEM_TOOMANY       0x00000100  // demux blocked waiting for some conn_streams to leave
#define H2_CF_DEM_BLOCK_ANY     0x000001F0  // aggregate of the demux flags above except DALLOC/DFULL

/* other flags */
#define H2_CF_GOAWAY_SENT       0x00001000  // a GOAWAY frame was successfully sent
#define H2_CF_GOAWAY_FAILED     0x00002000  // a GOAWAY frame failed to be sent
#define H2_CF_WAIT_FOR_HS       0x00004000  // We did check that at least a stream was waiting for handshake


/* H2 connection state, in h2c->st0 */
enum h2_cs {
	H2_CS_PREFACE,   // init done, waiting for connection preface
	H2_CS_SETTINGS1, // preface OK, waiting for first settings frame
	H2_CS_FRAME_H,   // first settings frame ok, waiting for frame header
	H2_CS_FRAME_P,   // frame header OK, waiting for frame payload
	H2_CS_FRAME_A,   // frame payload OK, trying to send ACK frame
	H2_CS_FRAME_E,   // frame payload OK, trying to send RST frame
	H2_CS_ERROR,     // send GOAWAY(errcode) and close the connection ASAP
	H2_CS_ERROR2,    // GOAWAY(errcode) sent, close the connection ASAP
	H2_CS_ENTRIES    // must be last
} __attribute__((packed));

/* H2 connection descriptor */
struct h2c {
	struct connection *conn;

	enum h2_cs st0; /* mux state */
	enum h2_err errcode; /* H2 err code (H2_ERR_*) */

	/* 16 bit hole here */
	uint32_t flags; /* connection flags: H2_CF_* */
	int32_t max_id; /* highest ID known on this connection, <0 before preface */
	uint32_t rcvd_c; /* newly received data to ACK for the connection */
	uint32_t rcvd_s; /* newly received data to ACK for the current stream (dsi) */

	/* states for the demux direction */
	struct hpack_dht *ddht; /* demux dynamic header table */
	struct buffer dbuf;    /* demux buffer */

	int32_t dsi; /* demux stream ID (<0 = idle) */
	int32_t dfl; /* demux frame length (if dsi >= 0) */
	int8_t  dft; /* demux frame type   (if dsi >= 0) */
	int8_t  dff; /* demux frame flags  (if dsi >= 0) */
	uint8_t dpl; /* demux pad length (part of dfl), init to 0 */
	/* 8 bit hole here */
	int32_t last_sid; /* last processed stream ID for GOAWAY, <0 before preface */

	/* states for the mux direction */
	struct buffer mbuf;    /* mux buffer */
	int32_t msi; /* mux stream ID (<0 = idle) */
	int32_t mfl; /* mux frame length (if dsi >= 0) */
	int8_t  mft; /* mux frame type   (if dsi >= 0) */
	int8_t  mff; /* mux frame flags  (if dsi >= 0) */
	/* 16 bit hole here */
	int32_t miw; /* mux initial window size for all new streams */
	int32_t mws; /* mux window size. Can be negative. */
	int32_t mfs; /* mux's max frame size */

	int timeout;        /* idle timeout duration in ticks */
	int shut_timeout;   /* idle timeout duration in ticks after GOAWAY was sent */
	unsigned int nb_streams;  /* number of streams in the tree */
	unsigned int nb_cs;       /* number of attached conn_streams */
	struct task *task;  /* timeout management task */
	struct eb_root streams_by_id; /* all active streams by their ID */
	struct list send_list; /* list of blocked streams requesting to send */
	struct list fctl_list; /* list of streams blocked by connection's fctl */
	struct buffer_wait buf_wait; /* wait list for buffer allocations */
	struct wait_list wait_list;  /* We're in a wait list, to send */
};

/* H2 stream state, in h2s->st */
enum h2_ss {
	H2_SS_IDLE = 0, // idle
	H2_SS_RLOC,     // reserved(local)
	H2_SS_RREM,     // reserved(remote)
	H2_SS_OPEN,     // open
	H2_SS_HREM,     // half-closed(remote)
	H2_SS_HLOC,     // half-closed(local)
	H2_SS_ERROR,    // an error needs to be sent using RST_STREAM
	H2_SS_CLOSED,   // closed
	H2_SS_ENTRIES   // must be last
} __attribute__((packed));

/* HTTP/2 stream flags (32 bit), in h2s->flags */
#define H2_SF_NONE              0x00000000
#define H2_SF_ES_RCVD           0x00000001
#define H2_SF_ES_SENT           0x00000002

#define H2_SF_RST_RCVD          0x00000004 // received RST_STREAM
#define H2_SF_RST_SENT          0x00000008 // sent RST_STREAM

/* stream flags indicating the reason the stream is blocked */
#define H2_SF_BLK_MBUSY         0x00000010 // blocked waiting for mux access (transient)
#define H2_SF_BLK_MROOM         0x00000020 // blocked waiting for room in the mux
#define H2_SF_BLK_MFCTL         0x00000040 // blocked due to mux fctl
#define H2_SF_BLK_SFCTL         0x00000080 // blocked due to stream fctl
#define H2_SF_BLK_ANY           0x000000F0 // any of the reasons above

/* stream flags indicating how data is supposed to be sent */
#define H2_SF_DATA_CLEN         0x00000100 // data sent using content-length
#define H2_SF_DATA_CHNK         0x00000200 // data sent using chunked-encoding

/* step we're currently in when sending chunks. This is needed because we may
 * have to transfer chunks as large as a full buffer so there's no room left
 * for size nor crlf around.
 */
#define H2_SF_CHNK_SIZE         0x00000000 // trying to send chunk size
#define H2_SF_CHNK_DATA         0x00000400 // trying to send chunk data
#define H2_SF_CHNK_CRLF         0x00000800 // trying to send chunk crlf after data

#define H2_SF_CHNK_MASK         0x00000C00 // trying to send chunk size

#define H2_SF_HEADERS_SENT      0x00001000  // a HEADERS frame was sent for this stream
#define H2_SF_OUTGOING_DATA     0x00002000  // set whenever we've seen outgoing data

/* H2 stream descriptor, describing the stream as it appears in the H2C, and as
 * it is being processed in the internal HTTP representation (H1 for now).
 */
struct h2s {
	struct conn_stream *cs;
	struct h2c *h2c;
	struct h1m h1m;         /* request or response parser state for H1 */
	struct eb32_node by_id; /* place in h2c's streams_by_id */
	int32_t id; /* stream ID */
	uint32_t flags;      /* H2_SF_* */
	int mws;             /* mux window size for this stream */
	enum h2_err errcode; /* H2 err code (H2_ERR_*) */
	enum h2_ss st;
	uint16_t status;     /* HTTP response status */
	struct buffer rxbuf; /* receive buffer, always valid (buf_empty or real buffer) */
	struct wait_list wait_list; /* Wait list, when we're attempting to send a RST but we can't send */
	struct wait_list *recv_wait_list; /* Address of the wait_list the conn_stream associated is waiting on */
};

/* descriptor for an h2 frame header */
struct h2_fh {
	uint32_t len;       /* length, host order, 24 bits */
	uint32_t sid;       /* stream id, host order, 31 bits */
	uint8_t ft;         /* frame type */
	uint8_t ff;         /* frame flags */
};

/* a few settings from the global section */
static int h2_settings_header_table_size      =  4096; /* initial value */
static int h2_settings_initial_window_size    = 65535; /* initial value */
static int h2_settings_max_concurrent_streams =   100;

/* a dmumy closed stream */
static const struct h2s *h2_closed_stream = &(const struct h2s){
	.cs        = NULL,
	.h2c       = NULL,
	.st        = H2_SS_CLOSED,
	.errcode   = H2_ERR_STREAM_CLOSED,
	.flags     = H2_SF_RST_RCVD,
	.id        = 0,
};

/* and a dummy idle stream for use with any unannounced stream */
static const struct h2s *h2_idle_stream = &(const struct h2s){
	.cs        = NULL,
	.h2c       = NULL,
	.st        = H2_SS_IDLE,
	.errcode   = H2_ERR_STREAM_CLOSED,
	.id        = 0,
};

static struct task *h2_timeout_task(struct task *t, void *context, unsigned short state);
static int h2_send(struct h2c *h2c);
static int h2_recv(struct h2c *h2c);
static int h2_process(struct h2c *h2c);
static struct task *h2_io_cb(struct task *t, void *ctx, unsigned short state);
static inline struct h2s *h2c_st_by_id(struct h2c *h2c, int id);
static int h2_frt_decode_headers(struct h2s *h2s);
static int h2_frt_transfer_data(struct h2s *h2s);
static struct task *h2_deferred_shut(struct task *t, void *ctx, unsigned short state);

/*****************************************************/
/* functions below are for dynamic buffer management */
/*****************************************************/

/* indicates whether or not the we may call the h2_recv() function to attempt
 * to receive data into the buffer and/or demux pending data. The condition is
 * a bit complex due to some API limits for now. The rules are the following :
 *   - if an error or a shutdown was detected on the connection and the buffer
 *     is empty, we must not attempt to receive
 *   - if the demux buf failed to be allocated, we must not try to receive and
 *     we know there is nothing pending
 *   - if no flag indicates a blocking condition, we may attempt to receive,
 *     regardless of whether the demux buffer is full or not, so that only
 *     de demux part decides whether or not to block. This is needed because
 *     the connection API indeed prevents us from re-enabling receipt that is
 *     already enabled in a polled state, so we must always immediately stop
 *     as soon as the demux can't proceed so as never to hit an end of read
 *     with data pending in the buffers.
 *   - otherwise must may not attempt
 */
static inline int h2_recv_allowed(const struct h2c *h2c)
{
	if (b_data(&h2c->dbuf) == 0 &&
	    (h2c->st0 >= H2_CS_ERROR ||
	     h2c->conn->flags & CO_FL_ERROR ||
	     conn_xprt_read0_pending(h2c->conn)))
		return 0;

	if (!(h2c->flags & H2_CF_DEM_DALLOC) &&
	    !(h2c->flags & H2_CF_DEM_BLOCK_ANY))
		return 1;

	return 0;
}

/* returns true if the connection has too many conn_streams attached */
static inline int h2_has_too_many_cs(const struct h2c *h2c)
{
	return h2c->nb_cs >= h2_settings_max_concurrent_streams;
}

/* Tries to grab a buffer and to re-enable processing on mux <target>. The h2c
 * flags are used to figure what buffer was requested. It returns 1 if the
 * allocation succeeds, in which case the connection is woken up, or 0 if it's
 * impossible to wake up and we prefer to be woken up later.
 */
static int h2_buf_available(void *target)
{
	struct h2c *h2c = target;
	struct h2s *h2s;

	if ((h2c->flags & H2_CF_DEM_DALLOC) && b_alloc_margin(&h2c->dbuf, 0)) {
		h2c->flags &= ~H2_CF_DEM_DALLOC;
		if (h2_recv_allowed(h2c)) {
			conn_xprt_want_recv(h2c->conn);
			tasklet_wakeup(h2c->wait_list.task);
		}
		return 1;
	}

	if ((h2c->flags & H2_CF_MUX_MALLOC) && b_alloc_margin(&h2c->mbuf, 0)) {
		h2c->flags &= ~H2_CF_MUX_MALLOC;
		if (!(h2c->flags & H2_CF_MUX_BLOCK_ANY))
			conn_xprt_want_send(h2c->conn);

		if (h2c->flags & H2_CF_DEM_MROOM) {
			h2c->flags &= ~H2_CF_DEM_MROOM;
			if (h2_recv_allowed(h2c)) {
				conn_xprt_want_recv(h2c->conn);
				tasklet_wakeup(h2c->wait_list.task);
			}
		}
		return 1;
	}

	if ((h2c->flags & H2_CF_DEM_SALLOC) &&
	    (h2s = h2c_st_by_id(h2c, h2c->dsi)) && h2s->cs &&
	    b_alloc_margin(&h2s->rxbuf, 0)) {
		h2c->flags &= ~H2_CF_DEM_SALLOC;
		if (h2_recv_allowed(h2c)) {
			conn_xprt_want_recv(h2c->conn);
			tasklet_wakeup(h2c->wait_list.task);
		}
		return 1;
	}

	return 0;
}

static inline struct buffer *h2_get_buf(struct h2c *h2c, struct buffer *bptr)
{
	struct buffer *buf = NULL;

	if (likely(LIST_ISEMPTY(&h2c->buf_wait.list)) &&
	    unlikely((buf = b_alloc_margin(bptr, 0)) == NULL)) {
		h2c->buf_wait.target = h2c;
		h2c->buf_wait.wakeup_cb = h2_buf_available;
		HA_SPIN_LOCK(BUF_WQ_LOCK, &buffer_wq_lock);
		LIST_ADDQ(&buffer_wq, &h2c->buf_wait.list);
		HA_SPIN_UNLOCK(BUF_WQ_LOCK, &buffer_wq_lock);
		__conn_xprt_stop_recv(h2c->conn);
	}
	return buf;
}

static inline void h2_release_buf(struct h2c *h2c, struct buffer *bptr)
{
	if (bptr->size) {
		b_free(bptr);
		offer_buffers(h2c->buf_wait.target, tasks_run_queue);
	}
}


/*****************************************************************/
/* functions below are dedicated to the mux setup and management */
/*****************************************************************/

/* tries to initialize the inbound h2c mux. Returns < 0 in case of failure. */
static int h2c_frt_init(struct connection *conn)
{
	struct h2c *h2c;
	struct task *t = NULL;
	struct session *sess = conn->owner;

	h2c = pool_alloc(pool_head_h2c);
	if (!h2c)
		goto fail;


	h2c->shut_timeout = h2c->timeout = sess->fe->timeout.client;
	if (tick_isset(sess->fe->timeout.clientfin))
		h2c->shut_timeout = sess->fe->timeout.clientfin;

	h2c->task = NULL;
	if (tick_isset(h2c->timeout)) {
		t = task_new(tid_bit);
		if (!t)
			goto fail;

		h2c->task = t;
		t->process = h2_timeout_task;
		t->context = h2c;
		t->expire = tick_add(now_ms, h2c->timeout);
	}

	h2c->wait_list.task = tasklet_new();
	if (!h2c->wait_list.task)
		goto fail;
	h2c->wait_list.task->process = h2_io_cb;
	h2c->wait_list.task->context = h2c;
	h2c->wait_list.wait_reason = 0;

	h2c->ddht = hpack_dht_alloc(h2_settings_header_table_size);
	if (!h2c->ddht)
		goto fail;

	/* Initialise the context. */
	h2c->st0 = H2_CS_PREFACE;
	h2c->conn = conn;
	h2c->max_id = -1;
	h2c->errcode = H2_ERR_NO_ERROR;
	h2c->flags = H2_CF_NONE;
	h2c->rcvd_c = 0;
	h2c->rcvd_s = 0;
	h2c->nb_streams = 0;
	h2c->nb_cs = 0;

	h2c->dbuf = BUF_NULL;
	h2c->dsi = -1;
	h2c->msi = -1;
	h2c->last_sid = -1;

	h2c->mbuf = BUF_NULL;
	h2c->miw = 65535; /* mux initial window size */
	h2c->mws = 65535; /* mux window size */
	h2c->mfs = 16384; /* initial max frame size */
	h2c->streams_by_id = EB_ROOT_UNIQUE;
	LIST_INIT(&h2c->send_list);
	LIST_INIT(&h2c->fctl_list);
	LIST_INIT(&h2c->buf_wait.list);
	conn->mux_ctx = h2c;

	if (t)
		task_queue(t);
	conn_xprt_want_recv(conn);
	LIST_INIT(&h2c->wait_list.list);

	/* Try to read, if nothing is available yet we'll just subscribe */
	if (h2_recv(h2c))
		h2_process(h2c);
	return 0;
 fail:
	if (t)
		task_free(t);
	if (h2c->wait_list.task)
		tasklet_free(h2c->wait_list.task);
	pool_free(pool_head_h2c, h2c);
	return -1;
}

/* Initialize the mux once it's attached. For outgoing connections, the context
 * is already initialized before installing the mux, so we detect incoming
 * connections from the fact that the context is still NULL. Returns < 0 on
 * error.
 */
static int h2_init(struct connection *conn, struct proxy *prx)
{
	if (conn->mux_ctx) {
		/* we don't support outgoing connections for now */
		return -1;
	}

	return h2c_frt_init(conn);
}

/* returns the stream associated with id <id> or NULL if not found */
static inline struct h2s *h2c_st_by_id(struct h2c *h2c, int id)
{
	struct eb32_node *node;

	if (id > h2c->max_id)
		return (struct h2s *)h2_idle_stream;

	node = eb32_lookup(&h2c->streams_by_id, id);
	if (!node)
		return (struct h2s *)h2_closed_stream;

	return container_of(node, struct h2s, by_id);
}

/* release function for a connection. This one should be called to free all
 * resources allocated to the mux.
 */
static void h2_release(struct connection *conn)
{
	struct h2c *h2c = conn->mux_ctx;

	LIST_DEL(&conn->list);

	if (h2c) {
		hpack_dht_free(h2c->ddht);

		HA_SPIN_LOCK(BUF_WQ_LOCK, &buffer_wq_lock);
		LIST_DEL(&h2c->buf_wait.list);
		HA_SPIN_UNLOCK(BUF_WQ_LOCK, &buffer_wq_lock);

		h2_release_buf(h2c, &h2c->dbuf);
		h2_release_buf(h2c, &h2c->mbuf);

		if (h2c->task) {
			h2c->task->context = NULL;
			task_wakeup(h2c->task, TASK_WOKEN_OTHER);
			h2c->task = NULL;
		}
		if (h2c->wait_list.task)
			tasklet_free(h2c->wait_list.task);
		LIST_DEL(&h2c->wait_list.list);
		LIST_INIT(&h2c->wait_list.list);
		while (!LIST_ISEMPTY(&h2c->send_list)) {
			struct wait_list *sw = LIST_ELEM(h2c->send_list.n,
			    struct wait_list *, list);
			LIST_DEL(&sw->list);
			LIST_INIT(&sw->list);
		}
		while (!LIST_ISEMPTY(&h2c->fctl_list)) {
			struct wait_list *sw = LIST_ELEM(h2c->fctl_list.n,
			    struct wait_list *, list);
			LIST_DEL(&sw->list);
			LIST_INIT(&sw->list);
		}


		pool_free(pool_head_h2c, h2c);
	}

	conn->mux = NULL;
	conn->mux_ctx = NULL;

	conn_stop_tracking(conn);
	conn_full_close(conn);
	if (conn->destroy_cb)
		conn->destroy_cb(conn);
	conn_free(conn);
}


/******************************************************/
/* functions below are for the H2 protocol processing */
/******************************************************/

/* returns the stream if of stream <h2s> or 0 if <h2s> is NULL */
static inline __maybe_unused int h2s_id(const struct h2s *h2s)
{
	return h2s ? h2s->id : 0;
}

/* returns true of the mux is currently busy as seen from stream <h2s> */
static inline __maybe_unused int h2c_mux_busy(const struct h2c *h2c, const struct h2s *h2s)
{
	if (h2c->msi < 0)
		return 0;

	if (h2c->msi == h2s_id(h2s))
		return 0;

	return 1;
}

/* marks an error on the connection */
static inline __maybe_unused void h2c_error(struct h2c *h2c, enum h2_err err)
{
	h2c->errcode = err;
	h2c->st0 = H2_CS_ERROR;
}

/* marks an error on the stream */
static inline __maybe_unused void h2s_error(struct h2s *h2s, enum h2_err err)
{
	if (h2s->st > H2_SS_IDLE && h2s->st < H2_SS_ERROR) {
		h2s->errcode = err;
		h2s->st = H2_SS_ERROR;
		if (h2s->cs)
			h2s->cs->flags |= CS_FL_ERROR;
	}
}

/* writes the 24-bit frame size <len> at address <frame> */
static inline __maybe_unused void h2_set_frame_size(void *frame, uint32_t len)
{
	uint8_t *out = frame;

	*out = len >> 16;
	write_n16(out + 1, len);
}

/* reads <bytes> bytes from buffer <b> starting at relative offset <o> from the
 * current pointer, dealing with wrapping, and stores the result in <dst>. It's
 * the caller's responsibility to verify that there are at least <bytes> bytes
 * available in the buffer's input prior to calling this function. The buffer
 * is assumed not to hold any output data.
 */
static inline __maybe_unused void h2_get_buf_bytes(void *dst, size_t bytes,
                                    const struct buffer *b, int o)
{
	readv_bytes(dst, bytes, b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}

static inline __maybe_unused uint16_t h2_get_n16(const struct buffer *b, int o)
{
	return readv_n16(b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}

static inline __maybe_unused uint32_t h2_get_n32(const struct buffer *b, int o)
{
	return readv_n32(b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}

static inline __maybe_unused uint64_t h2_get_n64(const struct buffer *b, int o)
{
	return readv_n64(b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}


/* Peeks an H2 frame header from buffer <b> into descriptor <h>. The algorithm
 * is not obvious. It turns out that H2 headers are neither aligned nor do they
 * use regular sizes. And to add to the trouble, the buffer may wrap so each
 * byte read must be checked. The header is formed like this :
 *
 *       b0         b1       b2     b3   b4         b5..b8
 *  +----------+---------+--------+----+----+----------------------+
 *  |len[23:16]|len[15:8]|len[7:0]|type|flag|sid[31:0] (big endian)|
 *  +----------+---------+--------+----+----+----------------------+
 *
 * Here we read a big-endian 64 bit word from h[1]. This way in a single read
 * we get the sid properly aligned and ordered, and 16 bits of len properly
 * ordered as well. The type and flags can be extracted using bit shifts from
 * the word, and only one extra read is needed to fetch len[16:23].
 * Returns zero if some bytes are missing, otherwise non-zero on success. The
 * buffer is assumed not to contain any output data.
 */
static __maybe_unused int h2_peek_frame_hdr(const struct buffer *b, struct h2_fh *h)
{
	uint64_t w;

	if (b_data(b) < 9)
		return 0;

	w = h2_get_n64(b, 1);
	h->len = *(uint8_t*)b_head(b) << 16;
	h->sid = w & 0x7FFFFFFF; /* RFC7540#4.1: R bit must be ignored */
	h->ff = w >> 32;
	h->ft = w >> 40;
	h->len += w >> 48;
	return 1;
}

/* skip the next 9 bytes corresponding to the frame header possibly parsed by
 * h2_peek_frame_hdr() above.
 */
static inline __maybe_unused void h2_skip_frame_hdr(struct buffer *b)
{
	b_del(b, 9);
}

/* same as above, automatically advances the buffer on success */
static inline __maybe_unused int h2_get_frame_hdr(struct buffer *b, struct h2_fh *h)
{
	int ret;

	ret = h2_peek_frame_hdr(b, h);
	if (ret > 0)
		h2_skip_frame_hdr(b);
	return ret;
}

/* marks stream <h2s> as CLOSED and decrement the number of active streams for
 * its connection if the stream was not yet closed. Please use this exclusively
 * before closing a stream to ensure stream count is well maintained.
 */
static inline void h2s_close(struct h2s *h2s)
{
	if (h2s->st != H2_SS_CLOSED)
		h2s->h2c->nb_streams--;
	h2s->st = H2_SS_CLOSED;
}

/* detaches an H2 stream from its H2C and releases it to the H2S pool. */
static void h2s_destroy(struct h2s *h2s)
{
	h2s_close(h2s);
	eb32_delete(&h2s->by_id);
	if (b_size(&h2s->rxbuf)) {
		b_free(&h2s->rxbuf);
		offer_buffers(NULL, tasks_run_queue);
	}
	LIST_DEL(&h2s->wait_list.list);
	LIST_INIT(&h2s->wait_list.list);
	tasklet_free(h2s->wait_list.task);
	pool_free(pool_head_h2s, h2s);
}

/* creates a new stream <id> on the h2c connection and returns it, or NULL in
 * case of memory allocation error.
 */
static struct h2s *h2c_stream_new(struct h2c *h2c, int id)
{
	struct session *sess = h2c->conn->owner;
	struct conn_stream *cs;
	struct h2s *h2s;

	h2s = pool_alloc(pool_head_h2s);
	if (!h2s)
		goto out;

	h2s->wait_list.task = tasklet_new();
	if (!h2s->wait_list.task) {
		pool_free(pool_head_h2s, h2s);
		goto out;
	}
	LIST_INIT(&h2s->wait_list.list);
	h2s->recv_wait_list = NULL;
	h2s->wait_list.task->process = h2_deferred_shut;
	h2s->wait_list.task->context = h2s;
	h2s->wait_list.handle = NULL;
	h2s->wait_list.wait_reason = 0;
	h2s->h2c       = h2c;
	h2s->mws       = h2c->miw;
	h2s->flags     = H2_SF_NONE;
	h2s->errcode   = H2_ERR_NO_ERROR;
	h2s->st        = H2_SS_IDLE;
	h2s->status    = 0;
	h2s->rxbuf     = BUF_NULL;
	h1m_init_res(&h2s->h1m);
	h2s->h1m.err_pos = -1; // don't care about errors on the response path
	h2s->h1m.flags |= H1_MF_TOLOWER;
	h2s->by_id.key = h2s->id = id;
	h2c->max_id    = id;

	eb32_insert(&h2c->streams_by_id, &h2s->by_id);
	h2c->nb_streams++;
	if (h2c->nb_streams > h2_settings_max_concurrent_streams)
		goto out_close;

	cs = cs_new(h2c->conn);
	if (!cs)
		goto out_close;

	h2s->cs = cs;
	cs->ctx = h2s;
	h2c->nb_cs++;

	if (stream_create_from_cs(cs) < 0)
		goto out_free_cs;

	/* We want the accept date presented to the next stream to be the one
	 * we have now, the handshake time to be null (since the next stream
	 * is not delayed by a handshake), and the idle time to count since
	 * right now.
	 */
	sess->accept_date = date;
	sess->tv_accept   = now;
	sess->t_handshake = 0;

	/* OK done, the stream lives its own life now */
	if (h2_has_too_many_cs(h2c))
		h2c->flags |= H2_CF_DEM_TOOMANY;
	return h2s;

 out_free_cs:
	h2c->nb_cs--;
	cs_free(cs);
 out_close:
	h2s_destroy(h2s);
	h2s = NULL;
	sess_log(sess);
 out:
	return h2s;
}

/* try to send a settings frame on the connection. Returns > 0 on success, 0 if
 * it couldn't do anything. It may return an error in h2c. See RFC7540#11.3 for
 * the various settings codes.
 */
static int h2c_snd_settings(struct h2c *h2c)
{
	struct buffer *res;
	char buf_data[100]; // enough for 15 settings
	struct buffer buf;
	int ret;

	if (h2c_mux_busy(h2c, NULL)) {
		h2c->flags |= H2_CF_DEM_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2c->flags |= H2_CF_DEM_MROOM;
		return 0;
	}

	chunk_init(&buf, buf_data, sizeof(buf_data));
	chunk_memcpy(&buf,
	       "\x00\x00\x00"      /* length    : 0 for now */
	       "\x04\x00"          /* type      : 4 (settings), flags : 0 */
	       "\x00\x00\x00\x00", /* stream ID : 0 */
	       9);

	if (h2_settings_header_table_size != 4096) {
		char str[6] = "\x00\x01"; /* header_table_size */

		write_n32(str + 2, h2_settings_header_table_size);
		chunk_memcat(&buf, str, 6);
	}

	if (h2_settings_initial_window_size != 65535) {
		char str[6] = "\x00\x04"; /* initial_window_size */

		write_n32(str + 2, h2_settings_initial_window_size);
		chunk_memcat(&buf, str, 6);
	}

	if (h2_settings_max_concurrent_streams != 0) {
		char str[6] = "\x00\x03"; /* max_concurrent_streams */

		/* Note: 0 means "unlimited" for haproxy's config but not for
		 * the protocol, so never send this value!
		 */
		write_n32(str + 2, h2_settings_max_concurrent_streams);
		chunk_memcat(&buf, str, 6);
	}

	if (global.tune.bufsize != 16384) {
		char str[6] = "\x00\x05"; /* max_frame_size */

		/* note: similarly we could also emit MAX_HEADER_LIST_SIZE to
		 * match bufsize - rewrite size, but at the moment it seems
		 * that clients don't take care of it.
		 */
		write_n32(str + 2, global.tune.bufsize);
		chunk_memcat(&buf, str, 6);
	}

	h2_set_frame_size(buf.area, buf.data - 9);
	ret = b_istput(res, ist2(buf.area, buf.data));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			h2c->flags |= H2_CF_MUX_MFULL;
			h2c->flags |= H2_CF_DEM_MROOM;
			return 0;
		}
		else {
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			return 0;
		}
	}
	return ret;
}

/* Try to receive a connection preface, then upon success try to send our
 * preface which is a SETTINGS frame. Returns > 0 on success or zero on
 * missing data. It may return an error in h2c.
 */
static int h2c_frt_recv_preface(struct h2c *h2c)
{
	int ret1;
	int ret2;

	ret1 = b_isteq(&h2c->dbuf, 0, b_data(&h2c->dbuf), ist(H2_CONN_PREFACE));

	if (unlikely(ret1 <= 0)) {
		if (ret1 < 0)
			sess_log(h2c->conn->owner);

		if (ret1 < 0 || conn_xprt_read0_pending(h2c->conn))
			h2c_error(h2c, H2_ERR_PROTOCOL_ERROR);
		return 0;
	}

	ret2 = h2c_snd_settings(h2c);
	if (ret2 > 0)
		b_del(&h2c->dbuf, ret1);

	return ret2;
}

/* try to send a GOAWAY frame on the connection to report an error or a graceful
 * shutdown, with h2c->errcode as the error code. Returns > 0 on success or zero
 * if nothing was done. It uses h2c->last_sid as the advertised ID, or copies it
 * from h2c->max_id if it's not set yet (<0). In case of lack of room to write
 * the message, it subscribes the requester (either <h2s> or <h2c>) to future
 * notifications. It sets H2_CF_GOAWAY_SENT on success, and H2_CF_GOAWAY_FAILED
 * on unrecoverable failure. It will not attempt to send one again in this last
 * case so that it is safe to use h2c_error() to report such errors.
 */
static int h2c_send_goaway_error(struct h2c *h2c, struct h2s *h2s)
{
	struct buffer *res;
	char str[17];
	int ret;

	if (h2c->flags & H2_CF_GOAWAY_FAILED)
		return 1; // claim that it worked

	if (h2c_mux_busy(h2c, h2s)) {
		if (h2s)
			h2s->flags |= H2_SF_BLK_MBUSY;
		else
			h2c->flags |= H2_CF_DEM_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		if (h2s)
			h2s->flags |= H2_SF_BLK_MROOM;
		else
			h2c->flags |= H2_CF_DEM_MROOM;
		return 0;
	}

	/* len: 8, type: 7, flags: none, sid: 0 */
	memcpy(str, "\x00\x00\x08\x07\x00\x00\x00\x00\x00", 9);

	if (h2c->last_sid < 0)
		h2c->last_sid = h2c->max_id;

	write_n32(str + 9, h2c->last_sid);
	write_n32(str + 13, h2c->errcode);
	ret = b_istput(res, ist2(str, 17));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			h2c->flags |= H2_CF_MUX_MFULL;
			if (h2s)
				h2s->flags |= H2_SF_BLK_MROOM;
			else
				h2c->flags |= H2_CF_DEM_MROOM;
			return 0;
		}
		else {
			/* we cannot report this error using GOAWAY, so we mark
			 * it and claim a success.
			 */
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			h2c->flags |= H2_CF_GOAWAY_FAILED;
			return 1;
		}
	}
	h2c->flags |= H2_CF_GOAWAY_SENT;
	return ret;
}

/* Try to send an RST_STREAM frame on the connection for the indicated stream
 * during mux operations. This stream must be valid and cannot be closed
 * already. h2s->id will be used for the stream ID and h2s->errcode will be
 * used for the error code. h2s->st will be update to H2_SS_CLOSED if it was
 * not yet.
 *
 * Returns > 0 on success or zero if nothing was done. In case of lack of room
 * to write the message, it subscribes the stream to future notifications.
 */
static int h2s_send_rst_stream(struct h2c *h2c, struct h2s *h2s)
{
	struct buffer *res;
	char str[13];
	int ret;

	if (!h2s || h2s->st == H2_SS_CLOSED)
		return 1;

	/* RFC7540#5.4.2: To avoid looping, an endpoint MUST NOT send a
	 * RST_STREAM in response to a RST_STREAM frame.
	 */
	if (h2c->dft == H2_FT_RST_STREAM) {
		ret = 1;
		goto ignore;
	}

	if (h2c_mux_busy(h2c, h2s)) {
		h2s->flags |= H2_SF_BLK_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2s->flags |= H2_SF_BLK_MROOM;
		return 0;
	}

	/* len: 4, type: 3, flags: none */
	memcpy(str, "\x00\x00\x04\x03\x00", 5);
	write_n32(str + 5, h2s->id);
	write_n32(str + 9, h2s->errcode);
	ret = b_istput(res, ist2(str, 13));

	if (unlikely(ret <= 0)) {
		if (!ret) {
			h2c->flags |= H2_CF_MUX_MFULL;
			h2s->flags |= H2_SF_BLK_MROOM;
			return 0;
		}
		else {
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			return 0;
		}
	}

 ignore:
	h2s->flags |= H2_SF_RST_SENT;
	h2s_close(h2s);
	return ret;
}

/* Try to send an RST_STREAM frame on the connection for the stream being
 * demuxed using h2c->dsi for the stream ID. It will use h2s->errcode as the
 * error code unless the stream's state already is IDLE or CLOSED in which
 * case STREAM_CLOSED will be used, and will update h2s->st to H2_SS_CLOSED if
 * it was not yet.
 *
 * Returns > 0 on success or zero if nothing was done. In case of lack of room
 * to write the message, it blocks the demuxer and subscribes it to future
 * notifications. It's worth mentionning that an RST may even be sent for a
 * closed stream.
 */
static int h2c_send_rst_stream(struct h2c *h2c, struct h2s *h2s)
{
	struct buffer *res;
	char str[13];
	int ret;

	/* RFC7540#5.4.2: To avoid looping, an endpoint MUST NOT send a
	 * RST_STREAM in response to a RST_STREAM frame.
	 */
	if (h2c->dft == H2_FT_RST_STREAM) {
		ret = 1;
		goto ignore;
	}

	if (h2c_mux_busy(h2c, h2s)) {
		h2c->flags |= H2_CF_DEM_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2c->flags |= H2_CF_DEM_MROOM;
		return 0;
	}

	/* len: 4, type: 3, flags: none */
	memcpy(str, "\x00\x00\x04\x03\x00", 5);

	write_n32(str + 5, h2c->dsi);
	write_n32(str + 9, (h2s->st > H2_SS_IDLE && h2s->st < H2_SS_CLOSED) ?
		  h2s->errcode : H2_ERR_STREAM_CLOSED);
	ret = b_istput(res, ist2(str, 13));

	if (unlikely(ret <= 0)) {
		if (!ret) {
			h2c->flags |= H2_CF_MUX_MFULL;
			h2c->flags |= H2_CF_DEM_MROOM;
			return 0;
		}
		else {
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			return 0;
		}
	}

 ignore:
	if (h2s->st > H2_SS_IDLE && h2s->st < H2_SS_CLOSED) {
		h2s->flags |= H2_SF_RST_SENT;
		h2s_close(h2s);
	}

	return ret;
}

/* try to send an empty DATA frame with the ES flag set to notify about the
 * end of stream and match a shutdown(write). If an ES was already sent as
 * indicated by HLOC/ERROR/RESET/CLOSED states, nothing is done. Returns > 0
 * on success or zero if nothing was done. In case of lack of room to write the
 * message, it subscribes the requesting stream to future notifications.
 */
static int h2_send_empty_data_es(struct h2s *h2s)
{
	struct h2c *h2c = h2s->h2c;
	struct buffer *res;
	char str[9];
	int ret;

	if (h2s->st == H2_SS_HLOC || h2s->st == H2_SS_ERROR || h2s->st == H2_SS_CLOSED)
		return 1;

	if (h2c_mux_busy(h2c, h2s)) {
		h2s->flags |= H2_SF_BLK_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2s->flags |= H2_SF_BLK_MROOM;
		return 0;
	}

	/* len: 0x000000, type: 0(DATA), flags: ES=1 */
	memcpy(str, "\x00\x00\x00\x00\x01", 5);
	write_n32(str + 5, h2s->id);
	ret = b_istput(res, ist2(str, 9));
	if (likely(ret > 0)) {
		h2s->flags |= H2_SF_ES_SENT;
	}
	else if (!ret) {
		h2c->flags |= H2_CF_MUX_MFULL;
		h2s->flags |= H2_SF_BLK_MROOM;
		return 0;
	}
	else {
		h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
		return 0;
	}
	return ret;
}

/* wake the streams attached to the connection, whose id is greater than <last>,
 * and assign their conn_stream the CS_FL_* flags <flags> in addition to
 * CS_FL_ERROR in case of error and CS_FL_REOS in case of closed connection.
 * The stream's state is automatically updated accordingly.
 */
static void h2_wake_some_streams(struct h2c *h2c, int last, uint32_t flags)
{
	struct eb32_node *node;
	struct h2s *h2s;

	if (h2c->st0 >= H2_CS_ERROR || h2c->conn->flags & CO_FL_ERROR)
		flags |= CS_FL_ERROR;

	if (conn_xprt_read0_pending(h2c->conn))
		flags |= CS_FL_REOS;

	node = eb32_lookup_ge(&h2c->streams_by_id, last + 1);
	while (node) {
		h2s = container_of(node, struct h2s, by_id);
		if (h2s->id <= last)
			break;
		node = eb32_next(node);

		if (!h2s->cs) {
			/* this stream was already orphaned */
			h2s_destroy(h2s);
			continue;
		}

		h2s->cs->flags |= flags;
		if (h2s->recv_wait_list) {
			struct wait_list *sw = h2s->recv_wait_list;
			sw->wait_reason &= ~SUB_CAN_RECV;
			tasklet_wakeup(sw->task);
			h2s->recv_wait_list = NULL;
		} else if (h2s->cs->data_cb->wake != NULL)
			h2s->cs->data_cb->wake(h2s->cs);

		if (flags & CS_FL_ERROR && h2s->st < H2_SS_ERROR)
			h2s->st = H2_SS_ERROR;
		else if (flags & CS_FL_REOS && h2s->st == H2_SS_OPEN)
			h2s->st = H2_SS_HREM;
		else if (flags & CS_FL_REOS && h2s->st == H2_SS_HLOC)
			h2s_close(h2s);
	}
}

/* Increase all streams' outgoing window size by the difference passed in
 * argument. This is needed upon receipt of the settings frame if the initial
 * window size is different. The difference may be negative and the resulting
 * window size as well, for the time it takes to receive some window updates.
 */
static void h2c_update_all_ws(struct h2c *h2c, int diff)
{
	struct h2s *h2s;
	struct eb32_node *node;

	if (!diff)
		return;

	node = eb32_first(&h2c->streams_by_id);
	while (node) {
		h2s = container_of(node, struct h2s, by_id);
		h2s->mws += diff;
		node = eb32_next(node);
	}
}

/* processes a SETTINGS frame whose payload is <payload> for <plen> bytes, and
 * ACKs it if needed. Returns > 0 on success or zero on missing data. It may
 * return an error in h2c. Described in RFC7540#6.5.
 */
static int h2c_handle_settings(struct h2c *h2c)
{
	unsigned int offset;
	int error;

	if (h2c->dff & H2_F_SETTINGS_ACK) {
		if (h2c->dfl) {
			error = H2_ERR_FRAME_SIZE_ERROR;
			goto fail;
		}
		return 1;
	}

	if (h2c->dsi != 0) {
		error = H2_ERR_PROTOCOL_ERROR;
		goto fail;
	}

	if (h2c->dfl % 6) {
		error = H2_ERR_FRAME_SIZE_ERROR;
		goto fail;
	}

	/* that's the limit we can process */
	if (h2c->dfl > global.tune.bufsize) {
		error = H2_ERR_FRAME_SIZE_ERROR;
		goto fail;
	}

	/* process full frame only */
	if (b_data(&h2c->dbuf) < h2c->dfl)
		return 0;

	/* parse the frame */
	for (offset = 0; offset < h2c->dfl; offset += 6) {
		uint16_t type = h2_get_n16(&h2c->dbuf, offset);
		int32_t  arg  = h2_get_n32(&h2c->dbuf, offset + 2);

		switch (type) {
		case H2_SETTINGS_INITIAL_WINDOW_SIZE:
			/* we need to update all existing streams with the
			 * difference from the previous iws.
			 */
			if (arg < 0) { // RFC7540#6.5.2
				error = H2_ERR_FLOW_CONTROL_ERROR;
				goto fail;
			}
			h2c_update_all_ws(h2c, arg - h2c->miw);
			h2c->miw = arg;
			break;
		case H2_SETTINGS_MAX_FRAME_SIZE:
			if (arg < 16384 || arg > 16777215) { // RFC7540#6.5.2
				error = H2_ERR_PROTOCOL_ERROR;
				goto fail;
			}
			h2c->mfs = arg;
			break;
		case H2_SETTINGS_ENABLE_PUSH:
			if (arg < 0 || arg > 1) { // RFC7540#6.5.2
				error = H2_ERR_PROTOCOL_ERROR;
				goto fail;
			}
			break;
		}
	}

	/* need to ACK this frame now */
	h2c->st0 = H2_CS_FRAME_A;
	return 1;
 fail:
	sess_log(h2c->conn->owner);
	h2c_error(h2c, error);
	return 0;
}

/* try to send an ACK for a settings frame on the connection. Returns > 0 on
 * success or one of the h2_status values.
 */
static int h2c_ack_settings(struct h2c *h2c)
{
	struct buffer *res;
	char str[9];
	int ret = -1;

	if (h2c_mux_busy(h2c, NULL)) {
		h2c->flags |= H2_CF_DEM_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2c->flags |= H2_CF_DEM_MROOM;
		return 0;
	}

	memcpy(str,
	       "\x00\x00\x00"     /* length : 0 (no data)  */
	       "\x04" "\x01"      /* type   : 4, flags : ACK */
	       "\x00\x00\x00\x00" /* stream ID */, 9);

	ret = b_istput(res, ist2(str, 9));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			h2c->flags |= H2_CF_MUX_MFULL;
			h2c->flags |= H2_CF_DEM_MROOM;
			return 0;
		}
		else {
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			return 0;
		}
	}
	return ret;
}

/* processes a PING frame and schedules an ACK if needed. The caller must pass
 * the pointer to the payload in <payload>. Returns > 0 on success or zero on
 * missing data. It may return an error in h2c.
 */
static int h2c_handle_ping(struct h2c *h2c)
{
	/* frame length must be exactly 8 */
	if (h2c->dfl != 8) {
		h2c_error(h2c, H2_ERR_FRAME_SIZE_ERROR);
		return 0;
	}

	/* schedule a response */
	if (!(h2c->dff & H2_F_PING_ACK))
		h2c->st0 = H2_CS_FRAME_A;
	return 1;
}

/* Try to send a window update for stream id <sid> and value <increment>.
 * Returns > 0 on success or zero on missing room or failure. It may return an
 * error in h2c.
 */
static int h2c_send_window_update(struct h2c *h2c, int sid, uint32_t increment)
{
	struct buffer *res;
	char str[13];
	int ret = -1;

	if (h2c_mux_busy(h2c, NULL)) {
		h2c->flags |= H2_CF_DEM_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2c->flags |= H2_CF_DEM_MROOM;
		return 0;
	}

	/* length: 4, type: 8, flags: none */
	memcpy(str, "\x00\x00\x04\x08\x00", 5);
	write_n32(str + 5, sid);
	write_n32(str + 9, increment);

	ret = b_istput(res, ist2(str, 13));

	if (unlikely(ret <= 0)) {
		if (!ret) {
			h2c->flags |= H2_CF_MUX_MFULL;
			h2c->flags |= H2_CF_DEM_MROOM;
			return 0;
		}
		else {
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			return 0;
		}
	}
	return ret;
}

/* try to send pending window update for the connection. It's safe to call it
 * with no pending updates. Returns > 0 on success or zero on missing room or
 * failure. It may return an error in h2c.
 */
static int h2c_send_conn_wu(struct h2c *h2c)
{
	int ret = 1;

	if (h2c->rcvd_c <= 0)
		return 1;

	/* send WU for the connection */
	ret = h2c_send_window_update(h2c, 0, h2c->rcvd_c);
	if (ret > 0)
		h2c->rcvd_c = 0;

	return ret;
}

/* try to send pending window update for the current dmux stream. It's safe to
 * call it with no pending updates. Returns > 0 on success or zero on missing
 * room or failure. It may return an error in h2c.
 */
static int h2c_send_strm_wu(struct h2c *h2c)
{
	int ret = 1;

	if (h2c->rcvd_s <= 0)
		return 1;

	/* send WU for the stream */
	ret = h2c_send_window_update(h2c, h2c->dsi, h2c->rcvd_s);
	if (ret > 0)
		h2c->rcvd_s = 0;

	return ret;
}

/* try to send an ACK for a ping frame on the connection. Returns > 0 on
 * success, 0 on missing data or one of the h2_status values.
 */
static int h2c_ack_ping(struct h2c *h2c)
{
	struct buffer *res;
	char str[17];
	int ret = -1;

	if (b_data(&h2c->dbuf) < 8)
		return 0;

	if (h2c_mux_busy(h2c, NULL)) {
		h2c->flags |= H2_CF_DEM_MBUSY;
		return 0;
	}

	res = h2_get_buf(h2c, &h2c->mbuf);
	if (!res) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2c->flags |= H2_CF_DEM_MROOM;
		return 0;
	}

	memcpy(str,
	       "\x00\x00\x08"     /* length : 8 (same payload) */
	       "\x06" "\x01"      /* type   : 6, flags : ACK   */
	       "\x00\x00\x00\x00" /* stream ID */, 9);

	/* copy the original payload */
	h2_get_buf_bytes(str + 9, 8, &h2c->dbuf, 0);

	ret = b_istput(res, ist2(str, 17));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			h2c->flags |= H2_CF_MUX_MFULL;
			h2c->flags |= H2_CF_DEM_MROOM;
			return 0;
		}
		else {
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			return 0;
		}
	}
	return ret;
}

/* processes a WINDOW_UPDATE frame whose payload is <payload> for <plen> bytes.
 * Returns > 0 on success or zero on missing data. It may return an error in
 * h2c or h2s. Described in RFC7540#6.9.
 */
static int h2c_handle_window_update(struct h2c *h2c, struct h2s *h2s)
{
	int32_t inc;
	int error;

	if (h2c->dfl != 4) {
		error = H2_ERR_FRAME_SIZE_ERROR;
		goto conn_err;
	}

	/* process full frame only */
	if (b_data(&h2c->dbuf) < h2c->dfl)
		return 0;

	inc = h2_get_n32(&h2c->dbuf, 0);

	if (h2c->dsi != 0) {
		/* stream window update */

		/* it's not an error to receive WU on a closed stream */
		if (h2s->st == H2_SS_CLOSED)
			return 1;

		if (!inc) {
			error = H2_ERR_PROTOCOL_ERROR;
			goto strm_err;
		}

		if (h2s->mws >= 0 && h2s->mws + inc < 0) {
			error = H2_ERR_FLOW_CONTROL_ERROR;
			goto strm_err;
		}

		h2s->mws += inc;
		if (h2s->mws > 0 && (h2s->flags & H2_SF_BLK_SFCTL)) {
			h2s->flags &= ~H2_SF_BLK_SFCTL;
			/* The task will be waken up later */
		}
	}
	else {
		/* connection window update */
		if (!inc) {
			error = H2_ERR_PROTOCOL_ERROR;
			goto conn_err;
		}

		if (h2c->mws >= 0 && h2c->mws + inc < 0) {
			error = H2_ERR_FLOW_CONTROL_ERROR;
			goto conn_err;
		}

		h2c->mws += inc;
	}

	return 1;

 conn_err:
	h2c_error(h2c, error);
	return 0;

 strm_err:
	if (h2s) {
		h2s_error(h2s, error);
		h2c->st0 = H2_CS_FRAME_E;
	}
	else
		h2c_error(h2c, error);
	return 0;
}

/* processes a GOAWAY frame, and signals all streams whose ID is greater than
 * the last ID. Returns > 0 on success or zero on missing data. It may return
 * an error in h2c. Described in RFC7540#6.8.
 */
static int h2c_handle_goaway(struct h2c *h2c)
{
	int error;
	int last;

	if (h2c->dsi != 0) {
		error = H2_ERR_PROTOCOL_ERROR;
		goto conn_err;
	}

	if (h2c->dfl < 8) {
		error = H2_ERR_FRAME_SIZE_ERROR;
		goto conn_err;
	}

	/* process full frame only */
	if (b_data(&h2c->dbuf) < h2c->dfl)
		return 0;

	last = h2_get_n32(&h2c->dbuf, 0);
	h2c->errcode = h2_get_n32(&h2c->dbuf, 4);
	h2_wake_some_streams(h2c, last, CS_FL_ERROR);
	if (h2c->last_sid < 0)
		h2c->last_sid = last;
	return 1;

 conn_err:
	h2c_error(h2c, error);
	return 0;
}

/* processes a PRIORITY frame, and either skips it or rejects if it is
 * invalid. Returns > 0 on success or zero on missing data. It may return
 * an error in h2c. Described in RFC7540#6.3.
 */
static int h2c_handle_priority(struct h2c *h2c)
{
	int error;

	if (h2c->dsi == 0) {
		error = H2_ERR_PROTOCOL_ERROR;
		goto conn_err;
	}

	if (h2c->dfl != 5) {
		error = H2_ERR_FRAME_SIZE_ERROR;
		goto conn_err;
	}

	/* process full frame only */
	if (b_data(&h2c->dbuf) < h2c->dfl)
		return 0;

	if (h2_get_n32(&h2c->dbuf, 0) == h2c->dsi) {
		/* 7540#5.3 : can't depend on itself */
		error = H2_ERR_PROTOCOL_ERROR;
		goto conn_err;
	}
	return 1;

 conn_err:
	h2c_error(h2c, error);
	return 0;
}

/* processes an RST_STREAM frame, and sets the 32-bit error code on the stream.
 * Returns > 0 on success or zero on missing data. It may return an error in
 * h2c. Described in RFC7540#6.4.
 */
static int h2c_handle_rst_stream(struct h2c *h2c, struct h2s *h2s)
{
	int error;

	if (h2c->dsi == 0) {
		error = H2_ERR_PROTOCOL_ERROR;
		goto conn_err;
	}

	if (h2c->dfl != 4) {
		error = H2_ERR_FRAME_SIZE_ERROR;
		goto conn_err;
	}

	/* process full frame only */
	if (b_data(&h2c->dbuf) < h2c->dfl)
		return 0;

	/* late RST, already handled */
	if (h2s->st == H2_SS_CLOSED)
		return 1;

	h2s->errcode = h2_get_n32(&h2c->dbuf, 0);
	h2s_close(h2s);

	if (h2s->cs) {
		h2s->cs->flags |= CS_FL_REOS | CS_FL_ERROR;
		if (h2s->recv_wait_list) {
			struct wait_list *sw = h2s->recv_wait_list;

			sw->wait_reason &= ~SUB_CAN_RECV;
			tasklet_wakeup(sw->task);
			h2s->recv_wait_list = NULL;
		}
	}

	h2s->flags |= H2_SF_RST_RCVD;
	return 1;

 conn_err:
	h2c_error(h2c, error);
	return 0;
}

/* processes a HEADERS frame. Returns h2s on success or NULL on missing data.
 * It may return an error in h2c or h2s. The caller must consider that the
 * return value is the new h2s in case one was allocated (most common case).
 * Described in RFC7540#6.2. Most of the
 * errors here are reported as connection errors since it's impossible to
 * recover from such errors after the compression context has been altered.
 */
static struct h2s *h2c_frt_handle_headers(struct h2c *h2c, struct h2s *h2s)
{
	int error;

	if (!h2c->dfl) {
		error = H2_ERR_PROTOCOL_ERROR; // empty headers frame!
		sess_log(h2c->conn->owner);
		goto strm_err;
	}

	if (!b_size(&h2c->dbuf))
		return NULL; // empty buffer

	if (b_data(&h2c->dbuf) < h2c->dfl && !b_full(&h2c->dbuf))
		return NULL; // incomplete frame

	if (h2c->flags & H2_CF_DEM_TOOMANY)
		return 0; // too many cs still present

	/* now either the frame is complete or the buffer is complete */
	if (h2s->st != H2_SS_IDLE) {
		/* FIXME: stream already exists, this is only allowed for
		 * trailers (not supported for now).
		 */
		error = H2_ERR_PROTOCOL_ERROR;
		sess_log(h2c->conn->owner);
		goto conn_err;
	}
	else if (h2c->dsi <= h2c->max_id || !(h2c->dsi & 1)) {
		/* RFC7540#5.1.1 stream id > prev ones, and must be odd here */
		error = H2_ERR_PROTOCOL_ERROR;
		sess_log(h2c->conn->owner);
		goto conn_err;
	}

	/* Note: we don't emit any other logs below because ff we return
	 * positively from h2c_stream_new(), the stream will report the error,
	 * and if we return in error, h2c_stream_new() will emit the error.
	 */
	h2s = h2c_stream_new(h2c, h2c->dsi);
	if (!h2s) {
		error = H2_ERR_INTERNAL_ERROR;
		goto conn_err;
	}

	h2s->st = H2_SS_OPEN;
	if (h2c->dff & H2_F_HEADERS_END_STREAM) {
		h2s->st = H2_SS_HREM;
		h2s->flags |= H2_SF_ES_RCVD;
		/* note: cs cannot be null for now (just created above) */
		h2s->cs->flags |= CS_FL_REOS;
	}

	if (!h2_frt_decode_headers(h2s))
		return NULL;

	if (h2c->st0 >= H2_CS_ERROR)
		return NULL;

	if (h2s->st >= H2_SS_ERROR) {
		/* stream error : send RST_STREAM */
		h2c->st0 = H2_CS_FRAME_E;
	}
	else {
		/* update the max stream ID if the request is being processed */
		if (h2s->id > h2c->max_id)
			h2c->max_id = h2s->id;
	}

	return h2s;

 conn_err:
	h2c_error(h2c, error);
	return NULL;

 strm_err:
	if (h2s) {
		h2s_error(h2s, error);
		h2c->st0 = H2_CS_FRAME_E;
	}
	else
		h2c_error(h2c, error);
	return NULL;
}

/* processes a DATA frame. Returns > 0 on success or zero on missing data.
 * It may return an error in h2c or h2s. Described in RFC7540#6.1.
 */
static int h2c_frt_handle_data(struct h2c *h2c, struct h2s *h2s)
{
	int error;

	/* note that empty DATA frames are perfectly valid and sometimes used
	 * to signal an end of stream (with the ES flag).
	 */

	if (!b_size(&h2c->dbuf) && h2c->dfl)
		return 0; // empty buffer

	if (b_data(&h2c->dbuf) < h2c->dfl && !b_full(&h2c->dbuf))
		return 0; // incomplete frame

	/* now either the frame is complete or the buffer is complete */

	if (!h2c->dsi) {
		/* RFC7540#6.1 */
		error = H2_ERR_PROTOCOL_ERROR;
		goto conn_err;
	}

	if (h2s->st != H2_SS_OPEN && h2s->st != H2_SS_HLOC) {
		/* RFC7540#6.1 */
		error = H2_ERR_STREAM_CLOSED;
		goto strm_err;
	}

	if (!h2_frt_transfer_data(h2s))
		return 0;

	/* call the upper layers to process the frame, then let the upper layer
	 * notify the stream about any change.
	 */
	if (!h2s->cs) {
		error = H2_ERR_STREAM_CLOSED;
		goto strm_err;
	}

	if (h2c->st0 >= H2_CS_ERROR)
		return 0;

	if (h2s->st >= H2_SS_ERROR) {
		/* stream error : send RST_STREAM */
		h2c->st0 = H2_CS_FRAME_E;
	}

	/* check for completion : the callee will change this to FRAME_A or
	 * FRAME_H once done.
	 */
	if (h2c->st0 == H2_CS_FRAME_P)
		return 0;


	/* last frame */
	if (h2c->dff & H2_F_DATA_END_STREAM) {
		h2s->st = H2_SS_HREM;
		h2s->flags |= H2_SF_ES_RCVD;
		h2s->cs->flags |= CS_FL_REOS;
	}

	return 1;

 conn_err:
	h2c_error(h2c, error);
	return 0;

 strm_err:
	if (h2s) {
		h2s_error(h2s, error);
		h2c->st0 = H2_CS_FRAME_E;
	}
	else
		h2c_error(h2c, error);
	return 0;
}

/* process Rx frames to be demultiplexed */
static void h2_process_demux(struct h2c *h2c)
{
	struct h2s *h2s = NULL, *tmp_h2s;

	if (h2c->st0 >= H2_CS_ERROR)
		return;

	if (unlikely(h2c->st0 < H2_CS_FRAME_H)) {
		if (h2c->st0 == H2_CS_PREFACE) {
			if (unlikely(h2c_frt_recv_preface(h2c) <= 0)) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				if (h2c->st0 == H2_CS_ERROR) {
					h2c->st0 = H2_CS_ERROR2;
					sess_log(h2c->conn->owner);
				}
				goto fail;
			}

			h2c->max_id = 0;
			h2c->st0 = H2_CS_SETTINGS1;
		}

		if (h2c->st0 == H2_CS_SETTINGS1) {
			struct h2_fh hdr;

			/* ensure that what is pending is a valid SETTINGS frame
			 * without an ACK.
			 */
			if (!h2_get_frame_hdr(&h2c->dbuf, &hdr)) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				if (h2c->st0 == H2_CS_ERROR) {
					h2c->st0 = H2_CS_ERROR2;
					sess_log(h2c->conn->owner);
				}
				goto fail;
			}

			if (hdr.sid || hdr.ft != H2_FT_SETTINGS || hdr.ff & H2_F_SETTINGS_ACK) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				h2c_error(h2c, H2_ERR_PROTOCOL_ERROR);
				h2c->st0 = H2_CS_ERROR2;
				sess_log(h2c->conn->owner);
				goto fail;
			}

			if ((int)hdr.len < 0 || (int)hdr.len > global.tune.bufsize) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				h2c_error(h2c, H2_ERR_FRAME_SIZE_ERROR);
				h2c->st0 = H2_CS_ERROR2;
				sess_log(h2c->conn->owner);
				goto fail;
			}

			/* that's OK, switch to FRAME_P to process it */
			h2c->dfl = hdr.len;
			h2c->dsi = hdr.sid;
			h2c->dft = hdr.ft;
			h2c->dff = hdr.ff;
			h2c->dpl = 0;
			h2c->st0 = H2_CS_FRAME_P;
		}
	}

	/* process as many incoming frames as possible below */
	while (b_data(&h2c->dbuf)) {
		int ret = 0;

		if (h2c->st0 >= H2_CS_ERROR)
			break;

		if (h2c->st0 == H2_CS_FRAME_H) {
			struct h2_fh hdr;

			if (!h2_peek_frame_hdr(&h2c->dbuf, &hdr))
				break;

			if ((int)hdr.len < 0 || (int)hdr.len > global.tune.bufsize) {
				h2c_error(h2c, H2_ERR_FRAME_SIZE_ERROR);
				h2c->st0 = H2_CS_ERROR;
				if (!h2c->nb_streams) {
					/* only log if no other stream can report the error */
					sess_log(h2c->conn->owner);
				}
				break;
			}

			h2c->dfl = hdr.len;
			h2c->dsi = hdr.sid;
			h2c->dft = hdr.ft;
			h2c->dff = hdr.ff;
			h2c->dpl = 0;
			h2c->st0 = H2_CS_FRAME_P;
			h2_skip_frame_hdr(&h2c->dbuf);
		}

		/* Only H2_CS_FRAME_P and H2_CS_FRAME_A here */
		tmp_h2s = h2c_st_by_id(h2c, h2c->dsi);

		if (tmp_h2s != h2s && h2s && h2s->cs && b_data(&h2s->rxbuf)) {
			/* we may have to signal the upper layers */
			h2s->cs->flags |= CS_FL_RCV_MORE;
			if (h2s->recv_wait_list) {
				h2s->recv_wait_list->wait_reason &= ~SUB_CAN_RECV;
				tasklet_wakeup(h2s->recv_wait_list->task);
				h2s->recv_wait_list = NULL;
			}
			if (h2c->st0 >= H2_CS_ERROR)
				goto strm_err;

			if (h2s->st >= H2_SS_ERROR) {
				/* stream error : send RST_STREAM */
				h2c->st0 = H2_CS_FRAME_E;
			}
		}
		h2s = tmp_h2s;

		if (h2c->st0 == H2_CS_FRAME_E)
			goto strm_err;

		if (h2s->st == H2_SS_IDLE &&
		    h2c->dft != H2_FT_HEADERS && h2c->dft != H2_FT_PRIORITY) {
			/* RFC7540#5.1: any frame other than HEADERS or PRIORITY in
			 * this state MUST be treated as a connection error
			 */
			h2c_error(h2c, H2_ERR_PROTOCOL_ERROR);
			h2c->st0 = H2_CS_ERROR;
			if (!h2c->nb_streams) {
				/* only log if no other stream can report the error */
				sess_log(h2c->conn->owner);
			}
			break;
		}

		if (h2s->st == H2_SS_HREM && h2c->dft != H2_FT_WINDOW_UPDATE &&
		    h2c->dft != H2_FT_RST_STREAM && h2c->dft != H2_FT_PRIORITY) {
			/* RFC7540#5.1: any frame other than WU/PRIO/RST in
			 * this state MUST be treated as a stream error
			 */
			h2s_error(h2s, H2_ERR_STREAM_CLOSED);
			goto strm_err;
		}

		/* Below the management of frames received in closed state is a
		 * bit hackish because the spec makes strong differences between
		 * streams closed by receiving RST, sending RST, and seeing ES
		 * in both directions. In addition to this, the creation of a
		 * new stream reusing the identifier of a closed one will be
		 * detected here. Given that we cannot keep track of all closed
		 * streams forever, we consider that unknown closed streams were
		 * closed on RST received, which allows us to respond with an
		 * RST without breaking the connection (eg: to abort a transfer).
		 * Some frames have to be silently ignored as well.
		 */
		if (h2s->st == H2_SS_CLOSED && h2c->dsi) {
			if (h2c->dft == H2_FT_HEADERS || h2c->dft == H2_FT_PUSH_PROMISE) {
				/* #5.1.1: The identifier of a newly
				 * established stream MUST be numerically
				 * greater than all streams that the initiating
				 * endpoint has opened or reserved. This
				 * governs streams that are opened using a
				 * HEADERS frame and streams that are reserved
				 * using PUSH_PROMISE. An endpoint that
				 * receives an unexpected stream identifier
				 * MUST respond with a connection error.
				 */
				h2c_error(h2c, H2_ERR_STREAM_CLOSED);
				goto strm_err;
			}

			if (h2s->flags & H2_SF_RST_RCVD) {
				/* RFC7540#5.1:closed: an endpoint that
				 * receives any frame other than PRIORITY after
				 * receiving a RST_STREAM MUST treat that as a
				 * stream error of type STREAM_CLOSED.
				 *
				 * Note that old streams fall into this category
				 * and will lead to an RST being sent.
				 */
				h2s_error(h2s, H2_ERR_STREAM_CLOSED);
				h2c->st0 = H2_CS_FRAME_E;
				goto strm_err;
			}

			/* RFC7540#5.1:closed: if this state is reached as a
			 * result of sending a RST_STREAM frame, the peer that
			 * receives the RST_STREAM might have already sent
			 * frames on the stream that cannot be withdrawn. An
			 * endpoint MUST ignore frames that it receives on
			 * closed streams after it has sent a RST_STREAM
			 * frame. An endpoint MAY choose to limit the period
			 * over which it ignores frames and treat frames that
			 * arrive after this time as being in error.
			 */
			if (!(h2s->flags & H2_SF_RST_SENT)) {
				/* RFC7540#5.1:closed: any frame other than
				 * PRIO/WU/RST in this state MUST be treated as
				 * a connection error
				 */
				if (h2c->dft != H2_FT_RST_STREAM &&
				    h2c->dft != H2_FT_PRIORITY &&
				    h2c->dft != H2_FT_WINDOW_UPDATE) {
					h2c_error(h2c, H2_ERR_STREAM_CLOSED);
					goto strm_err;
				}
			}
		}

#if 0
		// problem below: it is not possible to completely ignore such
		// streams as we need to maintain the compression state as well
		// and for this we need to completely process these frames (eg:
		// HEADERS frames) as well as counting DATA frames to emit
		// proper WINDOW UPDATES and ensure the connection doesn't stall.
		// This is a typical case of layer violation where the
		// transported contents are critical to the connection's
		// validity and must be ignored at the same time :-(

		/* graceful shutdown, ignore streams whose ID is higher than
		 * the one advertised in GOAWAY. RFC7540#6.8.
		 */
		if (unlikely(h2c->last_sid >= 0) && h2c->dsi > h2c->last_sid) {
			ret = MIN(b_data(&h2c->dbuf), h2c->dfl);
			b_del(&h2c->dbuf, ret);
			h2c->dfl -= ret;
			ret = h2c->dfl == 0;
			goto strm_err;
		}
#endif

		switch (h2c->dft) {
		case H2_FT_SETTINGS:
			if (h2c->st0 == H2_CS_FRAME_P)
				ret = h2c_handle_settings(h2c);

			if (h2c->st0 == H2_CS_FRAME_A)
				ret = h2c_ack_settings(h2c);
			break;

		case H2_FT_PING:
			if (h2c->st0 == H2_CS_FRAME_P)
				ret = h2c_handle_ping(h2c);

			if (h2c->st0 == H2_CS_FRAME_A)
				ret = h2c_ack_ping(h2c);
			break;

		case H2_FT_WINDOW_UPDATE:
			if (h2c->st0 == H2_CS_FRAME_P)
				ret = h2c_handle_window_update(h2c, h2s);
			break;

		case H2_FT_CONTINUATION:
			/* we currently don't support CONTINUATION frames since
			 * we have nowhere to store the partial HEADERS frame.
			 * Let's abort the stream on an INTERNAL_ERROR here.
			 */
			if (h2c->st0 == H2_CS_FRAME_P) {
				h2s_error(h2s, H2_ERR_INTERNAL_ERROR);
				h2c->st0 = H2_CS_FRAME_E;
			}
			break;

		case H2_FT_HEADERS:
			if (h2c->st0 == H2_CS_FRAME_P) {
				tmp_h2s = h2c_frt_handle_headers(h2c, h2s);
				if (tmp_h2s) {
					h2s = tmp_h2s;
					ret = 1;
				}
			}
			break;

		case H2_FT_DATA:
			if (h2c->st0 == H2_CS_FRAME_P)
				ret = h2c_frt_handle_data(h2c, h2s);

			if (h2c->st0 == H2_CS_FRAME_A)
				ret = h2c_send_strm_wu(h2c);
			break;

		case H2_FT_PRIORITY:
			if (h2c->st0 == H2_CS_FRAME_P)
				ret = h2c_handle_priority(h2c);
			break;

		case H2_FT_RST_STREAM:
			if (h2c->st0 == H2_CS_FRAME_P)
				ret = h2c_handle_rst_stream(h2c, h2s);
			break;

		case H2_FT_GOAWAY:
			if (h2c->st0 == H2_CS_FRAME_P)
				ret = h2c_handle_goaway(h2c);
			break;

		case H2_FT_PUSH_PROMISE:
			/* not permitted here, RFC7540#5.1 */
			h2c_error(h2c, H2_ERR_PROTOCOL_ERROR);
			if (!h2c->nb_streams) {
				/* only log if no other stream can report the error */
				sess_log(h2c->conn->owner);
			}
			break;

			/* implement all extra frame types here */
		default:
			/* drop frames that we ignore. They may be larger than
			 * the buffer so we drain all of their contents until
			 * we reach the end.
			 */
			ret = MIN(b_data(&h2c->dbuf), h2c->dfl);
			b_del(&h2c->dbuf, ret);
			h2c->dfl -= ret;
			ret = h2c->dfl == 0;
		}

	strm_err:
		/* We may have to send an RST if not done yet */
		if (h2s->st == H2_SS_ERROR)
			h2c->st0 = H2_CS_FRAME_E;

		if (h2c->st0 == H2_CS_FRAME_E)
			ret = h2c_send_rst_stream(h2c, h2s);

		/* error or missing data condition met above ? */
		if (ret <= 0) {
			h2s = NULL;
			break;
		}

		if (h2c->st0 != H2_CS_FRAME_H) {
			b_del(&h2c->dbuf, h2c->dfl);
			h2c->st0 = H2_CS_FRAME_H;
		}
	}

	if (h2c->rcvd_c > 0 &&
	    !(h2c->flags & (H2_CF_MUX_MFULL | H2_CF_DEM_MBUSY | H2_CF_DEM_MROOM)))
		h2c_send_conn_wu(h2c);

 fail:
	/* we can go here on missing data, blocked response or error */
	if (h2s && h2s->cs && b_data(&h2s->rxbuf)) {
		/* we may have to signal the upper layers */
		h2s->cs->flags |= CS_FL_RCV_MORE;
		if (h2s->recv_wait_list) {
				h2s->recv_wait_list->wait_reason &= ~SUB_CAN_RECV;
				tasklet_wakeup(h2s->recv_wait_list->task);
				h2s->recv_wait_list = NULL;
		}
	}
	return;
}

/* process Tx frames from streams to be multiplexed. Returns > 0 if it reached
 * the end.
 */
static int h2_process_mux(struct h2c *h2c)
{
	struct h2s *h2s;
	struct wait_list *sw, *sw_back;

	/* start by sending possibly pending window updates */
	if (h2c->rcvd_c > 0 &&
	    !(h2c->flags & (H2_CF_MUX_MFULL | H2_CF_MUX_MALLOC)) &&
	    h2c_send_conn_wu(h2c) < 0)
		goto fail;

	/* First we always process the flow control list because the streams
	 * waiting there were already elected for immediate emission but were
	 * blocked just on this.
	 */

	list_for_each_entry_safe(sw, sw_back, &h2c->fctl_list, list) {
		h2s = sw->handle;
		if (h2c->mws <= 0 || h2c->flags & H2_CF_MUX_BLOCK_ANY ||
		    h2c->st0 >= H2_CS_ERROR)
			break;

		/* If the tasklet was added to finish shutr/shutw, just wake the task */
		if ((long)(h2s) & 3) {
			sw->wait_reason &= ~SUB_CAN_SEND;
			LIST_DEL(&sw->list);
			LIST_INIT(&sw->list);
			tasklet_wakeup(sw->task);
		} else if (!(h2s->flags & H2_SF_BLK_SFCTL)) {
			h2s->flags &= ~H2_SF_BLK_ANY;
			LIST_DEL(&sw->list);
			LIST_INIT(&sw->list);
			sw->wait_reason &= ~SUB_CAN_SEND;
			tasklet_wakeup(sw->task);
		}
	}

	list_for_each_entry_safe(sw, sw_back, &h2c->send_list, list) {
		h2s = sw->handle;

		if (h2c->st0 >= H2_CS_ERROR || h2c->flags & H2_CF_MUX_BLOCK_ANY)
			break;

		/* If the tasklet was added to finish shutr/shutw, just wake the task */
		if ((long)(h2s) & 3) {
			sw->wait_reason &= ~SUB_CAN_SEND;
			LIST_DEL(&sw->list);
			LIST_INIT(&sw->list);
			tasklet_wakeup(sw->task);
		}
		else if (!(h2s->flags & H2_SF_BLK_SFCTL)) {
			h2s->flags &= ~H2_SF_BLK_ANY;

			LIST_DEL(&sw->list);
			LIST_INIT(&sw->list);
			sw->wait_reason &= ~SUB_CAN_SEND;
			tasklet_wakeup(sw->task);
		}
	}

 fail:
	if (unlikely(h2c->st0 >= H2_CS_ERROR)) {
		if (h2c->st0 == H2_CS_ERROR) {
			if (h2c->max_id >= 0) {
				h2c_send_goaway_error(h2c, NULL);
				if (h2c->flags & H2_CF_MUX_BLOCK_ANY)
					return 0;
			}

			h2c->st0 = H2_CS_ERROR2; // sent (or failed hard) !
		}
		return 1;
	}
	return (h2c->mws <= 0 || LIST_ISEMPTY(&h2c->fctl_list)) && LIST_ISEMPTY(&h2c->send_list);
}


/* Attempt to read data, and subscribe if none available */
static int h2_recv(struct h2c *h2c)
{
	struct connection *conn = h2c->conn;
	struct buffer *buf;
	int max;
	size_t ret;

	if (h2c->wait_list.wait_reason & SUB_CAN_RECV)
		return 0;

	if (!h2_recv_allowed(h2c))
		return 0;

	buf = h2_get_buf(h2c, &h2c->dbuf);
	if (!buf) {
		h2c->flags |= H2_CF_DEM_DALLOC;
		return 0;
	}

	do {
		max = buf->size - b_data(buf);
		if (max)
			ret = conn->xprt->rcv_buf(conn, buf, max, 0);
		else
			ret = 0;
	} while (ret > 0);

	if (h2_recv_allowed(h2c)) {
		conn_xprt_want_recv(conn);
		conn->xprt->subscribe(conn, SUB_CAN_RECV, &h2c->wait_list);
	}
	if (!b_data(buf)) {
		h2_release_buf(h2c, &h2c->dbuf);
		return 0;
	}

	if (b_data(buf) == buf->size)
		h2c->flags |= H2_CF_DEM_DFULL;
	return 1;
}

/* Try to send data if possible */
static int h2_send(struct h2c *h2c)
{
	struct connection *conn = h2c->conn;
	int done;
	int sent = 0;

	if (conn->flags & CO_FL_ERROR)
		return 0;


	if (conn->flags & (CO_FL_HANDSHAKE|CO_FL_WAIT_L4_CONN|CO_FL_WAIT_L6_CONN)) {
		/* a handshake was requested */
		goto schedule;
	}

	/* This loop is quite simple : it tries to fill as much as it can from
	 * pending streams into the existing buffer until it's reportedly full
	 * or the end of send requests is reached. Then it tries to send this
	 * buffer's contents out, marks it not full if at least one byte could
	 * be sent, and tries again.
	 *
	 * The snd_buf() function normally takes a "flags" argument which may
	 * be made of a combination of CO_SFL_MSG_MORE to indicate that more
	 * data immediately comes and CO_SFL_STREAMER to indicate that the
	 * connection is streaming lots of data (used to increase TLS record
	 * size at the expense of latency). The former can be sent any time
	 * there's a buffer full flag, as it indicates at least one stream
	 * attempted to send and failed so there are pending data. An
	 * alternative would be to set it as long as there's an active stream
	 * but that would be problematic for ACKs until we have an absolute
	 * guarantee that all waiters have at least one byte to send. The
	 * latter should possibly not be set for now.
	 */

	done = 0;
	while (!done) {
		unsigned int flags = 0;

		/* fill as much as we can into the current buffer */
		while (((h2c->flags & (H2_CF_MUX_MFULL|H2_CF_MUX_MALLOC)) == 0) && !done)
			done = h2_process_mux(h2c);

		if (conn->flags & CO_FL_ERROR)
			break;

		if (h2c->flags & (H2_CF_MUX_MFULL | H2_CF_DEM_MBUSY | H2_CF_DEM_MROOM))
			flags |= CO_SFL_MSG_MORE;

		if (b_data(&h2c->mbuf)) {
			int ret = conn->xprt->snd_buf(conn, &h2c->mbuf, b_data(&h2c->mbuf), flags);
			if (!ret)
				break;
			sent = 1;
			b_del(&h2c->mbuf, ret);
			b_realign_if_empty(&h2c->mbuf);
		}

		/* wrote at least one byte, the buffer is not full anymore */
		h2c->flags &= ~(H2_CF_MUX_MFULL | H2_CF_DEM_MROOM);
	}

	if (conn->flags & CO_FL_SOCK_WR_SH) {
		/* output closed, nothing to send, clear the buffer to release it */
		b_reset(&h2c->mbuf);
	}
	/* We're not full anymore, so we can wake any task that are waiting
	 * for us.
	 */
	if (!(h2c->flags & (H2_CF_MUX_MFULL | H2_CF_DEM_MROOM))) {
		while (!LIST_ISEMPTY(&h2c->send_list)) {
			struct wait_list *sw = LIST_ELEM(h2c->send_list.n,
			    struct wait_list *, list);
			LIST_DEL(&sw->list);
			LIST_INIT(&sw->list);
			sw->wait_reason &= ~SUB_CAN_SEND;
			tasklet_wakeup(sw->task);
		}
	}
	/* We're done, no more to send */
	if (!b_data(&h2c->mbuf))
		return sent;
schedule:
	if (LIST_ISEMPTY(&h2c->wait_list.list))
		conn->xprt->subscribe(conn, SUB_CAN_SEND, &h2c->wait_list);
	return sent;
}

static struct task *h2_io_cb(struct task *t, void *ctx, unsigned short status)
{
	struct h2c *h2c = ctx;
	int ret = 0;

	if (!(h2c->wait_list.wait_reason & SUB_CAN_SEND))
		ret = h2_send(h2c);
	if (!(h2c->wait_list.wait_reason & SUB_CAN_RECV))
		ret |= h2_recv(h2c);
	if (ret)
		h2_process(h2c);
	return NULL;
}

/* callback called on any event by the connection handler.
 * It applies changes and returns zero, or < 0 if it wants immediate
 * destruction of the connection (which normally doesn not happen in h2).
 */
static int h2_process(struct h2c *h2c)
{
	struct connection *conn = h2c->conn;
	struct session *sess = conn->owner;

	if (b_data(&h2c->dbuf) && !(h2c->flags & H2_CF_DEM_BLOCK_ANY)) {
		h2_process_demux(h2c);

		if (h2c->st0 >= H2_CS_ERROR || conn->flags & CO_FL_ERROR)
			b_reset(&h2c->dbuf);

		if (!b_full(&h2c->dbuf))
			h2c->flags &= ~H2_CF_DEM_DFULL;
	}
	h2_send(h2c);

	if (sess && unlikely(sess->fe->state == PR_STSTOPPED)) {
		/* frontend is stopping, reload likely in progress, let's try
		 * to announce a graceful shutdown if not yet done. We don't
		 * care if it fails, it will be tried again later.
		 */
		if (!(h2c->flags & (H2_CF_GOAWAY_SENT|H2_CF_GOAWAY_FAILED))) {
			if (h2c->last_sid < 0)
				h2c->last_sid = (1U << 31) - 1;
			h2c_send_goaway_error(h2c, NULL);
		}
	}

	/*
	 * If we received early data, and the handshake is done, wake
	 * any stream that was waiting for it.
	 */
	if (!(h2c->flags & H2_CF_WAIT_FOR_HS) &&
	    (conn->flags & (CO_FL_EARLY_SSL_HS | CO_FL_HANDSHAKE | CO_FL_EARLY_DATA)) == CO_FL_EARLY_DATA) {
		struct eb32_node *node;
		struct h2s *h2s;

		h2c->flags |= H2_CF_WAIT_FOR_HS;
		node = eb32_lookup_ge(&h2c->streams_by_id, 1);

		while (node) {
			h2s = container_of(node, struct h2s, by_id);
			if ((h2s->cs->flags & CS_FL_WAIT_FOR_HS) &&
			    h2s->recv_wait_list) {
				struct wait_list *sw = h2s->recv_wait_list;
				sw->wait_reason &= ~SUB_CAN_RECV;
				tasklet_wakeup(sw->task);
				h2s->recv_wait_list = NULL;
			}
			node = eb32_next(node);
		}
	}

	if (conn->flags & CO_FL_ERROR || conn_xprt_read0_pending(conn) ||
	    h2c->st0 == H2_CS_ERROR2 || h2c->flags & H2_CF_GOAWAY_FAILED ||
	    (eb_is_empty(&h2c->streams_by_id) && h2c->last_sid >= 0 &&
	     h2c->max_id >= h2c->last_sid)) {
		h2_wake_some_streams(h2c, 0, 0);

		if (eb_is_empty(&h2c->streams_by_id)) {
			/* no more stream, kill the connection now */
			h2_release(conn);
			return -1;
		}
	}

	if (!b_data(&h2c->dbuf))
		h2_release_buf(h2c, &h2c->dbuf);

	/* stop being notified of incoming data if we can't process them */
	if (!h2_recv_allowed(h2c))
		__conn_xprt_stop_recv(conn);
	else
		__conn_xprt_want_recv(conn);

	/* adjust output polling */
	if (!(conn->flags & CO_FL_SOCK_WR_SH) &&
	    h2c->st0 != H2_CS_ERROR2 && !(h2c->flags & H2_CF_GOAWAY_FAILED) &&
	    (h2c->st0 == H2_CS_ERROR ||
	     b_data(&h2c->mbuf) ||
	     (h2c->mws > 0 && !LIST_ISEMPTY(&h2c->fctl_list)) ||
	     (!(h2c->flags & H2_CF_MUX_BLOCK_ANY) && !LIST_ISEMPTY(&h2c->send_list)))) {
		__conn_xprt_want_send(conn);
	}
	else {
		h2_release_buf(h2c, &h2c->mbuf);
		__conn_xprt_stop_send(conn);
	}

	if (h2c->task) {
		if (eb_is_empty(&h2c->streams_by_id) || b_data(&h2c->mbuf)) {
			h2c->task->expire = tick_add(now_ms, h2c->last_sid < 0 ? h2c->timeout : h2c->shut_timeout);
			task_queue(h2c->task);
		}
		else
			h2c->task->expire = TICK_ETERNITY;
	}

	h2_send(h2c);
	return 0;
}

static int h2_wake(struct connection *conn)
{
	struct h2c *h2c = conn->mux_ctx;

	return (h2_process(h2c));
}


/* Connection timeout management. The principle is that if there's no receipt
 * nor sending for a certain amount of time, the connection is closed. If the
 * MUX buffer still has lying data or is not allocatable, the connection is
 * immediately killed. If it's allocatable and empty, we attempt to send a
 * GOAWAY frame.
 */
static struct task *h2_timeout_task(struct task *t, void *context, unsigned short state)
{
	struct h2c *h2c = context;
	int expired = tick_is_expired(t->expire, now_ms);

	if (!expired && h2c)
		return t;

	task_delete(t);
	task_free(t);

	if (!h2c) {
		/* resources were already deleted */
		return NULL;
	}

	h2c->task = NULL;
	h2c_error(h2c, H2_ERR_NO_ERROR);
	h2_wake_some_streams(h2c, 0, 0);

	if (b_data(&h2c->mbuf)) {
		/* don't even try to send a GOAWAY, the buffer is stuck */
		h2c->flags |= H2_CF_GOAWAY_FAILED;
	}

	/* try to send but no need to insist */
	h2c->last_sid = h2c->max_id;
	if (h2c_send_goaway_error(h2c, NULL) <= 0)
		h2c->flags |= H2_CF_GOAWAY_FAILED;

	if (b_data(&h2c->mbuf) && !(h2c->flags & H2_CF_GOAWAY_FAILED) && conn_xprt_ready(h2c->conn)) {
		int ret = h2c->conn->xprt->snd_buf(h2c->conn, &h2c->mbuf, b_data(&h2c->mbuf), 0);
		if (ret > 0) {
			b_del(&h2c->mbuf, ret);
			b_realign_if_empty(&h2c->mbuf);
		}
	}

	/* either we can release everything now or it will be done later once
	 * the last stream closes.
	 */
	if (eb_is_empty(&h2c->streams_by_id))
		h2_release(h2c->conn);

	return NULL;
}


/*******************************************/
/* functions below are used by the streams */
/*******************************************/

/*
 * Attach a new stream to a connection
 * (Used for outgoing connections)
 */
static struct conn_stream *h2_attach(struct connection *conn)
{
	return NULL;
}

/* callback used to update the mux's polling flags after changing a cs' status.
 * The caller (cs_update_mux_polling) will take care of propagating any changes
 * to the transport layer.
 */
static void h2_update_poll(struct conn_stream *cs)
{
	struct h2s *h2s = cs->ctx;

	if (!h2s)
		return;

	/* we may unblock a blocked read */

	if (cs->flags & CS_FL_DATA_RD_ENA) {
		/* the stream indicates it's willing to read */
		h2s->h2c->flags &= ~H2_CF_DEM_SFULL;
		if (h2s->h2c->dsi == h2s->id) {
			conn_xprt_want_recv(cs->conn);
			tasklet_wakeup(h2s->h2c->wait_list.task);
			conn_xprt_want_send(cs->conn);
		}
	}

	/* Note: the stream and stream-int code doesn't allow us to perform a
	 * synchronous send() here unfortunately, because this code is called
	 * as si_update() from the process_stream() context. This means that
	 * we have to queue the current cs and defer its processing after the
	 * connection's cs list is processed anyway.
	 */

	if (cs->flags & CS_FL_DATA_WR_ENA) {
		if (!b_data(&h2s->h2c->mbuf) && !(cs->conn->flags & CO_FL_SOCK_WR_SH))
			conn_xprt_want_send(cs->conn);
		tasklet_wakeup(h2s->h2c->wait_list.task);
	}
	/* We don't support unsubscribing from here, it shouldn't be a problem */

	/* this can happen from within si_chk_snd() */
	if (b_data(&h2s->h2c->mbuf) && !(cs->conn->flags & CO_FL_XPRT_WR_ENA))
		conn_xprt_want_send(cs->conn);
}

/*
 * Detach the stream from the connection and possibly release the connection.
 */
static void h2_detach(struct conn_stream *cs)
{
	struct h2s *h2s = cs->ctx;
	struct h2c *h2c;

	cs->ctx = NULL;
	if (!h2s)
		return;

	h2c = h2s->h2c;
	/* If the stream we're detaching waited for more data, unsubscribe it now */
	if (h2s->recv_wait_list && !((long)h2s->recv_wait_list->handle & 3))
		h2s->recv_wait_list = NULL;
	h2s->cs = NULL;
	h2c->nb_cs--;
	if (h2c->flags & H2_CF_DEM_TOOMANY &&
	    !h2_has_too_many_cs(h2c)) {
		h2c->flags &= ~H2_CF_DEM_TOOMANY;
		if (h2_recv_allowed(h2c)) {
			__conn_xprt_want_recv(h2c->conn);
			tasklet_wakeup(h2c->wait_list.task);
			conn_xprt_want_send(h2c->conn);
		}
	}

	/* this stream may be blocked waiting for some data to leave (possibly
	 * an ES or RST frame), so orphan it in this case.
	 */
	if (!(cs->conn->flags & CO_FL_ERROR) &&
	    (h2c->st0 < H2_CS_ERROR) &&
	    (h2s->flags & (H2_SF_BLK_MBUSY | H2_SF_BLK_MROOM | H2_SF_BLK_MFCTL)))
		return;

	if ((h2c->flags & H2_CF_DEM_BLOCK_ANY && h2s->id == h2c->dsi) ||
	    (h2c->flags & H2_CF_MUX_BLOCK_ANY && h2s->id == h2c->msi)) {
		/* unblock the connection if it was blocked on this
		 * stream.
		 */
		h2c->flags &= ~H2_CF_DEM_BLOCK_ANY;
		h2c->flags &= ~H2_CF_MUX_BLOCK_ANY;
		conn_xprt_want_recv(cs->conn);
		tasklet_wakeup(h2c->wait_list.task);
		conn_xprt_want_send(cs->conn);
	}

	h2s_destroy(h2s);

	/* We don't want to close right now unless we're removing the
	 * last stream, and either the connection is in error, or it
	 * reached the ID already specified in a GOAWAY frame received
	 * or sent (as seen by last_sid >= 0).
	 */
	if (eb_is_empty(&h2c->streams_by_id) &&     /* don't close if streams exist */
	    ((h2c->conn->flags & CO_FL_ERROR) ||    /* errors close immediately */
	     (h2c->st0 >= H2_CS_ERROR && !h2c->task) || /* a timeout stroke earlier */
	     (h2c->flags & H2_CF_GOAWAY_FAILED) ||
	     (!b_data(&h2c->mbuf) &&  /* mux buffer empty, also process clean events below */
	      (conn_xprt_read0_pending(h2c->conn) ||
	       (h2c->last_sid >= 0 && h2c->max_id >= h2c->last_sid))))) {
		/* no more stream will come, kill it now */
		h2_release(h2c->conn);
	}
	else if (h2c->task) {
		if (eb_is_empty(&h2c->streams_by_id) || b_data(&h2c->mbuf)) {
			h2c->task->expire = tick_add(now_ms, h2c->last_sid < 0 ? h2c->timeout : h2c->shut_timeout);
			task_queue(h2c->task);
		}
		else
			h2c->task->expire = TICK_ETERNITY;
	}
}

static void h2_do_shutr(struct h2s *h2s)
{
	struct h2c *h2c = h2s->h2c;
	struct wait_list *sw = &h2s->wait_list;

	if (h2s->st == H2_SS_HLOC || h2s->st == H2_SS_ERROR || h2s->st == H2_SS_CLOSED)
		return;

	/* if no outgoing data was seen on this stream, it means it was
	 * closed with a "tcp-request content" rule that is normally
	 * used to kill the connection ASAP (eg: limit abuse). In this
	 * case we send a goaway to close the connection.
	 */
	if (!(h2s->flags & H2_SF_RST_SENT) &&
	    h2s_send_rst_stream(h2c, h2s) <= 0)
		goto add_to_list;

	if (!(h2s->flags & H2_SF_OUTGOING_DATA) &&
	    !(h2s->h2c->flags & (H2_CF_GOAWAY_SENT|H2_CF_GOAWAY_FAILED)) &&
	    h2c_send_goaway_error(h2c, h2s) <= 0)
		return;
	if (b_data(&h2c->mbuf) && !(h2c->conn->flags & CO_FL_XPRT_WR_ENA))
		conn_xprt_want_send(h2c->conn);

	h2s_close(h2s);

	return;
add_to_list:
	if (LIST_ISEMPTY(&sw->list)) {
		sw->wait_reason |= SUB_CAN_SEND;
		if (h2s->flags & H2_SF_BLK_MFCTL)
			LIST_ADDQ(&h2c->fctl_list, &sw->list);
		else if (h2s->flags & (H2_SF_BLK_MBUSY|H2_SF_BLK_MROOM))
			LIST_ADDQ(&h2c->send_list, &sw->list);
	}
	/* Let the handler know we want shutr */
	sw->handle = (void *)((long)sw->handle | 1);

}

static void h2_do_shutw(struct h2s *h2s)
{
	struct h2c *h2c = h2s->h2c;
	struct wait_list *sw = &h2s->wait_list;

	if (h2s->st == H2_SS_HLOC || h2s->st == H2_SS_ERROR || h2s->st == H2_SS_CLOSED)
		return;

	if (h2s->flags & H2_SF_HEADERS_SENT) {
		/* we can cleanly close using an empty data frame only after headers */

		if (!(h2s->flags & (H2_SF_ES_SENT|H2_SF_RST_SENT)) &&
		    h2_send_empty_data_es(h2s) <= 0)
			goto add_to_list;

		if (h2s->st == H2_SS_HREM)
			h2s_close(h2s);
		else
			h2s->st = H2_SS_HLOC;
	} else {
		/* if no outgoing data was seen on this stream, it means it was
		 * closed with a "tcp-request content" rule that is normally
		 * used to kill the connection ASAP (eg: limit abuse). In this
		 * case we send a goaway to close the connection.
		 */
		if (!(h2s->flags & H2_SF_RST_SENT) &&
		    h2s_send_rst_stream(h2c, h2s) <= 0)
			goto add_to_list;

		if (!(h2s->flags & H2_SF_OUTGOING_DATA) &&
		    !(h2s->h2c->flags & (H2_CF_GOAWAY_SENT|H2_CF_GOAWAY_FAILED)) &&
		    h2c_send_goaway_error(h2c, h2s) <= 0)
			goto add_to_list;

		h2s_close(h2s);
	}

	if (b_data(&h2s->h2c->mbuf) && !(h2c->conn->flags & CO_FL_XPRT_WR_ENA))
		conn_xprt_want_send(h2c->conn);

 add_to_list:
	sw = &h2s->wait_list;

	if (LIST_ISEMPTY(&sw->list)) {
		sw->wait_reason |= SUB_CAN_SEND;
		if (h2s->flags & H2_SF_BLK_MFCTL)
			LIST_ADDQ(&h2s->h2c->fctl_list, &sw->list);
		else if (h2s->flags & (H2_SF_BLK_MBUSY|H2_SF_BLK_MROOM))
			LIST_ADDQ(&h2s->h2c->send_list, &sw->list);
	}
	/* let the handler know we want to shutr */
	sw->handle = (void *)((long)(sw->handle) | 2);
}

static struct task *h2_deferred_shut(struct task *t, void *ctx, unsigned short state)
{
	struct h2s *h2s = ctx;
	long reason = (long)h2s->wait_list.handle;

	if (reason & 1)
		h2_do_shutr(h2s);
	if (reason & 2)
		h2_do_shutw(h2s);

	return NULL;
}

static void h2_shutr(struct conn_stream *cs, enum cs_shr_mode mode)
{
	struct h2s *h2s = cs->ctx;

	if (!mode)
		return;

	h2_do_shutr(h2s);
}

static void h2_shutw(struct conn_stream *cs, enum cs_shw_mode mode)
{
	struct h2s *h2s = cs->ctx;

	h2_do_shutw(h2s);
}

/* Decode the payload of a HEADERS frame and produce the equivalent HTTP/1
 * request. Returns the number of bytes emitted if > 0, or 0 if it couldn't
 * proceed. Stream errors are reported in h2s->errcode and connection errors
 * in h2c->errcode.
 */
static int h2_frt_decode_headers(struct h2s *h2s)
{
	struct h2c *h2c = h2s->h2c;
	const uint8_t *hdrs = (uint8_t *)b_head(&h2c->dbuf);
	struct buffer *tmp = get_trash_chunk();
	struct http_hdr list[MAX_HTTP_HDR * 2];
	struct buffer *copy = NULL;
	unsigned int msgf;
	struct buffer *csbuf;
	int flen = h2c->dfl;
	int outlen = 0;
	int wrap;
	int try;

	if (!h2c->dfl) {
		h2s_error(h2s, H2_ERR_PROTOCOL_ERROR); // empty headers frame!
		h2c->st0 = H2_CS_FRAME_E;
		return 0;
	}

	if (b_data(&h2c->dbuf) < h2c->dfl && !b_full(&h2c->dbuf))
		return 0; // incomplete input frame

	/* if the input buffer wraps, take a temporary copy of it (rare) */
	wrap = b_wrap(&h2c->dbuf) - b_head(&h2c->dbuf);
	if (wrap < h2c->dfl) {
		copy = alloc_trash_chunk();
		if (!copy) {
			h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
			goto fail;
		}
		memcpy(copy->area, b_head(&h2c->dbuf), wrap);
		memcpy(copy->area + wrap, b_orig(&h2c->dbuf), h2c->dfl - wrap);
		hdrs = (uint8_t *) copy->area;
	}

	/* The padlen is the first byte before data, and the padding appears
	 * after data. padlen+data+padding are included in flen.
	 */
	if (h2c->dff & H2_F_HEADERS_PADDED) {
		h2c->dpl = *hdrs;
		if (h2c->dpl >= flen) {
			/* RFC7540#6.2 : pad length = length of frame payload or greater */
			h2c_error(h2c, H2_ERR_PROTOCOL_ERROR);
			goto fail;
		}
		flen -= h2c->dpl + 1;
		hdrs += 1; // skip Pad Length
	}

	/* Skip StreamDep and weight for now (we don't support PRIORITY) */
	if (h2c->dff & H2_F_HEADERS_PRIORITY) {
		if (read_n32(hdrs) == h2s->id) {
			/* RFC7540#5.3.1 : stream dep may not depend on itself */
			h2c_error(h2c, H2_ERR_PROTOCOL_ERROR);
			goto fail;
		}

		hdrs += 5; // stream dep = 4, weight = 1
		flen -= 5;
	}

	/* FIXME: lack of END_HEADERS means there's a continuation frame, we
	 * don't support this for now and can't even decompress so we have to
	 * break the connection.
	 */
	if (!(h2c->dff & H2_F_HEADERS_END_HEADERS)) {
		h2c_error(h2c, H2_ERR_INTERNAL_ERROR);
		goto fail;
	}

	csbuf = h2_get_buf(h2c, &h2s->rxbuf);
	if (!csbuf) {
		h2c->flags |= H2_CF_DEM_SALLOC;
		goto fail;
	}

	/* we can't retry a failed decompression operation so we must be very
	 * careful not to take any risks. In practice the output buffer is
	 * always empty except maybe for trailers, in which case we simply have
	 * to wait for the upper layer to finish consuming what is available.
	 */
	if (b_data(csbuf))
		goto fail;

	csbuf->head = 0;
	try = b_size(csbuf);

	outlen = hpack_decode_frame(h2c->ddht, hdrs, flen, list,
	                            sizeof(list)/sizeof(list[0]), tmp);
	if (outlen < 0) {
		h2c_error(h2c, H2_ERR_COMPRESSION_ERROR);
		goto fail;
	}

	/* OK now we have our header list in <list> */
	msgf = (h2c->dff & H2_F_DATA_END_STREAM) ? 0 : H2_MSGF_BODY;
	outlen = h2_make_h1_request(list, b_tail(csbuf), try, &msgf);

	if (outlen < 0) {
		h2c_error(h2c, H2_ERR_COMPRESSION_ERROR);
		goto fail;
	}

	if (msgf & H2_MSGF_BODY) {
		/* a payload is present */
		if (msgf & H2_MSGF_BODY_CL)
			h2s->flags |= H2_SF_DATA_CLEN;
		else if (!(msgf & H2_MSGF_BODY_TUNNEL))
			h2s->flags |= H2_SF_DATA_CHNK;
	}

	/* now consume the input data */
	b_del(&h2c->dbuf, h2c->dfl);
	h2c->st0 = H2_CS_FRAME_H;
	b_add(csbuf, outlen);

	if (h2c->dff & H2_F_HEADERS_END_STREAM) {
		h2s->flags |= H2_SF_ES_RCVD;
		h2s->cs->flags |= CS_FL_REOS;
	}

 leave:
	free_trash_chunk(copy);
	return outlen;
 fail:
	outlen = 0;
	goto leave;
}

/* Transfer the payload of a DATA frame to the HTTP/1 side. When content-length
 * or a tunnel is used, the contents are copied as-is. When chunked encoding is
 * in use, a new chunk is emitted for each frame. This is supposed to fit
 * because the smallest chunk takes 1 byte for the size, 2 for CRLF, X for the
 * data, 2 for the extra CRLF, so that's 5+X, while on the H2 side the smallest
 * frame will be 9+X bytes based on the same buffer size. The HTTP/2 frame
 * parser state is automatically updated. Returns the number of bytes emitted
 * if > 0, or 0 if it couldn't proceed, in which case CS_FL_RCV_MORE must be
 * checked to know if some data remain pending (an empty DATA frame can return
 * 0 as a valid result). Stream errors are reported in h2s->errcode and
 * connection errors in h2c->errcode. The caller must already have checked the
 * frame header and ensured that the frame was complete or the buffer full. It
 * changes the frame state to FRAME_A once done.
 */
static int h2_frt_transfer_data(struct h2s *h2s)
{
	struct h2c *h2c = h2s->h2c;
	int block1, block2;
	unsigned int flen = 0;
	unsigned int chklen = 0;
	struct buffer *csbuf;

	h2c->flags &= ~H2_CF_DEM_SFULL;

	/* The padlen is the first byte before data, and the padding appears
	 * after data. padlen+data+padding are included in flen.
	 */
	if (h2c->dff & H2_F_DATA_PADDED) {
		if (b_data(&h2c->dbuf) < 1)
			return 0;

		h2c->dpl = *(uint8_t *)b_head(&h2c->dbuf);
		if (h2c->dpl >= h2c->dfl) {
			/* RFC7540#6.1 : pad length = length of frame payload or greater */
			h2c_error(h2c, H2_ERR_PROTOCOL_ERROR);
			return 0;
		}

		/* skip the padlen byte */
		b_del(&h2c->dbuf, 1);
		h2c->dfl--;
		h2c->rcvd_c++; h2c->rcvd_s++;
		h2c->dff &= ~H2_F_DATA_PADDED;
	}

	csbuf = h2_get_buf(h2c, &h2s->rxbuf);
	if (!csbuf) {
		h2c->flags |= H2_CF_DEM_SALLOC;
		goto fail;
	}

	flen = h2c->dfl - h2c->dpl;
	if (!flen)
		goto end_transfer;

	if (flen > b_data(&h2c->dbuf)) {
		flen = b_data(&h2c->dbuf);
		if (!flen)
			goto fail;
	}

	if (unlikely(b_space_wraps(csbuf))) {
		/* it doesn't fit and the buffer is fragmented,
		 * so let's defragment it and try again.
		 */
		b_slow_realign(csbuf, trash.area, 0);
	}

	/* chunked-encoding requires more room */
	if (h2s->flags & H2_SF_DATA_CHNK) {
		chklen = MIN(flen, b_room(csbuf));
		chklen = (chklen < 16) ? 1 : (chklen < 256) ? 2 :
			(chklen < 4096) ? 3 : (chklen < 65536) ? 4 :
			(chklen < 1048576) ? 4 : 8;
		chklen += 4; // CRLF, CRLF
	}

	/* does it fit in output buffer or should we wait ? */
	if (flen + chklen > b_room(csbuf)) {
		if (chklen >= b_room(csbuf)) {
			h2c->flags |= H2_CF_DEM_SFULL;
			goto fail;
		}
		flen = b_room(csbuf) - chklen;
	}

	if (h2s->flags & H2_SF_DATA_CHNK) {
		/* emit the chunk size */
		unsigned int chksz = flen;
		char str[10];
		char *beg;

		beg = str + sizeof(str);
		*--beg = '\n';
		*--beg = '\r';
		do {
			*--beg = hextab[chksz & 0xF];
		} while (chksz >>= 4);
		b_putblk(csbuf, beg, str + sizeof(str) - beg);
	}

	/* Block1 is the length of the first block before the buffer wraps,
	 * block2 is the optional second block to reach the end of the frame.
	 */
	block1 = b_contig_data(&h2c->dbuf, 0);
	if (block1 > flen)
		block1 = flen;
	block2 = flen - block1;

	if (block1)
		b_putblk(csbuf, b_head(&h2c->dbuf), block1);

	if (block2)
		b_putblk(csbuf, b_peek(&h2c->dbuf, block1), block2);

	if (h2s->flags & H2_SF_DATA_CHNK) {
		/* emit the CRLF */
		b_putblk(csbuf, "\r\n", 2);
	}

	/* now mark the input data as consumed (will be deleted from the buffer
	 * by the caller when seeing FRAME_A after sending the window update).
	 */
	b_del(&h2c->dbuf, flen);
	h2c->dfl    -= flen;
	h2c->rcvd_c += flen;
	h2c->rcvd_s += flen;  // warning, this can also affect the closed streams!

	if (h2c->dfl > h2c->dpl) {
		/* more data available, transfer stalled on stream full */
		h2c->flags |= H2_CF_DEM_SFULL;
		goto fail;
	}

 end_transfer:
	/* here we're done with the frame, all the payload (except padding) was
	 * transferred.
	 */

	if (h2c->dff & H2_F_DATA_END_STREAM && h2s->flags & H2_SF_DATA_CHNK) {
		/* emit the trailing 0 CRLF CRLF */
		if (b_room(csbuf) < 5) {
			h2c->flags |= H2_CF_DEM_SFULL;
			goto fail;
		}
		chklen += 5;
		b_putblk(csbuf, "0\r\n\r\n", 5);
	}

	h2c->rcvd_c += h2c->dpl;
	h2c->rcvd_s += h2c->dpl;
	h2c->dpl = 0;
	h2c->st0 = H2_CS_FRAME_A; // send the corresponding window update

	if (h2c->dff & H2_F_DATA_END_STREAM) {
		h2s->flags |= H2_SF_ES_RCVD;
		h2s->cs->flags |= CS_FL_REOS;
	}

	return flen + chklen;
 fail:
	return 0;
}

/* Try to send a HEADERS frame matching HTTP/1 response present at offset <ofs>
 * and for <max> bytes in buffer <buf> for the H2 stream <h2s>. Returns the
 * number of bytes sent. The caller must check the stream's status to detect
 * any error which might have happened subsequently to a successful send.
 */
static size_t h2s_frt_make_resp_headers(struct h2s *h2s, const struct buffer *buf, size_t ofs, size_t max)
{
	struct http_hdr list[MAX_HTTP_HDR];
	struct h2c *h2c = h2s->h2c;
	struct h1m *h1m = &h2s->h1m;
	struct buffer outbuf;
	union h1_sl sl;
	int es_now = 0;
	int ret = 0;
	int hdr;

	if (h2c_mux_busy(h2c, h2s)) {
		h2s->flags |= H2_SF_BLK_MBUSY;
		return 0;
	}

	if (!h2_get_buf(h2c, &h2c->mbuf)) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2s->flags |= H2_SF_BLK_MROOM;
		return 0;
	}

	/* First, try to parse the H1 response and index it into <list>.
	 * NOTE! Since it comes from haproxy, we *know* that a response header
	 * block does not wrap and we can safely read it this way without
	 * having to realign the buffer.
	 */
	ret = h1_headers_to_hdr_list(b_peek(buf, ofs), b_peek(buf, ofs) + max,
	                             list, sizeof(list)/sizeof(list[0]), h1m, &sl);
	if (ret <= 0) {
		/* incomplete or invalid response, this is abnormal coming from
		 * haproxy and may only result in a bad errorfile or bad Lua code
		 * so that won't be fixed, raise an error now.
		 *
		 * FIXME: we should instead add the ability to only return a
		 * 502 bad gateway. But in theory this is not supposed to
		 * happen.
		 */
		h2s_error(h2s, H2_ERR_INTERNAL_ERROR);
		ret = 0;
		goto end;
	}

	h2s->status = sl.st.status;

	/* certain statuses have no body or an empty one, regardless of
	 * what the headers say.
	 */
	if (sl.st.status >= 100 && sl.st.status < 200) {
		h1m->flags &= ~(H1_MF_CLEN | H1_MF_CHNK);
		h1m->curr_len = h1m->body_len = 0;
	}
	else if (sl.st.status == 204 || sl.st.status == 304) {
		/* no contents, claim c-len is present and set to zero */
		h1m->flags &= ~H1_MF_CHNK;
		h1m->flags |=  H1_MF_CLEN;
		h1m->curr_len = h1m->body_len = 0;
	}

	chunk_reset(&outbuf);

	while (1) {
		outbuf.area  = b_tail(&h2c->mbuf);
		outbuf.size = b_contig_space(&h2c->mbuf);
		outbuf.data = 0;

		if (outbuf.size >= 9 || !b_space_wraps(&h2c->mbuf))
			break;
	realign_again:
		b_slow_realign(&h2c->mbuf, trash.area, b_data(&h2c->mbuf));
	}

	if (outbuf.size < 9)
		goto full;

	/* len: 0x000000 (fill later), type: 1(HEADERS), flags: ENDH=4 */
	memcpy(outbuf.area, "\x00\x00\x00\x01\x04", 5);
	write_n32(outbuf.area + 5, h2s->id); // 4 bytes
	outbuf.data = 9;

	/* encode status, which necessarily is the first one */
	if (outbuf.data < outbuf.size && h2s->status == 200)
		outbuf.area[outbuf.data++] = 0x88; // indexed field : idx[08]=(":status", "200")
	else if (outbuf.data < outbuf.size && h2s->status == 304)
		outbuf.area[outbuf.data++] = 0x8b; // indexed field : idx[11]=(":status", "304")
	else if (unlikely(list[0].v.len != 3)) {
		/* this is an unparsable response */
		h2s_error(h2s, H2_ERR_INTERNAL_ERROR);
		ret = 0;
		goto end;
	}
	else if (unlikely(outbuf.data + 2 + 3 <= outbuf.size)) {
		/* basic encoding of the status code */
		outbuf.area[outbuf.data++] = 0x48; // indexed name -- name=":status" (idx 8)
		outbuf.area[outbuf.data++] = 0x03; // 3 bytes status
		outbuf.area[outbuf.data++] = list[0].v.ptr[0];
		outbuf.area[outbuf.data++] = list[0].v.ptr[1];
		outbuf.area[outbuf.data++] = list[0].v.ptr[2];
	}
	else {
		if (b_space_wraps(&h2c->mbuf))
			goto realign_again;
		goto full;
	}

	/* encode all headers, stop at empty name */
	for (hdr = 1; hdr < sizeof(list)/sizeof(list[0]); hdr++) {
		/* these ones do not exist in H2 and must be dropped. */
		if (isteq(list[hdr].n, ist("connection")) ||
		    isteq(list[hdr].n, ist("proxy-connection")) ||
		    isteq(list[hdr].n, ist("keep-alive")) ||
		    isteq(list[hdr].n, ist("upgrade")) ||
		    isteq(list[hdr].n, ist("transfer-encoding")))
			continue;

		if (isteq(list[hdr].n, ist("")))
			break; // end

		if (!hpack_encode_header(&outbuf, list[hdr].n, list[hdr].v)) {
			/* output full */
			if (b_space_wraps(&h2c->mbuf))
				goto realign_again;
			goto full;
		}
	}

	/* we may need to add END_STREAM */
	if (((h1m->flags & H1_MF_CLEN) && !h1m->body_len) || h2s->cs->flags & CS_FL_SHW)
		es_now = 1;

	/* update the frame's size */
	h2_set_frame_size(outbuf.area, outbuf.data - 9);

	if (es_now)
		outbuf.area[4] |= H2_F_HEADERS_END_STREAM;

	/* consume incoming H1 response */
	max -= ret;

	/* commit the H2 response */
	b_add(&h2c->mbuf, outbuf.data);
	h2s->flags |= H2_SF_HEADERS_SENT;

	/* for now we don't implemented CONTINUATION, so we wait for a
	 * body or directly end in TRL2.
	 */
	if (es_now) {
		// trim any possibly pending data (eg: inconsistent content-length)
		ret += max;

		h1m->state = H1_MSG_DONE;
		h2s->flags |= H2_SF_ES_SENT;
		if (h2s->st == H2_SS_OPEN)
			h2s->st = H2_SS_HLOC;
		else
			h2s_close(h2s);
	}
	else if (h2s->status >= 100 && h2s->status < 200) {
		/* we'll let the caller check if it has more headers to send */
		h1m_init_res(h1m);
		h1m->err_pos = -1; // don't care about errors on the response path
		h2s->h1m.flags |= H1_MF_TOLOWER;
		goto end;
	}

	/* now the h1m state is either H1_MSG_CHUNK_SIZE or H1_MSG_DATA */

 end:
	//fprintf(stderr, "[%d] sent simple H2 response (sid=%d) = %d bytes (%d in, ep=%u, es=%s)\n", h2c->st0, h2s->id, outbuf.len, ret, h1m->err_pos, h1_msg_state_str(h1m->err_state));
	return ret;
 full:
	h1m_init_res(h1m);
	h1m->err_pos = -1; // don't care about errors on the response path
	h2c->flags |= H2_CF_MUX_MFULL;
	h2s->flags |= H2_SF_BLK_MROOM;
	ret = 0;
	goto end;
}

/* Try to send a DATA frame matching HTTP/1 response present at offset <ofs>
 * for up to <max> bytes in response buffer <buf>, for stream <h2s>. Returns
 * the number of bytes sent. The caller must check the stream's status to
 * detect any error which might have happened subsequently to a successful send.
 */
static size_t h2s_frt_make_resp_data(struct h2s *h2s, const struct buffer *buf, size_t ofs, size_t max)
{
	struct h2c *h2c = h2s->h2c;
	struct h1m *h1m = &h2s->h1m;
	struct buffer outbuf;
	int ret = 0;
	size_t total = 0;
	int es_now = 0;
	int size = 0;
	const char *blk1, *blk2;
	size_t len1, len2;

	if (h2c_mux_busy(h2c, h2s)) {
		h2s->flags |= H2_SF_BLK_MBUSY;
		goto end;
	}

	if (!h2_get_buf(h2c, &h2c->mbuf)) {
		h2c->flags |= H2_CF_MUX_MALLOC;
		h2s->flags |= H2_SF_BLK_MROOM;
		goto end;
	}

 new_frame:
	if (!max)
		goto end;

	chunk_reset(&outbuf);

	while (1) {
		outbuf.area  = b_tail(&h2c->mbuf);
		outbuf.size = b_contig_space(&h2c->mbuf);
		outbuf.data = 0;

		if (outbuf.size >= 9 || !b_space_wraps(&h2c->mbuf))
			break;
	realign_again:
		b_slow_realign(&h2c->mbuf, trash.area, b_data(&h2c->mbuf));
	}

	if (outbuf.size < 9) {
		h2c->flags |= H2_CF_MUX_MFULL;
		h2s->flags |= H2_SF_BLK_MROOM;
		goto end;
	}

	/* len: 0x000000 (fill later), type: 0(DATA), flags: none=0 */
	memcpy(outbuf.area, "\x00\x00\x00\x00\x00", 5);
	write_n32(outbuf.area + 5, h2s->id); // 4 bytes
	outbuf.data = 9;

	switch (h1m->flags & (H1_MF_CLEN|H1_MF_CHNK)) {
	case 0:           /* no content length, read till SHUTW */
		size = max;
		h1m->curr_len = size;
		break;
	case H1_MF_CLEN:  /* content-length: read only h2m->body_len */
		size = max;
		if ((long long)size > h1m->curr_len)
			size = h1m->curr_len;
		break;
	default:          /* te:chunked : parse chunks */
		if (h1m->state == H1_MSG_CHUNK_CRLF) {
			ret = h1_skip_chunk_crlf(buf, ofs, ofs + max);
			if (!ret)
				goto end;

			if (ret < 0) {
				/* FIXME: bad contents. how to proceed here when we're in H2 ? */
				h1m->err_pos = ofs + max + ret;
				h2s_error(h2s, H2_ERR_INTERNAL_ERROR);
				goto end;
			}
			max -= ret;
			ofs += ret;
			total += ret;
			h1m->state = H1_MSG_CHUNK_SIZE;
		}

		if (h1m->state == H1_MSG_CHUNK_SIZE) {
			unsigned int chunk;
			ret = h1_parse_chunk_size(buf, ofs, ofs + max, &chunk);
			if (!ret)
				goto end;

			if (ret < 0) {
				/* FIXME: bad contents. how to proceed here when we're in H2 ? */
				h1m->err_pos = ofs + max + ret;
				h2s_error(h2s, H2_ERR_INTERNAL_ERROR);
				goto end;
			}

			size = chunk;
			h1m->curr_len = chunk;
			h1m->body_len += chunk;
			max -= ret;
			ofs += ret;
			total += ret;
			h1m->state = size ? H1_MSG_DATA : H1_MSG_TRAILERS;
			if (!size)
				goto send_empty;
		}

		/* in MSG_DATA state, continue below */
		size = h1m->curr_len;
		break;
	}

	/* we have in <size> the exact number of bytes we need to copy from
	 * the H1 buffer. We need to check this against the connection's and
	 * the stream's send windows, and to ensure that this fits in the max
	 * frame size and in the buffer's available space minus 9 bytes (for
	 * the frame header). The connection's flow control is applied last so
	 * that we can use a separate list of streams which are immediately
	 * unblocked on window opening. Note: we don't implement padding.
	 */

	if (size > max)
		size = max;

	if (size > h2s->mws)
		size = h2s->mws;

	if (size <= 0) {
		h2s->flags |= H2_SF_BLK_SFCTL;
		goto end;
	}

	if (h2c->mfs && size > h2c->mfs)
		size = h2c->mfs;

	if (size + 9 > outbuf.size) {
		/* we have an opportunity for enlarging the too small
		 * available space, let's try.
		 */
		if (b_space_wraps(&h2c->mbuf))
			goto realign_again;
		size = outbuf.size - 9;
	}

	if (size <= 0) {
		h2c->flags |= H2_CF_MUX_MFULL;
		h2s->flags |= H2_SF_BLK_MROOM;
		goto end;
	}

	if (size > h2c->mws)
		size = h2c->mws;

	if (size <= 0) {
		h2s->flags |= H2_SF_BLK_MFCTL;
		goto end;
	}

	/* copy whatever we can */
	blk1 = blk2 = NULL; // silence a maybe-uninitialized warning
	ret = b_getblk_nc(buf, &blk1, &len1, &blk2, &len2, ofs, max);
	if (ret == 1)
		len2 = 0;

	if (!ret || len1 + len2 < size) {
		/* FIXME: must normally never happen */
		h2s_error(h2s, H2_ERR_INTERNAL_ERROR);
		goto end;
	}

	/* limit len1/len2 to size */
	if (len1 + len2 > size) {
		int sub = len1 + len2 - size;

		if (len2 > sub)
			len2 -= sub;
		else {
			sub -= len2;
			len2 = 0;
			len1 -= sub;
		}
	}

	/* now let's copy this this into the output buffer */
	memcpy(outbuf.area + 9, blk1, len1);
	if (len2)
		memcpy(outbuf.area + 9 + len1, blk2, len2);

 send_empty:
	/* we may need to add END_STREAM */
	/* FIXME: we should also detect shutdown(w) below, but how ? Maybe we
	 * could rely on the MSG_MORE flag as a hint for this ?
	 *
	 * FIXME: what we do here is not correct because we send end_stream
	 * before knowing if we'll have to send a HEADERS frame for the
	 * trailers. More importantly we're not consuming the trailing CRLF
	 * after the end of trailers, so it will be left to the caller to
	 * eat it. The right way to do it would be to measure trailers here
	 * and to send ES only if there are no trailers.
	 *
	 */
	if (((h1m->flags & H1_MF_CLEN) && !(h1m->curr_len - size)) ||
	    !h1m->curr_len || h1m->state >= H1_MSG_DONE)
		es_now = 1;

	/* update the frame's size */
	h2_set_frame_size(outbuf.area, size);

	if (es_now)
		outbuf.area[4] |= H2_F_DATA_END_STREAM;

	/* commit the H2 response */
	b_add(&h2c->mbuf, size + 9);

	/* consume incoming H1 response */
	if (size > 0) {
		max -= size;
		ofs += size;
		total += size;
		h1m->curr_len -= size;
		h2s->mws -= size;
		h2c->mws -= size;

		if (size && !h1m->curr_len && (h1m->flags & H1_MF_CHNK)) {
			h1m->state = H1_MSG_CHUNK_CRLF;
			goto new_frame;
		}
	}

	if (es_now) {
		if (h2s->st == H2_SS_OPEN)
			h2s->st = H2_SS_HLOC;
		else
			h2s_close(h2s);

		if (!(h1m->flags & H1_MF_CHNK)) {
			// trim any possibly pending data (eg: inconsistent content-length)
			total += max;
			ofs += max;
			max = 0;

			h1m->state = H1_MSG_DONE;
		}

		h2s->flags |= H2_SF_ES_SENT;
	}

 end:
	trace("[%d] sent simple H2 DATA response (sid=%d) = %d bytes out (%u in, st=%s, ep=%u, es=%s, h2cws=%d h2sws=%d) data=%u", h2c->st0, h2s->id, size+9, (unsigned int)total, h1_msg_state_str(h1m->state), h1m->err_pos, h1_msg_state_str(h1m->err_state), h2c->mws, h2s->mws, (unsigned int)b_data(buf));
	return total;
}

/* Called from the upper layer, to subscribe to events, such as being able to send */
static int h2_subscribe(struct conn_stream *cs, int event_type, void *param)
{
	struct wait_list *sw;
	struct h2s *h2s = cs->ctx;
	struct h2c *h2c = h2s->h2c;

	switch (event_type) {
	case SUB_CAN_RECV:
		sw = param;
		if (!(sw->wait_reason & SUB_CAN_RECV)) {
			sw->wait_reason |= SUB_CAN_RECV;
			sw->handle = h2s;
			h2s->recv_wait_list = sw;
		}
		return 0;
	case SUB_CAN_SEND:
		sw = param;
		if (!(sw->wait_reason & SUB_CAN_SEND)) {
			sw->wait_reason |= SUB_CAN_SEND;
			sw->handle = h2s;
			if (h2s->flags & H2_SF_BLK_MFCTL)
				LIST_ADDQ(&h2c->fctl_list, &sw->list);
			else
				LIST_ADDQ(&h2c->send_list, &sw->list);
		}
		return 0;
	default:
		break;
	}
	return -1;


}

/* Called from the upper layer, to receive data */
static size_t h2_rcv_buf(struct conn_stream *cs, struct buffer *buf, size_t count, int flags)
{
	struct h2s *h2s = cs->ctx;
	size_t ret = 0;

	/* transfer possibly pending data to the upper layer */
	ret = b_xfer(buf, &h2s->rxbuf, count);

	if (b_data(&h2s->rxbuf))
		cs->flags |= CS_FL_RCV_MORE;
	else {
		cs->flags &= ~CS_FL_RCV_MORE;
		if (cs->flags & CS_FL_REOS)
			cs->flags |= CS_FL_EOS;
		if (b_size(&h2s->rxbuf)) {
			b_free(&h2s->rxbuf);
			offer_buffers(NULL, tasks_run_queue);
		}
	}

	return ret;
}

/* Called from the upper layer, to send data */
static size_t h2_snd_buf(struct conn_stream *cs, struct buffer *buf, size_t count, int flags)
{
	struct h2s *h2s = cs->ctx;
	size_t total = 0;
	size_t ret;

	if (!(h2s->flags & H2_SF_OUTGOING_DATA) && count)
		h2s->flags |= H2_SF_OUTGOING_DATA;

	while (h2s->h1m.state < H1_MSG_DONE && count) {
		if (h2s->h1m.state <= H1_MSG_LAST_LF) {
			ret = h2s_frt_make_resp_headers(h2s, buf, total, count);
		}
		else if (h2s->h1m.state < H1_MSG_TRAILERS) {
			ret = h2s_frt_make_resp_data(h2s, buf, total, count);
		}
		else if (h2s->h1m.state == H1_MSG_TRAILERS) {
			/* consume the trailers if any (we don't forward them for now) */
			ret = h1_measure_trailers(buf, total, count);

			if (unlikely((int)ret <= 0)) {
				if ((int)ret < 0)
					h2s_error(h2s, H2_ERR_INTERNAL_ERROR);
				break;
			}
			// trim any possibly pending data (eg: extra CR-LF, ...)
			total += count;
			count  = 0;
			h2s->h1m.state = H1_MSG_DONE;
			break;
		}
		else {
			cs->flags |= CS_FL_ERROR;
			break;
		}

		total += ret;
		count -= ret;

		if (h2s->st >= H2_SS_ERROR)
			break;

		if (h2s->flags & H2_SF_BLK_ANY)
			break;
	}

	if (h2s->st >= H2_SS_ERROR) {
		/* trim any possibly pending data after we close (extra CR-LF,
		 * unprocessed trailers, abnormal extra data, ...)
		 */
		total += count;
		count = 0;
	}

	/* RST are sent similarly to frame acks */
	if (h2s->st == H2_SS_ERROR || h2s->flags & H2_SF_RST_RCVD) {
		cs->flags |= CS_FL_ERROR;
		if (h2s_send_rst_stream(h2s->h2c, h2s) > 0)
			h2s_close(h2s);
	}

	b_del(buf, total);
	if (total > 0) {
		conn_xprt_want_send(h2s->h2c->conn);
		if (!(h2s->h2c->wait_list.wait_reason & SUB_CAN_SEND))
			tasklet_wakeup(h2s->h2c->wait_list.task);
	}
	return total;
}

/* for debugging with CLI's "show fd" command */
static void h2_show_fd(struct buffer *msg, struct connection *conn)
{
	struct h2c *h2c = conn->mux_ctx;
	struct h2s *h2s;
	struct wait_list *sw;
	struct eb32_node *node;
	int fctl_cnt = 0;
	int send_cnt = 0;
	int tree_cnt = 0;
	int orph_cnt = 0;

	if (!h2c)
		return;

	list_for_each_entry(sw, &h2c->fctl_list, list)
		fctl_cnt++;

	list_for_each_entry(sw, &h2c->send_list, list)
		send_cnt++;

	node = eb32_first(&h2c->streams_by_id);
	while (node) {
		h2s = container_of(node, struct h2s, by_id);
		tree_cnt++;
		if (!h2s->cs)
			orph_cnt++;
		node = eb32_next(node);
	}

	chunk_appendf(msg, " st0=%d err=%d maxid=%d lastid=%d flg=0x%08x nbst=%u nbcs=%u"
		      " fctl_cnt=%d send_cnt=%d tree_cnt=%d orph_cnt=%d dbuf=%u/%u mbuf=%u/%u",
		      h2c->st0, h2c->errcode, h2c->max_id, h2c->last_sid, h2c->flags,
		      h2c->nb_streams, h2c->nb_cs, fctl_cnt, send_cnt, tree_cnt, orph_cnt,
		      (unsigned int)b_data(&h2c->dbuf), (unsigned int)b_size(&h2c->dbuf),
		      (unsigned int)b_data(&h2c->mbuf), (unsigned int)b_size(&h2c->mbuf));
}

/*******************************************************/
/* functions below are dedicated to the config parsers */
/*******************************************************/

/* config parser for global "tune.h2.header-table-size" */
static int h2_parse_header_table_size(char **args, int section_type, struct proxy *curpx,
                                      struct proxy *defpx, const char *file, int line,
                                      char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	h2_settings_header_table_size = atoi(args[1]);
	if (h2_settings_header_table_size < 4096 || h2_settings_header_table_size > 65536) {
		memprintf(err, "'%s' expects a numeric value between 4096 and 65536.", args[0]);
		return -1;
	}
	return 0;
}

/* config parser for global "tune.h2.initial-window-size" */
static int h2_parse_initial_window_size(char **args, int section_type, struct proxy *curpx,
                                        struct proxy *defpx, const char *file, int line,
                                        char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	h2_settings_initial_window_size = atoi(args[1]);
	if (h2_settings_initial_window_size < 0) {
		memprintf(err, "'%s' expects a positive numeric value.", args[0]);
		return -1;
	}
	return 0;
}

/* config parser for global "tune.h2.max-concurrent-streams" */
static int h2_parse_max_concurrent_streams(char **args, int section_type, struct proxy *curpx,
                                           struct proxy *defpx, const char *file, int line,
                                           char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	h2_settings_max_concurrent_streams = atoi(args[1]);
	if (h2_settings_max_concurrent_streams < 0) {
		memprintf(err, "'%s' expects a positive numeric value.", args[0]);
		return -1;
	}
	return 0;
}


/****************************************/
/* MUX initialization and instanciation */
/***************************************/

/* The mux operations */
const struct mux_ops h2_ops = {
	.init = h2_init,
	.wake = h2_wake,
	.update_poll = h2_update_poll,
	.snd_buf = h2_snd_buf,
	.rcv_buf = h2_rcv_buf,
	.subscribe = h2_subscribe,
	.attach = h2_attach,
	.detach = h2_detach,
	.shutr = h2_shutr,
	.shutw = h2_shutw,
	.show_fd = h2_show_fd,
	.flags = MX_FL_CLEAN_ABRT,
	.name = "H2",
};

/* PROTO selection : this mux registers PROTO token "h2" */
static struct mux_proto_list mux_proto_h2 =
	{ .token = IST("h2"), .mode = PROTO_MODE_HTTP, .side = PROTO_SIDE_FE, .mux = &h2_ops };

/* config keyword parsers */
static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "tune.h2.header-table-size",      h2_parse_header_table_size      },
	{ CFG_GLOBAL, "tune.h2.initial-window-size",    h2_parse_initial_window_size    },
	{ CFG_GLOBAL, "tune.h2.max-concurrent-streams", h2_parse_max_concurrent_streams },
	{ 0, NULL, NULL }
}};

static void __h2_deinit(void)
{
	pool_destroy(pool_head_h2s);
	pool_destroy(pool_head_h2c);
}

__attribute__((constructor))
static void __h2_init(void)
{
	register_mux_proto(&mux_proto_h2);
	cfg_register_keywords(&cfg_kws);
	hap_register_post_deinit(__h2_deinit);
	pool_head_h2c = create_pool("h2c", sizeof(struct h2c), MEM_F_SHARED);
	pool_head_h2s = create_pool("h2s", sizeof(struct h2s), MEM_F_SHARED);
}
