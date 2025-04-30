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
/*#include <notcurses/notcurses.h>*/
#include "../../fd_config.h"
#include "../../fd_cap_chk.h"
#include "../../../../util/log/fd_log.h"
#include "fdtop.h"
#include "box.h"

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
  for( ulong i = 0UL; i<bank_tile_cnt; i++ ){
        ulong tile_idx = fd_topo_find_tile( topo, "bank", i );
        fd_topo_tile_t const * bank = &topo->tiles[ tile_idx ];
        volatile ulong const * bank_metrics = fd_metrics_tile( bank->metrics );
        ulong bank_txn_success = bank_metrics[ MIDX( COUNTER, BANK, SUCCESSFUL_TRANSACTIONS ) ];
        FD_LOG_INFO(( "bank_txn: %lu", bank_txn_success ));
        app->bank.txn_success = bank_txn_success;
  }

}

/*static const unsigned MINROWS = 24;*/
/*static const unsigned MINCOLS = 76;*/

void
draw_monitor( fd_top_t const * app ){
  struct notcurses* nc;
  notcurses_options nopts = { 0 };
  nc = notcurses_init( &nopts, NULL );
  if( FD_UNLIKELY( NULL==nc ) ){
        FD_LOG_ERR(( "notcurses_init() failed" )); 
  }
  unsigned dimx, dimy;
  ncplane_dim_yx( notcurses_stdplane( nc ) , &dimy, &dimx );

  notcurses_drop_planes( nc );
  notcurses_stats_reset(nc, NULL);
  
  struct ncplane* n = notcurses_stdplane(nc);
  uint64_t channels = 0;
  ncchannels_set_fg_rgb(&channels, 0); // explicit black + opaque
  ncchannels_set_bg_rgb(&channels, 0);
  if(ncplane_set_base(n, "", 0, channels)){
    FD_LOG_ERR(( "-1" ));
  }
  ncplane_erase(n);
  hud_create(nc);
  hud_schedule( "hellodbhbydvetyvdev", 0); 
   notcurses_render( nc ); 
   notcurses_stop( nc );
}



void
fdtop_cmd_fn( args_t * args FD_PARAM_UNUSED,
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
   FD_LOG_ERR(( "sem_init(display_sem) lfailed (%i-%s)", errno, fd_io_strerror( errno ) ));
 }
 
 /*char buffer1[MAX_TERMINAL_BUFFER_SIZE];*/
 /*char buffer2[MAX_TERMINAL_BUFFER_SIZE];*/
 
 fd_topo_join_workspaces( &config->topo, FD_SHMEM_JOIN_MODE_READ_ONLY );
 fd_topo_fill( &config->topo );
 fd_top_t app;
 memset( &app, 0, sizeof(app) );
 poll_metrics(&app, &config->topo); 

 draw_monitor(&app); 
}


