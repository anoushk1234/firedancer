#ifndef HEADER_fd_src_app_shared_commands_fdtop_helpers_h
#define HEADER_fd_src_app_shared_commands_fdtop_helpers_h
#include "time.h"
#include "errno.h"
#include "../../../../util/log/fd_log.h"

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


#endif /* HEADER_fd_src_app_shared_commands_fdtop_helpers_h  */
