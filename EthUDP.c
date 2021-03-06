/* EthUDP: used to create tunnel over ipv4/ipv6 network
	  by james@ustc.edu.cn 2009.04.02
*/

// kernel use auxdata to send vlan tag, we use auxdata to reconstructe vlan header
#define HAVE_PACKET_AUXDATA 1

// enable OPENSSL encrypt/decrypt support
#define ENABLE_OPENSSL 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/if_tun.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdarg.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAXLEN 			2048
#define MAX_PACKET_SIZE		2048
#define MAXFD   		64

#define STATUS_BAD 	0
#define STATUS_OK  	1
#define MASTER 		0
#define SLAVE 		1

#define MODEE	0		// raw ether bridge mode
#define MODEI	1		// tap interface mode
#define MODEB	2		// bridge mode

#define XOR 	1

//#define DEBUGPINGPONG 1
//#define DEBUGSSL      1

#ifdef ENABLE_OPENSSL
#include <openssl/evp.h>
#define AES_128 2
#define AES_192 3
#define AES_256 3
#else
#define EVP_MAX_BLOCK_LENGTH 0
#endif

#define max(a,b)        ((a) > (b) ? (a) : (b))

#ifdef HAVE_PACKET_AUXDATA
#define VLAN_TAG_LEN   4
struct vlan_tag {
	u_int16_t vlan_tpid;	/* ETH_P_8021Q */
	u_int16_t vlan_tci;	/* VLAN TCI */
};
#endif

struct _EtherHeader {
	uint16_t destMAC1;
	uint32_t destMAC2;
	uint16_t srcMAC1;
	uint32_t srcMAC2;
	uint32_t VLANTag;
	uint16_t type;
	int32_t payload;
} __attribute__ ((packed));

typedef struct _EtherHeader EtherPacket;

int daemon_proc;		/* set nonzero by daemon_init() */
int debug = 0;

int mode = -1;			// 0 eth bridge, 1 interface, 2 bridge
int master_slave = 0;
int read_only = 0, write_only = 0;
int fixmss = 0;
int nopromisc = 0;
int loopback_check = 0;

int32_t ifindex;

char mypassword[MAXLEN];
int enc_algorithm;
unsigned char enc_key[MAXLEN];
unsigned char enc_iv[EVP_MAX_IV_LENGTH];
int enc_key_len = 0;

int fdudp[2], fdraw;
int transfamily[2];
int nat[2];

volatile struct sockaddr_storage remote_addr[2];
volatile u_int32_t myticket, last_pong[2];	// myticket inc 1 every 1 second after start
volatile u_int32_t ping_send[2], ping_recv[2], pong_send[2], pong_recv[2];
volatile int master_status = STATUS_OK;
volatile int slave_status = STATUS_OK;
volatile int current_remote = MASTER;
volatile int got_signal = 1;

void sig_handler(int signo)
{
	got_signal = 1;
}

void err_doit(int errnoflag, int level, const char *fmt, va_list ap)
{
	int errno_save, n;
	char buf[MAXLEN];

	errno_save = errno;	/* value caller might want printed */
	vsnprintf(buf, sizeof(buf), fmt, ap);	/* this is safe */
	n = strlen(buf);
	if (errnoflag)
		snprintf(buf + n, sizeof(buf) - n, ": %s", strerror(errno_save));
	strcat(buf, "\n");

	if (daemon_proc) {
		syslog(level, buf);
	} else {
		fflush(stdout);	/* in case stdout and stderr are the same */
		fputs(buf, stderr);
		fflush(stderr);
	}
	return;
}

void err_msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(0, LOG_INFO, fmt, ap);
	va_end(ap);
	return;
}

void Debug(const char *fmt, ...)
{
	va_list ap;
	if (debug) {
		va_start(ap, fmt);
		err_doit(0, LOG_INFO, fmt, ap);
		va_end(ap);
	}
	return;
}

void err_quit(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(0, LOG_ERR, fmt, ap);
	va_end(ap);
	exit(1);
}

void err_sys(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(1, LOG_ERR, fmt, ap);
	va_end(ap);
	exit(1);
}

void daemon_init(const char *pname, int facility)
{
	int i;
	pid_t pid;
	if ((pid = fork()) != 0)
		exit(0);	/* parent terminates */

	/* 41st child continues */
	setsid();		/* become session leader */

	signal(SIGHUP, SIG_IGN);
	if ((pid = fork()) != 0)
		exit(0);	/* 1st child terminates */

	/* 42nd child continues */
	daemon_proc = 1;	/* for our err_XXX() functions */

	umask(0);		/* clear our file mode creation mask */

	for (i = 0; i < MAXFD; i++)
		close(i);

	openlog(pname, LOG_PID, facility);
}

int udp_server(const char *host, const char *serv, socklen_t * addrlenp, int index)
{
	int sockfd, n;
	int on = 1;
	struct addrinfo hints, *res, *ressave;

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if ((n = getaddrinfo(host, serv, &hints, &res)) != 0)
		err_quit("udp_server error for %s, %s", host, serv);
	ressave = res;

	do {
		transfamily[index] = res->ai_family;
		sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (sockfd < 0)
			continue;	/* error, try next one */
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, 1);
		if (bind(sockfd, res->ai_addr, res->ai_addrlen) == 0)
			break;	/* success */
		close(sockfd);	/* bind error, close and try next one */
	}
	while ((res = res->ai_next) != NULL);

	if (res == NULL)	/* errno from final socket() or bind() */
		err_sys("udp_server error for %s, %s", host, serv);

	if (addrlenp)
		*addrlenp = res->ai_addrlen;	/* return size of protocol address */

	freeaddrinfo(ressave);

	return (sockfd);
}

