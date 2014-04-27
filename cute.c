#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#define RCVBUF 30000000	//8Mbps ~ 1MB/s ~ 30 seconds of such strem
#define LIFT -10	//Niceness addition: -20 max priority

int udp_client(const char*, const char*, struct sockaddr**, socklen_t*);
int mcast_join(int, const struct sockaddr*, socklen_t, const char*, u_int);
void *file_move(void*);

int	tflag, sflag = SIGHUP;
char	filename[64];

void
slicenow(int arg)
{
	sflag = arg;
	return;	//recv() interrupted herre
}

int
main(int argc, char **argv)
{
	int	sockfd, fd = 5;	//random fd > stderr
const	int	on = 1, op = RCVBUF;
struct	sockaddr	*sa;
	socklen_t	salen;
struct	sigaction	sig;
struct	tm	tm;
	char	fname[64] = {0};
	char	*buf;
	time_t	t;
	ssize_t	n, i, cnt;
	pthread_t	pid;

	if (argc < 4) {
		fputs("\nUsage: cute <MCast GRP> <UDP port> <dump location> [if name]\n", stderr);
		exit(1);
	}
	//file is created and grown in current working folder
	printf("\nUse SIGHUP to start over or SIGTERM to slice the record to %s\n", argv[3]);

	sockfd = udp_client(argv[1], argv[2], &sa, &salen); //creates socket and fills sockaddr
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
		perror("setsockopt reuseaddr");
		exit(1);
	}
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &op, sizeof(op)) < 0) {
		perror("setsockopt rcvbuf");
		exit(1);
	}
	if (bind(sockfd, sa, salen) < 0) {
		perror("bind");
		exit(1);
	}
	bzero(&sig, sizeof(sig));
	sig.sa_handler = slicenow;
	if (sigaction(SIGTERM, &sig, NULL) < 0) {
		perror("sigaction term");
		exit(1);
	}
	if (sigaction(SIGHUP, &sig, NULL) < 0) {
                perror("sigaction hup");
                exit(1);
        }
	if (sigaction(SIGINT, &sig, NULL) < 0) {
		perror("sigaction int");
		exit(1);
	}
	if (mcast_join(sockfd, sa, salen, (argc == 5) ? argv[4] : NULL, 0) < 0) {
		perror("mcast_join");
		//exit(1);
	}
	if ( (buf = malloc(RCVBUF / 2)) == NULL) {	// buffer for recv()
		fputs("malloc error", stderr);
		exit(1);
	}
	printf("Niceness set: %d\n\n", nice(LIFT));
	for (; ;) {
		if (sflag != 0) {
			if (sflag == SIGINT) {
				//unlink(fname);
				puts("");
				exit(0);
			}
			time(&t);
			if (gmtime_r(&t, &tm) == NULL) {
				fputs("gmtime_r error", stderr);
				exit(1);
			}
			strcpy(filename, fname);
			snprintf(fname, sizeof(fname), "%04d%02d%02d%02d%02d%02d.ts",\
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,\
				tm.tm_min, tm.tm_sec);
			puts(fname);
			close(fd);	//first time error is ignored
			if ( (fd = creat(fname, 0644)) < 0) {
				perror("create");
				exit(1);
			}
			if (sflag == SIGHUP) unlink(filename);
			else if (sflag == SIGTERM)
				if ( (errno = pthread_create(&pid, NULL, file_move, argv[3])) != 0)
					perror("pthread_create");
			sflag = 0;
		}
		//MSG_WAITALL looks irrelevant for SOCK_DGRAM
		if ( (n = recv(sockfd, buf, RCVBUF / 2, MSG_WAITALL)) < 0) {
			if (errno == EINTR) continue;
			else {
				perror("recv()");
				exit(1);
			}
		}
		if (n > 0) {
			i = 0;
			while (i < n) if ( (cnt = write(fd, buf + i, n - i)) < 0) {
				if (errno == EINTR) continue;
				else {
					perror("write()");
					exit(1);
				}
			} else i += cnt;
		} else fputs("paradox: recv() returned 0", stderr);
	}
	return 0;
}

