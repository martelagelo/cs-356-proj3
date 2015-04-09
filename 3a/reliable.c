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

#define DATA_HEADER_LEN 12
#define ACK_HEADER_LEN  8
#define MAX_DATA_LEN 500


struct inflight_pckt {
  struct inflight_pckt * next;
  struct inflight_pckt * prev;

  int seq;
  int size;
  char data[MAX_DATA_LEN];
  int ack_timer;
  int is_acked;
};
typedef struct inflight_pckt flight_t;

struct reliable_state {
  rel_t *next;      /* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;        /* This is the connection object */

  /* Add your own data fields below this */

int win_size;
int timeout;

flight_t * curr_win_head;
flight_t * curr_win_tail;

flight_t * ack_packet;

int LFS;  //Largest Frame Same
int LAR; // Last ack recieved

int LFR; //Last Frame recieved
int LAF; //upper edge of reciever's edge + 1

int num_inflight_packets;

int remote_eof;
int eof;
};
rel_t *rel_list;

int compare_csum(packet_t *pkt, int n) {
  int check = pkt->cksum;
  pkt->cksum = 0;
  pkt->cksum = cksum (pkt, n);
  if(check == pkt->cksum) {
    return 1;
  }
  return 0;
}

flight_t* get_eof_packet(int seqno) {
  flight_t* eof_packet = (flight_t *)malloc(sizeof(flight_t)); 
  eof_packet->ack_timer = -1;
  eof_packet->size = -1;
  eof_packet->is_acked = 0;
  eof_packet->seq = seqno;
  return eof_packet;
}

void send_packet (rel_t * r, flight_t * content) {
  packet_t pckt;
  memset(pckt.data, '\0', 500);

  if (content->ack_timer >= 0) { // is a data packet
    pckt.len = htons(DATA_HEADER_LEN + content->size);
    pckt.seqno = htonl(content->seq);
    memcpy(pckt.data, content->data, content->size);
    content->ack_timer = 0;
  } else if (content->size == -1) { // is an EOF packet
    pckt.len = htons(DATA_HEADER_LEN);
    pckt.seqno = htonl(content->seq);
    memset(pckt.data, '\0', MAX_DATA_LEN);
    content->ack_timer = 0;
  } else { // is an ACK packet
    pckt.len = htons(ACK_HEADER_LEN);
  }

  pckt.ackno = htonl(r->LFR+1);
  pckt.cksum = 0;
  pckt.cksum = cksum (&pckt, ntohs(pckt.len));
  conn_sendpkt (r->c, &pckt, ntohs(pckt.len));
}

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss,
      const struct config_common *cc) {
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
  
  r->win_size = cc->window;
  r->timeout = cc->timeout;

  r->LFS = 0;
  r->LAR = 0;
  r->num_inflight_packets = 0;

  r->remote_eof = 0;
  r->eof = 0;

  r->LFR = 0;
  r->LAF = r->win_size + 1;

  r->ack_packet = (flight_t *)malloc(sizeof(flight_t));
  r->ack_packet->ack_timer = -1;
  r->ack_packet->size = -2;

  return r;
}

void rel_destroy (rel_t *r) {
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
     packet_t *pkt, size_t len) {
}

void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
  if(n < 8 || (n > 8 && n < 12 ) || n != ntohs(pkt->len) || compare_csum(pkt, n) == 0) {
    return;
  } else {  
    //ack
    flight_t * p = r->curr_win_head;
    while(ntohl(pkt->ackno) - 1 >=  r->LAR+1 && ntohl(pkt->ackno) - 1 < r->LFS + 1) {
        r->num_inflight_packets--;
        r->LAR++;
        for(; p != NULL; p = p->next) {
          if(p->seq == r->LAR+1) {
            break;
          }
        }
        if(p == NULL) {
          continue;
        }
        p->is_acked = 1;
        if(r->remote_eof == 1 && r->eof == 1 && p->seq == r->LFS) {
          rel_destroy (r);
        }    
      
      while(r->curr_win_head != NULL) { // remove the first contigous block of acked things in sent buffer
        if (r->curr_win_head->is_acked == 0) {
          break;
        } else {
          flight_t *del = r->curr_win_head;
          r->curr_win_head = r->curr_win_head->next;
          free(del); 
        } 
      }  
    }

    //non ack
    if(n >= DATA_HEADER_LEN) {
      send_packet(r, r->ack_packet);
      if(ntohl(pkt->seqno) >= r->LFR + 1 && ntohl(pkt->seqno) < r->LAF && 
        (conn_bufspace(r->c) >= ntohs(pkt->len))) {
        r->LFR++;
        r->LAF++;
        conn_output(r->c, pkt->data, ntohs(pkt->len) - DATA_HEADER_LEN);
        send_packet(r, r->ack_packet);
      }
    }
    rel_read (r);
  }
}

void rel_read (rel_t *s) {
  while(s->num_inflight_packets < s->win_size && !s->eof) {
    if (s->curr_win_head) {
      s->curr_win_tail->next = (flight_t*)malloc(sizeof(flight_t));
      s->curr_win_tail->next->prev = s->curr_win_tail;
      s->curr_win_tail = s->curr_win_tail->next;
    } else {
      s->curr_win_tail = (flight_t*)malloc(sizeof(flight_t));
      s->curr_win_head = s->curr_win_tail;
      s->curr_win_head->prev = NULL;
      s->curr_win_head->next = NULL;
    }
    int data_size = conn_input(s->c, s->curr_win_tail->data, MAX_DATA_LEN);
    if (data_size > 0) { //Read from STDIN
      s->num_inflight_packets++;
      s->LFS++;
      s->curr_win_tail->seq = s->LFS;
      s->curr_win_tail->size = data_size;
      s->curr_win_tail->ack_timer = 0;
      s->curr_win_tail->is_acked = 0;

      send_packet(s, s->curr_win_tail);
      
    } else if(data_size < 0) {//EOF
      s->num_inflight_packets++;
      s->LFS++;
      s->eof = 1;
      flight_t * eof_pkt = get_eof_packet(s->LFS);
      send_packet(s, eof_pkt);
    } else {
      return;
    }
  }
  return;
}

void rel_output (rel_t *r) {
}

void rel_timer ()
{
  rel_t * s;
  flight_t * p;
  for (s = rel_list; s != NULL; s = s->next) {
     int i;
    for(i = s->LAR+1; i < (s->LFS+1) ; i++) {
      for(p = s->curr_win_head; p != NULL; p = p->next) {
        if(p->seq == s->LAR+1) {
          break;
        }
      }
      p->ack_timer += s->timeout * 0.2;
      if (p->ack_timer >= s->timeout) {
        send_packet(s, p);
      }
    }
  }
}
