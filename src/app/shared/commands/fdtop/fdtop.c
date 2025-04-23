#include <semaphore.h>
#include <errno.h>
#include "../../../../disco/metrics/fd_metrics.h"
#include "../../../../disco/topo/fd_topo.h"
/*#include <cstdlib>*/
#include <linux/capability.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../../fd_config.h"
#include "../../fd_cap_chk.h"
#include "../../../../util/log/fd_log.h"
#include "./fdtop.h"

/* TODO: Arbitrary number*/
#define MAX_TERMINAL_BUFFER_SIZE 32000


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

static void signal1( int sig ){
  /* Supress warning and exit. */
  (void)sig;
  exit( 0 );
} 

struct sigaction sa = {
  .sa_handler = signal1
};

void
poll_metrics( fd_top_t * const app, fd_topo_t const * topo ){
  ulong bank_tile_cnt = fd_topo_tile_name_cnt( topo, "bank" );
  for( ulong i = 0; i<bank_tile_cnt; i++ ){
        ulong tile_idx = fd_topo_find_tile( topo, "bank", i);
        fd_topo_tile_t const * bank = &topo->tiles[tile_idx];
        volatile ulong * bank_metrics = fd_metrics_tile( bank->metrics );
        ulong bank_txn_success = bank_metrics[ MIDX(COUNTER, BANK, SUCCESSFUL_TRANSACTIONS ) ];
        FD_LOG_DEBUG(( "bank_txn_success: %lu", bank_txn_success ));
        app->bank.txn_success = bank_txn_success;
  }

}

void
monitor_cmd_fn( args_t * args,
                config_t * config ) {
 if( FD_UNLIKELY( sigaction( SIGINT, &sa, NULL ) ) ){
   FD_LOG_ERR(( "sigaction(SIGINT) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
 } 
 if( FD_UNLIKELY( sigaction( SIGTERM, &sa, NULL ) ) ){
   FD_LOG_ERR(( "sigaction(SIGTERM) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
 }

 sem_t metrics_sem;
 sem_t display_sem;

 if( FD_UNLIKELY( -1==sem_init( &metrics_sem, 0, 0 ) ) ){
   FD_LOG_ERR(( "sem_init(metrics_sem) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
 }

 if( FD_UNLIKELY( -1==sem_init( &display_sem, 0, 0 ) ) ){
   FD_LOG_ERR(( "sem_init(metrics_sem) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
 }
 
 char buffer1[MAX_TERMINAL_BUFFER_SIZE];
 char buffer2[MAX_TERMINAL_BUFFER_SIZE];
 
 fd_top_t app;
 memset( &app, 0, sizeof(app) );
 poll_metrics(&app, &config->topo); 

 
}





















