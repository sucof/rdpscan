/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Protocol services - TCP layer
   Copyright (C) Matthew Chapman <matthewc.unsw.edu.au> 1999-2008
   Copyright 2005-2011 Peter Astrand <astrand@cendio.se> for Cendio AB
   Copyright 2012-2013 Henrik Andersson <hean01@cendio.se> for Cendio AB

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#define _CRT_SECURE_NO_WARNINGS 1
#include "util-sockets.h"
#include "rdesktop.h"
#include "util-xmalloc.h"
#include "util-log.h"
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>


/*
 * GLOBALS
 */
char g_targetaddr[256];
char g_targetport[8];
int g_scan_timeout = 10;
#ifdef WITH_SCARD
#define STREAM_COUNT 8
#else
#define STREAM_COUNT 1
#endif
static RD_BOOL g_ssl_initialized = False;
static SSL *g_ssl = NULL;
static SSL_CTX *g_ssl_ctx = NULL;
static int g_sock;
static RD_BOOL g_run_ui = False;
static struct stream g_in;
static struct stream g_out[STREAM_COUNT];
int g_tcp_port_rdp = 3389;
extern RD_BOOL g_user_quit;
extern RD_BOOL g_network_error;
extern RD_BOOL g_reconnect_loop;


/* wait till socket is ready to write or timeout */
static RD_BOOL
tcp_can_send(int sck, int millis)
{
	fd_set wfds;
	struct timeval time;
	int sel_count;

	time.tv_sec = millis / 1000;
	time.tv_usec = (millis * 1000) % 1000000;
	FD_ZERO(&wfds);
	FD_SET(sck, &wfds);
	sel_count = select(sck + 1, 0, &wfds, 0, &time);
	if (sel_count > 0)
	{
		return True;
	}
	return False;
}

static RD_BOOL
tcp_can_receive(int sck, int millis)
{
    fd_set wfds;
    struct timeval time;
    int sel_count;
    
    time.tv_sec = millis / 1000;
    time.tv_usec = (millis * 1000) % 1000000;
    FD_ZERO(&wfds);
    FD_SET(sck, &wfds);
    sel_count = select(sck + 1, &wfds, 0, 0, &time);
    if (sel_count > 0)
    {
        return True;
    }
    return False;
}

/* Initialise TCP transport data packet */
STREAM
tcp_init(uint32 maxlen)
{
	static int cur_stream_id = 0;
	STREAM result = NULL;

#ifdef WITH_SCARD
	scard_lock(SCARD_LOCK_TCP);
#endif
	result = &g_out[cur_stream_id];
	cur_stream_id = (cur_stream_id + 1) % STREAM_COUNT;

	if (maxlen > result->size)
	{
		result->data = (uint8 *) xrealloc(result->data, maxlen);
		result->size = maxlen;
	}

	result->p = result->data;
	result->end = result->data + result->size;
#ifdef WITH_SCARD
	scard_unlock(SCARD_LOCK_TCP);
#endif
	return result;
}

/* Send TCP transport data packet */
void
tcp_send(STREAM s)
{
	int ssl_err;
	int length = (int)(s->end - s->data);
    long sent;
    int total = 0;

	if (g_network_error == True)
		return;

#ifdef WITH_SCARD
	scard_lock(SCARD_LOCK_TCP);
#endif
	while (total < length)
	{
		if (g_ssl)
		{
			sent = SSL_write(g_ssl, s->data + total, length - total);
			if (sent <= 0)
			{
				ssl_err = SSL_get_error(g_ssl, (int)sent);
				if (sent < 0 && (ssl_err == SSL_ERROR_WANT_READ ||
						 ssl_err == SSL_ERROR_WANT_WRITE))
				{
					tcp_can_send(g_sock, 100);
					sent = 0;
				}
				else
				{
#ifdef WITH_SCARD
					scard_unlock(SCARD_LOCK_TCP);
#endif

					error("SSL_write: %d (%s)\n", ssl_err, $strerror($errno));
					g_network_error = True;
					return;
				}
			}
		}
		else
		{
			sent = send(g_sock, s->data + total, length - total, 0);
			if (sent <= 0)
			{
				if (sent == -1 && $errno == $EWOULDBLOCK)
				{
					tcp_can_send(g_sock, 100);
					sent = 0;
				}
				else
				{
#ifdef WITH_SCARD
					scard_unlock(SCARD_LOCK_TCP);
#endif

					error("send: %s\n", $strerror($errno));
					g_network_error = True;
					return;
				}
			}
		}
		total += sent;
	}
#ifdef WITH_SCARD
	scard_unlock(SCARD_LOCK_TCP);
#endif
}

