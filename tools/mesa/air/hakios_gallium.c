/* SPDX-License-Identifier: MIT
 * No private PM4 packets: AIR -> NIR -> the device's actual Gallium driver.
 */
#include "hakios_gallium.h"
#include "nir_air.h"
#include "nir.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "freedreno/drm/freedreno_drm_public.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_helpers.h"
#include "util/u_sampler.h"
#include "util/half_float.h"
#include "util/format/u_format.h"
#include "xf86drm.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

struct hg_context { struct pipe_screen *screen; struct pipe_context *pipe;
 struct pipe_rasterizer_state raster;
 struct pipe_vertex_buffer streams[32];
 struct { const void *source; size_t length; struct pipe_resource *resource; } uploads[64];
 unsigned upload_count; size_t upload_bytes;
 uint64_t started,upload_us,requested,uploaded,hits,bindings;
 void *blend, *dsa, *rast, *ve; driOptionCache *options,*options_info; };
struct hg_texture { struct hg_context *ctx; struct pipe_resource *res; };
struct hg_shader { struct hg_context *ctx; void *cso,*vertex_state; unsigned stream_count; unsigned stage; void *mem; nir_shader *nir; struct air_shader_info info; void *samplers[PIPE_MAX_SAMPLERS]; };
static _Thread_local char last_error[512];
static void fail(const char *s) { snprintf(last_error, sizeof(last_error), "%s", s); }
const char *hg_error(void) { return last_error; }
const char *hg_name(struct hg_context *c) { return c->screen->get_name(c->screen); }
struct hg_context *hg_open(void)
{
 int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
 if (fd < 0) { fail("cannot open msm render device"); return NULL; }
 static const driOptionDescription opts[] = {
#include "freedreno/driinfo_freedreno.h"
 };
 /* Driver retains these caches for the lifetime of the screen. */
 driOptionCache *info = calloc(1, sizeof(*info)), *cache = calloc(1, sizeof(*cache));
 if (!info || !cache) { free(info); free(cache); close(fd); fail("option allocation"); return NULL; }
 driParseOptionInfo(info, opts, ARRAY_SIZE(opts));
 driParseConfigFiles(cache, info, 0, "msm", NULL, NULL, NULL, 0, NULL, 0);
 struct pipe_screen_config config = { .options = cache, .options_info = info };
 struct pipe_screen *screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
 close(fd);
 if (!screen) { fail("freedreno screen creation failed"); return NULL; }
 struct hg_context *c = calloc(1, sizeof(*c));
 if (!c) { screen->destroy(screen); fail("context allocation"); return NULL; }
 c->options=cache; c->options_info=info;
 glsl_type_singleton_init_or_ref();
 c->screen = screen; c->pipe = screen->context_create(screen, NULL, 0);
 if (!c->pipe) { glsl_type_singleton_decref(); screen->destroy(screen); free(c); fail("Gallium context creation"); return NULL; }
 c->blend = c->pipe->create_blend_state(c->pipe, &(struct pipe_blend_state){ .rt[0].colormask = PIPE_MASK_RGBA });
 c->dsa = c->pipe->create_depth_stencil_alpha_state(c->pipe, &(struct pipe_depth_stencil_alpha_state){0});
 c->rast = c->pipe->create_rasterizer_state(c->pipe, &(struct pipe_rasterizer_state){ .depth_clip_near = 1, .depth_clip_far = 1, .half_pixel_center = 1, .bottom_edge_rule = 1, .scissor = 1, .clip_halfz = 1, .flatshade_first = 1, .line_width = 1.0f, .point_size = 1.0f, .point_size_per_vertex = 1 });
 c->pipe->bind_blend_state(c->pipe, c->blend);
 c->pipe->bind_depth_stencil_alpha_state(c->pipe, c->dsa);
 c->pipe->bind_rasterizer_state(c->pipe, c->rast);
 c->ve = c->pipe->create_vertex_elements_state(c->pipe,0,NULL);
 c->pipe->bind_vertex_elements_state(c->pipe,c->ve);
 return c;
}
void hg_flush(struct hg_context *c) { struct pipe_fence_handle *f=NULL; c->pipe->flush(c->pipe, &f, 0); if(f) { c->screen->fence_finish(c->screen,c->pipe,f,UINT64_MAX); c->screen->fence_reference(c->screen,&f,NULL); } }
void hg_close(struct hg_context *c) {
 if (!c) return;
 hg_flush(c);
 for(unsigned i=0;i<32;i++)pipe_resource_reference(&c->streams[i].buffer.resource,NULL);
 c->pipe->delete_blend_state(c->pipe,c->blend);
 c->pipe->delete_depth_stencil_alpha_state(c->pipe,c->dsa);
 c->pipe->delete_rasterizer_state(c->pipe,c->rast);
 c->pipe->delete_vertex_elements_state(c->pipe,c->ve);
 c->pipe->destroy(c->pipe); c->screen->destroy(c->screen);
 driDestroyOptionCache(c->options); driDestroyOptionInfo(c->options_info); free(c->options); free(c->options_info); free(c);
 glsl_type_singleton_decref();
}
static enum pipe_format format(unsigned f) {
 switch(f) {
 case 10: return PIPE_FORMAT_R8_UNORM;
 case 30: return PIPE_FORMAT_R8G8_UNORM;
 case 40: return PIPE_FORMAT_B5G6R5_UNORM;
 case 55: return PIPE_FORMAT_R32_FLOAT;
 case 70: return PIPE_FORMAT_R8G8B8A8_UNORM;
 case 71: return PIPE_FORMAT_R8G8B8A8_SRGB;
 case 80: return PIPE_FORMAT_B8G8R8A8_UNORM;
 case 81: return PIPE_FORMAT_B8G8R8A8_SRGB;
 case 112: return PIPE_FORMAT_R16G16B16A16_FLOAT;
 case 252: return PIPE_FORMAT_Z32_FLOAT;
 case 260: return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
 default: return PIPE_FORMAT_NONE;
 }
}
struct hg_texture *hg_texture_new(struct hg_context *c, unsigned w, unsigned h, unsigned fmt, unsigned levels, unsigned samples, unsigned type) {
 enum pipe_format f = format(fmt);
 if (!w || !h || !levels || f == PIPE_FORMAT_NONE || (samples != 1 && samples != 4)) { fail("unsupported texture description"); return NULL; }
 unsigned target=type==5 ? PIPE_TEXTURE_CUBE : PIPE_TEXTURE_2D;
 unsigned layers=type==5 ? 6 : 1;
 unsigned ns=samples>1 ? samples : 0;
 unsigned bind = PIPE_BIND_SAMPLER_VIEW | (util_format_is_depth_or_stencil(f) ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET);
 if (!c->screen->is_format_supported(c->screen, f, target, ns, ns, bind)) { fail("unsupported texture format"); return NULL; }
 struct hg_texture *t = calloc(1,sizeof(*t));
 if (!t) return NULL;
 t->ctx = c;
 t->res = c->screen->resource_create(c->screen, &(struct pipe_resource){ .target=target, .format=f, .width0=w, .height0=h, .depth0=1, .array_size=layers, .nr_samples=ns, .nr_storage_samples=ns, .last_level=levels-1, .bind=bind });
 if (!t->res) { free(t); fail("texture allocation failed"); return NULL; }
 return t;
}
void hg_texture_free(struct hg_texture *t) { if(t) { pipe_resource_reference(&t->res,NULL); free(t); } }
static int transfer(struct hg_texture *t, unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, void *data, size_t stride, int write, unsigned slice) {
 if (!t || slice>=t->res->array_size || level > t->res->last_level || !data || x+w > u_minify(t->res->width0,level) || y+h > u_minify(t->res->height0,level)) { fail("texture transfer bounds"); return -1; }
 size_t row = util_format_get_stride(t->res->format,w);
 if (stride < row) { fail("texture transfer stride"); return -1; }
 struct pipe_context *p = t->ctx->pipe; struct pipe_transfer *map;
 void *ptr = p->texture_map(p,t->res,level,write ? PIPE_MAP_WRITE : PIPE_MAP_READ,&(struct pipe_box){.x=x,.y=y,.z=slice,.width=w,.height=h,.depth=1},&map);
 if (!ptr) { fail("texture map failed"); return -1; }
 for(unsigned i=0;i<h;i++) {
 if(write) memcpy((char *)ptr+i*map->stride,(char *)data+i*stride,row);
 else memcpy((char *)data+i*stride,(char *)ptr+i*map->stride,row);
 }
 p->texture_unmap(p,map); return 0;
}
int hg_texture_write(struct hg_texture *t,unsigned l,unsigned x,unsigned y,unsigned w,unsigned h,const void *p,size_t s) { return transfer(t,l,x,y,w,h,(void *)p,s,1,0); }
int hg_texture_read(struct hg_texture *t,unsigned l,unsigned x,unsigned y,unsigned w,unsigned h,void *p,size_t s) { return transfer(t,l,x,y,w,h,p,s,0,0); }
int hg_texture_write_slice(struct hg_texture *t,unsigned l,unsigned slice,unsigned x,unsigned y,unsigned w,unsigned h,const void *p,size_t s) { return transfer(t,l,x,y,w,h,(void *)p,s,1,slice); }
static int slots(const struct glsl_type *t,bool b) { return glsl_count_attribute_slots(t,false); }
struct hg_shader *hg_shader_new(struct hg_context *c,const void *data,size_t size,const char *entry,unsigned stage,const struct hg_constant *values,unsigned num_values) {
 if(stage>1) { fail("unsupported shader stage"); return NULL; }
 unsigned ps = stage ? PIPE_SHADER_FRAGMENT : PIPE_SHADER_VERTEX;
 void *mem=ralloc_context(NULL); struct air_shader_info info; const char *error;
 struct air_constant_value *fc; unsigned n=air_module_function_constants(mem,data,size,&fc);
 for(unsigned i=0;i<n;i++)for(unsigned j=0;j<num_values;j++)if(fc[i].index==values[j].index)fc[i].bits=values[j].bits;
 struct air_to_nir_options ao={.constants=fc,.num_constants=n};
 nir_shader *nir=air_to_nir(mem,data,size,entry,&ao,c->screen->get_compiler_options(c->screen,PIPE_SHADER_IR_NIR,ps),&info,&error);
 if(!nir) { fail(error); ralloc_free(mem); return NULL; }
 if(info.stage != (stage ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX)) { fail("shader stage mismatch"); ralloc_free(mem); return NULL; }
 struct hg_shader *s=calloc(1,sizeof(*s));
 if(!s) { ralloc_free(mem); return NULL; }
 s->ctx=c;s->stage=stage;s->mem=mem;s->nir=nir;s->info=info;
 return s;
}
static struct pipe_sampler_state
sampler_from_air(const struct air_sampler_state *a, bool compares)
{
   static const unsigned wrap[] = {
      [AIR_SAMPLER_CLAMP_TO_ZERO] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
      [AIR_SAMPLER_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      [AIR_SAMPLER_REPEAT] = PIPE_TEX_WRAP_REPEAT,
      [AIR_SAMPLER_MIRRORED_REPEAT] = PIPE_TEX_WRAP_MIRROR_REPEAT,
      [AIR_SAMPLER_CLAMP_TO_BORDER] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
   };
   static const unsigned filt[] = {
      [AIR_SAMPLER_NEAREST] = PIPE_TEX_FILTER_NEAREST,
      [AIR_SAMPLER_LINEAR] = PIPE_TEX_FILTER_LINEAR,
      [AIR_SAMPLER_BICUBIC] = PIPE_TEX_FILTER_LINEAR,
   };
   static const unsigned mip[] = {
      [AIR_SAMPLER_MIP_NONE] = PIPE_TEX_MIPFILTER_NONE,
      [AIR_SAMPLER_MIP_NEAREST] = PIPE_TEX_MIPFILTER_NEAREST,
      [AIR_SAMPLER_MIP_LINEAR] = PIPE_TEX_MIPFILTER_LINEAR,
   };
   struct pipe_sampler_state st = {
      .wrap_s = wrap[a->address[0] % ARRAY_SIZE(wrap)],
      .wrap_t = wrap[a->address[1] % ARRAY_SIZE(wrap)],
      .wrap_r = wrap[a->address[2] % ARRAY_SIZE(wrap)],
      .mag_img_filter = filt[a->mag_filter % ARRAY_SIZE(filt)],
      .min_img_filter = filt[a->min_filter % ARRAY_SIZE(filt)],
      .min_mip_filter = mip[a->mip_filter % ARRAY_SIZE(mip)],
      .unnormalized_coords = a->pixel_coord,
      .max_anisotropy = a->max_anisotropy > 1 ? a->max_anisotropy : 0,
      .min_lod = _mesa_half_to_float(a->lod_min),
      .max_lod = _mesa_half_to_float(a->lod_max),
   };
   /* The comparison, where the shader asked for one. The table is the one in
    * air_metal.c's sampler_state_from_air -- MTLCompareFunction and
    * PIPE_FUNC_x run in the same order, so one table serves both -- and it is
    * written out here rather than cast because 2 `less_equal` would arrive as
    * PIPE_FUNC_EQUAL and 3 `greater` as PIPE_FUNC_LEQUAL, which is the
    * opposite comparison and still a picture.
    */
   if (compares) {
      static const unsigned func[] = {
         [0] = PIPE_FUNC_NEVER,     /* none: no MTLCompareFunction has a name for it */
         [1] = PIPE_FUNC_LESS,      /* less */
         [2] = PIPE_FUNC_LEQUAL,    /* less_equal */
         [3] = PIPE_FUNC_GREATER,   /* greater */
         [4] = PIPE_FUNC_GEQUAL,    /* greater_equal */
         [5] = PIPE_FUNC_EQUAL,     /* equal */
         [6] = PIPE_FUNC_NOTEQUAL,  /* not_equal */
         [7] = PIPE_FUNC_ALWAYS,    /* always */
         [8] = PIPE_FUNC_NEVER,     /* never */
      };
      st.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
      st.compare_func = func[a->compare_func % ARRAY_SIZE(func)];
   }

   /* Metal's border_color: 0 transparent black, 1 opaque black, 2 opaque white.
    * clamp_to_zero does not read it -- it is transparent black by definition.
    */
   if (a->address[0] == AIR_SAMPLER_CLAMP_TO_BORDER ||
       a->address[1] == AIR_SAMPLER_CLAMP_TO_BORDER ||
       a->address[2] == AIR_SAMPLER_CLAMP_TO_BORDER) {
      float v = a->border_color == 2 ? 1.0f : 0.0f;
      st.border_color.f[0] = st.border_color.f[1] = st.border_color.f[2] = v;
      st.border_color.f[3] = a->border_color == 0 ? 0.0f : 1.0f;
   }
   return st;
}

static void hg_inline_samplers(struct hg_shader *s) {
 unsigned ps=s->stage?PIPE_SHADER_FRAGMENT:PIPE_SHADER_VERTEX;
 for(unsigned i=0;i<PIPE_MAX_SAMPLERS;i++)if(s->samplers[i])s->ctx->pipe->bind_sampler_states(s->ctx->pipe,ps,i,1,&s->samplers[i]);
}
static int hg_compile(struct hg_shader *s) {
 nir_shader *nir=s->nir;
 static unsigned dumped;
 const char *names[]={"Grid::grid_vertex","PolygonFill::polygonFill_vertex"};
 for(unsigned i=0;i<2;i++)if(nir->info.name && !strcmp(nir->info.name,names[i]) && !(dumped&(1u<<i))) {
  dumped|=1u<<i;char path[128];snprintf(path,sizeof(path),"/private/var/tmp/jm21-shader-%u.nir",i);
  FILE *f=fopen(path,"w");if(f){nir_print_shader(nir,f);fclose(f);}
 }
 bool shadow[PIPE_MAX_SAMPLERS]={0};
 nir_foreach_function_impl(impl,nir) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
  if(instr->type==nir_instr_type_tex) {nir_tex_instr *tex=nir_instr_as_tex(instr);if(tex->is_shadow && tex->sampler_index<PIPE_MAX_SAMPLERS)shadow[tex->sampler_index]=true;}
 }
 for(unsigned i=0;i<s->info.num_resources;i++) {
  struct air_resource *r=&s->info.resources[i];
  if(r->kind!=AIR_RESOURCE_SAMPLER || !r->inline_sampler)continue;
  if(r->index>=PIPE_MAX_SAMPLERS) {fail("inline sampler index out of range");return -1;}
  if(r->sampler.min_filter==AIR_SAMPLER_BICUBIC || r->sampler.mag_filter==AIR_SAMPLER_BICUBIC) {fail("unsupported bicubic inline sampler");return -1;}
  struct pipe_sampler_state state=sampler_from_air(&r->sampler,shadow[r->index]);
  s->samplers[r->index]=s->ctx->pipe->create_sampler_state(s->ctx->pipe,&state);
 }
 NIR_PASS(_,nir,nir_lower_system_values);
 NIR_PASS(_,nir,nir_lower_io,nir_var_shader_in|nir_var_shader_out,slots,nir_lower_io_use_interpolated_input_intrinsics);
 ralloc_steal(NULL,nir);
 struct pipe_shader_state state={.type=PIPE_SHADER_IR_NIR,.ir.nir=nir};
 s->cso=s->stage?s->ctx->pipe->create_fs_state(s->ctx->pipe,&state):s->ctx->pipe->create_vs_state(s->ctx->pipe,&state);
 s->nir=NULL;
 if(!s->cso) { fail("shader driver compilation failed");return -1; }
 return 0;
}
/* Specialization can leave declared inputs in branches that are now dead.
 * Remove those loads before validating the vertex/fragment interface. */
