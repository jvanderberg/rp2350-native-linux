// SPDX-License-Identifier: MIT
/*
 * mdnsd: a small mDNS responder (RFC 6762) with DNS-SD (RFC 6763), IPv4
 * only, so that the board answers as HOST.local and shows up in service
 * browsers.
 *
 *   mdnsd [-v] [-i IFACE] [-n HOST] [SERVICE:PORT]...
 *
 *   mdnsd _ssh._tcp:22 _telnet._tcp:23
 *
 * HOST is the hostname by default and IFACE eth0. Each SERVICE is announced
 * as the instance "HOST.SERVICE.local" on HOST.local:PORT, with an empty
 * TXT record. The records:
 *
 *   HOST.local                    A     the interface's address
 *   HOST.local                    NSEC  "only A here", so that a resolver
 *                                       asking for AAAA stops waiting
 *   D.C.B.A.in-addr.arpa          PTR   HOST.local
 *   _services._dns-sd._udp.local  PTR   SERVICE.local (per service)
 *   SERVICE.local                 PTR   HOST.SERVICE.local
 *   HOST.SERVICE.local            SRV   0 0 PORT HOST.local
 *   HOST.SERVICE.local            TXT   (empty)
 *
 * Queries from port 5353 are answered by multicast, or by unicast when
 * they ask for it (the QU bit); others (dig @224.0.0.251 -p 5353, a
 * resolver's direct query) get a legacy unicast reply to their port.
 * Queries from outside the interface's subnet are ignored, and a record
 * that went out by multicast in the last second is left out of the next
 * multicast reply. The
 * records are announced twice when the interface gets an address or its
 * link comes back, and on SIGHUP; SIGTERM sends them again with TTL 0
 * (goodbye) and exits.
 *
 * Left out, as a responder for one board: probing for name conflicts,
 * known-answer suppression and the random response delay.
 *
 * The chip passes the mDNS group's frames only because core 1 puts its
 * address in the chip's multicast filter (boot/src/wifi.rs). With
 * CONFIG_IP_MULTICAST off the kernel still joins groups and delivers their
 * datagrams; it only sends no IGMP reports, which 224.0.0.x doesn't need.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 5353
#define GROUP "224.0.0.251"
#define MAXSVC 8
#define NAMELEN 256
#define MAXQ 8
/* RFC 6762 10: records about the host 120 s, the rest 75 min */
#define TTL_HOST 120
#define TTL_OTHER 4500
#define TTL_LEGACY 10

enum { T_A = 1, T_PTR = 12, T_TXT = 16, T_SRV = 33, T_NSEC = 47, T_ANY = 255 };

/* Record numbers: the host's three, then S_N per service. */
enum { R_A, R_NSEC, R_REV, R_SVC0 };
enum { S_ENUM, S_PTR, S_SRV, S_TXT, S_N };
#define BIT(r) ((uint64_t)1 << (r))
#define SVC(s, k) (R_SVC0 + (s) * S_N + (k))

static const char *ifname = "eth0";
static char hostname[NAMELEN];	/* HOST.local */
static char revname[NAMELEN];	/* D.C.B.A.in-addr.arpa */
static struct {
	char type[NAMELEN];	/* SERVICE.local */
	char inst[NAMELEN];	/* HOST.SERVICE.local */
	uint16_t port;
} svc[MAXSVC];
static int nsvc, verbose;
static struct in_addr addr;	/* 0: no address, or the link is down */
static struct in_addr mask;	/* its netmask: queries from off the link are dropped */
/* when each record was last multicast (RFC 6762 6: at most once a second) */
static long long last_mc[64];
static struct sockaddr_in group;
static volatile sig_atomic_t stop, hup;

static int nrec(void)
{
	return R_SVC0 + nsvc * S_N;
}

/* Record r's name, type and TTL; returns whether it is unique to this host
 * (sent with the cache-flush bit) rather than shared. */
