#include <stdlib.h>
#include <unistd.h>
#include "box.h"
struct ncplane* hud = NULL;
#define HUD_ROWS (3 + 2) // 2 for borders
static const int HUD_COLS = 23 + 2; // 2 for borders
static bool hud_hidden;

int
hud_standard_bg_rgb(struct ncplane* n){
  uint64_t channels = 0;
  ncchannels_set_fg_alpha(&channels, NCALPHA_BLEND);
  ncchannels_set_fg_rgb8(&channels, 0x80, 0x80, 0x80);
  ncchannels_set_bg_alpha(&channels, NCALPHA_BLEND);
  ncchannels_set_bg_rgb8(&channels, 0x80, 0x80, 0x80);
  if(ncplane_set_base(n, "", 0, channels) >= 0){
    return -1;
  }
  return 0;
}

int
hud_refresh(struct ncplane* n){
  ncplane_erase(n);
  nccell ul = NCCELL_TRIVIAL_INITIALIZER, ur = NCCELL_TRIVIAL_INITIALIZER;
  nccell lr = NCCELL_TRIVIAL_INITIALIZER, ll = NCCELL_TRIVIAL_INITIALIZER;
  nccell hl = NCCELL_TRIVIAL_INITIALIZER, vl = NCCELL_TRIVIAL_INITIALIZER;
  if(nccells_rounded_box(n, NCSTYLE_NONE, 0, &ul, &ur, &ll, &lr, &hl, &vl)){
    return -1;
  }
  ul.channels = NCCHANNELS_INITIALIZER(0xf0, 0xc0, 0xc0, 0, 0, 0);
  ur.channels = NCCHANNELS_INITIALIZER(0xf0, 0xc0, 0xc0, 0, 0, 0);
  ll.channels = NCCHANNELS_INITIALIZER(0xf0, 0xc0, 0xc0, 0, 0, 0);
  lr.channels = NCCHANNELS_INITIALIZER(0xf0, 0xc0, 0xc0, 0, 0, 0);
  hl.channels = NCCHANNELS_INITIALIZER(0xf0, 0xc0, 0xc0, 0, 0, 0);
  vl.channels = NCCHANNELS_INITIALIZER(0xf0, 0xc0, 0xc0, 0, 0, 0);
  nccell_set_bg_alpha(&ul, NCALPHA_BLEND);
  nccell_set_bg_alpha(&ur, NCALPHA_BLEND);
  nccell_set_bg_alpha(&ll, NCALPHA_BLEND);
  nccell_set_bg_alpha(&lr, NCALPHA_BLEND);
  nccell_set_bg_alpha(&hl, NCALPHA_BLEND);
  nccell_set_bg_alpha(&vl, NCALPHA_BLEND);
  if(ncplane_perimeter(n, &ul, &ur, &ll, &lr, &hl, &vl, 0)){
    nccell_release(n, &ul); nccell_release(n, &ur); nccell_release(n, &hl);
    nccell_release(n, &ll); nccell_release(n, &lr); nccell_release(n, &vl);
    return -1;
  }
  nccell_release(n, &ul); nccell_release(n, &ur); nccell_release(n, &hl);
  nccell_release(n, &ll); nccell_release(n, &lr); nccell_release(n, &vl);
  return 0;
}

struct ncplane* hud_create(struct notcurses* nc){
  if(hud){
    return NULL;
  }
  unsigned dimx, dimy;
  notcurses_term_dim_yx(nc, &dimy, &dimx);
  struct ncplane_options nopts = {
    // FPS graph is 6 rows tall; we want one row above it
    .y = (int)dimy - 6 - HUD_ROWS - 1,
    .x = NCALIGN_CENTER,
    .rows = HUD_ROWS,
    .cols = HUD_COLS,
    .userptr = NULL,
    .name = "hud",
    .resizecb = ncplane_resize_placewithin,
    .flags = NCPLANE_OPTION_HORALIGNED |
             NCPLANE_OPTION_FIXED,
  };
  struct ncplane* n = ncplane_create(notcurses_stdplane(nc), &nopts);
  if(n == NULL){
    return NULL;
  }
  hud_standard_bg_rgb(n);
  hud_refresh(n);
  ncplane_set_fg_rgb(n, 0xffffff);
  ncplane_set_bg_rgb(n, 0);
  ncplane_set_bg_alpha(n, NCALPHA_BLEND);
  if(hud_hidden){
    ncplane_reparent(n, n);
  }
  return (hud = n);
}


static struct elem* elems; 
#define NANOSECS_IN_SEC 1000000000ul

static int
hud_print_finished(elem* list){
  elem* e = list;
  if(hud){
    hud_refresh(hud);
  }
  int line = 0;
  while(e){
    if(++line == HUD_ROWS - 1){
      if(e->next){
        free(e->next->name);
        free(e->next);
        e->next = NULL;
      }
      break;
    }
    if(hud){
      nccell c = NCCELL_TRIVIAL_INITIALIZER;
      ncplane_base(hud, &c);
      ncplane_set_bg_rgb(hud, nccell_bg_rgb(&c));
      ncplane_set_bg_alpha(hud, NCALPHA_BLEND);
      ncplane_set_fg_rgb(hud, 0xffffff);
      ncplane_set_fg_alpha(hud, NCALPHA_OPAQUE);
      nccell_release(hud, &c);
      if(ncplane_printf_yx(hud, line, 1, "%d", e->frames) < 0){
        return -1;
      }
      char buf[NCPREFIXCOLUMNS + 2];
      ncnmetric(e->totalns, sizeof(buf), NANOSECS_IN_SEC, buf, 0, 1000, '\0');
      for(int x = 6 ; x < 14 - ncstrwidth(buf, NULL, NULL) ; ++x){
        nccell ci = NCCELL_TRIVIAL_INITIALIZER;
        ncplane_putc_yx(hud, 1, x, &ci);
      }
      if(ncplane_printf_yx(hud, line, 14 - ncstrwidth(buf, NULL, NULL), "%ss", buf) < 0){
        return -1;
      }
      if(ncplane_putstr_yx(hud, line, 16, e->name) < 0){
        return -1;
      }
    }
    e = e->next;
  }
  return 0;
}

int hud_schedule(const char* demoname, uint startns){
  elem* cure = malloc(sizeof(*cure));
  if(!cure){
    return -1;
  }
  cure->next = elems;
  cure->name = strdup(demoname);
  cure->totalns = 0;
  cure->frames = 0;
  cure->startns = startns;
  elems = cure;
  return hud_print_finished(elems);
}
