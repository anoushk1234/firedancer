
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <notcurses/notcurses.h>
#define DEMO_RENDER(nc) { int demo_render_err = demo_render(nc); if(demo_render_err){ return demo_render_err; }}


typedef struct elem {
  char* name;
  uint64_t startns;
  uint64_t totalns;
  unsigned frames;
  struct elem* next;
} elem;

int
hud_standard_bg_rgb(struct ncplane* n);

int
hud_refresh(struct ncplane* n);

struct ncplane*
hud_create(struct notcurses* nc);

int
hud_print_finished(elem* list);


int hud_schedule(const char* demoname, uint64_t startns);
