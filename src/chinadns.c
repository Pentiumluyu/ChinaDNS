#include <fcntl.h>
#include <netdb.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

typedef struct {
  uint16_t id;
  struct sockaddr *addr;
  socklen_t addrlen;
} id_addr_t;

typedef struct {
  int entries;
  struct in_addr *ips;
} ip_list_t;

typedef struct {
  struct in_addr net;
  in_addr_t mask;
} net_mask_t;

typedef struct {
  int entries;
  net_mask_t *nets;
} net_list_t;


// avoid malloc and free
#define BUF_SIZE 512
static char global_buf[BUF_SIZE];
static char global_rv_buf[BUF_SIZE];
static char compression_buf[BUF_SIZE];

static int verbose = 0;

static int bidirectional = 0;

#if defined(PACKAGE_STRING)
static const char *version = PACKAGE_STRING;
#else
static const char *version = "ChinaDNS";
#endif

static const char *default_dns_servers =
  "114.114.114.114,8.8.8.8,8.8.4.4,208.67.222.222:443,208.67.222.222:5353";
static char *dns_servers = NULL;
static int foreign_dns_servers_len;
static int chn_dns_servers_len;
static id_addr_t *chn_dns_server_addrs;
static id_addr_t *foreign_dns_server_addrs;

static int parse_args(int argc, char **argv);

static int setnonblock(int sock);
static int resolve_dns_servers();

static const char *default_listen_addr = "0.0.0.0";
static const char *default_listen_port = "53";

static char *listen_addr = NULL;
static char *listen_port = NULL;


static char *chnroute_file = NULL;
static net_list_t chnroute_list;
static int parse_chnroute();
static int test_ip_in_list(struct in_addr ip, const net_list_t *netlist);

static int dns_init_sockets();
static void dns_handle_local();
static void dns_handle_remote();

static const char *hostname_from_question(ns_msg msg);
static int should_filter_query(ns_msg msg, struct in_addr dns_addr);

static void queue_add(id_addr_t id_addr);
static id_addr_t *queue_lookup(uint16_t id);

//#define ID_ADDR_QUEUE_LEN 256
// use a queue instead of hash here since it's not long
static id_addr_t id_addr_queue[256];
static int id_addr_queue_pos = 0;

static int local_sock;
static int remote_sock;

static const char *help_message =
  "usage: chinadns [-h] [-b BIND_ADDR] [-p BIND_PORT]\n"
  "       [-c CHNROUTE_FILE] [-s DNS] [-v]\n"
  "Forward DNS requests.\n"
  "\n"
  "  -h, --help            show this help message and exit\n"
  "  -c CHNROUTE_FILE      path to china route file\n"
  "                        if not specified, CHNRoute will be turned off\n"
  "  -d                    enable bi-directional CHNRoute filter\n"
  "  -b BIND_ADDR          address that listens, default: 127.0.0.1\n"
  "  -p BIND_PORT          port that listens, default: 53\n"
  "  -s DNS                DNS servers intended to use,\n"
  "                        and the format should be \"ip:port,ip:port\"\n"
  "                        default: 114.114.114.114,208.67.222.222:443,8.8.8.8\n"
  "  -v                    verbose logging\n"
  "\n"
  "Online help: <https://github.com/Pentiumluyu/ChinaDNS>\n";

#define __LOG(o, t, v, s...) do {                                   \
  time_t now;                                                       \
  time(&now);                                                       \
  char *time_str = ctime(&now);                                     \
  time_str[strlen(time_str) - 1] = '\0';                            \
  if (t == 0) {                                                     \
    if (stdout != o || verbose) {                                   \
      fprintf(o, "%s ", time_str);                                  \
      fprintf(o, s);                                                \
      fflush(o);                                                    \
    }                                                               \
  } else if (t == 1) {                                              \
    fprintf(o, "%s %s:%d ", time_str, __FILE__, __LINE__);          \
    perror(v);                                                      \
  }                                                                 \
} while (0)

#define LOG(s...) __LOG(stdout, 0, "_", s)
#define ERR(s) __LOG(stderr, 1, s, "_")
#define VERR(s...) __LOG(stderr, 0, "_", s)

