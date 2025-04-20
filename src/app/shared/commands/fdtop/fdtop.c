
#include "../../../../disco/metrics/fd_metrics.h"
#include "../../../../disco/topo/fd_topo.h"
#include <linux/capability.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../../fd_config.h"
#include "../../fd_cap_chk.h"
#include "../../../../util/log/fd_log.h"
#include "../../../../flamenco//leaders/fd_leaders.h"

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
  } stats;

  struct {
    
   int page_number;

   /* An integer where the first eight bits signify if the corresponding
     monitor at the respective index is enabled or disabled. */
   int monitors;
  } app_state;

} fd_top_t;

void
fdtop_cmd_perm( args_t *         args FD_PARAM_UNUSED,
                fd_cap_chk_t *   chk,
                config_t const * config ) {
  /* Get the maximum limit to lock for the entire topology. */ 
  ulong mlock_limit = fd_topo_mlock( &config->topo );
  
  fd_cap_chk_raise_rlimit(chk,"fdtop", RLIMIT_MEMLOCK, mlock_limit, "call `rlimit(2)` to increase `RLIMIT_MEMLOCK` so all memory can be locked with `mlock(2)`");

  /* Check if system requires CAP_SYS_ADMIN for unprivledged usernamespaces. */
  if( fd_sandbox_requires_cap_sys_admin( config->uid, config->gid ) ){
   fd_cap_chk_cap( chk, "fdtop", CAP_SYS_ADMIN, "call `unshare(2)` with `CLONE_NEWUSER` to sandbox the process in a new user namespace");
  }
  if( FD_LIKELY( getuid()!=config->uid ) ){
    fd_cap_chk_cap( chk, "fdtop", CAP_SETUID, "call `setresuid(2)` to switch uid to the sandbox user");
  }

  if( FD_LIKELY( getgid()!=config->gid ) ){
    fd_cap_chk_cap( chk, "fdtop", CAP_SETGID, "call `setresgid(2)` to switch gid to the sandbox user");
  }
}

void
fdtop_cmd_args( int *    argc,
                char *** argv,
                args_t * args ) {
 args->fdtop.polling_rate_ms = fd_env_strip_cmdline_ulong( argc, argv, "--poll_ms", NULL, 100);
}