int udp_xconnect(char *lhost, char *lserv, char *rhost, char *rserv, int index)
{
	int sockfd, n;
	struct addrinfo hints, *res, *ressave;

	sockfd = udp_server(lhost, lserv, NULL, index);

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if ((n = getaddrinfo(rhost, rserv, &hints, &res)) != 0)
		err_quit("udp_xconnect error for %s, %s", rhost, rserv);
	ressave = res;

	if (((struct sockaddr_in *)res->ai_addr)->sin_port == 0) {
		Debug("port==0, nat = 1");
		nat[index] = 1;
		memcpy((void *)&(remote_addr[index]), res->ai_addr, res->ai_addrlen);
		return sockfd;
	}

	do {
		if (connect(sockfd, res->ai_addr, res->ai_addrlen) == 0) {
			memcpy((void *)&(remote_addr[index]), res->ai_addr, res->ai_addrlen);
			break;	/* success */
		}
	}
	while ((res = res->ai_next) != NULL);

	if (res == NULL)	/* errno set from final connect() */
		err_sys("udp_xconnect error for %s, %s", rhost, rserv);

	freeaddrinfo(ressave);

	n = 40 * 1024 * 1024;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
	if (debug) {
		socklen_t ln;
		if (getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &n, &ln) == 0) {
			Debug("UDP socket RCVBUF setting to %d\n", n);
		}
	}

	return (sockfd);
}

/**
 * Open a rawsocket for the network interface
 */
int32_t open_socket(char *ifname, int32_t * rifindex)
{
	unsigned char buf[MAX_PACKET_SIZE];
	int32_t ifindex;
	struct ifreq ifr;
	struct sockaddr_ll sll;
	int n;

	int32_t fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd == -1)
		err_sys("socket %s - ", ifname);

	// get interface index
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1)
		err_sys("SIOCGIFINDEX %s - ", ifname);
	ifindex = ifr.ifr_ifindex;
	*rifindex = ifindex;

	if (!nopromisc) {	// set promiscuous mode
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
		ioctl(fd, SIOCGIFFLAGS, &ifr);
		ifr.ifr_flags |= IFF_PROMISC;
		ioctl(fd, SIOCSIFFLAGS, &ifr);
	}

	memset(&sll, 0xff, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) == -1)
		err_sys("bind %s - ", ifname);

	/* flush all received packets. 
	 *
	 * raw-socket receives packets from all interfaces
	 * when the socket is not bound to an interface
	 */
	int32_t i;
	do {
		fd_set fds;
		struct timeval t;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		memset(&t, 0, sizeof(t));
		i = select(FD_SETSIZE, &fds, NULL, NULL, &t);
		if (i > 0) {
			recv(fd, buf, i, 0);
		};

		Debug("interface %d flushed", ifindex);
	}
	while (i);

	/* Enable auxillary data if supported and reserve room for
	 * reconstructing VLAN headers. */
#ifdef HAVE_PACKET_AUXDATA
	int val = 1;
	if (setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &val, sizeof(val)) == -1 && errno != ENOPROTOOPT) {
		err_sys("setsockopt(packet_auxdata): %s", strerror(errno));
	}
#endif				/* HAVE_PACKET_AUXDATA */

	Debug("%s opened (fd=%d interface=%d)", ifname, fd, ifindex);

	n = 40 * 1024 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
	if (debug) {
		socklen_t ln;
		if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, &ln) == 0) {
			Debug("RAW socket RCVBUF setting to %d\n", n);
		}
	}

	return fd;
}

int xor_encrypt(u_int8_t * buf, int n, u_int8_t * nbuf)
{
	int i;
	for (i = 0; i < n; i++)
		nbuf[i] = buf[i] ^ enc_key[i % enc_key_len];
	return n;
}

#ifdef ENABLE_OPENSSL
int openssl_encrypt(u_int8_t * buf, int len, u_int8_t * nbuf)
{
	EVP_CIPHER_CTX ctx;
	int outlen1, outlen2;
#ifdef DEBUGSSL
	Debug("aes encrypt len=%d", len);
#endif
	EVP_CIPHER_CTX_init(&ctx);
	if (enc_algorithm == AES_128)
		EVP_EncryptInit(&ctx, EVP_aes_128_cbc(), enc_key, enc_iv);
	else if (enc_algorithm == AES_192)
		EVP_EncryptInit(&ctx, EVP_aes_192_cbc(), enc_key, enc_iv);
	else if (enc_algorithm == AES_256)
		EVP_EncryptInit(&ctx, EVP_aes_256_cbc(), enc_key, enc_iv);
	EVP_EncryptUpdate(&ctx, nbuf, &outlen1, buf, len);
	EVP_EncryptFinal(&ctx, nbuf + outlen1, &outlen2);
	len = outlen1 + outlen2;

#ifdef DEBUGSSL
	Debug("after aes encrypt len=%d", len);
#endif
	EVP_CIPHER_CTX_cleanup(&ctx);
	return len;
}

int openssl_decrypt(u_int8_t * buf, int len, u_int8_t * nbuf)
{

	EVP_CIPHER_CTX ctx;
	int outlen1, outlen2;
#ifdef DEBUGSSL
	Debug("aes decrypt len=%d", len);
#endif

	EVP_CIPHER_CTX_init(&ctx);
	if (enc_algorithm == AES_128)
		EVP_DecryptInit(&ctx, EVP_aes_128_cbc(), enc_key, enc_iv);
	else if (enc_algorithm == AES_192)
		EVP_DecryptInit(&ctx, EVP_aes_192_cbc(), enc_key, enc_iv);
	else if (enc_algorithm == AES_256)
		EVP_DecryptInit(&ctx, EVP_aes_256_cbc(), enc_key, enc_iv);
	if (EVP_DecryptUpdate(&ctx, nbuf, &outlen1, buf, len) != 1 || EVP_DecryptFinal(&ctx, nbuf + outlen1, &outlen2) != 1)
		len = 0;
	else
		len = outlen1 + outlen2;
#ifdef DEBUGSSL
	Debug("after aes decrypt len=%d", len);
#endif
	EVP_CIPHER_CTX_cleanup(&ctx);
	return len;
}
#endif

int do_encrypt(u_int8_t * buf, int len, u_int8_t * nbuf)
{
	if (enc_key_len <= 0)
		return 0;	// you should not call me!
	if (enc_algorithm == XOR)
		return xor_encrypt(buf, len, nbuf);
#ifdef ENABLE_OPENSSL
	if ((enc_algorithm == AES_128)
	    || (enc_algorithm == AES_192)
	    || (enc_algorithm == AES_256))
		return openssl_encrypt(buf, len, nbuf);
#endif
	return 0;
}

