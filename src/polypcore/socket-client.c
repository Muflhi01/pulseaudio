/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* #undef HAVE_LIBASYNCNS */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_LIBASYNCNS
#include <asyncns.h>
#endif

#include "winsock.h"

#include <polyp/timeval.h>
#include <polyp/xmalloc.h>

#include <polypcore/core-error.h>
#include <polypcore/socket-util.h>
#include <polypcore/core-util.h>
#include <polypcore/log.h>
#include <polypcore/parseaddr.h>

#include "socket-client.h"

#define CONNECT_TIMEOUT 5

struct pa_socket_client {
    int ref;
    pa_mainloop_api *mainloop;
    int fd;
    pa_io_event *io_event;
    pa_time_event *timeout_event;
    pa_defer_event *defer_event;
    void (*callback)(pa_socket_client*c, pa_iochannel *io, void *userdata);
    void *userdata;
    int local;
#ifdef HAVE_LIBASYNCNS
    asyncns_t *asyncns;
    asyncns_query_t * asyncns_query;
    pa_io_event *asyncns_io_event;
#endif
};

static pa_socket_client*pa_socket_client_new(pa_mainloop_api *m) {
    pa_socket_client *c;
    assert(m);

    c = pa_xmalloc(sizeof(pa_socket_client));
    c->ref = 1;
    c->mainloop = m;
    c->fd = -1;
    c->io_event = NULL;
    c->defer_event = NULL;
    c->timeout_event = NULL;
    c->callback = NULL;
    c->userdata = NULL;
    c->local = 0;

#ifdef HAVE_LIBASYNCNS
    c->asyncns = NULL;
    c->asyncns_io_event = NULL;
    c->asyncns_query = NULL;
#endif

    return c;
}

static void free_events(pa_socket_client *c) {
    assert(c);
    
    if (c->io_event) {
        c->mainloop->io_free(c->io_event);
        c->io_event = NULL;
    }
    
    if (c->defer_event) {
        c->mainloop->defer_free(c->defer_event);
        c->defer_event = NULL;
    }
    
    if (c->timeout_event) {
        c->mainloop->time_free(c->timeout_event);
        c->timeout_event = NULL;
    }
}

static void do_call(pa_socket_client *c) {
    pa_iochannel *io = NULL;
    int error;
    socklen_t lerror;
    assert(c && c->callback);

    pa_socket_client_ref(c);

    if (c->fd < 0)
        goto finish;
    
    lerror = sizeof(error);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void*)&error, &lerror) < 0) {
        pa_log(__FILE__": getsockopt(): %s", pa_cstrerror(errno));
        goto finish;
    }

    if (lerror != sizeof(error)) {
        pa_log(__FILE__": getsockopt() returned invalid size.");
        goto finish;
    }

    if (error != 0) {
        pa_log_debug(__FILE__": connect(): %s", pa_cstrerror(errno));
        errno = error;
        goto finish;
    }

    io = pa_iochannel_new(c->mainloop, c->fd, c->fd);
    assert(io);
    
finish:
    if (!io && c->fd >= 0)
        close(c->fd);
    c->fd = -1;

    free_events(c);
    
    assert(c->callback);
    c->callback(c, io, c->userdata);
    
    pa_socket_client_unref(c);
}

static void connect_fixed_cb(pa_mainloop_api *m, pa_defer_event *e, void *userdata) {
    pa_socket_client *c = userdata;
    assert(m && c && c->defer_event == e);
    do_call(c);
}

static void connect_io_cb(pa_mainloop_api*m, pa_io_event *e, int fd, PA_GCC_UNUSED pa_io_event_flags_t f, void *userdata) {
    pa_socket_client *c = userdata;
    assert(m && c && c->io_event == e && fd >= 0);
    do_call(c);
}

static int do_connect(pa_socket_client *c, const struct sockaddr *sa, socklen_t len) {
    int r;
    assert(c && sa && len);
    
    pa_make_nonblock_fd(c->fd);
    
    if ((r = connect(c->fd, sa, len)) < 0) {
#ifdef OS_IS_WIN32
        if (WSAGetLastError() != EWOULDBLOCK) {
            pa_log_debug(__FILE__": connect(): %d", WSAGetLastError());
#else
        if (errno != EINPROGRESS) {
            pa_log_debug(__FILE__": connect(): %s (%d)", pa_cstrerror(errno), errno);
#endif
            return -1;
        }

        c->io_event = c->mainloop->io_new(c->mainloop, c->fd, PA_IO_EVENT_OUTPUT, connect_io_cb, c);
        assert(c->io_event);
    } else {
        c->defer_event = c->mainloop->defer_new(c->mainloop, connect_fixed_cb, c);
        assert(c->defer_event);
    }

    return 0;
}

pa_socket_client* pa_socket_client_new_ipv4(pa_mainloop_api *m, uint32_t address, uint16_t port) {
    struct sockaddr_in sa;
    assert(m && port > 0);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(address);

    return pa_socket_client_new_sockaddr(m, (struct sockaddr*) &sa, sizeof(sa));
}

#ifdef HAVE_SYS_UN_H

pa_socket_client* pa_socket_client_new_unix(pa_mainloop_api *m, const char *filename) {
    struct sockaddr_un sa;
    assert(m && filename);
    
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, filename, sizeof(sa.sun_path)-1);
    sa.sun_path[sizeof(sa.sun_path) - 1] = 0;

    return pa_socket_client_new_sockaddr(m, (struct sockaddr*) &sa, sizeof(sa));
}