static void hg_specialize_io(nir_shader *nir) {
 bool progress;
 do {
  progress=false;
  NIR_PASS(progress,nir,nir_copy_prop);
  NIR_PASS(progress,nir,nir_opt_constant_folding);
  NIR_PASS(progress,nir,nir_opt_algebraic);
  NIR_PASS(progress,nir,nir_opt_if,0);
  NIR_PASS(progress,nir,nir_opt_dead_cf);
  NIR_PASS(progress,nir,nir_opt_dce);
 } while(progress);
 NIR_PASS(_,nir,nir_remove_dead_variables,nir_var_shader_in,NULL);
}
/* Format mapping and attribute-to-driver-location contract adapted from
 * Chlorophyll air_metal.c build_vertex_state (MIT), revision 787338fc. */
int hg_vertex_state(struct hg_shader *s,const struct hg_vertex_attribute *attrs,unsigned n) {
 struct pipe_vertex_element elements[32]={0}; unsigned count=0;
 for(unsigned i=0;i<s->info.num_vertex_attributes;i++) {
  const struct air_vertex_attribute *a=&s->info.vertex_attributes[i];
  if(!a->used)continue;
  if(a->index>=n || a->driver_location>=32) {fail("vertex attribute outside descriptor");return -1;}
  const struct hg_vertex_attribute *d=&attrs[a->index];
  enum pipe_format f=PIPE_FORMAT_NONE;
  switch(d->format) {
   case 1: f=PIPE_FORMAT_R8G8_UINT;break;
   case 2: f=PIPE_FORMAT_R8G8B8_UINT;break;
   case 3: f=PIPE_FORMAT_R8G8B8A8_UINT;break;
   case 4: f=PIPE_FORMAT_R8G8_SINT;break;
   case 5: f=PIPE_FORMAT_R8G8B8_SINT;break;
   case 6: f=PIPE_FORMAT_R8G8B8A8_SINT;break;
   case 7: f=PIPE_FORMAT_R8G8_UNORM;break;
   case 8: f=PIPE_FORMAT_R8G8B8_UNORM;break;
   case 9: f=PIPE_FORMAT_R8G8B8A8_UNORM;break;
   case 10: f=PIPE_FORMAT_R8G8_SNORM;break;
   case 11: f=PIPE_FORMAT_R8G8B8_SNORM;break;
   case 12: f=PIPE_FORMAT_R8G8B8A8_SNORM;break;
   case 13: f=PIPE_FORMAT_R16G16_UINT;break;
   case 14: f=PIPE_FORMAT_R16G16B16_UINT;break;
   case 15: f=PIPE_FORMAT_R16G16B16A16_UINT;break;
   case 16: f=PIPE_FORMAT_R16G16_SINT;break;
   case 17: f=PIPE_FORMAT_R16G16B16_SINT;break;
   case 18: f=PIPE_FORMAT_R16G16B16A16_SINT;break;
   case 19: f=PIPE_FORMAT_R16G16_UNORM;break;
   case 20: f=PIPE_FORMAT_R16G16B16_UNORM;break;
   case 21: f=PIPE_FORMAT_R16G16B16A16_UNORM;break;
   case 22: f=PIPE_FORMAT_R16G16_SNORM;break;
   case 23: f=PIPE_FORMAT_R16G16B16_SNORM;break;
   case 24: f=PIPE_FORMAT_R16G16B16A16_SNORM;break;
   case 25: f=PIPE_FORMAT_R16G16_FLOAT;break;
   case 26: f=PIPE_FORMAT_R16G16B16_FLOAT;break;
   case 27: f=PIPE_FORMAT_R16G16B16A16_FLOAT;break;
   case 28: f=PIPE_FORMAT_R32_FLOAT;break;
   case 29: f=PIPE_FORMAT_R32G32_FLOAT;break;
   case 30: f=PIPE_FORMAT_R32G32B32_FLOAT;break;
   case 31: f=PIPE_FORMAT_R32G32B32A32_FLOAT;break;
   case 32: f=PIPE_FORMAT_R32_SINT;break;
   case 33: f=PIPE_FORMAT_R32G32_SINT;break;
   case 34: f=PIPE_FORMAT_R32G32B32_SINT;break;
   case 35: f=PIPE_FORMAT_R32G32B32A32_SINT;break;
   case 36: f=PIPE_FORMAT_R32_UINT;break;
   case 37: f=PIPE_FORMAT_R32G32_UINT;break;
   case 38: f=PIPE_FORMAT_R32G32B32_UINT;break;
   case 39: f=PIPE_FORMAT_R32G32B32A32_UINT;break;
   case 42: f=PIPE_FORMAT_B8G8R8A8_UNORM;break;
   case 45: f=PIPE_FORMAT_R8_UINT;break;
   case 46: f=PIPE_FORMAT_R8_SINT;break;
   case 47: f=PIPE_FORMAT_R8_UNORM;break;
   case 48: f=PIPE_FORMAT_R8_SNORM;break;
   case 49: f=PIPE_FORMAT_R16_UINT;break;
   case 50: f=PIPE_FORMAT_R16_SINT;break;
   case 51: f=PIPE_FORMAT_R16_UNORM;break;
   case 52: f=PIPE_FORMAT_R16_SNORM;break;
   case 53: f=PIPE_FORMAT_R16_FLOAT;break;
  }
  if(f==PIPE_FORMAT_NONE || d->buffer>=31 || d->offset>4095 || d->step>2 ||
   !s->ctx->screen->is_format_supported(s->ctx->screen,f,PIPE_BUFFER,0,0,PIPE_BIND_VERTEX_BUFFER)) {
   fail("unsupported vertex descriptor format/layout");return -1;
  }
  elements[a->driver_location]=(struct pipe_vertex_element){.src_format=f,.src_offset=d->offset,
   .src_stride=d->step?d->stride:0,.vertex_buffer_index=d->buffer,
   .instance_divisor=d->step==2?(d->rate?d->rate:1):0};
  count=MAX2(count,a->driver_location+1);s->stream_count=MAX2(s->stream_count,d->buffer+1);
 }
 s->vertex_state=s->ctx->pipe->create_vertex_elements_state(s->ctx->pipe,count,elements);
 if(!s->vertex_state){fail("vertex state creation failed");return -1;}return 0;
}
int hg_shader_link(struct hg_shader *vs,struct hg_shader *fs) {
 if(!vs||!fs||!vs->nir||!fs->nir) {fail("invalid shader link");return -1;}
 hg_specialize_io(vs->nir);hg_specialize_io(fs->nir);
 nir_foreach_shader_in_variable(var,fs->nir) {
  if(var->data.location<VARYING_SLOT_VAR0)continue;
  const char *name=NULL;
  for(unsigned i=0;i<fs->info.num_varyings;i++)if(fs->info.varyings[i].slot==var->data.location)name=fs->info.varyings[i].name;
  if(!name)continue;
  bool found=false;
  for(unsigned i=0;i<vs->info.num_varyings;i++)if(vs->info.varyings[i].name && !strcmp(name,vs->info.varyings[i].name)) {var->data.location=vs->info.varyings[i].slot;found=true;break;}
  if(!found){
   fprintf(stderr,"HakiosMetal: interface %s -> %s missing %s; vertex outputs:",vs->nir->info.name,fs->nir->info.name,name);
   for(unsigned i=0;i<vs->info.num_varyings;i++)fprintf(stderr," %s@%u",vs->info.varyings[i].name,vs->info.varyings[i].slot);
   fprintf(stderr,"\n");
   snprintf(last_error,sizeof(last_error),"unlinked varying %s",name);return -1;
  }
 }
 return hg_compile(vs)||hg_compile(fs)?-1:0;
}
void hg_shader_free(struct hg_shader *s) { if(s) { if(s->vertex_state)s->ctx->pipe->delete_vertex_elements_state(s->ctx->pipe,s->vertex_state); for(unsigned i=0;i<PIPE_MAX_SAMPLERS;i++)if(s->samplers[i])s->ctx->pipe->delete_sampler_state(s->ctx->pipe,s->samplers[i]); if(s->cso) { if(s->stage) s->ctx->pipe->delete_fs_state(s->ctx->pipe,s->cso);else s->ctx->pipe->delete_vs_state(s->ctx->pipe,s->cso); } ralloc_free(s->mem);free(s); } }
int hg_target(struct hg_context *c,struct hg_texture *t,const double clear[4],int load,struct hg_texture *depth,double clear_depth,unsigned clear_stencil,unsigned clear_flags) {
 if(!t || t->ctx!=c) { fail("invalid render target"); return -1; }
 struct pipe_context *p=c->pipe;
 struct pipe_surface *s=p->create_surface(p,t->res,&(struct pipe_surface){.format=t->res->format});
 if(!s) { fail("render surface creation"); return -1; }
 p->set_scissor_states(p,0,1,&(struct pipe_scissor_state){.maxx=t->res->width0,.maxy=t->res->height0});
 struct pipe_surface *zs=depth?p->create_surface(p,depth->res,&(struct pipe_surface){.format=depth->res->format}):NULL;
 p->set_framebuffer_state(p,&(struct pipe_framebuffer_state){.width=t->res->width0,.height=t->res->height0,.samples=t->res->nr_samples,.nr_cbufs=1,.cbufs={s},.zsbuf=zs});
 pipe_surface_reference(&zs,NULL);
 pipe_surface_reference(&s,NULL);
 p->set_viewport_states(p,0,1,&(struct pipe_viewport_state){.scale={t->res->width0/2.f,-t->res->height0/2.f,1.f},.translate={t->res->width0/2.f,t->res->height0/2.f,0.f}});
 unsigned flags=(!load?PIPE_CLEAR_COLOR:0)|((clear_flags&1)?PIPE_CLEAR_DEPTH:0)|((clear_flags&2)?PIPE_CLEAR_STENCIL:0);
 if(flags) { union pipe_color_union color={.f={clear[0],clear[1],clear[2],clear[3]}}; p->clear(p,flags,NULL,&color,clear_depth,clear_stencil); }
 return 0;
}
static unsigned hg_compare(unsigned m) {
 static const unsigned map[]={PIPE_FUNC_NEVER,PIPE_FUNC_LESS,PIPE_FUNC_EQUAL,PIPE_FUNC_LEQUAL,PIPE_FUNC_GREATER,PIPE_FUNC_NOTEQUAL,PIPE_FUNC_GEQUAL,PIPE_FUNC_ALWAYS};
 return map[m<8?m:7];
}
static unsigned hg_stencil_op(unsigned m) {
 static const unsigned map[]={PIPE_STENCIL_OP_KEEP,PIPE_STENCIL_OP_ZERO,PIPE_STENCIL_OP_REPLACE,PIPE_STENCIL_OP_INCR,PIPE_STENCIL_OP_DECR,PIPE_STENCIL_OP_INVERT,PIPE_STENCIL_OP_INCR_WRAP,PIPE_STENCIL_OP_DECR_WRAP};
 return map[m<8?m:0];
}
void hg_depth_stencil(struct hg_context *c,unsigned compare,unsigned write,const struct hg_stencil *front,const struct hg_stencil *back) {
 struct pipe_depth_stencil_alpha_state d={.depth_enabled=(compare!=7||write),.depth_writemask=write,.depth_func=hg_compare(compare)};
 const struct hg_stencil *st[]={front,back};
 for(unsigned i=0;i<2;i++) if(st[i]) d.stencil[i]=(struct pipe_stencil_state){.enabled=st[i]->enabled,.func=hg_compare(st[i]->compare),.fail_op=hg_stencil_op(st[i]->fail),.zfail_op=hg_stencil_op(st[i]->depth_fail),.zpass_op=hg_stencil_op(st[i]->pass),.valuemask=st[i]->read_mask,.writemask=st[i]->write_mask};
 void *state=c->pipe->create_depth_stencil_alpha_state(c->pipe,&d);
 c->pipe->bind_depth_stencil_alpha_state(c->pipe,state);
 c->pipe->delete_depth_stencil_alpha_state(c->pipe,c->dsa);c->dsa=state;
}
void hg_stencil_ref(struct hg_context *c,unsigned front,unsigned back) { c->pipe->set_stencil_ref(c->pipe,(struct pipe_stencil_ref){.ref_value={front,back}}); }
int hg_draw(struct hg_context *c,struct hg_shader *vs,struct hg_shader *fs,unsigned prim,unsigned start,unsigned count,unsigned instances) {
 static const unsigned modes[]={MESA_PRIM_POINTS,MESA_PRIM_LINES,MESA_PRIM_LINE_STRIP,MESA_PRIM_TRIANGLES,MESA_PRIM_TRIANGLE_STRIP};
 if(!vs||!fs||vs->ctx!=c||fs->ctx!=c||prim>=ARRAY_SIZE(modes)) { fail("invalid draw state"); return -1; }
 hg_inline_samplers(vs);hg_inline_samplers(fs);
 c->pipe->bind_vertex_elements_state(c->pipe,vs->vertex_state?vs->vertex_state:c->ve);
 /* set_vertex_buffers takes ownership of one reference per binding. */
 for(unsigned i=0;i<vs->stream_count;i++)if(c->streams[i].buffer.resource)pipe_reference(NULL,&c->streams[i].buffer.resource->reference);
 c->pipe->set_vertex_buffers(c->pipe,vs->stream_count,c->streams);
 c->pipe->bind_vs_state(c->pipe,vs->cso); c->pipe->bind_fs_state(c->pipe,fs->cso);
 struct pipe_draw_info info={.mode=modes[prim],.instance_count=instances};
 struct pipe_draw_start_count_bias draw={.start=start,.count=count};
 c->pipe->draw_vbo(c->pipe,&info,0,NULL,&draw,1); return 0;
}

