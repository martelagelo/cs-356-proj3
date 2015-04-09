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

/**********add by xin*************/
#define MAX_WIN_SIZE  1024
#define MAX_DATA_LEN 500
#define DATA_PKT      1
#define ACK_PKT       2
#define EOF_PKT       3
#define DATA_HEADER_LEN 12
#define ACK_HEADER_LEN  8


struct Q
{
  int seq;
  int size;
  char data[MAX_DATA_LEN];
};
typedef struct Q Q_t;

/********************************/

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
  
/*****************add by xin**************/

int win_size;
int timeout;


int remote_eof;
int eof;
int all_acked;            // TODO remove
int timeout_rounds;       // TODO remove


int LFS;  //Largest Frame Same
int LAR; // Last ack recieved

flight_t * curr_win_head;
flight_t * curr_win_tail;

flight_t * ack_packet;

Q_t out_q[MAX_WIN_SIZE];
int ack_timer[MAX_WIN_SIZE];
int acked[MAX_WIN_SIZE];
int num_inflight_packets;           //how many output buffers currently used

int pkt_expected;
int LAF; //upper edge of reciever's edge + 1
Q_t in_q[MAX_WIN_SIZE];
int arrived[MAX_WIN_SIZE];        //inbound bit map

/*****************************************/

};
rel_t *rel_list;

/****************add by xin***********************/
flight_t* get_eof_packet(int seqno) {
  flight_t* eof_packet = (flight_t *)malloc(sizeof(flight_t *)); 
  eof_packet->ack_timer = -1;
  eof_packet->size = -1;
  eof_packet->is_acked = 0;
  eof_packet->seq = seqno;
  return eof_packet;
}

void send_pkt_2 (rel_t * r, flight_t * content) {
  packet_t pckt;
  memset(pckt.data, '\0', 500);

  if (content->ack_timer >= 0) { // is a data packet
    pckt.len = htons(DATA_HEADER_LEN + content->size);
    pckt.seqno = htonl(content->seq);
    memcpy(pckt.data, content->data, content->size);
    content->ack_timer = 0;
    //fprintf(stderr, "DATA:Sending SeqNum: %u\n", content->seq);
  } else if (content->size == -1) { // is an EOF packet
    pckt.len = htons(DATA_HEADER_LEN);
    pckt.seqno = htonl(content->seq);
    memset(pckt.data, '\0', MAX_DATA_LEN);
    content->ack_timer = 0;
    //fprintf(stderr, "EOF:Sending SeqNum: %u\n", content->seq);
  } else { // is an ACK packet
    pckt.len = htons(ACK_HEADER_LEN);
  }

  pckt.ackno = htonl(r->pkt_expected);
  pckt.cksum = 0;
  pckt.cksum = cksum (&pckt, ntohs(pckt.len));
  conn_sendpkt (r->c, &pckt, ntohs(pckt.len));
}

void send_pkt(rel_t *s, int pkt_kind, int seq) {
  /*struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  fprintf(stderr, "%d,  %ld\n", getpid(), ts.tv_nsec);*/
  
  packet_t pkt;
  memset(pkt.data, '\0', 500);
  int index = seq % s->win_size;
  if(pkt_kind == DATA_PKT) {
    pkt.len = htons(DATA_HEADER_LEN + s->out_q[index].size);
    pkt.seqno = htonl(seq);
    pkt.ackno = htonl(s->pkt_expected);
    memcpy(pkt.data, s->out_q[index].data,s->out_q[index].size);
    pkt.cksum = 0;
    pkt.cksum = cksum (&pkt, ntohs(pkt.len));
    s->ack_timer[index] = 0;
    s->acked[index] = 0;
    char *test = "send data ";
    print_pkt (&pkt, test, ntohs(pkt.len));
  }
  else if(pkt_kind == ACK_PKT) {
    pkt.len = htons(ACK_HEADER_LEN);
    pkt.ackno = htonl(s->pkt_expected);
    pkt.cksum = 0;
    pkt.cksum = cksum (&pkt, ntohs(pkt.len));
    char *test = "send ack ";
    print_pkt (&pkt, test, ntohs(pkt.len));
  }
  else //EOF_PKT
  {
    pkt.len = htons(DATA_HEADER_LEN);
    pkt.seqno = htonl(seq);
    pkt.ackno = htonl(s->pkt_expected);
    memset(pkt.data, '\0',MAX_DATA_LEN);
    pkt.cksum = 0;
    pkt.cksum = cksum (&pkt, DATA_HEADER_LEN);
    s->ack_timer[index] = 0;
    s->acked[index] = 0;
    char *test = "send EOF ";
    print_pkt (&pkt, test, DATA_HEADER_LEN);
  }
  conn_sendpkt (s->c, &pkt, ntohs(pkt.len));  
}
/******************************************/