int do_decrypt(u_int8_t * buf, int len, u_int8_t * nbuf)
{
	if (enc_key_len <= 0)
		return 0;	// you should not call me!
	if (enc_algorithm == XOR)
		return xor_encrypt(buf, len, nbuf);
#ifdef ENABLE_OPENSSL
	if ((enc_algorithm == AES_128)
	    || (enc_algorithm == AES_192)
	    || (enc_algorithm == AES_256))
		return openssl_decrypt(buf, len, nbuf);
#endif
	return 0;
}

char *stamp(void)
{
	static char st_buf[200];
	struct timeval tv;
	struct timezone tz;
	struct tm *tm;

	gettimeofday(&tv, &tz);
	tm = localtime(&tv.tv_sec);

	snprintf(st_buf, 200, "%02d%02d %02d:%02d:%02d.%06ld", tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec);
	return st_buf;
}

void printPacket(EtherPacket * packet, ssize_t packetSize, char *message)
{
	printf("%s ", stamp());

	if ((ntohl(packet->VLANTag) >> 16) == 0x8100)	// VLAN tag
		printf("%s #%04x (VLAN %d) from %04x%08x to %04x%08x, len=%d\n",
		       message, ntohs(packet->type),
		       ntohl(packet->VLANTag) & 0xFFF, ntohs(packet->srcMAC1),
		       ntohl(packet->srcMAC2), ntohs(packet->destMAC1), ntohl(packet->destMAC2), (int)packetSize);
	else
		printf("%s #%04x (no VLAN) from %04x%08x to %04x%08x, len=%d\n",
		       message, ntohl(packet->VLANTag) >> 16,
		       ntohs(packet->srcMAC1), ntohl(packet->srcMAC2), ntohs(packet->destMAC1), ntohl(packet->destMAC2), (int)packetSize);
	fflush(stdout);
}

// function from http://www.bloof.de/tcp_checksumming, thanks to crunsh
u_int16_t tcp_sum_calc(u_int16_t len_tcp, u_int16_t src_addr[], u_int16_t dest_addr[], u_int16_t buff[])
{
	u_int16_t prot_tcp = 6;
	u_int32_t sum = 0;
	int nleft = len_tcp;
	u_int16_t *w = buff;

	/* calculate the checksum for the tcp header and payload */
	while (nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}

	/* if nleft is 1 there ist still on byte left. We add a padding byte (0xFF) to build a 16bit word */
	if (nleft > 0)
		sum += *w & ntohs(0xFF00);	/* Thanks to Dalton */

	/* add the pseudo header */
	sum += src_addr[0];
	sum += src_addr[1];
	sum += dest_addr[0];
	sum += dest_addr[1];
	sum += htons(len_tcp);
	sum += htons(prot_tcp);

	// keep only the last 16 bits of the 32 bit calculated sum and add the carries
	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += (sum >> 16);

	// Take the one's complement of sum
	sum = ~sum;

	return ((u_int16_t) sum);
}

u_int16_t tcp_sum_calc_v6(u_int16_t len_tcp, u_int16_t src_addr[], u_int16_t dest_addr[], u_int16_t buff[])
{
	u_int16_t prot_tcp = 6;
	u_int32_t sum = 0;
	int nleft = len_tcp;
	u_int16_t *w = buff;

	/* calculate the checksum for the tcp header and payload */
	while (nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}

	/* if nleft is 1 there ist still on byte left. We add a padding byte (0xFF) to build a 16bit word */
	if (nleft > 0)
		sum += *w & ntohs(0xFF00);	/* Thanks to Dalton */

	/* add the pseudo header */
	int i;
	for (i = 0; i < 8; i++)
		sum = sum + src_addr[i] + dest_addr[i];

	sum += htons(len_tcp);	// why using 32bit len_tcp
	sum += htons(prot_tcp);

	// keep only the last 16 bits of the 32 bit calculated sum and add the carries
	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += (sum >> 16);

	// Take the one's complement of sum
	sum = ~sum;

	return ((u_int16_t) sum);
}

static unsigned int optlen(const u_int8_t * opt, unsigned int offset)
{
	/* Beware zero-length options: make finite progress */
	if (opt[offset] <= TCPOPT_NOP || opt[offset + 1] == 0)
		return 1;
	else
		return opt[offset + 1];
}

