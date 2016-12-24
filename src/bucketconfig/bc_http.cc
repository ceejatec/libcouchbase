/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "internal.h"
#include "clconfig.h"
#include "bc_http.h"
#include <lcbio/ssl.h>
#include "ctx-log-inl.h"
#define LOGARGS(ht, lvlbase) ht->parent->settings, "htconfig", LCB_LOG_##lvlbase, __FILE__, __LINE__
#define LOGFMT "<%s:%s> "
#define LOGID(h) get_ctx_host(h->ioctx), get_ctx_port(h->ioctx)

using namespace lcb::clconfig;

static void io_error_handler(lcbio_CTX *, lcb_error_t);
static void on_connected(lcbio_SOCKET *, void *, lcb_error_t, lcbio_OSERR);
static lcb_error_t connect_next(HttpProvider *);
static void read_common(lcbio_CTX *, unsigned);
static lcb_error_t setup_request_header(HttpProvider *, const lcb_host_t *);

/**
 * Determine if we're in compatibility mode with the previous versions of the
 * library - where the idle timeout is disabled and a perpetual streaming
 * connection will always remain open (regardless of whether it was triggered
 * by start_refresh/get_refresh).
 */
bool HttpProvider::is_v220_compat() const {
    lcb_uint32_t setting =  parent->settings->bc_http_stream_time;
    return setting == (lcb_uint32_t)-1;
}

void HttpProvider::close_current()
{
    lcbio_timer_disarm(disconn_timer);
    if (ioctx) {
        lcbio_ctx_close(ioctx, NULL, NULL);
    } else if (creq){
        lcbio_connect_cancel(creq);
    }
    creq = NULL;
    ioctx = NULL;
}

/**
 * Call when there is an error in I/O. This includes read, write, connect
 * and timeouts.
 */
static lcb_error_t
io_error(HttpProvider *http, lcb_error_t origerr)
{
    lcb_confmon *mon = http->parent;
    lcb_settings *settings = mon->settings;

    http->close_current();

    http->creq = lcbio_connect_hl(
            mon->iot, settings, http->nodes, 0, settings->config_node_timeout,
            on_connected, http);
    if (http->creq) {
        return LCB_SUCCESS;
    }
    mon->provider_failed(http, origerr);
    lcbio_timer_disarm(http->io_timer);
    if (http->is_v220_compat() && http->parent->config != NULL) {
        lcb_log(LOGARGS(http, INFO), "HTTP node list finished. Trying to obtain connection from first node in list");
        if (!lcbio_timer_armed(http->as_reconnect)) {
            lcbio_timer_rearm(http->as_reconnect,
                http->settings().grace_next_cycle);
        }
    }
    return origerr;
}

void set_new_config(HttpProvider *http)
{
    const lcb_host_t *curhost;
    if (http->current_config) {
        http->current_config->decref();
    }

    curhost = lcbio_get_host(lcbio_ctx_sock(http->ioctx));
    http->current_config = http->last_parsed;
    http->current_config->incref();
    lcbvb_replace_host(http->current_config->vbc, curhost->host);
    http->parent->provider_got_config(http, http->current_config);
}

