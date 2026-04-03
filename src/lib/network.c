#include "../base/all.h"

// https://stackoverflow.com/questions/1098897/what-is-the-largest-safe-udp-packet-size-on-the-internet
#define UDP_MAX_MESSAGE_LEN 508
#ifndef NET_OUTGOING_MESSAGE_QUEUE_LEN
#define NET_OUTGOING_MESSAGE_QUEUE_LEN 16
#endif

#ifndef NET_SERVER_MAX_CLIENTS
#define NET_SERVER_MAX_CLIENTS (16)
#endif

typedef struct sockaddr_in SocketAddress;

typedef struct MultiServer {
  bool ready;
  i32 udp_socket_fd;
  i32 tcp_socket_fd;
  SocketAddress address;
} MultiServer;

typedef struct TCPServer {
  bool ready;
  i32 socket_fd;
  SocketAddress address;
} TCPServer;

typedef struct UDPServer {
  bool ready;
  SocketAddress server_address;
  i32 server_socket;
} UDPServer;

typedef struct ServableUDPInfo {
  UDPServer server;
  void (*callback)(u8* udp_message, u32 udp_len, SocketAddress sending_address, i32 socket);
} ServableUDPInfo;

typedef struct TCPClient {
  bool ready;
  SocketAddress server_address;
  i32 socket;
} TCPClient;

typedef struct UDPClient {
  bool ready;
  SocketAddress server_address;
  u16 client_port;
  i32 socket;
} UDPClient;

typedef struct NetworkMessage {
  bool use_socket; // default = false = UDP, just sendto(address)
  u16 bytes_len;
  SocketAddress address;
  i32 socket_fd;
  u8 bytes[UDP_MAX_MESSAGE_LEN];
} NetworkMessage;

typedef struct OutgoingMessageQueue {
  NetworkMessage items[NET_OUTGOING_MESSAGE_QUEUE_LEN];
  u32 head;
  u32 tail;
  u32 count;
  Mutex mutex;
  Cond not_empty;
  Cond not_full;
} OutgoingMessageQueue;

fn OutgoingMessageQueue* newOutgoingMessageQueue(Arena* a) {
  OutgoingMessageQueue* result = arenaAlloc(a, sizeof(OutgoingMessageQueue));
  MemoryZero(result, (sizeof *result));
  result->mutex = newMutex();
  result->not_full = newCond();
  result->not_empty = newCond();
  return result;
}

fn void outgoingMessageQueuePush(OutgoingMessageQueue* queue, NetworkMessage* msg) {
  lockMutex(&queue->mutex); {
    while (queue->count == NET_OUTGOING_MESSAGE_QUEUE_LEN) {
      waitForCondSignal(&queue->not_full, &queue->mutex);
    }

    MemoryCopy(&queue->items[queue->tail], msg, (sizeof *msg));
    queue->tail = (queue->tail + 1) % NET_OUTGOING_MESSAGE_QUEUE_LEN;
    queue->count++;

    signalCond(&queue->not_empty);
  } unlockMutex(&queue->mutex);
}

fn NetworkMessage* outgoingMessageNonblockingQueuePop(OutgoingMessageQueue* q, NetworkMessage* copy_target) {
  // immediately returns NULL if there's nothing in the ThreadQueue
  // copies the ParsedClientCommand into `copy_target` if there is something in the queue
  // and marks it as popped from the queue
  NetworkMessage* result = NULL;

  lockMutex(&q->mutex); {
    if (q->count > 0) {
      result = &q->items[q->head];
      MemoryCopy(copy_target, result, (sizeof *copy_target));
      q->head = (q->head + 1) % NET_OUTGOING_MESSAGE_QUEUE_LEN;
      q->count--;

      signalCond(&q->not_full);
    }
  } unlockMutex(&q->mutex);

  return result;
}

fn NetworkMessage* outgoingMessageQueuePop(OutgoingMessageQueue* q, NetworkMessage* copy_target) {
  NetworkMessage* result = NULL;

  lockMutex(&q->mutex); {
    while (q->count == 0) {
        waitForCondSignal(&q->not_empty, &q->mutex);
    }

    result = &q->items[q->head];
    MemoryCopy(copy_target, result, (sizeof *copy_target));
    q->head = (q->head + 1) % NET_OUTGOING_MESSAGE_QUEUE_LEN;
    q->count--;

    signalCond(&q->not_full);
  } unlockMutex(&q->mutex);

  return result;
}

fn bool socketAddressEqual(SocketAddress a, SocketAddress b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr
    && a.sin_port == b.sin_port;
}

// ONLY WORKS ON POSIX. taken from https://gist.github.com/miekg/a61d55a8ec6560ad6c4a2747b21e6128

