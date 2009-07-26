
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

/* only call it from the worker context the con belongs to */
void worker_con_put(liConnection *con); /* worker.c */

static void parse_request_body(liConnection *con) {
	if ((con->state > LI_CON_STATE_HANDLE_MAINVR || con->mainvr->state >= LI_VRS_READ_CONTENT) && !con->in->is_closed) {
		li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_READ);
		if (con->mainvr->request.content_length == -1) {
			/* TODO: parse chunked encoded request body, filters */
			li_chunkqueue_steal_all(con->in, con->raw_in);
		} else {
			if (con->in->bytes_in < con->mainvr->request.content_length) {
				li_chunkqueue_steal_len(con->in, con->raw_in, con->mainvr->request.content_length - con->in->bytes_in);
			}
			if (con->in->bytes_in == con->mainvr->request.content_length) {
				con->in->is_closed = TRUE;
				li_ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_READ);
			}
		}
		li_vrequest_handle_request_body(con->mainvr);
	} else {
		li_ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_READ);
	}
}

static void forward_response_body(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (con->state >= LI_CON_STATE_HANDLE_MAINVR) {
		if (!con->response_headers_sent) {
			con->response_headers_sent = TRUE;
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "write response headers");
			}
			li_response_send_headers(con);
		}

		if (vr->response.transfer_encoding & LI_HTTP_TRANSFER_ENCODING_CHUNKED) {
			li_filter_chunked_encode(con, con->raw_out, con->out);
		} else {
			li_chunkqueue_steal_all(con->raw_out, con->out);
		}
		if (con->out->is_closed) con->raw_out->is_closed = TRUE;
		if (con->raw_out->length > 0) {
			li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
		} else {
			li_ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
		}
	} else {
		li_ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
	}
}

static void connection_request_done(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(con->mainvr, "response end (keep_alive = %i)", con->keep_alive);
	}

	li_plugins_handle_close(con);

	if (con->keep_alive && g_atomic_int_get(&con->srv->state) == LI_SERVER_RUNNING) {
		li_connection_reset_keep_alive(con);
	} else {
		worker_con_put(con);
	}
}

static gboolean check_response_done(liConnection *con) {
	if (con->in->is_closed && con->raw_out->is_closed && 0 == con->raw_out->length) {
		connection_request_done(con);
		return TRUE;
	}
	return FALSE;
}

static void connection_close(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection closed");
	}

	li_plugins_handle_close(con);

	worker_con_put(con);
}

void li_connection_error(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection closed (error)");
	}

	li_plugins_handle_close(con);

	worker_con_put(con);
}

void li_connection_internal_error(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (con->response_headers_sent) {
		VR_ERROR(vr, "%s", "Couldn't send '500 Internal Error': headers already sent");
		li_connection_error(con);
	} else {
		liHttpVersion v;
		VR_ERROR(vr, "%s", "internal error");

		/* We only need the http version from the http request */
		v = con->mainvr->request.http_version;
		li_vrequest_reset(con->mainvr);
		con->mainvr->request.http_version = v;

		con->keep_alive = FALSE;
		con->mainvr->response.http_status = 500;
		con->state = LI_CON_STATE_WRITE; /* skips further vrequest handling */

		li_chunkqueue_reset(con->out);
		con->out->is_closed = TRUE;
		con->in->is_closed = TRUE;
		forward_response_body(con);
	}
}