/* Receive a message on the TCP layer */
STREAM
tcp_recv(STREAM s, uint32 length)
{
	uint32 new_length, end_offset, p_offset;
    long rcvd = 0;
    int ssl_err;

	if (g_network_error == True)
		return NULL;

	if (s == NULL)
	{
		/* read into "new" stream */
		if (length > g_in.size)
		{
			g_in.data = (uint8 *) xrealloc(g_in.data, length);
			g_in.size = length;
		}
		g_in.end = g_in.p = g_in.data;
		s = &g_in;
	}
	else
	{
		/* append to existing stream */
		new_length = (int)(s->end - s->data) + length;
		if (new_length > s->size)
		{
			p_offset = (int)(s->p - s->data);
			end_offset = (int)(s->end - s->data);
			s->data = (uint8 *) xrealloc(s->data, new_length);
			s->size = new_length;
			s->p = s->data + p_offset;
			s->end = s->data + end_offset;
		}
	}

	while (length > 0)
	{
        static size_t total_data_received = 0;
        static time_t timeof_last_receive = 0;
        
        if (timeof_last_receive == 0)
            timeof_last_receive = time(0);
        
        /* Use select to wait for incoming data */
        if (!tcp_can_receive(g_sock, 1000))
        {
            extern const unsigned MST120_TIMEOUT;
            extern time_t g_first_check;
            extern void mst120_check(void);
            
            
            if (g_first_check && g_first_check + MST120_TIMEOUT < time(0)) {
                /* CVE-2019-0708
                 * We'll check this when recieving 'orders' from the other side,
                 * but in case thye stop arriving, we need to continue to check
                 * this. However, we only do it once the T120 timeout has passed,
                 * so that should only happen once we've reached the
                 * result=patched state */
                mst120_check();
            } else if (time(0) > timeof_last_receive + MST120_TIMEOUT*2) {
                /* CVE-2019-0708
                 * We are finding machines on the Internet that respond to a
                 * connection (SYN-ACK), but report a zero window size, so
                 * we cannot transmit anything to those targets. Therefore,
                 * we are going to check for that condition here, that after
                 * a period of time, we data still remains to be sent without
                 * anything being received, we'll report that as as "result"
                 */
                int unsent_count = 0;
                socklen_t sizeof_count = sizeof(unsent_count);


                /* See if there is unsent data */
#ifdef SO_NWRITE        
                {
                    int err;
                    err = getsockopt(g_sock, SOL_SOCKET, SO_NWRITE, &unsent_count, &sizeof_count);
                    if (err) {
                        STATUS(1, "[-] getsockopt(SO_NWRITE) error %s\n", $strerror($errno));
                    } else {
                        STATUS(1, "[-] SO_NWRITE = %d\n", unsent_count);
                    }
                }
#endif
                
                /* Look for full TCP window on the other side */
                if (total_data_received == 0 && unsent_count > 0) {
                    RESULT("UNKNOWN - zero TCP window on connect\n");
                } else if (unsent_count) {
                    RESULT("UNKNOWN - zero TCP window\n");
                } else {
                    RESULT("UNKNOWN - receive timeout\n");
                }
            }
        }
        
		if ((!g_ssl || SSL_pending(g_ssl) <= 0) && g_run_ui)
		{
			if (0) //!ui_select(g_sock))
			{
				/* User quit */
				g_user_quit = True;
				return NULL;
			}
		}

		if (g_ssl)
		{
			rcvd = SSL_read(g_ssl, s->end, length);
			ssl_err = SSL_get_error(g_ssl, (int)rcvd);

			if (ssl_err == SSL_ERROR_SSL)
			{
				if (SSL_get_shutdown(g_ssl) & SSL_RECEIVED_SHUTDOWN)
				{
					STATUS(0, "Remote peer initiated ssl shutdown.\n");
                    RESULT("UNKNOWN - network error\n");
                    return NULL;
				}

				ERR_print_errors_fp(stdout);
				g_network_error = True;
				return NULL;
			}

			if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
			{
				rcvd = 0;
			}
			else if (ssl_err != SSL_ERROR_NONE)
			{
				STATUS(0, "SSL_read: %d (%s)\n", ssl_err, $strerror($errno));
                RESULT("UNKNOWN - network error\n");
				g_network_error = True;
				return NULL;
			}
		}
		else
		{
            tcp_can_receive(g_sock, 1000);
            
			rcvd = recv(g_sock, s->end, length, 0);
			if (rcvd < 0)
			{
                switch ($errno) {
                    case $EWOULDBLOCK:
                        rcvd = 0;
                        break;
                    case $ECONNRESET:
                        if (total_data_received)
                            RESULT("UNKNOWN - connection reset on connect\n");
                        else
                            RESULT("UNKNOWN - connection reset by peer\n");
                        break;
                    case $ECONNABORTED:
                        /* This happens on Windows when the TCP window is full
                         * on connect. */
                        STATUS(1, "connection aborted\n");
                        if (total_data_received == 0) {
                            RESULT("UNKNOWN - zero TCP window on connect\n");
                        } else {
                            RESULT("UNKNOWN - connection aborted\n");
                        }
                        break;
                    default:
                        STATUS(1, "[-] recv: %s\n", $strerror($errno));
                        RESULT("UNKNOWN - error: %s\n", $strerror($errno));
                        g_network_error = True;
                        return NULL;
                }
			}
			else if (rcvd == 0)
			{
				STATUS(1, "[-] connection closed (TCP FIN)\n");
                RESULT("UNKNOWN - FIN received\n");
				return NULL;
			}
		}

		s->end += rcvd;
		length -= rcvd;
        
        /* Mark the total amount we've seen on this connection, mostly
         * used to detect when things have broken */
        if (rcvd) {
            total_data_received += rcvd;
            timeof_last_receive = time(0);
        }
	}

	return s;
}