int hg_draw_indexed(struct hg_context *c,struct hg_shader *vs,struct hg_shader *fs,unsigned prim,unsigned count,unsigned size,const void *indices,unsigned instances,unsigned baseinstance,int basevertex) {
 static const unsigned modes[]={MESA_PRIM_POINTS,MESA_PRIM_LINES,MESA_PRIM_LINE_STRIP,MESA_PRIM_TRIANGLES,MESA_PRIM_TRIANGLE_STRIP};
 if(!vs||!fs||vs->ctx!=c||fs->ctx!=c||prim>=ARRAY_SIZE(modes)||(size!=2&&size!=4)||!indices) { fail("invalid indexed draw state"); return -1; }
 hg_inline_samplers(vs);hg_inline_samplers(fs);
 c->pipe->bind_vertex_elements_state(c->pipe,vs->vertex_state?vs->vertex_state:c->ve);
 /* set_vertex_buffers takes ownership of one reference per binding. */
 for(unsigned i=0;i<vs->stream_count;i++)if(c->streams[i].buffer.resource)pipe_reference(NULL,&c->streams[i].buffer.resource->reference);
 c->pipe->set_vertex_buffers(c->pipe,vs->stream_count,c->streams);
 c->pipe->bind_vs_state(c->pipe,vs->cso); c->pipe->bind_fs_state(c->pipe,fs->cso);
 struct pipe_draw_info info={.mode=modes[prim],.instance_count=instances,.start_instance=baseinstance,.index_size=size,.has_user_indices=true,.index.user=indices};
 struct pipe_draw_start_count_bias draw={.count=count,.index_bias=basevertex};
 c->pipe->draw_vbo(c->pipe,&info,0,NULL,&draw,1); return 0;
}
void *hg_sampler_new(struct hg_context *c,unsigned min,unsigned mag,unsigned mip,unsigned ws,unsigned wt,unsigned wr,float lo,float hi) {
 static const unsigned wrap[]={PIPE_TEX_WRAP_CLAMP_TO_EDGE,PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE,PIPE_TEX_WRAP_REPEAT,PIPE_TEX_WRAP_MIRROR_REPEAT,PIPE_TEX_WRAP_CLAMP_TO_BORDER,PIPE_TEX_WRAP_CLAMP_TO_BORDER};
 struct pipe_sampler_state state={.min_img_filter=min?PIPE_TEX_FILTER_LINEAR:PIPE_TEX_FILTER_NEAREST,.mag_img_filter=mag?PIPE_TEX_FILTER_LINEAR:PIPE_TEX_FILTER_NEAREST,.min_mip_filter=mip==0?PIPE_TEX_MIPFILTER_NONE:mip==1?PIPE_TEX_MIPFILTER_NEAREST:PIPE_TEX_MIPFILTER_LINEAR,.wrap_s=wrap[MIN2(ws,5)],.wrap_t=wrap[MIN2(wt,5)],.wrap_r=wrap[MIN2(wr,5)],.min_lod=lo,.max_lod=hi};
 return c->pipe->create_sampler_state(c->pipe,&state);
}
void hg_sampler_free(struct hg_context *c,void *state) { if(state)c->pipe->delete_sampler_state(c->pipe,state); }
void hg_sampler_bind(struct hg_context *c,unsigned fragment,unsigned index,void *state) { if(index<PIPE_MAX_SAMPLERS)c->pipe->bind_sampler_states(c->pipe,fragment?PIPE_SHADER_FRAGMENT:PIPE_SHADER_VERTEX,index,1,&state); }

