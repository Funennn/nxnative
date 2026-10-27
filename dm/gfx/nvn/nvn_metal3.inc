// Experimental classic Metal command backend. Included only by nvn_mtl.m.
// The replayer keeps its GPU-address argument blocks; only direct bindings and
// command submission are translated. No Metal 4 objects are created on iOS 18.
static int g_metal3;
static NSLock *dm3_lock;
static NSHashTable *dm3_buffers, *dm3_resources;
static NSMapTable *dm3_ids;

static void dm3_register(id r) {
    if (!g_metal3 || !r) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dm3_lock = [NSLock new]; dm3_buffers = [NSHashTable weakObjectsHashTable];
        dm3_resources = [NSHashTable weakObjectsHashTable];
        dm3_ids = [NSMapTable strongToWeakObjectsMapTable];
    });
    [dm3_lock lock];
    if ([r conformsToProtocol:@protocol(MTLBuffer)]) [dm3_buffers addObject:r];
    if ([r conformsToProtocol:@protocol(MTLResource)]) [dm3_resources addObject:r];
    if ([r respondsToSelector:@selector(gpuResourceID)]) {
        MTLResourceID rid = ((id<MTLTexture>)r).gpuResourceID;
        [dm3_ids setObject:r forKey:@(rid._impl)];
    }
    [dm3_lock unlock];
}
static MTLResourceID dm_resource_id(id r) {
    dm3_register(r); return r ? ((id<MTLTexture>)r).gpuResourceID : (MTLResourceID){0};
}
static id<MTLBuffer> dm3_buffer(uint64_t address, NSUInteger *offset) {
    if (!address) { *offset = 0; return nil; }
    [dm3_lock lock];
    for (id<MTLBuffer> b in dm3_buffers) {
        uint64_t base = b.gpuAddress;
        if (address >= base && address - base < b.length) {
            *offset = (NSUInteger)(address - base); [dm3_lock unlock]; return b;
        }
    }
    [dm3_lock unlock];
    fprintf(stderr, "[RED] Metal 3: unresolved GPU buffer address 0x%llx\n", (unsigned long long)address);
    abort();
}
static id dm3_resource(MTLResourceID rid) {
    if (!rid._impl) return nil;
    [dm3_lock lock]; id r = [dm3_ids objectForKey:@(rid._impl)]; [dm3_lock unlock];
    if (!r) { fprintf(stderr, "[RED] Metal 3: unresolved resource ID\n"); abort(); }
    return r;
}

int nvn_sparse_alias(uint64_t lo, uint64_t hi);
static BOOL dm3_buffer_alias(id<MTLBuffer> b) {
    if (!b || b.storageMode == MTLStorageModePrivate || !b.contents) return NO;
    uintptr_t lo = (uintptr_t)b.contents, hi = lo + b.length;
    if (nvn_sparse_alias(lo, hi)) return YES;
    [dm3_lock lock];
    for (id<MTLBuffer> other in dm3_buffers) {
        if (other == b || other.storageMode == MTLStorageModePrivate || !other.contents) continue;
        uintptr_t start = (uintptr_t)other.contents;
        if (lo < start + other.length && start < hi) { [dm3_lock unlock]; return YES; }
    }
    [dm3_lock unlock]; return NO;
}

static BOOL dm3_live_aliases(void) {
    [dm3_lock lock]; NSArray *resources = dm3_resources.allObjects; [dm3_lock unlock];
    for (id<MTLResource> r in resources)
        if (r.heap || ([r conformsToProtocol:@protocol(MTLBuffer)] && dm3_buffer_alias((id)r))) return YES;
    return NO;
}