static int rr_info(int r, const char **name, int *type, int *ttl)
{
	int s = (r - R_SVC0) / S_N;

	*ttl = TTL_HOST;
	switch (r) {
	case R_A: *name = hostname; *type = T_A; return 1;
	case R_NSEC: *name = hostname; *type = T_NSEC; return 1;
	case R_REV: *name = revname; *type = T_PTR; return 1;
	}
	switch ((r - R_SVC0) % S_N) {
	case S_ENUM:
		*name = "_services._dns-sd._udp.local"; *type = T_PTR; *ttl = TTL_OTHER;
		return 0;
	case S_PTR: *name = svc[s].type; *type = T_PTR; *ttl = TTL_OTHER; return 0;
	case S_SRV: *name = svc[s].inst; *type = T_SRV; return 1;
	default: *name = svc[s].inst; *type = T_TXT; *ttl = TTL_OTHER; return 1;
	}
}

/* ---- packets */

struct pkt {
	uint8_t b[1460];
	int n, err;
};

static void put8(struct pkt *p, unsigned v)
{
	if (p->n >= (int)sizeof(p->b)) {
		p->err = 1;
		return;
	}
	p->b[p->n++] = v;
}

static void put16(struct pkt *p, unsigned v)
{
	put8(p, v >> 8);
	put8(p, v);
}

static void put32(struct pkt *p, uint32_t v)
{
	put16(p, v >> 16);
	put16(p, v);
}

static void putbytes(struct pkt *p, const void *v, size_t n)
{
	const uint8_t *c = v;

	while (n--)
		put8(p, *c++);
}

/* A dotted name, uncompressed. */
static void putname(struct pkt *p, const char *name)
{
	while (*name) {
		const char *dot = strchr(name, '.');
		size_t l = dot ? (size_t)(dot - name) : strlen(name);

		if (l == 0 || l > 63) {
			p->err = 1;
			return;
		}
		put8(p, l);
		putbytes(p, name, l);
		name += l + (dot != NULL);
	}
	put8(p, 0);
}

/* Record r; ttl < 0: its own TTL. No cache-flush bit in legacy replies. */
static void put_rr(struct pkt *p, int r, int ttl, int legacy)
{
	const char *name;
	int type, own, s = (r - R_SVC0) / S_N, at;
	int uniq = rr_info(r, &name, &type, &own);

	if (ttl < 0)
		ttl = legacy && own > TTL_LEGACY ? TTL_LEGACY : own;
	putname(p, name);
	put16(p, type);
	put16(p, 1 | (uniq && !legacy ? 0x8000 : 0));
	put32(p, ttl);
	at = p->n;
	put16(p, 0);
	switch (r < R_SVC0 ? r : R_SVC0 + (r - R_SVC0) % S_N) {
	case R_A: putbytes(p, &addr.s_addr, 4); break;
	case R_NSEC:
		/* next name: itself; window 0, one byte, bit 1 (A) */
		putname(p, hostname);
		put8(p, 0);
		put8(p, 1);
		put8(p, 0x40);
		break;
	case R_REV: putname(p, hostname); break;
	case R_SVC0 + S_ENUM: putname(p, svc[s].type); break;
	case R_SVC0 + S_PTR: putname(p, svc[s].inst); break;
	case R_SVC0 + S_SRV:
		put16(p, 0);
		put16(p, 0);
		put16(p, svc[s].port);
		putname(p, hostname);
		break;
	default: put8(p, 0); break;	/* one empty string */
	}
	if (!p->err && p->n - at - 2 >= 0) {
		p->b[at] = (p->n - at - 2) >> 8;
		p->b[at + 1] = p->n - at - 2;
	}
}

static int count(uint64_t m)
{
	int n = 0;

	for (; m; m &= m - 1)
		n++;
	return n;
}

static long long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void send_pkt(int fd, struct pkt *p, const struct sockaddr_in *to)
{
	if (p->err) {
		fprintf(stderr, "mdnsd: reply too big for one packet\n");
		return;
	}
	if (sendto(fd, p->b, p->n, 0, (const struct sockaddr *)to, sizeof(*to)) < 0 && verbose)
		perror("mdnsd: sendto");
	else if (verbose)
		fprintf(stderr, "mdnsd: %d bytes to %s:%d\n", p->n, inet_ntoa(to->sin_addr),
			ntohs(to->sin_port));
}