int hg_resolve(struct hg_texture *src,struct hg_texture *dst) {
 if(!src||!dst||src->ctx!=dst->ctx||src->res->width0!=dst->res->width0||src->res->height0!=dst->res->height0) { fail("invalid resolve textures"); return -1; }
 struct pipe_blit_info b={.src={.resource=src->res,.format=src->res->format,.box={.width=src->res->width0,.height=src->res->height0,.depth=1}},.dst={.resource=dst->res,.format=dst->res->format,.box={.width=dst->res->width0,.height=dst->res->height0,.depth=1}},.mask=PIPE_MASK_RGBA,.filter=PIPE_TEX_FILTER_NEAREST};
 src->ctx->pipe->blit(src->ctx->pipe,&b); return 0;
}
void hg_blend(struct hg_context *c,unsigned enabled,unsigned sr,unsigned dr,unsigned sa,unsigned da,unsigned rop,unsigned aop,unsigned mask) {
 static const unsigned factors[]={PIPE_BLENDFACTOR_ZERO,PIPE_BLENDFACTOR_ONE,PIPE_BLENDFACTOR_SRC_COLOR,PIPE_BLENDFACTOR_INV_SRC_COLOR,PIPE_BLENDFACTOR_SRC_ALPHA,PIPE_BLENDFACTOR_INV_SRC_ALPHA,PIPE_BLENDFACTOR_DST_COLOR,PIPE_BLENDFACTOR_INV_DST_COLOR,PIPE_BLENDFACTOR_DST_ALPHA,PIPE_BLENDFACTOR_INV_DST_ALPHA,PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE,PIPE_BLENDFACTOR_CONST_COLOR,PIPE_BLENDFACTOR_INV_CONST_COLOR,PIPE_BLENDFACTOR_CONST_ALPHA,PIPE_BLENDFACTOR_INV_CONST_ALPHA,PIPE_BLENDFACTOR_SRC1_COLOR,PIPE_BLENDFACTOR_INV_SRC1_COLOR,PIPE_BLENDFACTOR_SRC1_ALPHA,PIPE_BLENDFACTOR_INV_SRC1_ALPHA};
 static const unsigned ops[]={PIPE_BLEND_ADD,PIPE_BLEND_SUBTRACT,PIPE_BLEND_REVERSE_SUBTRACT,PIPE_BLEND_MIN,PIPE_BLEND_MAX};
 struct pipe_blend_state d={.rt[0]={.blend_enable=enabled,.rgb_src_factor=factors[MIN2(sr,18)],.rgb_dst_factor=factors[MIN2(dr,18)],.alpha_src_factor=factors[MIN2(sa,18)],.alpha_dst_factor=factors[MIN2(da,18)],.rgb_func=ops[MIN2(rop,4)],.alpha_func=ops[MIN2(aop,4)],.colormask=((mask&8)?PIPE_MASK_R:0)|((mask&4)?PIPE_MASK_G:0)|((mask&2)?PIPE_MASK_B:0)|((mask&1)?PIPE_MASK_A:0)}};
 void *state=c->pipe->create_blend_state(c->pipe,&d); c->pipe->bind_blend_state(c->pipe,state); c->pipe->delete_blend_state(c->pipe,c->blend); c->blend=state;
}
void hg_blend_color(struct hg_context *c,float r,float g,float b,float a) { struct pipe_blend_color color={.color={r,g,b,a}}; c->pipe->set_blend_color(c->pipe,&color); }

