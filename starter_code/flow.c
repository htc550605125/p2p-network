//
// Created by htc on 15-6-28.
//

#include <stddef.h>
#include <unistd.h>
#include <time.h>
#include "flow.h"
#include "uthash.h"
#include "timeout.h"
#include "debug.h"
#include "peer.h"
#include "utlist.h"
#include "data.h"

#define TIMEOUT 400

static int flow_id = 0;

static struct task_recv_list {
  struct flow_task *task;
  struct task_recv_list *next, *prev;
};

static struct task_map {
  int peer_id;

  struct flow_task *cur_send_task;
  struct flow_task *cur_recv_task;
  struct task_recv_list *pending_recv_tasks;

  UT_hash_handle hh;
} *peer_tasks = NULL;

struct data_cache {
  size_t len;
  void* data[];
};

void next_recv_task(int id);

static struct timespec start_time;

void flow_init() {
  struct bt_peer_s *peer = peer_config.peers;
  for (; peer; peer = peer->next) {
    if (peer->id == peer_config.identity) {
      continue;
    }

    struct task_map *tm = malloc(sizeof (struct task_map));
    memset(tm, 0, sizeof(struct task_map));
    tm->peer_id = peer->id;
    HASH_ADD_INT(peer_tasks, peer_id, tm);
  }

  clock_gettime(CLOCK_MONOTONIC, &start_time);
}

void trace_window_size(struct flow_task *task) {
  if (task->last_window_size == task->window_size) {
    return;
  }
  task->last_window_size = task->window_size;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long long msec = (now.tv_sec - start_time.tv_sec) * 1000;
  msec += (now.tv_nsec - start_time.tv_nsec) / 1000 / 1000;

  FILE* f = fopen("problem2-peer.txt", "a");
  fprintf(f, "f%d\t%lld\t%d\n", task->flow_id, msec, task->window_size);
  fclose(f);
  printf("f%d\t%lld\t%d\n", task->flow_id, msec, task->window_size);
}

void ack_timeout(struct flow_task *task) {
  struct flow_packet *packet = task->send_window;

  ++task->timeout_cnt;
  if (task->timeout_cnt > 8) {
    DPRINTF(DEBUG_SOCKETS, "SEND FAILED: timeout\n");
    struct task_map *tm;
    HASH_FIND_INT(peer_tasks, &task->peer_id, tm);
    tm->cur_send_task = NULL;

    while (packet) {
      task->send_window = packet->next;
      free(packet);
      packet = task->send_window;
    }

    if (task->fail) {
      task->fail(task);
    }
    return;
  }

  task->window_size = 1;
  task->ack_sum = 0;
  if (task->window_size < 1) {
    task->window_size = 1;
  }
  trace_window_size(task);

  send_packet(task->peer_id, PACKET_DATA, packet->seq, -1, task->data + packet->pos, packet->data_len);
  task->timeout_id = timeout_register(TIMEOUT, (void (*)(void *)) ack_timeout, task);
}

void new_upload_task(struct flow_task* task) {
  task->flow_id = flow_id++;
  task->type = FLOW_SEND;
  task->last_window_size = -1;
  task->ssthresh = 8;
  task->rtt = 8;
  task->ack_sum = 0;
  task->ack = 1;
  task->pos = 0;
  task->timeout_id = -1;

  struct task_map *tm;
  HASH_FIND_INT(peer_tasks, &task->peer_id, tm);

  if (tm->cur_send_task) {
    send_packet(task->peer_id, PACKET_DENIED, -1, -1, NULL, 0);
    if (task->fail) {
      task->fail(task);
    }
    return;
  }

  struct flow_packet *packet = malloc(sizeof(struct flow_packet));
  packet->data_len = 0;
  packet->pos = 0;
  packet->seq = 1;
  packet->next = NULL;

  task->send_window = packet;
  task->send_window_tail = packet;
  task->send_window_size = 1;
  task->window_size = 1;
  task->dup_ack = 0;

  trace_window_size(task);

  tm->cur_send_task = task;
  send_packet(task->peer_id, PACKET_DATA, 1, -1, NULL, 0);
  task->timeout_id = timeout_register(TIMEOUT, (void (*)(void *)) ack_timeout, task);
}

