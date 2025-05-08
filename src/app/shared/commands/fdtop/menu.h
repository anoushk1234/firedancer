#include <notcurses/notcurses.h>
#include "helpers.h"
#include "../../../../util/log/fd_log.h"

int
fdtop_menu_create( struct notcurses* nc );

int fdtop_menu_refresh( struct ncplane *n, unsigned ylen, unsigned xlen );

void
fdtop_menu_bg_color_scheme( struct ncplane *n );

FD_FN_CONST static inline uint64_t 
fdtop_menu_box_color_scheme( void ){
  uint64_t channels = 0;
  ncchannels_set_bg_alpha( &channels, NCALPHA_TRANSPARENT );
  ncchannels_set_fg_alpha( &channels, NCALPHA_BLEND );
  ncchannels_set_bg_rgb( &channels, FD_BLACK );
  ncchannels_set_fg_rgb( &channels, FD_MINT ); 
  return channels;
}

FD_FN_CONST static inline uint64_t
fdtop_menu_bar_color_scheme( void ){
  uint64_t channels = 0;
  ncchannels_set_bg_alpha( &channels, NCALPHA_TRANSPARENT );
  ncchannels_set_fg_alpha( &channels, NCALPHA_OPAQUE );
  ncchannels_set_fg_rgb( &channels, FD_MINT );
  return channels;
}

int
fdtop_menu_bar_create( struct ncplane* n , unsigned ylen, unsigned xlen );