@interface Dm3TableDescriptor : NSObject
@property NSUInteger maxBufferBindCount, maxTextureBindCount, maxSamplerStateBindCount;
@end
@implementation Dm3TableDescriptor @end
@interface Dm3Table : NSObject { @public
    uint64_t addresses[31]; id textures[128]; id samplers[16];
}
- (void)setAddress:(uint64_t)a atIndex:(NSUInteger)i;
- (void)setTexture:(MTLResourceID)t atIndex:(NSUInteger)i;
- (void)setSamplerState:(MTLResourceID)s atIndex:(NSUInteger)i;
@end
@implementation Dm3Table
- (void)setAddress:(uint64_t)a atIndex:(NSUInteger)i { NSCAssert(i < 31, @"buffer slot"); addresses[i] = a; }
- (void)setTexture:(MTLResourceID)t atIndex:(NSUInteger)i { NSCAssert(i < 128, @"texture slot"); textures[i] = dm3_resource(t); }
- (void)setSamplerState:(MTLResourceID)s atIndex:(NSUInteger)i { NSCAssert(i < 16, @"sampler slot"); samplers[i] = dm3_resource(s); }
@end
@interface Dm3Allocator : NSObject
@property NSString *label;
- (void)reset;
- (NSUInteger)allocatedSize;
@end
@implementation Dm3Allocator
- (void)reset {} // Classic command buffers own their allocations until completion.
- (NSUInteger)allocatedSize { return 0; }
@end
@interface Dm3Options : NSObject
@property NSMutableArray *handlers;
- (void)addFeedbackHandler:(void (^)(id))handler;
@end
@implementation Dm3Options
- (instancetype)init { if ((self = [super init])) _handlers = [NSMutableArray array]; return self; }
- (void)addFeedbackHandler:(void (^)(id))handler { [_handlers addObject:[handler copy]]; }
@end

@class Dm3CommandBuffer;
@interface Dm3Encoder : NSObject {
@public id<MTLRenderCommandEncoder> render;
    id<MTLComputeCommandEncoder> compute; id<MTLBlitCommandEncoder> blit;
    Dm3CommandBuffer *owner; Dm3Table *vt, *ft, *ct;
    id<MTLComputePipelineState> cps;
}
- (void)endEncoding;
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v;
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v;
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v;
- (void)setArgumentTable:(id)t atStages:(MTLRenderStages)s;
- (void)setArgumentTable:(id)t;
- (id<MTLComputeCommandEncoder>)computeEncoder;
- (id<MTLBlitCommandEncoder>)blitEncoder;
@end
@interface Dm3CommandBuffer : NSObject {
@public id<MTLCommandBuffer> real; id<MTLCommandQueue> queue; id<MTLFence> fence; BOOL hasFence, splitAfterEncoder, aliasTouched;
}
- (void)beginCommandBufferWithAllocator:(id)a;
- (void)endCommandBuffer;
- (void)splitAndWait;
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d;
- (id)computeCommandEncoder;
@end