/* An unsolicited response with records `m` (ttl: 0 for goodbye, else -1). */
static void announce_set(int fd, uint64_t m, int ttl)
{
	struct pkt p = { .n = 0 };
	long long now = now_ms();
	int r;

	put16(&p, 0);
	put16(&p, 0x8400);	/* response, authoritative */
	put16(&p, 0);
	put16(&p, count(m));
	put16(&p, 0);
	put16(&p, 0);
	for (r = 0; r < nrec(); r++)
		if (m & BIT(r)) {
			put_rr(&p, r, ttl, 0);
			last_mc[r] = now;
		}
	send_pkt(fd, &p, &group);
}

/* Every record: the host's in one packet, then a packet per service. */
static void announce(int fd, int ttl)
{
	int s;

	if (!addr.s_addr)
		return;
	announce_set(fd, BIT(R_A) | BIT(R_NSEC) | BIT(R_REV), ttl);
	for (s = 0; s < nsvc; s++)
		announce_set(fd, BIT(SVC(s, S_ENUM)) | BIT(SVC(s, S_PTR)) | BIT(SVC(s, S_SRV)) |
				 BIT(SVC(s, S_TXT)), ttl);
}

/* The name at *off in a message of n bytes, dotted, with compression
 * pointers followed; *off moves past it. */
static int getname(const uint8_t *b, int n, int *off, char *out)
{
	int o = *off, len = 0, jumped = 0, hops = 0;

	for (;;) {
		unsigned l;

		if (o >= n)
			return -1;
		l = b[o];
		if ((l & 0xc0) == 0xc0) {
			if (o + 1 >= n || ++hops > 16)
				return -1;
			if (!jumped)
				*off = o + 2;
			jumped = 1;
			o = (l & 0x3f) << 8 | b[o + 1];
			continue;
		}
		if (l & 0xc0)
			return -1;
		o++;
		if (!l)
			break;
		if (o + (int)l > n || len + l + 2 > NAMELEN)
			return -1;
		if (len)
			out[len++] = '.';
		memcpy(out + len, b + o, l);
		len += l;
		o += l;
	}
	out[len] = 0;
	if (!jumped)
		*off = o;
	return 0;
}

/* The records that answer a question for `name`, type `type`. */
static uint64_t match(const char *name, int type)
{
	uint64_t m = 0;
	int r;

	for (r = 0; r < nrec(); r++) {
		const char *rn;
		int rt, ttl;

		rr_info(r, &rn, &rt, &ttl);
		if (strcasecmp(name, rn))
			continue;
		if (type == rt || type == T_ANY)
			m |= BIT(r);
		/* any other type for the host (AAAA): it has none */
		else if (r == R_NSEC && type != T_A)
			m |= BIT(r);
	}
	return m;
}