static uint64_t hg_now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
void hg_command_begin(struct hg_context *c) { c->started=hg_now();c->upload_us=c->requested=c->uploaded=c->hits=c->bindings=0; }
void hg_command_end(struct hg_context *c) {
 static uint64_t commands,elapsed,upload_us,requested,uploaded,hits,bindings,last;
 uint64_t now=hg_now();commands++;elapsed+=now-c->started;upload_us+=c->upload_us;
 requested+=c->requested;uploaded+=c->uploaded;hits+=c->hits;bindings+=c->bindings;
 for(unsigned i=0;i<c->upload_count;i++)pipe_resource_reference(&c->uploads[i].resource,NULL);
 c->upload_count=0;c->upload_bytes=0;
 if(now-last>5000000 && commands) {
  fprintf(stderr,"HakiosMetal perf: commands=%llu mean_us=%llu upload_us=%llu bindings=%llu reused=%llu requested_KB=%llu uploaded_KB=%llu\n",(unsigned long long)commands,(unsigned long long)(elapsed/commands),(unsigned long long)(upload_us/commands),(unsigned long long)bindings,(unsigned long long)hits,(unsigned long long)(requested/1024),(unsigned long long)(uploaded/1024));
  commands=elapsed=upload_us=requested=uploaded=hits=bindings=0;last=now;
 }
}
/* Upload a shared Metal buffer once per command, then bind its exact subranges.
 * Metal forbids CPU mutation while these commands execute; no cross-command cache. */