void fix_mss(u_int8_t * buf, int len, int index)
{
	u_int8_t *packet;
	int i;
	int VLANdot1Q = 0;

	if (len < 54)
		return;
	packet = buf + 12;	// skip ethernet dst & src addr
	len -= 12;

	if ((packet[0] == 0x81) && (packet[1] == 0x00)) {	// skip 802.1Q tag 0x8100
		packet += 4;
		len -= 4;
		VLANdot1Q = 1;
	}
	if ((packet[0] == 0x08) && (packet[1] == 0x00)) {	// IPv4 packet 0x0800
		packet += 2;
		len -= 2;

		struct iphdr *ip = (struct iphdr *)packet;
		if (ip->version != 4)
			return;	// check ipv4
		if (ntohs(ip->frag_off) & 0x1fff)
			return;	// not the first fragment
		if (ip->protocol != IPPROTO_TCP)
			return;	// not tcp packet
		if (ntohs(ip->tot_len) > len)
			return;	// tot_len should < len 

		struct tcphdr *tcph = (struct tcphdr *)(packet + ip->ihl * 4);
		if (!tcph->syn)
			return;

		Debug("fixmss ipv4 tcp syn");

		u_int8_t *opt = (u_int8_t *) tcph;
		for (i = sizeof(struct tcphdr); i < tcph->doff * 4; i += optlen(opt, i)) {
			if (opt[i] == 2 && tcph->doff * 4 - i >= 4 &&	// TCP_MSS
			    opt[i + 1] == 4) {
				u_int16_t newmss = 0, oldmss;
				if (transfamily[index] == PF_INET)
					newmss = 1418;
				else if (transfamily[index] == PF_INET6)
					newmss = 1398;
				if (VLANdot1Q)
					newmss -= 4;
				oldmss = (opt[i + 2] << 8) | opt[i + 3];
				/* Never increase MSS, even when setting it, as
				 * doing so results in problems for hosts that rely
				 * on MSS being set correctly.
				 */
				if (oldmss <= newmss)
					return;
				Debug("change inner v4 tcp mss from %d to %d", oldmss, newmss);
				opt[i + 2] = (newmss & 0xff00) >> 8;
				opt[i + 3] = newmss & 0x00ff;

				tcph->check = 0;	/* Checksum field has to be set to 0 before checksumming */
				tcph->check = (u_int16_t)
				    tcp_sum_calc((u_int16_t)
						 (ntohs(ip->tot_len) - ip->ihl * 4), (u_int16_t *) & ip->saddr, (u_int16_t *) & ip->daddr, (u_int16_t *) tcph);
				return;
			}
		}
		return;
	} else if ((packet[0] == 0x86) && (packet[1] == 0xdd)) {	// IPv6 packet, 0x86dd
		packet += 2;
		len -= 2;

		struct ip6_hdr *ip6 = (struct ip6_hdr *)packet;
		if ((ip6->ip6_vfc & 0xf0) != 0x60)
			return;	// check ipv6
		if (ip6->ip6_nxt != IPPROTO_TCP)
			return;	// not tcp packet
		if (ntohs(ip6->ip6_plen) > len)
			return;	// tot_len should < len 

		struct tcphdr *tcph = (struct tcphdr *)(packet + 40);
		if (!tcph->syn)
			return;
		Debug("fixmss ipv6 tcp syn");
		u_int8_t *opt = (u_int8_t *) tcph;
		for (i = sizeof(struct tcphdr); i < tcph->doff * 4; i += optlen(opt, i)) {
			if (opt[i] == 2 && tcph->doff * 4 - i >= 4 &&	// TCP_MSS
			    opt[i + 1] == 4) {
				u_int16_t newmss = 0, oldmss;
				if (transfamily[index] == PF_INET)
					newmss = 1398;
				else if (transfamily[index] == PF_INET6)
					newmss = 1378;
				if (VLANdot1Q)
					newmss -= 4;
				oldmss = (opt[i + 2] << 8) | opt[i + 3];
				/* Never increase MSS, even when setting it, as
				 * doing so results in problems for hosts that rely
				 * on MSS being set correctly.
				 */
				if (oldmss <= newmss)
					return;
				Debug("change inner v6 tcp mss from %d to %d", oldmss, newmss);

				opt[i + 2] = (newmss & 0xff00) >> 8;
				opt[i + 3] = newmss & 0x00ff;

				tcph->check = 0;	/* Checksum field has to be set to 0 before checksumming */
				tcph->check = (u_int16_t) tcp_sum_calc_v6((u_int16_t)
									  ntohs(ip6->ip6_plen),
									  (u_int16_t *) & ip6->ip6_src, (u_int16_t *) & ip6->ip6_dst, (u_int16_t *)
									  tcph);
				return;
			}
		}
		return;
	} else
		return;		// not IP packet
}

/*  return 1 if packet will cause loopback, DSTIP or SRCIP == remote address && PROTO == UDP
*/
int do_loopback_check(u_int8_t * buf, int len)
{
	u_int8_t *packet;

	if (len < 14)		// MAC(12)+Proto(2)+IP(20)
		return 0;
	packet = buf + 12;	// skip ethernet dst & src addr
	len -= 12;

	if ((packet[0] == 0x81) && (packet[1] == 0x00)) {	// skip 802.1Q tag 0x8100
		packet += 4;
		len -= 4;
	}
	if ((packet[0] == 0x08) && (packet[1] == 0x00)) {	// IPv4 packet 0x0800
		packet += 2;
		len -= 2;

		if (len < 20)	// IP header len is 20
			return 0;

		struct iphdr *ip = (struct iphdr *)packet;
		if (ip->version != 4)
			return 0;	// not ipv4
		if (ip->protocol != IPPROTO_UDP)
			return 0;	// not udp packet

		struct sockaddr_in *r = (struct sockaddr_in *)(&remote_addr[MASTER]);
		if (ip->saddr == r->sin_addr.s_addr) {
			Debug("master remote ipaddr == src addr, loopback");
			return 1;
		} else if (ip->daddr == r->sin_addr.s_addr) {
			Debug("master remote ipaddr == dst addr, loopback");
			return 1;
		}
		if (master_slave) {
			struct sockaddr_in *r = (struct sockaddr_in *)(&remote_addr[SLAVE]);
			if (ip->saddr == r->sin_addr.s_addr) {
				Debug("slave remote ipaddr == src addr, loopback");
				return 1;
			} else if (ip->daddr == r->sin_addr.s_addr) {
				Debug("slave remote ipaddr == dst addr, loopback");
				return 1;
			}
		}
	} else if ((packet[0] == 0x86) && (packet[1] == 0xdd)) {	// IPv6 packet, 0x86dd
		packet += 2;
		len -= 2;

		if (len < 40)	// IPv6 header len is 40
			return 0;

		struct ip6_hdr *ip6 = (struct ip6_hdr *)packet;
		if ((ip6->ip6_vfc & 0xf0) != 0x60)
			return 0;	// not ipv6
		if (ip6->ip6_nxt != IPPROTO_UDP)
			return 0;	// not udp packet

		struct sockaddr_in6 *r = (struct sockaddr_in6 *)&remote_addr[MASTER];
		if (memcmp(&ip6->ip6_src, &r->sin6_addr, 16) == 0) {
			Debug("master remote ip6_addr == src ip6 addr, loopback");
			return 1;
		} else if (memcmp(&ip6->ip6_dst, &r->sin6_addr, 16) == 0) {
			Debug("master remote ip6_addr == dst ip6 addr, loopback");
			return 1;
		}
		if (master_slave) {
			struct sockaddr_in6 *r = (struct sockaddr_in6 *)&remote_addr[SLAVE];
			if (memcmp(&ip6->ip6_src, &r->sin6_addr, 16) == 0) {
				Debug("slave remote ip6_addr == src ip6 addr, loopback");
				return 1;
			} else if (memcmp(&ip6->ip6_dst, &r->sin6_addr, 16) == 0) {
				Debug("slave remote ip6_addr == dst ip6 addr, loopback");
				return 1;
			}
		}
	}
	return 0;
}

