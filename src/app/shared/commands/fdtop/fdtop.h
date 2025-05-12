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
    
   int page_number;
   int show_help;

   /* An integer where the first eight bits signify if the corresponding
     monitor at the respective index is enabled or disabled. */
   int monitors;
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





#endif /* HEADER_fd_src_app_shared_commands_fdtop_fdtop_h */
