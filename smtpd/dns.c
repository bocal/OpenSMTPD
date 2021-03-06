/*	$OpenBSD: dns.c,v 1.78 2014/04/19 12:26:15 gilles Exp $	*/

/*
 * Copyright (c) 2008 Gilles Chehade <gilles@poolp.org>
 * Copyright (c) 2009 Jacek Masiulaniec <jacekm@dobremiasto.net>
 * Copyright (c) 2011-2014 Eric Faurot <eric@faurot.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/tree.h>
#include <sys/queue.h>
#include <sys/uio.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>

#include <asr.h>
#include <event.h>
#include <imsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "smtpd.h"
#include "log.h"

struct dns_lookup {
	struct dns_session	*session;
	int			 preference;
};

struct dns_session {
	struct mproc		*p;
	uint64_t		 reqid;
	int			 type;
	char			 name[HOST_NAME_MAX+1];
	size_t			 mxfound;
	int			 error;
	int			 refcount;
};

static void dns_lookup_host(struct dns_session *, const char *, int);
static void dns_dispatch_host(struct asr_result *, void *);
static void dns_dispatch_ptr(struct asr_result *, void *);
static void dns_dispatch_mx(struct asr_result *, void *);
static void dns_dispatch_mx_preference(struct asr_result *, void *);

struct unpack {
	const char	*buf;
	size_t		 len;
	size_t		 offset;
	const char	*err;
};

struct dns_header {
	uint16_t	id;
	uint16_t	flags;
	uint16_t	qdcount;
	uint16_t	ancount;
	uint16_t	nscount;
	uint16_t	arcount;
};

struct dns_query {
	char		q_dname[MAXDNAME];
	uint16_t	q_type;
	uint16_t	q_class;
};

struct dns_rr {
	char		rr_dname[MAXDNAME];
	uint16_t	rr_type;
	uint16_t	rr_class;
	uint32_t	rr_ttl;
	union {
		struct {
			char	cname[MAXDNAME];
		} cname;
		struct {
			uint16_t	preference;
			char		exchange[MAXDNAME];
		} mx;
		struct {
			char	nsname[MAXDNAME];
		} ns;
		struct {
			char	ptrname[MAXDNAME];
		} ptr;
		struct {
			char		mname[MAXDNAME];
			char		rname[MAXDNAME];
			uint32_t	serial;
			uint32_t	refresh;
			uint32_t	retry;
			uint32_t	expire;
			uint32_t	minimum;
		} soa;
		struct {
			struct in_addr	addr;
		} in_a;
		struct {
			struct in6_addr	addr6;
		} in_aaaa;
		struct {
			uint16_t	 rdlen;
			const void	*rdata;
		} other;
	} rr;
};

static char *print_dname(const char *, char *, size_t);
static ssize_t dname_expand(const unsigned char *, size_t, size_t, size_t *,
    char *, size_t);
static int unpack_data(struct unpack *, void *, size_t);
static int unpack_u16(struct unpack *, uint16_t *);
static int unpack_u32(struct unpack *, uint32_t *);
static int unpack_inaddr(struct unpack *, struct in_addr *);
static int unpack_in6addr(struct unpack *, struct in6_addr *);
static int unpack_dname(struct unpack *, char *, size_t);
static void unpack_init(struct unpack *, const char *, size_t);
static int unpack_header(struct unpack *, struct dns_header *);
static int unpack_query(struct unpack *, struct dns_query *);
static int unpack_rr(struct unpack *, struct dns_rr *);


static int
domainname_is_addr(const char *s, struct sockaddr *sa, socklen_t *sl)
{
	struct addrinfo	hints, *res;
	socklen_t	sl2;
	size_t		l;
	char		buf[SMTPD_MAXDOMAINPARTSIZE];
	int		i6, error;

	if (*s != '[')
		return (0);

	i6 = (strncasecmp("[IPv6:", s, 6) == 0);
	s += i6 ? 6 : 1;

	l = strlcpy(buf, s, sizeof(buf));
	if (l >= sizeof(buf) || l == 0 || buf[l - 1] != ']')
		return (0);

	buf[l - 1] = '\0';
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
	if (i6)
		hints.ai_family = AF_INET6;

	res = NULL;
	if ((error = getaddrinfo(buf, NULL, &hints, &res))) {
		log_warnx("getaddrinfo: %s", gai_strerror(error));
	}

	if (!res)
		return (0);

	if (sa && sl) {
		sl2 = *sl;
		if (sl2 > res->ai_addrlen)
			sl2 = res->ai_addrlen;
		memmove(sa, res->ai_addr, sl2);
		*sl = res->ai_addrlen;
	}

	freeaddrinfo(res);
	return (1);
}

void
dns_imsg(struct mproc *p, struct imsg *imsg)
{
	struct sockaddr_storage	 ss;
	struct dns_session	*s;
	struct sockaddr		*sa;
	struct asr_query	*as;
	struct msg		 m;
	const char		*domain, *mx, *host;
	socklen_t		 sl;

	s = xcalloc(1, sizeof *s, "dns_imsg");
	s->type = imsg->hdr.type;
	s->p = p;

	m_msg(&m, imsg);
	m_get_id(&m, &s->reqid);

	switch (s->type) {

	case IMSG_MTA_DNS_HOST:
		m_get_string(&m, &host);
		m_end(&m);
		dns_lookup_host(s, host, -1);
		return;

	case IMSG_MTA_DNS_PTR:
	case IMSG_SMTP_DNS_PTR:
		sa = (struct sockaddr *)&ss;
		m_get_sockaddr(&m, sa);
		m_end(&m);
		as = getnameinfo_async(sa, sa->sa_len, s->name, sizeof(s->name),
		    NULL, 0, 0, NULL);
		event_asr_run(as, dns_dispatch_ptr, s);
		return;

	case IMSG_MTA_DNS_MX:
		m_get_string(&m, &domain);
		m_end(&m);
		(void)strlcpy(s->name, domain, sizeof(s->name));

		sa = (struct sockaddr *)&ss;
		sl = sizeof(ss);

		if (domainname_is_addr(domain, sa, &sl)) {
			m_create(s->p, IMSG_MTA_DNS_HOST, 0, 0, -1);
			m_add_id(s->p, s->reqid);
			m_add_sockaddr(s->p, sa);
			m_add_int(s->p, -1);
			m_close(s->p);

			m_create(s->p, IMSG_MTA_DNS_HOST_END, 0, 0, -1);
			m_add_id(s->p, s->reqid);
			m_add_int(s->p, DNS_OK);
			m_close(s->p);
			free(s);
			return;
		}

		as = res_query_async(s->name, C_IN, T_MX, NULL);
		if (as == NULL) {
			log_warn("warn: req_query_async: %s", s->name);
			m_create(s->p, IMSG_MTA_DNS_HOST_END, 0, 0, -1);
			m_add_id(s->p, s->reqid);
			m_add_int(s->p, DNS_EINVAL);
			m_close(s->p);
			free(s);
			return;
		}

		event_asr_run(as, dns_dispatch_mx, s);
		return;

	case IMSG_MTA_DNS_MX_PREFERENCE:
		m_get_string(&m, &domain);
		m_get_string(&m, &mx);
		m_end(&m);
		(void)strlcpy(s->name, mx, sizeof(s->name));

		as = res_query_async(domain, C_IN, T_MX, NULL);
		if (as == NULL) {
			m_create(s->p, IMSG_MTA_DNS_MX_PREFERENCE, 0, 0, -1);
			m_add_id(s->p, s->reqid);
			m_add_int(s->p, DNS_ENOTFOUND);
			m_close(s->p);
			free(s);
			return;
		}

		event_asr_run(as, dns_dispatch_mx_preference, s);
		return;

	default:
		log_warnx("warn: bad dns request %d", s->type);
		fatal(NULL);
	}
}

static void
dns_dispatch_host(struct asr_result *ar, void *arg)
{
	struct dns_session	*s;
	struct dns_lookup	*lookup = arg;
	struct addrinfo		*ai;

	s = lookup->session;

	for (ai = ar->ar_addrinfo; ai; ai = ai->ai_next) {
		s->mxfound++;
		m_create(s->p, IMSG_MTA_DNS_HOST, 0, 0, -1);
		m_add_id(s->p, s->reqid);
		m_add_sockaddr(s->p, ai->ai_addr);
		m_add_int(s->p, lookup->preference);
		m_close(s->p);
	}
	free(lookup);
	if (ar->ar_addrinfo)
		freeaddrinfo(ar->ar_addrinfo);

	if (ar->ar_gai_errno)
		s->error = ar->ar_gai_errno;

	if (--s->refcount)
		return;

	m_create(s->p, IMSG_MTA_DNS_HOST_END, 0, 0, -1);
	m_add_id(s->p, s->reqid);
	m_add_int(s->p, s->mxfound ? DNS_OK : DNS_ENOTFOUND);
	m_close(s->p);
	free(s);
}

static void
dns_dispatch_ptr(struct asr_result *ar, void *arg)
{
	struct dns_session	*s = arg;

	/* The error code could be more precise, but we don't currently care */
	m_create(s->p,  s->type, 0, 0, -1);
	m_add_id(s->p, s->reqid);
	m_add_int(s->p, ar->ar_gai_errno ? DNS_ENOTFOUND : DNS_OK);
	if (ar->ar_gai_errno == 0)
		m_add_string(s->p, s->name);
	m_close(s->p);
	free(s);
}