/* Establish a SSL/TLS 1.0 connection */
RD_BOOL
tcp_tls_connect(void)
{
	int err;
	long options;

	if (!g_ssl_initialized)
	{
		SSL_load_error_strings();
		SSL_library_init();
		g_ssl_initialized = True;
	}

	/* create process context */
	if (g_ssl_ctx == NULL)
	{
		g_ssl_ctx = SSL_CTX_new(TLSv1_client_method());
		if (g_ssl_ctx == NULL)
		{
			error("tcp_tls_connect: SSL_CTX_new() failed to create TLS v1.0 context\n");
			goto fail;
		}

		options = 0;
#ifdef SSL_OP_NO_COMPRESSION
		options |= SSL_OP_NO_COMPRESSION;
#endif // __SSL_OP_NO_COMPRESSION
		options |= SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;
		SSL_CTX_set_options(g_ssl_ctx, options);
	}

	/* free old connection */
	if (g_ssl)
		SSL_free(g_ssl);

	/* create new ssl connection */
	g_ssl = SSL_new(g_ssl_ctx);
	if (g_ssl == NULL)
	{
		error("tcp_tls_connect: SSL_new() failed\n");
		goto fail;
	}

	if (SSL_set_fd(g_ssl, g_sock) < 1)
	{
		error("tcp_tls_connect: SSL_set_fd() failed\n");
		goto fail;
	}

	do
	{
		err = SSL_connect(g_ssl);
	}
	while (SSL_get_error(g_ssl, err) == SSL_ERROR_WANT_READ);

	if (err < 0)
	{
		ERR_print_errors_fp(stdout);
		goto fail;
	}

	return True;

      fail:
	if (g_ssl)
		SSL_free(g_ssl);
	if (g_ssl_ctx)
		SSL_CTX_free(g_ssl_ctx);

	g_ssl = NULL;
	g_ssl_ctx = NULL;
	return False;
}