// the only real difference between a udp "server" and a "client" is the bind() syscall
// that the server makes in order to specify a port/address that it's listening on
i32 recv_exact(i32 socket, void* buf, u16 bytes_to_recv) {
  i32 got, got_this_iter;
  for (got = 0; got < bytes_to_recv; got += got_this_iter) {
    got_this_iter = recv(socket, (char*)buf + got, bytes_to_recv - got, 0);
    if (got_this_iter <= 0) return got_this_iter;
  }
  return got;
}

UDPServer createUDPServer(u16 server_port) {
  UDPServer result = {0};
  // define the address we'll be listening on
  result.server_address.sin_family = AF_INET;
	result.server_address.sin_addr.s_addr = inet_addr("0.0.0.0");//htonl(INADDR_ANY);
	result.server_address.sin_port = htons(server_port);

  // get a FileDescriptor number from the OS to use for our socket
  result.server_socket = socket(PF_INET, SOCK_DGRAM, 0);
  if (result.server_socket < 0) {
    return result;
  }
  // to let us immediately kill and restart server
  i32 optval = 1;
	setsockopt(result.server_socket, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(i32));

  result.ready = bind(result.server_socket, (struct sockaddr *)&result.server_address, sizeof(result.server_address)) >= 0;

  return result;
}

UDPClient createUDPClient(u16 server_port, str addr) {
  UDPClient result = {0};
  // define the address we'll be listening on
  result.server_address.sin_family = AF_INET;
  if (addr == 0) {
    result.server_address.sin_addr.s_addr = inet_addr("127.0.0.1");
  } else {
    result.server_address.sin_addr.s_addr = inet_addr(addr);
  }
	result.server_address.sin_port = htons(server_port);

  // get a FileDescriptor number from the OS to use for our socket
  result.socket = socket(PF_INET, SOCK_DGRAM, 0);
  if (result.socket < 0) {
    return result;
  }

  struct sockaddr_in client_address = {0};
  client_address.sin_family = AF_INET;
  client_address.sin_addr.s_addr = htonl(INADDR_ANY);
  client_address.sin_port = 0;
  result.ready = bind(result.socket, (struct sockaddr *)&client_address, sizeof(client_address)) >= 0;
  struct sockaddr_in empty_addr;
  socklen_t addr_len = sizeof(empty_addr);
  getsockname(result.socket, (struct sockaddr *)&empty_addr, &addr_len);
  result.client_port = ntohs(empty_addr.sin_port);
  return result;
}

void infiniteReadUDPServer(UDPServer* server, void (*handleMessage)(u8* udp_message, u32 udp_len, SocketAddress sending_address, i32 socket)) {
  u8 message_buffer[UDP_MAX_MESSAGE_LEN] = {0};
  i32 bytes_recieved = 0;
  SocketAddress client_address = {0};
  i32 addrlen = sizeof(struct sockaddr);
  while (true) {
    bytes_recieved = recvfrom(server->server_socket, message_buffer, UDP_MAX_MESSAGE_LEN, 0, (struct sockaddr *)&client_address, (socklen_t*)&addrlen);
    handleMessage(message_buffer, bytes_recieved, client_address, server->server_socket);
    MemoryZero(message_buffer, UDP_MAX_MESSAGE_LEN);
  }
}

void* infinitelyServeUDPSocket(void* servable) {
  ServableUDPInfo info = *(ServableUDPInfo*)servable;
  infiniteReadUDPServer(&info.server, info.callback);
  return NULL;
}

TCPClient createTCPClient(u16 server_port, str addr) {
  TCPClient result = {0};
  // define the address we'll be listening on
  result.server_address.sin_family = AF_INET;
  if (addr == 0) {
    result.server_address.sin_addr.s_addr = inet_addr("127.0.0.1");
  } else {
    result.server_address.sin_addr.s_addr = inet_addr(addr);
  }
	result.server_address.sin_port = htons(server_port);

  // get a FileDescriptor number from the OS to use for our socket
  result.socket = socket(PF_INET, SOCK_STREAM, 0);
  if (result.socket < 0) {
    return result;
  }
  socklen_t addr_len = sizeof(struct sockaddr_in);
  i32 connect_result = connect(result.socket, (struct sockaddr *)&result.server_address, addr_len);
  result.ready = connect_result != -1;

  return result;
}