void hg_buffer_region(struct hg_context *c,unsigned stage,unsigned index,const void *bytes,size_t length,size_t offset) {
 if(index>=PIPE_MAX_CONSTANT_BUFFERS || offset>=length || !bytes || length>UINT32_MAX)return;
 /* Preserve tightly packed Metal constants when offsets are not UBO aligned. */
 if(offset%16) {bytes=(const char *)bytes+offset;length-=offset;offset=0;}
 uint64_t start=hg_now();c->requested+=length-offset;c->bindings++;
 struct pipe_resource *b=NULL;
 for(unsigned i=0;i<c->upload_count;i++)if(c->uploads[i].source==bytes && c->uploads[i].length==length) {b=c->uploads[i].resource;c->hits++;break;}
 bool owned=false;
 if(!b) {
  b=pipe_buffer_create(c->screen,PIPE_BIND_CONSTANT_BUFFER|PIPE_BIND_SHADER_BUFFER|PIPE_BIND_VERTEX_BUFFER,PIPE_USAGE_STREAM,length);
  if(!b)return;
  pipe_buffer_write(c->pipe,b,0,length,bytes);c->uploaded+=length;
  if(c->upload_count<64 && length<=64*1024*1024-c->upload_bytes) {
   unsigned i=c->upload_count++;c->uploads[i].source=bytes;c->uploads[i].length=length;c->uploads[i].resource=b;c->upload_bytes+=length;
  } else owned=true;
 }
 unsigned ps=stage?PIPE_SHADER_FRAGMENT:PIPE_SHADER_VERTEX;
 if(!stage && index<32) {pipe_resource_reference(&c->streams[index].buffer.resource,b);c->streams[index].buffer_offset=offset;}
 c->pipe->set_constant_buffer(c->pipe,ps,index,false,&(struct pipe_constant_buffer){.buffer=b,.buffer_offset=offset,.buffer_size=length-offset});
 if(index<PIPE_MAX_SHADER_BUFFERS)c->pipe->set_shader_buffers(c->pipe,ps,index,1,&(struct pipe_shader_buffer){.buffer=b,.buffer_offset=offset,.buffer_size=length-offset},0);
 if(owned)pipe_resource_reference(&b,NULL);
 c->upload_us+=hg_now()-start;
}
void hg_buffer(struct hg_context *c,unsigned stage,unsigned index,const void *bytes,size_t length) {hg_buffer_region(c,stage,index,bytes,length,0);}
void hg_sample(struct hg_context *c,unsigned stage,unsigned index,struct hg_texture *t) {
 if(index>=PIPE_MAX_SAMPLERS) return;
 unsigned ps=stage ? PIPE_SHADER_FRAGMENT : PIPE_SHADER_VERTEX;
 struct pipe_sampler_view *v=NULL;
 if(t) { struct pipe_sampler_view desc; u_sampler_view_default_template(&desc,t->res,t->res->format); v=c->pipe->create_sampler_view(c->pipe,t->res,&desc); }
 c->pipe->set_sampler_views(c->pipe,ps,index,1,0,false,&v);
 pipe_sampler_view_reference(&v,NULL);
}
void hg_viewport(struct hg_context *c,double x,double y,double w,double h,double zn,double zf) {
 c->pipe->set_viewport_states(c->pipe,0,1,&(struct pipe_viewport_state){.scale={w/2.,-h/2.,zf-zn},.translate={x+w/2.,y+h/2.,zn}});
}
/* Metal defaults are clockwise front faces, no culling, filled triangles,
 * and clipping in the 0..1 depth range. A render encoder owns this state. */
