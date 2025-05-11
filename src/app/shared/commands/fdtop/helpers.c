#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include "notcurses/notcurses.h"
#include "helpers.h"
#include "../../fd_sys_util.h"


/*static inline int*/
/*fd_getchar( void ) {*/
/**/
/*  fd_set read_fds;*/
/*  FD_ZERO( &read_fds );*/
/*  FD_SET( STDIN_FILENO, &read_fds );*/
/*  fd_set except_fds = read_fds;*/
/**/
/*  struct timeval timeout;*/
/*  timeout.tv_sec = 0;*/
/*  timeout.tv_usec = 0;*/
/**/
/*  if( FD_UNLIKELY( -1==select( 1, &read_fds, NULL, &except_fds, &timeout ) ) ) {*/
/*    FD_LOG_ERR(( "select(STDIN_FILENO) failed (%i-%s)", errno, fd_io_strerror( errno ) ));*/
/*  }*/
/**/
/*  int ch = 0;*/
/*  if( FD_UNLIKELY( FD_ISSET( STDIN_FILENO, &read_fds ) || FD_ISSET( STDIN_FILENO, &except_fds ) ) ) {*/
/*    long read_ret = read( STDIN_FILENO, &ch, 1 );*/
/*    if( FD_UNLIKELY( read_ret<0 ) ) {*/
/*      FD_LOG_ERR(( "read(STDIN_FILENO) failed (%i-%s)", errno, fd_io_strerror( errno ) ));*/
/*    } else if( FD_UNLIKELY( !read_ret ) ) {*/
/*      fd_sys_util_exit_group( 0 );*/
/*    }*/
/*  }*/
/**/
/*  return (int)ch;*/
/*}*/


u_int64_t
nc_channels_init( 
    unsigned int fg, 
    unsigned int bg,
    unsigned int fga,
    unsigned int bga ){
  
  uint64_t channels = 0; 
  ncchannels_set_bg_alpha( &channels,  bga );
  ncchannels_set_fg_alpha( &channels,  fga );
  ncchannels_set_bg_rgb( &channels, bg );
  ncchannels_set_fg_rgb( &channels, fg );

  return channels;
}
