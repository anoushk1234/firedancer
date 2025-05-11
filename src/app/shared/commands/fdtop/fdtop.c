#include <notcurses/notcurses.h>
#include <semaphore.h>
#include <pthread.h>
#include <locale.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
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
/*#include "box.h"*/
#include "helpers.h"
/*#include "internal.h"*/
/*#include "fdtop.h"*/
#include "menu.h"


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
 args->fdtop.polling_rate_ms = fd_env_strip_cmdline_ulong( argc, argv, "--poll_ms", NULL, 100UL );
}

static void signal1( int sig ){
  /* Supress warning and exit. */
  (void)sig;
  exit( 0 );
} 

struct sigaction sa = {
  .sa_handler = signal1
};

/*TODO: use ncurses blocking input on sep thread to avoid blocking monitor*/
void*
handle_input( void *arguments ){
  thread_args *args_p = arguments;
  fd_top_t * app = args_p->app;
  struct notcurses *nc = args_p->nc;
  ncinput key;

  for(;;){
  if( FD_UNLIKELY( NULL==nc ) ){
    FD_LOG_WARNING(( "nc NULL" ));
    sleep(2);
    continue;
  }
  notcurses_get_blocking( nc, &key );
  if( FD_UNLIKELY( '\t'==key.id ) ){
     app->app_state.page_number = (int)next_page( app );;
  /*FD_LOG_WARNING(( "key: %i %i %lu %lu", key.id, app->app_state.page_number, np, (ulong)((ulong)(app->app_state.page_number + 1) % MENU_ITEMS_LEN) ));*/
  }
  }
  pthread_exit(NULL);
  return NULL;
}

void*
poll_metrics( void *arguments ){
  thread_args *args_p = arguments;

  fd_topo_t const * topo = args_p->topo;
  fd_top_t * app = args_p->app;
  /*struct notcurses *nc = args_p->nc;*/

  ulong start = get_unix_timestamp_ms();
  for(;;){
    if( ( get_unix_timestamp_ms()-start)<app->polling_rate_ms  ){
       app->bank.txn_success = 0;
        ulong bank_tile_cnt = fd_topo_tile_name_cnt( topo, "bank" );
          for( ulong i = 0UL; i<bank_tile_cnt; i++ ){
                fd_topo_tile_t const * bank = &topo->tiles[ fd_topo_find_tile( topo, "bank", i ) ];
                volatile ulong const * bank_metrics = fd_metrics_tile( bank->metrics );
                ulong bank_txn_success = bank_metrics[ MIDX( COUNTER, BANK, SUCCESSFUL_TRANSACTIONS ) ]; 
                  /*bank_metrics[ MIDX( COUNTER, QUIC, TXNS_RECEIVED_QUIC_FRAG ) ];*/
                /*FD_LOG_INFO(( "bank_txn: %lu", bank_txn_success ));*/
                app->bank.txn_success += bank_txn_success;

          }

        ulong gossip_tile_cnt = fd_topo_tile_name_cnt( topo, "net");
          for( ulong i = 0UL; i<gossip_tile_cnt; i++) {
              fd_topo_tile_t const * gossip_tile = &topo->tiles[ fd_topo_find_tile( topo, "net", i ) ];
              volatile ulong const * gossip_metrics = fd_metrics_tile( gossip_tile->metrics );
              ulong peer_cnt = gossip_metrics[ MIDX( COUNTER, NET, RX_BYTES_TOTAL ) ] +
                MIDX( COUNTER, SOCK, RX_BYTES_TOTAL ) ; 
              /*FD_LOG_INFO(( "GOSSIP_PEER_COUNTS: %lu", peer_cnt ));*/
              app->stats.gossip_peer_cnt += peer_cnt;
          }
       }
    }
  pthread_exit( NULL );
  return NULL;
}

struct termios terminal_backup;

/*void*/
/*restore_terminal( ){*/
/*  (void)tcsetattr( STDIN_FILENO, TCSANOW, &terminal_backup );*/
/*}*/
/**/
void*
draw_monitor( void *arguments ){

  thread_args *args_p = arguments;
  fd_top_t *app = args_p->app;
  struct notcurses *nc = args_p->nc;


  /*args_p->nc = nc;*/
  /*(void)restore_terminal;*/
/*if( FD_UNLIKELY( 0!=tcgetattr( STDIN_FILENO, &terminal_backup ) ) ) {*/
/*    FD_LOG_ERR(( "tcgetattr(STDIN_FILENO) failed (%i-%s)", errno, fd_io_strerror( errno ) ));*/
/*  }*/
/**/
  /* Disable character echo and line buffering */
/*  struct termios term = terminal_backup;*/
/*  term.c_lflag &= (tcflag_t)~(ICANON | ECHO);*/
/*  if( FD_UNLIKELY( 0!=tcsetattr( STDIN_FILENO, TCSANOW, &term ) ) ) {*/
/*    FD_LOG_WARNING(( "tcsetattr(STDIN_FILENO) failed (%i-%s)", errno, fd_io_strerror( errno ) ));*/
/*  }*/

  
  unsigned dimx, dimy;
  ncplane_dim_yx( notcurses_stdplane( nc ) , &dimy, &dimx );

  notcurses_drop_planes( nc );
  notcurses_stats_reset(nc, NULL);
  
  for(;;){
   fdtop_menu_create( nc, app );
  /*hud_create(nc);*/
  /*char ts = (char)(app->bank.txn_success + 30);*/
  /*handle_input( nc, app );*/
  /*char ts[1024];*/
  /*sprintf( ts, "%lu\n", app->bank.txn_success );*/
    /*FD_LOG_ERR(( "%s", ts ));*/
  /*hud_schedule( ts,0); */
   notcurses_render( nc );
}
   notcurses_stop( nc );
   pthread_exit(NULL);
   return NULL;
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
 pthread_t t1;
 pthread_t t2;
 pthread_t t3;
 thread_args args_p;

 fd_topo_join_workspaces( &config->topo, FD_SHMEM_JOIN_MODE_READ_ONLY );
 fd_topo_fill( &config->topo );

 fd_top_t app;
 memset( &app, 0, sizeof(app) );
 app.bank.txn_success = 0;
 if( FD_UNLIKELY( NULL==setlocale( LC_ALL, "" ) ) ){
        FD_LOG_ERR(( "setlocale( LC_ALL ) failed" ));
  }
  struct notcurses* nc;
  notcurses_options nopts = { 0 };
#ifdef FD_DEBUG_MODE
  FILE *fp;
  if( FD_UNLIKELY( NULL==(fp = fopen( "/dev/tty1", "a+" )) ) ){
        FD_LOG_ERR(( " fopen(/dev/tty1) failed" ));
  }
  FD_LOG_WARNING(( "debug mode" ));
  nc = notcurses_init( &nopts, fp );
#else
  FD_LOG_WARNING(( "not debug mode" ));
  nc = notcurses_init(  &nopts, NULL );
#endif
  if( FD_UNLIKELY( NULL==nc ) ){
        FD_LOG_ERR(( "notcurses_init() failed" )); 
  }

 args_p.app = &app;
 args_p.topo = &config->topo;
 args_p.nc = nc;

 pthread_create( &t1, NULL, &poll_metrics, &args_p );
 pthread_create( &t2, NULL, &draw_monitor, &args_p );
 pthread_create( &t3, NULL, &handle_input, &args_p );
/*(void)t3;*/
 pthread_join( t2, NULL);
}