static gboolean connection_handle_read(liConnection *con) {
	liVRequest *vr = con->mainvr;

	if (con->raw_in->length == 0) return TRUE;

	if (con->state == LI_CON_STATE_KEEP_ALIVE) {
		/* stop keep alive timeout watchers */
		if (con->keep_alive_data.link) {
			g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
			con->keep_alive_data.link = NULL;
		}
		con->keep_alive_data.timeout = 0;
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

		con->keep_alive_requests++;
		/* disable keep alive if limit is reached */
		if (con->keep_alive_requests == CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_REQUESTS).number)
			con->keep_alive = FALSE;

		con->state = LI_CON_STATE_READ_REQUEST_HEADER;
		con->ts = CUR_TS(con->wrk);

	} else {
		if (con->state == LI_CON_STATE_REQUEST_START)
			con->state = LI_CON_STATE_READ_REQUEST_HEADER;
	}

	if (con->state == LI_CON_STATE_READ_REQUEST_HEADER && con->mainvr->state == LI_VRS_CLEAN) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "reading request header");
		}
		switch(li_http_request_parse(con->mainvr, &con->req_parser_ctx)) {
		case LI_HANDLER_GO_ON:
			break; /* go on */
		case LI_HANDLER_WAIT_FOR_EVENT:
			return TRUE;
		case LI_HANDLER_ERROR:
		case LI_HANDLER_COMEBACK: /* unexpected */
			/* unparsable header */
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "parsing header failed");
			}
			con->keep_alive = FALSE;
			con->mainvr->response.http_status = 400;
			li_vrequest_handle_direct(con->mainvr);
			con->state = LI_CON_STATE_WRITE;
			con->in->is_closed = TRUE;
			forward_response_body(con);
			return TRUE;
		}

		con->wrk->stats.requests++;

		/* headers ready */
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "validating request header");
		}
		if (!li_request_validate_header(con)) {
			/* skip mainvr handling */
			con->state = LI_CON_STATE_WRITE;
			con->keep_alive = FALSE;
			con->in->is_closed = TRUE;
			forward_response_body(con);
		} else {
			/* When does a client ask for 100 Continue? probably not while trying to ddos us
			 * as post content probably goes to a dynamic backend anyway, we don't
			 * care about the rare cases we could determine that we don't want a request at all
			 * before sending it to a backend - so just send the stupid header
			 */
			if (con->expect_100_cont) {
				if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
					VR_DEBUG(vr, "%s", "send 100 Continue");
				}
				li_chunkqueue_append_mem(con->raw_out, CONST_STR_LEN("HTTP/1.1 100 Continue\r\n\r\n"));
				con->expect_100_cont = FALSE;
				li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
			}

			con->state = LI_CON_STATE_HANDLE_MAINVR;
			li_action_enter(con->mainvr, con->srv->mainaction);
			li_vrequest_handle_request_headers(con->mainvr);
		}
	} else {
		parse_request_body(con);
	}

	return TRUE;
}

static void connection_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liNetworkStatus res;
	goffset write_max;
	goffset transferred;
	liConnection *con = (liConnection*) w->data;

	if (revents & EV_READ) {
		if (con->in->is_closed) {
			/* don't read the next request before current one is done */
			li_ev_io_rem_events(loop, w, EV_READ);
		} else {
			transferred = con->raw_in->length;

			res = li_network_read(con->mainvr, w->fd, con->raw_in);

			transferred = con->raw_in->length - transferred;
			con->wrk->stats.bytes_in += transferred;
			con->stats.bytes_in += transferred;

			switch (res) {
			case LI_NETWORK_STATUS_SUCCESS:
				if (!connection_handle_read(con)) return;
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				_ERROR(con->srv, con->mainvr, "%s", "network read fatal error");
				li_connection_error(con);
				return;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				con->raw_in->is_closed = TRUE;
				shutdown(w->fd, SHUT_RD);
				connection_close(con);
				return;
			case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				/* TODO: aio */
				li_ev_io_rem_events(loop, w, EV_READ);
				break;
			}
		}
	}

	if (revents & EV_WRITE) {
		if (con->raw_out->length > 0) {
			if (con->throttled) {
				write_max = MIN(con->throttle.con.magazine, 256*1024);
			} else {
				write_max = 256*1024; /* 256kB */
			}

			if (write_max > 0) {
				transferred = con->raw_out->length;

				res = li_network_write(con->mainvr, w->fd, con->raw_out, write_max);

				transferred = transferred - con->raw_out->length;
				con->wrk->stats.bytes_out += transferred;
				con->stats.bytes_out += transferred;

				switch (res) {
				case LI_NETWORK_STATUS_SUCCESS:
					li_vrequest_joblist_append(con->mainvr);
					break;
				case LI_NETWORK_STATUS_FATAL_ERROR:
					_ERROR(con->srv, con->mainvr, "%s", "network write fatal error");
					li_connection_error(con);
					return;
				case LI_NETWORK_STATUS_CONNECTION_CLOSE:
					connection_close(con);
					return;
				case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
					break;
				case LI_NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
					li_ev_io_rem_events(loop, w, EV_WRITE);
					_ERROR(con->srv, con->mainvr, "%s", "TODO: wait for aio");
					/* TODO: aio */
					break;
				}
			} else {
				transferred = 0;
			}

			if ((ev_now(loop) - con->stats.last_avg) >= 5.0) {
				con->stats.bytes_out_5s_diff = con->wrk->stats.bytes_out - con->wrk->stats.bytes_out_5s;
				con->stats.bytes_out_5s = con->stats.bytes_out;
				con->stats.bytes_in_5s_diff = con->stats.bytes_in - con->stats.bytes_in_5s;
				con->stats.bytes_in_5s = con->stats.bytes_in;
				con->stats.last_avg = ev_now(loop);
			}

			if (con->throttled) {
				con->throttle.con.magazine -= transferred;
				/*g_print("%p wrote %"G_GINT64_FORMAT"/%"G_GINT64_FORMAT" bytes, mags: %d/%d, queued: %s\n", (void*)con,
				transferred, write_max, con->throttle.pool.magazine, con->throttle.con.magazine, con->throttle.pool.queued ? "yes":"no");*/
				if (con->throttle.con.magazine <= 0) {
					li_ev_io_rem_events(loop, w, EV_WRITE);
					li_waitqueue_push(&con->wrk->throttle_queue, &con->throttle.wqueue_elem);
				}

				if (con->throttle.pool.ptr && con->throttle.pool.magazine <= MAX(write_max,0) && !con->throttle.pool.queued) {
					liThrottlePool *pool = con->throttle.pool.ptr;
					g_atomic_int_inc(&pool->num_cons);
					g_queue_push_tail_link(pool->queues[con->wrk->ndx+pool->current_queue[con->wrk->ndx]], &con->throttle.pool.lnk);
					con->throttle.pool.queued = TRUE;
				}
			}
		} else {
			_DEBUG(con->srv, con->mainvr, "%s", "write event for empty queue");
			li_ev_io_rem_events(loop, w, EV_WRITE);
		}
	}

	if ((con->io_timeout_elem.ts + 1.0) < ev_now(loop))
		li_waitqueue_push(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	check_response_done(con);
}

