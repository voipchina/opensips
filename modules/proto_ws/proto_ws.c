/*
 * Copyright (C) 2015 - OpenSIPS Foundation
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * History:
 * -------
 *  2015-02-11  first version (razvanc)
 */

#include <errno.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <poll.h>

#include "../../pt.h"
#include "../../sr_module.h"
#include "../../net/api_proto.h"
#include "../../net/api_proto_net.h"
#include "../../socket_info.h"
#include "../../tsend.h"
#include "../../receive.h"
#include "proto_ws.h"
#include "ws_handshake.h"
#include "ws.h"

static int mod_init(void);
static int proto_ws_init(struct proto_info *pi);
static int proto_ws_init_listener(struct socket_info *si);
static int proto_ws_send(struct socket_info* send_sock,
		char* buf, unsigned int len, union sockaddr_union* to, int id);

static int ws_read_req(struct tcp_connection* con, int* bytes_read);
static int ws_conn_init(struct tcp_connection* c);
static void ws_conn_clean(struct tcp_connection* c);



static cmd_export_t cmds[] = {
	{"proto_init", (cmd_function)proto_ws_init, 0, 0, 0, 0},
	{0,0,0,0,0,0}
};


static param_export_t params[] = {
	{0, 0, 0}
};


struct module_exports exports = {
	PROTO_PREFIX "ws",  /* module name*/
	MOD_TYPE_DEFAULT,/* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS, /* dlopen flags */
	NULL,            /* OpenSIPS module dependencies */
	cmds,       /* exported functions */
	0,          /* exported async functions */
	params,     /* module parameters */
	0,          /* exported statistics */
	0,          /* exported MI functions */
	0,          /* exported pseudo-variables */
	0,          /* extra processes */
	mod_init,   /* module initialization function */
	0,          /* response function */
	0,          /* destroy function */
	0,          /* per-child init function */
};



static int proto_ws_init(struct proto_info *pi)
{
	pi->default_port		= WS_DEFAULT_PORT;

	pi->tran.init_listener	= proto_ws_init_listener;
	pi->tran.send			= proto_ws_send;

	pi->net.flags			= PROTO_NET_USE_TCP;
	pi->net.read			= (proto_net_read_f)ws_read_req;
	pi->net.conn_init		= ws_conn_init;
	pi->net.conn_clean		= ws_conn_clean;

	return 0;
}


static int mod_init(void)
{
	LM_INFO("initializing WebSocket protocol\n");
	return 0;
}



static int proto_ws_init_listener(struct socket_info *si)
{
	/* we do not do anything particular to TCP plain here, so
	 * transparently use the generic listener init from net TCP layer */
	return tcp_init_listener(si);
}


static int ws_conn_init(struct tcp_connection* c)
{
	struct ws_data *d;
	struct ws_hs *hs;

	d = (struct ws_data *)shm_malloc(sizeof(struct ws_data));
	if (!d) {
		LM_ERR("Failed to create ws in shm memory\n");
		return -1;
	}
	hs = (struct ws_hs *)shm_malloc(sizeof(struct ws_hs));
	if (!hs) {
		LM_ERR("Failed to alloc handshake structure in shm memory\n");
		shm_free(d);
		return -1;
	}
	memset(hs, 0, sizeof(struct ws_hs));

	/* initialize all fields here */
	d->state = WS_CON_HANDSHAKE;
	d->handshake = hs;
	c->proto_data = (void *)d;
	return 0;
}


static void ws_conn_clean(struct tcp_connection* c)
{
	struct ws_data *wsd = (struct ws_data *)c->proto_data;
	wsd->handshake = NULL;
	shm_free(wsd);
	c->proto_data = NULL;
}




/**************  WRITE related functions ***************/



/*! \brief Finds a tcpconn & sends on it */
static int proto_ws_send(struct socket_info* send_sock,
											char* buf, unsigned int len,
											union sockaddr_union* to, int id)
{
	struct tcp_connection *c;
	struct ip_addr ip;
	int port;
	int fd, n;

	if (to){
		su2ip_addr(&ip, to);
		port=su_getport(to);
		n = tcp_conn_get(id, &ip, port, &c, &fd);
	}else if (id){
		n = tcp_conn_get(id, 0, 0, &c, &fd);
	}else{
		LM_CRIT("prot_tls_send called with null id & to\n");
		return -1;
	}

	if (n<0) {
		/* error during conn get, return with error too */
		LM_ERR("failed to aquire connection\n");
		return -1;
	}

