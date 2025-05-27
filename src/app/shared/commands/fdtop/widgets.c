#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <unistd.h>
#include "widgets.h"
#include "fdtop.h"
#include "menu.h"
#include "helpers.h"
#include "../../../../util/log/fd_log.h"

u_int64_t xcnt = 0;
int
fdtop_plot_graph( struct ncplane *parent, const char * title, unsigned int rows FD_PARAM_UNUSED, unsigned int cols FD_PARAM_UNUSED, int y, int x, ring_buffer* gossip_msg_rx ){
  ncplane_options nopts = {
    .y = y,
    .x = x,
    .rows = rows,
    .cols = cols,
    .userptr = NULL,
    .name = title,
    .resizecb = ncplane_resize_placewithin,
    /*.flags = NCPLANE_OPTION_AUTOGROW,*/
      /*.flags = NCPLANE_OPTION_HORALIGNED | */
      /*       NCPLANE_OPTION_VERALIGNED*/
             /*NCPLANE_OPTION_FIXED,*/
  };
  
  /*struct ncplane* gp1 = ncplane_create( parent, &nopts);*/
  struct ncplane* gplane = ncplane_create(  parent, &nopts);

  uint16_t style = 0;
  uint64_t channels = 0;
  ncchannels_set_fg_alpha(&channels, NCALPHA_OPAQUE);
  ncchannels_set_fg_rgb(&channels, FD_MINT);
  ncchannels_set_bg_alpha(&channels, NCALPHA_OPAQUE);
  ncchannels_set_bg_rgb(&channels, FD_BLACK);
  ncplane_set_base( gplane , "", style, channels );

  ncplot_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.flags = NCPLOT_OPTION_LABELTICKSD |
               NCPLOT_OPTION_EXPONENTIALD |
               NCPLOT_OPTION_DETECTMAXONLY |
               NCPLOT_OPTION_PRINTSAMPLE;
  opts.gridtype = NCBLIT_BRAILLE;
  opts.legendstyle = NCSTYLE_ITALIC | NCSTYLE_BOLD;
  opts.title = title;

  /* TODO: Change these channels */
  ncchannels_set_fg_rgb8(&opts.minchannels, 0x80, 0x80, 0xff);
  ncchannels_set_bg_rgb(&opts.minchannels, 0x201040);
  ncchannels_set_bg_alpha(&opts.minchannels, NCALPHA_BLEND);
  ncchannels_set_fg_rgb8(&opts.maxchannels, 0x80, 0xff, 0x80);
  ncchannels_set_bg_rgb(&opts.maxchannels, 0x201040);
  ncchannels_set_bg_alpha(&opts.maxchannels, NCALPHA_BLEND);

  /*fdtop_menu_refresh( gplane, rows, cols );*/

  struct ncuplot* ncu = ncuplot_create( gplane, &opts, 0, 0 );
/*u_int64_t ymax = 0;*/
#pragma unroll(8)
  for( ulong i = 0; i<8; i++ ){
#pragma unroll(8)
    for( ulong i = 0; i<8; i++ ){
      ulong yval = 0;
      rb_pop_front( gossip_msg_rx, &yval );
      ncuplot_add_sample( ncu, xcnt, yval );
      xcnt++;
    }
  }
 /*(void)gossip_msg_rx;*/
  /*ncuplot_add_sample( ncu, (u_int64_t)get_unix_timestamp_s(), (u_int64_t)gossip_msg_rx );*/
/*#pragma unroll(10)*/
/*  for( ulong i = 0; i<10; i++ ){*/
/*#pragma unroll(10)*/
/*    for( ulong i = 0; i<10; i++ ){*/
/*      ncuplot_add_sample( ncu, (u_int64_t)rand() % 200, (u_int64_t)rand() % 200 );*/
/*    }*/
/*  }*/
  return 0;
}

int
fdtop_render_stats( struct ncplane* parent, const char* title, unsigned int rows, unsigned int cols, int y, int x , char* tag){
  
  ncplane_options nopts = {
      .y = y,
      .x = x,
      .rows = rows,
      .cols = cols,
      .userptr = NULL,
      .name = title,
      .resizecb = ncplane_resize_placewithin,
      /*.flags = NCPLANE_OPTION_AUTOGROW,*/
        /*.flags = NCPLANE_OPTION_HORALIGNED | */
        /*       NCPLANE_OPTION_VERALIGNED*/
               /*NCPLANE_OPTION_FIXED,*/
    };

  struct ncplane* s_plane = ncplane_create( parent, &nopts );
  if( s_plane ){
  uint64_t channels = 0;
  ncchannels_set_fg_alpha(&channels, NCALPHA_OPAQUE);
  ncchannels_set_fg_rgb(&channels, FD_MINT);
  ncchannels_set_bg_alpha(&channels, NCALPHA_OPAQUE);
  ncchannels_set_bg_rgb(&channels, FD_BLACK);
  ncplane_set_base( s_plane , "", 0, channels );

  /*ncplane_move_top(s_plane);*/
  /*ncplane_rounded_box( s_plane, NCSTYLE_NONE, channels, (unsigned int)y, (unsigned int)x,0 );*/
  nccell ul = NCCELL_TRIVIAL_INITIALIZER, ll = NCCELL_TRIVIAL_INITIALIZER;
  nccell lr = NCCELL_TRIVIAL_INITIALIZER, ur = NCCELL_TRIVIAL_INITIALIZER;
  nccell hl = NCCELL_TRIVIAL_INITIALIZER, vl = NCCELL_TRIVIAL_INITIALIZER;


  if(nccells_light_box(s_plane, NCSTYLE_NONE, fdtop_menu_box_color_scheme(), &ul, &ur, &ll, &lr, &hl, &vl)){
    return -1;
  }
  nccell_set_bg_alpha(&ul, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&ur, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&ll, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&lr, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&hl, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&vl, NCALPHA_OPAQUE);
  if(ncplane_perimeter(s_plane, &ul, &ur, &ll, &lr, &hl, &vl, 0)){
    nccell_release(s_plane, &ul); nccell_release(s_plane, &ur); nccell_release(s_plane, &hl);
    nccell_release(s_plane, &ll); nccell_release(s_plane, &lr); nccell_release(s_plane, &vl);
    return -1;
  }
  nccell_release(s_plane, &ul); nccell_release(s_plane, &ur); nccell_release(s_plane, &hl);
  nccell_release(s_plane, &ll); nccell_release(s_plane, &lr); nccell_release(s_plane, &vl);
  }
  int tag_y = 0, tag_x = (int)cols - 2;
  nccell tab_title_c = NCCELL_TRIVIAL_INITIALIZER;
  nccell_prime( 
          s_plane, 
          &tab_title_c,
          tag, 
          0, 
          nc_channels_init( FD_WHITE, FD_BLACK, NCALPHA_OPAQUE, NCALPHA_OPAQUE ) 
          );
      
  ncplane_putc_yx( s_plane, tag_y, tag_x, &tab_title_c );
  

  int title_y = 0, title_x = 2;
  #pragma unroll(8)
  while( *title ){
  nccell tab_title_c = NCCELL_TRIVIAL_INITIALIZER;
  nccell_prime( 
          s_plane, 
          &tab_title_c, 
          title, 
          0, 
          nc_channels_init( FD_WHITE, FD_MINT, NCALPHA_OPAQUE, NCALPHA_OPAQUE ) 
          );
      
  ncplane_putc_yx( s_plane, title_y, title_x, &tab_title_c );
  title++;
  title_x++;
  }
  return 0;
}