TCPServer createTCPServer(u16 server_port) {
  TCPServer result = {0};
  // define the address we'll be listening on
  result.address.sin_family = AF_INET;
	result.address.sin_addr.s_addr = inet_addr("0.0.0.0");//htonl(INADDR_ANY);
	result.address.sin_port = htons(server_port);

  // get a FileDescriptor number from the OS to use for our TCP socket
  result.socket_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (result.socket_fd < 0) {
    return result;
  }
  // to let us immediately kill and restart server
  i32 optval = 1;
	setsockopt(result.socket_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(i32));

  // bind() the TCP
  result.ready = bind(result.socket_fd, (struct sockaddr *)&result.address, sizeof(result.address)) >= 0;
  if (result.ready) {
    result.ready = result.ready && (listen(result.socket_fd, 10) >= 0);
  }

  return result;
}

i32 recvMessage(i32 socket, u8* message_buffer, void (*addSystemMessage)(u8* msg)) {
  i32 bytes_recieved;
  u16 msg_len;
  i32 first_recv_got = recv_exact(socket, &msg_len, 2);
  if (first_recv_got <= 0) {
    return first_recv_got;
  }
  msg_len = ntohs(msg_len); // parse it to our correct byte order

  bytes_recieved = recv_exact(socket, message_buffer, msg_len);
  if (addSystemMessage != NULL) {
    char sbuf[128] = {0};
    sprintf(sbuf, "bytes_recieved=%d\n", bytes_recieved);
    addSystemMessage((u8*)sbuf);
  }
  return bytes_recieved;
}

void infiniteReadTCPServer(
  TCPServer* server,
  bool* should_quit,
  void (*handleMessage)(u8* udp_message, u32 udp_len, SocketAddress sending_address, i32 socket),
  void (*closeConnection)(i32 socket_fd),
  void (*addSystemMessage)(u8* msg)
) {
  u8 message_buffer[UDP_MAX_MESSAGE_LEN] = {0};
  i32 bytes_recieved = 0;
  SocketAddress client_address = {0};
  i32 addrlen = sizeof(struct sockaddr);
  i32 new_fd;
  struct pollfd pollable_fds[NET_SERVER_MAX_CLIENTS+1] = {0}; // +1 for the listener socket
  // poll the `listen()`ed socket_fd
  pollable_fds[0].fd = server->socket_fd;
  pollable_fds[0].events = POLLIN;
  u32 pollable_fd_count = 1;

  i32 poll_event_count;
  while (*should_quit == false) {
    poll_event_count = poll(pollable_fds, pollable_fd_count, 3000); // times out after 3 seconds so that quitting the server will actually quit the server process in relatively short order.
    if (poll_event_count == -1) {
      // TODO handle error better
      *should_quit = true;
      continue;
    }
    for(u32 i = 0; i < pollable_fd_count; i++) {
      bool is_fd_readable = pollable_fds[i].revents & (POLLIN | POLLHUP);
      if (is_fd_readable) {
        bool is_fd_main_server_listener = pollable_fds[i].fd == server->socket_fd;
        if (is_fd_main_server_listener) { // it's a new connection
          new_fd = accept(server->socket_fd, (struct sockaddr *)&client_address, (socklen_t*)&addrlen);
          if (new_fd == -1) {
            if (addSystemMessage != NULL) {
              addSystemMessage((u8*)"TODO: handle this accept() error for real, bitch");
            }
          } else {
            if (pollable_fd_count < NET_SERVER_MAX_CLIENTS+1) {
              pollable_fds[pollable_fd_count].fd = new_fd;
              pollable_fds[pollable_fd_count].events = POLLIN | POLLHUP;
              pollable_fds[pollable_fd_count].revents = 0;
              pollable_fd_count++;
              if (addSystemMessage != NULL) {
                char sbuf[128] = {0};
                sprintf(sbuf, "new connection on socket=%d, total=%d\n", new_fd, pollable_fd_count);
                addSystemMessage((u8*)sbuf);
              }
            } else {
              send(new_fd, "server full", 11, 0); // go away sir, we are out of space to keep track of this socket
              close(new_fd);
            }
          }
        } else {// Otherwise we're just a regular client
          bytes_recieved = recvMessage(pollable_fds[i].fd, message_buffer, addSystemMessage);
          if (bytes_recieved <= 0) { // error condition
            bool client_hung_up = bytes_recieved == 0;
            // TODO do something with the error case
            closeConnection(pollable_fds[i].fd);
            close(pollable_fds[i].fd);
            if (addSystemMessage != NULL) {
              char sbuf[256] = {0};
              sprintf(sbuf, "closed connection on socket=%d, client_hung_up? %s\n", pollable_fds[i].fd, client_hung_up ? "yes" : "no");
              addSystemMessage((u8*)sbuf);
            }
            // copy the last one over the current one and "forget" the last one by decrementing the count
            pollable_fds[i] = pollable_fds[--pollable_fd_count];
          } else { // we got an actual message from this guy
            handleMessage(message_buffer, bytes_recieved, client_address, pollable_fds[i].fd);
          }
          MemoryZero(message_buffer, UDP_MAX_MESSAGE_LEN);
        }
      }
    }
  }
}