/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
      const struct config_common *cc) {
  //fprintf(stderr, "%d, call rel_create\n", getpid());
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
  
  /***********add by xin*********************/
  r->win_size = cc->window;
  r->timeout = cc->timeout;

  r->LFS = 0;
  r->LAR = 0;
  r->num_inflight_packets = 0;

  r->remote_eof = 0;
  r->eof = 0;
  r->all_acked = 0;

  r->pkt_expected = 1;
  r->LAF = r->win_size + 1;
  int i;
  for(i = 0; i < r->win_size; i++) {
    r->acked[i] = 0;
    r->arrived[i] = 0;
  }
/******************************************/

  return r;
}

void
rel_destroy (rel_t *r) {
  //fprintf(stderr, "%d, call rel_destroy\n", getpid());
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);
  /* Free any other allocated memory here */
  free(r);
  //fprintf(stderr, "rel_destroy is called\n");

}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
     const struct sockaddr_storage *ss,
     packet_t *pkt, size_t len) {
}

/*****************add by xin***************/

int
check_cksum(packet_t *pkt, size_t n) {
  uint16_t cs = pkt->cksum;
  pkt->cksum = 0;
  pkt->cksum = cksum (pkt, n);
  if(cs == pkt->cksum) {
    return 1;
  }
  return 0;
}

int between(int a, int b, int c) {
  if(a <= b && b < c)
    return 1;
  return 0;
}

int all_acked(rel_t *s) {
  int i;
  for(i = s->LAR+1; i < (s->LFS + 1); i++) {
      int index = i % s->win_size;
      if(s->acked[index] == 0)
        break;
    } 
  if(i == (s->LFS + 1))
    return 1;
  return 0;
}

/*****************************************/
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
  //fprintf(stderr, "%d, call rel_recvpkt\n", getpid());
  /************add by xin******************/  
  if(n < 8 || (n > 8 && n < 12 ) || n != ntohs(pkt->len) || check_cksum(pkt, n) == 0) {
    // send_pkt(r, ACK_PKT, 0);
    // send_pkt_2(r, r->ack_packet);
    return;
  } else {  
    //handle ackno
    flight_t * p = r->curr_win_head;
    while(ntohl(pkt->ackno) - 1 >=  r->LAR+1 && ntohl(pkt->ackno) - 1 < r->LFS + 1) {
        
        for(; p != NULL; p = p->next) {
          //fprintf(stderr, "FLIGHT SEQ: %d PACKET SEQ: %d\n", p->seq, r->LAR+1);
          if(p->seq == r->LAR+1) {
            break;
          }
        }

        if(p == NULL || p->is_acked == 1) {
          continue;
        }
        int index = (r->LAR+1) % r->win_size;
        r->ack_timer[index] = 0;
        r->acked[index] = 1;
        r->num_inflight_packets--;
        r->LAR++;
        r->acked[(r->LAR+1) % r->win_size] = 0;
        r->all_acked = all_acked(r);
        p->is_acked = 1;
        //fprintf(stderr, "%d !!! handle ack remote_eof = %d,  eof = %d,  all_acked = %d\n", getpid(), r->remote_eof, r->eof, r->all_acked);
        if(r->remote_eof == 1 && r->eof == 1 && r->all_acked == 1) {
          rel_destroy (r);
        }

      
      while(r->curr_win_head != NULL) { // remove the first contigous block of acked things in sent buffer
        if (r->curr_win_head->is_acked == 0) {
          break;
        } else {
         // fprintf(stderr, "curr_win_head adress; %u tmp adress : %u\n", r->curr_win_head, r->curr_win_head->next);
          flight_t *del = r->curr_win_head;
          r->curr_win_head = r->curr_win_head->next;
        //if (r->curr_win_head != NULL) {
            //if(r->curr_win_head->prev != NULL) {
              //fprintf(stderr, "HERE\n"); 
              free(del); 
              //fprintf(stderr, "DONE\n");
           // }
        // } 
        } 
      }      
    }
    //fprintf(stderr, "%d,  sender  LAR = %d,   pkt->ackno = %d,    LFS = %d\n", getpid(), r->LAR, ntohl(pkt->ackno), r->LFS);

    //handle seqno
    if(n >= DATA_HEADER_LEN) {
      if(ntohl(pkt->seqno) != r->pkt_expected)
        send_pkt(r, ACK_PKT, 0);
        // send_pkt_2(r, r->ack_packet);
      int index = ntohl(pkt->seqno) % r->win_size;
      if(between(r->pkt_expected, ntohl(pkt->seqno), r->LAF) == 1 && !r->arrived[index]) {
        r->in_q[index].seq = ntohl(pkt->seqno);
        r->in_q[index].size = n - DATA_HEADER_LEN;
        memcpy(r->in_q[index].data, pkt->data, n - DATA_HEADER_LEN);
        r->arrived[index] = 1;
        rel_output(r);
      }
    }
    //fprintf(stderr, "%d,  reciever  pkt_expected = %d,   pkt->seqno = %d,   LAF = %d\n", getpid(), r->pkt_expected, ntohl(pkt->seqno), r->LAF);

    if(r->num_inflight_packets < r->win_size && r->eof == 0) {
      rel_read (r);
    }
  }
  /***************************************/
}