void handle_ack(struct packet_header *header, struct flow_task *task) {
  if (task->type == FLOW_RECV) {
    return;
  }

  if (header->ack < task->send_window->seq) {
    return;
  }

  struct flow_packet *packet = task->send_window;
  if (header->ack == task->send_window->seq) {
    ++task->dup_ack;
    if (task->dup_ack >= 3) {
      // fast retransmit
      send_packet(task->peer_id, PACKET_DATA, packet->seq, -1, task->data + packet->pos, packet->data_len);
      task->window_size = 1;
      task->ack_sum = 0;
      task->dup_ack = -1;
    }
  } else {
    task->dup_ack = 0;
  }


  // ACK packet
  if (packet && header->ack > packet->seq && task->pos < BT_CHUNK_SIZE) {
    task->rtt = task->rtt * 0.75f + (task->send_window_tail->seq - packet->seq) * 0.25f;

    if (task->window_size < task->ssthresh) {
      ++task->window_size;
    } else {
      ++task->ack_sum;
      if (task->ack_sum > task->rtt) {
        task->ack_sum = 0;
        ++task->window_size;
        task->ssthresh = task->window_size / 2;
      }
    }
  }
  while (packet && header->ack > packet->seq) {
    --task->send_window_size;
    task->send_window = packet->next;
    free(packet);
    packet = task->send_window;
  }

  // Cancel timer
  if (task->timeout_id != -1) {
    timeout_cancel(task->timeout_id);
    task->timeout_id = -1;
  }

  // Check if we have done
  if (packet == NULL && task->pos == BT_CHUNK_SIZE) {
    DPRINTF(DEBUG_SOCKETS, "SEND DONE\n");
    struct task_map *tm;
    HASH_FIND_INT(peer_tasks, &task->peer_id, tm);
    tm->cur_send_task = NULL;

    if (task->done) {
      task->done(task);
    }
    return;
  }

  if (task->window_size < 1) {
    task->window_size = 1;
  }
  trace_window_size(task);

  // Send more packets
  while (task->send_window_size < task->window_size && task->pos != BT_CHUNK_SIZE) {
    packet = malloc(sizeof(struct flow_packet));
    packet->seq = ++task->ack;
    packet->pos = task->pos;
    packet->data_len = 1432;
    if (task->pos + packet->data_len > BT_CHUNK_SIZE) {
      packet->data_len = BT_CHUNK_SIZE - task->pos;
    }
    task->pos += packet->data_len;

    send_packet(task->peer_id, PACKET_DATA, packet->seq, -1, task->data + packet->pos, packet->data_len);

    packet->next = NULL;
    if (task->send_window == NULL) {
      task->send_window = packet;
    } else {
      task->send_window_tail->next = packet;
    }
    task->send_window_tail = packet;
    ++task->send_window_size;
  }

  // Register timeout
  task->timeout_cnt = 0;
  task->timeout_id = timeout_register(TIMEOUT, (void (*)(void *)) ack_timeout, task);
}

void data_timeout(struct flow_task *task) {
  ++task->timeout_cnt;
  if (task->timeout_cnt > 8) {
    DPRINTF(DEBUG_SOCKETS, "RECV FAIL\n");
    while (priq_size(task->recv_window)) {
      struct data_cache *cache = priq_pop(task->recv_window, NULL);
      free(cache);
    }
    priq_free(task->recv_window);
    struct task_map *tm;
    HASH_FIND_INT(peer_tasks, &task->peer_id, tm);
    tm->cur_recv_task = NULL;

    if (task->fail) {
      task->fail(task);
    }

    next_recv_task(task->peer_id);
    return;
  }

  if (task->ack == 1) {
    send_packet(task->peer_id, PACKET_GET, 1, -1, task->data, SHA1_HASH_SIZE);
  } else {
    send_packet(task->peer_id, PACKET_ACK, -1, task->ack, NULL, 0);
  }

  task->timeout_id = timeout_register(TIMEOUT, (void (*)(void *)) data_timeout, task);
}

void next_recv_task(int peer_id) {
  DPRINTF(DEBUG_SOCKETS, "NEXT RECV TASK\n");
  struct task_map *tm;
  HASH_FIND_INT(peer_tasks, &peer_id, tm);
  if (tm->cur_recv_task != NULL) {
    DPRINTF(DEBUG_SOCKETS, "REFUSE TO DO NEXT RECV TASK DUE TO CURRENT TASK\n");
    return;
  }
  if (tm->pending_recv_tasks == NULL) {
    DPRINTF(DEBUG_SOCKETS, "NOTHING TO DO\n");
    return;
  }

  struct task_recv_list *node = tm->pending_recv_tasks;
  struct flow_task *task = node->task;
  DL_DELETE(tm->pending_recv_tasks, node);
  free(node);

  tm->cur_recv_task = task;
  send_packet(task->peer_id, PACKET_GET, 1, -1, task->data, SHA1_HASH_SIZE);

  task->timeout_cnt = 0;
  task->timeout_id = timeout_register(TIMEOUT, (void (*)(void *)) data_timeout, task);
  DPRINTF(DEBUG_SOCKETS, "SET NEXT TASK\n");
}

