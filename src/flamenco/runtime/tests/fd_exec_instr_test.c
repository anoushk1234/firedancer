
#include "generated/invoke.pb.h"
#undef FD_SPAD_USE_HANDHOLDING
#define FD_SPAD_USE_HANDHOLDING 1
#include "fd_exec_instr_test.h"
#include "../fd_acc_mgr.h"
#include "../fd_executor.h"
#include "../fd_hashes.h"
#include "../fd_runtime.h"
#include "../program/fd_bpf_loader_serialization.h"
#include "../../stakes/fd_stakes.h"
#include "../../rewards/fd_rewards.h"
#include "../program/fd_bpf_loader_program.h"
#include "../program/fd_vote_program.h"
#include "../program/fd_stake_program.h"
#include "../program/fd_bpf_program_util.h"
#include "../program/fd_builtin_programs.h"
#include "../context/fd_exec_epoch_ctx.h"
#include "../context/fd_exec_slot_ctx.h"
#include "../context/fd_exec_txn_ctx.h"
#include "../sysvar/fd_sysvar_recent_hashes.h"
#include "../sysvar/fd_sysvar_last_restart_slot.h"
#include "../sysvar/fd_sysvar_slot_hashes.h"
#include "../sysvar/fd_sysvar_stake_history.h"
#include "../sysvar/fd_sysvar_epoch_rewards.h"
#include "../../../funk/fd_funk.h"
#include "../../../util/bits/fd_float.h"
#include "../../../ballet/sbpf/fd_sbpf_loader.h"
#include "../../vm/fd_vm.h"
#include <assert.h>
#include "../sysvar/fd_sysvar_cache.h"
#include "../sysvar/fd_sysvar_epoch_schedule.h"
#include "../sysvar/fd_sysvar_clock.h"
#include "../../../disco/pack/fd_pack.h"
#include "fd_vm_test.h"
#include "../../vm/test_vm_util.h"

/* LOGFMT_REPORT is the log prefix for instruction processing tests */

#define LOGFMT_REPORT "%s"
static FD_TL char _report_prefix[100] = {0};

#define REPORTV( level, fmt, ... ) \
  FD_LOG_##level(( LOGFMT_REPORT fmt, _report_prefix, __VA_ARGS__ ))

#define REPORT( level, fmt ) \
  FD_LOG_##level(( LOGFMT_REPORT fmt, _report_prefix ))

#define REPORT_ACCTV( level, addr, fmt, ... )                                  \
  do {                                                                         \
    char         _acct_log_private_addr[ FD_BASE58_ENCODED_32_SZ ];            \
    void const * _acct_log_private_addr_ptr = (addr);                          \
    fd_acct_addr_cstr( _acct_log_private_addr, _acct_log_private_addr_ptr );        \
    REPORTV( level, " account %s: " fmt, _acct_log_private_addr, __VA_ARGS__ ); \
  } while(0);

#define REPORT_ACCT( level, addr, fmt ) REPORT_ACCTV( level, addr, fmt, 0 )

/* Macros to append data to construct a serialized transaction
   without exceeding bounds */
#define FD_CHECKED_ADD_TO_TXN_DATA( _begin, _cur_data, _to_add, _sz ) __extension__({ \
  if( FD_UNLIKELY( (*_cur_data)+_sz>_begin+FD_TXN_MTU ) ) return ULONG_MAX;           \
  fd_memcpy( *_cur_data, _to_add, _sz );                                              \
  *_cur_data += _sz;                                                                  \
})

#define FD_CHECKED_ADD_CU16_TO_TXN_DATA( _begin, _cur_data, _to_add ) __extension__({ \
  do {                                                                                \
    uchar _buf[3];                                                                    \
    fd_bincode_encode_ctx_t _encode_ctx = { .data = _buf, .dataend = _buf+3 };        \
    fd_bincode_compact_u16_encode( &_to_add, &_encode_ctx );                          \
    ulong _sz = (ulong) ((uchar *)_encode_ctx.data - _buf );                          \
    FD_CHECKED_ADD_TO_TXN_DATA( _begin, _cur_data, _buf, _sz );                       \
  } while(0);                                                                         \
})

/* Define routine to sort accounts to support query-by-pubkey via
   binary search. */

#define SORT_NAME sort_pubkey_p
#define SORT_KEY_T void const *
#define SORT_BEFORE(a,b) ( memcmp( (a), (b), sizeof(fd_pubkey_t) )<0 )
#include "../../../util/tmpl/fd_sort.c"
#include "../../vm/fd_vm_base.h"

ulong
fd_exec_instr_test_runner_align( void ) {
  return alignof(fd_exec_instr_test_runner_t);
}

ulong
fd_exec_instr_test_runner_footprint( void ) {
  ulong txn_max = 4+fd_tile_cnt();
  ulong rec_max = 1024UL;

  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, fd_exec_instr_test_runner_align(), sizeof(fd_exec_instr_test_runner_t) );
  l = FD_LAYOUT_APPEND( l, fd_funk_align(),                   fd_funk_footprint( txn_max, rec_max)                 );
  return FD_LAYOUT_FINI( l, fd_exec_instr_test_runner_align() );
}

fd_exec_instr_test_runner_t *
fd_exec_instr_test_runner_new( void * mem,
                               void * spad_mem,
                               ulong  wksp_tag ) {
  ulong txn_max = 4+fd_tile_cnt();
  ulong rec_max = 1024UL;

  FD_SCRATCH_ALLOC_INIT( l, mem );
  void * runner_mem = FD_SCRATCH_ALLOC_APPEND( l, fd_exec_instr_test_runner_align(), sizeof(fd_exec_instr_test_runner_t) );
  void * funk_mem   = FD_SCRATCH_ALLOC_APPEND( l, fd_funk_align(),                   fd_funk_footprint( txn_max, rec_max) );
  FD_SCRATCH_ALLOC_FINI( l, fd_exec_instr_test_runner_align() );

  fd_funk_t * funk = fd_funk_join( fd_funk_new( funk_mem, wksp_tag, (ulong)fd_tickcount(), txn_max, rec_max ) );
  if( FD_UNLIKELY( !funk ) ) {
    FD_LOG_WARNING(( "fd_funk_new() failed" ));
    return NULL;
  }

  fd_exec_instr_test_runner_t * runner = runner_mem;
  runner->funk = funk;

  /* Create spad */
  runner->spad = fd_spad_join( fd_spad_new( spad_mem, FD_RUNTIME_TRANSACTION_EXECUTION_FOOTPRINT_FUZZ ) );
  return runner;
}

void *
fd_exec_instr_test_runner_delete( fd_exec_instr_test_runner_t * runner ) {
  if( FD_UNLIKELY( !runner ) ) return NULL;
  fd_funk_delete( fd_funk_leave( runner->funk ) );
  runner->funk = NULL;
  if( FD_UNLIKELY( fd_spad_verify( runner->spad ) ) ) {
    FD_LOG_ERR(( "fd_spad_verify() failed" ));
  }
  if( FD_UNLIKELY( fd_spad_frame_used( runner->spad )!=0 ) ) {
    FD_LOG_ERR(( "stray spad frame frame_used=%lu", fd_spad_frame_used( runner->spad ) ));
  }
  runner->spad = NULL;
  return runner;
}

fd_spad_t *
fd_exec_instr_test_runner_get_spad( fd_exec_instr_test_runner_t * runner ) {
  return runner->spad;
}

static int
fd_double_is_normal( double dbl ) {
  ulong x = fd_dblbits( dbl );
  int is_denorm =
    ( fd_dblbits_bexp( x ) == 0 ) &
    ( fd_dblbits_mant( x ) != 0 );
  int is_inf =
    ( fd_dblbits_bexp( x ) == 2047 ) &
    ( fd_dblbits_mant( x ) ==    0 );
  int is_nan =
    ( fd_dblbits_bexp( x ) == 2047 ) &
    ( fd_dblbits_mant( x ) !=    0 );
  return !( is_denorm | is_inf | is_nan );
}

static int
_load_account( fd_txn_account_t *                acc,
               fd_funk_t *                       funk,
               fd_funk_txn_t *                   funk_txn,
               fd_exec_test_acct_state_t const * state,
               uchar                             override_acct_state ) {
  fd_txn_account_init( acc );
  ulong size = 0UL;
  if( state->data ) size = state->data->size;

  fd_pubkey_t pubkey[1];  memcpy( pubkey, state->address, sizeof(fd_pubkey_t) );

  /* Account must not yet exist */
  if( FD_UNLIKELY( !override_acct_state && fd_funk_get_acc_meta_readonly( funk, funk_txn, pubkey, NULL, NULL, NULL) ) ) {
    return 0;
  }

  assert( funk );
  assert( funk->magic == FD_FUNK_MAGIC );
  int err = fd_txn_account_init_from_funk_mutable( /* acc         */ acc,
                                                   /* pubkey      */ pubkey,
                                                   /* funk        */ funk,
                                                   /* txn         */ funk_txn,
                                                   /* do_create   */ 1,
                                                   /* min_data_sz */ size );
  assert( err==FD_ACC_MGR_SUCCESS );
  if( state->data ) {
    acc->vt->set_data( acc, state->data->bytes, size );
  }

  acc->starting_lamports = state->lamports;
  acc->starting_dlen     = size;
  acc->vt->set_lamports( acc, state->lamports );
  acc->vt->set_executable( acc, state->executable );
  acc->vt->set_rent_epoch( acc, state->rent_epoch );
  acc->vt->set_owner( acc, (fd_pubkey_t const *)state->owner );

  /* make the account read-only by default */
  acc->vt->set_readonly( acc );

  fd_txn_account_mutable_fini( acc, funk, funk_txn );

  return 1;
}

static int
_load_txn_account( fd_txn_account_t *                acc,
                   fd_funk_t *                       funk,
                   fd_funk_txn_t *                   funk_txn,
                   fd_exec_test_acct_state_t const * state,
                   uchar                             override_acct_state ) {
  // In the Agave transaction fuzzing harness, accounts with 0 lamports are not saved in the accounts db.
  // When they are fetched for transactions, the fields of the account are 0-set.
  fd_exec_test_acct_state_t account_state_to_save = FD_EXEC_TEST_ACCT_STATE_INIT_ZERO;
  memcpy( account_state_to_save.address, state->address, sizeof(fd_pubkey_t) );

  // Restore the account state if it has lamports
  if( state->lamports ) {
    account_state_to_save = *state;
  }

  return _load_account( acc, funk, funk_txn, &account_state_to_save, override_acct_state );
}

static int
_restore_feature_flags( fd_exec_epoch_ctx_t *              epoch_ctx,
                        fd_exec_test_feature_set_t const * feature_set ) {
  fd_features_disable_all( &epoch_ctx->features );
  for( ulong j=0UL; j < feature_set->features_count; j++ ) {
    ulong                   prefix = feature_set->features[j];
    fd_feature_id_t const * id     = fd_feature_id_query( prefix );
    if( FD_UNLIKELY( !id ) ) {
      FD_LOG_WARNING(( "unsupported feature ID 0x%016lx", prefix ));
      return 0;
    }
    /* Enabled since genesis */
    fd_features_set( &epoch_ctx->features, id, 0UL );
  }
  return 1;
}

/* Serializes a Protobuf SanitizedTransaction and returns the number of bytes consumed.
   Returns ULONG_MAX if the number of bytes read exceeds 1232 (FD_TXN_MTU).
   _txn_raw_begin is assumed to be a pre-allocated buffer of at least 1232 bytes. */
ulong
_serialize_txn( uchar * txn_raw_begin,
                const fd_exec_test_sanitized_transaction_t * tx,
                ushort * out_instr_cnt,
                ushort * out_addr_table_cnt ) {
  const uchar empty_bytes[64] = { 0 };
  uchar * txn_raw_cur_ptr = txn_raw_begin;

  /* Compact array of signatures (https://solana.com/docs/core/transactions#transaction)
     Note that although documentation interchangably refers to the signature cnt as a compact-u16
     and a u8, the max signature cnt is capped at 48 (due to txn size limits), so u8 and compact-u16
     is represented the same way anyways and can be parsed identically. */
  // Note: always create a valid txn with 1+ signatures, add an empty signature if none is provided
  uchar signature_cnt = fd_uchar_max( 1, (uchar) tx->signatures_count );
  FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &signature_cnt, sizeof(uchar) );
  for( uchar i = 0; i < signature_cnt; ++i ) {
    FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, tx->signatures && tx->signatures[i] ? tx->signatures[i]->bytes : empty_bytes, FD_TXN_SIGNATURE_SZ );
  }

  /* Message */
  /* For v0 transactions, the highest bit of the num_required_signatures is set, and an extra byte is used for the version.
     https://solanacookbook.com/guides/versioned-transactions.html#versioned-transactions-transactionv0

     We will always create a transaction with at least 1 signature, and cap the signature count to 127 to avoid
     collisions with the header_b0 tag. */
  uchar num_required_signatures = fd_uchar_max( 1, fd_uchar_min( 127, (uchar) tx->message.header.num_required_signatures ) );
  if( !tx->message.is_legacy ) {
    uchar header_b0 = (uchar) 0x80UL;
    FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &header_b0, sizeof(uchar) );
  }

  /* Header (3 bytes) (https://solana.com/docs/core/transactions#message-header) */
  FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &num_required_signatures, sizeof(uchar) );
  FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &tx->message.header.num_readonly_signed_accounts, sizeof(uchar) );
  FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &tx->message.header.num_readonly_unsigned_accounts, sizeof(uchar) );

  /* Compact array of account addresses (https://solana.com/docs/core/transactions#compact-array-format) */
  // Array length is a compact u16
  ushort num_acct_keys = (ushort) tx->message.account_keys_count;
  FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, num_acct_keys );
  for( ushort i = 0; i < num_acct_keys; ++i ) {
    FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, tx->message.account_keys[i]->bytes, sizeof(fd_pubkey_t) );
  }

  /* Recent blockhash (32 bytes) (https://solana.com/docs/core/transactions#recent-blockhash) */
  // Note: add an empty blockhash if none is provided
  FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, tx->message.recent_blockhash ? tx->message.recent_blockhash->bytes : empty_bytes, sizeof(fd_hash_t) );

  /* Compact array of instructions (https://solana.com/docs/core/transactions#array-of-instructions) */
  // Instruction count is a compact u16
  ushort instr_count = (ushort) tx->message.instructions_count;
  FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, instr_count );
  for( ushort i = 0; i < instr_count; ++i ) {
    // Program ID index
    uchar program_id_index = (uchar) tx->message.instructions[i].program_id_index;
    FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &program_id_index, sizeof(uchar) );

    // Compact array of account addresses
    ushort acct_count = (ushort) tx->message.instructions[i].accounts_count;
    FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, acct_count );
    for( ushort j = 0; j < acct_count; ++j ) {
      uchar account_index = (uchar) tx->message.instructions[i].accounts[j];
      FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &account_index, sizeof(uchar) );
    }

    // Compact array of 8-bit data
    pb_bytes_array_t * data = tx->message.instructions[i].data;
    ushort data_len;
    if( data ) {
      data_len = (ushort) data->size;
      FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, data_len );
      FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, data->bytes, data_len );
    } else {
      data_len = 0;
      FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, data_len );
    }
  }

  /* Address table lookups (N/A for legacy transactions) */
  ushort addr_table_cnt = 0;
  if( !tx->message.is_legacy ) {
    /* Compact array of address table lookups (https://solanacookbook.com/guides/versioned-transactions.html#compact-array-of-address-table-lookups) */
    // NOTE: The diagram is slightly wrong - the account key is a 32 byte pubkey, not a u8
    addr_table_cnt = (ushort) tx->message.address_table_lookups_count;
    FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, addr_table_cnt );
    for( ushort i = 0; i < addr_table_cnt; ++i ) {
      // Account key
      FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, tx->message.address_table_lookups[i].account_key, sizeof(fd_pubkey_t) );

      // Compact array of writable indexes
      ushort writable_count = (ushort) tx->message.address_table_lookups[i].writable_indexes_count;
      FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, writable_count );
      for( ushort j = 0; j < writable_count; ++j ) {
        uchar writable_index = (uchar) tx->message.address_table_lookups[i].writable_indexes[j];
        FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &writable_index, sizeof(uchar) );
      }

      // Compact array of readonly indexes
      ushort readonly_count = (ushort) tx->message.address_table_lookups[i].readonly_indexes_count;
      FD_CHECKED_ADD_CU16_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, readonly_count );
      for( ushort j = 0; j < readonly_count; ++j ) {
        uchar readonly_index = (uchar) tx->message.address_table_lookups[i].readonly_indexes[j];
        FD_CHECKED_ADD_TO_TXN_DATA( txn_raw_begin, &txn_raw_cur_ptr, &readonly_index, sizeof(uchar) );
      }
    }
  }
  *out_instr_cnt = instr_count;
  *out_addr_table_cnt = addr_table_cnt;
  return (ulong)(txn_raw_cur_ptr - txn_raw_begin);
}


