#include <notcurses/notcurses.h>
/*#include <semaphore.h>*/
#include <pthread.h>
#include <locale.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#include "../../../../disco/keyguard/fd_keyload.h"
#include "../../../../disco/metrics/fd_metrics.h"
#include "../../../../disco/topo/fd_topo.h"
/*#include <cstdlib>*/
#include <linux/capability.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
/*#include <notcurses/notcurses.h>*/
#include "../../../../disco/plugin/fd_plugin.h"
#include "../../fd_config.h"
#include "../../fd_cap_chk.h"
#include "../../../../util/log/fd_log.h"
#include "helpers.h"
#include "widgets.h"
#include "menu.h"


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
 args->fdtop.polling_rate_ms = (long)fd_env_strip_cmdline_ulong( argc, argv, "--poll_ms", NULL, 200 );
}

static void signal1( int sig ){
  /* Supress warning and exit. */
  (void)sig;
  exit( 0 );
}

struct sigaction sa = {
  .sa_handler = signal1
};

void*
handle_input( void *arguments ){
  thread_args *args_p = arguments;
  fd_top_t * app = args_p->app;
  struct notcurses *nc = args_p->nc;

 if( NULL==nc){
    pthread_exit(NULL);
    return NULL;
  }

  ncinput key;
  u_int32_t id;

  while( (id = notcurses_get_blocking( nc, &key )) != (u_int32_t)-1 ){


    if( FD_UNLIKELY( NCKEY_EOF==id ) ){
      break;
    }

    if( FD_LIKELY( '\t'==key.id ) ){
       app->app_state.page_number = (int)next_page( app );
    /*FD_LOG_WARNING(( "key: %i %i %lu %lu", key.id, app->app_state.page_number, np, (ulong)((ulong)(app->app_state.page_number + 1) % MENU_ITEMS_LEN) ));*/
    }


    if( FD_LIKELY( 'h'==key.id ) ){
      app->app_state.show_help ^= 1;
    }
  }
  pthread_exit(NULL);
  return NULL;
}
fdtop_plugin_state_t*
fdtop_plugin_state_poll( fd_top_t* app, fdtop_plugin_state_t* state_t ){
  fd_frag_meta_t* mline = state_t->mcache + fd_mcache_line_idx( state_t->seq, state_t->depth );
  ulong seq_found = fd_frag_meta_seq_query( mline );

  if( FD_UNLIKELY( fd_seq_lt( seq_found, state_t->seq ) ) ){
    return NULL;
  }

  while( FD_LIKELY( fd_seq_eq( seq_found, state_t->seq ) ) ){
    void const* data = fd_chunk_to_laddr_const( state_t->base, mline->chunk );

    ulong sig = mline->sig;
    ulong sz = mline->sz;

//    FD_LOG_WARNING(( "seq=%lu data:%p size:%lu sig:%lu, chunk: %u", seq_found, data, sz, sig, mline->chunk ));
    fd_memcpy( state_t->buf, data, mline->sz );
    FD_COMPILER_MFENCE();

    ulong seq_check = fd_frag_meta_seq_query( mline );
    /* Check if the frag was overrun. */
    if( fd_seq_ne( seq_check, state_t->seq ) ){
      state_t->seq = seq_check; /* Use latest seq */
      return state_t;
    }

    fdtop_on_plugin_message( app, state_t->buf, sig, sz );

    /* Wind up for next iteration */
    state_t->seq = fd_seq_inc( seq_found, 1UL );
    mline = state_t->mcache + fd_mcache_line_idx( state_t->seq, state_t->depth );
    seq_found = fd_frag_meta_seq_query( mline );
  }

  /* Overrun while polling */;
  ulong seq_check = fd_frag_meta_seq_query( mline );
  state_t->seq = seq_check;
  return state_t;
}
void
fdtop_on_plugin_message( fd_top_t* app, uchar const *data, ulong sig, ulong sz){
   (void)sz;
    ulong* msg;
    switch( sig ) {
    case FD_PLUGIN_MSG_SLOT_START:
       msg = (ulong*)(data);
       app->current_slot = msg[ 0 ];
#ifndef FD_DEBUG_MODE
       /*FD_LOG_WARNING(( "Slot started slot=%lu parent_slot=%lu", msg[ 0 ], msg[ 1 ] ));*/
#endif
       break;
    case FD_PLUGIN_MSG_SLOT_ROOTED:
        msg = (ulong*)(data);
        app->rooted_slot = msg[ 0 ];
#ifndef FD_DEBUG_MODE
        FD_LOG_WARNING(( "Slot rooted  slot=%lu", msg[ 0 ] ));
#endif
        break;
      /*case */
    default:
        break;
    }
}
void*
poll_metrics( void *arguments ){
  thread_args *args_p = arguments;

  fd_topo_t * topo = args_p->topo;
  fd_top_t * app = args_p->app;
  void* alloc_mem = app->app_state.alloc_mem;

  app->sock_tile_cnt   = fd_topo_tile_name_cnt( topo, "sock"   );
  app->net_tile_cnt    = fd_topo_tile_name_cnt( topo, "net"    );
  app->quic_tile_cnt   = fd_topo_tile_name_cnt( topo, "quic"   );
  app->verify_tile_cnt = fd_topo_tile_name_cnt( topo, "verify" );
  app->resolv_tile_cnt = fd_topo_tile_name_cnt( topo, "resolv" );
  app->bank_tile_cnt   = fd_topo_tile_name_cnt( topo, "bank"   );
  app->shred_tile_cnt  = fd_topo_tile_name_cnt( topo, "shred"  );
  app->pack_tile_cnt   = fd_topo_tile_name_cnt( topo, "pack"   );

  rb_new( &app->bank.txn_success, alloc_mem, FDTOP_RB_LEN, sizeof(int) );
  fdtop_plugin_state_t state_t = { 0 };
  fd_topo_link_t const* plugin_out_link = &topo->links[ fd_topo_find_link( topo, "plugin_out", 0  ) ];
  void* base = (void*)fd_wksp_containing( plugin_out_link->dcache );
  fdtop_plugin_state_init( &state_t, plugin_out_link->mcache, base );

  for(;;){
    long duration = (fd_log_wallclock()-app->stats.last_poll_ns);

    if( duration >= (long)((long)app->polling_rate_ms * 1000000L) ){
/*FD_LOG_WARNING(( "pubkey: %s", app->identity_key_base58 ));*/
        if( FD_UNLIKELY( fd_keyswitch_state_query( app->keyswitch )==FD_KEYSWITCH_STATE_COMPLETED ) ){
          fd_memcpy( app->identity_key->uc, app->keyswitch->bytes, 32UL );  
          fd_base58_encode_32( app->keyswitch->bytes, NULL, app->identity_key_base58 );
          app->identity_key_base58[ FD_BASE58_ENCODED_32_SZ-1UL ] = '\0';
        }
          for( ulong i = 0UL; i<app->bank_tile_cnt; i++ ){
                fd_topo_tile_t const * bank = &topo->tiles[ fd_topo_find_tile( topo, "bank", i ) ];
                volatile ulong const * bank_metrics = fd_metrics_tile( bank->metrics );
                app->bank.txn_success_last += bank_metrics[ MIDX( COUNTER, BANK, SUCCESSFUL_TRANSACTIONS ) ];

          }

          /* TODO: temporary just to demo the chart */
          rb_push_back( &app->bank.txn_success, &app->bank.txn_success_last );

          ulong pack_cus_consumed_in_block = 0;
          for( ulong i = 0UL; i<app->pack_tile_cnt; i++ ){
                fd_topo_tile_t const * pack = &topo->tiles[ fd_topo_find_tile( topo, "pack", i ) ];
                volatile ulong const * pack_metrics = fd_metrics_tile( pack->metrics );
                pack_cus_consumed_in_block += pack_metrics[ MIDX( GAUGE, PACK, CUS_CONSUMED_IN_BLOCK ) ];
          }
       app->pack.cus_consumed_in_block = pack_cus_consumed_in_block;

        app->stats.quic_conn_cnt = 0UL;

          for( ulong i = 0UL; i<app->quic_tile_cnt; i++ ){
                fd_topo_tile_t const * quic = &topo->tiles[ fd_topo_find_tile( topo, "quic", i ) ];
                volatile ulong const * quic_metrics = fd_metrics_tile( quic->metrics );
                app->stats.quic_conn_cnt += quic_metrics[ MIDX( GAUGE, QUIC, CONNECTIONS_ACTIVE ) ];

          }


          app->prev.net_total_rx_bytes = app->stats.net_total_rx_bytes;
          app->prev.net_total_tx_bytes = app->stats.net_total_tx_bytes;
          app->stats.net_total_rx_bytes = 0UL;
          app->stats.net_total_tx_bytes = 0UL;

          for( ulong i = 0UL; i<app->net_tile_cnt; i++ ){
                fd_topo_tile_t const * net = &topo->tiles[ fd_topo_find_tile( topo, "net", i ) ];
                volatile ulong const * net_metrics = fd_metrics_tile( net->metrics );

                app->stats.net_total_rx_bytes  += net_metrics[ MIDX( COUNTER, NET, RX_BYTES_TOTAL ) ];
                app->stats.net_total_tx_bytes += net_metrics[ MIDX( COUNTER, NET, TX_BYTES_TOTAL ) ];

          }
          for( ulong i=0UL; i<app->sock_tile_cnt; i++ ) {
              fd_topo_tile_t const * sock = &topo->tiles[ fd_topo_find_tile( topo, "sock", i ) ];
              volatile ulong * sock_metrics = fd_metrics_tile( sock->metrics );

               app->stats.net_total_rx_bytes += sock_metrics[ MIDX( COUNTER, SOCK, RX_BYTES_TOTAL ) ];
               app->stats.net_total_tx_bytes += sock_metrics[ MIDX( COUNTER, SOCK, TX_BYTES_TOTAL ) ];
            }

         if( !fdtop_plugin_state_poll( app, &state_t ) ){
            FD_LOG_WARNING(( "fdtop_plugin_state_poll NULL" ));
         };
          app->prev.last_poll_ns = app->stats.last_poll_ns;
          app->stats.last_poll_ns = fd_log_wallclock();
      }

    }
  pthread_exit( NULL );
  rb_free( &app->bank.txn_success);
  return NULL;
}