void handle_data(struct packet_header *header, struct flow_task *task, void *data) {
  size_t data_len = header->total_len - header->header_len;

  if (header->seq == task->ack) {
    task->ack = header->seq + 1;
    memcpy(task->data + task->pos, data, data_len);
    task->pos += data_len;

    while (!priq_size(task->recv_window)) {
      int64_t seq;

      priq_top(task->recv_window, &seq);
      if (seq < task->ack) {
        priq_pop(task->recv_window, NULL);
        continue;
      }

      if (seq != task->ack) {
        break;
      }

      ++task->ack;
      struct data_cache *cache = priq_pop(task->recv_window, NULL);
      memcpy(task->data + task->pos, cache->data, cache->len);
      task->pos += data_len;
      free(cache);
    }

  } else {
    struct data_cache *cache = malloc(sizeof(size_t) + data_len);
    cache->len = data_len;
    memcpy(cache->data, data, data_len);
    priq_push(task->recv_window, cache, header->seq);

    // fast retransmit
    send_packet(task->peer_id, PACKET_ACK, -1, task->ack, NULL, 0);
  }

  send_packet(task->peer_id, PACKET_ACK, -1, task->ack, NULL, 0);

  // Cancel timer
  if (task->timeout_id != -1) {
    timeout_cancel(task->timeout_id);
    task->timeout_id = -1;
  }

  // Receive done
  if (task->pos == BT_CHUNK_SIZE) {
    DPRINTF(DEBUG_SOCKETS, "RECV DONE\n");
    priq_free(task->recv_window);

    struct task_map *tm;
    HASH_FIND_INT(peer_tasks, &task->peer_id, tm);
    tm->cur_recv_task = NULL;

    if (task->done) {
      task->done(task);
    }

    next_recv_task(task->peer_id);
    return;
  }

  task->timeout_cnt = 0;
  task->timeout_id = timeout_register(TIMEOUT, (void (*)(void *)) data_timeout, task);
}


void new_download_task(char *hash, struct flow_task *task) {
  DPRINTF(DEBUG_SOCKETS, "TRY TO LOAD FROM CACHE\n");
  int ret = data_load_chunk(hash, task->data);
  if (ret == 0) {
    DPRINTF(DEBUG_SOCKETS, "LOAD FROM CACHE\n");
    if (task->done) {
      task->done(task);
    }
    return;
  }

  task->flow_id = flow_id++;
  task->type = FLOW_RECV;
  task->recv_window = priq_new(8);
  task->ack = 1;
  task->pos = 0;
  task->timeout_id = -1;
  memcpy(task->data, hash, SHA1_HASH_SIZE);

  struct task_map *tm;
  HASH_FIND_INT(peer_tasks, &task->peer_id, tm);

  struct task_recv_list *node = malloc(sizeof(struct task_recv_list));
  node->task = task;
  DL_APPEND(tm->pending_recv_tasks, node);

  DPRINTF(DEBUG_SOCKETS, "%x\n", tm->cur_recv_task);
  if (tm->cur_recv_task == NULL) {
    next_recv_task(task->peer_id);
  }
}

void handle_denied(struct packet_header *header, struct flow_task *task) {
  DPRINTF(DEBUG_SOCKETS, "task failed\n");
  struct task_map *tm;
  HASH_FIND_INT(peer_tasks, &task->peer_id, tm);
  tm->cur_recv_task = NULL;

  priq_free(task->recv_window);

  if (task->fail) {
    task->fail(task);
  }
}

void new_packet(int peer_id, struct packet_header *header, char* data) {
  struct task_map *tm;
  HASH_FIND_INT(peer_tasks, &peer_id, tm);

  switch (header->type) {
    case PACKET_ACK:
      if (!tm->cur_send_task) {
        DPRINTF(DEBUG_SOCKETS, "Unexcepted ACK packet\n");
        break;
      }
      handle_ack(header, tm->cur_send_task);
      break;

    case PACKET_DATA:
      if (!tm->cur_recv_task) {
        DPRINTF(DEBUG_SOCKETS, "Unexcepted DATA packet\n");
        break;
      }
      handle_data(header, tm->cur_recv_task, data);
      break;

    case PACKET_DENIED:
      if (!tm->cur_recv_task) {
        DPRINTF(DEBUG_SOCKETS, "Unexcepted DENIED packet\n");
        break;
      }
      handle_denied(header, tm->cur_recv_task);
      break;

    default:
      break;
  }
}