/* Get public key from server of TLS 1.0 connection */
RD_BOOL
tcp_tls_get_server_pubkey(STREAM s)
{
	X509 *cert = NULL;
	EVP_PKEY *pkey = NULL;

	s->data = s->p = NULL;
	s->size = 0;

	if (g_ssl == NULL)
		goto out;

	cert = SSL_get_peer_certificate(g_ssl);
	if (cert == NULL)
	{
		error("tcp_tls_get_server_pubkey: SSL_get_peer_certificate() failed\n");
		goto out;
	}

	pkey = X509_get_pubkey(cert);
	if (pkey == NULL)
	{
		error("tcp_tls_get_server_pubkey: X509_get_pubkey() failed\n");
		goto out;
	}

	s->size = i2d_PublicKey(pkey, NULL);
	if (s->size < 1)
	{
		error("tcp_tls_get_server_pubkey: i2d_PublicKey() failed\n");
		goto out;
	}

	s->data = s->p = xmalloc(s->size);
	i2d_PublicKey(pkey, &s->p);
	s->p = s->data;
	s->end = s->p + s->size;

      out:
	if (cert)
		X509_free(cert);
	if (pkey)
		EVP_PKEY_free(pkey);
	return (s->size != 0);
}

/**
 * Establish a raw connection to the desired target.
 */
