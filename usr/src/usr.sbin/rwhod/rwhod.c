#ifndef lint
static char sccsid[] = "@(#)rwhod.c	4.9 83/05/05";
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <netinet/in.h>

#include <nlist.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <utmp.h>
#include <ctype.h>
#include <netdb.h>
#include <rwhod.h>

struct	sockaddr_in sin = { AF_INET };

extern	errno;

char	myname[32];

struct	nlist nl[] = {
#define	NL_AVENRUN	0
	{ "_avenrun" },
#define	NL_BOOTTIME	1
	{ "_boottime" },
	0
};

/*
 * We communicate with each neighbor in
 * a list constructed at the time we're
 * started up.  Neighbors are currently
 * directly connected via a hardware interface.
 */
struct	neighbor {
	struct	neighbor *n_next;
	char	*n_name;		/* interface name */
	char	*n_addr;		/* who to send to */
	int	n_addrlen;		/* size of address */
	int	n_flags;		/* should forward?, interface flags */
};

struct	neighbor *neighbors;
struct	whod mywd;
struct	servent *sp;
int	s, utmpf, kmemf = -1;

#define	WHDRSIZE	(sizeof (mywd) - sizeof (mywd.wd_we))
#define	RWHODIR		"/usr/spool/rwho"

int	onalrm();
char	*strcpy(), *sprintf(), *malloc();
long	lseek();
int	getkmem();
struct	in_addr inet_makeaddr();

main()
{
	struct sockaddr_in from;
	char path[64];
	int addr;
	struct hostent *hp;

	sp = getservbyname("who", "udp");
	if (sp == 0) {
		fprintf(stderr, "rwhod: udp/who: unknown service\n");
		exit(1);
	}
#ifndef DEBUG
	if (fork())
		exit(0);
	{ int s;
	  for (s = 0; s < 10; s++)
		(void) close(s);
	  (void) open("/", 0);
	  (void) dup2(0, 1);
	  (void) dup2(0, 2);
	  s = open("/dev/tty", 2);
	  if (s >= 0) {
		ioctl(s, TIOCNOTTY, 0);
		(void) close(s);
	  }
	}
#endif
	(void) chdir("/dev");
	(void) signal(SIGHUP, getkmem);
	if (getuid()) {
		fprintf(stderr, "rwhod: not super user\n");
		exit(1);
	}
	/*
	 * Establish host name as returned by system.
	 */
	if (gethostname(myname, sizeof (myname) - 1) < 0) {
		perror("gethostname");
		exit(1);
	}
	strncpy(mywd.wd_hostname, myname, sizeof (myname) - 1);
	utmpf = open("/etc/utmp", 0);
	if (utmpf < 0) {
		(void) close(creat("/etc/utmp", 0644));
		utmpf = open("/etc/utmp", 0);
	}
	if (utmpf < 0) {
		perror("rwhod: /etc/utmp");
		exit(1);
	}
	getkmem();
	if ((s = socket(AF_INET, SOCK_DGRAM, 0, 0)) < 0) {
		perror("rwhod: socket");
		exit(1);
	}
	hp = gethostbyname(myname);
	if (hp == NULL) {
		fprintf(stderr, "%s: don't know my own name\n", myname);
		exit(1);
	}
	sin.sin_family = hp->h_addrtype;
	sin.sin_port = sp->s_port;
	if (bind(s, &sin, sizeof (sin), 0) < 0) {
		perror("rwhod: bind");
		exit(1);
	}
	if (!configure(s))
		exit(1);
	sigset(SIGALRM, onalrm);
	onalrm();
	for (;;) {
		struct whod wd;
		int cc, whod, len = sizeof (from);

		cc = recvfrom(s, (char *)&wd, sizeof (struct whod), 0,
			&from, &len);
		if (cc <= 0) {
			if (cc < 0 && errno != EINTR)
				perror("rwhod: recv");
			continue;
		}
		if (from.sin_port != sp->s_port) {
			fprintf(stderr, "rwhod: %d: bad from port\n",
				ntohs(from.sin_port));
			continue;
		}
#ifdef notdef
		if (gethostbyname(wd.wd_hostname) == 0) {
			fprintf(stderr, "rwhod: %s: unknown host\n",
				wd.wd_hostname);
			continue;
		}
#endif
		if (wd.wd_type != WHODTYPE_STATUS)
			continue;
		if (!verify(wd.wd_hostname)) {
			fprintf(stderr, "rwhod: malformed host name from %x\n",
				from.sin_addr);
			continue;
		}
		(void) sprintf(path, "%s/whod.%s", RWHODIR, wd.wd_hostname);
		whod = creat(path, 0666);
		if (whod < 0) {
			fprintf(stderr, "rwhod: ");
			perror(path);
			continue;
		}
#if vax || pdp11
		{
			int i, n = (cc - WHDRSIZE)/sizeof(struct utmp);
			struct whoent *we;

			/* undo header byte swapping before writing to file */
			wd.wd_sendtime = ntohl(wd.wd_sendtime);
			for (i = 0; i < 3; i++)
				wd.wd_loadav[i] = ntohl(wd.wd_loadav[i]);
			wd.wd_boottime = ntohl(wd.wd_boottime);
			we = wd.wd_we;
			for (i = 0; i < n; i++) {
				we->we_idle = ntohl(we->we_idle);
				we->we_utmp.ut_time = ntohl(we->we_utmp.ut_time);
				we++;
			}
		}
#endif
		(void) time(&wd.wd_recvtime);
		(void) write(whod, (char *)&wd, cc);
		(void) close(whod);
	}
}