void send_udp_to_remote(u_int8_t * buf, int len, int index)	// send udp packet to remote 
{
	if (nat[index]) {
		char rip[200];
		if (remote_addr[index].ss_family == AF_INET) {
			struct sockaddr_in *r = (struct sockaddr_in *)(&remote_addr[index]);
			Debug("nat mode: send len %d to %s:%d", len, inet_ntop(r->sin_family, (void *)&r->sin_addr, rip, 200), ntohs(r->sin_port));
			if (r->sin_port)
				sendto(fdudp[index], buf, len, 0, (struct sockaddr *)&remote_addr[index], sizeof(struct sockaddr_storage));
		} else if (remote_addr[index].ss_family == AF_INET6) {
			struct sockaddr_in6 *r = (struct sockaddr_in6 *)&remote_addr[index];
			Debug("nat mode: send len %d to [%s]:%d", len, inet_ntop(r->sin6_family, (void *)&r->sin6_addr, rip, 200), ntohs(r->sin6_port));
			if (r->sin6_port)
				sendto(fdudp[index], buf, len, 0, (struct sockaddr *)&remote_addr[index], sizeof(struct sockaddr_storage));
		}
	} else
		write(fdudp[index], buf, len);
}

void send_keepalive_to_udp(void)	// send keepalive to remote  
{
	u_int8_t buf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
	u_int8_t nbuf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
	u_int8_t *pbuf;
	int len;
	static u_int32_t lasttm;
	while (1) {
		if (got_signal || (myticket >= lasttm + 3600)) {	// log ping/pong every hour
			err_msg("============= myticket=%lu, master_slave=%d, master_status=%d, slave_status=%d", (unsigned long)myticket,
				master_slave, master_status, slave_status);
			err_msg("master ping_send/pong_recv: %d/%d, ping_recv/pong_send: %d/%d",
				(unsigned long)ping_send[MASTER], (unsigned long)pong_recv[MASTER], (unsigned long)ping_recv[MASTER],
				(unsigned long)pong_send[MASTER]);
			err_msg(" slave ping_send/pong_recv: %d/%d, ping_recv/pong_send: %d/%d", (unsigned long)ping_send[SLAVE],
				(unsigned long)pong_recv[SLAVE], (unsigned long)ping_recv[SLAVE], (unsigned long)pong_send[SLAVE]);
			if (myticket >= lasttm + 3600) {
				ping_send[MASTER] = ping_send[SLAVE] = ping_recv[MASTER] = ping_recv[SLAVE] = 0;
				pong_send[MASTER] = pong_send[SLAVE] = pong_recv[MASTER] = pong_recv[SLAVE] = 0;
				lasttm = myticket;
			}
			got_signal = 0;
		}
		myticket++;
		if (mypassword[0]) {
			if (nat[MASTER] == 0) {
				len = snprintf((char *)buf, MAX_PACKET_SIZE, "PASSWORD:%s", mypassword);
				Debug("send password: %s", buf);
				len++;
				if (enc_key_len > 0) {
					len = do_encrypt((u_int8_t *) buf, len, nbuf);
					pbuf = nbuf;
				} else
					pbuf = buf;
				send_udp_to_remote(pbuf, len, MASTER);	// send to master
			}
			if (master_slave && (nat[SLAVE] == 0))
				send_udp_to_remote(pbuf, len, SLAVE);	// send to slave
		}
		memcpy(buf, "PING:PING:", 10);
		len = 10;
		if (enc_key_len > 0) {
			len = do_encrypt((u_int8_t *) buf, len, nbuf);
			pbuf = nbuf;
		} else
			pbuf = buf;
		send_udp_to_remote(pbuf, len, MASTER);	// send to master
		ping_send[MASTER]++;

		if (master_status == STATUS_OK) {	// now master is OK
			if (myticket > last_pong[MASTER] + 5) {	// master OK->BAD
				master_status = STATUS_BAD;
				if (master_slave)
					current_remote = SLAVE;	// switch to SLAVE
				err_msg("master OK-->BAD, current_remote is %d", current_remote);
			}
		} else {	// now master is BAD
			if (myticket < last_pong[MASTER] + 4) {	// master BAD->OK
				master_status = STATUS_OK;
				current_remote = MASTER;	// switch to MASTER
				err_msg("master BAD-->OK, current_remote is %d", current_remote);
			}
		}

		if (master_slave) {
			send_udp_to_remote(pbuf, len, SLAVE);	// send to slave
			ping_send[SLAVE]++;

			if (slave_status == STATUS_OK) {	// now slave is OK
				if (myticket > last_pong[SLAVE] + 5) {	// slave OK->BAD
					slave_status = STATUS_BAD;
					err_msg("slave OK-->BAD");
				}
			} else {	// now slave is BAD
				if (myticket < last_pong[SLAVE] + 4) {	// slave BAD->OK
					slave_status = STATUS_OK;
					err_msg("slave BAD-->OK");
				}
			}
		}
		sleep(1);
	}
}

void process_raw_to_udp(void)	// used by mode==0 & mode==1
{
	u_int8_t buf[MAX_PACKET_SIZE + VLAN_TAG_LEN + EVP_MAX_BLOCK_LENGTH];
	u_int8_t nbuf[MAX_PACKET_SIZE + VLAN_TAG_LEN + EVP_MAX_BLOCK_LENGTH];
	u_int8_t *pbuf;
	int len;
	int offset = 0;

	while (1) {		// read from eth rawsocket
		if (mode == MODEE) {
#ifdef HAVE_PACKET_AUXDATA
			struct sockaddr from;
			struct iovec iov;
			struct msghdr msg;
			struct cmsghdr *cmsg;
			union {
				struct cmsghdr cmsg;
				char buf[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
			} cmsg_buf;
			msg.msg_name = &from;
			msg.msg_namelen = sizeof(from);
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = &cmsg_buf;
			msg.msg_controllen = sizeof(cmsg_buf);
			msg.msg_flags = 0;

			offset = VLAN_TAG_LEN;
			iov.iov_len = MAX_PACKET_SIZE;
			iov.iov_base = buf + offset;
			len = recvmsg(fdraw, &msg, MSG_TRUNC);
			if (len <= 0)
				continue;
			for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
				struct tpacket_auxdata *aux;
				struct vlan_tag *tag;

				if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct tpacket_auxdata))
				    || cmsg->cmsg_level != SOL_PACKET || cmsg->cmsg_type != PACKET_AUXDATA)
					continue;

				aux = (struct tpacket_auxdata *)CMSG_DATA(cmsg);

#if defined(TP_STATUS_VLAN_VALID)
				if ((aux->tp_vlan_tci == 0)
				    && !(aux->tp_status & TP_STATUS_VLAN_VALID))
#else
				if (aux->tp_vlan_tci == 0)	/* this is ambigious but without the */
#endif
					continue;

				Debug("len=%d, iov_len=%d, ", len, (int)iov.iov_len);

				len = len > iov.iov_len ? iov.iov_len : len;
				if (len < 12)	// MAC_len * 2
					break;
				Debug("len=%d", len);

				memmove(buf, buf + VLAN_TAG_LEN, 12);
				offset = 0;

				/*
				 * Now insert the tag.
				 */
				tag = (struct vlan_tag *)(buf + 12);
				Debug("insert vlan id, recv len=%d", len);
				tag->vlan_tpid = 0x0081;
				tag->vlan_tci = htons(aux->tp_vlan_tci);

				/* Add the tag to the packet lengths.
				 */
				len += VLAN_TAG_LEN;
				break;
			}