static lcb_error_t
process_chunk(HttpProvider *http, const void *buf, unsigned nbuf)
{
    lcb_error_t err = LCB_SUCCESS;
    char *term;
    int rv;
    lcbvb_CONFIG *cfgh;
    unsigned state, oldstate, diff;
    lcbht_RESPONSE *resp = lcbht_get_response(http->htp);

    oldstate = resp->state;
    state = lcbht_parse(http->htp, buf, nbuf);
    diff = state ^ oldstate;

    if (state & LCBHT_S_ERROR) {
        return LCB_PROTOCOL_ERROR;
    }

    if (diff & LCBHT_S_HEADER) {
        /* see that we got a success? */
        if (resp->status == 200) {
            /* nothing */
        } else if (resp->status == 404) {
            const int urlmode = http->settings().bc_http_urltype;
            err = LCB_BUCKET_ENOENT;

            if (++http->uritype > LCB_HTCONFIG_URLTYPE_COMPAT) {
                lcb_log(LOGARGS(http, ERR), LOGFMT "Got 404 on config stream. Assuming bucket does not exist as we've tried both URL types", LOGID(http));
                goto GT_HT_ERROR;

            } else if ((urlmode & LCB_HTCONFIG_URLTYPE_COMPAT) == 0) {
                lcb_log(LOGARGS(http, ERR), LOGFMT "Got 404 on config stream for terse URI. Compat URI disabled, so not trying", LOGID(http));

            } else {
                /* reissue the request; but wait for it to drain */
                lcb_log(LOGARGS(http, WARN), LOGFMT "Got 404 on config stream. Assuming terse URI not supported on cluster", LOGID(http));
                http->try_nexturi = 1;
                err = LCB_SUCCESS;
                goto GT_CHECKDONE;
            }
        } else if (resp->status == 401) {
            err = LCB_AUTH_ERROR;
        } else {
            err = LCB_ERROR;
        }

        GT_HT_ERROR:
        if (err != LCB_SUCCESS) {
            lcb_log(LOGARGS(http, ERR), LOGFMT "Got non-success HTTP status code %d", LOGID(http), resp->status);
            return err;
        }
    }

    GT_CHECKDONE:
    if (http->try_nexturi) {
        lcb_host_t *host;
        if (!(state & LCBHT_S_DONE)) {
            return LCB_SUCCESS;
        }
        host = lcbio_get_host(lcbio_ctx_sock(http->ioctx));
        http->try_nexturi = 0;
        if ((err = setup_request_header(http, host)) != LCB_SUCCESS) {
            return err;
        }

        /* reset the state? */
        lcbht_reset(http->htp);
        lcbio_ctx_put(http->ioctx, http->request_buf, strlen(http->request_buf));
        return LCB_SUCCESS;
    }

    if (http->settings().conntype == LCB_TYPE_CLUSTER) {
        /* don't bother with parsing the actual config */
        resp->body.nused = 0;
        return LCB_SUCCESS;
    }
    if (!(state & LCBHT_S_BODY)) {
        /* nothing to parse yet */
        return LCB_SUCCESS;
    }

    /* seek ahead for strstr */
    term = strstr(resp->body.base, CONFIG_DELIMITER);
    if (!term) {
        return LCB_SUCCESS;
    }

    *term = '\0';
    cfgh = lcbvb_create();
    if (!cfgh) {
        return LCB_CLIENT_ENOMEM;
    }
    rv = lcbvb_load_json(cfgh, resp->body.base);
    if (rv != 0) {
        lcb_log(LOGARGS(http, ERR), LOGFMT "Failed to parse a valid config from HTTP stream", LOGID(http));
        lcb_log_badconfig(LOGARGS(http, ERR), cfgh, resp->body.base);
        lcbvb_destroy(cfgh);
        return LCB_PROTOCOL_ERROR;
    }
    if (http->last_parsed) {
        http->last_parsed->decref();
    }
    http->last_parsed = ConfigInfo::create(cfgh, CLCONFIG_HTTP);
    http->generation++;

    /** Relocate the stream */
    lcb_string_erase_beginning(&resp->body,
        (term+sizeof(CONFIG_DELIMITER)-1)-resp->body.base);

    return LCB_SUCCESS;
}

/**
 * Common function to handle parsing the HTTP stream for both v0 and v1 io
 * implementations.
 */