static void
dns_dispatch_mx(struct asr_result *ar, void *arg)
{
	struct dns_session	*s = arg;
	struct unpack		 pack;
	struct dns_header	 h;
	struct dns_query	 q;
	struct dns_rr		 rr;
	char			 buf[512];
	size_t			 found;

	if (ar->ar_h_errno && ar->ar_h_errno != NO_DATA) {

		m_create(s->p,  IMSG_MTA_DNS_HOST_END, 0, 0, -1);
		m_add_id(s->p, s->reqid);
		if (ar->ar_rcode == NXDOMAIN)
			m_add_int(s->p, DNS_ENONAME);
		else if (ar->ar_h_errno == NO_RECOVERY)
			m_add_int(s->p, DNS_EINVAL);
		else
			m_add_int(s->p, DNS_RETRY);
		m_close(s->p);
		free(s);
		free(ar->ar_data);
		return;
	}

	found = 0;

	unpack_init(&pack, ar->ar_data, ar->ar_datalen);
	if (unpack_header(&pack, &h) == -1 || unpack_query(&pack, &q) == -1)
		return;

	for (; h.ancount; h.ancount--) {
		if (unpack_rr(&pack, &rr) == -1)
			break;
		if (rr.rr_type != T_MX)
			continue;
		print_dname(rr.rr.mx.exchange, buf, sizeof(buf));
		buf[strlen(buf) - 1] = '\0';
		dns_lookup_host(s, buf, rr.rr.mx.preference);
		found++;
	}
	free(ar->ar_data);

	/* fallback to host if no MX is found. */
	if (found == 0)
		dns_lookup_host(s, s->name, 0);
}

