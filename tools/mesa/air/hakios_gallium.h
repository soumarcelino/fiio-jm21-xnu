/* Metal's rendering bridge to the same Gallium driver used by EAGL. */
#ifndef HAKIOS_GALLIUM_H
#define HAKIOS_GALLIUM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct hg_context;
struct hg_texture;
struct hg_shader;
struct hg_context *hg_open(void);
void hg_close(struct hg_context *);
const char *hg_error(void);
const char *hg_name(struct hg_context *);
struct hg_texture *hg_texture_new(struct hg_context *, unsigned width, unsigned height,
 unsigned metal_format, unsigned levels, unsigned samples, unsigned metal_type);
int hg_texture_write_slice(struct hg_texture *, unsigned level, unsigned slice, unsigned x, unsigned y,
 unsigned width, unsigned height, const void *, size_t stride);
int hg_texture_copy(struct hg_texture *src, unsigned sl, unsigned ss, unsigned sx, unsigned sy,
 struct hg_texture *dst, unsigned dl, unsigned ds, unsigned dx, unsigned dy, unsigned w, unsigned h);
void hg_texture_free(struct hg_texture *);
int hg_texture_write(struct hg_texture *, unsigned level, unsigned x, unsigned y,
 unsigned width, unsigned height, const void *, size_t stride);
int hg_texture_read(struct hg_texture *, unsigned level, unsigned x, unsigned y,
 unsigned width, unsigned height, void *, size_t stride);
struct hg_constant { unsigned index; uint64_t bits; };
struct hg_shader *hg_shader_new(struct hg_context *, const void *, size_t,
 const char *entry, unsigned stage, const struct hg_constant *, unsigned);
struct hg_vertex_attribute { unsigned format,offset,buffer,stride,step,rate; };
int hg_vertex_state(struct hg_shader *,const struct hg_vertex_attribute *,unsigned);
int hg_shader_link(struct hg_shader *,struct hg_shader *);
void hg_shader_free(struct hg_shader *);
int hg_target(struct hg_context *, struct hg_texture *, const double clear[4], int load, struct hg_texture *depth, double clear_depth, unsigned clear_stencil, unsigned clear_flags);
int hg_draw(struct hg_context *, struct hg_shader *vs, struct hg_shader *fs,
 unsigned primitive, unsigned start, unsigned count, unsigned instances);
int hg_draw_indexed(struct hg_context *,struct hg_shader *,struct hg_shader *,unsigned,unsigned,unsigned,const void *,unsigned,unsigned,int);
void *hg_sampler_new(struct hg_context *,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,float,float);
void hg_sampler_free(struct hg_context *,void *);
void hg_sampler_bind(struct hg_context *,unsigned,unsigned,void *);
int hg_resolve(struct hg_texture *,struct hg_texture *);
void hg_blend(struct hg_context *,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
struct hg_stencil { unsigned enabled,compare,fail,depth_fail,pass,read_mask,write_mask; };
void hg_depth_stencil(struct hg_context *,unsigned compare,unsigned write,const struct hg_stencil *front,const struct hg_stencil *back);
void hg_stencil_ref(struct hg_context *,unsigned,unsigned);
void hg_blend_color(struct hg_context *,float,float,float,float);
void hg_command_begin(struct hg_context *);
void hg_command_end(struct hg_context *);
void hg_buffer_region(struct hg_context *,unsigned,unsigned,const void *,size_t,size_t);
void hg_buffer(struct hg_context *, unsigned stage, unsigned index, const void *, size_t);
void hg_sample(struct hg_context *, unsigned stage, unsigned index, struct hg_texture *);
void hg_viewport(struct hg_context *, double x, double y, double w, double h, double nearz, double farz);
void hg_raster(struct hg_context *,unsigned winding,unsigned cull,unsigned fill,unsigned clamp,float bias,float slope,float limit);
void hg_scissor(struct hg_context *, unsigned x, unsigned y, unsigned w, unsigned h);
void hg_flush(struct hg_context *);
#ifdef __cplusplus
}
#endif
#endif
