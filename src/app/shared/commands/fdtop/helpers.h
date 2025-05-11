#ifndef HEADER_fd_src_app_shared_commands_fdtop_helpers_h
#define HEADER_fd_src_app_shared_commands_fdtop_helpers_h
#include <termios.h>
#include <sys/types.h>
#include "time.h"
#include "errno.h"
#include "../../../../util/log/fd_log.h"

u_int64_t
nc_channels_init( 
    unsigned int fg, 
    unsigned int bg,
    unsigned int fga,
    unsigned int bga );

time_t static inline
get_unix_timestamp_s( void ){
  time_t now = time( NULL );
  
  if( FD_UNLIKELY( (time_t)-1==now ) ){
      FD_LOG_ERR(( "time(NULL) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
  }

  return now;
}

ulong static inline
get_unix_timestamp_ms( void ){
  return (ulong)get_unix_timestamp_s() * 1000;
}

/* fd_getchar does a non-blocking read of one byte from stdin.
   Returns the byte in [1,256) on success. Returns 0 if stdin was not
   ready for reading (select(2)) or a null byte was read. */
/*static int*/
/*fd_getchar( void );*/

void
restore_terminal( void );

#endif /* HEADER_fd_src_app_shared_commands_fdtop_helpers_h  */