void
rel_read (rel_t *s) {
  //fprintf(stderr, "%d, call rel_read\n", getpid());
  /*****************add by xin****************************/
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
    int index = (s->LFS+1) % s->win_size;
    memset(s->out_q[index].data, '\0', MAX_DATA_LEN);
    int data_size = conn_input(s->c, s->curr_win_tail->data, MAX_DATA_LEN);
    memcpy(s->out_q[index].data, s->curr_win_tail->data, MAX_DATA_LEN);
    //fprintf(stderr, "%d, READ Len = %d\n", getpid(), data_size);
    if (data_size > 0) { //there is some input from STDIN
      s->num_inflight_packets++;
      s->LFS++;
      //fprintf(stderr, "%d,  READ  DATA num_inflight_packets = %d\n", getpid(), s->num_inflight_packets);
      s->curr_win_tail->seq = s->LFS;
      s->curr_win_tail->size = data_size;
      s->curr_win_tail->ack_timer = 0;
      s->curr_win_tail->is_acked = 0;

      s->out_q[index].seq = s->LFS;
      s->out_q[index].size = data_size;
      // TODO Refactor/rewrite send_pkt
      // send_pkt(s, DATA_PKT, s->LFS);
      send_pkt_2(s, s->curr_win_tail);
      
    } else if(data_size < 0) {//EOF or error
      //fprintf(stderr, "%d,  READ  EOF num_inflight_packets = %d\n", getpid(), s->num_inflight_packets);
      s->num_inflight_packets++;
      s->LFS++;
      s->eof = 1;
      flight_t * eof_pkt = get_eof_packet(s->LFS);
      // TODO Refactor/rewrite send_pkt
      // send_pkt(s, EOF_PKT, s->LFS);
      send_pkt_2(s, eof_pkt);
    } else {
      return;
    }
  }
  return;
  /*******************************************************/
}

void
rel_output (rel_t *r) {
  /*********************add by xin***********************/
  //fprintf(stderr, "%d, call rel_output\n", getpid());
  int flag = 0;
  int index = r->pkt_expected % r->win_size;
  while(r->arrived[index] && r->in_q[index].size && conn_bufspace(r->c) >= r->in_q[index].size) {
    conn_output(r->c, r->in_q[index].data, r->in_q[index].size);
    r->arrived[index] = 0;
    memset(&(r->in_q[index]), '\0', sizeof(r->in_q[index]));
    r->pkt_expected++;
    r->LAF++;
    index = r->pkt_expected % r->win_size;
    flag = 1;
  }
  if(flag == 1) {
    send_pkt(r, ACK_PKT, 0);
    // send_pkt_2(r, r->ack_packet);
  }

  if(r->arrived[index] && r->in_q[index].size == 0) {
    conn_output(r->c, r->in_q[index].data, r->in_q[index].size);
    r->arrived[index] = 0;
    memset(&(r->in_q[index]), '\0', sizeof(r->in_q[index]));
    r->pkt_expected++;
    r->LAF++;
    send_pkt(r, ACK_PKT, 0);
    // send_pkt_2(r, r->ack_packet);  
    r->remote_eof = 1;
    r->all_acked = all_acked(r);
    if(r->remote_eof == 1 && r->eof == 1 && r->all_acked == 1) {
      rel_destroy (r);
    }
  }
  /****************************************************/
}

void
rel_timer () {
  /* Retransmit any packets that need to be retransmitted */
  /***********add by xin************/
  rel_t *s;
  for(s = rel_list; s != NULL; s = s->next) {
    int i;
    for(i = s->LAR+1; i < (s->LFS+1) ; i++) {
      int index = i % s->win_size;
      s->ack_timer[index] = s->ack_timer[index] + s->timeout * 0.2;
      if(s->ack_timer[index] >= s->timeout) {
        if(!(s->out_q[index].size == 0)) {
          send_pkt(s, DATA_PKT, s->out_q[index].seq);
        }
          
      }
    } 
  }
  /*********************************/
}