#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>

#define MAX_CONNS 1024
#define PORT 33893
#define REMOTE_PORT 23
#define NUM_GLOBAL_SOCKETS 2
#define MAX_CMD_SIZE 128
#define RCVR_CMD_WAIT_USEC 100000

static int g_sl;

static struct sockaddr_in6 g_rcvr_addr = { 0 };

typedef struct buf_s {
  char data[MAX_CMD_SIZE];
  size_t size;
} buf_t;

typedef struct conn_s {
  int fd;
  buf_t buf;
} conn_t;

static conn_t g_rcvr_conn = { 0 };

static conn_t g_conns[MAX_CONNS];
static int g_num_conns = 0;

static struct pollfd g_pollfds[NUM_GLOBAL_SOCKETS + MAX_CONNS];


static int set_blocking(int fd, bool blocking) {
  int flags;
  if ((flags = fcntl(fd, F_GETFL)) < 0) {
    perror("fcntl");
    return flags;
  }
  if (blocking) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |= O_NONBLOCK;
  }
  if (fcntl(fd, F_SETFL, flags) < 0) {
    perror("fcntl");
    return -1;
  }
  return 0;
}

static bool is_network_error(int err) {
  switch (err) {
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ECONNREFUSED:
    case ENETUNREACH:
    case ECONNRESET:
    case ETIMEDOUT:
      return true;
    default:
      return false;
  }
}

static bool is_wouldblock(int err) {
  return (err == EAGAIN) || (err == EWOULDBLOCK);
}

static bool should_retry_error(int err) {
  if (is_network_error(err)) {
    return true;
  }
  if (is_wouldblock(err)) {
    return true;
  }
  switch (err) {
    case EINPROGRESS:
    case EINTR:
    case ECONNRESET:
      return true;
    default:
      return false;
  }
}

static int create_socket(int listen_port) {
  int s = socket(AF_INET6, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket");
    return s;
  }
  if (set_blocking(s, false) < 0) {
    close(s);
    return -1;
  }
  if (listen_port) {
    struct sockaddr_in6 addr = { 0 };
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(listen_port);

    int one = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
      perror("setsockopt(SO_REUSEADDR)");
      close(s);
      return -1;
    }
    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
      perror("setsockopt(TCP_NODELAY)");
      close(s);
      return -1;
    }
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      perror("bind");
      close(s);
      return -1;
    }
    if (listen(s, MAX_CONNS) < 0) {
      perror("listen");
      close(s);
      return -1;
    }
  }
  return s;
}

static void close_conn(int index) {
  assert(index >= 0);
  assert(index < g_num_conns);
  close(g_conns[index].fd);
  g_conns[index] = g_conns[g_num_conns - 1];
  g_pollfds[NUM_GLOBAL_SOCKETS + index] =
      g_pollfds[NUM_GLOBAL_SOCKETS + g_num_conns - 1];
  g_num_conns--;
}

static int blocking_connect_rcvr() {
  if (set_blocking(g_rcvr_conn.fd, true) < 0) {
    return -1;
  }
  while (connect(g_rcvr_conn.fd, (struct sockaddr*)&g_rcvr_addr,
                 sizeof(g_rcvr_addr)) < 0) {
    if (!should_retry_error(errno)) {
      perror("connect");
      return -1;
    }
    sleep(1);
  }
  if (set_blocking(g_rcvr_conn.fd, false) < 0) {
    return -1;
  }
  return 0;
}

static ssize_t do_recv(conn_t* conn) {
  ssize_t r = recv(conn->fd, conn->buf.data + conn->buf.size,
                   sizeof(conn->buf.data) - conn->buf.size, 0);
  if (r > 0) {
    conn->buf.size += r;
  }
  return r;
}

static int accept_one_conn(int server) {
  int s = accept(server, NULL, NULL);
  if (s < 0) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      return 0;
    } else if (should_retry_error(errno)) {
      return 1;
    } else {
      perror("accept");
      return -1;
    }
  }

  set_blocking(s, false);

  if (g_num_conns >= MAX_CONNS) {
    close(s);
    return 1;
  }

  int index = g_num_conns++;
  conn_t* conn = &g_conns[index];
  struct pollfd* pfd = &g_pollfds[NUM_GLOBAL_SOCKETS + index];
  conn->fd = s;
  memset(&conn->buf, 0, sizeof(conn->buf));
  pfd->fd = s;
  pfd->events = POLLIN;
  pfd->revents = 0;

  return 0;
}