int
fd_exec_test_instr_context_create( fd_exec_instr_test_runner_t *        runner,
                                   fd_exec_instr_ctx_t *                ctx,
                                   fd_exec_test_instr_context_t const * test_ctx,
                                   bool                                 is_syscall ) {
  memset( ctx, 0, sizeof(fd_exec_instr_ctx_t) );

  fd_funk_t * funk = runner->funk;

  /* Generate unique ID for funk txn */

  fd_funk_txn_xid_t xid[1] = {0};
  xid[0] = fd_funk_generate_xid();

  /* Create temporary funk transaction and txn / slot / epoch contexts */

  fd_funk_txn_start_write( funk );
  fd_funk_txn_t * funk_txn = fd_funk_txn_prepare( funk, NULL, xid, 1 );
  fd_funk_txn_end_write( funk );

  ulong vote_acct_max = MAX_TX_ACCOUNT_LOCKS;

  /* Allocate contexts */
  uchar *               epoch_ctx_mem = fd_spad_alloc( runner->spad, fd_exec_epoch_ctx_align(), fd_exec_epoch_ctx_footprint( vote_acct_max ) );
  uchar *               slot_ctx_mem  = fd_spad_alloc( runner->spad,FD_EXEC_SLOT_CTX_ALIGN,  FD_EXEC_SLOT_CTX_FOOTPRINT  );
  uchar *               txn_ctx_mem   = fd_spad_alloc( runner->spad,FD_EXEC_TXN_CTX_ALIGN,   FD_EXEC_TXN_CTX_FOOTPRINT   );

  fd_exec_epoch_ctx_t * epoch_ctx     = fd_exec_epoch_ctx_join( fd_exec_epoch_ctx_new( epoch_ctx_mem, vote_acct_max ) );
  fd_exec_slot_ctx_t *  slot_ctx      = fd_exec_slot_ctx_join ( fd_exec_slot_ctx_new ( slot_ctx_mem, runner->spad ) );
  fd_exec_txn_ctx_t *   txn_ctx       = fd_exec_txn_ctx_join  ( fd_exec_txn_ctx_new  ( txn_ctx_mem ), runner->spad, fd_wksp_containing( runner->spad ) );

  assert( epoch_ctx );
  assert( slot_ctx  );

  ctx->txn_ctx = txn_ctx;

  /* Set up epoch context. Defaults obtained from GenesisConfig::Default() */
  fd_epoch_bank_t * epoch_bank = fd_exec_epoch_ctx_epoch_bank( epoch_ctx );
  epoch_bank->rent.lamports_per_uint8_year = 3480;
  epoch_bank->rent.exemption_threshold = 2;
  epoch_bank->rent.burn_percent = 50;

  /* Set up slot context */

  slot_ctx->epoch_ctx = epoch_ctx;
  slot_ctx->funk_txn  = funk_txn;
  slot_ctx->funk      = funk;

  /* Restore feature flags */

  fd_exec_test_feature_set_t const * feature_set = &test_ctx->epoch_context.features;
  if( !_restore_feature_flags( epoch_ctx, feature_set ) ) {
    return 0;
  }

  /* Restore slot_bank */

  fd_slot_bank_new( &slot_ctx->slot_bank );

  /* Blockhash queue init */
  fd_block_hash_queue_t * blockhash_queue = &slot_ctx->slot_bank.block_hash_queue;
  blockhash_queue->max_age   = FD_BLOCKHASH_QUEUE_MAX_ENTRIES;
  blockhash_queue->last_hash = fd_spad_alloc( runner->spad, FD_HASH_ALIGN, FD_HASH_FOOTPRINT );
  fd_memset( blockhash_queue->last_hash, 0, FD_HASH_FOOTPRINT );

  /* Set up txn context */

  fd_wksp_t * funk_wksp          = fd_funk_wksp( funk );
  fd_wksp_t * runtime_wksp       = fd_wksp_containing( slot_ctx );
  ulong       funk_txn_gaddr     = fd_wksp_gaddr( funk_wksp, funk_txn );
  ulong       funk_gaddr         = fd_wksp_gaddr( funk_wksp, funk );
  ulong       sysvar_cache_gaddr = fd_wksp_gaddr( runtime_wksp, slot_ctx->sysvar_cache );

  fd_exec_txn_ctx_from_exec_slot_ctx( slot_ctx,
                                      txn_ctx,
                                      funk_wksp,
                                      runtime_wksp,
                                      funk_txn_gaddr,
                                      sysvar_cache_gaddr,
                                      funk_gaddr );
  fd_exec_txn_ctx_setup_basic( txn_ctx );

  txn_ctx->compute_unit_limit      = test_ctx->cu_avail;
  txn_ctx->compute_meter           = test_ctx->cu_avail;
  txn_ctx->vote_accounts_pool      = NULL;
  txn_ctx->spad                    = runner->spad;

  /* Set up instruction context */

  fd_instr_info_t * info = fd_spad_alloc( runner->spad, 8UL, sizeof(fd_instr_info_t) );
  assert( info );
  memset( info, 0, sizeof(fd_instr_info_t) );

  if( test_ctx->data ) {
    info->data_sz = (ushort)test_ctx->data->size;
    info->data    = test_ctx->data->bytes;
  }

  /* Prepare borrowed account table (correctly handles aliasing) */

  if( FD_UNLIKELY( test_ctx->accounts_count > MAX_TX_ACCOUNT_LOCKS ) ) {
    REPORT( NOTICE, "too many accounts" );
    return 0;
  }

  /* Load accounts into database */

  fd_txn_account_t * accts = txn_ctx->accounts;
  fd_memset( accts, 0, test_ctx->accounts_count * sizeof(fd_txn_account_t) );
  txn_ctx->accounts_cnt = test_ctx->accounts_count;

  int has_program_id = 0;

  for( ulong j=0UL; j < test_ctx->accounts_count; j++ ) {
    memcpy(  &(txn_ctx->account_keys[j]), test_ctx->accounts[j].address, sizeof(fd_pubkey_t) );
    if( !_load_account( &accts[j], funk, funk_txn, &test_ctx->accounts[j], 0 ) ) {
      return 0;
    }

    fd_txn_account_t * acc = &accts[j];
    if( acc->vt->get_meta( acc ) ) {
      uchar * data = fd_spad_alloc( txn_ctx->spad, FD_ACCOUNT_REC_ALIGN, FD_ACC_TOT_SZ_MAX );
      ulong   dlen = acc->vt->get_data_len( acc );
      fd_memcpy( data, acc->vt->get_meta( acc ), sizeof(fd_account_meta_t)+dlen );
      fd_txn_account_init_from_meta_and_data_readonly( acc,
                                                       (fd_account_meta_t const *)data,
                                                       data + sizeof(fd_account_meta_t) );
    }

    if( !memcmp( accts[j].pubkey, test_ctx->program_id, sizeof(fd_pubkey_t) ) ) {
      has_program_id = 1;
      info->program_id = (uchar)txn_ctx->accounts_cnt;
    }
  }

  /* If the program id is not in the set of accounts it must be added to the set of accounts. */
  if( FD_UNLIKELY( !has_program_id ) ) {
    fd_txn_account_t * program_acc = &accts[ test_ctx->accounts_count ];
    fd_pubkey_t *      program_key = &txn_ctx->account_keys[ txn_ctx->accounts_cnt ];
    fd_txn_account_init( program_acc );
    memcpy( program_key, test_ctx->program_id, sizeof(fd_pubkey_t) );
    memcpy( program_acc->pubkey, test_ctx->program_id, sizeof(fd_pubkey_t) );
    fd_account_meta_t * meta = fd_spad_alloc( txn_ctx->spad, alignof(fd_account_meta_t), sizeof(fd_account_meta_t) );
    fd_account_meta_init( meta );
    program_acc->vt->set_meta_mutable( program_acc, meta );
    info->program_id = (uchar)txn_ctx->accounts_cnt;
    txn_ctx->accounts_cnt++;
  }

  /* Load in executable accounts */
  for( ulong i = 0; i < txn_ctx->accounts_cnt; i++ ) {
    fd_txn_account_t * acc = &accts[i];
    if ( memcmp( acc->vt->get_owner( acc ), fd_solana_bpf_loader_deprecated_program_id.key, sizeof(fd_pubkey_t) ) != 0
      && memcmp( acc->vt->get_owner( acc ), fd_solana_bpf_loader_program_id.key, sizeof(fd_pubkey_t) ) != 0
      && memcmp( acc->vt->get_owner( acc ), fd_solana_bpf_loader_upgradeable_program_id.key, sizeof(fd_pubkey_t) ) != 0
      && memcmp( acc->vt->get_owner( acc ), fd_solana_bpf_loader_v4_program_id.key, sizeof(fd_pubkey_t) ) != 0
    ) {
      continue;
    }

    fd_account_meta_t const * meta = acc->vt->get_meta( acc );
    if (meta == NULL) {
      static const fd_account_meta_t sentinel = { .magic = FD_ACCOUNT_META_MAGIC };
      acc->vt->set_meta_readonly( acc, &sentinel );
      accts[i].starting_lamports = 0UL;
      accts[i].starting_dlen     = 0UL;
      continue;
    }

    if( FD_UNLIKELY( 0 == memcmp(meta->info.owner, fd_solana_bpf_loader_upgradeable_program_id.key, sizeof(fd_pubkey_t)) ) ) {
      int err = 0;
      fd_bpf_upgradeable_loader_state_t * program_loader_state = read_bpf_upgradeable_loader_state_for_program( txn_ctx,
                                                                                                                (ushort)i,
                                                                                                                &err );

      if( FD_UNLIKELY( !program_loader_state ) ) {
        continue;
      }

      if( !fd_bpf_upgradeable_loader_state_is_program( program_loader_state ) ) {
        continue;
      }

      fd_pubkey_t * programdata_acc = &program_loader_state->inner.program.programdata_address;
      if( FD_UNLIKELY( fd_txn_account_init_from_funk_readonly( &txn_ctx->executable_accounts[txn_ctx->executable_cnt],
                                                               programdata_acc,
                                                               txn_ctx->funk,
                                                               txn_ctx->funk_txn ) ) ) {
        continue;
      }
      txn_ctx->executable_cnt++;
    }
  }

  /* Add accounts to bpf program cache */
  fd_bpf_scan_and_create_bpf_program_cache_entry( slot_ctx, funk_txn, runner->spad );

  /* Restore sysvar cache */
  fd_sysvar_cache_restore( slot_ctx->sysvar_cache, funk, funk_txn, runner->spad, runtime_wksp );

  /* Fill missing sysvar cache values with defaults */
  /* We create mock accounts for each of the sysvars and hardcode the data fields before loading it into the account manager */
  /* We use Agave sysvar defaults for data field values */

  /* Clock */
  // https://github.com/firedancer-io/solfuzz-agave/blob/agave-v2.0/src/lib.rs#L466-L474
  if( !slot_ctx->sysvar_cache->has_clock ) {
    slot_ctx->sysvar_cache->has_clock = 1;
    fd_sol_sysvar_clock_t sysvar_clock = {
                                          .slot = 10,
                                          .epoch_start_timestamp = 0,
                                          .epoch = 0,
                                          .leader_schedule_epoch = 0,
                                          .unix_timestamp = 0
                                        };
    memcpy( slot_ctx->sysvar_cache->val_clock, &sysvar_clock, sizeof(fd_sol_sysvar_clock_t) );
  }

  /* Epoch schedule */
  // https://github.com/firedancer-io/solfuzz-agave/blob/agave-v2.0/src/lib.rs#L476-L483
  if ( !slot_ctx->sysvar_cache->has_epoch_schedule ) {
    slot_ctx->sysvar_cache->has_epoch_schedule = 1;
    fd_epoch_schedule_t sysvar_epoch_schedule = {
                                                  .slots_per_epoch = 432000,
                                                  .leader_schedule_slot_offset = 432000,
                                                  .warmup = 1,
                                                  .first_normal_epoch = 14,
                                                  .first_normal_slot = 524256
                                                };
    memcpy( slot_ctx->sysvar_cache->val_epoch_schedule, &sysvar_epoch_schedule, sizeof(fd_epoch_schedule_t) );
  }

  /* Rent */
  // https://github.com/firedancer-io/solfuzz-agave/blob/agave-v2.0/src/lib.rs#L487-L500
  if ( !slot_ctx->sysvar_cache->has_rent ) {
    slot_ctx->sysvar_cache->has_rent = 1;
    fd_rent_t sysvar_rent = {
                              .lamports_per_uint8_year = 3480,
                              .exemption_threshold = 2.0,
                              .burn_percent = 50
                            };
    memcpy( slot_ctx->sysvar_cache->val_rent, &sysvar_rent, sizeof(fd_rent_t) );
  }

  if ( !slot_ctx->sysvar_cache->has_last_restart_slot ) {
    slot_ctx->sysvar_cache->has_last_restart_slot = 1;

    fd_sol_sysvar_last_restart_slot_t restart = {.slot = 5000};

    memcpy( slot_ctx->sysvar_cache->val_last_restart_slot, &restart, sizeof(fd_sol_sysvar_last_restart_slot_t) );
  }

  /* Set slot bank variables */
  slot_ctx->slot_bank.slot = fd_sysvar_cache_clock( slot_ctx->sysvar_cache )->slot;

  /* Handle undefined behavior if sysvars are malicious (!!!) */

  /* Override epoch bank rent setting */
  fd_rent_t const * rent = fd_sysvar_cache_rent( slot_ctx->sysvar_cache );
  if( rent ) {
    epoch_bank->rent = *rent;
  }

  /* Override most recent blockhash if given */
  fd_recent_block_hashes_global_t const * rbh_global = fd_sysvar_cache_recent_block_hashes( slot_ctx->sysvar_cache );
  fd_recent_block_hashes_t rbh[1];
  if( rbh_global ) {
    rbh->hashes = deq_fd_block_block_hash_entry_t_join( fd_wksp_laddr_fast( runtime_wksp, rbh_global->hashes_gaddr ) );
  }

  if( rbh_global && !deq_fd_block_block_hash_entry_t_empty( rbh->hashes ) ) {
    fd_block_block_hash_entry_t const * last = deq_fd_block_block_hash_entry_t_peek_tail_const( rbh->hashes );
    if( last ) {
      *blockhash_queue->last_hash                = last->blockhash;
      slot_ctx->slot_bank.lamports_per_signature = last->fee_calculator.lamports_per_signature;
      slot_ctx->prev_lamports_per_signature      = last->fee_calculator.lamports_per_signature;
    }
  }

  /* Load instruction accounts */

  if( FD_UNLIKELY( test_ctx->instr_accounts_count > MAX_TX_ACCOUNT_LOCKS ) ) {
    REPORT( NOTICE, "too many instruction accounts" );
    return 0;
  }

  uchar acc_idx_seen[ FD_INSTR_ACCT_MAX ] = {0};
  for( ulong j=0UL; j < test_ctx->instr_accounts_count; j++ ) {
    uint index = test_ctx->instr_accounts[j].index;
    if( index >= test_ctx->accounts_count ) {
      REPORTV( NOTICE, " instruction account index out of range (%u > %u)", index, test_ctx->instr_accounts_count );
      return 0;
    }

    fd_txn_account_t * acc = &accts[ index ];

    /* Setup instruction accounts */
    fd_instr_info_setup_instr_account( info,
                                       acc_idx_seen,
                                       (ushort)index,
                                       (ushort)j,
                                       (ushort)j,
                                       test_ctx->instr_accounts[j].is_writable,
                                       test_ctx->instr_accounts[j].is_signer );

    if( test_ctx->instr_accounts[j].is_writable ) {
      acc->vt->set_mutable( acc );
    }
  }
  info->acct_cnt = (uchar)test_ctx->instr_accounts_count;

  //  FIXME: Specifically for CPI syscalls, flag guard this?
  fd_instr_info_sum_account_lamports( info, txn_ctx, &info->starting_lamports_h, &info->starting_lamports_l );

  /* The remaining checks enforce that the program is in the accounts list. */
  bool found_program_id = false;
  for( uint i = 0; i < test_ctx->accounts_count; i++ ) {
    if( 0 == memcmp( test_ctx->accounts[i].address, test_ctx->program_id, sizeof(fd_pubkey_t) ) ) {
      info->program_id = (uchar) i;
      found_program_id = true;
      break;
    }
  }

  /* Early returning only happens in instruction execution. */
  if( !is_syscall && !found_program_id ) {
    FD_LOG_NOTICE(( " Unable to find program_id in accounts" ));
    return 0;
  }

  ctx->funk_txn  = funk_txn;
  ctx->funk      = funk;
  ctx->instr     = info;

  /* Refresh the setup from the updated slot and epoch ctx. */
  fd_exec_txn_ctx_from_exec_slot_ctx( slot_ctx,
                                      txn_ctx,
                                      funk_wksp,
                                      runtime_wksp,
                                      funk_txn_gaddr,
                                      sysvar_cache_gaddr,
                                      funk_gaddr );

  fd_log_collector_init( &ctx->txn_ctx->log_collector, 1 );
  fd_base58_encode_32( txn_ctx->account_keys[ ctx->instr->program_id ].uc, NULL, ctx->program_id_base58 );

  return 1;
}