static int
sockets_connect(const char *target, unsigned port)
{
    int err;
    struct addrinfo hints;
    struct addrinfo *ai;
    struct addrinfo *result;
    char portsz[10];
    int fd = -1;
    struct sockaddr_storage sa;
    socklen_t sizeof_sa = sizeof(sa);
   

    /* Create a string out of the integer in order to pass it
     * to getaddrinfo(), which requires a string */
    snprintf(portsz, sizeof(portsz), "%d", port);

    /* Temporarily save these to strings for use in error messages, they'll
     * be replaced later with the number address once we connect. At this
     * point, they could still be DNS names */
    snprintf(g_targetaddr, 256, "%s", target);
    snprintf(g_targetport, 8, "%s", portsz);

    /* Create a hints structure that will allow us to get an
     * address suitable for connectin with either IPv4 or IPv6 */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    /* Do the DNS lookup (if a name) or parse the IPv4/IPv6 address
     * if not a name */
    err = getaddrinfo(target, portsz, &hints, &result);
    if (err)
    {
        STATUS(0, "[-] getaddrinfo() failed: %s\n", gai_strerror(err));
        RESULT("UNKNOWN - name resolution failed\n");
        return -1;
    }
    
    
    /* Multiple addresses, including both IPv6 and IPv4 addresses, can
     * be returned for a single name. Therefore, we are going to go
     * through the list and choose the first IP address that successfully
     * connects. */
    for (ai = result ; ai != NULL; ai = ai->ai_next)
    {
        /* Create a potential socket to use */
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            STATUS(1, "[-] socket() failed: %s\n", $strerror($errno));
            continue;
        }
        
        /* Print the address/port to strings for logging/debugging. Most logging
         * messages from now on will include this address. These are global
         * variables, so will be used by STATUS() and RESULT() */
        err = getnameinfo(ai->ai_addr, (int)ai->ai_addrlen,
                          g_targetaddr, (int)sizeof(g_targetaddr),
                          g_targetport, (int)sizeof(g_targetport),
                          NI_NUMERICHOST | NI_NUMERICSERV);

        /* Configure this socket as non-blocking, so we can control how long
         * timeouts take. This wasn't done in the original rdesktop code, but
         * yet the code seemed to have been written to anticipate non-blocking
         * sockets anyway, so it all works pretty cleanly */
#ifdef WIN32
        {
            ULONG yes = 1;
            ioctlsocket(fd, FIONBIO, &yes);
        }
#else
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif

        /* Attempt the connection */
        STATUS(1, "[+] connecting...\n");
        err = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        if (err == 0) {
            /* We've got a gootdTCP connection, so break out of this loop
             * and use this connection below */
            break;
        }
        
        /* If non-blocking, then wait for results */
        if ($errno == $EWOULDBLOCK || $errno == $EINTR || $errno == $EINPROGRESS)
        {
            time_t timeof_start = time(0);
            while (!tcp_can_send(fd, 100)) {
                if (timeof_start + g_scan_timeout < time(0)) {
                    STATUS(1, "[-] connect: timeout\n");
                    RESULT("UNKNOWN - connect timeout\n");
                    $close(fd);
                    fd = -1;
                    break;
                }
            }
            if (fd != -1) {
                int errcode;
                socklen_t sizeof_errcode = sizeof(errcode);
                err = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&errcode, &sizeof_errcode);
                if (err != 0) {
                    STATUS(1, "[-] getsockopt() error: %s\n", $strerror($errno));
                    $close(fd);
                    fd = -1;
                } else if (errcode != 0) {
                    $close(fd);
                    fd = -1;
                    switch ($errno) {
                        case $EINTR:
                        case $ETIMEDOUT:
                            STATUS(1, "[-] time out\n");
                            break;
                        case $ECONNREFUSED:
                            STATUS(1, "[-] refused\n");
                            break;
                        default:
                            STATUS(1, "[-] failed: %s (%d)\n", $strerror($errno), $errno);
                    }
                }
            }
            if (fd != -1)
                break;
        }
        
        /* We did not get a connection, but if this is a DNS name we were
         * given, then there may be more IP addresses in the list, so loop
         * around and try again */
        switch ($errno) {
            case $EINTR:
            case $ETIMEDOUT:
                STATUS(1, "[-] time out\n");
                break;
            case $ECONNREFUSED:
                STATUS(1, "[-] refused\n");
                break;
            default:
                STATUS(1, "[-] failed: %s (%d)\n", $strerror($errno), $errno);
        }
        $close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    
    /* CVE-2019-0708
     * When scanning for this vuln, the final verdicat is "unknown"
     * if we cannot connect */
    if (fd == -1)
    {
        switch ($errno) {
            case $EINTR:
            case $ETIMEDOUT:
                RESULT("UNKNOWN - connect timeout\n");
                break;
            case $ECONNREFUSED:
                RESULT("UNKNOWN - connect refused\n");
                break;
            default:
                RESULT("UNKNOWN - connect failed %d\n", $errno);
        }
        return -1;
    }
    
    
    /* Logging: While we normally use the remote address for logging,
     * at this step, we also grab the local address/port that's used
     * for this connection, so that we can better compare these
     * messages to what we see in log files */
    err = getsockname(fd, (struct sockaddr *)&sa, &sizeof_sa);
    if (err == 0) {
        char myaddr[64];
        char myport[8];
        
        err = getnameinfo((struct sockaddr *)&sa, (int)sizeof_sa,
                          myaddr, (int)sizeof(myaddr),
                          myport, (int)sizeof(myport),
                          NI_NUMERICHOST | NI_NUMERICSERV);
        if (err == 0) {
            STATUS(1, "[+] connected from [%s]:%s\n", myaddr, myport);
        }
    }
    
    /* Set some further socket options */
    {
        uint32 option_value = 1;
        socklen_t option_len = sizeof(option_value);

        /* Disable Nagle's algorithm so that we get faster responses
         * from the server */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *) &option_value, option_len);
        
        /* receive buffer must be a least 16 K */
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *) &option_value, &option_len) == 0)
        {
            if (option_value < (1024 * 16))
            {
                option_value = 1024 * 16;
                option_len = sizeof(option_value);
                setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *) &option_value,
                           option_len);
            }
        }
    }

    /* Success! */
    return fd;
}


