/* SPDX-License-Identifier: MIT
 * Experimental rendering objects using the JM21's existing Gallium driver.
 * Included after the basic Metal library/buffer implementations.
 */
#include "../../tools/mesa/air/hakios_gallium.h"
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurfaceRef.h>
static Class hTextureClass, hPipelineClass, hCommandClass, hEncoderClass, hBlitClass, hSamplerClass, hDepthClass;
static struct hg_context *hContext;
static struct hg_context *hGPU(void) {
 static dispatch_once_t once;
 dispatch_once(&once, ^{ hContext=hg_open(); say(hContext ? [NSString stringWithFormat:@"Gallium renderer: %s",hg_name(hContext)] : [NSString stringWithFormat:@"Gallium unavailable: %s",hg_error()]); });
 return hContext;
}
static void *hState(id obj) { void **s=stateOf(obj); return s ? *s : NULL; }
static id hNew(Class cls, void *state) { id obj=[[cls alloc] init]; if(!obj) { free(state); return nil; } *stateOf(obj)=state; return obj; }
struct htexture { struct hg_texture *tex; id descriptor; IOSurfaceRef surface; NSUInteger plane; id label; };
struct hpipeline { struct hg_shader *vs,*fs; id descriptor; id label; };
struct hcommand { NSMutableArray *commands,*completed,*scheduled,*outputs; id queue; NSError *error; MTLCommandBufferStatus status; id label; };
struct hencoder { id command; id pipeline; BOOL ended; id label; id target,resolve; unsigned winding,cull,fill,clamp; float bias,slope,biasClamp; };
static id *hLabelSlot(id s) {
 void *p=hState(s); if(!p) return NULL;
 Class c=object_getClass(s);
 if(c==hTextureClass) return &((struct htexture *)p)->label;
 if(c==hPipelineClass) return &((struct hpipeline *)p)->label;
 if(c==hCommandClass) return &((struct hcommand *)p)->label;
 if(c==hEncoderClass || c==hBlitClass) return &((struct hencoder *)p)->label;
 return NULL;
}
static id hLabelGet(id s,SEL cmd) { id *p=hLabelSlot(s); return p?*p:nil; }
static void hLabelSet(id s,SEL cmd,id label) { id *p=hLabelSlot(s); if(p) { id n=[label copy]; [*p release]; *p=n; } }
static void hTextureFree(void *ptr) { struct htexture *t=ptr; hg_texture_free(t->tex); [t->descriptor release]; [t->label release]; if(t->surface) CFRelease(t->surface); free(t); }
static void hTextureDealloc(id s,SEL cmd) { deallocWith(s,cmd,hTextureClass,hTextureFree); }
static NSUInteger hTextureProp(id s,SEL cmd) { struct htexture *t=hState(s); return ((NSUInteger(*)(id,SEL))objc_msgSend)(t->descriptor,cmd); }
static id hNewTexture(id s,SEL cmd,MTLTextureDescriptor *d) {
 if(!hGPU() || (d.textureType!=MTLTextureType2D && d.textureType!=MTLTextureType2DArray && d.textureType!=MTLTextureTypeCube && d.textureType!=MTLTextureType2DMultisample) || d.arrayLength!=1) { say([NSString stringWithFormat:@"unsupported texture type=%lu array=%lu size=%lux%lu samples=%lu format=%lu",(unsigned long)d.textureType,(unsigned long)d.arrayLength,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.sampleCount,(unsigned long)d.pixelFormat]); return nil; }
 struct hg_texture *gpu=hg_texture_new(hContext,d.width,d.height,d.pixelFormat,d.mipmapLevelCount,d.sampleCount,d.textureType);
 if(!gpu) { say([NSString stringWithFormat:@"texture creation failed: %s type=%lu array=%lu size=%lux%lu levels=%lu samples=%lu format=%lu",hg_error(),(unsigned long)d.textureType,(unsigned long)d.arrayLength,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.mipmapLevelCount,(unsigned long)d.sampleCount,(unsigned long)d.pixelFormat]); return nil; }
 struct htexture *t=calloc(1,sizeof(*t)); t->tex=gpu; t->descriptor=[d copy]; return hNew(hTextureClass,t);
}
static IOSurfaceRef hTextureSurface(id s,SEL cmd) { return ((struct htexture *)hState(s))->surface; }
static void hRecord(id cb,void (^block)(void));
struct hsampler { void *gpu; };
static void hSamplerFree(void *ptr) { struct hsampler *p=ptr; hg_sampler_free(hContext,p->gpu); free(p); }
static void hSamplerDealloc(id s,SEL cmd) { deallocWith(s,cmd,hSamplerClass,hSamplerFree); }
static id hNewSampler(id s,SEL cmd,MTLSamplerDescriptor *d) {
 if(!hGPU()) return nil;
 struct hsampler *p=calloc(1,sizeof(*p));
 p->gpu=hg_sampler_new(hContext,d.minFilter,d.magFilter,d.mipFilter,d.sAddressMode,d.tAddressMode,d.rAddressMode,d.lodMinClamp,d.lodMaxClamp);
 if(!p->gpu) { free(p); return nil; } return hNew(hSamplerClass,p);
}
static void hEncoderSampler(id s,SEL cmd,id sampler,NSUInteger index) {
 struct hencoder *e=hState(s); BOOL fragment=strstr(sel_getName(cmd),"Fragment")!=NULL;
 hRecord(e->command,^{ struct hsampler *p=object_getClass(sampler)==hSamplerClass?hState(sampler):NULL; hg_sampler_bind(hContext,fragment,index,p?p->gpu:NULL); });
}
static id hNewSurfaceTexture(id s,SEL cmd,MTLTextureDescriptor *d,IOSurfaceRef surface,NSUInteger plane) {
 if(!surface || plane) return nil;
 id obj=hNewTexture(s,cmd,d); struct htexture *t=hState(obj);
 if(t) { t->surface=(IOSurfaceRef)CFRetain(surface); t->plane=plane; }
 return obj;
}
static void hTextureReplace(id s,SEL cmd,MTLRegion r,NSUInteger level,const void *bytes,NSUInteger stride) {
 struct htexture *t=hState(s);
 if(r.origin.z || r.size.depth!=1 || hg_texture_write(t->tex,level,r.origin.x,r.origin.y,r.size.width,r.size.height,bytes,stride)) say([NSString stringWithUTF8String:hg_error()]);
}
static void hTextureReplaceSlice(id s,SEL cmd,MTLRegion r,NSUInteger level,NSUInteger slice,const void *bytes,NSUInteger stride,NSUInteger imageStride) {
 struct htexture *t=hState(s);
 if(r.origin.z || r.size.depth!=1 || hg_texture_write_slice(t->tex,level,slice,r.origin.x,r.origin.y,r.size.width,r.size.height,bytes,stride)) say([NSString stringWithUTF8String:hg_error()]);
}
static void hTextureGet(id s,SEL cmd,void *bytes,NSUInteger stride,MTLRegion r,NSUInteger level) {
 struct htexture *t=hState(s);
 if(r.origin.z || r.size.depth!=1 || hg_texture_read(t->tex,level,r.origin.x,r.origin.y,r.size.width,r.size.height,bytes,stride)) say([NSString stringWithUTF8String:hg_error()]);
}
static id hPipelineError(NSError **error) {
 NSString *msg=[NSString stringWithUTF8String:hg_error()]; say(msg);
 if(error) *error=[NSError errorWithDomain:@"HakiosMetal" code:1 userInfo:@{NSLocalizedDescriptionKey:msg}]; return nil;
}
static void hPipelineFree(void *ptr) { struct hpipeline *p=ptr; hg_shader_free(p->vs); hg_shader_free(p->fs); [p->descriptor release]; [p->label release]; free(p); }
static void hPipelineDealloc(id s,SEL cmd) { deallocWith(s,cmd,hPipelineClass,hPipelineFree); }
static struct hg_shader *hCompileFunction(id function,NSData *code,unsigned stage) {
 struct function *state=object_getClass(function)==gFunctionClass?hState(function):NULL;
 NSDictionary *values=state?state->constants:nil;
 static unsigned saved;
 NSArray *names=@[@"Grid::grid_vertex",@"PolygonFill::polygonFill_vertex"];
 for(unsigned i=0;i<names.count;i++)if(state && [state->name isEqual:names[i]] && !(saved&(1u<<i))) {
  saved|=1u<<i;
  NSString *base=[NSString stringWithFormat:@"/private/var/tmp/jm21-shader-%u",i];
  [code writeToFile:[base stringByAppendingString:@".bc"] atomically:NO];
  [[values description] writeToFile:[base stringByAppendingString:@".constants"] atomically:NO encoding:NSUTF8StringEncoding error:NULL];
 }

