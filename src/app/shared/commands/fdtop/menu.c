#include <notcurses/notcurses.h>
#include <sys/types.h>
#include "../../../../util/log/fd_log.h"
#include "menu.h"
#include "helpers.h"


void
fdtop_menu_bg_color_scheme( struct ncplane *n ){
  uint64_t channels = 0;
  ncchannels_set_bg_alpha( &channels,  NCALPHA_BLEND );
  ncchannels_set_fg_alpha( &channels,  NCALPHA_BLEND );
  ncchannels_set_bg_rgb( &channels, FD_BLACK );
  if( FD_UNLIKELY( -1==ncplane_set_base( n, "", 0, channels ) ) ){
    FD_LOG_ERR(( "ncplane_set_base failed" ));
  }

}

int
fdtop_menu_refresh( struct ncplane *zero_plane, unsigned ylen FD_PARAM_UNUSED, unsigned xlen FD_PARAM_UNUSED){
    
  /*ncplane_erase( zero_plane );*/


  nccell ul = NCCELL_TRIVIAL_INITIALIZER, ll = NCCELL_TRIVIAL_INITIALIZER;
  nccell lr = NCCELL_TRIVIAL_INITIALIZER, ur = NCCELL_TRIVIAL_INITIALIZER;
  nccell hl = NCCELL_TRIVIAL_INITIALIZER, vl = NCCELL_TRIVIAL_INITIALIZER;


  if(nccells_light_box(zero_plane, NCSTYLE_NONE, fdtop_menu_box_color_scheme(), &ul, &ur, &ll, &lr, &hl, &vl)){
    return -1;
  }
 
  nccell_set_bg_alpha(&ul, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&ur, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&ll, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&lr, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&hl, NCALPHA_OPAQUE);
  nccell_set_bg_alpha(&vl, NCALPHA_OPAQUE);
  if(ncplane_perimeter(zero_plane, &ul, &ur, &ll, &lr, &hl, &vl, 0)){
    nccell_release(zero_plane, &ul); nccell_release(zero_plane, &ur); nccell_release(zero_plane, &hl);
    nccell_release(zero_plane, &ll); nccell_release(zero_plane, &lr); nccell_release(zero_plane, &vl);
    return -1;
  }
  nccell_release(zero_plane, &ul); nccell_release(zero_plane, &ur); nccell_release(zero_plane, &hl);
  nccell_release(zero_plane, &ll); nccell_release(zero_plane, &lr); nccell_release(zero_plane, &vl);
 return 0; 
}

int
fdtop_menu_create( struct notcurses* nc, fd_top_t *app ){
  unsigned ylen, xlen;
  notcurses_term_dim_yx( nc, &ylen, &xlen );

  struct ncplane_options nopts = {
     .y = NCALIGN_TOP,
     .x = NCALIGN_LEFT,
     .rows = ylen - 2,
     .cols = xlen,
     .userptr = NULL,
     .name = "wubba lubba dub dub it's fdtop baby!",
     .resizecb = ncplane_resize_placewithin,
     .flags = NCPLANE_OPTION_HORALIGNED |
              NCPLANE_OPTION_FIXED,
   };
  /*(void)nopts;*/
  struct ncplane *zero_plane = notcurses_stdplane( nc );
  /*ncplane_erase( zero_plane );*/

  /*TODO: Implement pragmatic error handling here. */
  fdtop_menu_bg_color_scheme( zero_plane );
  struct ncplane *base_plane = ncplane_create(
      zero_plane,
      &nopts
      );
  if( FD_UNLIKELY( ( NULL==zero_plane ) ) ){
      FD_LOG_WARNING(( "ncplane_create failed" ));
      return -1;
  }
  fdtop_menu_bg_color_scheme( base_plane );
  fdtop_menu_refresh( base_plane, ylen, xlen );
  fdtop_menu_bar_create( base_plane, xlen, app->app_state.page_number );
  return 0; 
}

#include <wchar.h>

int fdtop_menu_bar_create( struct ncplane *zero_plane , unsigned xlen, int page_number ){
  struct ncplane *one_plane = ncplane_dup( zero_plane, NULL );
  /*ncplane_erase( one_plane );*/

  unsigned int oldy, oldx;
  ncplane_dim_yx( one_plane, &oldy, &oldx );
  ncplane_resize_simple( one_plane, 4, oldx );
  ncplane_cursor_move_yx( one_plane , 0, 0 );
  
  int ret = ncplane_double_box_sized( 
      one_plane, 
      0, 
      fdtop_menu_bar_color_scheme(), 
      3, 
      xlen,  
      NCBOXMASK_TOP   | 
      NCBOXMASK_LEFT  |
      NCBOXMASK_RIGHT | 
      NCBOXCORNER_MASK
  );

  if( FD_UNLIKELY( -1==ret ) ){
      FD_LOG_WARNING(( "ncplane_double_box_sized failed" ));
      ncplane_destroy( one_plane );
      return -1;
  }

  int title_y = 1, title_x = 2;
  
  for( ulong idx = 0; idx<MENU_ITEMS_LEN; idx++ ){
    u_int32_t bg = (ulong)page_number == idx ? FD_MINT : FD_WHITE;
    unsigned int bg_alpha = (ulong)page_number == idx ? NCALPHA_OPAQUE : NCALPHA_TRANSPARENT;
    const char* word = menu_items[ idx ];

    /* Unroll manually since we know word length */
    #pragma unroll(8)
    while( *word ){
      nccell tab_title_c = NCCELL_TRIVIAL_INITIALIZER;
      nccell_prime( 
          one_plane, 
          &tab_title_c, 
          word, 
          0, 
          nc_channels_init( FD_WHITE, bg, NCALPHA_OPAQUE, bg_alpha ) 
          );
      
      ncplane_putc_yx( one_plane, title_y, title_x, &tab_title_c );

      word++;
      title_x++;
    }

    title_x+=sizeof(menu_items[ idx ]);
  }
  return 0;
}