static fd_execute_txn_task_info_t *
_txn_context_create_and_exec( fd_exec_instr_test_runner_t *      runner,
                              fd_exec_slot_ctx_t *               slot_ctx,
                              fd_exec_test_txn_context_t const * test_ctx ) {
  const uchar empty_bytes[64] = { 0 };
  fd_funk_t * funk = runner->funk;

  /* Generate unique ID for funk txn */

  fd_funk_txn_xid_t xid[1] = {0};
  xid[0] = fd_funk_generate_xid();

  /* Create temporary funk transaction and spad contexts */

  fd_funk_txn_start_write( funk );
  fd_funk_txn_t * funk_txn = fd_funk_txn_prepare( funk, NULL, xid, 1 );
  fd_funk_txn_end_write( funk );

  ulong vote_acct_max = MAX_TX_ACCOUNT_LOCKS;

  /* Allocate contexts */
  uchar *               epoch_ctx_mem = fd_spad_alloc( runner->spad, fd_exec_epoch_ctx_align(), fd_exec_epoch_ctx_footprint( vote_acct_max ) );
  fd_exec_epoch_ctx_t * epoch_ctx     = fd_exec_epoch_ctx_join( fd_exec_epoch_ctx_new( epoch_ctx_mem, vote_acct_max ) );

  assert( epoch_ctx );
  assert( slot_ctx  );

  /* Set up epoch context */
  fd_epoch_bank_t * epoch_bank = fd_exec_epoch_ctx_epoch_bank( epoch_ctx );

  /* Set up slot context */

  slot_ctx->epoch_ctx = epoch_ctx;
  slot_ctx->funk_txn  = funk_txn;
  slot_ctx->funk      = funk;

  /* Restore feature flags */

  fd_exec_test_feature_set_t const * feature_set = &test_ctx->epoch_ctx.features;
  if( !_restore_feature_flags( epoch_ctx, feature_set ) ) {
    return NULL;
  }

  /* Restore slot bank */
  fd_slot_bank_new( &slot_ctx->slot_bank );

  /* Initialize builtin accounts */
  fd_builtin_programs_init( slot_ctx );

  /* Load account states into funk (note this is different from the account keys):
    Account state = accounts to populate Funk
    Account keys = account keys that the transaction needs */
  for( ulong i = 0; i < test_ctx->account_shared_data_count; i++ ) {
    /* Load the accounts into the account manager
       Borrowed accounts get reset anyways - we just need to load the account somewhere */
    FD_TXN_ACCOUNT_DECL(acc);
    _load_txn_account( acc, funk, funk_txn, &test_ctx->account_shared_data[i], 0 );
  }

  /* Restore sysvar cache */
  fd_sysvar_cache_restore( slot_ctx->sysvar_cache, funk, funk_txn, runner->spad, fd_wksp_containing( slot_ctx ) );

  /* Add accounts to bpf program cache */
  fd_bpf_scan_and_create_bpf_program_cache_entry( slot_ctx, funk_txn, runner->spad );

  /* Default slot */
  ulong slot = test_ctx->slot_ctx.slot ? test_ctx->slot_ctx.slot : 10; // Arbitrary default > 0

  /* Set slot bank variables (defaults obtained from GenesisConfig::default() in Agave) */
  slot_ctx->slot_bank.slot                                            = slot;
  slot_ctx->slot_bank.prev_slot                                       = slot_ctx->slot_bank.slot - 1; // Can underflow, but its fine since it will correctly be ULONG_MAX
  slot_ctx->slot_bank.fee_rate_governor.burn_percent                  = 50;
  slot_ctx->slot_bank.fee_rate_governor.min_lamports_per_signature    = 0;
  slot_ctx->slot_bank.fee_rate_governor.max_lamports_per_signature    = 0;
  slot_ctx->slot_bank.fee_rate_governor.target_lamports_per_signature = 10000;
  slot_ctx->slot_bank.fee_rate_governor.target_signatures_per_slot    = 20000;
  slot_ctx->slot_bank.lamports_per_signature                          = 5000;
  slot_ctx->prev_lamports_per_signature                               = 5000;

  /* Set epoch bank variables if not present (defaults obtained from GenesisConfig::default() in Agave) */
  fd_epoch_schedule_t default_epoch_schedule = {
                                                .slots_per_epoch             = 432000,
                                                .leader_schedule_slot_offset = 432000,
                                                .warmup                      = 1,
                                                .first_normal_epoch          = 14,
                                                .first_normal_slot           = 524256
                                               };
  fd_rent_t           default_rent           = {
                                                .lamports_per_uint8_year     = 3480,
                                                .exemption_threshold         = 2.0,
                                                .burn_percent                = 50
                                               };
  epoch_bank->epoch_schedule      = default_epoch_schedule;
  epoch_bank->rent_epoch_schedule = default_epoch_schedule;
  epoch_bank->rent                = default_rent;
  epoch_bank->ticks_per_slot      = 64;
  epoch_bank->slots_per_year      = SECONDS_PER_YEAR * (1000000000.0 / (double)6250000) / (double)epoch_bank->ticks_per_slot;

  // Override default values if provided
  if( slot_ctx->sysvar_cache->has_epoch_schedule ) {
    epoch_bank->epoch_schedule      = *(fd_epoch_schedule_t *)fd_type_pun_const( slot_ctx->sysvar_cache->val_epoch_schedule );
    epoch_bank->rent_epoch_schedule = *(fd_epoch_schedule_t *)fd_type_pun_const( slot_ctx->sysvar_cache->val_epoch_schedule );
  }

  if( slot_ctx->sysvar_cache->has_rent ) {
    epoch_bank->rent = *(fd_rent_t *)fd_type_pun_const( slot_ctx->sysvar_cache->val_rent );
  }

  /* Provide default slot hashes of size 1 if not provided */
  if( !slot_ctx->sysvar_cache->has_slot_hashes ) {
    uchar * deque_mem = fd_spad_alloc( runner->spad, deq_fd_slot_hash_t_align(), deq_fd_slot_hash_t_footprint( 1 ) );
    fd_slot_hash_t * slot_hashes = deq_fd_slot_hash_t_join( deq_fd_slot_hash_t_new( deque_mem, 1 ) );
    fd_slot_hash_t * dummy_elem = deq_fd_slot_hash_t_push_tail_nocopy( slot_hashes );
    memset( dummy_elem, 0, sizeof(fd_slot_hash_t) );
    fd_slot_hashes_t default_slot_hashes = { .hashes = slot_hashes };
    fd_sysvar_slot_hashes_init( slot_ctx, &default_slot_hashes );
  }

  /* Provide default stake history if not provided */
  if( !slot_ctx->sysvar_cache->has_stake_history ) {
    // Provide a 0-set default entry
    fd_stake_history_entry_t entry = {0};
    fd_sysvar_stake_history_init( slot_ctx );
    fd_sysvar_stake_history_update( slot_ctx, &entry, runner->spad );
  }

  /* Provide default last restart slot sysvar if not provided */
  if( !slot_ctx->sysvar_cache->has_last_restart_slot ) {
    fd_sysvar_last_restart_slot_init( slot_ctx );
  }

  /* Provide a default clock if not present */
  if( !slot_ctx->sysvar_cache->has_clock ) {
    fd_sysvar_clock_init( slot_ctx );
    fd_sysvar_clock_update( slot_ctx, runner->spad );
  }

  /* Epoch schedule and rent get set from the epoch bank */
  fd_sysvar_epoch_schedule_init( slot_ctx );
  fd_sysvar_rent_init( slot_ctx );

  /* Set the epoch rewards sysvar if partition epoch rewards feature is enabled

     TODO: The init parameters are not exactly conformant with Agave's epoch rewards sysvar. We should
     be calling `fd_begin_partitioned_rewards` with the same parameters as Agave. However,
     we just need the `active` field to be conformant due to a single Stake program check.
     THIS MAY CHANGE IN THE FUTURE. If there are other parts of transaction execution that use
     the epoch rewards sysvar, we may need to update this.
  */
  if( (FD_FEATURE_ACTIVE( slot_ctx->slot_bank.slot, slot_ctx->epoch_ctx->features, enable_partitioned_epoch_reward ) ||
       FD_FEATURE_ACTIVE( slot_ctx->slot_bank.slot, slot_ctx->epoch_ctx->features, partitioned_epoch_rewards_superfeature ))
      && !slot_ctx->sysvar_cache->has_epoch_rewards ) {
    fd_point_value_t point_value = {0};
    fd_hash_t const * last_hash = test_ctx->blockhash_queue_count > 0 ? (fd_hash_t const *)test_ctx->blockhash_queue[0]->bytes : (fd_hash_t const *)empty_bytes;
    fd_sysvar_epoch_rewards_init( slot_ctx, 0UL, 0UL, 2UL, 1UL, point_value, last_hash);
  }

  /* Restore sysvar cache (again, since we may need to provide default sysvars) */
  fd_sysvar_cache_restore( slot_ctx->sysvar_cache, funk, funk_txn, runner->spad, fd_wksp_containing( slot_ctx ) );

  /* A NaN rent exemption threshold is U.B. in Solana Labs */
  fd_rent_t const * rent = fd_sysvar_cache_rent( slot_ctx->sysvar_cache );
  if( ( !fd_double_is_normal( rent->exemption_threshold ) ) |
      ( rent->exemption_threshold     <      0.0 ) |
      ( rent->exemption_threshold     >    999.0 ) |
      ( rent->lamports_per_uint8_year > UINT_MAX ) |
      ( rent->burn_percent            >      100 ) ) {
    return NULL;
  }

  /* Blockhash queue is given in txn message. We need to populate the following two fields:
     - slot_ctx->slot_bank.block_hash_queue
     - slot_ctx->slot_bank.recent_block_hashes */
  ulong num_blockhashes = test_ctx->blockhash_queue_count;

  /* Blockhash queue init */
  slot_ctx->slot_bank.block_hash_queue.max_age   = FD_BLOCKHASH_QUEUE_MAX_ENTRIES;
  slot_ctx->slot_bank.block_hash_queue.ages_root = NULL;
  uchar * pool_mem = fd_spad_alloc( runner->spad, fd_hash_hash_age_pair_t_map_align(), fd_hash_hash_age_pair_t_map_footprint( 400 ) );
  slot_ctx->slot_bank.block_hash_queue.ages_pool = fd_hash_hash_age_pair_t_map_join( fd_hash_hash_age_pair_t_map_new( pool_mem, 400 ) );
  slot_ctx->slot_bank.block_hash_queue.last_hash = fd_spad_alloc( runner->spad, FD_HASH_ALIGN, FD_HASH_FOOTPRINT );

  // Save lamports per signature for most recent blockhash, if sysvar cache contains recent block hashes
  fd_recent_block_hashes_global_t const * rbh_global = fd_sysvar_cache_recent_block_hashes( slot_ctx->sysvar_cache );
  fd_recent_block_hashes_t rbh[1];
  if( rbh_global ) {
    rbh->hashes = deq_fd_block_block_hash_entry_t_join( fd_wksp_laddr_fast( fd_wksp_containing( runner->spad ), rbh_global->hashes_gaddr ) );
  }

  if( rbh_global && !deq_fd_block_block_hash_entry_t_empty( rbh->hashes ) ) {
    fd_block_block_hash_entry_t const * last = deq_fd_block_block_hash_entry_t_peek_head_const( rbh->hashes );
    if( last && last->fee_calculator.lamports_per_signature!=0UL ) {
      slot_ctx->slot_bank.lamports_per_signature = last->fee_calculator.lamports_per_signature;
      slot_ctx->prev_lamports_per_signature      = last->fee_calculator.lamports_per_signature;
    }
  }

  // Blockhash_queue[end] = last (latest) hash
  // Blockhash_queue[0] = genesis hash
  if( num_blockhashes > 0 ) {
    memcpy( &epoch_bank->genesis_hash, test_ctx->blockhash_queue[0]->bytes, sizeof(fd_hash_t) );

    for( ulong i = 0; i < num_blockhashes; ++i ) {
      // Recent block hashes cap is 150 (actually 151), while blockhash queue capacity is 300 (actually 301)
      fd_block_block_hash_entry_t blockhash_entry;
      memcpy( &blockhash_entry.blockhash, test_ctx->blockhash_queue[i]->bytes, sizeof(fd_hash_t) );
      slot_ctx->slot_bank.poh = blockhash_entry.blockhash;
      fd_sysvar_recent_hashes_update( slot_ctx, runner->spad );
    }
  } else {
    // Add a default empty blockhash and use it as genesis
    num_blockhashes = 1;
    memcpy( &epoch_bank->genesis_hash, empty_bytes, sizeof(fd_hash_t) );
    fd_block_block_hash_entry_t blockhash_entry;
    memcpy( &blockhash_entry.blockhash, empty_bytes, sizeof(fd_hash_t) );
    slot_ctx->slot_bank.poh = blockhash_entry.blockhash;
    fd_sysvar_recent_hashes_update( slot_ctx, runner->spad );
  }
  fd_sysvar_cache_restore_recent_block_hashes( slot_ctx->sysvar_cache, funk, funk_txn, runner->spad, fd_wksp_containing( slot_ctx ) );

  /* Create the raw txn (https://solana.com/docs/core/transactions#transaction-size) */
  uchar * txn_raw_begin = fd_spad_alloc( runner->spad, alignof(uchar), 1232 );
  ushort instr_count, addr_table_cnt;
  ulong msg_sz = _serialize_txn( txn_raw_begin, &test_ctx->tx, &instr_count, &addr_table_cnt );
  if( FD_UNLIKELY( msg_sz==ULONG_MAX ) ) {
    return NULL;
  }

  /* Set up txn descriptor from raw data */
  fd_txn_t * txn_descriptor = (fd_txn_t *) fd_spad_alloc( runner->spad, fd_txn_align(), fd_txn_footprint( instr_count, addr_table_cnt ) );
  if( FD_UNLIKELY( !fd_txn_parse( txn_raw_begin, msg_sz, txn_descriptor, NULL ) ) ) {
    return NULL;
  }

  /* Run txn preparation phases and execution
     NOTE: This should be modified accordingly if transaction setup logic changes */
  fd_txn_p_t * txn = fd_spad_alloc( runner->spad, alignof(fd_txn_p_t), sizeof(fd_txn_p_t) );
  memcpy( txn->payload, txn_raw_begin, msg_sz );
  txn->payload_sz = msg_sz;
  txn->flags = FD_TXN_P_FLAGS_SANITIZE_SUCCESS;
  memcpy( txn->_, txn_descriptor, fd_txn_footprint( instr_count, addr_table_cnt ) );

  fd_execute_txn_task_info_t * task_info = fd_spad_alloc( runner->spad, alignof(fd_execute_txn_task_info_t), sizeof(fd_execute_txn_task_info_t) );
  memset( task_info, 0, sizeof(fd_execute_txn_task_info_t) );
  task_info->txn     = txn;
  task_info->txn_ctx = fd_spad_alloc( runner->spad, FD_EXEC_TXN_CTX_ALIGN, FD_EXEC_TXN_CTX_FOOTPRINT );

  fd_tpool_t tpool[1];
  tpool->worker_cnt = 1;
  tpool->worker_max = 1;

  fd_runtime_prepare_txns_start( slot_ctx, task_info, txn, 1UL, runner->spad );

  /* Setup the spad for account allocation */
  task_info->txn_ctx->spad      = runner->spad;
  task_info->txn_ctx->spad_wksp = fd_wksp_containing( runner->spad );

  fd_runtime_pre_execute_check( task_info, 0 );

  if( task_info->txn->flags & FD_TXN_P_FLAGS_SANITIZE_SUCCESS ) {
    task_info->txn->flags |= FD_TXN_P_FLAGS_EXECUTE_SUCCESS;
    task_info->exec_res    = fd_execute_txn( task_info );
  }

  slot_ctx->slot_bank.collected_execution_fees += task_info->txn_ctx->execution_fee;
  slot_ctx->slot_bank.collected_priority_fees  += task_info->txn_ctx->priority_fee;
  slot_ctx->slot_bank.collected_rent           += task_info->txn_ctx->collected_rent;
  return task_info;
}

