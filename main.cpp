#include <arpa/inet.h>
#include <memory.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

pthread_mutex_t event_lock;

#define MAX_PKT 8
#define MAX_SEQ 7

typedef unsigned int seq_nr;
typedef struct {
  unsigned char data[MAX_PKT];
} packet;

typedef enum { data, ack, nak } frame_kind;

typedef struct {
  frame_kind kind;
  seq_nr seq;
  seq_nr ack;
  packet info;
} frame;

typedef enum {
  frame_arrival,
  cksum_err,
  timeout,
  network_layer_ready
} event_type;

int frame_arrival_event = 0;
int cksum_err_event = 0;
int timeout_event = 0;
int network_layer_ready_event = 0;
int network_layer_enabled = 0;

// Wait for an event to happen; return its type in event.
void wait_for_event(event_type *event) {
  while (1) {
    if (frame_arrival_event) {
      *event = frame_arrival;
      pthread_mutex_lock(&event_lock);
      frame_arrival_event = 0;
      pthread_mutex_unlock(&event_lock);
      return;
    }
    if (cksum_err_event) {
      *event = cksum_err;
      pthread_mutex_lock(&event_lock);
      cksum_err_event = 0;
      pthread_mutex_unlock(&event_lock);
      return;
    }
    if (timeout_event) {
      *event = timeout;
      pthread_mutex_lock(&event_lock);
      timeout_event = 0;
      pthread_mutex_unlock(&event_lock);
      return;
    }
    if (network_layer_ready_event && network_layer_enabled) {
      *event = network_layer_ready;
      pthread_mutex_lock(&event_lock);
      network_layer_ready_event = 0;
      pthread_mutex_unlock(&event_lock);
      return;
    }
    usleep(1000);
  }
}

// Fetch a packet from the network layer for transmission on the channel.
packet *g_packet;
void *network_layer(void *) {
  while (1) {
    packet *p = (packet *)malloc(sizeof(packet));
    sleep(1);
    p->data[0] = 'a';
    p->data[0] = 'b';
    g_packet = p;
    pthread_mutex_lock(&event_lock);
    network_layer_ready_event = 1;
    pthread_mutex_unlock(&event_lock);
  }
}

void from_network_layer(packet *p) { p = g_packet; }

// Deliver information from an inbound frame to the network layer.
void to_network_layer(packet *p) {
  printf("network layer rececved: %s\n", p->data);
}

// Go get an inbound frame from the physical layer and copy it to r.
int send_fd;
int recv_fd;
void from_physical_layer(frame *r) {
  if (read(recv_fd, &r, sizeof(frame)) < 0)
    perror("from_physical_layer: read faild");
  printf("%d %d\n", r->seq, r->ack);
  printf("%s\n", "rececved from physical layer");
  printf("%lu\n", sizeof(r->info.data));
}

// Pass the frame to the physical layer for transmission,
void to_physical_layer(frame *s) {
  printf("%d %d\n", s->seq, s->ack);
  printf("%s\n", "write to physical layer");
  printf("%lu\n", sizeof(s->info.data));
  if (write(send_fd, s, sizeof(frame)) < 0)
    perror("to_physical_layer: write failed");
}

// Start the dock running and enable the timeout event,
void start_timer(seq_nr k) {
  // start timer
  sleep(5);
  pthread_mutex_lock(&event_lock);
  timeout_event = 1;
  pthread_mutex_unlock(&event_lock);
}

// Stop the clock and disable the timeout event.
void stop_timer(seq_nr k) {}

// Start an auxiliary timer and enable the ack timeout event.
void start_ack_timer(void) {}

// Stop the auxiliary timer and disable the ack timeout event.
void stop_ack_timer(void) {}

// Allow the network layer to cause a network layer ready event.
void enable_network_layer(void) {
  pthread_mutex_lock(&event_lock);
  network_layer_enabled = 1;
  pthread_mutex_unlock(&event_lock);
}