static int
socks_send(int fd, const unsigned char *buf, size_t length)
{
    extern int g_scan_timeout;
    time_t timeof_start = time(0);
    size_t offset = 0;
    
    while (offset < length)
    {
        ssize_t bytes_sent;

        if (timeof_start + g_scan_timeout < time(0)) {
            STATUS(1, "[-] SOCKS5: timeout\n");
            $close(fd);
            return -1;
        }
        if (!tcp_can_send(fd, 100))
            continue;
        
        bytes_sent = send(fd, buf + offset, (int)(length - offset), 0);
        if (bytes_sent < 0) {
            STATUS(1, "[-] SOCKS: send(): %s\n", $strerror($errno));
            $close(fd);
            return -1;
        } else
            offset += bytes_sent;
    }

    return fd;
}

static int
socks_receive(int fd, const unsigned char *buf, size_t length)
{
    extern int g_scan_timeout;
    time_t timeof_start = time(0);
    size_t offset = 0;
    
    while (offset < length)
    {
        ssize_t bytes_received;
        
        if (timeof_start + g_scan_timeout < time(0)) {
            STATUS(1, "[-] SOCKS5: timeout\n");
            $close(fd);
            return -1;
        }
        
        if (!tcp_can_receive(fd, 100))
            continue;
        
        bytes_received = recv(fd, (char*)buf + offset, (int)(length - offset), 0);
        if (bytes_received < 0) {
            STATUS(1, "[-] SOCKS: recv(): %s\n", $strerror($errno));
            $close(fd);
            return -1;
        } else
            offset += bytes_received;
    }
    
    return fd;
}

/**
 * Establish a connection to the sockets server, and thence to
 * the actual target. When we exit this function, we'll have a
 * TCP connection ready for further use as if it were directly
 * connected to the server.
 */
