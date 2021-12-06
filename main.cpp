#include <arpa/inet.h>
#include <fcntl.h>
#include <list>
#include <memory.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

pthread_mutex_t event_lock;
pthread_mutex_t timer_lock;

#define MAX_PKT 8
#define MAX_SEQ 7
#define MAX_TIMEOUT 5000

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

// typedef unsigned long long time_t;

int frame_arrival_event = 0;
int cksum_err_event = 0;
int timeout_event = 0;
int network_layer_ready_event = 0;
int network_layer_enabled = 0;

packet *received_packet;

int send_fd;
int recv_fd;

bool master;

char read_buffer[sizeof(frame)];
size_t read_bytes;

std::list<std::pair<seq_nr, time_t>> timers;

int n_sent_to_physical_layer = 0;
int n_received_from_physical_layer = 0;
int n_sent_to_network_layer = 0;
int n_received_from_network_layer = 0;
int n_total_timeouts = 0;
bool stats_printed = false;

// Retuens the current time in milliseconds
time_t get_time() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

// Check if any timer has expired
void check_timers() {
  time_t current_time = get_time();
  pthread_mutex_lock(&timer_lock);
  if (!timers.empty() && timers.front().second < current_time) {
    timers.clear();
    pthread_mutex_lock(&event_lock);
    timeout_event = 1;
    pthread_mutex_unlock(&event_lock);
  }
  pthread_mutex_unlock(&timer_lock);
}

// Start the dock running and enable the timeout event,
void start_timer(seq_nr k) {
  pthread_mutex_lock(&timer_lock);
  timers.push_back(std::make_pair(k, get_time() + MAX_TIMEOUT));
  pthread_mutex_unlock(&timer_lock);
}

// Stop the clock and disable the timeout event.
void stop_timer(seq_nr k) {
  pthread_mutex_lock(&timer_lock);
  for (auto it = timers.begin(); it != timers.end(); it++)
    if (it->first == k) {
      timers.erase(it);
      break;
    }
  pthread_mutex_unlock(&timer_lock);
}

// Wait for an event to happen; return its type in event.
void wait_for_event(event_type *event) {
  while (1) {
    check_timers();
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
    
    // usleep(1000);
  }
}

// Fetch a packet from the network layer for transmission on the channel.
void *network_layer(void *) {
  while (1) {
    sleep(1);

    if (network_layer_enabled) {
      packet *p = (packet *)malloc(sizeof(packet));
      for (int i = 0; i < MAX_PKT - 1; i++) {
        p->data[i] = 'a' + (rand() % (('z' - 'a') + 1));
      }
      p->data[MAX_PKT - 1] = '\0';
      received_packet = p;

      pthread_mutex_lock(&event_lock);
      network_layer_ready_event = 1;
      pthread_mutex_unlock(&event_lock);
    }
  }
}

void from_network_layer(packet *p) {
  *p = *received_packet;
  printf("network layer sent packet: %s\n", p->data);
  fflush(stdout);
}

// Deliver information from an inbound frame to the network layer.
void to_network_layer(packet *p) {
  printf("network layer received packet: %s\n", p->data);
  fflush(stdout);
}

void *physical_layer(void *) {
  while (1) {
    int retcode;
    while ((retcode = read(recv_fd, read_buffer + read_bytes,
                           sizeof(frame) - read_bytes)) > 0)
      read_bytes += retcode;

    if (read_bytes == sizeof(frame)) {
      read_bytes = 0;
      // 90% chance of getting a frame
      if ((rand() % 100) < 90) {
        pthread_mutex_lock(&event_lock);
        frame_arrival_event = 1;
        pthread_mutex_unlock(&event_lock);
      }
    }
  }
}

// Go get an inbound frame from the physical layer and copy it to r.
void from_physical_layer(frame *r) {
  memcpy(r, read_buffer, sizeof(frame));

  printf("frame #%d received from physical layer with ack #%d: %s\n", r->seq,
         r->ack, r->info.data);

  fflush(stdout);
}