static void
dns_dispatch_mx_preference(struct asr_result *ar, void *arg)
{
	struct dns_session	*s = arg;
	struct unpack		 pack;
	struct dns_header	 h;
	struct dns_query	 q;
	struct dns_rr		 rr;
	char			 buf[512];
	int			 error;

	if (ar->ar_h_errno) {
		if (ar->ar_rcode == NXDOMAIN)
			error = DNS_ENONAME;
		else if (ar->ar_h_errno == NO_RECOVERY
		    || ar->ar_h_errno == NO_DATA)
			error = DNS_EINVAL;
		else
			error = DNS_RETRY;
	}
	else {
		error = DNS_ENOTFOUND;
		unpack_init(&pack, ar->ar_data, ar->ar_datalen);
		if (unpack_header(&pack, &h) != -1 &&
		    unpack_query(&pack, &q) != -1) {
			for (; h.ancount; h.ancount--) {
				if (unpack_rr(&pack, &rr) == -1)
					break;
				if (rr.rr_type != T_MX)
					continue;
				print_dname(rr.rr.mx.exchange, buf, sizeof(buf));
				buf[strlen(buf) - 1] = '\0';
				if (!strcasecmp(s->name, buf)) {
					error = DNS_OK;
					break;
				}
			}
		}
	}

	free(ar->ar_data);

	m_create(s->p, IMSG_MTA_DNS_MX_PREFERENCE, 0, 0, -1);
	m_add_id(s->p, s->reqid);
	m_add_int(s->p, error);
	if (error == DNS_OK)
		m_add_int(s->p, rr.rr.mx.preference);
	m_close(s->p);
	free(s);
}