#else
			len = recv(fdraw, buf, MAX_PACKET_SIZE, 0);
#endif
		} else if ((mode == MODEI) || (mode == MODEB))
			len = read(fdraw, buf, MAX_PACKET_SIZE);
		else
			return;

		if (len <= 0)
			continue;
		if (write_only)
			continue;	// write only

		if (loopback_check && do_loopback_check(buf + offset, len))
			continue;
		if (!read_only && fixmss)	// read only, no fix_mss
			fix_mss(buf + offset, len, current_remote);
		if (debug) {
			printPacket((EtherPacket *) (buf + offset), len, "from local  rawsocket:");
			if (offset)
				printf("offset=%d\n", offset);
		}

		if (enc_key_len > 0) {
			len = do_encrypt((u_int8_t *) buf + offset, len, nbuf);
			pbuf = nbuf;
		} else
			pbuf = buf + offset;

		send_udp_to_remote(pbuf, len, current_remote);
	}
}

void save_remote_addr(struct sockaddr_storage *rmt, int sock_len, int index)
{
	char rip[200];
	if (memcmp((void *)rmt, (void *)(&remote_addr[index]), sock_len) == 0)
		return;
	memcpy((void *)&remote_addr[index], rmt, sock_len);
	if (rmt->ss_family == AF_INET) {
		struct sockaddr_in *r = (struct sockaddr_in *)rmt;
		err_msg("nat mode, change remote to %s:%d", inet_ntop(r->sin_family, (void *)&r->sin_addr, rip, 200), ntohs(r->sin_port));
	} else if (rmt->ss_family == AF_INET6) {
		struct sockaddr_in6 *r = (struct sockaddr_in6 *)rmt;
		err_msg("nat mode, change remote to [%s]:%d", inet_ntop(r->sin6_family, (void *)&r->sin6_addr, rip, 200), ntohs(r->sin6_port));
	}
}

void process_udp_to_raw(int index)
{
	u_int8_t buf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
	u_int8_t nbuf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
	u_int8_t *pbuf;
	int len;

	while (1) {		// read from remote udp
		if (nat[index]) {
			struct sockaddr_storage rmt;
			socklen_t sock_len = sizeof(struct sockaddr_storage);
			len = recvfrom(fdudp[index], buf, MAX_PACKET_SIZE, 0, (struct sockaddr *)&rmt, &sock_len);
			if (debug) {
				char rip[200];
				if (rmt.ss_family == AF_INET) {
					struct sockaddr_in *r = (struct sockaddr_in *)&rmt;
					Debug("nat mode: len %d recv from %s:%d",
					      len, inet_ntop(r->sin_family, (void *)&r->sin_addr, rip, 200), ntohs(r->sin_port));
				} else if (rmt.ss_family == AF_INET6) {
					struct sockaddr_in6 *r = (struct sockaddr_in6 *)&rmt;
					Debug("nat mode: len %d recv from [%s]:%d",
					      len, inet_ntop(r->sin6_family, (void *)&r->sin6_addr, rip, 200), ntohs(r->sin6_port));
				}
			}
			if (len <= 0)
				continue;
			if (enc_key_len > 0) {
				len = do_decrypt((u_int8_t *) buf, len, nbuf);
				pbuf = nbuf;
			} else
				pbuf = buf;

			if (len <= 0)
				continue;

			nbuf[len] = 0;
			if (mypassword[0] == 0) {	// no password set, accept new ip and port
				Debug("no password, accept new remote ip and port");
				save_remote_addr(&rmt, sock_len, index);
				if (memcmp(pbuf, "PASSWORD:", 9) == 0)	// got password packet, skip this packet
					continue;
			} else {
				if (memcmp(pbuf, "PASSWORD:", 9) == 0) {	// got password packet
					Debug("password packet from remote %s", pbuf);
					if ((memcmp(pbuf + 9, mypassword, strlen(mypassword)) == 0)
					    && (*(pbuf + 9 + strlen(mypassword))
						== 0)) {
						Debug("password ok");
						save_remote_addr(&rmt, sock_len, index);
					} else if (debug)
						printf("error\n");
					continue;
				}
				if (memcmp((void *)&remote_addr[index], &rmt, sock_len)) {
					Debug("packet from unknow host, drop...");
					continue;
				}
			}
		} else {
			len = recv(fdudp[index], buf, MAX_PACKET_SIZE, 0);
			if (len <= 0)
				continue;
			if (enc_key_len > 0) {
				len = do_decrypt((u_int8_t *) buf, len, nbuf);
				pbuf = nbuf;
			} else
				pbuf = buf;
			if (len <= 0)
				continue;
		}

		if (memcmp(pbuf, "PING:PING:", 10) == 0) {
#ifdef DEBUGPINGPONG
			Debug("ping from index %d udp", index);
#endif
			ping_recv[index]++;
			memcpy(buf, "PONG:PONG:", 10);
			len = 10;
			if (enc_key_len > 0) {
				len = do_encrypt((u_int8_t *) buf, len, nbuf);
				pbuf = nbuf;
			} else
				pbuf = buf;
			send_udp_to_remote(pbuf, len, index);
			pong_send[index]++;
			continue;
		}

		if (memcmp(pbuf, "PONG:PONG:", 10) == 0) {
#ifdef DEBUGPINGPONG
			Debug("pong from index %d udp", index);
#endif
			last_pong[index] = myticket;
			pong_recv[index]++;
			continue;
		}

		if (read_only)
			continue;	// read only
		if (!write_only && fixmss)	// write only, no fix_mss
			fix_mss(pbuf, len, index);

		if (debug)
			printPacket((EtherPacket *) pbuf, len, "from remote udpsocket:");
		if (mode == MODEE) {
			struct sockaddr_ll sll;
			memset(&sll, 0, sizeof(sll));
			sll.sll_family = AF_PACKET;
			sll.sll_protocol = htons(ETH_P_ALL);
			sll.sll_ifindex = ifindex;
			sendto(fdraw, pbuf, len, 0, (struct sockaddr *)&sll, sizeof(sll));
		} else if ((mode == MODEI) || (mode == MODEB))
			write(fdraw, pbuf, len);
	}
}