#ifdef DEBUG
#define DLOG(s...) LOG(s)
void __gcov_flush(void);
static void gcov_handler(int signum)
{
  __gcov_flush();
  exit(1);
}
#else
#define DLOG(s...)
#endif

int main(int argc, char **argv) {
  fd_set readset, errorset;
  int max_fd;

#ifdef DEBUG
  signal(SIGTERM, gcov_handler);
#endif

  memset(&id_addr_queue, 0, sizeof(id_addr_queue));
  if (0 != parse_args(argc, argv))
    return EXIT_FAILURE;
  if (0 != parse_chnroute())
    return EXIT_FAILURE;
  if (0 != resolve_dns_servers())
    return EXIT_FAILURE;
  if (0 != dns_init_sockets())
    return EXIT_FAILURE;

  printf("%s\n", version);
  max_fd = MAX(local_sock, remote_sock) + 1;
  while (1) {
    FD_ZERO(&readset);
    FD_ZERO(&errorset);
    FD_SET(local_sock, &readset);
    FD_SET(local_sock, &errorset);
    FD_SET(remote_sock, &readset);
    FD_SET(remote_sock, &errorset);
    struct timeval timeout = {
      .tv_sec = 0,
      .tv_usec = 50 * 1000,
    };
    if (-1 == select(max_fd, &readset, NULL, &errorset, &timeout)) {
      ERR("select");
      return EXIT_FAILURE;
    }
    //check_and_send_delay();
    if (FD_ISSET(local_sock, &errorset)) {
      // TODO getsockopt(..., SO_ERROR, ...);
      VERR("local_sock error\n");
      return EXIT_FAILURE;
    }
    if (FD_ISSET(remote_sock, &errorset)) {
      // TODO getsockopt(..., SO_ERROR, ...);
      VERR("remote_sock error\n");
      return EXIT_FAILURE;
    }
    if (FD_ISSET(local_sock, &readset))
      dns_handle_local();
    if (FD_ISSET(remote_sock, &readset))
      dns_handle_remote();
  }
  return EXIT_SUCCESS;
}

static int setnonblock(int sock) {
  int flags;
  flags = fcntl(sock, F_GETFL, 0);
  if (flags == -1) {
    ERR("fcntl");
    return -1;
  }
  if (-1 == fcntl(sock, F_SETFL, flags | O_NONBLOCK)) {
    ERR("fcntl");
    return -1;
  }
  return 0;
}

static int parse_args(int argc, char **argv) {
  int ch;
  dns_servers = strdup(default_dns_servers);
  listen_addr = strdup(default_listen_addr);
  listen_port = strdup(default_listen_port);
  while ((ch = getopt(argc, argv, "hb:p:s:l:c:y:dv")) != -1) {
    switch (ch) {
    case 'h':
      printf("%s", help_message);
      exit(0);
    case 'b':
      listen_addr = strdup(optarg);
      break;
    case 'p':
      listen_port = strdup(optarg);
      break;
    case 's':
      dns_servers = strdup(optarg);
      break;
    case 'c':
      chnroute_file = strdup(optarg);
      break;
    case 'd':
      bidirectional = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    default:
      printf("%s", help_message);
      exit(1);
    }
  }
  argc -= optind;
  argv += optind;
  return 0;
}

