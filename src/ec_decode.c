/*
    ettercap -- decoder module

    Copyright (C) ALoR & NaGA

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

    $Id: ec_decode.c,v 1.35 2003/09/19 16:47:51 alor Exp $
*/

#include <ec.h>
#include <ec_decode.h>
#include <ec_dispatcher.h>
#include <ec_threads.h>
#include <ec_ui.h>
#include <ec_packet.h>
#include <ec_hook.h>
#include <ec_filter.h>

#include <pcap.h>
#include <pthread.h>

/* globals */

FUNC_DECODER_PTR(l2_decoder);

static SLIST_HEAD (, dec_entry) decoders_table;

struct dec_entry {
   u_int32 type;
   u_int8 level;
   FUNC_DECODER_PTR(decoder);
   SLIST_ENTRY (dec_entry) next;
};

static SLIST_HEAD (, mtu_entry) mtu_table;

struct mtu_entry {
   u_int16 type;
   u_int16 mtu;
   SLIST_ENTRY (mtu_entry) next;
};

/* protos */

void __init data_init(void);
FUNC_DECODER(decode_data);

void ec_decode(u_char *param, const struct pcap_pkthdr *pkthdr, const u_char *pkt);
int set_L2_decoder(int dlt);
void set_iface_mtu(u_int16 dlt);
void add_iface_mtu(u_int16 type, u_int16 mtu);
void add_decoder(u_int8 level, u_int32 type, FUNC_DECODER_PTR(decoder));
void del_decoder(u_int8 level, u_int32 type);
void * get_decoder(u_int8 level, u_int32 type);

/* mutexes */

static pthread_mutex_t decoders_mutex = PTHREAD_MUTEX_INITIALIZER;
#define DECODERS_LOCK     do{ pthread_mutex_lock(&decoders_mutex); } while(0)
#define DECODERS_UNLOCK   do{ pthread_mutex_unlock(&decoders_mutex); } while(0)

static pthread_mutex_t dump_mutex = PTHREAD_MUTEX_INITIALIZER;
#define DUMP_LOCK     do{ pthread_mutex_lock(&dump_mutex); } while(0)
#define DUMP_UNLOCK   do{ pthread_mutex_unlock(&dump_mutex); } while(0)

/*******************************************/


