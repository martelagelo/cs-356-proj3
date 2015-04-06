
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define MAX_DATA_LEN 500
#define DATA_HEADER_LEN 12
#define ACK_HEADER_LEN 8

struct inflight_pckt {
  struct inflight_pckt* next;
  struct inflight_pckt* prev;

  int seq;
  int size;
  char data[MAX_DATA_LEN];
  int ack_timer;
  int is_acked;
};
typedef struct inflight_pckt flight_t;

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */
 
  /* Add your own data fields below this */
  int window_size;     // Number of packets in flight
  int timeout;        // Time until another transmission needed: milliseconds

  int LAR;             // last ack received - (next expected seq num to recv)
  int LFS;             // last frame sent.

  flight_t* curr_win_head; // current head of in-flight packet window ('oldest' un-acked packet)
  flight_t* curr_win_tail; // current tail of in-flight packet window (last sent packet)

  int num_inflight_packets;

  int LAF;     // largest acceptable frame              
  int LFR;             // last frame received

  flight_t *eof_packet; 
  flight_t *ack_packet;

  //need some sort of EOF

};
rel_t *rel_list;

void send_pkt (rel_t * r, flight_t * content) {
  packet_t pckt;
  memset(pckt.data, '\0', 500);

  if (content->ack_timer >= 0) { // is a data packet
    pckt.len = htons(DATA_HEADER_LEN + content->size);
    pckt.seqno = htonl(content->seq);
    memcpy(pckt.data, content->data, content->size);
  } else if (content->size == -1) { // is an EOF packet
    pckt.len = htons(DATA_HEADER_LEN);
    pckt.seqno = htonl(content->seq);
    memset(pckt.data, '\0', MAX_DATA_LEN);
  } else { // is an ACK packet
    pckt.len = htons(ACK_HEADER_LEN);
  }

  pckt.ackno = htonl(r->LFR+1);
  pckt.cksum = 0;
  pckt.cksum = cksum (&pckt, ntohs(pckt.len));
  // TODO: mark recv_buffer[LFR] as acked;

  content->ack_timer = 0;

  conn_sendpkt (r->c, &pckt, ntohs(pckt.len));
}

int check_cksum(packet_t *pkt, size_t n) {
  uint16_t cs = pkt->cksum;
  pkt->cksum = 0;
  pkt->cksum = cksum(pkt, n);
  if(cs == pkt->cksum)
  {
    return 1;
  }
  return 0;
}

/* Creates a new reliable protocol ion, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* Do any other initialization you need here */
  r->window_size = cc->window;
  r->timeout = cc->timeout;
  r->LAR = 0;
  r->LAF = r->window_size + 1;
  r->LFS = 0;
  r->LFR = 0;

  r->eof_packet = (flight_t *)malloc(sizeof(flight_t *)); 
  r->eof_packet->ack_timer = -1;
  r->eof_packet->size = -1;
  r->eof_packet->is_acked = 0;

  r->ack_packet = (flight_t *)malloc(sizeof(flight_t *));
  r->ack_packet->ack_timer = -1;
  r->ack_packet->size = -2;

  return r;
}

void rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */
  free(r);
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
}

void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  if (
    n < ACK_HEADER_LEN || // too short
    (n > ACK_HEADER_LEN && n < DATA_HEADER_LEN) || // impossible
    n != ntohs(pkt->len) || // wrong packet length
    check_cksum(pkt, n) == 0 // bad checksum
  ) {
    //send_pkt(r, r->ack_packet);
    return;
  } else {
    flight_t * p;
    for (p = r->curr_win_head; p != NULL; p=p->next) {
      if (p->seq == (ntohl(pkt->ackno) - 1)) {
        p->is_acked = 1;
        break;
      }
    }
    for (p = r->curr_win_head; p != NULL; p=p->next) {
      if (p->is_acked == 0) {
        break;
      } else {
        r->curr_win_head = p->next;
        r->num_inflight_packets--;
        if (r->curr_win_head != NULL) {
          free(r->curr_win_head->prev);
        } else {
          break;
        }
      }
    }
  }
}
      // if (p->is_acked == 1) {
      //   r->curr_win_head = p;
      //   free(p->prev);
      //   r->curr_win_head->prev = NULL;
      // } else {
      //   r->curr_win_head = p->next;
      //   free(p);
      //   r->curr_win_head->prev = NULL;
      //   break; // stop as soon as an unacked packet is reached
      // }

void rel_read (rel_t *s)
{
	printf("Num Inflight packets: %d\n Window Size: %d\n", s->num_inflight_packets, s->window_size);
  
  while (s->num_inflight_packets < s->window_size) { // TODO: Add EOF condition
    if (s->curr_win_head) {
      s->curr_win_tail->next = (flight_t*)malloc(sizeof(flight_t*));
      s->curr_win_tail = s->curr_win_tail->next;
    } else {
      s->curr_win_tail = (flight_t*)malloc(sizeof(flight_t*));
      s->curr_win_head = s->curr_win_tail;
    }
    int data_size = conn_input(s->c, s->curr_win_tail->data, MAX_DATA_LEN);
    if (data_size > 0) { // is data packet
      s->num_inflight_packets++;
        // SHOULD equal LFS - LAR
      s->LFS++;
      s->curr_win_tail->seq = s->LFS;
      s->curr_win_tail->size = data_size;
      s->curr_win_tail->ack_timer = 0;
      s->curr_win_tail->is_acked = 0;
      send_pkt(s, s->curr_win_tail);
    } else if (data_size < 0){ // is EOF
      s->num_inflight_packets++;
        // SHOULD equal LFS - LAR
      s->LFS++;
      send_pkt(s, s->eof_packet);
    } else { // IS other
      return;
    }
  }
  return;
}

void rel_output (rel_t *r)
{
}

void rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */
  rel_t * s;
  flight_t * p;
  for (s = rel_list; s != NULL; s = s->next) {
    for (p = s->curr_win_head; p != NULL; p = p->next) {
      p->ack_timer += s->timeout * 0.2;
      if (p->ack_timer >= s->timeout) {
        send_pkt(s, p);
      }
    }
  }
}