static int resolve_dns_servers() {
  struct addrinfo hints;
  struct addrinfo *addr_ip;
  char* token;
  int r;
  char *pch = strchr(dns_servers, ',');
  chn_dns_servers_len = 0;
  foreign_dns_servers_len = 0;
  foreign_dns_server_addrs = calloc(1, sizeof(id_addr_t));
  chn_dns_server_addrs = calloc(1, sizeof(id_addr_t));

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
  token = strtok(dns_servers, ",");
  while (token) {
    char *port;
    memset(global_buf, 0, BUF_SIZE);
    strncpy(global_buf, token, BUF_SIZE - 1);
    port = (strrchr(global_buf, ':'));
    if (port) {
      *port = '\0';
      port++;
    } else {
      port = "53";
    }
    if (0 != (r = getaddrinfo(global_buf, port, &hints, &addr_ip))) {
      VERR("%s:%s\n", gai_strerror(r), token);
      return -1;
    }
    token = strtok(0, ",");
    if (test_ip_in_list(((struct sockaddr_in *)addr_ip->ai_addr)->sin_addr,
                        &chnroute_list)) {
      chn_dns_server_addrs[chn_dns_servers_len].addr = addr_ip->ai_addr;
      chn_dns_server_addrs[chn_dns_servers_len].addrlen = addr_ip->ai_addrlen;
      chn_dns_servers_len += 1;
      chn_dns_server_addrs = realloc(chn_dns_server_addrs, (chn_dns_servers_len+1) * sizeof(id_addr_t));
    } else {
      foreign_dns_server_addrs[foreign_dns_servers_len].addr = addr_ip->ai_addr;
      foreign_dns_server_addrs[foreign_dns_servers_len].addrlen = addr_ip->ai_addrlen;
      foreign_dns_servers_len += 1;
      foreign_dns_server_addrs = realloc(foreign_dns_server_addrs, (foreign_dns_servers_len+1) * sizeof(id_addr_t));
    }
  }
  if (chnroute_file) {
    if (!(chn_dns_servers_len && foreign_dns_servers_len)) {
      VERR("You should have at least one Chinese DNS and one foreign DNS when "
          "chnroutes is enabled\n");
      return -1;
    }
  }
  return 0;
}

static int cmp_net_mask(const void *a, const void *b) {
  net_mask_t *neta = (net_mask_t *)a;
  net_mask_t *netb = (net_mask_t *)b;
  if (neta->net.s_addr == netb->net.s_addr)
    return 0;
  // TODO: pre ntohl
  if (ntohl(neta->net.s_addr) > ntohl(netb->net.s_addr))
    return 1;
  return -1;
}

static int parse_chnroute() {
  FILE *fp;
  char line_buf[32];
  char *line;
  size_t len = sizeof(line_buf);
  ssize_t read;
  char net[32];
  chnroute_list.entries = 0;
  int i = 0;

  if (chnroute_file == NULL) {
    VERR("CHNROUTE_FILE not specified, CHNRoute is disabled\n");
    return 0;
  }

  fp = fopen(chnroute_file, "rb");
  if (fp == NULL) {
    ERR("fopen");
    VERR("Can't open chnroute: %s\n", chnroute_file);
    return -1;
  }
  while ((line = fgets(line_buf, len, fp))) {
    chnroute_list.entries++;
  }

  chnroute_list.nets = calloc(chnroute_list.entries, sizeof(net_mask_t));
  if (0 != fseek(fp, 0, SEEK_SET)) {
    VERR("fseek");
    return -1;
  }
  while ((line = fgets(line_buf, len, fp))) {
    char *sp_pos;
    sp_pos = strchr(line, '\r');
    if (sp_pos) *sp_pos = 0;
    sp_pos = strchr(line, '\n');
    if (sp_pos) *sp_pos = 0;
    sp_pos = strchr(line, '/');
    if (sp_pos) {
      *sp_pos = 0;
      chnroute_list.nets[i].mask = (1 << (32 - atoi(sp_pos + 1))) - 1;
    } else {
      chnroute_list.nets[i].mask = UINT32_MAX;
    }
    if (0 == inet_aton(line, &chnroute_list.nets[i].net)) {
      VERR("invalid addr %s in %s:%d\n", line, chnroute_file, i + 1);
      return 1;
    }
    i++;
  }

  qsort(chnroute_list.nets, chnroute_list.entries, sizeof(net_mask_t),
        cmp_net_mask);

  fclose(fp);
  return 0;
}

static int test_ip_in_list(struct in_addr ip, const net_list_t *netlist) {
  // binary search
  int l = 0, r = netlist->entries - 1;
  int m, cmp;
  if (netlist->entries == 0)
    return 0;
  net_mask_t ip_net;
  ip_net.net = ip;
  while (l != r) {
    m = (l + r) / 2;
    cmp = cmp_net_mask(&ip_net, &netlist->nets[m]);
    if (cmp == -1) {
      if (r != m)
        r = m;
      else
        break;
    } else {
      if (l != m)
        l = m;
      else
        break;
    }
    DLOG("l=%d, r=%d\n", l, r);
    DLOG("%s, %d\n", inet_ntoa(netlist->nets[m].net),
         netlist->nets[m].mask);
  }
  DLOG("result: %x\n",
       (ntohl(netlist->nets[l].net.s_addr) ^ ntohl(ip.s_addr)));
  DLOG("mask: %x\n", (UINT32_MAX - netlist->nets[l].mask));
  if ((ntohl(netlist->nets[l].net.s_addr) ^ ntohl(ip.s_addr)) &
      (UINT32_MAX ^ netlist->nets[l].mask)) {
    return 0;
  }
  return 1;
}

