#ifndef HEADER_fd_src_app_shared_commands_fdtop_menu_h
#define HEADER_fd_src_app_shared_commands_fdtop_menu_h
#include <notcurses/notcurses.h>
#include "fdtop.h"

const static char* menu_items[] = {
  "Main",
  "Tiles",
  "Sys Stats"
};
#define MENU_ITEMS_LEN (ulong)(sizeof(menu_items)/sizeof(char*))

int
fdtop_menu_create( struct notcurses* nc, fd_top_t *app );

int fdtop_menu_refresh( struct ncplane *n, unsigned ylen, unsigned xlen );

uint64_t
fdtop_menu_bg_color_scheme( struct ncplane *n );

FD_FN_CONST static inline uint64_t 
fdtop_menu_box_color_scheme( void ){
  uint64_t channels = 0;
  ncchannels_set_bg_alpha( &channels, NCALPHA_TRANSPARENT );
  ncchannels_set_fg_alpha( &channels, NCALPHA_OPAQUE );
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
fdtop_menu_bar_create( struct ncplane* n, unsigned xlen, int page_number );

FD_FN_CONST static inline ulong
next_page( fd_top_t const *app ){
  return (ulong)(app->app_state.page_number + 1) % MENU_ITEMS_LEN; 
}
#endif /* HEADER_fd_src_app_shared_commands_fdtop_menu_h */
