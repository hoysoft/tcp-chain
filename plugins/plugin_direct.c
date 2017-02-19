#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <linux/netfilter_ipv4.h>
#include <ev.h>
#include <errno.h>
#include <fcntl.h>

#include "../tcp_chain.h"

struct io_wrap {
  ev_io io;
  struct proxy_wrap* proxy;
};

struct proxy_wrap {
  struct sock_info* identifier;
  int (*relay_send)();
  int (*relay_close)();
  char* pending_send_data;
  size_t pending_send_data_len;
  size_t pending_send_data_buf_len;
  int remote_connected;
  struct io_wrap read_io;
  struct io_wrap write_io;
};

struct ev_loop* default_loop;

int (*relay_send)();
int (*relay_close)();

int setnonblocking(int fd) {
  int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
    flags = 0;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void remote_read_cb(struct ev_loop *loop, struct ev_io *w_, int revents) {
  char buffer[BUFFER_SIZE];

  if(EV_ERROR & revents) {
    return;
  }

  struct proxy_wrap* proxy = ((struct io_wrap*)w_)->proxy;
  struct ev_io* io = &(((struct io_wrap*)w_)->io);

  int remote_fd = io->fd;
  ssize_t read = recv(remote_fd, buffer, BUFFER_SIZE, 0);

  if ((read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) || read == 0) {
    printf("direct: Remote closed connection.\n");
    (*(proxy->relay_close))(proxy->identifier);
    return;
  }

  if (read == -1) {
    return;
  }

  if ((*(proxy->relay_send))(proxy->identifier, buffer, read, 0) == -1) {
    (*(proxy->relay_close))(proxy->identifier);
  }

}

void remote_write_cb(struct ev_loop *loop, struct ev_io *w_, int revents) {
  if(EV_ERROR & revents) {
    return;
  }

  struct proxy_wrap* proxy = ((struct io_wrap*)w_)->proxy;
  struct ev_io* io = &(((struct io_wrap*)w_)->io);

  proxy->remote_connected = 1;

  int remote_fd = io->fd;

  if (proxy->pending_send_data_len <= 0) {
    ev_io_stop(loop, io);
    return;
  }

  size_t bytes_sent = send(remote_fd, proxy->pending_send_data, proxy->pending_send_data_len, 0);

  if (bytes_sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    //ev_io_stop(loop, io);
    printf("direct: Remote sock send error.\n");
    (*(proxy->relay_close))(proxy->identifier);
  }

  if (bytes_sent == -1) {
    return;
  }

  if (bytes_sent < proxy->pending_send_data_len) {
    memmove(proxy->pending_send_data, proxy->pending_send_data + bytes_sent, proxy->pending_send_data_len - bytes_sent);
    proxy->pending_send_data_len -= bytes_sent;
  } else {
    proxy->pending_send_data_len = 0;
    ev_io_stop(loop, io);
  }

}

void on_connect(struct sock_info* identifier) {
  //printf("on_connect() invoked.\n");
  int remote_sock = socket(AF_INET, SOCK_STREAM, 0);

  int sock_mark = 100;
  setsockopt(remote_sock, SOL_SOCKET, SO_MARK, &sock_mark, sizeof(sock_mark));

  int keep_alive = 1;
  int keep_idle = 40;
  int keep_interval = 5;
  int keep_count = 3;
  setsockopt(remote_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&keep_alive, sizeof(keep_alive));
  setsockopt(remote_sock, SOL_TCP, TCP_KEEPIDLE, (void*)&keep_idle, sizeof(keep_idle));
  setsockopt(remote_sock, SOL_TCP, TCP_KEEPINTVL, (void *)&keep_interval, sizeof(keep_interval)); 
  setsockopt(remote_sock, SOL_TCP, TCP_KEEPCNT, (void *)&keep_count, sizeof(keep_count));

  setnonblocking(remote_sock);

  int connect_ret = connect(remote_sock, identifier->dst_addr, sizeof(struct sockaddr));
  if (connect_ret == -1 && errno != EINPROGRESS) {
    printf("direct: Remote connection failed.\n");
    (*relay_close)(identifier);
    return;
  }

  printf("direct: Remote connected.\n");

  struct proxy_wrap* proxy = (struct proxy_wrap*)malloc(sizeof(struct proxy_wrap));
  proxy->identifier = identifier;
  proxy->relay_send = relay_send;
  proxy->relay_close = relay_close;
  proxy->pending_send_data = (char*)malloc(BUFFER_SIZE);
  proxy->pending_send_data_len = 0;
  proxy->pending_send_data_buf_len = BUFFER_SIZE;
  proxy->remote_connected = 0;
  (proxy->read_io).proxy = proxy;
  (proxy->write_io).proxy = proxy;


  identifier->data = (void*)proxy;
  *(identifier->takeovered) = 1;

  ev_io_init(&((proxy->read_io).io), remote_read_cb, remote_sock, EV_READ);
  ev_io_init(&((proxy->write_io).io), remote_write_cb, remote_sock, EV_WRITE);
  ev_io_start(default_loop, &((proxy->read_io).io));
  ev_io_start(default_loop, &((proxy->write_io).io));
}

void on_recv(struct sock_info* identifier, char* data, size_t* length) {
  //printf("on_recv() invoked.\n");
  struct proxy_wrap* proxy = (struct proxy_wrap*)(identifier->data);
  size_t ret;
  if (proxy->remote_connected && proxy->pending_send_data_len == 0) {
    ret = send((proxy->write_io).io.fd, data, *length, 0);
  } else {
    ret = 0;
  }

  if (ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    printf("direct: Remote closed.\n");
    (*relay_close)(identifier);
    return;
  }

  if (ret == -1) {
    ret = 0;
  }

  if (ret < *length) {
    if (proxy->pending_send_data_len + (*length - ret) > proxy->pending_send_data_buf_len) {
      proxy->pending_send_data = (char*)realloc(proxy->pending_send_data, proxy->pending_send_data_len + (*length - ret));
      proxy->pending_send_data_buf_len = proxy->pending_send_data_len + (*length - ret);
    }

    memcpy(proxy->pending_send_data + proxy->pending_send_data_len, data + ret, *length - ret);
    proxy->pending_send_data_len += (*length - ret);

    ev_io_start(default_loop, &((proxy->write_io).io));
  }
}

void on_send(struct sock_info* identifier, char* data, size_t* length) {
  //printf("on_send() invoked.\n");
}

void on_close(struct sock_info* identifier) {
  //printf("on_close() invoked.\n");
  struct proxy_wrap* proxy = (struct proxy_wrap*)(identifier->data);
  if (proxy != NULL) {
    ev_io_stop(default_loop, &((proxy->read_io).io));
    ev_io_stop(default_loop, &((proxy->write_io).io));
    close((proxy->read_io).io.fd);
    free(proxy->pending_send_data);
    free(identifier->data);
  }
}

void on_init(struct init_info* info) {
  default_loop = info->default_loop;

  relay_send = info->relay_send;
  relay_close = info->relay_close;
}