void process_udp_to_raw_master(void)
{
	process_udp_to_raw(MASTER);
}

void process_udp_to_raw_slave(void)
{
	process_udp_to_raw(SLAVE);
}

int open_tun(const char *dev, char **actual)
{
	struct ifreq ifr;
	int fd;
	// char *device = "/dev/tun"; //uClinux tun
	char *device = "/dev/net/tun";	//RedHat tun
	int size;

	if ((fd = open(device, O_RDWR)) < 0)	//???? 
	{
		Debug("Cannot open TUN/TAP dev %s", device);
		exit(1);
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_NO_PI;
	if (!strncmp(dev, "tun", 3)) {
		ifr.ifr_flags |= IFF_TUN;
	} else if (!strncmp(dev, "tap", 3)) {
		ifr.ifr_flags |= IFF_TAP;
	} else {
		Debug("I don't recognize device %s as a TUN or TAP device", dev);
		exit(1);
	}
	if (strlen(dev) > 3)	//unit number specified? 
		strncpy(ifr.ifr_name, dev, IFNAMSIZ);
	if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0)	//? 
	{
		Debug("Cannot ioctl TUNSETIFF %s", dev);
		exit(1);
	}
	Debug("TUN/TAP device %s opened", ifr.ifr_name);
	size = strlen(ifr.ifr_name) + 1;
	*actual = (char *)malloc(size);
	memcpy(*actual, ifr.ifr_name, size);
	int n = 40 * 1024 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
	if (debug) {
		socklen_t ln;
		if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, &ln) == 0) {
			Debug("RAW socket RCVBUF setting to %d", n);
		} else
			Debug("RAW socket getopt error");
	}
	return fd;
}

void usage(void)
{
	printf("Usage:\n");
	printf("./EthUDP -e [ options ] localip localport remoteip remoteport eth? \\\n");
	printf("            [ localip localport remoteip remoteport ]\n");
	printf("./EthUDP -i [ options ] localip localport remoteip remoteport ipaddress masklen \\\n");
	printf("            [ localip localport remoteip remoteport ]\n");
	printf("./EthUDP -b [ options ] localip localport remoteip remoteport bridge \\\n");
	printf("            [ localip localport remoteip remoteport ]\n");
	printf("     options:\n");
	printf("         -p password\n");
	printf("         -enc [ xor | aes-128 | aes-192 | aes-256 ]\n");
	printf("         -k key_string\n");
	printf("         -d    enable debug\n");
	printf("         -f    enable fix mss\n");
	printf("         -r    read only of ethernet interface\n");
	printf("         -w    write only of ethernet interface\n");
	printf("         -B    benchmark\n");
	printf("         -nopromisc    do not set ethernet interface to promisc mode(mode e)\n");
	printf("         -noloopcheck  do not check loopback(-r default do check)\n");
	exit(0);
}

#define BENCHCNT 300000
#define PKT_LEN 1500

void do_benchmark(void)
{
	u_int8_t buf[MAX_PACKET_SIZE];
	u_int8_t nbuf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
	unsigned long int pkt_cnt;
	int len;
	struct timeval start_tm, end_tm;
	gettimeofday(&start_tm, NULL);
	fprintf(stderr, "benchmarking for %d packets, %d size...\n", BENCHCNT, PKT_LEN);
	fprintf(stderr, "enc_algorithm = %s\n",
		enc_algorithm == XOR ? "xor" : enc_algorithm == AES_128 ? "aes-128" : enc_algorithm == AES_192 ? "aes-192" : enc_algorithm ==
		AES_256 ? "aes-256" : "none");
	fprintf(stderr, "      enc_key = %s\n", enc_key);
	fprintf(stderr, "      key_len = %d\n", enc_key_len);
	pkt_cnt = BENCHCNT;
	while (1) {
		len = PKT_LEN;
		len = do_encrypt(buf, len, nbuf);
		pkt_cnt--;
		if (pkt_cnt == 0)
			break;
	}
	gettimeofday(&end_tm, NULL);
	float tspan = ((end_tm.tv_sec - start_tm.tv_sec) * 1000000L + end_tm.tv_usec) - start_tm.tv_usec;
	tspan = tspan / 1000000L;
	fprintf(stderr, "%0.3f seconds\n", tspan);
	fprintf(stderr, "PPS: %.0f PKT/S, %.0f Byte/S\n", (float)BENCHCNT / tspan, 1.0 * (PKT_LEN) * (float)BENCHCNT / tspan);
	fprintf(stderr, "UDP BPS: %.0f BPS\n", 8.0 * (PKT_LEN) * (float)BENCHCNT / tspan);
	exit(0);
}