static int
_block_context_create_and_exec( fd_exec_instr_test_runner_t *        runner,
                                fd_exec_slot_ctx_t *                 slot_ctx,
                                fd_exec_test_block_context_t const * test_ctx ) {
  fd_funk_t * funk = runner->funk;

  /* Generate unique ID for funk txn */
  fd_funk_txn_xid_t xid[1] = {0};
  xid[0] = fd_funk_generate_xid();

  /* Create temporary funk transaction and slot / epoch contexts */
  fd_funk_txn_start_write( funk );
  fd_funk_txn_t * funk_txn = fd_funk_txn_prepare( funk, NULL, xid, 1 );
  fd_funk_txn_end_write( funk );

  /* Allocate contexts */
  ulong                 vote_acct_max = fd_ulong_max( 128UL,
                                                      fd_ulong_max( test_ctx->epoch_ctx.stake_accounts_count,
                                                                       fd_ulong_max( test_ctx->epoch_ctx.vote_accounts_t_count,
                                                                                        fd_ulong_max( test_ctx->epoch_ctx.vote_accounts_t_1_count,
                                                                                                         test_ctx->epoch_ctx.vote_accounts_t_2_count ) ) ) );
  uchar *               epoch_ctx_mem = fd_spad_alloc( runner->spad, 128UL, fd_exec_epoch_ctx_footprint( vote_acct_max ) );
  fd_exec_epoch_ctx_t * epoch_ctx     = fd_exec_epoch_ctx_join( fd_exec_epoch_ctx_new( epoch_ctx_mem, vote_acct_max ) );

  /* Restore feature flags */
  if( !_restore_feature_flags( epoch_ctx, &test_ctx->epoch_ctx.features ) ) {
    return 1;
  }

  /* Set up slot context */
  slot_ctx->funk_txn              = funk_txn;
  slot_ctx->funk                  = funk;
  slot_ctx->enable_exec_recording = 0;
  slot_ctx->epoch_ctx             = epoch_ctx;
  slot_ctx->runtime_wksp          = fd_wksp_containing( slot_ctx );
  fd_memcpy( &slot_ctx->slot_bank.banks_hash, test_ctx->slot_ctx.parent_bank_hash, sizeof( fd_hash_t ) );

  /* Set up slot bank */
  ulong slot = test_ctx->slot_ctx.slot;
  fd_slot_bank_t * slot_bank = &slot_ctx->slot_bank;

  fd_memcpy( slot_bank->lthash.lthash, test_ctx->slot_ctx.parent_lt_hash, FD_LTHASH_LEN_BYTES );
  slot_bank->slot                  = slot;
  slot_bank->block_height          = test_ctx->slot_ctx.block_height;
  slot_bank->prev_slot             = test_ctx->slot_ctx.prev_slot;
  slot_bank->fee_rate_governor     = (fd_fee_rate_governor_t) {
    .target_lamports_per_signature = 10000UL,
    .target_signatures_per_slot    = 20000UL,
    .min_lamports_per_signature    = 5000UL,
    .max_lamports_per_signature    = 100000UL,
    .burn_percent                  = 50,
  };
  slot_bank->capitalization        = test_ctx->slot_ctx.prev_epoch_capitalization;

  /* Set up epoch context and epoch bank */
  /* TODO: Do we need any more of these? */
  fd_epoch_bank_t * epoch_bank    = fd_exec_epoch_ctx_epoch_bank( epoch_ctx );

  // self.max_tick_height = (self.slot + 1) * self.ticks_per_slot;
  epoch_bank->hashes_per_tick       = test_ctx->epoch_ctx.hashes_per_tick;
  epoch_bank->ticks_per_slot        = test_ctx->epoch_ctx.ticks_per_slot;
  epoch_bank->slots_per_year        = test_ctx->epoch_ctx.slots_per_year;
  epoch_bank->inflation             = (fd_inflation_t) {
    .initial         = test_ctx->epoch_ctx.inflation.initial,
    .terminal        = test_ctx->epoch_ctx.inflation.terminal,
    .taper           = test_ctx->epoch_ctx.inflation.taper,
    .foundation      = test_ctx->epoch_ctx.inflation.foundation,
    .foundation_term = test_ctx->epoch_ctx.inflation.foundation_term
  };
  epoch_bank->genesis_creation_time = test_ctx->epoch_ctx.genesis_creation_time;

  /* Load in all accounts provided in the context */
  for( ushort i=0; i<test_ctx->acct_states_count; i++ ) {
    FD_TXN_ACCOUNT_DECL(acc);
    _load_txn_account( acc, funk, funk_txn, &test_ctx->acct_states[i], 0 );
  }

  /* Restore sysvar cache */
  fd_runtime_sysvar_cache_load( slot_ctx, runner->spad );

  /* Finish init epoch bank sysvars */
  fd_memcpy( &epoch_bank->epoch_schedule, slot_ctx->sysvar_cache->val_epoch_schedule, sizeof(fd_epoch_schedule_t) );
  fd_memcpy( &epoch_bank->rent_epoch_schedule, slot_ctx->sysvar_cache->val_epoch_schedule, sizeof(fd_epoch_schedule_t) );
  fd_memcpy( &epoch_bank->rent, slot_ctx->sysvar_cache->val_rent, sizeof(fd_rent_t) );
  epoch_bank->stakes.epoch = fd_slot_to_epoch( &epoch_bank->epoch_schedule, slot_bank->prev_slot, NULL );

  /* Update stake cache for epoch T */
  for( uint i=0U; i<test_ctx->epoch_ctx.stake_accounts_count; i++ ) {
    fd_pubkey_t voter_pubkey;
    fd_memcpy( &voter_pubkey, test_ctx->epoch_ctx.stake_accounts[i].voter_pubkey, sizeof(fd_pubkey_t) );

    fd_pubkey_t stake_account;
    fd_memcpy( &stake_account, test_ctx->epoch_ctx.stake_accounts[i].stake_account_pubkey, sizeof(fd_pubkey_t) );

    ulong  stake                = test_ctx->epoch_ctx.stake_accounts[i].stake;
    ulong  activation_epoch     = test_ctx->epoch_ctx.stake_accounts[i].activation_epoch;
    ulong  deactivation_epoch   = test_ctx->epoch_ctx.stake_accounts[i].deactivation_epoch;
    double warmup_cooldown_rate = test_ctx->epoch_ctx.stake_accounts[i].warmup_cooldown_rate;

    fd_delegation_pair_t_mapnode_t * stake_node = fd_delegation_pair_t_map_acquire( epoch_bank->stakes.stake_delegations_pool );
    fd_memcpy( &stake_node->elem.account, &stake_account, sizeof(fd_pubkey_t) );
    fd_memcpy( &stake_node->elem.delegation.voter_pubkey, &voter_pubkey, sizeof(fd_pubkey_t) );
    stake_node->elem.delegation.stake                = stake;
    stake_node->elem.delegation.activation_epoch     = activation_epoch;
    stake_node->elem.delegation.deactivation_epoch   = deactivation_epoch;
    stake_node->elem.delegation.warmup_cooldown_rate = warmup_cooldown_rate;

    fd_delegation_pair_t_map_insert( epoch_bank->stakes.stake_delegations_pool,
                                     &epoch_bank->stakes.stake_delegations_root,
                                     stake_node );
  }

  /* Update vote cache for epoch T */
  for( uint i=0U; i<test_ctx->epoch_ctx.vote_accounts_t_count; i++ ) {
    fd_exec_test_acct_state_t * vote_account = &test_ctx->epoch_ctx.vote_accounts_t[i].vote_account;
    ulong                       stake        = test_ctx->epoch_ctx.vote_accounts_t[i].stake;

    fd_vote_accounts_pair_t_mapnode_t * vote_node = fd_vote_accounts_pair_t_map_acquire( epoch_bank->stakes.vote_accounts.vote_accounts_pool );
    vote_node->elem.stake = stake;
    fd_memcpy( &vote_node->elem.key, vote_account->address, sizeof(fd_pubkey_t) );
    vote_node->elem.value.executable = vote_account->executable;
    vote_node->elem.value.lamports   = vote_account->lamports;
    vote_node->elem.value.rent_epoch = vote_account->rent_epoch;
    vote_node->elem.value.data_len   = vote_account->data->size;
    vote_node->elem.value.data       = fd_spad_alloc( runner->spad, alignof(uchar), vote_account->data->size );
    fd_memcpy( vote_node->elem.value.data, vote_account->data->bytes, vote_account->data->size );
    fd_memcpy( &vote_node->elem.value.owner, vote_account->owner, sizeof(fd_pubkey_t) );

    fd_vote_accounts_pair_t_map_insert( epoch_bank->stakes.vote_accounts.vote_accounts_pool,
                                        &epoch_bank->stakes.vote_accounts.vote_accounts_root,
                                        vote_node );
  }

  /* Update vote cache for epoch T-1 */
  for( uint i=0U; i<test_ctx->epoch_ctx.vote_accounts_t_1_count; i++ ) {
    fd_exec_test_acct_state_t * vote_account = &test_ctx->epoch_ctx.vote_accounts_t_1[i].vote_account;
    ulong                       stake        = test_ctx->epoch_ctx.vote_accounts_t_1[i].stake;

    fd_vote_accounts_pair_t_mapnode_t * vote_node = fd_vote_accounts_pair_t_map_acquire( epoch_bank->next_epoch_stakes.vote_accounts_pool );
    vote_node->elem.stake = stake;
    fd_memcpy( &vote_node->elem.key, vote_account->address, sizeof(fd_pubkey_t) );
    vote_node->elem.value.executable = vote_account->executable;
    vote_node->elem.value.lamports   = vote_account->lamports;
    vote_node->elem.value.rent_epoch = vote_account->rent_epoch;
    vote_node->elem.value.data_len   = vote_account->data->size;
    vote_node->elem.value.data       = fd_spad_alloc( runner->spad, alignof(uchar), vote_account->data->size );
    fd_memcpy( vote_node->elem.value.data, vote_account->data->bytes, vote_account->data->size );
    fd_memcpy( &vote_node->elem.value.owner, vote_account->owner, sizeof(fd_pubkey_t) );

    fd_vote_accounts_pair_t_map_insert( epoch_bank->next_epoch_stakes.vote_accounts_pool,
                                        &epoch_bank->next_epoch_stakes.vote_accounts_root,
                                        vote_node );
  }

  /* Update vote cache for epoch T-2 */
  uchar * pool_mem                           = fd_spad_alloc( runner->spad,
                                                              fd_vote_accounts_pair_t_map_align(),
                                                              fd_vote_accounts_pair_t_map_footprint( vote_acct_max ) );
  slot_bank->epoch_stakes.vote_accounts_pool = fd_vote_accounts_pair_t_map_join( fd_vote_accounts_pair_t_map_new( pool_mem, vote_acct_max ) );
  slot_bank->epoch_stakes.vote_accounts_root = NULL;
  for( uint i=0U; i<test_ctx->epoch_ctx.vote_accounts_t_2_count; i++ ) {
    fd_exec_test_acct_state_t * vote_account = &test_ctx->epoch_ctx.vote_accounts_t_2[i].vote_account;
    ulong                       stake        = test_ctx->epoch_ctx.vote_accounts_t_2[i].stake;

    fd_vote_accounts_pair_t_mapnode_t * vote_node = fd_vote_accounts_pair_t_map_acquire( slot_bank->epoch_stakes.vote_accounts_pool );
    vote_node->elem.stake = stake;
    fd_memcpy( &vote_node->elem.key, vote_account->address, sizeof(fd_pubkey_t) );
    vote_node->elem.value.executable = vote_account->executable;
    vote_node->elem.value.lamports   = vote_account->lamports;
    vote_node->elem.value.rent_epoch = vote_account->rent_epoch;
    vote_node->elem.value.data_len   = vote_account->data->size;
    vote_node->elem.value.data       = fd_spad_alloc( runner->spad, alignof(uchar), vote_account->data->size );
    fd_memcpy( vote_node->elem.value.data, vote_account->data->bytes, vote_account->data->size );
    fd_memcpy( &vote_node->elem.value.owner, vote_account->owner, sizeof(fd_pubkey_t) );

    fd_vote_accounts_pair_t_map_insert( slot_bank->epoch_stakes.vote_accounts_pool,
                                        &slot_bank->epoch_stakes.vote_accounts_root,
                                        vote_node );
  }

  /* Initialize the current running epoch stake and vote accounts */
  pool_mem                                        = fd_spad_alloc( runner->spad,
                                                                   fd_account_keys_pair_t_map_align(),
                                                                   fd_account_keys_pair_t_map_footprint( vote_acct_max ) );
  slot_bank->stake_account_keys.account_keys_pool = fd_account_keys_pair_t_map_join( fd_account_keys_pair_t_map_new( pool_mem, vote_acct_max ) );
  slot_bank->stake_account_keys.account_keys_root = NULL;
  for( uint i=0U; i<test_ctx->epoch_ctx.new_stake_accounts_count; i++ ) {
    FD_TXN_ACCOUNT_DECL( acc );

    fd_pubkey_t stake_pubkey;
    fd_memcpy( &stake_pubkey, test_ctx->epoch_ctx.new_stake_accounts[i]->bytes, sizeof(fd_pubkey_t) );

    // Fetch and store the stake account using acc mgr
    if( fd_txn_account_init_from_funk_readonly( acc, &stake_pubkey, slot_ctx->funk, slot_ctx->funk_txn ) ) {
      continue;
    }

    fd_store_stake_delegation( slot_ctx, acc );
  }

  pool_mem                                       = fd_spad_alloc( runner->spad,
                                                                  fd_account_keys_pair_t_map_align(),
                                                                  fd_account_keys_pair_t_map_footprint( vote_acct_max ) );
  slot_bank->vote_account_keys.account_keys_pool = fd_account_keys_pair_t_map_join( fd_account_keys_pair_t_map_new( pool_mem, vote_acct_max ) );
  slot_bank->vote_account_keys.account_keys_root = NULL;
  for( uint i=0U; i<test_ctx->epoch_ctx.new_vote_accounts_count; i++ ) {
    FD_TXN_ACCOUNT_DECL( acc );

    fd_pubkey_t vote_pubkey;
    memcpy( &vote_pubkey, test_ctx->epoch_ctx.new_vote_accounts[i]->bytes, sizeof(fd_pubkey_t) );

    // Fetch and store the vote account from the acc mgr
    if( fd_txn_account_init_from_funk_readonly( acc, &vote_pubkey, slot_ctx->funk, slot_ctx->funk_txn ) ) {
      continue;
    }

    fd_vote_store_account( slot_ctx, acc );
  }

  /* Update leader schedule */
  fd_runtime_update_leaders( slot_ctx, slot_ctx->slot_bank.slot, runner->spad );

  /* Initialize the blockhash queue and recent blockhashes sysvar from the input blockhash queue */
  slot_bank->block_hash_queue.max_age   = FD_BLOCKHASH_QUEUE_MAX_ENTRIES; // Max age is fixed at 300
  slot_bank->block_hash_queue.ages_root = NULL;
  pool_mem = fd_spad_alloc( runner->spad, fd_hash_hash_age_pair_t_map_align(), fd_hash_hash_age_pair_t_map_footprint( FD_BLOCKHASH_QUEUE_MAX_ENTRIES+1UL ) );
  slot_bank->block_hash_queue.ages_pool = fd_hash_hash_age_pair_t_map_join( fd_hash_hash_age_pair_t_map_new( pool_mem, FD_BLOCKHASH_QUEUE_MAX_ENTRIES+1UL ) );
  slot_bank->block_hash_queue.last_hash = fd_valloc_malloc( fd_spad_virtual( runner->spad ), FD_HASH_ALIGN, FD_HASH_FOOTPRINT );

  /* TODO: Restore this from input */
  pool_mem                              = fd_spad_alloc( runner->spad, fd_clock_timestamp_vote_t_map_align(), fd_clock_timestamp_vote_t_map_footprint( 10000UL ) );
  slot_bank->timestamp_votes.votes_pool = fd_clock_timestamp_vote_t_map_join( fd_clock_timestamp_vote_t_map_new( pool_mem, 10000UL ) );
  slot_bank->timestamp_votes.votes_root = NULL;

  /* TODO: We might need to load this in from the input. We also need to size this out for worst case, but this also blows up the memory requirement. */
  /* Allocate all the memory for the rent fresh accounts list */
  fd_rent_fresh_accounts_new( &slot_bank->rent_fresh_accounts );
  slot_bank->rent_fresh_accounts.total_count        = 0UL;
  slot_bank->rent_fresh_accounts.fresh_accounts_len = FD_RENT_FRESH_ACCOUNTS_MAX;
  slot_bank->rent_fresh_accounts.fresh_accounts     = fd_spad_alloc(
    runner->spad,
    FD_RENT_FRESH_ACCOUNT_ALIGN,
    FD_RENT_FRESH_ACCOUNT_FOOTPRINT * FD_RENT_FRESH_ACCOUNTS_MAX );
  fd_memset(  slot_bank->rent_fresh_accounts.fresh_accounts, 0, FD_RENT_FRESH_ACCOUNT_FOOTPRINT * FD_RENT_FRESH_ACCOUNTS_MAX );

  // Set genesis hash to {0}
  fd_memset( &epoch_bank->genesis_hash, 0, sizeof(fd_hash_t) );
  fd_memset( slot_bank->block_hash_queue.last_hash, 0, sizeof(fd_hash_t) );

  // Use the latest lamports per signature
  fd_recent_block_hashes_global_t const * rbh_global = fd_sysvar_cache_recent_block_hashes( slot_ctx->sysvar_cache );
  fd_recent_block_hashes_t rbh[1];
  if( rbh_global ) {
    rbh->hashes = deq_fd_block_block_hash_entry_t_join( fd_wksp_laddr_fast( fd_wksp_containing( runner->spad ), rbh_global->hashes_gaddr ) );
  }

  if( rbh_global && !deq_fd_block_block_hash_entry_t_empty( rbh->hashes ) ) {
    fd_block_block_hash_entry_t const * last = deq_fd_block_block_hash_entry_t_peek_head_const( rbh->hashes );
    if( last && last->fee_calculator.lamports_per_signature!=0UL ) {
      slot_bank->lamports_per_signature     = last->fee_calculator.lamports_per_signature;
      slot_ctx->prev_lamports_per_signature = last->fee_calculator.lamports_per_signature;
    }
  }

  // Populate blockhash queue and recent blockhashes sysvar
  for( ushort i=0; i<test_ctx->blockhash_queue_count; ++i ) {
    fd_block_block_hash_entry_t blockhash_entry;
    memcpy( &blockhash_entry.blockhash, test_ctx->blockhash_queue[i]->bytes, sizeof(fd_hash_t) );
    slot_bank->poh = blockhash_entry.blockhash;
    fd_sysvar_recent_hashes_update( slot_ctx, runner->spad );
  }

  // Set the current poh from the input (we skip POH verification in this fuzzing target)
  fd_memcpy( slot_ctx->slot_bank.poh.uc, test_ctx->slot_ctx.poh, FD_HASH_FOOTPRINT );

  /* Make a new funk transaction since we're done loading in accounts for context */
  fd_funk_txn_xid_t fork_xid[1] = {0};
  fork_xid[0] = fd_funk_generate_xid();
  fd_funk_txn_start_write( funk );
  slot_ctx->funk_txn = fd_funk_txn_prepare( funk, slot_ctx->funk_txn, fork_xid, 1 );
  fd_funk_txn_end_write( funk );

  /* Calculate epoch account hash values. This sets epoch_bank.eah_{start_slot, stop_slot, interval} */
  fd_calculate_epoch_accounts_hash_values( slot_ctx );

  /* Prepare raw transaction pointers and block / microblock infos */
  ulong microblock_cnt = test_ctx->microblocks_count;

  // For fuzzing, we're using a single microblock batch that contains all microblocks
  fd_runtime_block_info_t *    block_info       = fd_spad_alloc( runner->spad, alignof(fd_runtime_block_info_t), sizeof(fd_runtime_block_info_t) );
  fd_microblock_batch_info_t * batch_info       = fd_spad_alloc( runner->spad, alignof(fd_microblock_batch_info_t), sizeof(fd_microblock_batch_info_t) );
  fd_microblock_info_t *       microblock_infos = fd_spad_alloc( runner->spad, alignof(fd_microblock_info_t), microblock_cnt * sizeof(fd_microblock_info_t) );
  fd_memset( block_info, 0, sizeof(fd_runtime_block_info_t) );
  fd_memset( batch_info, 0, sizeof(fd_microblock_batch_info_t) );
  fd_memset( microblock_infos, 0, microblock_cnt * sizeof(fd_microblock_info_t) );

  block_info->microblock_batch_cnt   = 1UL;
  block_info->microblock_cnt         = microblock_cnt;
  block_info->microblock_batch_infos = batch_info;

  batch_info->microblock_cnt         = microblock_cnt;
  batch_info->microblock_infos       = microblock_infos;

  ulong batch_signature_cnt          = 0UL;
  ulong batch_txn_cnt                = 0UL;
  ulong batch_account_cnt            = 0UL;

  for( ulong i=0UL; i<microblock_cnt; i++ ) {
    fd_exec_test_microblock_t const * input_microblock = &test_ctx->microblocks[i];
    fd_microblock_info_t *            microblock_info  = &microblock_infos[i];
    fd_microblock_hdr_t *             microblock_hdr   = fd_spad_alloc( runner->spad, alignof(fd_microblock_hdr_t), sizeof(fd_microblock_hdr_t) );
    fd_memset( microblock_hdr, 0, sizeof(fd_microblock_hdr_t) );

    ulong txn_cnt       = input_microblock->txns_count;
    ulong signature_cnt = 0UL;
    ulong account_cnt   = 0UL;

    fd_txn_p_t * txn_ptrs = fd_spad_alloc( runner->spad, alignof(fd_txn_p_t), txn_cnt * sizeof(fd_txn_p_t) );

    for( ulong j=0UL; j<txn_cnt; j++ ) {
      fd_txn_p_t * txn = &txn_ptrs[j];

      ushort _instr_count, _addr_table_cnt;
      ulong msg_sz = _serialize_txn( txn->payload, &input_microblock->txns[j], &_instr_count, &_addr_table_cnt );

      // Reject any transactions over 1232 bytes
      if( FD_UNLIKELY( msg_sz==ULONG_MAX ) ) {
        return 1;
      }
      txn->payload_sz = msg_sz;

      // Reject any transactions that cannot be parsed
      if( FD_UNLIKELY( !fd_txn_parse( txn->payload, msg_sz, TXN( txn ), NULL ) ) ) {
        return 1;
      }

      signature_cnt += TXN( txn )->signature_cnt;
      account_cnt   += fd_txn_account_cnt( TXN( txn ), FD_TXN_ACCT_CAT_ALL );
    }

    microblock_hdr->txn_cnt         = txn_cnt;
    microblock_info->microblock.raw = (uchar *)microblock_hdr;

    microblock_info->signature_cnt  = signature_cnt;
    microblock_info->account_cnt    = account_cnt;
    microblock_info->txns           = txn_ptrs;

    batch_signature_cnt            += signature_cnt;
    batch_txn_cnt                  += txn_cnt;
    batch_account_cnt              += account_cnt;
  }

  block_info->signature_cnt = batch_info->signature_cnt = batch_signature_cnt;
  block_info->txn_cnt       = batch_info->txn_cnt       = batch_txn_cnt;
  block_info->account_cnt   = batch_info->account_cnt   = batch_account_cnt;

  /* Initialize tpool and spad(s)
    TODO: We should decide how many workers to use for the execution tpool. We might have a bunch of
    transactions within a single block, but increasing the worker cnt increases the memory requirements by
    1.28 GB per additional worker (for spad memory allocation). We also fuzz block execution using
    multiple cores, so it may be possible to get away with only 1 worker. Additionally, Agave will more than
    likely always be the execution speed bottleneck, so we can play around with numbers and see what yields
    the best results. */
  ulong worker_max = 2UL;
  void * tpool_mem = fd_spad_alloc( runner->spad, FD_TPOOL_ALIGN, FD_TPOOL_FOOTPRINT( worker_max ) );
  fd_tpool_t * tpool = fd_tpool_init( tpool_mem, worker_max );
  fd_tpool_worker_push( tpool, 1UL, NULL, 0UL );

  fd_spad_t * runtime_spad = runner->spad;

  /* Format chunks of memory for the exec spads
     TODO: This memory needs a better bound. */
  fd_spad_t * exec_spads[2]  = { 0 };
  ulong       exec_spads_cnt = 2UL;
  ulong exec_spad_mem_max = 1UL << 30;
  for( ulong i=0UL; i<worker_max; i++ ) {
    void *      exec_spad_mem = fd_spad_alloc( runtime_spad, FD_SPAD_ALIGN, FD_SPAD_FOOTPRINT( exec_spad_mem_max ) );
    fd_spad_t * exec_spad     = fd_spad_join( fd_spad_new( exec_spad_mem, exec_spad_mem_max ) );
    exec_spads[i] = exec_spad;
  }

  // Prepare. Execute. Finalize.
  int res = 0UL;
  FD_SPAD_FRAME_BEGIN( runtime_spad ) {
    fd_rewards_recalculate_partitioned_rewards( slot_ctx, tpool, exec_spads, exec_spads_cnt, runtime_spad );

    /* Process new epoch may push a new spad frame onto the runtime spad. We should make sure this frame gets
       cleared (if it was allocated) before executing the block. */
    ulong spad_frame_ct     = fd_spad_frame_used( runtime_spad );
    int   is_epoch_boundary = 0;
    fd_runtime_block_pre_execute_process_new_epoch( slot_ctx, tpool, exec_spads, exec_spads_cnt, runtime_spad, &is_epoch_boundary );
    while( fd_spad_frame_used( runtime_spad )>spad_frame_ct ) {
      fd_spad_pop( runtime_spad );
    }

    res = fd_runtime_block_execute_tpool( slot_ctx, NULL, block_info, tpool, exec_spads, exec_spads_cnt, runtime_spad );
  } FD_SPAD_FRAME_END;

  fd_tpool_worker_pop( tpool );

  return res;
}