#else /* HAVE_SYS_UN_H */

pa_socket_client* pa_socket_client_new_unix(pa_mainloop_api *m, const char *filename) {
    return NULL;
}

#endif /* HAVE_SYS_UN_H */

static int sockaddr_prepare(pa_socket_client *c, const struct sockaddr *sa, size_t salen) {
    assert(c);
    assert(sa);
    assert(salen);
    
    switch (sa->sa_family) {
        case AF_UNIX:
            c->local = 1;
            break;
            
        case AF_INET:
            c->local = ((const struct sockaddr_in*) sa)->sin_addr.s_addr == INADDR_LOOPBACK;
            break;
            
        case AF_INET6:
            c->local = memcmp(&((const struct sockaddr_in6*) sa)->sin6_addr, &in6addr_loopback, sizeof(struct in6_addr)) == 0;
            break;
            
        default:
            c->local = 0;
    }
    
    if ((c->fd = socket(sa->sa_family, SOCK_STREAM, 0)) < 0) {
        pa_log(__FILE__": socket(): %s", pa_cstrerror(errno));
        return -1;
    }

    pa_fd_set_cloexec(c->fd, 1);
    if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6)
        pa_socket_tcp_low_delay(c->fd);
    else
        pa_socket_low_delay(c->fd);

    if (do_connect(c, sa, salen) < 0)
        return -1;

    return 0;
}

pa_socket_client* pa_socket_client_new_sockaddr(pa_mainloop_api *m, const struct sockaddr *sa, size_t salen) {
    pa_socket_client *c;
    assert(m && sa);
    c = pa_socket_client_new(m);
    assert(c);

    if (sockaddr_prepare(c, sa, salen) < 0)
        goto fail;
    
    return c;

fail:
    pa_socket_client_unref(c);
    return NULL;
    
}

static void socket_client_free(pa_socket_client *c) {
    assert(c && c->mainloop);


    free_events(c);
    
    if (c->fd >= 0)
        close(c->fd);

#ifdef HAVE_LIBASYNCNS
    if (c->asyncns_query)
        asyncns_cancel(c->asyncns, c->asyncns_query);
    if (c->asyncns)
        asyncns_free(c->asyncns);
    if (c->asyncns_io_event)
        c->mainloop->io_free(c->asyncns_io_event);
#endif
    
    pa_xfree(c);
}

void pa_socket_client_unref(pa_socket_client *c) {
    assert(c && c->ref >= 1);

    if (!(--(c->ref)))
        socket_client_free(c);
}

pa_socket_client* pa_socket_client_ref(pa_socket_client *c) {
    assert(c && c->ref >= 1);
    c->ref++;
    return c;
}

void pa_socket_client_set_callback(pa_socket_client *c, void (*on_connection)(pa_socket_client *c, pa_iochannel*io, void *userdata), void *userdata) {
    assert(c);
    c->callback = on_connection;
    c->userdata = userdata;
}

pa_socket_client* pa_socket_client_new_ipv6(pa_mainloop_api *m, uint8_t address[16], uint16_t port) {
    struct sockaddr_in6 sa;
    
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    memcpy(&sa.sin6_addr, address, sizeof(sa.sin6_addr));

    return pa_socket_client_new_sockaddr(m, (struct sockaddr*) &sa, sizeof(sa));
}

#ifdef HAVE_LIBASYNCNS

static void asyncns_cb(pa_mainloop_api*m, pa_io_event *e, int fd, PA_GCC_UNUSED pa_io_event_flags_t f, void *userdata) {
    pa_socket_client *c = userdata;
    struct addrinfo *res = NULL;
    int ret;
    assert(m && c && c->asyncns_io_event == e && fd >= 0);

    if (asyncns_wait(c->asyncns, 0) < 0)
        goto fail;

    if (!asyncns_isdone(c->asyncns, c->asyncns_query))
        return;

    ret = asyncns_getaddrinfo_done(c->asyncns, c->asyncns_query, &res);
    c->asyncns_query = NULL;

    if (ret != 0 || !res)
        goto fail;
    
    if (res->ai_addr)
        sockaddr_prepare(c, res->ai_addr, res->ai_addrlen);
    
    asyncns_freeaddrinfo(res);

    m->io_free(c->asyncns_io_event);
    c->asyncns_io_event = NULL;
    return;
    
fail:
    m->io_free(c->asyncns_io_event);
    c->asyncns_io_event = NULL;
    
    errno = EHOSTUNREACH;
    do_call(c);
    return;
    
}