void ec_decode(u_char *param, const struct pcap_pkthdr *pkthdr, const u_char *pkt)
{
   struct packet_object po;
   struct pcap_stat ps;
   int len;
   u_char *data;
   int datalen;
   
   CANCELLATION_POINT();

   /* start the timer for the stats */
   stats_half_start(&GBL_STATS->bh);
   
   /* XXX -- remove this */
#if 0
   if (!GBL_OPTIONS->quiet) {
      USER_MSG("\n***************************************************************\n");
      USER_MSG("ec_get_packets (one packet dispatched from pcap)\n");

      USER_MSG("CAPTURED: 0x%04x bytes form %s\n", pkthdr->caplen, param );
   }
#endif
   
   if (GBL_OPTIONS->read)
      /* update the offset pointer */
      GBL_PCAP->dump_off = ftell(pcap_file(GBL_PCAP->pcap));
   else {
      /* update the statistics 
       *
       * statistics are available only in live capture
       * no statistics are stored in savefiles
       */
      pcap_stats(GBL_PCAP->pcap, &ps);
      /* 
       * add to the previous value, since every call
       * to pcap_stats reset the counter 
       */
      GBL_STATS->ps_recv += ps.ps_recv;
      GBL_STATS->ps_drop += ps.ps_drop;
      GBL_STATS->ps_ifdrop += ps.ps_ifdrop;
   }
   
   /* 
    * dump packet to file if specified on command line 
    * it dumps all the packets disregarding the filter
    */
   if (GBL_OPTIONS->write) {
      /* 
       * we need to lock this because in SM_BRIDGED the
       * packets are dumped in the log file by two threads
       */
      DUMP_LOCK;
      pcap_dump((u_char *)GBL_PCAP->dump, pkthdr, pkt);
      DUMP_UNLOCK;
   }
   
   /* extract data and datalen from pcap packet */
   data = (u_char *)pkt;
   datalen = pkthdr->caplen;

   /* alloc the packet object structure to be passet through decoders */
   packet_create_object(&po, data, datalen);

   /* set the po timestamp */
   memcpy(&po.ts, &pkthdr->ts, sizeof(struct timeval));
   
   /* 
    * set the interface from which the packet comes 
    * 
    * GBL_OPTIONS->iface may be null when reading from file
    */
   if (GBL_OPTIONS->iface && !strcmp(param, GBL_OPTIONS->iface))
      po.flags |= PO_FROMIFACE;
   else if (GBL_OPTIONS->iface_bridge && !strcmp(param, GBL_OPTIONS->iface_bridge))
      po.flags |= PO_FROMBRIDGE;

   /* HOOK POINT: RECEIVED */ 
   hook_point(HOOK_RECEIVED, &po);
   
   /* 
    * by default the packet should not be processed by ettercap.
    * if the sniffing filter matches it, the flag will be reset.
    */
   po.flags |= PO_IGNORE;
  
   /* 
    * start the analysis through the decoders stack 
    *
    * if the packet can be handled it will reach the top of the stack
    * where the decoder_data will add it to the top_half queue,
    * else the packet will not be handled but it should be forwarded
    *
    * after this fuction the packet is completed (all flags set)
    */
   l2_decoder(data, datalen, &len, &po);

   /* XXX - BIG WARNING !!
    *
    * if the packet was filtered by the filtering engine
    * the state of the packet_object is inconsistent !
    * the fields in the structure may not reflect the real
    * packet fields...
    */
   
   /* use the sniffing method funcion to forward the packet */
   if (po.flags & PO_FORWARDABLE ) {
      /* HOOK POINT: PRE_FORWARD */ 
      hook_point(HOOK_PRE_FORWARD, &po);
      EXECUTE(GBL_SNIFF->forward, &po);
   }
   
   /* 
    * if it is the last packet set the flag 
    * and send the packet to the top half.
    * we have to do this because the last packet 
    * might be dropped by the filter.
    */
   if (GBL_OPTIONS->read && GBL_PCAP->dump_size == GBL_PCAP->dump_off) {
      po.flags |= PO_EOF;
      top_half_queue_add(&po);
   }
   
   /* free the structure */
   packet_destroy_object(&po);
   
   /* clear the buffer */
   memset((u_char *)pkt, 0, pkthdr->caplen);
   
   /* calculate the stats */
   stats_half_end(&GBL_STATS->bh, pkthdr->caplen);
   
   CANCELLATION_POINT();

   return;
}

/* register the data decoder */
void __init data_init(void)
{
   add_decoder(APP_LAYER, PL_DEFAULT, decode_data);
}

/* 
 * if the packet reach the top of the stack (it can be handled),
 * this decoder is invoked
 */

FUNC_DECODER(decode_data)
{
   FUNC_DECODER_PTR(app_decoder);
      
   CANCELLATION_POINT();
   
   /* HOOK POINT: HANDLED */ 
   hook_point(HOOK_HANDLED, po);

   /* reset the flag PO_INGNORE if the packet should be processed */
   EXECUTE(GBL_SNIFF->display, po);

   /* 
    * the display engine has stated that this
    * packet should not be processed by us.
    */
   if ( po->flags & PO_IGNORE )
      return NULL;

   /* 
    * run the APP_LAYER decoders 
    *
    * we should run the decoder on both the tcp/udp ports
    * since we may be interested in both client and server traffic.
    */
   switch (po->L4.proto) {
      case NL_TYPE_TCP:
         app_decoder = get_decoder(APP_LAYER_TCP, ntohs(po->L4.src));
         EXECUTE_DECODER(app_decoder);
         app_decoder = get_decoder(APP_LAYER_TCP, ntohs(po->L4.dst));
         EXECUTE_DECODER(app_decoder);
         break;
         
      case NL_TYPE_UDP:
         app_decoder = get_decoder(APP_LAYER_UDP, ntohs(po->L4.src));
         EXECUTE_DECODER(app_decoder);
         app_decoder = get_decoder(APP_LAYER_UDP, ntohs(po->L4.dst));
         EXECUTE_DECODER(app_decoder);
         break;
   }
   
   /* HOOK POINT: DECODED (the po structure is filled) */ 
   hook_point(HOOK_DECODED, po);

   /*
    * here we can filter the content of the packet.
    * the injection is done elsewhere.
    */
   if (GBL_FILTERS->chain)
      filter_engine(GBL_FILTERS->chain, po);

   /* 
    * this hook point is executed only it the packet
    * has to be forwarded 
    */
   if (po->flags & PO_FORWARDABLE) {
      /* HOOK POINT: FILTER */ 
      hook_point(HOOK_FILTER, po);
   }
   
   /* 
    * add the packet to the queue and return.
    * we must be fast here !
    */
   top_half_queue_add(po);     

   CANCELLATION_POINT();
  
   return NULL;
}
      