void hg_raster(struct hg_context *c,unsigned winding,unsigned cull,unsigned fill,unsigned clamp,float bias,float slope,float limit) {
 struct pipe_rasterizer_state r={.depth_clip_near=!clamp,.depth_clip_far=!clamp,
  .half_pixel_center=1,.bottom_edge_rule=1,.scissor=1,.clip_halfz=1,
  /* Chlorophyll air_metal.c:6963: Metal flat varyings use the FIRST vertex. */
  .flatshade_first=1,.line_width=1.0f,.point_size=1.0f,.point_size_per_vertex=1,
  .front_ccw=winding==1,.cull_face=cull==1?PIPE_FACE_FRONT:cull==2?PIPE_FACE_BACK:PIPE_FACE_NONE,
  .fill_front=fill?PIPE_POLYGON_MODE_LINE:PIPE_POLYGON_MODE_FILL,
  .fill_back=fill?PIPE_POLYGON_MODE_LINE:PIPE_POLYGON_MODE_FILL,
  .offset_tri=(bias!=0 || slope!=0),.offset_units=bias,.offset_scale=slope,.offset_clamp=limit};
 if(!memcmp(&r,&c->raster,sizeof(r)))return;
 void *state=c->pipe->create_rasterizer_state(c->pipe,&r);
 c->pipe->bind_rasterizer_state(c->pipe,state);
 c->pipe->delete_rasterizer_state(c->pipe,c->rast);c->rast=state;c->raster=r;
}
void hg_scissor(struct hg_context *c,unsigned x,unsigned y,unsigned w,unsigned h) {
 c->pipe->set_scissor_states(c->pipe,0,1,&(struct pipe_scissor_state){.minx=x,.miny=y,.maxx=x+w,.maxy=y+h});
}

