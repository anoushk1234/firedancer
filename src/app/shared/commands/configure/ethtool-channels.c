#include "configure.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

#define NAME "ethtool-channels"

static int
enabled( config_t const * config ) {

  /* if we're running in a network namespace, we configure ethtool on
     the virtual device as part of netns setup, not here */
  if( config->development.netns.enabled ) return 0;

  /* only enable if network stack is XDP */
  if( 0!=strcmp( config->development.net.provider, "xdp" ) ) return 0;

  return 1;
}

static void
init_perm( fd_cap_chk_t *   chk,
           config_t const * config FD_PARAM_UNUSED ) {
  fd_cap_chk_root( chk, NAME, "increase network device channels with `ethtool --set-channels`" );
}

static int
device_is_bonded( const char * device ) {
  char path[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( path, PATH_MAX, NULL, "/sys/class/net/%s/bonding", device ) );
  struct stat st;
  int err = stat( path, &st );
  if( FD_UNLIKELY( err && errno != ENOENT ) )
    FD_LOG_ERR(( "error checking if device `%s` is bonded, stat(%s) failed (%i-%s)",
                 device, path, errno, fd_io_strerror( errno ) ));
  return !err;
}

static void
device_read_slaves( const char * device,
                    char         output[ 4096 ] ) {
  char path[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( path, PATH_MAX, NULL, "/sys/class/net/%s/bonding/slaves", device ) );

  FILE * fp = fopen( path, "r" );
  if( FD_UNLIKELY( !fp ) )
    FD_LOG_ERR(( "error configuring network device, fopen(%s) failed (%i-%s)", path, errno, fd_io_strerror( errno ) ));
  if( FD_UNLIKELY( !fgets( output, 4096, fp ) ) )
    FD_LOG_ERR(( "error configuring network device, fgets(%s) failed (%i-%s)", path, errno, fd_io_strerror( errno ) ));
  if( FD_UNLIKELY( feof( fp ) ) ) FD_LOG_ERR(( "error configuring network device, fgets(%s) failed (EOF)", path ));
  if( FD_UNLIKELY( ferror( fp ) ) ) FD_LOG_ERR(( "error configuring network device, fgets(%s) failed (error)", path ));
  if( FD_UNLIKELY( strlen( output ) == 4095 ) ) FD_LOG_ERR(( "line too long in `%s`", path ));
  if( FD_UNLIKELY( strlen( output ) == 0 ) ) FD_LOG_ERR(( "line empty in `%s`", path ));
  if( FD_UNLIKELY( fclose( fp ) ) )
    FD_LOG_ERR(( "error configuring network device, fclose(%s) failed (%i-%s)", path, errno, fd_io_strerror( errno ) ));
  output[ strlen( output ) - 1 ] = '\0';
}

static void
init_device( const char * device,
             uint         combined_channel_count ) {
  if( FD_UNLIKELY( strlen( device ) >= IF_NAMESIZE ) ) FD_LOG_ERR(( "device name `%s` is too long", device ));
  if( FD_UNLIKELY( strlen( device ) == 0 ) ) FD_LOG_ERR(( "device name `%s` is empty", device ));

  int sock = socket( AF_INET, SOCK_DGRAM, 0 );
  if( FD_UNLIKELY( sock < 0 ) )
    FD_LOG_ERR(( "error configuring network device, socket(AF_INET,SOCK_DGRAM,0) failed (%i-%s)",
                 errno, fd_io_strerror( errno ) ));

  struct ethtool_channels channels = {0};
  channels.cmd = ETHTOOL_GCHANNELS;

  struct ifreq ifr = {0};
  strncpy( ifr.ifr_name, device, IF_NAMESIZE-1 );
  ifr.ifr_data = (void *)&channels;

  if( FD_UNLIKELY( ioctl( sock, SIOCETHTOOL, &ifr ) ) )
    FD_LOG_ERR(( "error configuring network device, ioctl(SIOCETHTOOL,ETHTOOL_GCHANNELS) failed (%i-%s)",
                 errno, fd_io_strerror( errno ) ));

  channels.cmd = ETHTOOL_SCHANNELS;
  if( channels.max_combined ) {
    channels.combined_count = combined_channel_count;
    channels.rx_count       = 0;
    channels.tx_count       = 0;
    FD_LOG_NOTICE(( "RUN: `ethtool --set-channels %s combined %u`", device, combined_channel_count ));
  } else {
    channels.combined_count = 0;
    channels.rx_count       = combined_channel_count;
    channels.tx_count       = combined_channel_count;
    FD_LOG_NOTICE(( "RUN: `ethtool --set-channels %s rx %u tx %u`", device, combined_channel_count, combined_channel_count ));
  }

  if( FD_UNLIKELY( ioctl( sock, SIOCETHTOOL, &ifr ) ) ) {
    if( FD_LIKELY( errno == EBUSY ) )
      FD_LOG_ERR(( "error configuring network device, ioctl(SIOCETHTOOL,ETHTOOL_SCHANNELS) failed (%i-%s). "
                   "This is most commonly caused by an issue with the Intel ice driver on certain versions "
                   "of Ubuntu.  If you are using the ice driver, `sudo dmesg | grep %s` contains "
                   "messages about RDMA, and you do not need RDMA, try running `rmmod irdma` and/or "
                   "blacklisting the irdma kernel module.",
                   errno, fd_io_strerror( errno ), device ));
    else
      FD_LOG_ERR(( "error configuring network device, ioctl(SIOCETHTOOL,ETHTOOL_SCHANNELS) failed (%i-%s)",
                   errno, fd_io_strerror( errno ) ));
  }


  if( FD_UNLIKELY( close( sock ) ) )
    FD_LOG_ERR(( "error configuring network device, close() socket failed (%i-%s)", errno, fd_io_strerror( errno ) ));
}

