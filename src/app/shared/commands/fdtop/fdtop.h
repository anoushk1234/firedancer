#ifndef HEADER_fd_src_app_shared_commands_fdtop_fdtop_h
#define HEADER_fd_src_app_shared_commands_fdtop_fdtop_h
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include "../../fd_config.h"
#include "../../../../util/log/fd_log.h"
#include "../../../../tango/mcache/fd_mcache.h"
#include "../../../../flamenco/leaders/fd_leaders.h"
/*#include "menu.h"*/

#define FD_BLACK 0x000000
#define FD_WHITE 0xFFFFFF
#define FD_MINT 0x1abfa2


#define NANOSECS_IN_SEC 1000000000ul

#define MENU_BAR_Y 3
#define WIDGET_MARGIN 2

typedef struct {
  void* buffer;
  void* head;
  void* tail;
  /* Note: Assuming every element has the same size.*/
  size_t data_sz;
  size_t length;
} ring_buffer __attribute__((aligned(8)));

/*#define FDTOP_FOOTPRINT FDTOP_RB_LENGTH*/
/*#define FDTOP_RB_FOOTPRINT 128 */
#define FDTOP_RB_LEN 128

FD_FN_CONST static inline ulong
rb_footprint( ulong length, ulong data_sz ){
   return length * data_sz;
}

static inline ring_buffer*
rb_new( ring_buffer* rb, void* alloc_mem, size_t length, size_t data_sz ){

  /*FD_COMPILER_MFENCE();*/
  /*rb->buffer = NULL;*/
  rb->buffer = (char*)alloc_mem;
  /*FD_COMPILER_MFENCE();*/
  memset( rb->buffer, 0, (size_t)rb_footprint( FDTOP_RB_LEN, sizeof(int) ));
  if( FD_UNLIKELY( NULL==rb->buffer ) ){
    return NULL;
  }
  rb->data_sz = data_sz;
  rb->head = rb->buffer;
  rb->tail = rb->buffer;
  rb->length = length;
  return rb;
}
static inline ring_buffer*
rb_pop_front( ring_buffer* rb,  void* data ){
   if( rb->head == rb->tail ){
     return NULL;
   }
   void* end = ((char*)rb->buffer) + (rb->length*rb->data_sz);
   if( FD_UNLIKELY( rb->head==end ) ){
     rb->head = rb->buffer;
     memcpy( data, rb->head, rb->data_sz );
     return rb;
   }
   memcpy( data, rb->head, rb->data_sz );
   rb->head = ((char*)rb->head) + rb->data_sz;
   return rb;
}
/*Note: rb_ push_back does not check for overflows
 * as it is expected that the buffer be large enough
 * for the application that overflows are extremely
 * unlikely. Another reason is that our consumer
 * is faster than our producer since the monitor
 * is just a spin loop.*/
static inline ring_buffer*
rb_push_back( ring_buffer* rb, void* data ){
   void* end = ((char*)rb->buffer) + (rb->length*rb->data_sz);

   if( FD_UNLIKELY( rb->tail==end ) ){
     rb->tail=rb->buffer;
     memcpy( rb->tail, data, rb->data_sz );
     return rb;
   }
    memcpy( rb->tail, data, rb->data_sz );
    rb->tail = ((char*)rb->tail) + rb->data_sz;
     return rb;
}

static inline void
rb_free( ring_buffer* rb ){
  free( rb->buffer );
}


/*TODO: Check if this is aligned by the compiler, if not align manually.*/
typedef struct {
  long polling_rate_ms;

  fd_pubkey_t identity_key;
  char identity_key_base58[ FD_BASE58_ENCODED_32_SZ ];
  ulong next_leader_slot;
  ulong current_slot;
  ulong rooted_slot;

  ulong bank_tile_cnt;
  ulong sock_tile_cnt;
  ulong quic_tile_cnt;
  ulong resolv_tile_cnt;
  ulong shred_tile_cnt;
  ulong net_tile_cnt;
  ulong verify_tile_cnt;
  ulong pack_tile_cnt;

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
   ulong net_total_rx_bytes;
   ulong net_total_tx_bytes;

   ulong gossip_peer_cnt;
   long last_poll_ns;
  } stats;
  struct {
   ulong net_total_rx_bytes;
   ulong net_total_tx_bytes;
   long last_poll_ns;
  } prev;
  struct {
   ring_buffer txn_success;
   ulong txn_success_last;
  } bank;
  struct {
    ulong gossip_msg_rx;
  } gossip;
  struct {
    ulong cus_consumed_in_block;
  } pack;
  struct {

   int page_number;
   int show_help;
   /*Layout:
    * [0..FDTOP_RB_FOOTPRINT*sizeof(int)] - Main page chart one ring buffer
    * [0..8]
    * */
   void* alloc_mem;

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
  fd_topo_t * topo;
  struct notcurses * nc;
  void* alloc_mem;
  /*sem_t *control_t;*/
} thread_args __attribute__((aligned(8)));

void fdtop_cmd_fn( args_t * args, config_t * config );
void fdtop_cmd_args( int * argc, char *** argv, args_t * args );
void fdtop_cmd_perm( args_t * args, fd_cap_chk_t * chk, config_t const * config );
void* draw_monitor( void * arguments );
void* poll_metrics( void * arguments );
void* handle_input( void *arguments );

#define CHART_BUFFER_LEN 32

typedef struct{
 fd_frag_meta_t* mcache;
 /*Base chunk in the cache line, points to the start of the workspace*/
 void* base;
 ulong depth;
 ulong seq;
 uchar buf[ 65536 ];
} fdtop_plugin_state_t;

inline void
fdtop_plugin_state_init(
    fdtop_plugin_state_t* state_t,
    fd_frag_meta_t* mcache,
    void* base
    ){
  state_t->base = base;
  state_t->mcache = mcache;
  state_t->seq = 0UL;
  state_t->depth = fd_mcache_depth( mcache );
}


extern fdtop_plugin_state_t*
fdtop_plugin_state_poll(
    fd_top_t* app,
    fdtop_plugin_state_t* state_t
    );
extern void
fdtop_on_plugin_message( fd_top_t* app, uchar const* data, ulong sig, ulong sz );

#endif /* HEADER_fd_src_app_shared_commands_fdtop_fdtop_h */