static void
dns_lookup_host(struct dns_session *s, const char *host, int preference)
{
	struct dns_lookup	*lookup;
	struct addrinfo		 hints;
	char			 hostcopy[HOST_NAME_MAX+1];
	char			*p;
	void			*as;

	lookup = xcalloc(1, sizeof *lookup, "dns_lookup_host");
	lookup->preference = preference;
	lookup->session = s;
	s->refcount++;

	if (*host == '[') {
		if (strncasecmp("[IPv6:", host, 6) == 0)
			host += 6;
		else
			host += 1;
		(void)strlcpy(hostcopy, host, sizeof hostcopy);
		p = strchr(hostcopy, ']');
		if (p)
			*p = 0;
		host = hostcopy;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	as = getaddrinfo_async(host, NULL, &hints, NULL);
	event_asr_run(as, dns_dispatch_host, lookup);
}

static char *
print_dname(const char *_dname, char *buf, size_t max)
{
	const unsigned char *dname = _dname;
	char    *res;
	size_t   left, n, count;

	if (_dname[0] == 0) {
		(void)strlcpy(buf, ".", max);
		return buf;
	}

	res = buf;
	left = max - 1;
	for (n = 0; dname[0] && left; n += dname[0]) {
		count = (dname[0] < (left - 1)) ? dname[0] : (left - 1);
		memmove(buf, dname + 1, count);
		dname += dname[0] + 1;
		left -= count;
		buf += count;
		if (left) {
			left -= 1;
			*buf++ = '.';
		}
	}
	buf[0] = 0;

	return (res);
}

static ssize_t
dname_expand(const unsigned char *data, size_t len, size_t offset,
    size_t *newoffset, char *dst, size_t max)
{
	size_t		 n, count, end, ptr, start;
	ssize_t		 res;

	if (offset >= len)
		return (-1);

	res = 0;
	end = start = offset;

	for (; (n = data[offset]); ) {
		if ((n & 0xc0) == 0xc0) {
			if (offset + 2 > len)
				return (-1);
			ptr = 256 * (n & ~0xc0) + data[offset + 1];
			if (ptr >= start)
				return (-1);
			if (end < offset + 2)
				end = offset + 2;
			offset = start = ptr;
			continue;
		}
		if (offset + n + 1 > len)
			return (-1);

		/* copy n + at offset+1 */
		if (dst != NULL && max != 0) {
			count = (max < n + 1) ? (max) : (n + 1);
			memmove(dst, data + offset, count);
			dst += count;
			max -= count;
		}
		res += n + 1;
		offset += n + 1;
		if (end < offset)
			end = offset;
	}
	if (end < offset + 1)
		end = offset + 1;

	if (dst != NULL && max != 0)
		dst[0] = 0;
	if (newoffset)
		*newoffset = end;
	return (res + 1);
}

void
unpack_init(struct unpack *unpack, const char *buf, size_t len)
{
	unpack->buf = buf;
	unpack->len = len;
	unpack->offset = 0;
	unpack->err = NULL;
}

static int
unpack_data(struct unpack *p, void *data, size_t len)
{
	if (p->err)
		return (-1);

	if (p->len - p->offset < len) {
		p->err = "too short";
		return (-1);
	}

	memmove(data, p->buf + p->offset, len);
	p->offset += len;

	return (0);
}

static int
unpack_u16(struct unpack *p, uint16_t *u16)
{
	if (unpack_data(p, u16, 2) == -1)
		return (-1);

	*u16 = ntohs(*u16);

	return (0);
}

static int
unpack_u32(struct unpack *p, uint32_t *u32)
{
	if (unpack_data(p, u32, 4) == -1)
		return (-1);

	*u32 = ntohl(*u32);

	return (0);
}

static int
unpack_inaddr(struct unpack *p, struct in_addr *a)
{
	return (unpack_data(p, a, 4));
}

static int
unpack_in6addr(struct unpack *p, struct in6_addr *a6)
{
	return (unpack_data(p, a6, 16));
}

static int
unpack_dname(struct unpack *p, char *dst, size_t max)
{
	ssize_t e;

	if (p->err)
		return (-1);

	e = dname_expand(p->buf, p->len, p->offset, &p->offset, dst, max);
	if (e == -1) {
		p->err = "bad domain name";
		return (-1);
	}
	if (e < 0 || e > MAXDNAME) {
		p->err = "domain name too long";
		return (-1);
	}

	return (0);
}

static int
unpack_header(struct unpack *p, struct dns_header *h)
{
	if (unpack_data(p, h, HFIXEDSZ) == -1)
		return (-1);

	h->flags = ntohs(h->flags);
	h->qdcount = ntohs(h->qdcount);
	h->ancount = ntohs(h->ancount);
	h->nscount = ntohs(h->nscount);
	h->arcount = ntohs(h->arcount);

	return (0);
}

static int
unpack_query(struct unpack *p, struct dns_query *q)
{
	unpack_dname(p, q->q_dname, sizeof(q->q_dname));
	unpack_u16(p, &q->q_type);
	unpack_u16(p, &q->q_class);

	return (p->err) ? (-1) : (0);
}

static int
unpack_rr(struct unpack *p, struct dns_rr *rr)
{
	uint16_t	rdlen;
	size_t		save_offset;

	unpack_dname(p, rr->rr_dname, sizeof(rr->rr_dname));
	unpack_u16(p, &rr->rr_type);
	unpack_u16(p, &rr->rr_class);
	unpack_u32(p, &rr->rr_ttl);
	unpack_u16(p, &rdlen);

	if (p->err)
		return (-1);

	if (p->len - p->offset < rdlen) {
		p->err = "too short";
		return (-1);
	}

	save_offset = p->offset;

	switch (rr->rr_type) {

	case T_CNAME:
		unpack_dname(p, rr->rr.cname.cname, sizeof(rr->rr.cname.cname));
		break;

	case T_MX:
		unpack_u16(p, &rr->rr.mx.preference);
		unpack_dname(p, rr->rr.mx.exchange, sizeof(rr->rr.mx.exchange));
		break;

	case T_NS:
		unpack_dname(p, rr->rr.ns.nsname, sizeof(rr->rr.ns.nsname));
		break;

	case T_PTR:
		unpack_dname(p, rr->rr.ptr.ptrname, sizeof(rr->rr.ptr.ptrname));
		break;

	case T_SOA:
		unpack_dname(p, rr->rr.soa.mname, sizeof(rr->rr.soa.mname));
		unpack_dname(p, rr->rr.soa.rname, sizeof(rr->rr.soa.rname));
		unpack_u32(p, &rr->rr.soa.serial);
		unpack_u32(p, &rr->rr.soa.refresh);
		unpack_u32(p, &rr->rr.soa.retry);
		unpack_u32(p, &rr->rr.soa.expire);
		unpack_u32(p, &rr->rr.soa.minimum);
		break;

	case T_A:
		if (rr->rr_class != C_IN)
			goto other;
		unpack_inaddr(p, &rr->rr.in_a.addr);
		break;

	case T_AAAA:
		if (rr->rr_class != C_IN)
			goto other;
		unpack_in6addr(p, &rr->rr.in_aaaa.addr6);
		break;
	default:
	other:
		rr->rr.other.rdata = p->buf + p->offset;
		rr->rr.other.rdlen = rdlen;
		p->offset += rdlen;
	}

	if (p->err)
		return (-1);

	/* make sure that the advertised rdlen is really ok */
	if (p->offset - save_offset != rdlen)
		p->err = "bad dlen";

	return (p->err) ? (-1) : (0);
}