static void
init( config_t const * config ) {
  /* we need one channel for both TX and RX on the NIC for each QUIC
     tile, but the interface probably defaults to one channel total */
  if( FD_UNLIKELY( device_is_bonded( config->tiles.net.interface ) ) ) {
    /* if using a bonded device, we need to set channels on the
       underlying devices. */
    char line[ 4096 ];
    device_read_slaves( config->tiles.net.interface, line );
    char * saveptr;
    for( char * token=strtok_r( line , " \t", &saveptr ); token!=NULL; token=strtok_r( NULL, " \t", &saveptr ) ) {
      init_device( token, config->layout.net_tile_count );
    }
  } else {
    init_device( config->tiles.net.interface, config->layout.net_tile_count );
  }
}

static configure_result_t
check_device( const char * device,
              uint         expected_channel_count ) {
  if( FD_UNLIKELY( strlen( device ) >= IF_NAMESIZE ) ) FD_LOG_ERR(( "device name `%s` is too long", device ));
  if( FD_UNLIKELY( strlen( device ) == 0 ) ) FD_LOG_ERR(( "device name `%s` is empty", device ));

  int sock = socket( AF_INET, SOCK_DGRAM, 0 );
  if( FD_UNLIKELY( sock < 0 ) )
    FD_LOG_ERR(( "error configuring network device, socket(AF_INET,SOCK_DGRAM,0) failed (%i-%s)",
                 errno, fd_io_strerror( errno ) ));

  struct ethtool_channels channels = {0};
  channels.cmd = ETHTOOL_GCHANNELS;

  struct ifreq ifr = {0};
  strncpy( ifr.ifr_name, device, IF_NAMESIZE );
  ifr.ifr_name[ IF_NAMESIZE - 1 ] = '\0'; // silence linter, not needed for correctness
  ifr.ifr_data = (void *)&channels;

  int  supports_channels = 1;
  uint current_channels  = 0;
  if( FD_UNLIKELY( ioctl( sock, SIOCETHTOOL, &ifr ) ) ) {
    if( FD_LIKELY( errno == EOPNOTSUPP ) ) {
      /* network device doesn't support setting number of channels, so
         it must always be 1 */
      supports_channels = 0;
      current_channels  = 1;
    } else {
      FD_LOG_ERR(( "error configuring network device `%s`, ioctl(SIOCETHTOOL,ETHTOOL_GCHANNELS) failed (%i-%s)",
                   device, errno, fd_io_strerror( errno ) ));
    }
  }

  if( FD_UNLIKELY( close( sock ) ) )
    FD_LOG_ERR(( "error configuring network device, close() socket failed (%i-%s)", errno, fd_io_strerror( errno ) ));

  if( channels.combined_count ) {
    current_channels = channels.combined_count;
  } else if( channels.rx_count || channels.tx_count ) {
    if( FD_UNLIKELY( channels.rx_count != channels.tx_count ) ) {
      NOT_CONFIGURED( "device `%s` has unbalanced channel count: (got %u rx, %u tx, expected %u)",
                      device, channels.rx_count, channels.tx_count, expected_channel_count );
    }
    current_channels = channels.rx_count;
  }

  if( FD_UNLIKELY( current_channels != expected_channel_count ) ) {
    if( FD_UNLIKELY( !supports_channels ) ) {
      FD_LOG_ERR(( "Network device `%s` does not support setting number of channels, "
                   "but you are running with more than one net tile (expected {%u}), "
                   "and there must be one channel per tile. You can either use a NIC "
                   "that supports multiple channels, or run Firedancer with only one "
                   "net tile. You can configure Firedancer to run with only one QUIC "
                   "tile by setting `layout.net_tile_count` to 1 in your "
                   "configuration file. It is not recommended to do this in production "
                   "as it will limit network performance.",
                   device, expected_channel_count ));
    } else {
      NOT_CONFIGURED( "device `%s` does not have right number of channels (got %u but "
                      "expected %u)",
                      device, current_channels, expected_channel_count );
    }
  }

  CONFIGURE_OK();
}

static configure_result_t
check( config_t const * config ) {
  if( FD_UNLIKELY( device_is_bonded( config->tiles.net.interface ) ) ) {
    char line[ 4096 ];
    device_read_slaves( config->tiles.net.interface, line );
    char * saveptr;
    for( char * token=strtok_r( line, " \t", &saveptr ); token!=NULL; token=strtok_r( NULL, " \t", &saveptr ) ) {
      CHECK( check_device( token, config->layout.net_tile_count ) );
    }
  } else {
    CHECK( check_device( config->tiles.net.interface, config->layout.net_tile_count ) );
  }

  CONFIGURE_OK();
}

configure_stage_t fd_cfg_stage_ethtool_channels = {
  .name            = NAME,
  .always_recreate = 0,
  .enabled         = enabled,
  .init_perm       = init_perm,
  .fini_perm       = NULL,
  .init            = init,
  .fini            = NULL,
  .check           = check,
};

#undef NAME