static void
read_common(lcbio_CTX *ctx, unsigned nr)
{
    lcbio_CTXRDITER riter;
    HttpProvider *http = reinterpret_cast<HttpProvider*>(lcbio_ctx_data(ctx));
    int old_generation = http->generation;

    lcb_log(LOGARGS(http, TRACE), LOGFMT "Received %d bytes on HTTP stream", LOGID(http), nr);

    lcbio_timer_rearm(http->io_timer,
                      http->settings().config_node_timeout);

    LCBIO_CTX_ITERFOR(ctx, &riter, nr) {
        unsigned nbuf = lcbio_ctx_risize(&riter);
        void *buf = lcbio_ctx_ribuf(&riter);
        lcb_error_t err = process_chunk(http, buf, nbuf);

        if (err != LCB_SUCCESS) {
            io_error(http, err);
            return;
        }
    }

    if (http->generation != old_generation) {
        lcb_log(LOGARGS(http, DEBUG), LOGFMT "Generation %d -> %d", LOGID(http), old_generation, http->generation);
        lcbio_timer_disarm(http->io_timer);
        set_new_config(http);
    }

    lcbio_ctx_rwant(ctx, 1);
    lcbio_ctx_schedule(ctx);
}

static lcb_error_t
setup_request_header(HttpProvider *http, const lcb_host_t *host)
{
    lcb_settings *settings = http->parent->settings;

    char *buf = http->request_buf;
    const char *username = NULL, *password = NULL;
    lcb_size_t nbuf = sizeof(http->request_buf);

    lcb_size_t offset = 0;
    http->request_buf[0] = '\0';

    if (settings->conntype == LCB_TYPE_BUCKET) {
        const char *fmt;
        if (http->uritype == LCB_HTCONFIG_URLTYPE_25PLUS) {
            fmt = REQBUCKET_TERSE_FMT;
        } else {
            fmt = REQBUCKET_COMPAT_FMT;
        }
        offset = snprintf(buf, nbuf, fmt, settings->bucket);

    } else if (settings->conntype == LCB_TYPE_CLUSTER) {
        offset = snprintf(buf, nbuf, REQPOOLS_FMT);

    } else {
        return LCB_EINVAL;
    }
    lcbauth_get_upass(settings->auth, &username, &password);

    if (password) {
        char cred[256], b64[256];
        snprintf(cred, sizeof(cred), "%s:%s", username, password);

        if (lcb_base64_encode(cred, b64, sizeof(b64)) == -1) {
            return LCB_EINTERNAL;
        }

        offset += snprintf(buf + offset, nbuf - offset, AUTHDR_FMT, b64);
    }

    offset += snprintf(buf + offset, nbuf - offset, HOSTHDR_FMT,
                       host->host, host->port);

    offset += snprintf(buf + offset, nbuf - offset, "%s\r\n", LAST_HTTP_HEADER);

    return LCB_SUCCESS;
}

void HttpProvider::reset_stream_state() {
    const int urlmode = settings().bc_http_urltype;
    if (last_parsed) {
        last_parsed->decref();
        last_parsed = NULL;
    }
    if (urlmode & LCB_HTCONFIG_URLTYPE_25PLUS) {
        uritype = LCB_HTCONFIG_URLTYPE_25PLUS;
    } else {
        uritype = LCB_HTCONFIG_URLTYPE_COMPAT;
    }
    try_nexturi = false;
    lcbht_reset(htp);
}

static void
on_connected(lcbio_SOCKET *sock, void *arg, lcb_error_t err, lcbio_OSERR syserr)
{
    HttpProvider *http = reinterpret_cast<HttpProvider*>(arg);
    lcb_host_t *host;
    lcbio_CTXPROCS procs;
    http->creq = NULL;

    if (err != LCB_SUCCESS) {
        lcb_log(LOGARGS(http, ERR), "Connection to REST API failed with code=0x%x (%d)", err, syserr);
        io_error(http, err);
        return;
    }
    host = lcbio_get_host(sock);
    lcb_log(LOGARGS(http, DEBUG), "Successfuly connected to REST API %s:%s", host->host, host->port);

    lcbio_sslify_if_needed(sock, http->parent->settings);
    http->reset_stream_state();

    if ((err = setup_request_header(http, host)) != LCB_SUCCESS) {
        lcb_log(LOGARGS(http, ERR), "Couldn't setup request header");
        io_error(http, err);
        return;
    }

    memset(&procs, 0, sizeof(procs));
    procs.cb_err = io_error_handler;
    procs.cb_read = read_common;
    http->ioctx = lcbio_ctx_new(sock, http, &procs);
    http->ioctx->subsys = "bc_http";

    lcbio_ctx_put(http->ioctx, http->request_buf, strlen(http->request_buf));
    lcbio_ctx_rwant(http->ioctx, 1);
    lcbio_ctx_schedule(http->ioctx);
    lcbio_timer_rearm(http->io_timer,
                      http->settings().config_node_timeout);
}