void
fd_exec_test_instr_context_destroy( fd_exec_instr_test_runner_t * runner,
                                    fd_exec_instr_ctx_t *         ctx ) {
  if( !ctx ) return;
  fd_funk_txn_t * funk_txn = ctx->txn_ctx->funk_txn;

  fd_funk_txn_cancel( runner->funk, funk_txn, 1 );
}

static void
_txn_context_destroy( fd_exec_instr_test_runner_t * runner,
                      fd_exec_slot_ctx_t *          slot_ctx ) {
  if( !slot_ctx ) return; // This shouldn't be false either
  fd_funk_txn_t *       funk_txn  = slot_ctx->funk_txn;

  fd_funk_txn_cancel( runner->funk, funk_txn, 1 );
}

static void
_block_context_destroy( fd_exec_instr_test_runner_t * runner,
                        fd_exec_slot_ctx_t *            slot_ctx,
                        fd_wksp_t *                     wksp,
                        fd_alloc_t *                    alloc ) {
  if( !slot_ctx ) return; // This shouldn't be false either

  fd_wksp_free_laddr( fd_alloc_delete( fd_alloc_leave( alloc ) ) );
  fd_wksp_detach( wksp );

  fd_funk_txn_cancel_all( runner->funk, 1 );
}

static fd_sbpf_syscalls_t *
lookup_syscall_func( fd_sbpf_syscalls_t *syscalls,
                     const char *syscall_name,
                     size_t len) {
  ulong i;

  if (!syscall_name) return NULL;

  for (i = 0; i < fd_sbpf_syscalls_slot_cnt(); ++i) {
    if (!fd_sbpf_syscalls_key_inval(syscalls[i].key) && syscalls[i].name && strlen(syscalls[i].name) == len) {
      if (!memcmp(syscalls[i].name, syscall_name, len)) {
        return syscalls + i;
      }
    }
  }

  return NULL;
}