static int parse_cmd(buf_t* buf, char delim, char* cmd, size_t cmd_size) {
  int i;
  for (i = 0; (i < buf->size) && (i < cmd_size) &&
              (buf->data[i] != delim); i++) {
    cmd[i] = buf->data[i];
  }
  if (i >= cmd_size) {
    buf->size = 0;
    return -1;
  }
  if ((i < buf->size) && (buf->data[i] == delim)) {
    cmd[i] = 0;
    memmove(buf->data, buf->data + i + 1, sizeof(buf->data) - i - 1);
    buf->size -= i + 1;
    return 1;
  }
  return 0;
}

int main(int argc, const char** argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <rcvr addr>\n", argv[0]);
    return 1;
  }

  g_rcvr_addr.sin6_family = AF_INET6;
  g_rcvr_addr.sin6_port = htons(REMOTE_PORT);
  if (inet_pton(AF_INET6, argv[1], &g_rcvr_addr.sin6_addr) != 1) {
    fprintf(stderr, "Bad address: %s\n", argv[1]);
    return 1;
  }

  if ((g_rcvr_conn.fd = create_socket(0)) < 0) {
    return 1;
  }
  if ((g_sl = create_socket(PORT)) < 0) {
    return 1;
  }

  if (blocking_connect_rcvr() < 0) {
    return 1;
  }

  g_pollfds[0].fd = g_rcvr_conn.fd;
  g_pollfds[0].events = POLLIN;
  g_pollfds[1].fd = g_sl;
  g_pollfds[1].events = POLLIN;

  while (1) {
    if (poll(g_pollfds, NUM_GLOBAL_SOCKETS + g_num_conns, -1) < 0) {
      perror("poll");
      return 1;
    }

    if (g_pollfds[1].revents) {
      int r;
      while ((r = accept_one_conn(g_pollfds[1].fd)) >= 1);
      if (r < 0) {
        return 1;
      }
    }

    if (g_pollfds[0].revents) {
      ssize_t r;
      r = do_recv(&g_rcvr_conn);
      if ((r <= 0) && !is_wouldblock(errno)) {
        if (blocking_connect_rcvr() < 0) {
          return 1;
        }
      }
      char cmd[MAX_CMD_SIZE];
      while (parse_cmd(&g_rcvr_conn.buf, '\r', cmd, sizeof(cmd)) > 0) {
        printf("%s\n", cmd);
        char sendcmd[MAX_CMD_SIZE];
        int n = snprintf(sendcmd, sizeof(sendcmd), "%s\n", cmd);
        for (int i = 0; i < g_num_conns; i++) {
          r = send(g_conns[i].fd, sendcmd, n, 0);
          if ((r < 0) && !is_wouldblock(errno)) {
            close_conn(i);
            i--;
            continue;
          }
        }
      }
    }

    for (int i = 0; i < g_num_conns; i++) {
      if (g_pollfds[NUM_GLOBAL_SOCKETS + i].revents) {
        ssize_t r;
        r = do_recv(&g_conns[i]);
        if ((r <= 0) && !is_wouldblock(errno)) {
          close_conn(i);
          i--;
          continue;
        }
        char cmd[MAX_CMD_SIZE];
        while (parse_cmd(&g_conns[i].buf, '\n', cmd, sizeof(cmd)) > 0) {
          char sendcmd[MAX_CMD_SIZE];
          int n = snprintf(sendcmd, sizeof(sendcmd), "%s\r", cmd);
          r = -1;
          while (r <= 0) {
            r = send(g_rcvr_conn.fd, sendcmd, n, 0);
            if ((r <= 0) && !is_wouldblock(errno)) {
              if (blocking_connect_rcvr() < 0) {
                return 1;
              }
            }
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = RCVR_CMD_WAIT_USEC;
            select(0, NULL, NULL, NULL, &tv);
          }
        }
      }
    }
  }

  return 0;
}