 struct hg_constant *constants=calloc(values.count,sizeof(*constants)); unsigned n=0;
 for(NSNumber *index in values)constants[n++]=(struct hg_constant){index.unsignedIntValue,[values[index] unsignedLongLongValue]};
 struct hg_shader *shader=hg_shader_new(hContext,code.bytes,code.length,NULL,stage,constants,n);
 free(constants);return shader;
}
static id hNewPipeline(id s,SEL cmd,MTLRenderPipelineDescriptor *desc,NSError **error) {
 if(!hGPU()) return hPipelineError(error);
 NSData *v=functionBitcode(desc.vertexFunction,NULL), *f=functionBitcode(desc.fragmentFunction,NULL);
 if(!v || !f) { if(error) *error=[NSError errorWithDomain:@"HakiosMetal" code:2 userInfo:@{NSLocalizedDescriptionKey:@"missing AIR function"}]; return nil; }
 static unsigned described;
 if(described++<32) {
  unsigned attributes=0;
  for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *a=desc.vertexDescriptor.attributes[i];
   if(a.format) { attributes++; say([NSString stringWithFormat:@"vertex attribute %u format=%lu offset=%lu buffer=%lu",i,(unsigned long)a.format,(unsigned long)a.offset,(unsigned long)a.bufferIndex]); }
  }
  say([NSString stringWithFormat:@"pipeline %@ attributes=%u",[desc.vertexFunction name],attributes]);
 }
 struct hpipeline *p=calloc(1,sizeof(*p));
 p->vs=hCompileFunction(desc.vertexFunction,v,0);
 if(p->vs) p->fs=hCompileFunction(desc.fragmentFunction,f,1);
 struct hg_vertex_attribute attrs[31]={0};
 for(unsigned i=0;i<31;i++) {
  MTLVertexAttributeDescriptor *a=desc.vertexDescriptor.attributes[i];
  if(a.format && a.bufferIndex<31) {
   MTLVertexBufferLayoutDescriptor *l=desc.vertexDescriptor.layouts[a.bufferIndex];
   attrs[i]=(struct hg_vertex_attribute){a.format,a.offset,a.bufferIndex,l.stride,l.stepFunction,l.stepRate};
  }
 }
 if(!p->vs || !p->fs || hg_vertex_state(p->vs,attrs,31) || hg_shader_link(p->vs,p->fs)) { hPipelineFree(p); return hPipelineError(error); }
 p->descriptor=[desc copy]; return hNew(hPipelineClass,p);
}
static id hNewPipelineOptions(id s,SEL cmd,id desc,NSUInteger options,id *reflection,NSError **error) {
 if(reflection) *reflection=nil; return hNewPipeline(s,cmd,desc,error);
}
static void hCommandFree(void *ptr) { struct hcommand *c=ptr; [c->outputs release]; [c->commands release]; [c->completed release]; [c->scheduled release]; [c->queue release]; [c->error release]; [c->label release]; free(c); }
static void hCommandDealloc(id s,SEL cmd) { deallocWith(s,cmd,hCommandClass,hCommandFree); }
static id hCommandNew(id s,SEL cmd) {
 if(!hGPU()) return nil;
 struct hcommand *c=calloc(1,sizeof(*c)); c->outputs=[NSMutableArray new]; c->commands=[NSMutableArray new]; c->completed=[NSMutableArray new]; c->scheduled=[NSMutableArray new]; c->queue=[s retain]; c->status=MTLCommandBufferStatusNotEnqueued;
 return [hNew(hCommandClass,c) autorelease];
}
static void hRecord(id cb,void (^block)(void)) { struct hcommand *c=hState(cb); if(c->status<MTLCommandBufferStatusCommitted) { id b=[block copy]; [c->commands addObject:b]; [b release]; } }
static void hSyncSurface(id texture);
static void hCommandCommit(id s,SEL cmd) {
 static unsigned commits; if(commits++<8) say(@"command commit");
 struct hcommand *c=hState(s); if(c->status>=MTLCommandBufferStatusCommitted) return;
 c->status=MTLCommandBufferStatusCommitted;
 @synchronized(gDevice) {
 c->status=MTLCommandBufferStatusScheduled;
 for(void (^handler)(id) in c->scheduled) handler(s);
 @try {
  hg_command_begin(hContext); CFTimeInterval t0=CACurrentMediaTime();
  for(void (^work)(void) in c->commands) work();
  CFTimeInterval t1=CACurrentMediaTime();
  for(id texture in c->outputs)hSyncSurface(texture);
  CFTimeInterval t2=CACurrentMediaTime();hg_flush(hContext);
  CFTimeInterval t3=CACurrentMediaTime();
  static unsigned samples;static double workTime,syncTime,flushTime,last;
  samples++;workTime+=t1-t0;syncTime+=t2-t1;flushTime+=t3-t2;
  if(t3-last>5) {
   say([NSString stringWithFormat:@"phases n=%u work_us=%.0f surface_us=%.0f flush_us=%.0f",samples,workTime*1e6/samples,syncTime*1e6/samples,flushTime*1e6/samples]);
   samples=0;workTime=syncTime=flushTime=0;last=t3;
  }
  c->status=MTLCommandBufferStatusCompleted;
 }
 @catch(NSException *e) { say([NSString stringWithFormat:@"command failed: %@",e.reason]); c->error=[[NSError errorWithDomain:@"HakiosMetal" code:3 userInfo:@{NSLocalizedDescriptionKey:e.reason?:@"encoding error"}] retain]; c->status=MTLCommandBufferStatusError; }
 hg_command_end(hContext);
 }
 for(void (^handler)(id) in c->completed) handler(s);
 [c->outputs removeAllObjects]; [c->commands removeAllObjects]; [c->completed removeAllObjects]; [c->scheduled removeAllObjects];
}
static void hCommandEnqueue(id s,SEL cmd) { struct hcommand *c=hState(s); if(c->status==0)c->status=MTLCommandBufferStatusEnqueued; }
static void hCommandWait(id s,SEL cmd) { /* commit executes synchronously */ }
static NSUInteger hCommandStatus(id s,SEL cmd) { return ((struct hcommand *)hState(s))->status; }
static id hCommandError(id s,SEL cmd) { return ((struct hcommand *)hState(s))->error; }
static id hCommandQueue(id s,SEL cmd) { return ((struct hcommand *)hState(s))->queue; }
static void hCommandCompleted(id s,SEL cmd,id handler) { struct hcommand *c=hState(s); id b=[handler copy]; [c->completed addObject:b]; [b release]; }
static void hCommandScheduled(id s,SEL cmd,id handler) { struct hcommand *c=hState(s); id b=[handler copy]; [c->scheduled addObject:b]; [b release]; }
static void hEncoderFree(void *ptr) { struct hencoder *e=ptr; [e->command release]; [e->pipeline release]; [e->label release]; [e->target release]; [e->resolve release]; free(e); }
static void hEncoderDealloc(id s,SEL cmd) { deallocWith(s,cmd,hEncoderClass,hEncoderFree); }
static void hBlitDealloc(id s,SEL cmd) { deallocWith(s,cmd,hBlitClass,hEncoderFree); }
static id hBlitNew(id s,SEL cmd) {
 if(((struct hcommand *)hState(s))->status>=MTLCommandBufferStatusCommitted) return nil;
 struct hencoder *e=calloc(1,sizeof(*e)); if(!e) return nil;
 e->command=[s retain]; return [hNew(hBlitClass,e) autorelease];
}
static void hBlitCopy(id s,SEL cmd,id source,NSUInteger ss,NSUInteger sl,MTLOrigin so,MTLSize size,
 id dest,NSUInteger ds,NSUInteger dl,MTLOrigin dorigin) {
 struct hencoder *e=hState(s);
 if(e->ended) [NSException raise:@"HakiosMetal" format:@"blit encoder already ended"];
 hRecord(e->command,^{
  if(object_getClass(source)!=hTextureClass || object_getClass(dest)!=hTextureClass ||
     size.depth!=1 || so.z || dorigin.z || ss>UINT32_MAX || sl>UINT32_MAX || ds>UINT32_MAX || dl>UINT32_MAX ||
     so.x>UINT32_MAX || so.y>UINT32_MAX || dorigin.x>UINT32_MAX || dorigin.y>UINT32_MAX ||
     size.width>UINT32_MAX || size.height>UINT32_MAX)
   [NSException raise:@"HakiosMetal" format:@"unsupported texture copy region"];
  struct htexture *a=hState(source), *b=hState(dest);
  if(!hg_texture_copy(a->tex,sl,ss,so.x,so.y,b->tex,dl,ds,dorigin.x,dorigin.y,size.width,size.height))
   [NSException raise:@"HakiosMetal" format:@"texture copy: %s",hg_error()];
 });
}
static void hBlitBufferTexture(id s,SEL cmd,id source,NSUInteger offset,NSUInteger stride,NSUInteger imageStride,MTLSize size,id dest,NSUInteger slice,NSUInteger level,MTLOrigin origin) {
 struct hencoder *e=hState(s);
 if(e->ended) [NSException raise:@"HakiosMetal" format:@"blit encoder already ended"];
 hRecord(e->command,^{
  struct buffer *b=bufferOf(source);
  struct htexture *t=object_getClass(dest)==hTextureClass ? hState(dest) : NULL;
  if(!b || !t || size.depth!=1 || origin.z || !stride || offset>b->length ||
     size.height>(b->length-offset)/stride || slice>UINT32_MAX || level>UINT32_MAX ||
     origin.x>UINT32_MAX || origin.y>UINT32_MAX || size.width>UINT32_MAX || size.height>UINT32_MAX)
   [NSException raise:@"HakiosMetal" format:@"invalid buffer to texture copy"];
  if(hg_texture_write_slice(t->tex,level,slice,origin.x,origin.y,size.width,size.height,(char *)b->memory+offset,stride))
   [NSException raise:@"HakiosMetal" format:@"buffer to texture copy: %s",hg_error()];
 });
}
struct hdepth { id descriptor; };
static void hDepthFree(void *p) { [((struct hdepth *)p)->descriptor release];free(p); }
static void hDepthDealloc(id s,SEL cmd) { deallocWith(s,cmd,hDepthClass,hDepthFree); }
static id hNewDepth(id s,SEL cmd,MTLDepthStencilDescriptor *d) { struct hdepth *p=calloc(1,sizeof(*p));p->descriptor=[d copy];return hNew(hDepthClass,p); }
static struct hg_stencil hStencil(MTLStencilDescriptor *d) {
 return (struct hg_stencil){.enabled=d!=nil,.compare=d.stencilCompareFunction,.fail=d.stencilFailureOperation,.depth_fail=d.depthFailureOperation,.pass=d.depthStencilPassOperation,.read_mask=d.readMask,.write_mask=d.writeMask};
}
static void hEncoderDepth(id s,SEL cmd,id state) {
 struct hencoder *e=hState(s);
 MTLDepthStencilDescriptor *d=object_getClass(state)==hDepthClass?((struct hdepth *)hState(state))->descriptor:nil;
 unsigned compare=d?d.depthCompareFunction:MTLCompareFunctionAlways,write=d.depthWriteEnabled;
 struct hg_stencil front=hStencil(d.frontFaceStencil),back=hStencil(d.backFaceStencil);
 hRecord(e->command,^{ hg_depth_stencil(hContext,compare,write,&front,&back); });
}
static void hEncoderStencil(id s,SEL cmd,uint32_t value) { struct hencoder *e=hState(s);hRecord(e->command,^{hg_stencil_ref(hContext,value,value);}); }
static void hEncoderStencilFaces(id s,SEL cmd,uint32_t front,uint32_t back) { struct hencoder *e=hState(s);hRecord(e->command,^{hg_stencil_ref(hContext,front,back);}); }
static void hEncoderRaster(struct hencoder *e) {
 unsigned winding=e->winding,cull=e->cull,fill=e->fill,clamp=e->clamp;
 float bias=e->bias,slope=e->slope,limit=e->biasClamp;
 hRecord(e->command,^{hg_raster(hContext,winding,cull,fill,clamp,bias,slope,limit);});
}
static void hEncoderWinding(id s,SEL cmd,NSUInteger v) {struct hencoder *e=hState(s);e->winding=v;hEncoderRaster(e);}
static void hEncoderCull(id s,SEL cmd,NSUInteger v) {struct hencoder *e=hState(s);e->cull=v;hEncoderRaster(e);}
static void hEncoderFill(id s,SEL cmd,NSUInteger v) {struct hencoder *e=hState(s);e->fill=v;hEncoderRaster(e);}
static void hEncoderClip(id s,SEL cmd,NSUInteger v) {struct hencoder *e=hState(s);e->clamp=v;hEncoderRaster(e);}
static void hEncoderBias(id s,SEL cmd,float bias,float slope,float clamp) {struct hencoder *e=hState(s);e->bias=bias;e->slope=slope;e->biasClamp=clamp;hEncoderRaster(e);}
static id hEncoderNew(id s,SEL cmd,MTLRenderPassDescriptor *desc) {
 static unsigned passes; if(passes++<8) say([NSString stringWithFormat:@"render pass %@ load=%lu store=%lu",desc.colorAttachments[0].texture,(unsigned long)desc.colorAttachments[0].loadAction,(unsigned long)desc.colorAttachments[0].storeAction]);
 struct hcommand *c=hState(s); if(c->status>=MTLCommandBufferStatusCommitted) return nil;
 id target=desc.colorAttachments[0].texture;
 if(object_getClass(target)!=hTextureClass) { say(@"render pass requires a supported color texture"); return nil; }
 MTLClearColor color=desc.colorAttachments[0].clearColor; BOOL load=desc.colorAttachments[0].loadAction==MTLLoadActionLoad;
 id depth=desc.depthAttachment.texture?:desc.stencilAttachment.texture;
 double clearDepth=desc.depthAttachment.clearDepth; unsigned clearStencil=desc.stencilAttachment.clearStencil;
 unsigned flags=(desc.depthAttachment.texture && desc.depthAttachment.loadAction==MTLLoadActionClear?1:0)|(desc.stencilAttachment.texture && desc.stencilAttachment.loadAction==MTLLoadActionClear?2:0);
 hRecord(s,^{ struct htexture *t=hState(target),*z=object_getClass(depth)==hTextureClass?hState(depth):NULL; double clear[]={color.red,color.green,color.blue,color.alpha};
 if(hg_target(hContext,t->tex,clear,load,z?z->tex:NULL,clearDepth,clearStencil,flags)) [NSException raise:@"HakiosMetal" format:@"%s",hg_error()];
 hg_depth_stencil(hContext,MTLCompareFunctionAlways,0,NULL,NULL);hg_stencil_ref(hContext,0,0);hg_raster(hContext,0,0,0,0,0,0,0);
 });
 struct hencoder *e=calloc(1,sizeof(*e)); e->command=[s retain];
 e->target=[target retain];
 if(desc.colorAttachments[0].storeAction==MTLStoreActionMultisampleResolve || desc.colorAttachments[0].storeAction==MTLStoreActionStoreAndMultisampleResolve) e->resolve=[desc.colorAttachments[0].resolveTexture retain];
 return [hNew(hEncoderClass,e) autorelease];
}
static void hEncoderPipeline(id s,SEL cmd,id pipeline) {
 struct hencoder *e=hState(s); [pipeline retain]; [e->pipeline release]; e->pipeline=pipeline;
 hRecord(e->command,^{ struct hpipeline *p=hState(pipeline); MTLRenderPipelineDescriptor *d=p->descriptor; MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[0];
 hg_blend(hContext,a.blendingEnabled,a.sourceRGBBlendFactor,a.destinationRGBBlendFactor,a.sourceAlphaBlendFactor,a.destinationAlphaBlendFactor,a.rgbBlendOperation,a.alphaBlendOperation,a.writeMask); });
}
static void hEncoderDrawInstances(id s,SEL cmd,NSUInteger prim,NSUInteger start,NSUInteger count,NSUInteger instances) {
 struct hencoder *e=hState(s); if(e->ended) return;
 id pipeline=e->pipeline;
 hRecord(e->command,^{ struct hpipeline *p=hState(pipeline); if(!p || hg_draw(hContext,p->vs,p->fs,prim,start,count,instances)) [NSException raise:@"HakiosMetal" format:@"draw: %s",hg_error()]; });
}
static void hEncoderDraw(id s,SEL cmd,NSUInteger prim,NSUInteger start,NSUInteger count) { hEncoderDrawInstances(s,cmd,prim,start,count,1); }
static void hEncoderBuffer(id s,SEL cmd,id buffer,NSUInteger offset,NSUInteger index) {
 struct hencoder *e=hState(s); struct buffer *b=bufferOf(buffer);
 if(!b || offset>b->length) return;
 BOOL fragment=strstr(sel_getName(cmd),"Fragment")!=NULL;
 hRecord(e->command,^{ struct buffer *data=bufferOf(buffer); hg_buffer_region(hContext,fragment,index,data->memory,data->length,offset); });
}
static void hEncoderBuffers(id s,SEL cmd,const id *buffers,const NSUInteger *offsets,NSRange range) {
 BOOL fragment=strstr(sel_getName(cmd),"Fragment")!=NULL;
 SEL one=sel_registerName(fragment?"setFragmentBuffer:offset:atIndex:":"setVertexBuffer:offset:atIndex:");
 for(NSUInteger i=0;i<range.length;i++) hEncoderBuffer(s,one,buffers[i],offsets[i],range.location+i);
}
static void hEncoderIndexedFull(id s,SEL cmd,NSUInteger prim,NSUInteger count,NSUInteger type,id buffer,NSUInteger offset,NSUInteger instances,NSInteger basevertex,NSUInteger baseinstance) {
 struct hencoder *e=hState(s); if(e->ended)return;
 id pipeline=e->pipeline;
 hRecord(e->command,^{ struct hpipeline *p=hState(pipeline); struct buffer *b=bufferOf(buffer); unsigned size=type==0?2:4;
 if(!p || !b || type>1 || offset>b->length || count>(b->length-offset)/size || hg_draw_indexed(hContext,p->vs,p->fs,prim,count,size,(char *)b->memory+offset,instances,baseinstance,basevertex))
  [NSException raise:@"HakiosMetal" format:@"indexed draw: %s",hg_error()];
 });
}
static void hEncoderIndexedInstances(id s,SEL cmd,NSUInteger prim,NSUInteger count,NSUInteger type,id buffer,NSUInteger offset,NSUInteger instances) { hEncoderIndexedFull(s,cmd,prim,count,type,buffer,offset,instances,0,0); }
static void hEncoderIndexed(id s,SEL cmd,NSUInteger prim,NSUInteger count,NSUInteger type,id buffer,NSUInteger offset) { hEncoderIndexedFull(s,cmd,prim,count,type,buffer,offset,1,0,0); }
static void hEncoderBytes(id s,SEL cmd,const void *bytes,NSUInteger length,NSUInteger index) {
 struct hencoder *e=hState(s); NSData *copy=[NSData dataWithBytes:bytes length:length];
 BOOL fragment=strstr(sel_getName(cmd),"Fragment")!=NULL;
 hRecord(e->command,^{ hg_buffer(hContext,fragment,index,copy.bytes,copy.length); });
}
static void hEncoderTexture(id s,SEL cmd,id texture,NSUInteger index) {
 struct hencoder *e=hState(s); BOOL fragment=strstr(sel_getName(cmd),"Fragment")!=NULL;
 hRecord(e->command,^{ struct htexture *t=hState(texture); hg_sample(hContext,fragment,index,t ? t->tex : NULL); });
}
static void hEncoderViewport(id s,SEL cmd,MTLViewport v) { struct hencoder *e=hState(s); hRecord(e->command,^{ hg_viewport(hContext,v.originX,v.originY,v.width,v.height,v.znear,v.zfar); }); }
static void hEncoderScissor(id s,SEL cmd,MTLScissorRect r) { struct hencoder *e=hState(s); hRecord(e->command,^{ hg_scissor(hContext,r.x,r.y,r.width,r.height); }); }
static void hSyncSurface(id texture) {
 struct htexture *t=object_getClass(texture)==hTextureClass?hState(texture):NULL;
 if(!t || !t->surface) return;
 IOSurfaceLock(t->surface,0,NULL);
 int rc=hg_texture_read(t->tex,0,0,0,IOSurfaceGetWidth(t->surface),IOSurfaceGetHeight(t->surface),IOSurfaceGetBaseAddress(t->surface),IOSurfaceGetBytesPerRow(t->surface));
 IOSurfaceUnlock(t->surface,0,NULL);
 if(rc) [NSException raise:@"HakiosMetal" format:@"surface writeback: %s",hg_error()];
 static unsigned writes; if(writes++<8) say(@"surface writeback");
}
static void hEncoderEnd(id s,SEL cmd) {
 struct hencoder *e=hState(s); if(e->ended)return;
 if(e->resolve) { id source=e->target,dest=e->resolve; hRecord(e->command,^{
  struct htexture *a=hState(source),*b=hState(dest);
  if(!a||!b||hg_resolve(a->tex,b->tex)) [NSException raise:@"HakiosMetal" format:@"resolve: %s",hg_error()];
 }); }
 // VectorKit can present its drawable directly, bypassing presentDrawable:.
 // Complete the IOSurface backing before command completion is reported.
 id output=e->resolve?:e->target;
 if(output) {
  struct hcommand *command=hState(e->command);
  hRecord(e->command,^{ if(![command->outputs containsObject:output])[command->outputs addObject:output]; });
 }
 e->ended=YES;
}
static void hEncoderBlendColor(id s,SEL cmd,float r,float g,float b,float a) { struct hencoder *e=hState(s); hRecord(e->command,^{hg_blend_color(hContext,r,g,b,a);}); }
static void hPresent(id s,SEL cmd,id<CAMetalDrawable> drawable) {
 static unsigned presents; if(presents++<8) say(@"present drawable");
 id texture=drawable.texture;
 struct hcommand *command=hState(s);
 hRecord(s,^{ hSyncSurface(texture); [command->outputs removeObjectIdenticalTo:texture]; [drawable present]; });
}
static void hMethod(Class c,const char *name,IMP imp,const char *types) {
 class_replaceMethod(c,sel_registerName(name),imp,types);
 class_replaceMethod(c,sel_registerName("label"),(IMP)hLabelGet,"@@:");
 class_replaceMethod(c,sel_registerName("setLabel:"),(IMP)hLabelSet,"v@:@");
}
static void hQueueDealloc(id s,SEL cmd) {
 // NSObject init never creates Apple's private dispatch queues. Its Metal
 // destructor cannot run on a queue whose commands belong to Gallium.
 struct objc_super sup={s,objc_getClass("NSObject")};
 ((void(*)(struct objc_super *,SEL))objc_msgSendSuper)(&sup,cmd);
}
static void registerGalliumObjects(Class device) {
 class_replaceMethod(gQueueClass,sel_registerName("dealloc"),(IMP)hQueueDealloc,"v@:");
 static const char *const tp[]={"MTLTexture","MTLResource",NULL}, *const pp[]={"MTLRenderPipelineState",NULL}, *const cp[]={"MTLCommandBuffer",NULL}, *const ep[]={"MTLRenderCommandEncoder","MTLCommandEncoder",NULL};
 hTextureClass=objectClass("HakiosGalliumTexture","_MTLResource",tp,(IMP)hTextureDealloc);
 for(const char **n=(const char *[]){"width","height","depth","arrayLength","mipmapLevelCount","sampleCount","pixelFormat","textureType","usage","storageMode","cpuCacheMode","resourceOptions",NULL};*n;n++) hMethod(hTextureClass,*n,(IMP)hTextureProp,"Q@:");
 hMethod(hTextureClass,"replaceRegion:mipmapLevel:withBytes:bytesPerRow:",(IMP)hTextureReplace,"v@:{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}Q^vQ");
 hMethod(hTextureClass,"replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:",(IMP)hTextureReplaceSlice,"v@:{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}QQ^vQQ");
 hMethod(hTextureClass,"getBytes:bytesPerRow:fromRegion:mipmapLevel:",(IMP)hTextureGet,"v@:^vQ{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}Q");
 hMethod(hTextureClass,"iosurface",(IMP)hTextureSurface,"^{__IOSurface=}@:");
 objc_registerClassPair(hTextureClass);
 hPipelineClass=objectClass("HakiosGalliumPipeline","_MTLRenderPipelineState",pp,(IMP)hPipelineDealloc); objc_registerClassPair(hPipelineClass);
 hCommandClass=objectClass("HakiosGalliumCommandBuffer","_MTLCommandBuffer",cp,(IMP)hCommandDealloc);
 hMethod(hCommandClass,"commit",(IMP)hCommandCommit,"v@:"); hMethod(hCommandClass,"enqueue",(IMP)hCommandEnqueue,"v@:");
 hMethod(hCommandClass,"waitUntilCompleted",(IMP)hCommandWait,"v@:"); hMethod(hCommandClass,"waitUntilScheduled",(IMP)hCommandWait,"v@:");
 hMethod(hCommandClass,"status",(IMP)hCommandStatus,"Q@:"); hMethod(hCommandClass,"error",(IMP)hCommandError,"@@:"); hMethod(hCommandClass,"commandQueue",(IMP)hCommandQueue,"@@:");
 hMethod(hCommandClass,"addCompletedHandler:",(IMP)hCommandCompleted,"v@:@?"); hMethod(hCommandClass,"addScheduledHandler:",(IMP)hCommandScheduled,"v@:@?");
 hMethod(hCommandClass,"renderCommandEncoderWithDescriptor:",(IMP)hEncoderNew,"@@:@"); hMethod(hCommandClass,"presentDrawable:",(IMP)hPresent,"v@:@");
 hMethod(hCommandClass,"blitCommandEncoder",(IMP)hBlitNew,"@@:");
 objc_registerClassPair(hCommandClass);
 static const char *const bp[]={"MTLBlitCommandEncoder","MTLCommandEncoder",NULL};
 hBlitClass=objectClass("HakiosGalliumBlitEncoder","_MTLCommandEncoder",bp,(IMP)hBlitDealloc);
 hMethod(hBlitClass,"copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",(IMP)hBlitCopy,"v@:@QQ{MTLOrigin=QQQ}{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}");
 hMethod(hBlitClass,"copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",(IMP)hBlitBufferTexture,"v@:@QQQ{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}");
 hMethod(hBlitClass,"endEncoding",(IMP)hEncoderEnd,"v@:");
 objc_registerClassPair(hBlitClass);
 static const char *const sp[]={"MTLSamplerState",NULL};
 static const char *const dp[]={"MTLDepthStencilState",NULL};
 hDepthClass=objectClass("HakiosGalliumDepthStencil","_MTLDepthStencilState",dp,(IMP)hDepthDealloc);objc_registerClassPair(hDepthClass);
 hMethod(device,"newDepthStencilStateWithDescriptor:",(IMP)hNewDepth,"@@:@");
 hSamplerClass=objectClass("HakiosGalliumSampler","_MTLSamplerState",sp,(IMP)hSamplerDealloc); objc_registerClassPair(hSamplerClass);
 hMethod(device,"newSamplerStateWithDescriptor:",(IMP)hNewSampler,"@@:@");
 hEncoderClass=objectClass("HakiosGalliumRenderEncoder","_MTLCommandEncoder",ep,(IMP)hEncoderDealloc);
 hMethod(hEncoderClass,"setVertexBuffers:offsets:withRange:",(IMP)hEncoderBuffers,"v@:^@^Q{_NSRange=QQ}");
 hMethod(hEncoderClass,"setFragmentBuffers:offsets:withRange:",(IMP)hEncoderBuffers,"v@:^@^Q{_NSRange=QQ}");
 hMethod(hEncoderClass,"setVertexSamplerState:atIndex:",(IMP)hEncoderSampler,"v@:@Q");
 hMethod(hEncoderClass,"setFragmentSamplerState:atIndex:",(IMP)hEncoderSampler,"v@:@Q");
 hMethod(hEncoderClass,"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",(IMP)hEncoderIndexed,"v@:QQQ@Q");
 hMethod(hEncoderClass,"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",(IMP)hEncoderIndexedInstances,"v@:QQQ@QQ");
 hMethod(hEncoderClass,"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",(IMP)hEncoderIndexedFull,"v@:QQQ@QQqQ");
 hMethod(hEncoderClass,"setVertexBuffer:offset:atIndex:",(IMP)hEncoderBuffer,"v@:@QQ");
 hMethod(hEncoderClass,"setFragmentBuffer:offset:atIndex:",(IMP)hEncoderBuffer,"v@:@QQ");
 hMethod(hEncoderClass,"setVertexBytes:length:atIndex:",(IMP)hEncoderBytes,"v@:^vQQ");
 hMethod(hEncoderClass,"setFragmentBytes:length:atIndex:",(IMP)hEncoderBytes,"v@:^vQQ");
 hMethod(hEncoderClass,"setVertexTexture:atIndex:",(IMP)hEncoderTexture,"v@:@Q");
 hMethod(hEncoderClass,"setFragmentTexture:atIndex:",(IMP)hEncoderTexture,"v@:@Q");
 hMethod(hEncoderClass,"setBlendColorRed:green:blue:alpha:",(IMP)hEncoderBlendColor,"v@:ffff");
 hMethod(hEncoderClass,"setViewport:",(IMP)hEncoderViewport,"v@:{MTLViewport=dddddd}");
 hMethod(hEncoderClass,"setScissorRect:",(IMP)hEncoderScissor,"v@:{MTLScissorRect=QQQQ}");
 hMethod(hEncoderClass,"setRenderPipelineState:",(IMP)hEncoderPipeline,"v@:@"); hMethod(hEncoderClass,"drawPrimitives:vertexStart:vertexCount:",(IMP)hEncoderDraw,"v@:QQQ");
 hMethod(hEncoderClass,"drawPrimitives:vertexStart:vertexCount:instanceCount:",(IMP)hEncoderDrawInstances,"v@:QQQQ"); hMethod(hEncoderClass,"endEncoding",(IMP)hEncoderEnd,"v@:"); hMethod(hEncoderClass,"setDepthStencilState:",(IMP)hEncoderDepth,"v@:@");
 hMethod(hEncoderClass,"setFrontFacingWinding:",(IMP)hEncoderWinding,"v@:Q");
 hMethod(hEncoderClass,"setCullMode:",(IMP)hEncoderCull,"v@:Q");
 hMethod(hEncoderClass,"setTriangleFillMode:",(IMP)hEncoderFill,"v@:Q");
 hMethod(hEncoderClass,"setDepthClipMode:",(IMP)hEncoderClip,"v@:Q");
 hMethod(hEncoderClass,"setDepthBias:slopeScale:clamp:",(IMP)hEncoderBias,"v@:fff");
 hMethod(hEncoderClass,"setStencilReferenceValue:",(IMP)hEncoderStencil,"v@:I");
 hMethod(hEncoderClass,"setStencilFrontReferenceValue:backReferenceValue:",(IMP)hEncoderStencilFaces,"v@:II");
 objc_registerClassPair(hEncoderClass);
 hMethod(gQueueClass,"commandBuffer",(IMP)hCommandNew,"@@:"); hMethod(gQueueClass,"commandBufferWithUnretainedReferences",(IMP)hCommandNew,"@@:");
 hMethod(device,"newTextureWithDescriptor:",(IMP)hNewTexture,"@@:@"); hMethod(device,"newTextureWithDescriptor:iosurface:plane:",(IMP)hNewSurfaceTexture,"@@:@^{__IOSurface=}Q");
 hMethod(device,"newRenderPipelineStateWithDescriptor:error:",(IMP)hNewPipeline,"@@:@^@");
 hMethod(device,"newRenderPipelineStateWithDescriptor:options:reflection:error:",(IMP)hNewPipelineOptions,"@@:@Q^@^@");
}
