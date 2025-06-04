#ifndef HEADER_fd_src_app_shared_commands_fdtop_helpers_h
#define HEADER_fd_src_app_shared_commands_fdtop_helpers_h
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

FD_FN_CONST static inline char*
itoa(ulong value, char* result, ulong base) {
		// check that the base if valid
		if (base < 2 || base > 36) { *result = '\0'; return result; }

		char* ptr = result, *ptr1 = result, tmp_char;
		ulong tmp_value;

		do {
			tmp_value = value;
			value /= base;
			*ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" [35 + (tmp_value - value * base)];
		} while ( value );

		// Apply negative sign
		if (tmp_value < 0) *ptr++ = '-';
		*ptr-- = '\0';
		while(ptr1 < ptr) {
			tmp_char = *ptr;
			*ptr--= *ptr1;
			*ptr1++ = tmp_char;
		}
		return result;
	}
/*FD_FN_CONST double static inline*/
/*scale_plot_num( double unscaled, double min, double max, double tmax){*/
/*  return ((max - min) * (unscaled - (double)0L) / (tmax - (double)0L) + min);*/
/*}*/
/**/
/*FD_FN_CONST static inline u_int16_t*/
/*scaling(u_int32_t in, u_int32_t base)*/
/*{*/
/*    u_int32_t new_base = 2147483647; // 32767*/
/**/
/*    if (in >= base) {*/
/*        return (u_int16_t)new_base;*/
/*    }*/
/**/
/*    if (in != ((in * new_base) / new_base) ) {*/
/*        // overflow, increase bit width!*/
/*        u_int64_t tmp = in * (u_int64_t)new_base;*/
/*        tmp /= base;*/
/*        return (u_int16_t)(tmp);*/
/*    }*/
/**/
/*    // simply multiply*/
/*    u_int32_t tmp = (in * new_base) / base;*/
/**/
/*    if (tmp > new_base) {*/
/*        tmp = new_base; // clamp*/
/*    }*/
/**/
/*    return (uint16_t)tmp;*/
/*}*/
#endif /* HEADER_fd_src_app_shared_commands_fdtop_helpers_h  */