void infiniteReadTCPClient(
  i32 socket,
  bool* should_quit,
  void (*handleMessage)(u8* udp_message, i32 bytes_recieved),
  void (*addSystemMessage)(u8* msg)
) {
  u8 message_buffer[UDP_MAX_MESSAGE_LEN] = {0};
  i32 bytes_recieved = 0;
  while ((*should_quit) != true) {
    bytes_recieved = recvMessage(socket, message_buffer, addSystemMessage);
    if (bytes_recieved == -1) {
      if (addSystemMessage != NULL) {
        char sbuf[128] = {0};
        sprintf(sbuf, "TODO handle this error, bytes_recieved=%d\n", bytes_recieved);
        addSystemMessage((u8*)sbuf);
      }
      continue;
    }

    handleMessage(message_buffer, bytes_recieved);
    MemoryZero(message_buffer, UDP_MAX_MESSAGE_LEN);
  }
}

MultiServer createMultiServer(u16 server_port) {
  MultiServer result = {0};
  // define the address we'll be listening on
  result.address.sin_family = AF_INET;
	result.address.sin_addr.s_addr = inet_addr("0.0.0.0");//htonl(INADDR_ANY);
	result.address.sin_port = htons(server_port);

  // get a FileDescriptor number from the OS to use for our UDP socket
  result.udp_socket_fd = socket(PF_INET, SOCK_DGRAM, 0);
  if (result.udp_socket_fd < 0) {
    return result;
  }
  // get a FileDescriptor number from the OS to use for our TCP socket
  result.tcp_socket_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (result.tcp_socket_fd < 0) {
    return result;
  }
  // to let us immediately kill and restart server
  i32 optval = 1;
	setsockopt(result.udp_socket_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(i32));
	setsockopt(result.tcp_socket_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(i32));

  // bind() the UDP
  result.ready = bind(result.udp_socket_fd, (struct sockaddr *)&result.address, sizeof(result.address)) >= 0;

  // bind() the TCP
  result.ready = result.ready && (bind(result.tcp_socket_fd, (struct sockaddr *)&result.address, sizeof(result.address)) >= 0);
  
  if (result.ready) {
    result.ready = result.ready && (listen(result.tcp_socket_fd, 10) >= 0);
  }

  return result;
}

void infiniteReadMultiServer(MultiServer* server, void (*handleMessage)(u8* udp_message, u32 udp_len, SocketAddress sending_address, i32 socket)) {
  UDPServer udp_server = {
    .ready = server->ready,
    .server_address = server->address,
    .server_socket = server->udp_socket_fd,
  };
  ServableUDPInfo udp_info = {
    .server = udp_server,
    .callback = handleMessage,
  };
  Thread udp_listener_thread = spawnThread(&infinitelyServeUDPSocket, &udp_info);

  u8 message_buffer[UDP_MAX_MESSAGE_LEN] = {0};
  SocketAddress client_address = {0};
  i32 addrlen = sizeof(struct sockaddr);
  i32 new_fd;
  i32 bytes_recieved = 0;
  while (true) {
    // TCP
    new_fd = accept(server->tcp_socket_fd, (struct sockaddr *)&client_address, (socklen_t*)&addrlen);
    bytes_recieved = recv(new_fd, message_buffer, UDP_MAX_MESSAGE_LEN, 0);
    if (bytes_recieved > 0) {
      handleMessage(message_buffer, bytes_recieved, client_address, new_fd);
    }
    MemoryZero(message_buffer, UDP_MAX_MESSAGE_LEN);
  }
  osThreadJoin(udp_listener_thread, MAX_u64);
}

// TODO: sendall() to handle cases when the sendto() bytes return value is less than the intended bytes to send... stupid kernel fuckin wit us.
i32 sendUDPu8List(i32 using_socket, SocketAddress* to, u8List* message) {
  return sendto(
    using_socket,
    message->items,
    message->length,
    0,
    (const struct sockaddr *)to,
    sizeof(struct sockaddr)
  );
}

i32 sendUDPMessage(UDPServer* to, u8* message, u32 len) {
  return sendto(to->server_socket, message, len, 0, (struct sockaddr *)&to->server_address, sizeof(struct sockaddr));
}
i32 sendTCPMessage(NetworkMessage msg) {
  assert(msg.use_socket);
  assert(msg.socket_fd >= 0);
  u16 msg_len = htons(msg.bytes_len);
  i32 result = send(msg.socket_fd, &msg_len, 2, 0);
  if (result == -1) {
    return result;
  }
  return send(msg.socket_fd, msg.bytes, msg.bytes_len, 0);
}