static void
timeout_handler(void *arg)
{
    HttpProvider *http = reinterpret_cast<HttpProvider*>(arg);

    lcb_log(LOGARGS(http, ERR), LOGFMT "HTTP Provider timed out waiting for I/O", LOGID(http));

    /**
     * If we're not the current provider then ignore the timeout until we're
     * actively requested to do so
     */
    if (http != http->parent->cur_provider || !http->parent->is_refreshing()) {
        lcb_log(LOGARGS(http, DEBUG), LOGFMT "Ignoring timeout because we're either not in a refresh or not the current provider", LOGID(http));
        return;
    }

    io_error(http, LCB_ETIMEDOUT);
}


static lcb_error_t
connect_next(HttpProvider *http)
{
    lcb_settings *settings = http->parent->settings;
    lcb_log(LOGARGS(http, TRACE), "Starting HTTP Configuration Provider %p", (void*)http);
    http->close_current();
    lcbio_timer_disarm(http->as_reconnect);

    if (http->nodes->empty()) {
        lcb_log(LOGARGS(http, ERROR), "Not scheduling HTTP provider since no nodes have been configured for HTTP bootstrap");
        return LCB_CONNECT_ERROR;
    }

    http->creq = lcbio_connect_hl(http->parent->iot, settings, http->nodes, 1,
                                  settings->config_node_timeout, on_connected, http);
    if (http->creq) {
        return LCB_SUCCESS;
    }
    lcb_log(LOGARGS(http, ERROR), "%p: Couldn't schedule connection", (void*)http);
    return LCB_CONNECT_ERROR;
}

static void delayed_disconn(void *arg)
{
    HttpProvider *http = reinterpret_cast<HttpProvider*>(arg);
    lcb_log(LOGARGS(http, DEBUG), "Stopping HTTP provider %p", (void*)http);

    /** closes the connection and cleans up the timer */
    http->close_current();
    lcbio_timer_disarm(http->io_timer);
}

static void delayed_reconnect(void *arg)
{
    HttpProvider *http = reinterpret_cast<HttpProvider*>(arg);
    lcb_error_t err;
    if (http->ioctx) {
        /* have a context already */
        return;
    }
    err = connect_next(http);
    if (err != LCB_SUCCESS) {
        io_error(http, err);
    }
}

bool HttpProvider::pause() {
    if (is_v220_compat()) {
        return LCB_SUCCESS;
    }

    if (!lcbio_timer_armed(disconn_timer)) {
        lcbio_timer_rearm(disconn_timer, parent->settings->bc_http_stream_time);
    }
    return LCB_SUCCESS;
}

lcb_error_t HttpProvider::refresh() {
    /**
     * We want a grace interval here because we might already be fetching a
     * connection. HOWEVER we don't want to indefinitely wait on a socket
     * so we issue a timer indicating how long we expect to wait for a
     * streaming update until we get something.
     */

    /** If we need a new socket, we do connect_next. */
    if (ioctx == NULL && creq == NULL) {
        lcbio_async_signal(as_reconnect);
    }

    lcbio_timer_disarm(disconn_timer);
    if (ioctx) {
        lcbio_timer_rearm(io_timer, parent->settings->config_node_timeout);
    }
    return LCB_SUCCESS;
}

clconfig_info* HttpProvider::get_cached() {
    return current_config;
}