int main(int argc, char *argv[])
{
	pthread_t tid;
	int i = 1;
	int got_one = 0;
	do {
		got_one = 1;
		if (argc - i <= 0)
			usage();
		if (strcmp(argv[i], "-e") == 0)
			mode = MODEE;
		else if (strcmp(argv[i], "-i") == 0)
			mode = MODEI;
		else if (strcmp(argv[i], "-b") == 0)
			mode = MODEB;
		else if (strcmp(argv[i], "-d") == 0)
			debug = 1;
		else if (strcmp(argv[i], "-f") == 0)
			fixmss = 1;
		else if (strcmp(argv[i], "-r") == 0) {
			read_only = 1;
			loopback_check = 1;
		} else if (strcmp(argv[i], "-w") == 0)
			write_only = 1;
		else if (strcmp(argv[i], "-nopromisc") == 0)
			nopromisc = 1;
		else if (strcmp(argv[i], "-noloopcheck") == 0)
			loopback_check = 0;
		else if (strcmp(argv[i], "-B") == 0)
			do_benchmark();
		else if (strcmp(argv[i], "-p") == 0) {
			i++;
			if (argc - i <= 0)
				usage();
			strncpy(mypassword, argv[i], MAXLEN - 1);
		} else if (strcmp(argv[i], "-enc") == 0) {
			i++;
			if (argc - i <= 0)
				usage();
			if (strcmp(argv[i], "xor") == 0)
				enc_algorithm = XOR;
			else if (strcmp(argv[i], "aes-128") == 0)
				enc_algorithm = AES_128;
			else if (strcmp(argv[i], "aes-192") == 0)
				enc_algorithm = AES_192;
			else if (strcmp(argv[i], "aes-256") == 0)
				enc_algorithm = AES_256;
		} else if (strcmp(argv[i], "-k") == 0) {
			i++;
			if (argc - i <= 0)
				usage();
			memset(enc_key, MAXLEN, 0);
			strncpy((char *)enc_key, argv[i], MAXLEN - 1);
			enc_key_len = strlen((char *)enc_key);
		} else
			got_one = 0;
		if (got_one)
			i++;
	}
	while (got_one);
	if ((mode == MODEE) || (mode == MODEB)) {
		if (argc - i == 9)
			master_slave = 1;
		else if (argc - i != 5)
			usage();
	}
	if (mode == MODEI) {
		if (argc - i == 10)
			master_slave = 1;
		else if (argc - i != 6)
			usage();
	}
	if (mode == -1)
		usage();
	if (debug) {
		printf("         debug = 1\n");
		printf("          mode = %d (0 raw eth bridge, 1 interface, 2 bridge)\n", mode);
		printf("      password = %s\n", mypassword);
		printf(" enc_algorithm = %s\n",
		       enc_algorithm == XOR ? "xor" : enc_algorithm == AES_128 ? "aes-128" : enc_algorithm == AES_192 ? "aes-192" : enc_algorithm ==
		       AES_256 ? "aes-256" : "none");
		printf("       enc_key = %s\n", enc_key);
		printf("       key_len = %d\n", enc_key_len);
		printf("  master_slave = %d\n", master_slave);
		printf("        fixmss = %d\n", fixmss);
		printf("     read_only = %d\n", read_only);
		printf("loopback_check = %d\n", loopback_check);
		printf("    write_only = %d\n", write_only);
		printf("     nopromisc = %d\n", nopromisc);
		printf("           cmd = ");
		int n;
		for (n = i; n < argc; n++)
			printf("%s ", argv[n]);
		printf("\n");
	}

	if (debug == 0) {
		daemon_init("EthUDP", LOG_DAEMON);
		while (1) {
			int pid;
			pid = fork();
			if (pid == 0)	// child do the job
				break;
			else if (pid == -1)	// error
				exit(0);
			else
				wait(NULL);	// parent wait for child
			sleep(2);	// wait 2 second, and rerun
		}
	}

	signal(SIGHUP, sig_handler);

	if (mode == MODEE) {	// eth bridge mode
		fdudp[MASTER] = udp_xconnect(argv[i], argv[i + 1], argv[i + 2], argv[i + 3], MASTER);
		if (master_slave)
			fdudp[SLAVE] = udp_xconnect(argv[i + 5], argv[i + 6], argv[i + 7], argv[i + 8], SLAVE);
		fdraw = open_socket(argv[i + 4], &ifindex);
	} else if (mode == MODEI) {	// interface mode
		char *actualname = NULL;
		char buf[MAXLEN];
		fdudp[MASTER] = udp_xconnect(argv[i], argv[i + 1], argv[i + 2], argv[i + 3], MASTER);
		if (master_slave)
			fdudp[SLAVE] = udp_xconnect(argv[i + 6], argv[i + 7], argv[i + 8], argv[i + 9], SLAVE);
		fdraw = open_tun("tap", &actualname);
		snprintf(buf, MAXLEN, "/sbin/ip addr add %s/%s dev %s; /sbin/ip link set %s up", argv[i + 4], argv[i + 5], actualname, actualname);
		if (debug)
			printf(" run cmd: %s\n", buf);
		system(buf);
		if (debug)
			system("/sbin/ip addr");
	} else if (mode == MODEB) {	// bridge mode
		char *actualname = NULL;
		char buf[MAXLEN];
		fdudp[MASTER] = udp_xconnect(argv[i], argv[i + 1], argv[i + 2], argv[i + 3], MASTER);
		if (master_slave)
			fdudp[SLAVE] = udp_xconnect(argv[i + 5], argv[i + 6], argv[i + 7], argv[i + 8], SLAVE);
		fdraw = open_tun("tap", &actualname);
		snprintf(buf, MAXLEN, "/sbin/ip link set %s up; brctl addif %s %s", actualname, argv[i + 4], actualname);
		if (debug)
			printf(" run cmd: %s\n", buf);
		system(buf);
		if (debug)
			system("/sbin/ip addr");
	}
	// create a pthread to forward packets from master udp to raw
	if (pthread_create(&tid, NULL, (void *)process_udp_to_raw_master, NULL)
	    != 0)
		err_sys("pthread_create udp_to_raw_master error");

	// create a pthread to forward packets from slave udp to raw
	if (master_slave)
		if (pthread_create(&tid, NULL, (void *)process_udp_to_raw_slave, NULL)
		    != 0)
			err_sys("pthread_create udp_to_raw_slave error");

	if (pthread_create(&tid, NULL, (void *)send_keepalive_to_udp, NULL) != 0)	// send keepalive to remote  
		err_sys("pthread_create send_keepalive error");

	//  forward packets from raw to udp
	process_raw_to_udp();

	return 0;
}