int
socks5_connect(const char *socks_host, unsigned socks_port,
               const char *target_host, unsigned target_port)
{
    int fd;
    unsigned char buf[1024];
    ssize_t length;
    size_t offset;
    enum {Ver4=1, VerDNS=3, Ver6=4} ip_version = VerDNS;
    struct in_addr ipv4 = {0};
    struct in6_addr ipv6 = {0};
    size_t i;
    char bindaddr[64];
    char bindport[8];
    unsigned port;
    
    /* First, we need to figure out whether we have an IPv4 address,
     * an IPv6 address, or a DNS name */
    if (inet_pton(AF_INET, target_host, &ipv4)) {
        ip_version = Ver4;
    } else if (inet_pton(AF_INET6, target_host, &ipv6)) {
        ip_version = Ver6;
    } else
        ip_version = VerDNS;
    
    /* Create a TCP connection to the SOCKS5 server */
    fd = sockets_connect(socks_host, socks_port);
    if (fd == -1)
        return -1;
    
    /* Send the SOCKS5 connection message
     +----+----------+----------+
     |VER | NMETHODS | METHODS  |
     +----+----------+----------+
     | 1  |    1     | 1 to 255 |
     +----+----------+----------+
     */
    buf[0] = 0x05;  /* version = 5 */
    buf[1] = 1;     /* two auth methods are supported */
    buf[2] = 0x00;  /* method = no authentication */
    //buf[3] = 0x02;  /* method = username/password */
    fd = socks_send(fd, buf, 3);
    if (fd == -1)
        return -1;
    
    /* Now receive the response back from the server acknowleging
     * use
     +----+--------+
     |VER | METHOD |
     +----+--------+
     | 1  |   1    |
     +----+--------+
     */
    fd = socks_receive(fd, buf, 2);
    if (fd == -1)
        return -1;
    if (buf[0] != 5 || buf[1] != 0) {
        STATUS(1, "[-] SOCKS5: bad connect response\n");
        $close(fd);
        return -1;
    }
    
    /* Now construct the buffer for establishing the SOCKS5
     * connection
    +----+-----+-------+------+----------+----------+
    |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
    +----+-----+-------+------+----------+----------+
    | 1  |  1  | X'00' |  1   | Variable |    2     |
    +----+-----+-------+------+----------+----------+
     */
    length = sizeof(buf);
    buf[0] = 0x05; /* version = 5 */
    buf[1] = 0x01; /* cmd = connect */
    buf[2] = 0x00; /* reserved = empty */
    buf[3] = ip_version; /* address type = 1=ipv4, 3=ipv6, 4=dns */
    offset = 4;
    switch (ip_version) {
        case Ver4:
            for (i=0; i<4; i++)
                buf[offset++] = ((unsigned char *)(&ipv4.s_addr))[i];
            break;
        case Ver6:
            for (i=0; i<16; i++)
                buf[offset++] = ipv6.s6_addr[i];
            break;
        case VerDNS:
            for (i=0; target_host[i]; i++)
                buf[offset++] = target_host[i];
            break;
    }
    buf[offset++] = (unsigned char)(target_port >> 8);
    buf[offset++] = (unsigned char)(target_port >> 0);
    fd = socks_send(fd, buf, offset);
    if (fd == -1)
        return -1;
    
    /* Change the debug strings for TCP to instead be the eventual target
     * and not the Socks server */
    snprintf(g_targetaddr, 256, "%s", target_host);
    snprintf(g_targetport, 8, "%u", target_port);

    /* Now receive the connection response
     +----+-----+-------+------+----------+----------+
     |VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
     +----+-----+-------+------+----------+----------+
     | 1  |  1  | X'00' |  1   | Variable |    2     |
     +----+-----+-------+------+----------+----------+
     */
    fd = socks_receive(fd, buf, 4);
    if (fd == -1) {
        RESULT("UNKNOWN - connect timeout\n");
        return -1;
    }
    switch (buf[3]) {
        case Ver4:
            fd = socks_receive(fd, buf+4, 4 + 2);
            ipv4.s_addr = *(unsigned*)(buf+4);
            port = buf[8]<<8 | buf[9];
            inet_ntop(AF_INET, &ipv4, bindaddr, sizeof(bindaddr));
            snprintf(bindport, sizeof(bindport), "%u", port);
            break;
        case Ver6:
            fd = socks_receive(fd, buf+4, 16 + 2);
            memcpy(ipv6.s6_addr, buf+4, 16);
            port = buf[20]<<8 | buf[21];
            inet_ntop(AF_INET6, &ipv6, bindaddr, sizeof(bindaddr));
            snprintf(bindport, sizeof(bindport), "%u", port);
            break;
        default:
            STATUS(1, "[-] SOCKS5: unknown response\n");
            $close(fd);
            fd = -1;
            break;
    }
    if (fd == -1)
        return -1;
    if (buf[0] != 5) {
        STATUS(1, "[-] SOCKS5: unknown response\n");
        $close(fd);
        fd = -1;
    } else if (buf[1] != 0) {
        STATUS(1, "[-] SOCKS5: error #%u\n", buf[1]);
        $close(fd);
        fd = -1;
    }
    
    STATUS(1, "[+] SOCKS5: connecting through [%s]:%s\n", bindaddr, bindport);
    
    
    /* At this point, everything seems to be okay, and we can return this
     * socket as a normally connected socket for continued use
     * as if it weren't through socks */
    return fd;
}