int
udp_client(const char *host, const char *serv, struct sockaddr **saptr, socklen_t *lenp)
{
        int		sockfd, n;
struct	addrinfo	hints, *res, *ressave;

        bzero(&hints, sizeof(struct addrinfo));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        if ( (n = getaddrinfo(host, serv, &hints, &res)) != 0) {
                fprintf(stderr, "udp_client error for %s, %s: %s", host, serv, gai_strerror(n));
		exit(1);
	}
        ressave = res;

        do {
                sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (sockfd >= 0) break;          /* success */
        } while ( (res = res->ai_next) != NULL);

        if (res == NULL) {       /* errno set from final socket() */
                fprintf(stderr, "udp_client error for %s, %s", host, serv);
		exit(1);
	}
        if ( (*saptr = malloc(res->ai_addrlen)) == NULL) {
		fputs("malloc error", stderr);
		exit(1);
	}
        memcpy(*saptr, res->ai_addr, res->ai_addrlen);
        *lenp = res->ai_addrlen;

        freeaddrinfo(ressave);

        return(sockfd);
}

int
family_to_level(int family)
{
        switch (family) {
        case AF_INET:
                return IPPROTO_IP;
#ifdef  IPV6
        case AF_INET6:
                return IPPROTO_IPV6;
#endif
        default:
                return -1;
        }
}

int
mcast_join(int sockfd, const struct sockaddr *grp, socklen_t grplen, const char *ifname, u_int ifindex)
{
#ifdef MCAST_JOIN_GROUP
struct	group_req	req;

        if (ifindex > 0) {
                req.gr_interface = ifindex;
        } else if (ifname != NULL) {
                if ( (req.gr_interface = if_nametoindex(ifname)) == 0) {
                        errno = ENXIO;  /* i/f name not found */
                        return -1;
                }
        } else req.gr_interface = 0;
        if (grplen > sizeof(req.gr_group)) {
                errno = EINVAL;
                return -1;
        }
        memcpy(&req.gr_group, grp, grplen);
        return setsockopt(sockfd, family_to_level(grp->sa_family), MCAST_JOIN_GROUP, &req, sizeof(req));
#else
/* end mcast_join1 */

/* include mcast_join2 */
        switch (grp->sa_family) {
        case AF_INET:
        struct	ip_mreqn	mreq;
        struct	ifreq		ifreq;

                memcpy(&mreq.imr_multiaddr,\
                       &((const struct sockaddr_in *) grp)->sin_addr,\
                       sizeof(struct in_addr));

                if (ifindex > 0) {
                        if (if_indextoname(ifindex, ifreq.ifr_name) == NULL) {
                                errno = ENXIO;  /* i/f index not found */
                                return -1;
                        }
                        goto doioctl;
                } else if (ifname != NULL) {
                        strncpy(ifreq.ifr_name, ifname, IFNAMSIZ);
		doioctl:
                        if (ioctl(sockfd, SIOCGIFADDR, &ifreq) < 0) return -1;
                        memcpy(&mreq.imr_interface,\
                               &((struct sockaddr_in *) &ifreq.ifr_addr)->sin_addr,\
                               sizeof(struct in_addr));
                } else mreq.imr_interface.s_addr = htonl(INADDR_ANY);

                return setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
/* end mcast_join2 */

/* include mcast_join3 */
#ifdef  IPV6
#ifndef IPV6_JOIN_GROUP         /* APIv0 compatibility */
#define IPV6_JOIN_GROUP         IPV6_ADD_MEMBERSHIP
#endif
        case AF_INET6:
        struct	ipv6_mreq	mreq6;

                memcpy(&mreq6.ipv6mr_multiaddr,\
                       &((const struct sockaddr_in6 *) grp)->sin6_addr,\
                       sizeof(struct in6_addr));

                if (ifindex > 0) mreq6.ipv6mr_interface = ifindex;
                else if (ifname != NULL) {
                        if ( (mreq6.ipv6mr_interface = if_nametoindex(ifname)) == 0) {
                                errno = ENXIO;  /* i/f name not found */
                                return -1;
                        }
                } else mreq6.ipv6mr_interface = 0;

                return setsockopt(sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6));
#endif

        default:
                errno = EAFNOSUPPORT;
                return -1;
        }
#endif
}
/* end mcast_join3 */

void*
file_move(void *arg)
{
	int	ret;
	char	buf[512];

	pthread_detach(pthread_self());

	if (tflag == 0) tflag = 1;
	else fputs("WARNING: last file moving operation hangs!", stderr);

	snprintf(buf, sizeof(buf), "mv -f %s %s", filename, (char*) arg);
	if ( (ret = system(buf)) < 0) fputs("fork @moving file fails", stderr);
	if (WEXITSTATUS(ret) == 127) fputs("shell couldn't execute", stderr);
	if (WEXITSTATUS(ret) != 0) fprintf(stderr, "file moving returned %d\n", WEXITSTATUS(ret));
	tflag = 0;
	return NULL;
}