static int dns_init_sockets() {
  struct addrinfo hints;
  struct addrinfo *addr_ip;
  int r;

  local_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (0 != setnonblock(local_sock))
    return -1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (0 != (r = getaddrinfo(listen_addr, listen_port, &hints, &addr_ip))) {
    VERR("%s:%s:%s\n", gai_strerror(r), listen_addr, listen_port);
    return -1;
  }
  if (0 != bind(local_sock, addr_ip->ai_addr, addr_ip->ai_addrlen)) {
    ERR("bind");
    VERR("Can't bind address %s:%s\n", listen_addr, listen_port);
    return -1;
  }
  freeaddrinfo(addr_ip);
  remote_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (0 != setnonblock(remote_sock))
    return -1;
  return 0;
}

static void dns_handle_local() {
  struct sockaddr *src_addr = malloc(sizeof(struct sockaddr));
  socklen_t src_addrlen = sizeof(struct sockaddr);
  uint16_t query_id;
  ssize_t len;
  int i;
  const char *question_hostname;
  ns_msg msg;
  len = recvfrom(local_sock, global_buf, BUF_SIZE, 0, src_addr, &src_addrlen);
  if (len > 0) {
    if (ns_initparse((const u_char *)global_buf, len, &msg) < 0) {
      ERR("ns_initparse");
      free(src_addr);
      return;
    }
    // parse DNS query id
    // TODO generate id for each request to avoid conflicts
    query_id = ns_msg_id(msg);
    question_hostname = hostname_from_question(msg);
    LOG("request %s\n", question_hostname);
    id_addr_t id_addr;
    id_addr.id = query_id;
    id_addr.addr = src_addr;
    id_addr.addrlen = src_addrlen;
    queue_add(id_addr);
    //use compression pointer for foreign dns
    if (len > 16){
      size_t off = 12;
      int ended = 0;
      while (off < len - 4){
        if (global_buf[off] & 0xc0)
          break;
        if (global_buf[off] == 0){
          ended = 1;
          off ++;
          break;
        }
        off += 1 + global_buf[off];
      }
      if (ended) {
        memcpy(compression_buf, global_buf, off-1);
        memcpy(compression_buf + off + 1, global_buf + off, len - off);
        compression_buf[off-1] = '\xc0';
        compression_buf[off] = '\x04';
        for (i = 0; i < foreign_dns_servers_len; i++) {
        	if (-1 == sendto(remote_sock, compression_buf, len + 1, 0,
                       foreign_dns_server_addrs[i].addr, foreign_dns_server_addrs[i].addrlen))
                ERR("sendto");
        }
      }else{
      	for (i = 0; i < foreign_dns_servers_len; i++) {
        	if (-1 == sendto(remote_sock, global_buf, len, 0,
                       foreign_dns_server_addrs[i].addr, foreign_dns_server_addrs[i].addrlen))
                ERR("sendto");
        }
      }
    for (i = 0; i < chn_dns_servers_len; i++) {
      if (-1 == sendto(remote_sock, global_buf, len, 0,
                       chn_dns_server_addrs[i].addr, chn_dns_server_addrs[i].addrlen))
        ERR("sendto");
    }
    }
  }
  else
    ERR("recvfrom");
}