/**
 * Establish a connection on the TCP layer. This connection can
 * morph widely, suchs as first being a SOCKS5 connection and
 * alter being an SSL connection.
 */
RD_BOOL
tcp_connect(char *server)
{
	int i;
    extern char *g_socks5_server;
    extern unsigned g_socks5_port;

    /* On Windos, we have to do this on startup, or the sockets
     * functions won't work */
#ifdef WIN32
    {WSADATA x; WSAStartup(0x201,&x);}
#endif

    /*
     * Establish the connection, which is usually directly to the target
     * we are scannning, but it may instead be to a SOCKS5 server, and
     * thence to the target. Either way, it's the same TCP connection
     * that we'll use after this point.
     */
    if (g_socks5_server)
        g_sock = socks5_connect(g_socks5_server, g_socks5_port, server, g_tcp_port_rdp);
    else
        g_sock = sockets_connect(server, g_tcp_port_rdp);
    if (g_sock == -1)
        return False;
   
    
	g_in.size = 4096;
	g_in.data = (uint8 *) xmalloc(g_in.size);
    
	for (i = 0; i < STREAM_COUNT; i++)
	{
		g_out[i].size = 4096;
		g_out[i].data = (uint8 *) xmalloc(g_out[i].size);
	}
    
    return True;
}

/* Disconnect on the TCP layer */
void
tcp_disconnect(void)
{
	if (g_ssl)
	{
		if (!g_network_error)
			(void) SSL_shutdown(g_ssl);
		SSL_free(g_ssl);
		g_ssl = NULL;
		SSL_CTX_free(g_ssl_ctx);
		g_ssl_ctx = NULL;
	}

	$close(g_sock);
	g_sock = -1;
}

char *
tcp_get_address()
{
	static char ipaddr[32];
	struct sockaddr_in sockaddr;
	socklen_t len = sizeof(sockaddr);
	if (getsockname(g_sock, (struct sockaddr *) &sockaddr, &len) == 0)
	{
		uint8 *ip = (uint8 *) & sockaddr.sin_addr;
		sprintf(ipaddr, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
	}
	else
		strcpy(ipaddr, "127.0.0.1");
	return ipaddr;
}

RD_BOOL
tcp_is_connected()
{
	struct sockaddr_in sockaddr;
	socklen_t len = sizeof(sockaddr);
	if (getpeername(g_sock, (struct sockaddr *) &sockaddr, &len))
		return True;
	return False;
}

/* reset the state of the tcp layer */
/* Support for Session Directory */
void
tcp_reset_state(void)
{
	int i;

	/* Clear the incoming stream */
	if (g_in.data != NULL)
		xfree(g_in.data);
	g_in.p = NULL;
	g_in.end = NULL;
	g_in.data = NULL;
	g_in.size = 0;
	g_in.iso_hdr = NULL;
	g_in.mcs_hdr = NULL;
	g_in.sec_hdr = NULL;
	g_in.rdp_hdr = NULL;
	g_in.channel_hdr = NULL;

	/* Clear the outgoing stream(s) */
	for (i = 0; i < STREAM_COUNT; i++)
	{
		if (g_out[i].data != NULL)
			xfree(g_out[i].data);
		g_out[i].p = NULL;
		g_out[i].end = NULL;
		g_out[i].data = NULL;
		g_out[i].size = 0;
		g_out[i].iso_hdr = NULL;
		g_out[i].mcs_hdr = NULL;
		g_out[i].sec_hdr = NULL;
		g_out[i].rdp_hdr = NULL;
		g_out[i].channel_hdr = NULL;
	}
}

void
tcp_run_ui(RD_BOOL run)
{
	g_run_ui = run;
}