// Pass the frame to the physical layer for transmission,
void to_physical_layer(frame *r) {
  if (write(send_fd, r, sizeof(frame)) < 0)
    perror("to_physical_layer: write failed");

  printf("frame #%d sent to physical layer with ack #%d: %s\n", r->seq, r->ack,
         r->info.data);
  fflush(stdout);
}

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
  s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
  s.info = buffer[frame_nr];
  to_physical_layer(&s);
  n_sent_to_physical_layer++;
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
      n_received_from_physical_layer++;

      if (r.seq == frame_expected) {
        to_network_layer(&r.info);
        inc(frame_expected);
        n_sent_to_network_layer++;
      }
      while (between(ack_expected, r.ack, next_frame_to_send)) {
        nbuffered--;
        stop_timer(ack_expected);
        inc(ack_expected);
      }
      break;

    case network_layer_ready:
      // Accept, save, and transmit a new frame.
      from_network_layer(&buffer[next_frame_to_send]);
      n_received_from_network_layer++;

      nbuffered++;
      send_data(next_frame_to_send, frame_expected, buffer);
      inc(next_frame_to_send);
      break;

    case cksum_err:
      break;

    case timeout:
      printf("timeout: resending from frame #%d\n", ack_expected);
      n_total_timeouts++;
      next_frame_to_send = ack_expected;
      for (int i = 1; i <= nbuffered; i++) {
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

void print_stats() {
  printf("\tTotal data frames sent to physical layer:       %9d\n",
         n_sent_to_physical_layer);
  printf("\tTotal data frames received from physical layer: %9d\n",
         n_received_from_physical_layer);
  printf("\tTotal data frames sent to network layer:        %9d\n",
         n_sent_to_network_layer);
  printf("\tTotal data frames received from network layer:  %9d\n",
         n_received_from_network_layer);
  printf("\tTimeouts:                                       %9d\n",
         n_total_timeouts);
}

void exit_handler(int signum) {
  if (stats_printed == false) {
    stats_printed = true;
    printf("%s", "\n");
    print_stats();
    printf("%s", "\n");
  }
  exit(signum);
}

int main(int argc, char *argv[]) {
  srand(time(NULL));
  master = strcmp(argv[1], "master") ? true : false;
 
  // Set up signal handlers.
  for (int i = 1; i < _NSIG; i++) {
    signal(i, exit_handler);
  }

  mkfifo("/tmp/go-back-n-fifo-0", S_IFIFO | 0640);
  mkfifo("/tmp/go-back-n-fifo-1", S_IFIFO | 0640);

  if (master) {
    send_fd = open("/tmp/go-back-n-fifo-0", O_WRONLY);
    if (send_fd == -1) {
      perror("Error opening write FD on Master");
      return 1;
    }
    recv_fd = open("/tmp/go-back-n-fifo-1", O_RDONLY);
    if (recv_fd == -1) {
      perror("Error opening read FD on Master");
      return 1;
    }
  } else {
    recv_fd = open("/tmp/go-back-n-fifo-0", O_RDONLY);
    if (recv_fd == -1) {
      perror("Error opening read FD on Slave");
      return 1;
    }
    send_fd = open("/tmp/go-back-n-fifo-1", O_WRONLY);
    if (send_fd == -1) {
      perror("Error opening write FD on Slave");
      return 1;
    }
  }

  if (pthread_mutex_init(&event_lock, NULL) != 0) {
    perror("\n mutex init has failed\n");
    return 1;
  }
  if (pthread_mutex_init(&timer_lock, NULL) != 0) {
    perror("\n mutex init has failed\n");
    return 1;
  }

  pthread_t data_link_layer_thread;
  pthread_t network_layer_thread;
  pthread_t physical_layer_thread;
  pthread_create(&data_link_layer_thread, NULL, &go_back_n_protocol, NULL);
  pthread_create(&network_layer_thread, NULL, &network_layer, NULL);
  pthread_create(&physical_layer_thread, NULL, &physical_layer, NULL);

  pthread_join(data_link_layer_thread, NULL);
  pthread_join(network_layer_thread, NULL);
  pthread_join(physical_layer_thread, NULL);

  if (stats_printed == false) {
    stats_printed = true;
    printf("%s", "\n");
    print_stats();
    printf("%s", "\n");
  }
  return 0;
}