/*
 * set the L2 decoder and the pcap offset.
 * lookup the decoders_table to find wich decoder are
 * available
 */

int set_L2_decoder(int dlt)
{
   struct dec_entry *e;

   DECODERS_LOCK;
   
   SLIST_FOREACH (e, &decoders_table, next) {
      if (e->level == 2 && e->type == (u_int16)dlt) {
         DEBUG_MSG("DLT = %d : decoder found !", dlt);
         l2_decoder = e->decoder;

         /* set the MTU */
         set_iface_mtu((u_int16)dlt);
            
         DECODERS_UNLOCK;
         return ESUCCESS;
      }
   }

   DECODERS_UNLOCK;
   /* error NOT FOUND */
   return -ENOTFOUND;
}

/*
 * set the GBL_IFACE->mtu looking in the table
 * of registerd decoders
 */
void set_iface_mtu(u_int16 dlt)
{
   struct mtu_entry *e;

   /* 
    * a dirty hack for loopback:
    * it is an ehternet but with larger mtu
    */
   if (GBL_OPTIONS->iface && !strcasecmp(GBL_OPTIONS->iface, "lo")) {
      GBL_IFACE->mtu = 16436;
      DEBUG_MSG("MTU = %d", GBL_IFACE->mtu);
      return;
   }
         
   /* search in the registerd mtu */
   SLIST_FOREACH (e, &mtu_table, next) {
      if (e->type == dlt) {
         GBL_IFACE->mtu = e->mtu;
         DEBUG_MSG("MTU = %d", GBL_IFACE->mtu);
      }
   }
}

/*
 * add an mtu to the mtu table 
 */
void add_iface_mtu(u_int16 type, u_int16 mtu)
{
   struct mtu_entry *e;

   e = calloc(1, sizeof(struct mtu_entry));
   ON_ERROR(e, NULL, "can't allocate memory");

   e->type = type;
   e->mtu = mtu;

   SLIST_INSERT_HEAD (&mtu_table, e, next); 
}

/*
 * add a decoder to the decoders table 
 */
void add_decoder(u_int8 level, u_int32 type, FUNC_DECODER_PTR(decoder))
{
   struct dec_entry *e;

   e = calloc(1, sizeof(struct dec_entry));
   ON_ERROR(e, NULL, "can't allocate memory");

   e->level = level;
   e->type = type;
   e->decoder = decoder;

   DECODERS_LOCK;
   
   SLIST_INSERT_HEAD (&decoders_table, e, next); 

   DECODERS_UNLOCK;
   
   return;
}

/*
 * get a decoder from the decoders table 
 */

void * get_decoder(u_int8 level, u_int32 type)
{
   struct dec_entry *e;
   void *ret;

   DECODERS_LOCK;
   
   SLIST_FOREACH (e, &decoders_table, next) {
      if (e->level == level && e->type == type) {
         ret = (void *)e->decoder;
         DECODERS_UNLOCK;
         return ret;
      }
   }

   DECODERS_UNLOCK;
   return NULL;
}

/*
 * remove a decoder from the decoders table
 */

void del_decoder(u_int8 level, u_int32 type)
{
   struct dec_entry *e;

   DECODERS_LOCK;
   
   SLIST_FOREACH (e, &decoders_table, next) {
      
      if (e->level == level && e->type == type) {
         //DEBUG_MSG("L%d %d removed !!", level, type);
         SLIST_REMOVE(&decoders_table, e, dec_entry, next);
         SAFE_FREE(e);
         DECODERS_UNLOCK;
         return;
      }
   }
   
   DECODERS_UNLOCK;
   return;
}

/* EOF */

// vim:ts=3:expandtab

