/* Profiler for ruby */
#include "mruby.h"
#include "mruby/irep.h"
#include "mruby/array.h"
#include "mruby/debug.h"
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct prof_counter {
  double time;
  uint32_t num;
};

struct prof_irep {
  mrb_irep *irep;
  struct prof_counter *cnt;
};

struct prof_root {
  int irep_len;
  struct prof_irep *pirep;
};

static struct prof_root result;
static mrb_irep *old_irep = NULL;
static mrb_code *old_pc = NULL;
static double old_time = 0.0;
static mrb_value prof_module;

void
mrb_profiler_reallocinfo(mrb_state* mrb)
{
  int i;
  int j;
  result.pirep = mrb_realloc(mrb, result.pirep, mrb->irep_len * sizeof(struct prof_irep));

  for (i = result.irep_len; i < mrb->irep_len; i++) {
    static struct prof_irep *rirep;
    mrb_irep *irep;

    rirep = &result.pirep[i];
    irep = rirep->irep = mrb->irep[i];
    rirep->cnt = mrb_malloc(mrb, irep->ilen * sizeof(struct prof_counter));
    for (j = 0; j < irep->ilen; j++) {
      rirep->cnt[j].num = 0;
      rirep->cnt[j].time = 0.0;
    }
  }
  result.irep_len = mrb->irep_len;
}

void
prof_code_fetch_hook(struct mrb_state *mrb, struct mrb_irep *irep, mrb_code *pc, mrb_value *regs)
{
  struct timeval tv;
  double curtime;
  unsigned long ctimehi;
  unsigned long ctimelo;
  
  int off;

  if (irep->idx == -1) {
    /* CALL ISEQ */
    return;
  }

  if (irep->idx >= result.irep_len) {
    mrb_profiler_reallocinfo(mrb);
  }
#ifdef __i386__
  asm volatile ("rdtsc\n\t"
		:
		:
		: "%eax", "%edx");
  asm volatile ("mov %%eax, %0\n\t"
		:"=r"(ctimelo));
  asm volatile ("mov %%edx, %0\n\t"
		:"=r"(ctimehi));
  curtime = ((double)ctimehi) * 256.0;
  curtime += ((double)ctimelo / (65536.0 * 256.0));
#else
  gettimeofday(&tv, NULL);
  curtime = ((double)tv.tv_sec) + ((double)tv.tv_usec * 1e-6);
#endif

  if (old_irep) {
    off = old_pc - old_irep->iseq;
    result.pirep[old_irep->idx].cnt[off].time += (curtime - old_time);
  }
  
  off = pc - irep->iseq;
  result.pirep[irep->idx].cnt[off].num++;
  old_irep = irep;
  old_pc = pc;
  old_time = curtime;
}

static mrb_value
mrb_mruby_profiler_irep_len(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(result.irep_len);
}

static mrb_value
mrb_mruby_profiler_ilen(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_get_args(mrb, "i", &irepno);
  
  return mrb_fixnum_value(result.pirep[irepno].irep->ilen);
}

static mrb_value
mrb_mruby_profiler_read(mrb_state *mrb, mrb_value self)
{
  char *fn;
  int len;
  mrb_value res;
  FILE *fp;
  char buf[256];

  mrb_get_args(mrb, "s", &fn, &len);
  fn[len] = '\0';

  res = mrb_ary_new_capa(mrb, 5);
  fp = fopen(fn, "r");
  while (fgets(buf, 255, fp)) {
    int ai = mrb_gc_arena_save(mrb);
    mrb_value ele = mrb_str_new(mrb, buf, strlen(buf));
    
    mrb_ary_push(mrb, res, ele);
    mrb_gc_arena_restore(mrb, ai);
  }
  fclose(fp);
  return res;
}

static mrb_value
mrb_mruby_profiler_get_prof_info(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_int iseqoff;
  mrb_value res;
  const char *str;
  int32_t line;
  mrb_get_args(mrb, "ii", &irepno, &iseqoff);

  res = mrb_ary_new_capa(mrb, 5);

  str = mrb_debug_get_filename(result.pirep[irepno].irep, 0);
  if (str) {
    mrb_ary_push(mrb, res, mrb_str_new(mrb, str, strlen(str)));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  line = mrb_debug_get_line(result.pirep[irepno].irep, iseqoff);
  if (line) {
    mrb_ary_push(mrb, res, mrb_fixnum_value(line));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  mrb_ary_push(mrb, res, 
	       mrb_fixnum_value(result.pirep[irepno].cnt[iseqoff].num));
  mrb_ary_push(mrb, res, 
	       mrb_float_value(mrb, result.pirep[irepno].cnt[iseqoff].time));
  
  return res;
}

void
mrb_mruby_profiler_gem_init(mrb_state* mrb) {
  struct RObject *m;

  mrb_profiler_reallocinfo(mrb);
  m = (struct RObject *)mrb_define_module(mrb, "Profiler");
  prof_module = mrb_obj_value(m);
  mrb->code_fetch_hook = prof_code_fetch_hook;
  mrb_define_singleton_method(mrb, m, "get_prof_info",  
			      mrb_mruby_profiler_get_prof_info, MRB_ARGS_REQ(2));
  mrb_define_singleton_method(mrb, m, "irep_len", mrb_mruby_profiler_irep_len, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, m, "ilen", mrb_mruby_profiler_ilen, MRB_ARGS_REQ(1));
  mrb_define_singleton_method(mrb, m, "read", mrb_mruby_profiler_read, MRB_ARGS_REQ(1));
}

void
mrb_mruby_profiler_gem_final(mrb_state* mrb) {
  mrb_funcall(mrb, prof_module, "analyze", 0);
}