/*
 * Check out host name for unprintables
 * and other funnies before allowing a file
 * to be created.  Sorry, but blanks aren't allowed.
 */
verify(name)
	register char *name;
{
	register int size = 0;

	while (*name) {
		if (!isascii(*name) || !isalnum(*name))
			return (0);
		name++, size++;
	}
	return (size > 0);
}

int	utmptime;
int	utmpent;
struct	utmp utmp[100];
int	alarmcount;

onalrm()
{
	register int i;
	struct stat stb;
	register struct whoent *we = mywd.wd_we, *wlast;
	int cc;
	double avenrun[3];
	time_t now = time(0);
	register struct neighbor *np;

	if (alarmcount % 10 == 0)
		getkmem();
	alarmcount++;
	(void) fstat(utmpf, &stb);
	if (stb.st_mtime != utmptime) {
		(void) lseek(utmpf, (long)0, 0);
		cc = read(utmpf, (char *)utmp, sizeof (utmp));
		if (cc < 0) {
			perror("/etc/utmp");
			return;
		}
		wlast = &mywd.wd_we[1024 / sizeof (struct whoent) - 1];
		utmpent = cc / sizeof (struct utmp);
		for (i = 0; i < utmpent; i++)
			if (utmp[i].ut_name[0]) {
				we->we_utmp = utmp[i];
				if (we >= wlast)
					break;
				we++;
			}
		utmpent = we - mywd.wd_we;
	}
	we = mywd.wd_we;
	for (i = 0; i < utmpent; i++) {
		if (stat(we->we_utmp.ut_line, &stb) >= 0)
			we->we_idle = now - stb.st_atime;
		we++;
	}
	(void) lseek(kmemf, (long)nl[NL_AVENRUN].n_value, 0);
	(void) read(kmemf, (char *)avenrun, sizeof (avenrun));
	for (i = 0; i < 3; i++)
		mywd.wd_loadav[i] = avenrun[i] * 100;
	cc = (char *)we - (char *)&mywd;
	(void) time(&mywd.wd_sendtime);
#if vax || pdp11
	mywd.wd_sendtime = htonl(mywd.wd_sendtime);
	for (i = 0; i < 3; i++)
		mywd.wd_loadav[i] = htonl(mywd.wd_loadav[i]);
	mywd.wd_boottime = htonl(mywd.wd_boottime);
	we = mywd.wd_we;
	for (i = 0; i < utmpent; i++) {
		we->we_idle = htonl(we->we_idle);
		we->we_utmp.ut_time = htonl(we->we_utmp.ut_time);
		we++;
	}
#endif
	mywd.wd_vers = WHODVERSION;
	mywd.wd_type = WHODTYPE_STATUS;
	for (np = neighbors; np != NULL; np = np->n_next)
		(void) sendto(s, (char *)&mywd, cc, 0,
			np->n_addr, np->n_addrlen);
	(void) alarm(60);
}

