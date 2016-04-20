
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
#include <signal.h>
#include "rlib.h"

//static int quit = 0;
void iterPackNAdd(packet_t * pack, rel_t * s);
//static int counter = 0;
//static packet_t* windowfull = NULL;
static int eofrec = 0;
static int eofread = 0;
static int eofseqno = 0;

struct packetnode {
	int length;
	packet_t* packet;
	struct packetnode* next;
};

struct rec_slidingWindow {
	uint32_t rws; // upper bound on no. of out-of-order frames that the receiver is willing to accept
	uint32_t laf; // seqNum of largest acceptable frame
	uint32_t lfr; // seqNum of last frame received
};

struct send_slidingWindow {
	uint32_t sws; //upper bound on no. of unacked frames that sender can transmitt
	uint32_t lar; // sequence of last ack received
	int32_t lfs; //last frame sent
};



struct reliable_state {
	rel_t *next;			/* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c;			/* This is the connection object */
	/* Add your own data fields below this */


	packet_t** senderbuffer;
	packet_t** receiverbuffer;

	int window_size;
	int timeout_len;
	long* times;

	/*bc rel_t gets passed btw all functions it should keep track of our sliding windows*/
	struct send_slidingWindow * send_sw;
	struct rec_slidingWindow * rec_sw;
};
rel_t *rel_list;


/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
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

	//fprintf(stderr,"rel_create\n");
	/* Do any other initialization you need here */

	/*init our packet information*/
	r->window_size= cc->window;
	r->timeout_len = cc->timeout;


	r->senderbuffer = malloc(r->window_size * sizeof(packet_t));
	r->receiverbuffer = malloc(r->window_size * sizeof(packet_t));
	memset(r->senderbuffer, 0, r->window_size * sizeof(packet_t));
	memset(r->receiverbuffer, 0, r->window_size * sizeof(packet_t));

	/*init our sliding windows*/
	r->rec_sw = xmalloc(sizeof(struct rec_slidingWindow));
	r->send_sw = xmalloc(sizeof(struct send_slidingWindow));
	r->rec_sw->rws = r->window_size;//cc->window; //window size
	r->rec_sw->lfr = 0; //no frames recieved
	r->rec_sw->laf = r->rec_sw->lfr + r->rec_sw->rws; //last acceptable frame

	//will be our window size plus last seqnum accepted
	r->send_sw->sws = r->window_size;//cc->window; //window size
	r->send_sw->lar = 0; //no acks received
	r->send_sw->lfs = 0; //no frames sent so far

	r->times = malloc(r->window_size * sizeof(long));
	return r;
}

void
rel_destroy (rel_t *r)
{
	if ( r->receiverbuffer[0]==NULL && r->send_sw->lar > r->send_sw->lfs && eofrec && eofread){
		if (r->next){
			r->next->prev = r->prev;	
		}
		*r->prev = r->next;
		conn_destroy (r->c);

		free(r->senderbuffer);
		free(r->receiverbuffer);
		free(r->times);
		free(r);
	}
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
           packet_t *pkt, size_t len)
{
}

void
rel_sendack(rel_t *r) {
	//fprintf(stderr,"sendack");
	packet_t *ackpack = malloc(sizeof(*ackpack));
	memset(ackpack, 0, sizeof(*ackpack));
	ackpack->cksum = 0;
	//r->acknum++; //Not sure if this is necessary
	ackpack->ackno = htonl((r->rec_sw->lfr + 1));
	ackpack->len = htons(sizeof(struct ack_packet)); //not sure if this is correct
	ackpack->cksum = cksum(ackpack, sizeof(struct ack_packet));
	conn_sendpkt(r->c, ackpack, sizeof(struct ack_packet));
	free(ackpack);
}

long long current_timestamp() {
    struct timeval te; 
    gettimeofday(&te, NULL);
    long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000;
    return milliseconds;
}