static void connection_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liConnection *con = (liConnection*) w->data;
	UNUSED(loop); UNUSED(revents);
	worker_con_put(con);
}

static liHandlerResult mainvr_handle_response_headers(liVRequest *vr) {
	liConnection *con = vr->con;
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "read request/handle response header");
	}
	parse_request_body(con);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult mainvr_handle_response_body(liVRequest *vr) {
	liConnection *con = vr->con;
	if (check_response_done(con)) return LI_HANDLER_GO_ON;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "write response");
	}

	parse_request_body(con);
	forward_response_body(con);

	if (check_response_done(con)) return LI_HANDLER_GO_ON;

	return LI_HANDLER_GO_ON;
}

static liHandlerResult mainvr_handle_response_error(liVRequest *vr) {
	li_connection_internal_error(vr->con);
	return LI_HANDLER_GO_ON;
}

static liHandlerResult mainvr_handle_request_headers(liVRequest *vr) {
	/* start reading input */
	parse_request_body(vr->con);
	return LI_HANDLER_GO_ON;
}

liConnection* li_connection_new(liWorker *wrk) {
	liServer *srv = wrk->srv;
	liConnection *con = g_slice_new0(liConnection);
	con->wrk = wrk;
	con->srv = srv;

	con->state = LI_CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_init(&con->sock_watcher, connection_cb);
	ev_io_set(&con->sock_watcher, -1, 0);
	con->sock_watcher.data = con;
	con->remote_addr_str = g_string_sized_new(INET6_ADDRSTRLEN);
	con->keep_alive = TRUE;

	con->raw_in  = li_chunkqueue_new();
	con->raw_out = li_chunkqueue_new();

	con->mainvr = li_vrequest_new(con,
		mainvr_handle_response_headers,
		mainvr_handle_response_body,
		mainvr_handle_response_error,
		mainvr_handle_request_headers);
	li_http_request_parser_init(&con->req_parser_ctx, &con->mainvr->request, con->raw_in);

	con->in      = con->mainvr->vr_in;
	con->out     = con->mainvr->vr_out;

	li_chunkqueue_use_limit(con->raw_in, con->mainvr);
	li_chunkqueue_use_limit(con->raw_out, con->mainvr);
	li_chunkqueue_set_limit(con->mainvr->vr_in, con->raw_in->limit);
	li_chunkqueue_set_limit(con->mainvr->vr_out, con->raw_out->limit);
	li_chunkqueue_set_limit(con->mainvr->in, con->raw_in->limit);
	li_chunkqueue_set_limit(con->mainvr->out, con->raw_out->limit);

	con->keep_alive_data.link = NULL;
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	ev_init(&con->keep_alive_data.watcher, connection_keepalive_cb);
	con->keep_alive_data.watcher.data = con;

	con->io_timeout_elem.data = con;

	con->throttle.wqueue_elem.data = con;
	con->throttle.pool.lnk.data = con;
	con->throttle.ip.lnk.data = con;

	return con;
}