#endif

static void timeout_cb(pa_mainloop_api *m, pa_time_event *e, const struct timeval *tv, void *userdata) {
    pa_socket_client *c = userdata;
    assert(m);
    assert(e);
    assert(tv);
    assert(c);

    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }

    errno = ETIMEDOUT;
    do_call(c);
}

static void start_timeout(pa_socket_client *c) {
    struct timeval tv;
    assert(c);
    assert(!c->timeout_event);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, CONNECT_TIMEOUT * 1000000);
    c->timeout_event = c->mainloop->time_new(c->mainloop, &tv, timeout_cb, c);
}

pa_socket_client* pa_socket_client_new_string(pa_mainloop_api *m, const char*name, uint16_t default_port) {
    pa_socket_client *c = NULL;
    pa_parsed_address a;
    assert(m && name);

    if (pa_parse_address(name, &a) < 0)
        return NULL;

    if (!a.port)
        a.port = default_port;
    
    switch (a.type) {
        case PA_PARSED_ADDRESS_UNIX:
            if ((c = pa_socket_client_new_unix(m, a.path_or_host)))
            	start_timeout(c);
            break;

        case PA_PARSED_ADDRESS_TCP4:  /* Fallthrough */
        case PA_PARSED_ADDRESS_TCP6:  /* Fallthrough */
        case PA_PARSED_ADDRESS_TCP_AUTO:{

            struct addrinfo hints;
            char port[12];

            snprintf(port, sizeof(port), "%u", (unsigned) a.port);

            memset(&hints, 0, sizeof(hints));
            hints.ai_family = a.type == PA_PARSED_ADDRESS_TCP4 ? PF_INET : (a.type == PA_PARSED_ADDRESS_TCP6 ? PF_INET6 : PF_UNSPEC);
            hints.ai_socktype = SOCK_STREAM;
            
#ifdef HAVE_LIBASYNCNS
            {
                asyncns_t *asyncns;
                
                if (!(asyncns = asyncns_new(1)))
                    goto finish;

                c = pa_socket_client_new(m);
                c->asyncns = asyncns;
                c->asyncns_io_event = m->io_new(m, asyncns_fd(c->asyncns), PA_IO_EVENT_INPUT, asyncns_cb, c);
                c->asyncns_query = asyncns_getaddrinfo(c->asyncns, a.path_or_host, port, &hints);
                assert(c->asyncns_query);
                start_timeout(c);
            }
#else /* HAVE_LIBASYNCNS */
            {
#ifdef HAVE_GETADDRINFO
                int ret;
                struct addrinfo *res = NULL;

                ret = getaddrinfo(a.path_or_host, port, &hints, &res);
                
                if (ret < 0 || !res)
                    goto finish;

                if (res->ai_addr) {
                    if ((c = pa_socket_client_new_sockaddr(m, res->ai_addr, res->ai_addrlen)))
                        start_timeout(c);
				}
                
                freeaddrinfo(res);
#else /* HAVE_GETADDRINFO */
                struct hostent *host = NULL;
                struct sockaddr_in s;

                /* FIXME: PF_INET6 support */
                if (hints.ai_family == PF_INET6) {
                    pa_log_error(__FILE__": IPv6 is not supported on Windows");
                    goto finish;
                }

                host = gethostbyname(a.path_or_host);
                if (!host) {
                    unsigned int addr = inet_addr(a.path_or_host);
                    if (addr != INADDR_NONE)
                        host = gethostbyaddr((char*)&addr, 4, AF_INET);
                }

                if (!host)
                    goto finish;

                s.sin_family = AF_INET;
                memcpy(&s.sin_addr, host->h_addr, sizeof(struct in_addr));
                s.sin_port = htons(a.port);

                if ((c = pa_socket_client_new_sockaddr(m, (struct sockaddr*)&s, sizeof(s))))
                	start_timeout(c);
#endif /* HAVE_GETADDRINFO */
            }
#endif /* HAVE_LIBASYNCNS */
        }
    }

finish:
    pa_xfree(a.path_or_host);
    return c;
    
}

/* Return non-zero when the target sockaddr is considered
   local. "local" means UNIX socket or TCP socket on localhost. Other
   local IP addresses are not considered local. */
int pa_socket_client_is_local(pa_socket_client *c) {
    assert(c);
    return c->local;
}
