#include "../../../shared/commands/configure/configure.h"

#include "../../../shared/fd_file_util.h"

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#define NAME "keys"

void FD_FN_SENSITIVE
generate_keypair( char const *     keyfile,
                  config_t const * config,
                  int              use_grnd_random );

static void
init( config_t const * config ) {
  if( FD_UNLIKELY( -1==fd_file_util_mkdir_all( config->scratch_directory, config->uid, config->gid ) ) )
    FD_LOG_ERR(( "could not create scratch directory `%s` (%i-%s)", config->scratch_directory, errno, fd_io_strerror( errno ) ));

  struct stat st;
  if( FD_UNLIKELY( stat( config->consensus.identity_path, &st ) && errno==ENOENT ) )
    generate_keypair( config->consensus.identity_path, config, 0 );

  char faucet[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( faucet, PATH_MAX, NULL, "%s/faucet.json", config->scratch_directory ) );
  generate_keypair( faucet, config, 0 );

  char stake[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( stake, PATH_MAX, NULL, "%s/stake-account.json", config->scratch_directory ) );
  generate_keypair( stake, config, 0 );

  char vote[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( vote, PATH_MAX, NULL, "%s/vote-account.json", config->scratch_directory ) );
  generate_keypair( vote, config, 0 );
}


static void
fini( config_t const * config,
      int              pre_init FD_PARAM_UNUSED ) {
  char path[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( path, PATH_MAX, NULL, "%s/faucet.json", config->scratch_directory ) );
  if( FD_UNLIKELY( unlink( path ) && errno != ENOENT ) )
    FD_LOG_ERR(( "could not remove cluster file `%s` (%i-%s)", path, errno, fd_io_strerror( errno ) ));
  FD_TEST( fd_cstr_printf_check( path, PATH_MAX, NULL, "%s/stake-account.json", config->scratch_directory ) );
  if( FD_UNLIKELY( unlink( path ) && errno != ENOENT ) )
    FD_LOG_ERR(( "could not remove cluster file `%s` (%i-%s)", path, errno, fd_io_strerror( errno ) ));
  FD_TEST( fd_cstr_printf_check( path, PATH_MAX, NULL, "%s/vote-account.json", config->scratch_directory ) );
  if( FD_UNLIKELY( unlink( path ) && errno != ENOENT ) )
    FD_LOG_ERR(( "could not remove cluster file `%s` (%i-%s)", path, errno, fd_io_strerror( errno ) ));
}


static configure_result_t
check( config_t const * config ) {
  char faucet[ PATH_MAX ], stake[ PATH_MAX ], vote[ PATH_MAX ];

  FD_TEST( fd_cstr_printf_check( faucet, PATH_MAX, NULL, "%s/faucet.json", config->scratch_directory ) );
  FD_TEST( fd_cstr_printf_check( stake,  PATH_MAX, NULL, "%s/stake-account.json", config->scratch_directory ) );
  FD_TEST( fd_cstr_printf_check( vote,   PATH_MAX, NULL, "%s/vote-account.json", config->scratch_directory ) );

  struct stat st;
  if( FD_UNLIKELY( stat( faucet, &st ) && errno == ENOENT &&
                   stat( stake,  &st ) && errno == ENOENT &&
                   stat( vote,   &st ) && errno == ENOENT ) )
    NOT_CONFIGURED( "not all of faucet.json, stake-account.json, and vote-account.json exist" );

  CHECK( check_dir( config->scratch_directory, config->uid, config->gid, S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR ) );

  CHECK( check_file( faucet, config->uid, config->gid, S_IFREG | S_IRUSR | S_IWUSR ) );
  CHECK( check_file( stake,  config->uid, config->gid, S_IFREG | S_IRUSR | S_IWUSR ) );
  CHECK( check_file( vote,   config->uid, config->gid, S_IFREG | S_IRUSR | S_IWUSR ) );
  CONFIGURE_OK();
}

configure_stage_t fd_cfg_stage_keys = {
  .name            = NAME,
  .always_recreate = 0,
  .enabled         = NULL,
  .init_perm       = NULL,
  .fini_perm       = NULL,
  .init            = init,
  .fini            = fini,
  .check           = check,
};

#undef NAME