ulong
fd_exec_instr_test_run( fd_exec_instr_test_runner_t * runner,
                        void const *                  input_,
                        void **                       output_,
                        void *                        output_buf,
                        ulong                         output_bufsz ) {
  fd_exec_test_instr_context_t const * input  = fd_type_pun_const( input_ );
  fd_exec_test_instr_effects_t **      output = fd_type_pun( output_ );

  FD_SPAD_FRAME_BEGIN( runner->spad ) {

  /* Convert the Protobuf inputs to a fd_exec context */
  fd_exec_instr_ctx_t ctx[1];
  if( !fd_exec_test_instr_context_create( runner, ctx, input, false ) ) {
    fd_exec_test_instr_context_destroy( runner, ctx );
    return 0UL;
  }

  fd_instr_info_t * instr = (fd_instr_info_t *) ctx->instr;

  /* Execute the test */
  int exec_result = fd_execute_instr(ctx->txn_ctx, instr);

  /* Allocate space to capture outputs */

  ulong output_end = (ulong)output_buf + output_bufsz;
  FD_SCRATCH_ALLOC_INIT( l, output_buf );

  fd_exec_test_instr_effects_t * effects =
    FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_instr_effects_t),
                                sizeof (fd_exec_test_instr_effects_t) );
  if( FD_UNLIKELY( _l > output_end ) ) {
    fd_exec_test_instr_context_destroy( runner, ctx );
    return 0UL;
  }
  fd_memset( effects, 0, sizeof(fd_exec_test_instr_effects_t) );

  /* Capture error code */

  effects->result   = -exec_result;
  effects->cu_avail = ctx->txn_ctx->compute_meter;

  if( exec_result == FD_EXECUTOR_INSTR_ERR_CUSTOM_ERR ) {
    effects->custom_err     = ctx->txn_ctx->custom_err;
  }

  /* Allocate space for captured accounts */
  ulong modified_acct_cnt = ctx->txn_ctx->accounts_cnt;

  fd_exec_test_acct_state_t * modified_accts =
    FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_acct_state_t),
                                sizeof (fd_exec_test_acct_state_t) * modified_acct_cnt );
  if( FD_UNLIKELY( _l > output_end ) ) {
    fd_exec_test_instr_context_destroy( runner, ctx );
    return 0;
  }
  effects->modified_accounts       = modified_accts;
  effects->modified_accounts_count = 0UL;

  /* Capture borrowed accounts */

  for( ulong j=0UL; j < ctx->txn_ctx->accounts_cnt; j++ ) {
    fd_txn_account_t * acc = &ctx->txn_ctx->accounts[j];
    if( !acc->vt->get_meta( acc ) ) {
      continue;
    }

    ulong modified_idx = effects->modified_accounts_count;
    assert( modified_idx < modified_acct_cnt );

    fd_exec_test_acct_state_t * out_acct = &effects->modified_accounts[ modified_idx ];
    memset( out_acct, 0, sizeof(fd_exec_test_acct_state_t) );
    /* Copy over account content */

    memcpy( out_acct->address, acc->pubkey, sizeof(fd_pubkey_t) );
    out_acct->lamports = acc->vt->get_lamports( acc );
    if( acc->vt->get_data_len( acc )>0UL ) {
      out_acct->data =
        FD_SCRATCH_ALLOC_APPEND( l, alignof(pb_bytes_array_t),
                                    PB_BYTES_ARRAY_T_ALLOCSIZE( acc->vt->get_data_len( acc ) ) );
      if( FD_UNLIKELY( _l > output_end ) ) {
        fd_exec_test_instr_context_destroy( runner, ctx );
        return 0UL;
      }
      out_acct->data->size = (pb_size_t)acc->vt->get_data_len( acc );
      fd_memcpy( out_acct->data->bytes, acc->vt->get_data( acc ), acc->vt->get_data_len( acc ) );
    }

    out_acct->executable     = acc->vt->is_executable( acc );
    out_acct->rent_epoch     = acc->vt->get_rent_epoch( acc );
    memcpy( out_acct->owner, acc->vt->get_owner( acc ), sizeof(fd_pubkey_t) );

    effects->modified_accounts_count++;
  }

  /* Capture return data */
  fd_txn_return_data_t * return_data = &ctx->txn_ctx->return_data;
  if( return_data->len>0UL ) {
    effects->return_data = FD_SCRATCH_ALLOC_APPEND(l, alignof(pb_bytes_array_t),
                                PB_BYTES_ARRAY_T_ALLOCSIZE( return_data->len ) );
    if( FD_UNLIKELY( _l > output_end ) ) {
      fd_exec_test_instr_context_destroy( runner, ctx );
      return 0UL;
    }
    effects->return_data->size = (pb_size_t)return_data->len;
    fd_memcpy( effects->return_data->bytes, return_data->data, return_data->len );
  }

  ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
  fd_exec_test_instr_context_destroy( runner, ctx );

  *output = effects;
  return actual_end - (ulong)output_buf;

  } FD_SPAD_FRAME_END;
}

ulong
fd_exec_block_test_run( fd_exec_instr_test_runner_t * runner,
                        void const *                  input_,
                        void **                       output_,
                        void *                        output_buf,
                        ulong                         output_bufsz ) {
  fd_exec_test_block_context_t const * input  = fd_type_pun_const( input_ );
  fd_exec_test_block_effects_t **      output = fd_type_pun( output_ );

  FD_SPAD_FRAME_BEGIN( runner->spad ) {
    /* Initialize memory */
    fd_wksp_t *           wksp          = fd_wksp_attach( "wksp" );
    fd_alloc_t *          alloc         = fd_alloc_join( fd_alloc_new( fd_wksp_alloc_laddr( wksp, fd_alloc_align(), fd_alloc_footprint(), 2 ), 2 ), 0 );
    uchar *               slot_ctx_mem  = fd_spad_alloc( runner->spad, FD_EXEC_SLOT_CTX_ALIGN,  FD_EXEC_SLOT_CTX_FOOTPRINT );
    fd_exec_slot_ctx_t *  slot_ctx      = fd_exec_slot_ctx_join ( fd_exec_slot_ctx_new ( slot_ctx_mem, runner->spad ) );

    int res = _block_context_create_and_exec( runner, slot_ctx, input );
    if( res>0 ) {
      _block_context_destroy( runner, slot_ctx, wksp, alloc );
      return 0;
    }

    /* Start saving block exec results */
    FD_SCRATCH_ALLOC_INIT( l, output_buf );
    ulong output_end = (ulong)output_buf + output_bufsz;

    fd_exec_test_block_effects_t * effects =
    FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_block_effects_t),
                                  sizeof (fd_exec_test_block_effects_t) );
    if( FD_UNLIKELY( _l > output_end ) ) {
      abort();
    }
    fd_memset( effects, 0, sizeof(fd_exec_test_block_effects_t) );

    /* Capture capitalization */
    effects->slot_capitalization = slot_ctx->slot_bank.capitalization;

    /* Capture hashes */
    uchar out_lt_hash[32];
    fd_lthash_hash( (fd_lthash_value_t const *)slot_ctx->slot_bank.lthash.lthash, out_lt_hash );
    fd_memcpy( effects->bank_hash, slot_ctx->slot_bank.banks_hash.hash, sizeof(fd_hash_t) );
    fd_memcpy( effects->lt_hash, out_lt_hash, sizeof(fd_hash_t) );
    fd_memcpy( effects->account_delta_hash, slot_ctx->account_delta_hash.hash, sizeof(fd_hash_t) );

    /* Capture accounts. Since the only input accounts list comes from the input transactions, we have to iterate through
       all input transactions, gather the account keys in order, and skip any duplicate accounts. */
    // TODO: implement me

    ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
    _block_context_destroy( runner, slot_ctx, wksp, alloc );

    *output = effects;
    return actual_end - (ulong)output_buf;
  } FD_SPAD_FRAME_END;
}