void HttpProvider::config_updated(lcbvb_CONFIG *newconfig)
{
    unsigned sopts;
    lcbvb_SVCMODE mode;
    nodes->clear();

    sopts = settings().sslopts;
    if (sopts & LCB_SSL_ENABLED) {
        mode = LCBVB_SVCMODE_SSL;
    } else {
        mode = LCBVB_SVCMODE_PLAIN;
    }

    for (size_t ii = 0; ii < newconfig->nsrv; ++ii) {
        const char *ss;
        lcb_error_t status;
        ss = lcbvb_get_hostport(newconfig, ii, LCBVB_SVCTYPE_MGMT, mode);
        if (!ss) {
            /* not supported? */
            continue;
        }
        status = nodes->add(ss, LCB_CONFIG_HTTP_PORT);
        lcb_assert(status == LCB_SUCCESS);
    }
    if (nodes->empty()) {
        lcb_log(LOGARGS(this, FATAL), "New nodes do not contain management ports");
    }

    if (settings().randomize_bootstrap_nodes) {
        nodes->randomize();
    }
}

void HttpProvider::configure_nodes(const lcb::Hostlist& newnodes) {
    nodes->assign(newnodes);
    if (settings().randomize_bootstrap_nodes) {
        nodes->randomize();
    }
}

const lcb::Hostlist* HttpProvider::get_nodes() const {
    return nodes;
}

HttpProvider::~HttpProvider() {
    reset_stream_state();
    close_current();
    lcbht_free(htp);

    if (current_config) {
        current_config->decref();
    }
    if (disconn_timer) {
        lcbio_timer_destroy(disconn_timer);
    }
    if (io_timer) {
        lcbio_timer_destroy(io_timer);
    }
    if (as_reconnect) {
        lcbio_timer_destroy(as_reconnect);
    }
    if (nodes) {
        delete nodes;
    }
}

void HttpProvider::dump(FILE *fp) const {
    fprintf(fp, "## BEGIN HTTP PROVIDER DUMP\n");
    fprintf(fp, "NUMBER OF CONFIGS RECEIVED: %u\n", generation);
    fprintf(fp, "DUMPING I/O TIMER\n");
    lcbio_timer_dump(io_timer, fp);
    if (ioctx) {
        fprintf(fp, "DUMPING CURRENT CONNECTION:\n");
        lcbio_ctx_dump(ioctx, fp);
    } else if (creq) {
        fprintf(fp, "CURRENTLY CONNECTING..\n");
    } else {
        fprintf(fp, "NO CONNECTION ACTIVE\n");
    }
}


HttpProvider::HttpProvider(Confmon *parent_)
    : Provider(parent_, CLCONFIG_HTTP),
      ioctx(NULL),
      htp(lcbht_new(parent->settings)),
      disconn_timer(lcbio_timer_new(parent->iot, this, delayed_disconn)),
      io_timer(lcbio_timer_new(parent->iot, this, timeout_handler)),
      as_reconnect(lcbio_timer_new(parent->iot, this, delayed_reconnect)),
      nodes(new Hostlist()),
      current_config(NULL),
      last_parsed(NULL),
      generation(0),
      try_nexturi(false),
      uritype(0) {

    memset(&creq, 0, sizeof creq);
}

static void
io_error_handler(lcbio_CTX *ctx, lcb_error_t err)
{
    io_error(reinterpret_cast<HttpProvider *>(lcbio_ctx_data(ctx)), err);
}


const lcbio_SOCKET* lcb::clconfig::http_get_conn(const Provider *p) {
    const HttpProvider *http = static_cast<const HttpProvider *>(p);
    if (!http->ioctx) {
        return NULL;
    }
    return lcbio_ctx_sock(http->ioctx);

}

const lcb_host_t* lcb::clconfig::http_get_host(const Provider *p)
{
    const lcbio_SOCKET *sock = http_get_conn(p);
    if (sock) {
        return lcbio_get_host(sock);
    }
    return NULL;
}

Provider* lcb::clconfig::new_http_provider(lcb_confmon* mon) {
    return new HttpProvider(mon);
}