// Classic Metal needs explicit declarations for resources reached through GPU
// addresses and resource IDs in argument blocks. The initial backend declares
// the live set conservatively; per-shader residency pruning can follow profiling.
static void dm3_use_resources(id encoder, BOOL rendering) {
    [dm3_lock lock]; NSArray *resources = dm3_resources.allObjects; [dm3_lock unlock];
    for (id<MTLResource> r in resources) {
        if (rendering) [(id<MTLRenderCommandEncoder>)encoder useResource:r usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageVertex | MTLRenderStageFragment];
        else [(id<MTLComputeCommandEncoder>)encoder useResource:r usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    }
}
static void dm3_bind(Dm3Table *t, id encoder, int stage) {
    if (!t) return;
    for (NSUInteger i = 0; i < 31; i++) {
        NSUInteger off; id<MTLBuffer> b = dm3_buffer(t->addresses[i], &off);
        if (stage == 0) [(id<MTLRenderCommandEncoder>)encoder setVertexBuffer:b offset:off atIndex:i];
        else if (stage == 1) [(id<MTLRenderCommandEncoder>)encoder setFragmentBuffer:b offset:off atIndex:i];
        else [(id<MTLComputeCommandEncoder>)encoder setBuffer:b offset:off atIndex:i];
    }
    for (NSUInteger i = 0; i < 128; i++) if (t->textures[i]) {
        if (stage == 0) [(id<MTLRenderCommandEncoder>)encoder setVertexTexture:t->textures[i] atIndex:i];
        else if (stage == 1) [(id<MTLRenderCommandEncoder>)encoder setFragmentTexture:t->textures[i] atIndex:i];
        else [(id<MTLComputeCommandEncoder>)encoder setTexture:t->textures[i] atIndex:i];
    }
    for (NSUInteger i = 0; i < 16; i++) if (t->samplers[i]) {
        if (stage == 0) [(id<MTLRenderCommandEncoder>)encoder setVertexSamplerState:t->samplers[i] atIndex:i];
        else if (stage == 1) [(id<MTLRenderCommandEncoder>)encoder setFragmentSamplerState:t->samplers[i] atIndex:i];
        else [(id<MTLComputeCommandEncoder>)encoder setSamplerState:t->samplers[i] atIndex:i];
    }
}
@implementation Dm3Encoder
- (BOOL)conformsToProtocol:(Protocol *)p {
    return p == @protocol(MTL4CommandEncoder) || [super conformsToProtocol:p];
}
- (id)forwardingTargetForSelector:(SEL)s { return render && [render respondsToSelector:s] ? render : [super forwardingTargetForSelector:s]; }
- (void)setLabel:(NSString *)label { render.label = label; compute.label = label; blit.label = label; }
- (void)finishComputeOrBlit {
    if (compute) { [compute updateFence:owner->fence]; [compute endEncoding]; compute = nil; owner->hasFence = YES; }
    if (blit) { [blit updateFence:owner->fence]; [blit endEncoding]; blit = nil; owner->hasFence = YES; }
}
- (void)endEncoding {
    if (render) { [render updateFence:owner->fence afterStages:MTLRenderStageVertex | MTLRenderStageFragment]; [render endEncoding]; render = nil; owner->hasFence = YES; }
    [self finishComputeOrBlit]; if (owner->splitAfterEncoder) [owner splitAndWait]; owner = nil;
}
- (id<MTLComputeCommandEncoder>)computeEncoder {
    if (blit) [self finishComputeOrBlit];
    if (!compute) { if (dm3_live_aliases()) { [owner splitAndWait]; owner->aliasTouched = YES; } compute = [owner->real computeCommandEncoder]; if (owner->hasFence) [compute waitForFence:owner->fence]; }
    return compute;
}
- (id<MTLBlitCommandEncoder>)blitEncoder {
    // A fresh blit encoder per copy orders even overlapping aliasing resources.
    [self finishComputeOrBlit]; blit = [owner->real blitCommandEncoder];
    if (owner->hasFence) [blit waitForFence:owner->fence]; return blit;
}
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { (void)a; (void)b; (void)v; }
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { (void)a; (void)b; if ((v & MTL4VisibilityOptionResourceAlias) && owner->aliasTouched) owner->splitAfterEncoder = YES; }
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v {
    (void)a; (void)b; (void)v;
    if (render) [render memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures afterStages:MTLRenderStageVertex | MTLRenderStageFragment beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
    else { [self finishComputeOrBlit]; if ((v & MTL4VisibilityOptionResourceAlias) && owner->aliasTouched) [owner splitAndWait]; }
}
- (void)setArgumentTable:(id)t atStages:(MTLRenderStages)s { if (s & MTLRenderStageVertex) vt = t; if (s & MTLRenderStageFragment) ft = t; }
- (void)setArgumentTable:(id)t { ct = t; }
- (void)noteIndirectAliases {
    [dm3_lock lock]; NSArray *resources = dm3_resources.allObjects; [dm3_lock unlock];
    for (id<MTLResource> r in resources) if (r.heap || ([r conformsToProtocol:@protocol(MTLBuffer)] && dm3_buffer_alias((id)r))) { owner->aliasTouched = YES; break; }
}
- (void)beforeBuffer:(id<MTLBuffer>)a other:(id<MTLBuffer>)b {
    if (dm3_buffer_alias(a) || dm3_buffer_alias(b)) {
        [self finishComputeOrBlit]; [owner splitAndWait]; owner->aliasTouched = YES;
    }
}
- (void)prepareDraw { [self noteIndirectAliases]; dm3_bind(vt, render, 0); dm3_bind(ft, render, 1); dm3_use_resources(render, YES); }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n { [self prepareDraw]; [render drawPrimitives:p vertexStart:s vertexCount:n]; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)c baseInstance:(NSUInteger)b { [self prepareDraw]; [render drawPrimitives:p vertexStart:s vertexCount:n instanceCount:c baseInstance:b]; }
- (void)drawPrimitives:(MTLPrimitiveType)p indirectBuffer:(uint64_t)a { NSUInteger o; id<MTLBuffer> b = dm3_buffer(a, &o); [self prepareDraw]; [render drawPrimitives:p indirectBuffer:b indirectBufferOffset:o]; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n indexType:(MTLIndexType)t indexBuffer:(uint64_t)a indexBufferLength:(NSUInteger)length instanceCount:(NSUInteger)c baseVertex:(NSInteger)v baseInstance:(NSUInteger)i {
    NSUInteger o; id<MTLBuffer> b = dm3_buffer(a, &o); NSCAssert(length <= b.length - o, @"index buffer range");
    [self prepareDraw]; [render drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:b indexBufferOffset:o instanceCount:c baseVertex:v baseInstance:i];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexType:(MTLIndexType)t indexBuffer:(uint64_t)a indexBufferLength:(NSUInteger)length indirectBuffer:(uint64_t)ind {
    NSUInteger o, io; id<MTLBuffer> b = dm3_buffer(a, &o), ib = dm3_buffer(ind, &io); NSCAssert(length <= b.length - o, @"index buffer range");
    [self prepareDraw]; [render drawIndexedPrimitives:p indexType:t indexBuffer:b indexBufferOffset:o indirectBuffer:ib indirectBufferOffset:io];
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)p { cps = p; }
- (void)prepareDispatch { [self noteIndirectAliases]; id<MTLComputeCommandEncoder> e = [self computeEncoder]; [e setComputePipelineState:cps]; dm3_bind(ct, e, 2); dm3_use_resources(e, NO); }
- (void)dispatchThreadgroups:(MTLSize)n threadsPerThreadgroup:(MTLSize)t { [self prepareDispatch]; [compute dispatchThreadgroups:n threadsPerThreadgroup:t]; }
- (void)dispatchThreads:(MTLSize)n threadsPerThreadgroup:(MTLSize)t { [self prepareDispatch]; [compute dispatchThreads:n threadsPerThreadgroup:t]; }
- (void)dispatchThreadgroupsWithIndirectBuffer:(uint64_t)a threadsPerThreadgroup:(MTLSize)t { NSUInteger o; id<MTLBuffer> b = dm3_buffer(a, &o); [self prepareDispatch]; [compute dispatchThreadgroupsWithIndirectBuffer:b indirectBufferOffset:o threadsPerThreadgroup:t]; }
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)doff size:(NSUInteger)n { [self beforeBuffer:s other:d]; [[self blitEncoder] copyFromBuffer:s sourceOffset:so toBuffer:d destinationOffset:doff size:n]; }
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v { [self beforeBuffer:b other:nil]; [[self blitEncoder] fillBuffer:b range:r value:v]; }
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin { [self beforeBuffer:s other:nil]; [[self blitEncoder] copyFromBuffer:s sourceOffset:o sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:size toTexture:d destinationSlice:slice destinationLevel:level destinationOrigin:origin]; }
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image { [self beforeBuffer:d other:nil]; [[self blitEncoder] copyFromTexture:s sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:d destinationOffset:o destinationBytesPerRow:row destinationBytesPerImage:image]; }
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorigin { [[self blitEncoder] copyFromTexture:s sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:size toTexture:d destinationSlice:ds destinationLevel:dl destinationOrigin:dorigin]; }
@end
@implementation Dm3CommandBuffer
- (void)beginCommandBufferWithAllocator:(id)a { (void)a; real = [queue commandBuffer]; hasFence = NO; }
- (void)splitAndWait {
    // Classic encoder fences do not flush caches for distinct GPU mappings of
    // the same physical pages. A completed command buffer does. This expensive
    // boundary is deliberately limited to the replay's resource-alias barriers.
    [real commit]; [real waitUntilCompleted];
    if (real.error) { fprintf(stderr, "[RED] Metal 3 alias boundary: %s\n", real.error.description.UTF8String); abort(); }
    real = [queue commandBuffer]; hasFence = NO; splitAfterEncoder = NO; aliasTouched = NO;
}
- (void)endCommandBuffer {} // The encoders already ended; commit belongs to the queue.
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    if (dm3_live_aliases()) { [self splitAndWait]; aliasTouched = YES; }
    Dm3Encoder *e = [Dm3Encoder new]; e->owner = self; e->render = [real renderCommandEncoderWithDescriptor:d];
    if (hasFence) [e->render waitForFence:fence beforeStages:MTLRenderStageVertex | MTLRenderStageFragment]; return e;
}
- (id)computeCommandEncoder { Dm3Encoder *e = [Dm3Encoder new]; e->owner = self; return e; }
@end

@interface Dm3Queue : NSObject
@property id<MTLCommandQueue> real;
@property dispatch_queue_t feedback;
@property id<MTLCommandBuffer> lastSubmitted;
- (void)commit:(id const *)buffers count:(NSUInteger)count options:(Dm3Options *)options;
@end
@implementation Dm3Queue
- (void)addResidencySet:(id)set { (void)set; }
- (void)commit:(id const *)buffers count:(NSUInteger)count { [self commit:buffers count:count options:nil]; }
- (void)commit:(id const *)buffers count:(NSUInteger)count options:(Dm3Options *)options {
    NSArray *handlers = [options.handlers copy]; dispatch_queue_t feedback = _feedback;
    for (NSUInteger i = 0; i < count; i++) {
        Dm3CommandBuffer *b = buffers[i]; id<MTLCommandBuffer> cb = b->real;
        if (handlers.count) [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            dispatch_sync(feedback, ^{ for (void (^handler)(id) in handlers) handler(completed); });
        }];
        [cb commit]; self.lastSubmitted = cb;
    }
}
- (void)signalEvent:(id<MTLEvent>)e value:(uint64_t)v { id<MTLCommandBuffer> b = [_real commandBuffer]; [b encodeSignalEvent:e value:v]; [b commit]; }
- (void)waitForEvent:(id<MTLEvent>)e value:(uint64_t)v { id<MTLCommandBuffer> b = [_real commandBuffer]; [b encodeWaitForEvent:e value:v]; [b commit]; }
- (void)waitForDrawable:(id)d { (void)d; } // nextDrawable already acquires the classic drawable.
- (void)signalDrawable:(id)d {
    (void)d;
    // The caller presents CAMetalDrawable directly. Classic Metal requires the
    // producer to have been scheduled first (the usual addScheduledHandler path).
    [self.lastSubmitted waitUntilScheduled];
}
@end

@interface Dm3Device : NSObject { @public id<MTLDevice> real; id<MTLCommandQueue> queue; }
@end
@implementation Dm3Device
- (id)forwardingTargetForSelector:(SEL)s { return [real respondsToSelector:s] ? real : [super forwardingTargetForSelector:s]; }
- (id)newCommandQueue { return queue; }
- (id)newCommandAllocatorWithDescriptor:(id)d error:(NSError **)error { (void)d; (void)error; return [Dm3Allocator new]; }
- (id)newCommandAllocator { return [Dm3Allocator new]; }
- (id)newCommandBuffer { Dm3CommandBuffer *b = [Dm3CommandBuffer new]; b->queue = queue; b->fence = [real newFence]; return b; }
- (id)newArgumentTableWithDescriptor:(id)d error:(NSError **)error { (void)d; (void)error; return [Dm3Table new]; }
- (id)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o { id b = [real newBufferWithLength:n options:o]; dm3_register(b); return b; }
- (id)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o { id b = [real newBufferWithBytes:p length:n options:o]; dm3_register(b); return b; }
- (id)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)n options:(MTLResourceOptions)o deallocator:(void (^)(void *, NSUInteger))deallocator { id b = [real newBufferWithBytesNoCopy:p length:n options:o deallocator:deallocator]; dm3_register(b); return b; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d { MTLTextureDescriptor *tracked = [d copy]; tracked.hazardTrackingMode = MTLHazardTrackingModeTracked; id t = [real newTextureWithDescriptor:tracked]; dm3_register(t); return t; }
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d { id s = [real newSamplerStateWithDescriptor:d]; dm3_register(s); return s; }
@end
static id dm3_wrap_device(id<MTLDevice> real) {
    Dm3Device *d = [Dm3Device new]; d->real = real; d->queue = [real newCommandQueue]; return d;
}
static id dm_argument_descriptor(void) { return g_metal3 ? (id)[Dm3TableDescriptor new] : (id)[MTL4ArgumentTableDescriptor new]; }
static id dm_render_descriptor(void) { return g_metal3 ? (id)[MTLRenderPassDescriptor new] : (id)[MTL4RenderPassDescriptor new]; }
static id dm_commit_options(void) { return g_metal3 ? (id)[Dm3Options new] : (id)[MTL4CommitOptions new]; }