// Forbid the network layer from causing a network layer ready event.
void disable_network_layer(void) {
  pthread_mutex_lock(&event_lock);
  network_layer_enabled = 0;
  pthread_mutex_unlock(&event_lock);
}

// Macro inc is expanded in-line: increment k circularly.
#define inc(k)                                                                 \
  if (k < MAX_SEQ)                                                             \
    k = k + 1;                                                                 \
  else                                                                         \
    k = 0;

static bool between(seq_nr a, seq_nr b, seq_nr c) {
  // Return true if a <- b < c circularly; false otherwise.
  if ((a <= b && b < c) || (c < a && a <= b) || (b < c && c < a))
    return true;
  return false;
}

static void send_data(seq_nr frame_nr, seq_nr frame_expected, packet buffer[]) {
  // Send the frame with sequence number frame_nr.
  frame s;
  s.kind = data;
  s.seq = frame_nr;
  s.ack = (frame_expected + MAX_SEQ - 1) % MAX_SEQ;
  s.info = buffer[frame_nr];
  to_physical_layer(&s);
  start_timer(frame_nr);
}

// Go-Back-N Protocol allows multiple outstanding frames. The sender may
// transmit up to MAX_SEQ frames without waiting for an ack. In addition, the
// network layer is not assumed to have a new packet all the time. Instead, the
// network layer causes a network_layer_ready event when there is a packet to
// send.

void *go_back_n_protocol(void *) {
  seq_nr next_frame_to_send;
  seq_nr ack_expected;
  seq_nr frame_expected;
  frame r;
  packet buffer[MAX_SEQ + 1];
  seq_nr nbuffered;
  seq_nr i;
  event_type event;

  enable_network_layer();
  ack_expected = 0;
  next_frame_to_send = 0;
  frame_expected = 0;
  nbuffered = 0;

  while (1) {
    wait_for_event(&event);
    switch (event) {
    case frame_arrival:
      from_physical_layer(&r);

      if (r.seq == frame_expected) {
        to_network_layer(&r.info);
        inc(frame_expected);
      }
      while (between(ack_expected, frame_expected, r.ack)) {
        nbuffered--;
        stop_timer(ack_expected);
        inc(ack_expected);
      }
      break;

    case network_layer_ready:
      // Accept, save, and transmit a new frame.
      from_network_layer(&buffer[next_frame_to_send]);
      nbuffered++;
      frame_expected = (frame_expected + 1) % MAX_SEQ;
      send_data(next_frame_to_send, frame_expected, buffer);
      inc(next_frame_to_send);
      break;

    case cksum_err:
      break;

    case timeout:
      next_frame_to_send = ack_expected;
      for (int i = 0; i <= nbuffered; i++) {
        send_data(next_frame_to_send, frame_expected, buffer);
        inc(next_frame_to_send);
      }
      break;
    }

    if (nbuffered <= MAX_SEQ) {
      enable_network_layer();
    } else {
      disable_network_layer();
    }
  }
}

int main(int argc, char *argv[]) {
  int fd1[2];
  int fd2[2];

  pipe(fd1);
  pipe(fd2);

  if (fork() > 0) {
    close(fd1[1]);
    close(fd2[0]);
    send_fd = fd2[1];
    recv_fd = fd1[0];
  } else {
    close(fd1[0]);
    close(fd2[1]);
    send_fd = fd1[1];
    recv_fd = fd2[0];
  }

  if (pthread_mutex_init(&event_lock, NULL) != 0) {
    printf("\n mutex init has failed\n");
    return 1;
  }

  pthread_t data_link_layer_thread;
  pthread_t network_layer_thread;
  pthread_create(&data_link_layer_thread, NULL, &go_back_n_protocol, NULL);
  pthread_create(&network_layer_thread, NULL, &network_layer, NULL);

  pthread_join(data_link_layer_thread, NULL);
  pthread_join(network_layer_thread, NULL);
  return 0;
}