/* VectorKit compacts its glyph atlas with overlapping self-copies.
 * Adapted from hakios-metal render.cpp: preserve memmove semantics via scratch. */
int hg_texture_copy(struct hg_texture *s, unsigned sl, unsigned ss, unsigned sx, unsigned sy,
 struct hg_texture *d, unsigned dl, unsigned ds, unsigned dx, unsigned dy, unsigned w, unsigned h)
{
 if (!s || !d || s->ctx != d->ctx || sl > s->res->last_level || dl > d->res->last_level ||
     s->res->format != d->res->format || s->res->nr_samples != d->res->nr_samples ||
     ss >= s->res->array_size || ds >= d->res->array_size) { fail("invalid texture copy"); return 0; }
 unsigned sw=u_minify(s->res->width0,sl), sh=u_minify(s->res->height0,sl);
 unsigned dw=u_minify(d->res->width0,dl), dh=u_minify(d->res->height0,dl);
 if (sx>sw || sy>sh || dx>dw || dy>dh || w>sw-sx || h>sh-sy || w>dw-dx || h>dh-dy) {
  fail("texture copy out of bounds"); return 0;
 }
 if (!w || !h || (s==d && sl==dl && ss==ds && sx==dx && sy==dy)) return 1;
 struct pipe_context *p=s->ctx->pipe;
 struct pipe_box box={.x=sx,.y=sy,.z=ss,.width=w,.height=h,.depth=1};
 if (s->res==d->res && sl==dl && ss==ds && sx<dx+w && dx<sx+w && sy<dy+h && dy<sy+h) {
  struct pipe_resource desc=*s->res;
  desc.target=PIPE_TEXTURE_2D; desc.width0=w; desc.height0=h;
  desc.depth0=1; desc.array_size=1; desc.last_level=0;
  struct pipe_resource *tmp=s->ctx->screen->resource_create(s->ctx->screen,&desc);
  if (!tmp) { fail("glyph copy scratch allocation failed"); return 0; }
  p->resource_copy_region(p,tmp,0,0,0,0,s->res,sl,&box);
  box.x=box.y=box.z=0;
  p->resource_copy_region(p,d->res,dl,dx,dy,ds,tmp,0,&box);
  /* Keep scratch alive through completion, even with a deferred driver. */
  hg_flush(s->ctx);
  pipe_resource_reference(&tmp,NULL);
 } else p->resource_copy_region(p,d->res,dl,dx,dy,ds,s->res,sl,&box);
 return 1;
}