	/* was connection found ?? */
	if (c==0) {
		if (tcp_no_new_conn) {
			return -1;
		}
		/* XXX: currently cannot work as a WebSocket client */
		LM_ERR("no open tcp connection found. "
				"WebSocket connect is not supported!\n");
		return -1;
	}

	/* now we have a connection, let's what we can do with it */
	/* BE CAREFUL now as we need to release the conn before exiting !!! */
	if (fd==-1) {
		/* connection is not writable because of its state */
		/* return error, nothing to do about it */
		tcp_conn_release(c, 0);
		return -1;
	}

	LM_DBG("sending via fd %d...\n",fd);

	n = ws_req_write(c, fd, buf, len);
	tcp_conn_set_lifetime( c, tcp_con_lifetime);

	LM_DBG("after write: c= %p n=%d fd=%d\n",c, n, fd);
	LM_DBG("buf=\n%.*s\n", (int)len, buf);
	if (n<0){
		LM_ERR("failed to send\n");
		c->state=S_CONN_BAD;
		tcp_conn_release(c, 0);
		close(fd);
		return -1;
	}
	close(fd);
	tcp_conn_release(c, 0);
	return n;
}


/* Responsible for writing the TCP send chunks - called under con write lock
 *	* if returns = 1 : the connection will be released for more writting
 *	* if returns = 0 : the connection will be released
 *	* if returns < 0 : the connection will be released as BAD /  broken
 */
#if 0
static int ws_write_async_req(struct tcp_connection* con)
{
	int n,left;
	struct tcp_send_chunk *chunk;
	struct tcp_data *d = (struct tcp_data*)con->proto_data;

	if (d->async_chunks_no == 0) {
		LM_DBG("The connection has been triggered "
		" for a write event - but we have no pending write chunks\n");
		return 0;
	}

next_chunk:
	chunk=d->async_chunks[0];
again:
	left = (int)((chunk->buf+chunk->len)-chunk->pos);
	LM_DBG("Trying to send %d bytes from chunk %p in conn %p - %d %d \n",
		   left,chunk,con,chunk->ticks,get_ticks());
	n=send(con->fd, chunk->pos, left,
#ifdef HAVE_MSG_NOSIGNAL
			MSG_NOSIGNAL
#else
			0
#endif
	);

	if (n<0) {
		if (errno==EINTR)
			goto again;
		else if (errno==EAGAIN || errno==EWOULDBLOCK) {
			LM_DBG("Can't finish to write chunk %p on conn %p\n",
				   chunk,con);
			/* report back we have more writting to be done */
			return 1;
		} else {
			LM_ERR("Error occured while sending async chunk %d (%s)\n",
				   errno,strerror(errno));
			/* report the conn as broken */
			return -1;
		}
	}

	if (n < left) {
		/* partial write */
		chunk->pos+=n;
		goto again;
	} else {
		/* written a full chunk - move to the next one, if any */
		shm_free(chunk);
		d->async_chunks_no--;
		if (d->async_chunks_no == 0) {
			LM_DBG("We have finished writing all our async chunks in %p\n",con);
			d->oldest_chunk=0;
			/*  report back everything ok */
			return 0;
		} else {
			LM_DBG("We still have %d chunks pending on %p\n",
					d->async_chunks_no,con);
			memmove(&d->async_chunks[0],&d->async_chunks[1],
					d->async_chunks_no * sizeof(struct tcp_send_chunk*));
			d->oldest_chunk = d->async_chunks[0]->ticks;
			goto next_chunk;
		}
	}
	return 0;
}
#endif



/**************  READ related functions ***************/




/* Responsible for reading the request
 *	* if returns >= 0 : the connection will be released
 *	* if returns <  0 : the connection will be released as BAD / broken
 */
static int ws_read_req(struct tcp_connection* con, int* bytes_read)
{
	struct ws_data* wsd;
	int size;

	wsd = (struct ws_data *)con->proto_data;

	if (wsd->state != WS_CON_HANDSHAKE_DONE) {

		size = ws_handshake(con);
		if (size < 0) {
			LM_ERR("cannot complete WebSocket handshake\n");
			goto error;
		}
		if (size == 0)
			goto done;
	}
	if (wsd->state == WS_CON_HANDSHAKE_DONE && ws_process(con) < 0)
		goto error;

done:
	return 0;
error:
	/* connection will be released as ERROR */
	return -1;
}