static void handle(int fd, const uint8_t *b, int n, const struct sockaddr_in *from)
{
	static struct {
		char name[NAMELEN];
		uint16_t type, cls;
	} q[MAXQ];
	char name[NAMELEN];
	unsigned flags, qd;
	uint64_t ans = 0, add = 0;
	int legacy = ntohs(from->sin_port) != PORT, qu = 0, nq = 0, off = 12, i, r;
	struct pkt p = { .n = 0 };
	long long now = now_ms();

	if (n < 12 || !addr.s_addr)
		return;
	/* RFC 6762 11: only queries from the local link */
	if ((from->sin_addr.s_addr ^ addr.s_addr) & mask.s_addr)
		return;
	flags = b[2] << 8 | b[3];
	qd = b[4] << 8 | b[5];
	/* responses, and opcodes other than a standard query */
	if (flags & 0xf800)
		return;
	for (i = 0; i < (int)qd; i++) {
		unsigned type, cls;
		uint64_t m;

		if (getname(b, n, &off, name) < 0 || off + 4 > n)
			return;
		type = b[off] << 8 | b[off + 1];
		cls = b[off + 2] << 8 | b[off + 3];
		off += 4;
		/* a legacy reply echoes the questions */
		if (nq < MAXQ) {
			strcpy(q[nq].name, name);
			q[nq].type = type;
			q[nq].cls = cls & 0x7fff;
			nq++;
		}
		if ((cls & 0x7fff) != 1 && (cls & 0x7fff) != 255)
			continue;
		m = match(name, type);
		if (verbose)
			fprintf(stderr, "mdnsd: %s:%d asks %s type %u%s%s\n", inet_ntoa(from->sin_addr),
				ntohs(from->sin_port), name, type, cls & 0x8000 ? " (QU)" : "",
				m ? ": ours" : "");
		if (!m)
			continue;
		if (cls & 0x8000)
			qu = 1;
		ans |= m;
	}
	/* a multicast reply leaves out what went out by multicast in the
	 * last second */
	if (!legacy && !qu)
		for (r = 0; r < nrec(); r++)
			if ((ans & BIT(r)) && last_mc[r] && now - last_mc[r] < 1000)
				ans &= ~BIT(r);
	if (!ans)
		return;
	/* what the asker will want next */
	for (r = 0; r < nrec(); r++) {
		if (!(ans & BIT(r)))
			continue;
		if (r == R_A)
			add |= BIT(R_NSEC);
		else if (r >= R_SVC0 && (r - R_SVC0) % S_N == S_PTR)
			add |= BIT(r + 1) | BIT(r + 2) | BIT(R_A) | BIT(R_NSEC);
		else if (r >= R_SVC0 && (r - R_SVC0) % S_N == S_SRV)
			add |= BIT(R_A) | BIT(R_NSEC);
	}
	add &= ~ans;
	/* a legacy reply carries the query's ID and questions */
	put16(&p, legacy ? (b[0] << 8 | b[1]) : 0);
	put16(&p, 0x8400);
	put16(&p, legacy ? nq : 0);
	put16(&p, count(ans));
	put16(&p, 0);
	put16(&p, count(add));
	for (i = 0; legacy && i < nq; i++) {
		putname(&p, q[i].name);
		put16(&p, q[i].type);
		put16(&p, q[i].cls);
	}
	for (r = 0; r < nrec(); r++)
		if (ans & BIT(r))
			put_rr(&p, r, -1, legacy);
	for (r = 0; r < nrec(); r++)
		if (add & BIT(r))
			put_rr(&p, r, -1, legacy);
	if (legacy || qu) {
		send_pkt(fd, &p, from);
		return;
	}
	send_pkt(fd, &p, &group);
	for (r = 0; r < nrec(); r++)
		if ((ans | add) & BIT(r))
			last_mc[r] = now;
}

/* ---- the interface */

static int ifreq(int fd, unsigned long req, struct ifreq *ifr)
{
	memset(ifr, 0, sizeof(*ifr));
	strncpy(ifr->ifr_name, ifname, IFNAMSIZ - 1);
	return ioctl(fd, req, ifr);
}

/* Join the group on the interface, and send through it. */
static int join(int fd)
{
	struct ifreq ifr;
	struct ip_mreqn mr;

	if (ifreq(fd, SIOCGIFINDEX, &ifr) < 0)
		return 0;
	memset(&mr, 0, sizeof(mr));
	inet_aton(GROUP, &mr.imr_multiaddr);
	mr.imr_ifindex = ifr.ifr_ifindex;
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0 && errno != EADDRINUSE) {
		perror("mdnsd: IP_ADD_MEMBERSHIP");
		return 0;
	}
	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof(mr)) < 0) {
		perror("mdnsd: IP_MULTICAST_IF");
		return 0;
	}
	return 1;
}

/* The interface's address while its link is up, else 0, and its netmask. */
static struct in_addr ifaddr(int fd, struct in_addr *m)
{
	struct ifreq ifr;
	struct in_addr a = { 0 };

	if (ifreq(fd, SIOCGIFFLAGS, &ifr) < 0 || !(ifr.ifr_flags & IFF_UP) ||
	    !(ifr.ifr_flags & IFF_RUNNING))
		return a;
	if (ifreq(fd, SIOCGIFNETMASK, &ifr) < 0)
		return a;
	*m = ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr;
	if (ifreq(fd, SIOCGIFADDR, &ifr) < 0)
		return a;
	return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
}

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		hup = 1;
	else
		stop = 1;
}

static void usage(void)
{
	fprintf(stderr, "usage: mdnsd [-v] [-i IFACE] [-n HOST] [SERVICE:PORT]...\n"
			"  e.g. mdnsd _ssh._tcp:22\n");
	exit(2);
}

