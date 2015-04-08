
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
  int eof;
  int remote_eof;

};
rel_t *rel_list;

void send_pkt (rel_t * r, flight_t * content) {
  packet_t pckt;
  memset(pckt.data, '\0', 500);

  if (content->ack_timer >= 0) { // is a data packet
    pckt.len = htons(DATA_HEADER_LEN + content->size);
    pckt.seqno = htonl(content->seq);
    memcpy(pckt.data, content->data, content->size);
    content->ack_timer = 0;
    fprintf(stderr, "Sending SeqNum: %u", content->seq);
  } else if (content->size == -1) { // is an EOF packet
    pckt.len = htons(DATA_HEADER_LEN);
    pckt.seqno = htonl(content->seq);
    memset(pckt.data, '\0', MAX_DATA_LEN);
    content->ack_timer = 0;
    fprintf(stderr, "Sending SeqNum: %u", content->seq);
  } else { // is an ACK packet
    pckt.len = htons(ACK_HEADER_LEN);
  }

  pckt.ackno = htonl(r->LFR+1);
  pckt.cksum = 0;
  pckt.cksum = cksum (&pckt, ntohs(pckt.len));
  // TODO: mark recv_buffer[LFR] as acked;
  conn_sendpkt (r->c, &pckt, ntohs(pckt.len));
}

int check_sum(packet_t *pkt, int size) {
  int cs = pkt->cksum;
  pkt->cksum = 0;
  pkt->cksum = cksum(pkt, size);
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
  r->eof = 0;
  r->remote_eof = 0;

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
  // TODO: if curr win head undefined add dummy value
  if (
    n < ACK_HEADER_LEN || // too short
    (n > ACK_HEADER_LEN && n < DATA_HEADER_LEN) || // impossible
    n != ntohs(pkt->len) || // wrong packet length
    check_sum(pkt, n) == 0 // bad checksum
  ) {
    //send_pkt(r, r->ack_packet);
    return;
  } else {
    flight_t * p;
    for (p = r->curr_win_head; p != NULL; p=p->next) { // mark packet in sent buffer w/ matching seq num as Acked
      if (p->seq == (ntohl(pkt->ackno) - 1)) {
        p->is_acked = 1;
        break;
      }
    }
    if (ntohs(pkt->len) == ACK_HEADER_LEN) { // is an ack
      for (p = r->curr_win_head; p != NULL; p=p->next) { // remove the first contigous block of acked things in sent buffer
        if (p->is_acked == 0) {
          break;
        } else {
          r->curr_win_head = p->next;
          r->num_inflight_packets--;
          if (r->curr_win_head != NULL) {
            if(r->curr_win_head->prev != NULL) {
            	free(r->curr_win_head->prev);	
            }
          } else {
            break;
          }
        }
      }
      //rel_read(r);
    } else { // is data
      if(ntohs(pkt->len) == DATA_HEADER_LEN) {
        r->remote_eof = 1;
      }
      if (ntohl(pkt->seqno) == (r->LFR + 1) && (conn_bufspace(r->c) >= ntohs(pkt->len)) ) { // this is the expected packet
        r->LFR++;
        if(r->remote_eof && r->eof) {
          rel_destroy(r);
        }
        conn_output(r->c, pkt->data, ntohs(pkt->len) - DATA_HEADER_LEN);
      }
      send_pkt(r, r->ack_packet);
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
  
  while (s->num_inflight_packets < s->window_size && !(s->eof)) {
    if (s->curr_win_head) {
      s->curr_win_tail->next = (flight_t*)malloc(sizeof(flight_t*));
      s->curr_win_tail->prev = s->curr_win_tail; 
      s->curr_win_tail = s->curr_win_tail->next;

    } else {
      s->curr_win_tail = (flight_t*)malloc(sizeof(flight_t*));
      s->curr_win_head = s->curr_win_tail;
      s->curr_win_tail->prev = NULL;
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
      s->eof = 1;
      s->eof_packet->seq = s->LFS;
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
      	printf("Timeout\n");
        send_pkt(s, p);
      }
    }
  }
}