int
fdtop_help_modal( struct notcurses* nc ){
  unsigned ylen, xlen;
  notcurses_term_dim_yx( nc, &ylen, &xlen );
  struct ncplane_options nopts = {
     .y = NCALIGN_CENTER,
     .x = NCALIGN_CENTER,
     .rows = ylen / 2,
     .cols = xlen / 3,
     .userptr = NULL,
     .name = "help modal",
     .resizecb = ncplane_resize_placewithin,
     .flags = NCPLANE_OPTION_HORALIGNED |
              NCPLANE_OPTION_VERALIGNED,
   };
  struct ncplane *modal_p = ncplane_create(
      notcurses_stdplane( nc ),
      &nopts
      );
  ncplane_set_base( modal_p, "", 0, nc_channels_init(0, FD_BLACK, NCALPHA_BLEND, NCALPHA_BLEND ) );
  if( FD_UNLIKELY( ( NULL==modal_p ) ) ){
      FD_LOG_WARNING(( "ncplane_create failed" ));
      return -1;
  }
/*TODO: cleanup, add hints
   * Temporary shenanigans to display some kind of loading logo purely for
   * aesthetic reasons. Eventually this will become a help menu for new users.
   * */
  struct ncvisual* ncv = ncvisual_from_file( "src/app/shared/commands/fdtop/images/fdgif.gif" );
  if( NULL==ncv ){
      FD_LOG_WARNING(( "ncvisual_from_file failed" ));
    return -1;
  }

  struct ncvisual_options vopts = {
  .n = modal_p,
  .y = NCALIGN_CENTER,
  .x = NCALIGN_CENTER,
  .blitter = NCBLIT_PIXEL,
  .scaling = NCSCALE_STRETCH,
   };
  struct timespec abstime;
  abstime.tv_nsec = 10 * NANOSECS_IN_SEC;
  abstime.tv_sec = 10;
  ncvisual_stream( nc, ncv, 1.0, NULL, &vopts, NULL);

  ncvisual_destroy( ncv );
  ncplane_erase( modal_p );
  return 0;
}
/* TODO: Change gossip to something else because fd doesnt have gossip tile */
int
fdtop_gossip( ring_buffer* gossip_msg_rx, struct ncplane* n, int* widget_y){
  unsigned dimy, dimx;
  ncplane_dim_yx( n, &dimy, &dimx);
  unsigned int rows = ( dimy - MENU_BAR_Y ) / 4;
  unsigned int cols = dimx / 3;
  /*FD_LOG_WARNING(( "wy: %lu", gossip_msg_rx ));*/
  /*ncplane_cursor_move_yx( n, 3, 3 );*/
  fdtop_plot_graph( n, "Txn Success", rows, cols, *widget_y, 1, gossip_msg_rx );
  *widget_y += rows / 2;
  /*fdtop_plot_graph( n, "Txn Success", rows, cols, *widget_y + (int)rows, 1, gossip_msg_rx );*/
  /**widget_y += rows / 2;*/
  /*widget_x += cols*/
  return 0;
}
int
fdtop_validator_stats( int* widget_y, struct ncplane* n, fd_top_t* app ){
  void* alloc_mem = app->app_state.alloc_mem;
  unsigned dimy, dimx;
  ncplane_dim_yx( n, &dimy, &dimx);
  unsigned int rows = ( dimy - MENU_BAR_Y );
  unsigned int cols = dimx / 3;
  /*TODO: Change this to a heap hashmap data sturcture for cleaner control flow
   * map["bank"] = map<keys* [],values*[]>*/
  /* TODO:Make all these into constants*/
  char* bank_keys[5] = { "Current Slot", "Rooted Slot", "Transaction Success", "Ingress( kb/s )", "Egress( kb/s )" };
  char* bank_values[6] = { NULL, NULL, NULL, NULL, NULL, NULL };

  for(ulong i = 0; i<5; i++){
    char* data = &((char*)alloc_mem)[ FDTOP_RB_LEN*sizeof(int) + 8UL * i];
    bank_values[ i ] = data;
  }

  itoa( app->current_slot, bank_values[ 0 ], 10 );
  itoa( app->rooted_slot, bank_values[ 1 ], 10 );
  itoa( app->bank.txn_success_last, bank_values[ 2 ], 10 );
  itoa( (ulong)((double)(app->stats.net_total_rx_bytes - app->prev.net_total_rx_bytes) * 1000000000.0 / (double)(app->stats.last_poll_ns - app->prev.last_poll_ns) ), bank_values[ 3 ], 10 );
  itoa( (ulong)((double)(app->stats.net_total_tx_bytes - app->prev.net_total_tx_bytes) * 1000000000.0 / (double)(app->stats.last_poll_ns - app->prev.last_poll_ns) ), bank_values[ 4 ], 10 );

  char* quic_keys[1] = { "Active Connections" };
  char* quic_values[2] = { NULL, NULL };

  quic_values[0] = malloc(8 * sizeof(char));
  itoa( app->stats.quic_conn_cnt, quic_values[0], 10 );

  fdtop_render_stats( n, "Validator Overview", rows / 3, cols, *widget_y, (dimx / 3) + 2 , "1", bank_keys, bank_values);
  *widget_y+= rows / 3;
  fdtop_render_stats( n, "Quic Stats", rows / 5, cols, *widget_y, (dimx / 3) + 2, "2", quic_keys, quic_values );
  *widget_y = MENU_BAR_Y;
  /*fdtop_render_stats( n, "Shred Stats", rows, cols - WIDGET_MARGIN, *widget_y, (dimx / 3) * 2 + 2, "3");*/
  /**widget_y += rows;*/
  /*fdtop_render_stats( n, "Pack Stats", rows, cols - WIDGET_MARGIN, *widget_y, (dimx / 3) * 2 + 2, "4");*/
  return 0;
}
void*
draw_monitor( void *arguments ){

  thread_args *args_p = arguments;
  fd_top_t *app = args_p->app;
  struct notcurses *nc = args_p->nc;

  if( NULL==nc){
    pthread_exit(NULL);
    return NULL;
  }



  unsigned dimx, dimy;
  ncplane_dim_yx( notcurses_stdplane( nc ) , &dimy, &dimx );

  notcurses_drop_planes( nc );
  notcurses_stats_reset(nc, NULL);


  for(;;){

  app->app_state.widget_y = 0;
  fdtop_menu_create( nc, app );
  app->app_state.widget_y = MENU_BAR_Y;
  if( app->app_state.show_help ){
    fdtop_help_modal( nc );
  }
  /*int widget_y =0;*/
  if( FD_LIKELY( NULL!=app->app_state.base_plane ) ){
    fdtop_gossip( &app->bank.txn_success, app->app_state.base_plane, &app->app_state.widget_y );
     app->app_state.widget_y = MENU_BAR_Y;
    fdtop_validator_stats( &app->app_state.widget_y, app->app_state.base_plane , app );
  }
  notcurses_render( nc );
}
   notcurses_stop( nc );
   pthread_exit(NULL);;
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



 fd_topo_join_workspaces( &config->topo, FD_SHMEM_JOIN_MODE_READ_ONLY );
 fd_topo_fill( &config->topo );


 fd_top_t app = { 0 };
 app.polling_rate_ms = args->fdtop.polling_rate_ms;
 
 fd_memcpy( app.identity_key, fd_keyload_load( config->consensus.identity_path, 1 ), sizeof(fd_pubkey_t) );
 fd_memcpy( app.identity_key_base58, FD_BASE58_ENC_32_ALLOCA( app.identity_key ), FD_BASE58_ENCODED_32_SZ );
 fd_topo_tile_t* gui_tile = &config->topo.tiles[ fd_topo_find_tile( &config->topo, "gui", 0 ) ];
 app.keyswitch = fd_keyswitch_join( fd_topo_obj_laddr( &config->topo, gui_tile->keyswitch_obj_id ) );


 ulong cpu_id = fd_log_cpu_id();
 ulong page_cnt = 1UL;
 fd_wksp_t*  wksp  = fd_wksp_new_anon( "fdtop", fd_cstr_to_shmem_page_sz( "huge" ), 1UL, &page_cnt, &cpu_id,0 ,0 );
 fd_alloc_t* alloc = fd_alloc_join( fd_alloc_new( fd_wksp_alloc_laddr( wksp, fd_alloc_align(), fd_alloc_footprint(), 3 ), 3 ), 0 );
 ulong footprint = rb_footprint( FDTOP_RB_LEN, sizeof(int) );
 ulong max = 0;
 /*TODO: should we handle the error if malloc fails?*/
 void* alloc_mem = fd_alloc_malloc_at_least( alloc, 0, footprint, &max );
 app.app_state.alloc_mem = alloc_mem;



 if( FD_UNLIKELY( NULL==setlocale( LC_ALL, "" ) ) ){
        FD_LOG_ERR(( "setlocale( LC_ALL ) failed" ));
  }
  struct notcurses* nc;
#ifndef FD_DEBUG_MODE
  FD_LOG_WARNING(( "debug mode" ));
  nc = NULL;
#else
  FD_LOG_WARNING(( "not debug mode" ));
  notcurses_options nopts = { 0 };
  nc = notcurses_init(  &nopts, NULL );
  if( FD_UNLIKELY( NULL==nc ) ){
        FD_LOG_ERR(( "notcurses_init()  failed" ));
  }
#endif

 thread_args args_p;
 args_p.app = &app;
 args_p.topo = &config->topo;
 args_p.nc = nc;
 /*args_p.alloc_mem = alloc_mem;*/

#ifndef FD_DEBUG_MODE
 poll_metrics( &args_p );
#else
 pthread_t t1;
 pthread_t t2;
 pthread_t t3;
 pthread_create( &t1, NULL, &poll_metrics, &args_p );
 pthread_create( &t2, NULL, &draw_monitor, &args_p );
 pthread_create( &t3, NULL, &handle_input, &args_p );
 pthread_join( t2, NULL);
#endif
}