void li_connection_reset(liConnection *con) {
	con->state = LI_CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	li_server_socket_release(con->srv_sock);
	con->srv_sock = NULL;

	ev_io_stop(con->wrk->loop, &con->sock_watcher);
	if (con->sock_watcher.fd != -1) {
		if (con->raw_in->is_closed) { /* read already shutdown */
			shutdown(con->sock_watcher.fd, SHUT_WR);
			close(con->sock_watcher.fd);
		} else {
			li_worker_add_closing_socket(con->wrk, con->sock_watcher.fd);
		}
	}
	ev_io_set(&con->sock_watcher, -1, 0);

	li_vrequest_reset(con->mainvr);
	li_http_request_parser_reset(&con->req_parser_ctx);

	g_string_truncate(con->remote_addr_str, 0);
	li_sockaddr_clear(&con->remote_addr);
	con->keep_alive = TRUE;

	li_chunkqueue_reset(con->raw_in);
	li_chunkqueue_reset(con->raw_out);
	li_cqlimit_reset(con->raw_in->limit);
	li_cqlimit_reset(con->raw_out->limit);

	if (con->keep_alive_data.link) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
	con->keep_alive_requests = 0;

	/* reset stats */
	con->stats.bytes_in = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.last_avg = 0;

	/* remove from timeout queue */
	li_waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	/* remove from throttle queue */
	li_waitqueue_remove(&con->wrk->throttle_queue, &con->throttle.wqueue_elem);

	if (con->throttle.pool.ptr) {
		if (con->throttle.pool.queued) {
			liThrottlePool *pool = con->throttle.pool.ptr;
			g_queue_unlink(pool->queues[con->wrk->ndx+pool->current_queue[con->wrk->ndx]], &con->throttle.pool.lnk);
			g_atomic_int_add(&con->throttle.pool.ptr->num_cons, -1);
			con->throttle.pool.queued = FALSE;
		}
		g_atomic_int_add(&con->throttle.pool.ptr->magazine, con->throttle.pool.magazine);
		g_atomic_int_add(&con->throttle.pool.ptr->magazine, con->throttle.ip.magazine);
		g_atomic_int_add(&con->throttle.pool.ptr->magazine, con->throttle.con.magazine);
		con->throttle.pool.magazine = 0;
		con->throttle.ip.magazine = 0;
		con->throttle.con.magazine = 0;
		con->throttle.pool.ptr = NULL;
	}

	if (con->throttle.ip.ptr) {
		if (con->throttle.ip.queued) {
			liThrottlePool *pool = con->throttle.ip.ptr;
			g_queue_unlink(pool->queues[con->wrk->ndx+pool->current_queue[con->wrk->ndx]], &con->throttle.ip.lnk);
			g_atomic_int_add(&con->throttle.ip.ptr->num_cons, -1);
			con->throttle.ip.queued = FALSE;
		}
		g_atomic_int_add(&con->throttle.ip.ptr->magazine, con->throttle.ip.magazine);
		g_atomic_int_add(&con->throttle.ip.ptr->magazine, con->throttle.con.magazine);
		con->throttle.ip.ptr = NULL;
	}

	con->throttle.con.rate = 0;
	con->throttle.pool.magazine = 0;
	con->throttle.ip.magazine = 0;
	con->throttle.con.magazine = 0;
	con->throttled = FALSE;
}