ulong
fd_exec_txn_test_run( fd_exec_instr_test_runner_t * runner, // Runner only contains funk instance, so we can borrow instr test runner
                      void const *                  input_,
                      void **                       output_,
                      void *                        output_buf,
                      ulong                         output_bufsz ) {
  fd_exec_test_txn_context_t const * input  = fd_type_pun_const( input_ );
  fd_exec_test_txn_result_t **       output = fd_type_pun( output_ );

  FD_SPAD_FRAME_BEGIN( runner->spad ) {

    /* Initialize memory */
    uchar *               slot_ctx_mem = fd_spad_alloc( runner->spad, FD_EXEC_SLOT_CTX_ALIGN,  FD_EXEC_SLOT_CTX_FOOTPRINT  );
    fd_exec_slot_ctx_t *  slot_ctx     = fd_exec_slot_ctx_join( fd_exec_slot_ctx_new( slot_ctx_mem, runner->spad ) );

    /* Create and exec transaction */
    fd_execute_txn_task_info_t * task_info = _txn_context_create_and_exec( runner, slot_ctx, input );
    if( task_info == NULL ) {
      _txn_context_destroy( runner, slot_ctx );
      return 0UL;
    }
    fd_exec_txn_ctx_t * txn_ctx = task_info->txn_ctx;

    int exec_res = task_info->exec_res;

    /* Start saving txn exec results */
    FD_SCRATCH_ALLOC_INIT( l, output_buf );
    ulong output_end = (ulong)output_buf + output_bufsz;

    fd_exec_test_txn_result_t * txn_result =
    FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_txn_result_t),
                                  sizeof (fd_exec_test_txn_result_t) );
    if( FD_UNLIKELY( _l > output_end ) ) {
      abort();
    }
    fd_memset( txn_result, 0, sizeof(fd_exec_test_txn_result_t) );

    /* Capture basic results fields */
    txn_result->executed                          = task_info->txn->flags & FD_TXN_P_FLAGS_EXECUTE_SUCCESS;
    txn_result->sanitization_error                = !( task_info->txn->flags & FD_TXN_P_FLAGS_SANITIZE_SUCCESS );
    txn_result->has_resulting_state               = false;
    txn_result->resulting_state.acct_states_count = 0;
    txn_result->is_ok                             = !exec_res;
    txn_result->status                            = (uint32_t) -exec_res;
    txn_result->instruction_error                 = 0;
    txn_result->instruction_error_index           = 0;
    txn_result->custom_error                      = 0;
    txn_result->executed_units                    = txn_ctx->compute_unit_limit - txn_ctx->compute_meter;
    txn_result->has_fee_details                   = false;

    if( txn_result->sanitization_error ) {
      if( exec_res==FD_RUNTIME_TXN_ERR_INSTRUCTION_ERROR ) {
      /* If exec_res was an instruction error and we have a sanitization error, it was a precompile error */
        txn_result->instruction_error       = (uint32_t) -txn_ctx->exec_err;
        txn_result->instruction_error_index = (uint32_t) txn_ctx->instr_err_idx;

        /*
        TODO: precompile error codes are not conformant, so we're ignoring custom error codes for them for now. This should be revisited in the future.
        For now, only precompiles throw custom error codes, so we can ignore all custom error codes thrown in the sanitization phase. If this changes,
        this logic will have to be revisited.

        if( task_info->txn_ctx->exec_err == FD_EXECUTOR_INSTR_ERR_CUSTOM_ERR ) {
          txn_result->custom_error = txn_ctx->custom_err;
        }
        */
      }

      ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
      _txn_context_destroy( runner, slot_ctx );

      *output = txn_result;
      return actual_end - (ulong)output_buf;

    } else {
      /* Capture the instruction error code */
      if( exec_res==FD_RUNTIME_TXN_ERR_INSTRUCTION_ERROR ) {
        int instr_err_idx                   = txn_ctx->instr_err_idx;
        int program_id_idx                  = txn_ctx->instr_infos[instr_err_idx].program_id;

        txn_result->instruction_error       = (uint32_t) -txn_ctx->exec_err;
        txn_result->instruction_error_index = (uint32_t) instr_err_idx;

        /* If the exec err was a custom instr error and came from a precompile instruction, don't capture the custom error code. */
        if( txn_ctx->exec_err==FD_EXECUTOR_INSTR_ERR_CUSTOM_ERR &&
            fd_executor_lookup_native_precompile_program( &txn_ctx->accounts[ program_id_idx ] )==NULL ) {
          txn_result->custom_error = txn_ctx->custom_err;
        }
      }
    }

    txn_result->has_fee_details                = true;
    txn_result->fee_details.transaction_fee    = slot_ctx->slot_bank.collected_execution_fees;
    txn_result->fee_details.prioritization_fee = slot_ctx->slot_bank.collected_priority_fees;

    /* Rent is only collected on successfully loaded transactions */
    txn_result->rent                           = txn_ctx->collected_rent;

    if( txn_ctx->return_data.len > 0 ) {
      txn_result->return_data = FD_SCRATCH_ALLOC_APPEND( l, alignof(pb_bytes_array_t),
                                      PB_BYTES_ARRAY_T_ALLOCSIZE( txn_ctx->return_data.len ) );
      if( FD_UNLIKELY( _l > output_end ) ) {
        abort();
      }

      txn_result->return_data->size = (pb_size_t)txn_ctx->return_data.len;
      fd_memcpy( txn_result->return_data->bytes, txn_ctx->return_data.data, txn_ctx->return_data.len );
    }

    /* Allocate space for captured accounts */
    ulong modified_acct_cnt = txn_ctx->accounts_cnt;

    txn_result->has_resulting_state         = true;
    txn_result->resulting_state.acct_states =
      FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_acct_state_t),
                                  sizeof (fd_exec_test_acct_state_t) * modified_acct_cnt );
    if( FD_UNLIKELY( _l > output_end ) ) {
      abort();
    }

    /* Capture borrowed accounts */
    for( ushort j=0; j < txn_ctx->accounts_cnt; j++ ) {
      fd_txn_account_t * acc = &txn_ctx->accounts[j];

      /* For fees-only transactions, only save the fee payer (and potentially the nonce) only */
      if( task_info->txn->flags & FD_TXN_P_FLAGS_FEES_ONLY ) {
        if( j!=FD_FEE_PAYER_TXN_IDX && j!=task_info->txn_ctx->nonce_account_idx_in_txn ) {
          continue;
        }
      }

      if( !( fd_exec_txn_ctx_account_is_writable_idx( txn_ctx, j ) || j==FD_FEE_PAYER_TXN_IDX ) ) continue;
      assert( acc->vt->is_mutable( acc ) );

      ulong modified_idx = txn_result->resulting_state.acct_states_count;
      assert( modified_idx < modified_acct_cnt );

      fd_exec_test_acct_state_t * out_acct = &txn_result->resulting_state.acct_states[ modified_idx ];
      memset( out_acct, 0, sizeof(fd_exec_test_acct_state_t) );
      /* Copy over account content */

      memcpy( out_acct->address, acc->pubkey, sizeof(fd_pubkey_t) );

      out_acct->lamports = acc->vt->get_lamports( acc );

      if( acc->vt->get_data_len( acc ) > 0 ) {
        out_acct->data =
          FD_SCRATCH_ALLOC_APPEND( l, alignof(pb_bytes_array_t),
                                      PB_BYTES_ARRAY_T_ALLOCSIZE( acc->vt->get_data_len( acc ) ) );
        if( FD_UNLIKELY( _l > output_end ) ) {
          abort();
        }
        out_acct->data->size = (pb_size_t)acc->vt->get_data_len( acc );
        fd_memcpy( out_acct->data->bytes, acc->vt->get_data( acc ), acc->vt->get_data_len( acc ) );
      }

      out_acct->executable = acc->vt->is_executable( acc );
      out_acct->rent_epoch = acc->vt->get_rent_epoch( acc );
      memcpy( out_acct->owner, acc->vt->get_owner( acc ), sizeof(fd_pubkey_t) );

      txn_result->resulting_state.acct_states_count++;
    }

    ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
    _txn_context_destroy( runner, slot_ctx );

    *output = txn_result;
    return actual_end - (ulong)output_buf;
  } FD_SPAD_FRAME_END;
}


ulong
fd_sbpf_program_load_test_run( fd_exec_instr_test_runner_t * runner,
                               void const *                  input_,
                               void **                       output_,
                               void *                        output_buf,
                               ulong                         output_bufsz ) {
  fd_exec_test_elf_loader_ctx_t const * input  = fd_type_pun_const( input_ );
  fd_exec_test_elf_loader_effects_t **  output = fd_type_pun( output_ );

  fd_sbpf_elf_info_t info;
  fd_valloc_t valloc = fd_spad_virtual( runner->spad );

  if ( FD_UNLIKELY( !input->has_elf || !input->elf.data ) ){
    return 0UL;
  }

  ulong elf_sz = input->elf_sz;
  void const * _bin;

  /* elf_sz will be passed as arguments to elf loader functions.
     pb decoder allocates memory for elf.data based on its actual size,
     not elf_sz !.
     If elf_sz is larger than the size of actual elf data, this may result
     in out-of-bounds accesses which will upset ASAN (however intentional).
     So in this case we just copy the data into a memory region of elf_sz bytes

     ! The decoupling of elf_sz and the actual binary size is intentional to test
      underflow/overflow behavior */
  if ( elf_sz > input->elf.data->size ){
    void * tmp = fd_valloc_malloc( valloc, 1UL, elf_sz );
    if ( FD_UNLIKELY( !tmp ) ){
      return 0UL;
    }
    fd_memcpy( tmp, input->elf.data->bytes, input->elf.data->size );
    _bin = tmp;
  } else {
    _bin = input->elf.data->bytes;
  }

  // Allocate space for captured effects
  ulong output_end = (ulong)output_buf + output_bufsz;
  FD_SCRATCH_ALLOC_INIT( l, output_buf );

  fd_exec_test_elf_loader_effects_t * elf_effects =
    FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_elf_loader_effects_t),
                                sizeof (fd_exec_test_elf_loader_effects_t) );
  if( FD_UNLIKELY( _l > output_end ) ) {
    /* return 0 on fuzz-specific failures */
    return 0UL;
  }
  fd_memset( elf_effects, 0, sizeof(fd_exec_test_elf_loader_effects_t) );

  /* wrap the loader code in do-while(0) block so that we can exit
     immediately if execution fails at any point */

  do{

    if( FD_UNLIKELY( !fd_sbpf_elf_peek( &info, _bin, elf_sz, input->deploy_checks, FD_SBPF_V0, FD_SBPF_V3 ) ) ) {
      /* return incomplete effects on execution failures */
      break;
    }

    void* rodata = fd_valloc_malloc( valloc, FD_SBPF_PROG_RODATA_ALIGN, info.rodata_footprint );
    FD_TEST( rodata );

    fd_sbpf_program_t * prog = fd_sbpf_program_new( fd_valloc_malloc( valloc, fd_sbpf_program_align(), fd_sbpf_program_footprint( &info ) ), &info, rodata );
    FD_TEST( prog );

    fd_sbpf_syscalls_t * syscalls = fd_sbpf_syscalls_new( fd_valloc_malloc( valloc, fd_sbpf_syscalls_align(), fd_sbpf_syscalls_footprint() ));
    FD_TEST( syscalls );

    fd_vm_syscall_register_all( syscalls, 0 );

    int res = fd_sbpf_program_load( prog, _bin, elf_sz, syscalls, input->deploy_checks );
    if( FD_UNLIKELY( res ) ) {
      break;
    }

    fd_memset( elf_effects, 0, sizeof(fd_exec_test_elf_loader_effects_t) );
    elf_effects->rodata_sz = prog->rodata_sz;

    // Load rodata section
    elf_effects->rodata = FD_SCRATCH_ALLOC_APPEND(l, 8UL, PB_BYTES_ARRAY_T_ALLOCSIZE( prog->rodata_sz ));
    if( FD_UNLIKELY( _l > output_end ) ) {
      return 0UL;
    }
    elf_effects->rodata->size = (pb_size_t) prog->rodata_sz;
    fd_memcpy( &(elf_effects->rodata->bytes), prog->rodata, prog->rodata_sz );

    elf_effects->text_cnt = prog->text_cnt;
    elf_effects->text_off = prog->text_off;

    elf_effects->entry_pc = prog->entry_pc;


    pb_size_t calldests_sz = (pb_size_t) fd_sbpf_calldests_cnt( prog->calldests);
    elf_effects->calldests_count = calldests_sz;
    elf_effects->calldests = FD_SCRATCH_ALLOC_APPEND(l, 8UL, calldests_sz * sizeof(uint64_t));
    if( FD_UNLIKELY( _l > output_end ) ) {
      return 0UL;
    }

    ulong i = 0;
    for(ulong target_pc = fd_sbpf_calldests_const_iter_init(prog->calldests); !fd_sbpf_calldests_const_iter_done(target_pc);
    target_pc = fd_sbpf_calldests_const_iter_next(prog->calldests, target_pc)) {
      elf_effects->calldests[i] = target_pc;
      ++i;
    }
  } while(0);

  ulong actual_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );

  *output = elf_effects;
  return actual_end - (ulong) output_buf;
}

static fd_exec_test_instr_effects_t const * cpi_exec_effects = NULL;

