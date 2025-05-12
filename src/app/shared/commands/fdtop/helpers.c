#include "notcurses/notcurses.h"
#include "helpers.h"
#include "../../fd_sys_util.h"



u_int64_t
nc_channels_init( 
    unsigned int fg, 
    unsigned int bg,
    unsigned int fga,
    unsigned int bga ){
  
  uint64_t channels = 0; 
  bga && ncchannels_set_bg_alpha( &channels,  bga );
  fga && ncchannels_set_fg_alpha( &channels,  fga );
  bg  && ncchannels_set_bg_rgb( &channels, bg );
  fg  && ncchannels_set_fg_rgb( &channels, fg );

  return channels;
}
