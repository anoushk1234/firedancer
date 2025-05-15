#ifndef HEADER_fd_src_app_shared_commands_fdtop_fdtop_h
#define HEADER_fd_src_app_shared_commands_fdtop_fdtop_h
#include <notcurses/notcurses.h>
#include "../../fd_config.h"
#include "../../../../flamenco/leaders/fd_leaders.h"
/*#include "menu.h"*/

#define FD_BLACK 0x000000
#define FD_WHITE 0xFFFFFF
#define FD_MINT 0x1abfa2


#define NANOSECS_IN_SEC 1000000000ul

#define MENU_BAR_Y 3
#define WIDGET_MARGIN 2

/*TODO: Check if this is aligned by the compiler, if not align manually.*/
typedef struct {
  ulong polling_rate_ms;

  fd_pubkey_t identity_key;
  char identity_key_base58[ FD_BASE58_ENCODED_32_SZ ];
  ulong next_leader_slot;
  ulong current_slot;
  
  struct {
    ulong epoch;
    ulong tsstart;
    ulong tsend;

    ulong my_total_slots;
    ulong my_skipped_slots;
    
    ulong epoch_total_stake;
    ulong my_total_stake;
    fd_epoch_leaders_t * leader_sched;
  } epoch;

  struct {
    fd_pubkey_t vote_account [ 1 ];
    ulong last_vote;
    ulong epoch_credits;

    /* The vote credit data for last 32 slots. 
       TODO: Change this to a better number. */
    ulong tvc_historical [ 32 ];
    int delinquent;
  } vote_info;

  struct {
   ulong gossip_in_bytes;
   ulong gossip_out_bytes;

   ulong quic_conn_cnt;
   ulong net_in_rx_cnt;
   ulong net_out_tx_cnt;

   ulong gossip_peer_cnt;
  } stats;
  struct {
   ulong txn_success;
  } bank;
  struct {
    ulong gossip_msg_rx;
  } gossip;
  struct {
    
   int page_number;
   int show_help;

   /* An integer where the first eight bits signify if the corresponding
     monitor at the respective index is enabled or disabled. */
   int monitors;
   /* Ground zero plane is the std plane given by notcurses context,
    * Base plane is plane the covers the whole screen and is above std plane but 
    * below all the other planes where the main widgets exist.*/
   struct ncplane* base_plane;
   int widget_y;
  } app_state;

} fd_top_t __attribute__((aligned(8)));



typedef struct {
  fd_top_t * app;
  fd_topo_t const * topo;
  struct notcurses * nc;
  /*sem_t *control_t;*/
} thread_args __attribute__((aligned(8)));

void fdtop_cmd_fn( args_t * args, config_t * config );
void fdtop_cmd_args( int * argc, char *** argv, args_t * args );
void fdtop_cmd_perm( args_t * args, fd_cap_chk_t * chk, config_t const * config );
void* draw_monitor( void * arguments );
void* poll_metrics( void * arguments );
void* handle_input( void *arguments );

#define CHART_BUFFER_LEN 32

typedef struct {
  void* buffer;
  void* head;
  void* tail;
  /* Note: Assuming every element has the same size.*/
  size_t data_sz;
  size_t length;
} ring_buffer __attribute__((aligned(8)));

int
rb_new( ring_buffer* rb, fd_alloc_t* alloc, size_t length, size_t data_sz ){
  rb->buffer = fd_alloc_malloc( alloc, alignof(int), length * data_sz );
  
  if( FD_UNLIKELY( NULL==rb->buffer ) ){
    return -1;
  }
  rb->data_sz = data_sz;
  rb->head = rb->buffer;
  rb->tail = rb->buffer;
  rb->length = length;
  return 0;
}
int
rb_pop_front( ring_buffer* rb,  void* data ){
   if( rb->head == rb->tail ){
     return -1;
   }
   void* end = ((char*)rb->buffer) + (rb->length*rb->data_sz);
   if( FD_UNLIKELY( rb->head==end ) ){
     rb->head = rb->buffer;
     memcpy( data, rb->head, rb->data_sz );
     return 0;
   }
   memcpy( data, rb->head, rb->data_sz );
   rb->head = ((char*)rb->head) + rb->data_sz;
   return 0;

}
/*Note: rb_ push_back does not check for overflows
 * as it is expected the buffer be large enough
 * for the application that overflows just don't
 * happen often. Another reason is that our consumer
 * is faster than our producer since the monitor
 * is just a spin loop.*/
int
rb_push_back( ring_buffer* rb, void* data ){
   void* end = ((char*)rb->buffer) + (rb->length*rb->data_sz);
  
   if( FD_UNLIKELY( rb->tail==end ) ){
     rb->tail=rb->buffer;
     memcpy( rb->tail, data, rb->data_sz );
     return 0;
   }
    memcpy( rb->tail, data, rb->data_sz );
    rb->tail = ((char*)rb->tail) + rb->data_sz;
  
  return 0;
}

void
rb_free( ring_buffer* rb, fd_alloc_t* alloc ){
  fd_alloc_free( alloc, rb->buffer );
}
#endif /* HEADER_fd_src_app_shared_commands_fdtop_fdtop_h */