void send_packet(packet_t* pkt, rel_t* s, int index, int len) {
	s->times[index] = current_timestamp();
	conn_sendpkt(s->c, pkt, len);
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	//fprintf(stderr, "\nrecvpkt len: %d\n", ntohs(pkt->len));
	//fprintf(stderr, "my window size is %d", r->rec_sw->rws);

	int ackSize = sizeof(struct ack_packet);
	int dataPackSize = sizeof(pkt->data) + 12;
	if (htons(pkt->len) > dataPackSize || htons(pkt->len) < ackSize) {
		return;
	} 
	packet_t* newpack = malloc(ntohs(pkt->len));
	memcpy(newpack, pkt, ntohs(pkt->len));
	newpack->cksum = 0;
	newpack->cksum = cksum(newpack, ntohs(newpack->len));

	// check for corrupted packet
	if (newpack->cksum != pkt->cksum) {
		fprintf(stderr, "Corrupted Data");
	}
	// Check for corrupted data
	//fprintf(stderr, "cksum=%d, checksum=%d", cksum(pkt, pkt_len), n);
	/*if (n != 512 || (cksum(pkt, pkt_len) != checksum)) {
		//drop pack
		//fprintf(stderr, "dropped");
		return;
	}*/
	else if (ntohs(pkt->len) < sizeof(struct ack_packet) || ntohs(pkt->len) > sizeof(pkt->data)+12) {
		fprintf(stderr, "completely messed up packet");
		return;
	} 
	//Handle Ack Packet
	else if (ntohs(pkt->len) == sizeof(struct ack_packet)) {
		//fprintf(stderr,"ackkkkkkkkkkkkkk");
		if (ntohl(pkt->ackno) > r->send_sw->lar) {
			r->send_sw->lar = ntohl(pkt->ackno);
		}
		if (ntohl(pkt->ackno) == eofseqno + 1) {
			eofread = 1;
		}
		while (r->senderbuffer[0] != NULL) {
			int i;
			if  (ntohl(r->senderbuffer[0]->seqno) >= r->send_sw->lar) {
				break;
			}
			for (i = 0; i < r->window_size - 1; i++) {
				r->senderbuffer[i] = r->senderbuffer[i + 1];
				r->times[i] = r->times[i + 1];
			}
			r->senderbuffer[r->window_size - 1] = NULL;
			r->times[r->window_size - 1] = 0;
		}
		rel_read(r);
	}
	// Handle a data packet
	else {
		//fprintf(stderr, "datapacket!!");
		if (ntohl(pkt->seqno) > r->rec_sw->laf) {
			fprintf(stderr, "Packet is greater than largest acceptable frame");
		}
		else if (ntohl(pkt->seqno) <= r->rec_sw->lfr) {
			rel_sendack(r);
			fprintf(stderr, "Already received");
		}

		else {
			if (ntohs(pkt->len) == 12) {
				eofrec = 1;
			}
			int in = (ntohl(pkt->seqno) - (r->rec_sw->lfr + 1)) % (r->window_size);
			packet_t* temppack = malloc(sizeof(packet_t));
			memcpy(temppack, pkt, sizeof(packet_t));
			r->receiverbuffer[in] = temppack;
			rel_output(r);
			rel_sendack(r);
		}
	}
}

void
rel_read (rel_t *s)
{
	//packet_t* pack = malloc(sizeof(packet_t));
	while (eofseqno == 0) {
		if (s->senderbuffer[s->window_size - 1] != NULL) {
			break;
		}
		packet_t *temp = malloc(sizeof(packet_t));
		int input = conn_input(s->c, temp->data, sizeof(temp->data));
		int inputLen = input + 12;

		if (input == -1) {
			eofseqno = s->send_sw->lfs + 1;
			inputLen = 12;
		}
		else if (input == 0) {
			break;
		}
		//memcpy(temp->data, pack->data, sizeof(temp->data));
		//memset(pack, 0, sizeof(packet_t));

		int ind = 0;
		while (s->senderbuffer[ind] != NULL) {
			ind ++;
		}
		s->senderbuffer[ind] = temp;

		temp->cksum = 0;
		temp->len = htons(inputLen);
		temp->seqno = htonl(s->send_sw->lfs + 1);
		temp->ackno = htonl(s->send_sw->lar);
		temp->cksum = cksum(temp, inputLen);

		send_packet(temp, s, ind, inputLen);
		s->send_sw->lfs += 1;
	}
}

void
rel_output (rel_t *r)
{
	while (r->receiverbuffer[0] != NULL) {
		int pack_size = ntohs(r->receiverbuffer[0]->len) - 12;
		int avail_buf_space = conn_bufspace(r->c);
		if (pack_size > avail_buf_space) {
			return;
		}
		conn_output(r->c, r->receiverbuffer[0]->data, pack_size);

		r->rec_sw->lfr = ntohl(r->receiverbuffer[0]->seqno);
		r->rec_sw->laf = r->rec_sw->lfr + r->window_size;
		int i;
		for (i = 0; i < r->window_size - 1; i++) {
			r->receiverbuffer[i] = r->receiverbuffer[i + 1];
		}
		r->receiverbuffer[r->window_size - 1] = NULL;
	}
}

void
rel_timer ()
{
	int i;
	for (i = 0; i < rel_list->window_size; i++) {
		if (rel_list->times[i] > 0) {
			long currentTime = current_timestamp();
			long elapsedTime = currentTime - rel_list->times[i];
			//fprintf(stderr, "pos:%d, time:%lu\n", i, rel_list->times[i]);
			if (elapsedTime > rel_list->timeout_len) {
				//fprintf(stderr, "packet seqno %d TIMEOUT, retransmitting!\n",ntohl(rel_list->senderbuffer[i]->seqno));
				send_packet(rel_list->senderbuffer[i], rel_list, i, ntohs(rel_list->senderbuffer[i]->len));
			}
		}
	}
	rel_destroy(rel_list);
}


