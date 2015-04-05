
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

struct inflight_pckt {
  struct inflight_pckt* next;
  struct inflight_pckt* prev;

  int seq;
  int size;
  char data[MAX_DATA_LEN];
  int ack_timer;
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
  int LFS;             // last frame sent. not plus 1 (curr seq num)

  flight_t* curr_win_head; // current head of in-flight packet window ('oldest' un-acked packet)
  flight_t* curr_win_tail; // current tail of in-flight packet window (last sent packet)

  int num_inflight_packets;

  int LAF;             // largest acceptable frame
  int LFR;             // last frame received

  //need some sort of EOF

};
rel_t *rel_list;





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
  r -> window_size = cc -> window;
  r -> timeout = cc -> timeout;
  r -> LAR = 1;
  r -> LAF = r -> window_size + 1;
  //r -> 

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
}


void rel_read (rel_t *s)
{
  while (s->num_inflight_packets < s->window_size) { // TODO: Add EOF condition
    s->curr_win_tail->next = (flight_t*)malloc(sizeof(flight_t*));
    s->curr_win_tail = s->curr_win_tail->next;
    int data_size = conn_input(s->c, s->curr_win_tail->data, MAX_DATA_LEN);
    if (data_size > 0) {
      s->num_inflight_packets++;
      s->LFS++;
      s->curr_win_tail->seq = s->LFS;
      s->curr_win_tail->size = data_size;
      s->curr_win_tail->ack_timer = 0;
      //send_pkt(s, s->curr_win_tail);
    } else if (data_size < 0){
      s->num_inflight_packets++;
      s->LFS++;
      //send(s, ) pointer to a flight_t with seqno -1, data = eof something
    } else {
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

}