getkmem()
{
	struct nlist *nlp;

	signal(SIGHUP, getkmem);
	if (kmemf >= 0)
		(void) close(kmemf);
loop:
	for (nlp = &nl[sizeof (nl) / sizeof (nl[0])]; --nlp >= nl; ) {
		nlp->n_value = 0;
		nlp->n_type = 0;
	}
	nlist("/vmunix", nl);
	if (nl[0].n_value == 0) {
		fprintf(stderr, "/vmunix namelist botch\n");
		sleep(300);
		goto loop;
	}
	kmemf = open("/dev/kmem", 0);
	if (kmemf < 0) {
		perror("/dev/kmem");
		sleep(300);
		goto loop;
	}
	(void) lseek(kmemf, (long)nl[NL_BOOTTIME].n_value, 0);
	(void) read(kmemf, (char *)&mywd.wd_boottime, sizeof (mywd.wd_boottime));
}

/*
 * Figure out device configuration and select
 * networks which deserve status information.
 */
configure(s)
	int s;
{
	char buf[BUFSIZ];
	struct ifconf ifc;
	struct ifreq ifreq, *ifr;
	int n;
	struct sockaddr_in *sin;
	register struct neighbor *np;

	ifc.ifc_len = sizeof (buf);
	ifc.ifc_buf = buf;
	if (ioctl(s, SIOCGIFCONF, (char *)&ifc) < 0) {
		perror("rwhod: ioctl (get interface configuration)");
		return (0);
	}
	ifr = ifc.ifc_req;
	for (n = ifc.ifc_len / sizeof (struct ifreq); n > 0; n--, ifr++) {
		for (np = neighbors; np != NULL; np = np->n_next)
			if (np->n_name &&
			    strcmp(ifr->ifr_name, np->n_name) == 0)
				break;
		if (np != NULL)
			continue;
		ifreq = *ifr;
		np = (struct neighbor *)malloc(sizeof (*np));
		if (np == NULL)
			continue;
		np->n_name = malloc(strlen(ifr->ifr_name) + 1);
		if (np->n_name == NULL) {
			free((char *)np);
			continue;
		}
		strcpy(np->n_name, ifr->ifr_name);
		np->n_addrlen = sizeof (ifr->ifr_addr);
		np->n_addr = malloc(np->n_addrlen);
		if (np->n_addr == NULL) {
			free(np->n_name);
			free((char *)np);
			continue;
		}
		bcopy((char *)&ifr->ifr_addr, np->n_addr, np->n_addrlen);
		if (ioctl(s, SIOCGIFFLAGS, (char *)&ifreq) < 0) {
			perror("rwhod: ioctl (get interface flags)");
			free((char *)np);
			continue;
		}
		if ((ifreq.ifr_flags & (IFF_BROADCAST|IFF_POINTOPOINT)) == 0) {
			free((char *)np);
			continue;
		}
		np->n_flags = ifreq.ifr_flags;
		if (np->n_flags & IFF_POINTOPOINT) {
			if (ioctl(s, SIOCGIFDSTADDR, (char *)&ifreq) < 0) {
				perror("rwhod: ioctl (get dstaddr)");
				free((char *)np);
				continue;
			}
			/* we assume addresses are all the same size */
			bcopy((char *)&ifreq.ifr_dstaddr,
			  np->n_addr, np->n_addrlen);
		}
		if (np->n_flags & IFF_BROADCAST) {
			/* we assume addresses are all the same size */
			sin = (struct sockaddr_in *)np->n_addr;
			sin->sin_addr =
			  inet_makeaddr(inet_netof(sin->sin_addr), INADDR_ANY);
		}
		/* gag, wish we could get rid of Internet dependencies */
		sin = (struct sockaddr_in *)np->n_addr;
		sin->sin_port = sp->s_port;
		np->n_next = neighbors;
		neighbors = np;
	}
	return (1);
}