ulong
fd_exec_vm_syscall_test_run( fd_exec_instr_test_runner_t * runner,
                             void const *                  input_,
                             void **                       output_,
                             void *                        output_buf,
                             ulong                         output_bufsz ) {
  fd_exec_test_syscall_context_t const * input =  fd_type_pun_const( input_ );
  fd_exec_test_syscall_effects_t **      output = fd_type_pun( output_ );

  /* Create execution context */
  const fd_exec_test_instr_context_t * input_instr_ctx = &input->instr_ctx;
  fd_exec_instr_ctx_t ctx[1];
  // Skip extra checks for non-CPI syscalls
  int is_cpi            = !strncmp( (const char *)input->syscall_invocation.function_name.bytes, "sol_invoke_signed", 17 );
  int skip_extra_checks = !is_cpi;

  if( !fd_exec_test_instr_context_create( runner, ctx, input_instr_ctx, skip_extra_checks ) )
    goto error;
  fd_valloc_t valloc = fd_spad_virtual( runner->spad );
  fd_spad_t * spad = fd_exec_instr_test_runner_get_spad( runner );

  if (is_cpi) {
    ctx->txn_ctx->instr_info_cnt = 1;

    /* Need to setup txn_descriptor for txn account write checks (see fd_exec_txn_ctx_account_is_writable_idx)
       FIXME: this could probably go in fd_exec_test_instr_context_create? */
    fd_txn_t * txn_descriptor = (fd_txn_t *)fd_spad_alloc_debug( spad, fd_txn_align(), fd_txn_footprint( ctx->txn_ctx->instr_info_cnt, 0UL ) );
    txn_descriptor->transaction_version = FD_TXN_V0;
    txn_descriptor->acct_addr_cnt = (ushort)ctx->txn_ctx->accounts_cnt;

    ctx->txn_ctx->txn_descriptor = txn_descriptor;
  }

  ctx->txn_ctx->instr_trace[0].instr_info = (fd_instr_info_t *)ctx->instr;
  ctx->txn_ctx->instr_trace[0].stack_height = 1;

  /* Capture outputs */
  ulong output_end = (ulong)output_buf + output_bufsz;
  FD_SCRATCH_ALLOC_INIT( l, output_buf );
  fd_exec_test_syscall_effects_t * effects =
    FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_test_syscall_effects_t),
                                sizeof (fd_exec_test_syscall_effects_t) );
  if( FD_UNLIKELY( _l > output_end ) ) {
    goto error;
  }

  if (input->vm_ctx.return_data.program_id && input->vm_ctx.return_data.program_id->size == sizeof(fd_pubkey_t)) {
    fd_memcpy( ctx->txn_ctx->return_data.program_id.uc, input->vm_ctx.return_data.program_id->bytes, sizeof(fd_pubkey_t) );
    ctx->txn_ctx->return_data.len = input->vm_ctx.return_data.data->size;
    fd_memcpy( ctx->txn_ctx->return_data.data, input->vm_ctx.return_data.data->bytes, ctx->txn_ctx->return_data.len );
  }

  *effects = (fd_exec_test_syscall_effects_t) FD_EXEC_TEST_SYSCALL_EFFECTS_INIT_ZERO;

  /* Set up the VM instance */
  fd_sha256_t _sha[1];
  fd_sha256_t * sha = fd_sha256_join( fd_sha256_new( _sha ) );
  fd_sbpf_syscalls_t * syscalls = fd_sbpf_syscalls_new( fd_valloc_malloc( valloc, fd_sbpf_syscalls_align(), fd_sbpf_syscalls_footprint() ) );
  fd_vm_syscall_register_all( syscalls, 0 );

  /* Pull out the memory regions */
  if( !input->has_vm_ctx ) {
    goto error;
  }
  if( input->has_exec_effects ){
    cpi_exec_effects = &input->exec_effects;
  }

  ulong rodata_sz = input->vm_ctx.rodata ? input->vm_ctx.rodata->size : 0UL;
  uchar * rodata = fd_valloc_malloc( valloc, 8UL, rodata_sz );
  if ( input->vm_ctx.rodata != NULL ) {
    fd_memcpy( rodata, input->vm_ctx.rodata->bytes, rodata_sz );
  }

  if( input->vm_ctx.heap_max > FD_VM_HEAP_MAX ) {
    goto error;
  }

  fd_vm_t * vm = fd_vm_join( fd_vm_new( fd_valloc_malloc( valloc, fd_vm_align(), fd_vm_footprint() ) ) );
  if ( !vm ) {
    goto error;
  }

  /* If the program ID account owner is the v1 BPF loader, then alignment is disabled (controlled by
     the `is_deprecated` flag) */

  ulong                   input_sz                = 0UL;
  ulong                   pre_lens[256]           = {0};
  fd_vm_input_region_t    input_mem_regions[1000] = {0}; /* We can have a max of (3 * num accounts + 1) regions */
  fd_vm_acc_region_meta_t acc_region_metas[256]   = {0}; /* instr acc idx to idx */
  uint                    input_mem_regions_cnt   = 0U;
  int                     direct_mapping          = FD_FEATURE_ACTIVE( ctx->txn_ctx->slot, ctx->txn_ctx->features, bpf_account_data_direct_mapping );

  uchar *            input_ptr      = NULL;
  uchar              program_id_idx = ctx->instr->program_id;
  fd_txn_account_t * program_acc    = &ctx->txn_ctx->accounts[program_id_idx];
  uchar              is_deprecated  = ( program_id_idx < ctx->txn_ctx->accounts_cnt ) &&
                                      ( !memcmp( program_acc->vt->get_owner( program_acc ), fd_solana_bpf_loader_deprecated_program_id.key, sizeof(fd_pubkey_t) ) );

  /* TODO: Check for an error code. Probably unlikely during fuzzing though */
  fd_bpf_loader_input_serialize_parameters( ctx,
                                            &input_sz,
                                            pre_lens,
                                            input_mem_regions,
                                            &input_mem_regions_cnt,
                                            acc_region_metas,
                                            direct_mapping,
                                            is_deprecated,
                                            &input_ptr );

  fd_vm_init( vm,
              ctx,
              input->vm_ctx.heap_max,
              ctx->txn_ctx->compute_meter,
              rodata,
              rodata_sz,
              NULL, // TODO
              0, // TODO
              0, // TODO
              0, // TODO, text_sz
              0, // TODO
              NULL, // TODO
              TEST_VM_DEFAULT_SBPF_VERSION,
              syscalls,
              NULL, // TODO
              sha,
              input_mem_regions,
              input_mem_regions_cnt,
              acc_region_metas,
              is_deprecated,
              FD_FEATURE_ACTIVE( ctx->txn_ctx->slot, ctx->txn_ctx->features, bpf_account_data_direct_mapping ) );

  // Override some execution state values from the syscall fuzzer input
  // This is so we can test if the syscall mutates any of these erroneously
  vm->reg[0] = input->vm_ctx.r0;
  vm->reg[1] = input->vm_ctx.r1;
  vm->reg[2] = input->vm_ctx.r2;
  vm->reg[3] = input->vm_ctx.r3;
  vm->reg[4] = input->vm_ctx.r4;
  vm->reg[5] = input->vm_ctx.r5;
  vm->reg[6] = input->vm_ctx.r6;
  vm->reg[7] = input->vm_ctx.r7;
  vm->reg[8] = input->vm_ctx.r8;
  vm->reg[9] = input->vm_ctx.r9;
  vm->reg[10] = input->vm_ctx.r10;
  vm->reg[11] = input->vm_ctx.r11;

  // Override initial part of the heap, if specified the syscall fuzzer input
  if( input->syscall_invocation.heap_prefix ) {
    fd_memcpy( vm->heap, input->syscall_invocation.heap_prefix->bytes,
               fd_ulong_min(input->syscall_invocation.heap_prefix->size, vm->heap_max) );
  }

  // Override initial part of the stack, if specified the syscall fuzzer input
  if( input->syscall_invocation.stack_prefix ) {
    fd_memcpy( vm->stack, input->syscall_invocation.stack_prefix->bytes,
               fd_ulong_min(input->syscall_invocation.stack_prefix->size, FD_VM_STACK_MAX) );
  }

  // Look up the syscall to execute
  char * syscall_name = (char *)input->syscall_invocation.function_name.bytes;
  fd_sbpf_syscalls_t const * syscall = lookup_syscall_func(syscalls, syscall_name, input->syscall_invocation.function_name.size);
  if( !syscall ) {
    goto error;
  }

  /* Actually invoke the syscall */
  int stack_push_err = fd_instr_stack_push( ctx->txn_ctx, (fd_instr_info_t *)ctx->instr );
  if( FD_UNLIKELY( stack_push_err ) ) {
      FD_LOG_WARNING(( "instr stack push err" ));
      goto error;
  }
  /* There's an instr ctx struct embedded in the txn ctx instr stack. */
  fd_exec_instr_ctx_t * instr_ctx = &ctx->txn_ctx->instr_stack[ ctx->txn_ctx->instr_stack_sz - 1 ];
  *instr_ctx = (fd_exec_instr_ctx_t) {
    .instr     = ctx->instr,
    .txn_ctx   = ctx->txn_ctx,
    .funk       = ctx->funk,
    .funk_txn  = ctx->funk_txn,
    .parent    = NULL,
    .index     = 0U,
    .depth     = 0U,
    .child_cnt = 0U,
  };
  int syscall_err = syscall->func( vm, vm->reg[1], vm->reg[2], vm->reg[3], vm->reg[4], vm->reg[5], &vm->reg[0] );
  int stack_pop_err = fd_instr_stack_pop( ctx->txn_ctx, ctx->instr );
  if( FD_UNLIKELY( stack_pop_err ) ) {
      FD_LOG_WARNING(( "instr stack pop err" ));
      goto error;
  }
  if( syscall_err ) {
    fd_log_collector_program_failure( vm->instr_ctx );
  }

  /* Capture the effects */
  int exec_err = vm->instr_ctx->txn_ctx->exec_err;
  effects->error = 0;
  if( syscall_err ) {
    if( exec_err==0 ) {
      FD_LOG_WARNING(( "TODO: syscall returns error, but exec_err not set. this is probably missing a log." ));
      effects->error = -1;
    } else {
      effects->error = (exec_err <= 0) ? -exec_err : -1;

      /* Map error kind, equivalent to:
          effects->error_kind = (fd_exec_test_err_kind_t)(vm->instr_ctx->txn_ctx->exec_err_kind); */
      switch (vm->instr_ctx->txn_ctx->exec_err_kind) {
        case FD_EXECUTOR_ERR_KIND_EBPF:
          effects->error_kind = FD_EXEC_TEST_ERR_KIND_EBPF;
          break;
        case FD_EXECUTOR_ERR_KIND_SYSCALL:
          effects->error_kind = FD_EXEC_TEST_ERR_KIND_SYSCALL;
          break;
        case FD_EXECUTOR_ERR_KIND_INSTR:
          effects->error_kind = FD_EXEC_TEST_ERR_KIND_INSTRUCTION;
          break;
        default:
          effects->error_kind = FD_EXEC_TEST_ERR_KIND_UNSPECIFIED;
          break;
      }
    }
  }
  effects->r0 = syscall_err ? 0 : vm->reg[0]; // Save only on success
  effects->cu_avail = (ulong)vm->cu;

  if( vm->heap_max ) {
    effects->heap = FD_SCRATCH_ALLOC_APPEND(
      l, alignof(uint), PB_BYTES_ARRAY_T_ALLOCSIZE( vm->heap_max ) );
    if( FD_UNLIKELY( _l > output_end ) ) {
      goto error;
    }
    effects->heap->size = (uint)vm->heap_max;
    fd_memcpy( effects->heap->bytes, vm->heap, vm->heap_max );
  } else {
    effects->heap = NULL;
  }

  effects->stack = FD_SCRATCH_ALLOC_APPEND(
    l, alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE( FD_VM_STACK_MAX ) );
    if( FD_UNLIKELY( _l > output_end ) ) {
      goto error;
    }
  effects->stack->size = (uint)FD_VM_STACK_MAX;
  fd_memcpy( effects->stack->bytes, vm->stack, FD_VM_STACK_MAX );

  if( vm->rodata_sz ) {
    effects->rodata = FD_SCRATCH_ALLOC_APPEND(
      l, alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE( rodata_sz ) );
    if( FD_UNLIKELY( _l > output_end ) ) {
      goto error;
    }
    effects->rodata->size = (uint)rodata_sz;
    fd_memcpy( effects->rodata->bytes, vm->rodata, rodata_sz );
  } else {
    effects->rodata = NULL;
  }

  effects->frame_count = vm->frame_cnt;

  fd_log_collector_t * log = &vm->instr_ctx->txn_ctx->log_collector;
  /* Only collect log on valid errors (i.e., != -1). Follows
     https://github.com/firedancer-io/solfuzz-agave/blob/99758d3c4f3a342d56e2906936458d82326ae9a8/src/utils/err_map.rs#L148 */
  if( effects->error != -1 && log->buf_sz ) {
    effects->log = FD_SCRATCH_ALLOC_APPEND(
      l, alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE( log->buf_sz ) );
    if( FD_UNLIKELY( _l > output_end ) ) {
      goto error;
    }
    effects->log->size = (uint)fd_log_collector_debug_sprintf( log, (char *)effects->log->bytes, 0 );
  } else {
    effects->log = NULL;
  }

  /* Capture input regions */
  effects->inputdata = NULL; /* Deprecated, using input_data_regions instead */
  ulong tmp_end = FD_SCRATCH_ALLOC_FINI( l, 1UL );
  ulong input_regions_size = load_from_vm_input_regions( vm->input_mem_regions,
                                                        vm->input_mem_regions_cnt,
                                                        &effects->input_data_regions,
                                                        &effects->input_data_regions_count,
                                                        (void *)tmp_end,
                                                        fd_ulong_sat_sub( output_end, tmp_end ) );

  if( !!vm->input_mem_regions_cnt && !effects->input_data_regions ) {
    goto error;
  }

  /* Return the effects */
  ulong actual_end = tmp_end + input_regions_size;
  fd_exec_test_instr_context_destroy( runner, ctx );
  cpi_exec_effects = NULL;

  *output = effects;
  return actual_end - (ulong)output_buf;

error:
  fd_exec_test_instr_context_destroy( runner, ctx );
  cpi_exec_effects = NULL;
  return 0;
}

/* Stubs fd_execute_instr for binaries compiled with
   `-Xlinker --wrap=fd_execute_instr` */
int
__wrap_fd_execute_instr( fd_exec_txn_ctx_t * txn_ctx,
                         fd_instr_info_t *   instr_info )
{
    static const pb_byte_t zero_blk[32] = {0};

    if( cpi_exec_effects == NULL ) {
      FD_LOG_WARNING(( "fd_execute_instr is disabled" ));
      return FD_EXECUTOR_INSTR_SUCCESS;
    }

    // Iterate through instruction accounts
    for( ushort i = 0; i < instr_info->acct_cnt; ++i ) {
      ushort        idx_in_txn  = instr_info->accounts[i].index_in_transaction;
      fd_pubkey_t * acct_pubkey = &txn_ctx->account_keys[ idx_in_txn ];

      fd_txn_account_t * acct = NULL;
      /* Find (first) account in cpi_exec_effects->modified_accounts that matches pubkey */
      for( uint j = 0UL; j < cpi_exec_effects->modified_accounts_count; ++j ) {
        fd_exec_test_acct_state_t * acct_state = &cpi_exec_effects->modified_accounts[j];
        if( memcmp( acct_state->address, acct_pubkey, sizeof(fd_pubkey_t) ) != 0 ) continue;

        /* Fetch borrowed account */
        /* First check if account is read-only.
           TODO: Once direct mapping is enabled we _technically_ don't need
                 this check */

        if( fd_exec_txn_ctx_get_account_at_index( txn_ctx,
                                                  idx_in_txn,
                                                  &acct,
                                                  fd_txn_account_check_exists ) ) {
          break;
        }
        if( !acct->vt->is_mutable( acct ) ){
          break;
        }

        /* Now get account with is_writable check */
        int err = fd_exec_txn_ctx_get_account_at_index( txn_ctx,
                                                        idx_in_txn,
                                                        /* Do not reallocate if data is not going to be modified */
                                                        &acct,
                                                        fd_txn_account_check_is_writable );
        if( err ) break;

        /* Update account state */
        acct->vt->set_lamports( acct, acct_state->lamports );
        acct->vt->set_executable( acct, acct_state->executable );
        acct->vt->set_rent_epoch( acct, acct_state->rent_epoch );

        if( acct_state->data ){
          /* resize manually */
          if( acct_state->data->size > acct->vt->get_data_len( acct ) ) {
            acct->vt->resize( acct, acct_state->data->size );
          }
          acct->vt->set_data( acct, acct_state->data->bytes, acct_state->data->size );
        }

        /* Follow solfuzz-agave, which skips if pubkey is malformed */
        if( memcmp( acct_state->owner, zero_blk, sizeof(fd_pubkey_t) ) != 0 ) {
          acct->vt->set_owner( acct, (fd_pubkey_t const *)acct_state->owner );
        }

        break;
      }
    }
    return FD_EXECUTOR_INSTR_SUCCESS;
}