int main(int argc, char **argv)
{
	char host[64] = "", *colon;
	struct sockaddr_in sa;
	struct sigaction sig;
	long long next_check = 0, next_announce = 0;
	int fd, c, one = 1, ttl = 255, loop = 0, joined = 0, announcing = 0, i;

	while ((c = getopt(argc, argv, "vi:n:")) != -1) {
		switch (c) {
		case 'v': verbose = 1; break;
		case 'i': ifname = optarg; break;
		case 'n': snprintf(host, sizeof(host), "%s", optarg); break;
		default: usage();
		}
	}
	if (!*host && gethostname(host, sizeof(host) - 1) < 0)
		strcpy(host, "rp2350");
	if (!*host || strchr(host, '.') || strlen(host) > 63)
		usage();
	snprintf(hostname, sizeof(hostname), "%s.local", host);
	for (i = optind; i < argc; i++) {
		char *end;
		long port;

		colon = strrchr(argv[i], ':');
		if (nsvc == MAXSVC || !colon || colon == argv[i] || colon - argv[i] > 100)
			usage();
		port = strtol(colon + 1, &end, 10);
		if (*end || port <= 0 || port > 65535)
			usage();
		*colon = 0;
		snprintf(svc[nsvc].type, NAMELEN, "%s.local", argv[i]);
		snprintf(svc[nsvc].inst, NAMELEN, "%s.%s.local", host, argv[i]);
		svc[nsvc].port = port;
		nsvc++;
	}

	memset(&sig, 0, sizeof(sig));
	sig.sa_handler = on_signal;	/* no SA_RESTART: poll returns at once */
	sigaction(SIGTERM, &sig, NULL);
	sigaction(SIGHUP, &sig, NULL);
	/* SIGINT stays ignored when the shell started us with & */
	{
		struct sigaction old;

		if (sigaction(SIGINT, NULL, &old) == 0 && old.sa_handler != SIG_IGN)
			sigaction(SIGINT, &sig, NULL);
	}
	signal(SIGPIPE, SIG_IGN);

	memset(&group, 0, sizeof(group));
	group.sin_family = AF_INET;
	group.sin_port = htons(PORT);
	inet_aton(GROUP, &group.sin_addr);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(PORT);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
	    bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("mdnsd: socket on port 5353");
		return 1;
	}
	/* RFC 6762 11: sent with IP TTL 255, unicast replies too; our own
	 * packets don't come back */
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long long now = now_ms(), wait;

		if (stop)
			break;
		if (hup) {
			hup = 0;
			announcing = 2;
			next_announce = now;
		}
		/* the interface every 2 s: joined yet, its address, its link */
		if (now >= next_check) {
			struct in_addr a = { 0 }, m = { 0 };

			next_check = now + 2000;
			if (!joined)
				joined = join(fd);
			if (joined)
				a = ifaddr(fd, &m);
			if (a.s_addr != addr.s_addr) {
				const uint8_t *o = (const uint8_t *)&a.s_addr;

				/* a new address on a live link: the old reverse
				 * name goes (the new A replaces the old one in
				 * caches by itself, being unique) */
				if (addr.s_addr && a.s_addr)
					announce_set(fd, BIT(R_REV), 0);
				addr = a;
				mask = m;
				memset(last_mc, 0, sizeof(last_mc));
				snprintf(revname, sizeof(revname), "%u.%u.%u.%u.in-addr.arpa", o[3], o[2],
					 o[1], o[0]);
				if (verbose)
					fprintf(stderr, "mdnsd: %s is %s\n", hostname,
						addr.s_addr ? inet_ntoa(addr) : "(none)");
				announcing = addr.s_addr ? 2 : 0;
				next_announce = now;
			}
		}
		if (announcing && now >= next_announce) {
			announce(fd, -1);
			announcing--;
			next_announce = now + 1000;
		}
		wait = next_check - now;
		if (announcing && next_announce - now < wait)
			wait = next_announce - now;
		if (wait < 0)
			wait = 0;
		if (poll(&pfd, 1, (int)wait) > 0 && (pfd.revents & POLLIN)) {
			static uint8_t buf[1500];
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);

			if (n > 0)
				handle(fd, buf, n, &from);
		}
	}
	announce(fd, 0);
	return 0;
}