static void dns_handle_remote() {
  struct sockaddr *src_addr = malloc(sizeof(struct sockaddr));
  socklen_t src_len = sizeof(struct sockaddr);
  uint16_t query_id;
  ssize_t len;
  const char *question_hostname;
  //int r;
  ns_msg msg;
  len = recvfrom(remote_sock, global_rv_buf, BUF_SIZE, 0, src_addr, &src_len);
  if (len > 0) {
    if (ns_initparse((const u_char *)global_rv_buf, len, &msg) < 0) {
      ERR("ns_initparse");
      free(src_addr);
      return;
    }
    // parse DNS query id
    // TODO assign new id instead of using id from clients
    query_id = ns_msg_id(msg);
    question_hostname = hostname_from_question(msg);
    if (question_hostname) {
      LOG("response %s from %s:%d - ", question_hostname,
          inet_ntoa(((struct sockaddr_in *)src_addr)->sin_addr),
          htons(((struct sockaddr_in *)src_addr)->sin_port));
    }
    id_addr_t *id_addr = queue_lookup(query_id);
    if (id_addr) {
      id_addr->addr->sa_family = AF_INET;
      if (!should_filter_query(msg, ((struct sockaddr_in *)src_addr)->sin_addr)) {
        if (verbose)
          printf("pass\n");
        if (-1 == sendto(local_sock, global_rv_buf, len, 0, id_addr->addr,
                        id_addr->addrlen))
          ERR("sendto");
      } else {
        if (verbose)
          printf("filter\n");
      }
    } else {
      if (verbose)
        printf("skip\n");
    }
    free(src_addr);
  }
  else
    ERR("recvfrom");
}

static void queue_add(id_addr_t id_addr) {
  id_addr_queue_pos = (uint8_t)(id_addr.id & 0xff);
  // free next hole
  id_addr_t old_id_addr = id_addr_queue[id_addr_queue_pos];
  free(old_id_addr.addr);
  id_addr_queue[id_addr_queue_pos] = id_addr;
}

static id_addr_t *queue_lookup(uint16_t id) {
  int i;
  i = (uint8_t)(id & 0xff);
  if (id_addr_queue[i].id == id)
  	return id_addr_queue + i;
  return NULL;
}

static char *hostname_buf = NULL;
static size_t hostname_buflen = 0;
static const char *hostname_from_question(ns_msg msg) {
  ns_rr rr;
  int rrnum, rrmax;
  const char *result;
  int result_len;
  rrmax = ns_msg_count(msg, ns_s_qd);
  if (rrmax == 0)
    return NULL;
  for (rrnum = 0; rrnum < rrmax; rrnum++) {
    if (ns_parserr(&msg, ns_s_qd, rrnum, &rr)) {
      ERR("ns_parserr");
      return NULL;
    }
    result = ns_rr_name(rr);
    result_len = strlen(result) + 1;
    if (result_len > hostname_buflen) {
      hostname_buflen = result_len << 1;
      hostname_buf = realloc(hostname_buf, hostname_buflen);
    }
    memcpy(hostname_buf, result, result_len);
    return hostname_buf;
  }
  return NULL;
}

static int should_filter_query(ns_msg msg, struct in_addr dns_addr) {
  ns_rr rr;
  int rrnum, rrmax;
  void *r;
  // TODO cache result for each dns server
  int dns_is_chn = 0;
  int i;
  if (chnroute_file && (foreign_dns_servers_len > 0)) {
    for (i = 0; i < chn_dns_servers_len; i++){
       if(dns_addr.s_addr == ((struct sockaddr_in *)chn_dns_server_addrs[i].addr)->sin_addr.s_addr){
          dns_is_chn = 1;
          break;
          }
    }
  }
  rrmax = ns_msg_count(msg, ns_s_an);
  for (rrnum = 0; rrnum < rrmax; rrnum++) {
    if (ns_parserr(&msg, ns_s_an, rrnum, &rr)) {
      ERR("ns_parserr");
      return 0;
    }
    u_int type;
    const u_char *rd;
    type = ns_rr_type(rr);
    rd = ns_rr_rdata(rr);
    if (type == ns_t_a) {
      if (verbose)
        printf("%s, ", inet_ntoa(*(struct in_addr *)rd));
      if (test_ip_in_list(*(struct in_addr *)rd, &chnroute_list)) {
        // result is chn
        if (!dns_is_chn) {
          if (bidirectional) {
            // filter DNS result from chn dns if result is outside chn
            return 1;
          }
        }
      } else {
        // result is foreign
        if (dns_is_chn) {
          // filter DNS result from foreign dns if result is inside chn
          return 1;
        }
      }
    }
  }
  return 0;
}