void server_check_keepalive(liServer *srv);
void li_connection_reset_keep_alive(liConnection *con) {
	liVRequest *vr = con->mainvr;
	ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
	{
		con->keep_alive_data.max_idle = CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_IDLE).number;
		if (con->keep_alive_data.max_idle == 0) {
			worker_con_put(con);
			return;
		}
		if (con->keep_alive_data.max_idle >= con->srv->keep_alive_queue_timeout) {
			/* queue is sorted by con->keep_alive_data.timeout */
			gboolean need_start = (0 == con->wrk->keep_alive_queue.length);
			con->keep_alive_data.timeout = ev_now(con->wrk->loop) + con->srv->keep_alive_queue_timeout;
			g_queue_push_tail(&con->wrk->keep_alive_queue, con);
			con->keep_alive_data.link = g_queue_peek_tail_link(&con->wrk->keep_alive_queue);
			if (need_start)
				li_worker_check_keepalive(con->wrk);
		} else {
			ev_timer_set(&con->keep_alive_data.watcher, con->keep_alive_data.max_idle, 0);
			ev_timer_start(con->wrk->loop, &con->keep_alive_data.watcher);
		}
	}

	con->state = LI_CON_STATE_KEEP_ALIVE;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	li_ev_io_set_events(con->wrk->loop, &con->sock_watcher, EV_READ);
	con->keep_alive = TRUE;

	con->raw_out->is_closed = FALSE;

	li_vrequest_reset(con->mainvr);
	li_http_request_parser_reset(&con->req_parser_ctx);

	con->ts = CUR_TS(con->wrk);

	/* reset stats */
	con->stats.bytes_in = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.last_avg = 0;

	/* remove from timeout queue */
	li_waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);
	/* remove from throttle queue */
	li_waitqueue_remove(&con->wrk->throttle_queue, &con->throttle.wqueue_elem);

	if (con->throttle.pool.ptr) {
		if (con->throttle.pool.queued) {
			liThrottlePool *pool = con->throttle.pool.ptr;
			g_queue_unlink(pool->queues[con->wrk->ndx+pool->current_queue[con->wrk->ndx]], &con->throttle.pool.lnk);
			g_atomic_int_add(&con->throttle.pool.ptr->num_cons, -1);
			con->throttle.pool.queued = FALSE;
		}
		g_atomic_int_add(&con->throttle.pool.ptr->magazine, con->throttle.pool.magazine);
		g_atomic_int_add(&con->throttle.pool.ptr->magazine, con->throttle.ip.magazine);
		g_atomic_int_add(&con->throttle.pool.ptr->magazine, con->throttle.con.magazine);
		con->throttle.pool.magazine = 0;
		con->throttle.ip.magazine = 0;
		con->throttle.con.magazine = 0;
		con->throttle.pool.ptr = NULL;
	}

	if (con->throttle.ip.ptr) {
		if (con->throttle.ip.queued) {
			liThrottlePool *pool = con->throttle.ip.ptr;
			g_queue_unlink(pool->queues[con->wrk->ndx+pool->current_queue[con->wrk->ndx]], &con->throttle.ip.lnk);
			g_atomic_int_add(&con->throttle.ip.ptr->num_cons, -1);
			con->throttle.ip.queued = FALSE;
		}
		g_atomic_int_add(&con->throttle.ip.ptr->magazine, con->throttle.ip.magazine);
		g_atomic_int_add(&con->throttle.ip.ptr->magazine, con->throttle.con.magazine);
		con->throttle.ip.ptr = NULL;
	}

	con->throttle.con.rate = 0;
	con->throttle.pool.magazine = 0;
	con->throttle.ip.magazine = 0;
	con->throttle.con.magazine = 0;
	con->throttled = FALSE;
}

void li_connection_free(liConnection *con) {
	con->state = LI_CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	li_server_socket_release(con->srv_sock);
	con->srv_sock = NULL;

	if (con->wrk)
		ev_io_stop(con->wrk->loop, &con->sock_watcher);
	if (con->sock_watcher.fd != -1) {
		/* just close it; _free should only be called on dead connections anyway */
		shutdown(con->sock_watcher.fd, SHUT_WR);
		close(con->sock_watcher.fd);
	}
	ev_io_set(&con->sock_watcher, -1, 0);
	g_string_free(con->remote_addr_str, TRUE);
	li_sockaddr_clear(&con->remote_addr);
	con->keep_alive = TRUE;

	li_chunkqueue_free(con->raw_in);
	li_chunkqueue_free(con->raw_out);

	li_vrequest_free(con->mainvr);
	li_http_request_parser_clear(&con->req_parser_ctx);

	if (con->keep_alive_data.link && con->wrk) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	if (con->wrk)
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

	g_slice_free(liConnection, con);
}

gchar *li_connection_state_str(liConnectionState state) {
	static const gchar *states[] = {
		"dead",
		"keep-alive",
		"request start",
		"read request header",
		"handle main vrequest",
		"write"
	};

	return (gchar*)states[state];
}