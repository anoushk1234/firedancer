#ifndef HEADER_fd_src_app_shared_commands_fdtop_widgets_h
#define HEADER_fd_src_app_shared_commands_fdtop_widgets_h
#include "notcurses/notcurses.h"
#include "fdtop.h"
#include "../../../../util/log/fd_log.h"

int
fdtop_plot_graph( struct ncplane* n,
                  const char * title,
                  unsigned int rows,
                  unsigned int cols,
                  int y,
                  int x,
                  ring_buffer* sample );

int
fdtop_render_stats( struct ncplane* parent, const char* title, unsigned int rows, unsigned int cols, int y, int x, char* tag);
#endif
