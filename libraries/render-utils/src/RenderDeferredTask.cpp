
//
//  RenderDeferredTask.cpp
//  render-utils/src/
//
//  Created by Sam Gateau on 5/29/15.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderDeferredTask.h"

#include <PerfStat.h>
#include <PathUtils.h>
#include <RenderArgs.h>
#include <ViewFrustum.h>
#include <gpu/Context.h>

#include <render/CullTask.h>
#include <render/SortTask.h>
#include <render/DrawTask.h>
#include <render/DrawStatus.h>
#include <render/DrawSceneOctree.h>
#include <render/BlurTask.h>

#include "LightingModel.h"
#include "DebugDeferredBuffer.h"
#include "DeferredFramebuffer.h"
#include "DeferredLightingEffect.h"
#include "SurfaceGeometryPass.h"
#include "FramebufferCache.h"
#include "HitEffect.h"
#include "TextureCache.h"

#include "AmbientOcclusionEffect.h"
#include "AntialiasingEffect.h"
#include "ToneMappingEffect.h"
#include "SubsurfaceScattering.h"

#include <gpu/StandardShaderLib.h>

#include "drawOpaqueStencil_frag.h"


using namespace render;
extern void initOverlay3DPipelines(render::ShapePlumber& plumber);
extern void initDeferredPipelines(render::ShapePlumber& plumber);

RenderDeferredTask::RenderDeferredTask(RenderFetchCullSortTask::Output items) {
    // Prepare the ShapePipelines
    ShapePlumberPointer shapePlumber = std::make_shared<ShapePlumber>();
    initDeferredPipelines(*shapePlumber);

    // Extract opaques / transparents / lights / metas / overlays / background
    const auto opaques = items[RenderFetchCullSortTask::OPAQUE_SHAPE];
    const auto transparents = items[RenderFetchCullSortTask::TRANSPARENT_SHAPE];
    const auto lights = items[RenderFetchCullSortTask::LIGHT];
    const auto metas = items[RenderFetchCullSortTask::META];
    const auto overlayOpaques = items[RenderFetchCullSortTask::OVERLAY_OPAQUE_SHAPE];
    const auto overlayTransparents = items[RenderFetchCullSortTask::OVERLAY_TRANSPARENT_SHAPE];
    const auto background = items[RenderFetchCullSortTask::BACKGROUND];
    const auto spatialSelection = items[RenderFetchCullSortTask::SPATIAL_SELECTION];

    // Filter the non antialiaased overlays
    const int LAYER_NO_AA = 3;
    const auto nonAAOverlays = addJob<FilterItemLayer>("Filter2DWebOverlays", overlayOpaques, LAYER_NO_AA);

    // Prepare deferred, generate the shared Deferred Frame Transform
    const auto deferredFrameTransform = addJob<GenerateDeferredFrameTransform>("DeferredFrameTransform");
    const auto lightingModel = addJob<MakeLightingModel>("LightingModel");


    // GPU jobs: Start preparing the primary, deferred and lighting buffer
    const auto primaryFramebuffer = addJob<PreparePrimaryFramebuffer>("PreparePrimaryBuffer");

   // const auto fullFrameRangeTimer = addJob<BeginGPURangeTimer>("BeginRangeTimer");
    const auto opaqueRangeTimer = addJob<BeginGPURangeTimer>("BeginOpaqueRangeTimer", "DrawOpaques");

    const auto prepareDeferredInputs = PrepareDeferred::Inputs(primaryFramebuffer, lightingModel).hasVarying();
    const auto prepareDeferredOutputs = addJob<PrepareDeferred>("PrepareDeferred", prepareDeferredInputs);
    const auto deferredFramebuffer = prepareDeferredOutputs.getN<PrepareDeferred::Outputs>(0);
    const auto lightingFramebuffer = prepareDeferredOutputs.getN<PrepareDeferred::Outputs>(1);

    // Render opaque objects in DeferredBuffer
    const auto opaqueInputs = DrawStateSortDeferred::Inputs(opaques, lightingModel).hasVarying();
    addJob<DrawStateSortDeferred>("DrawOpaqueDeferred", opaqueInputs, shapePlumber);

    // Once opaque is all rendered create stencil background
    addJob<DrawStencilDeferred>("DrawOpaqueStencil", deferredFramebuffer);

    addJob<EndGPURangeTimer>("OpaqueRangeTimer", opaqueRangeTimer);


    // Opaque all rendered

    // Linear Depth Pass
    const auto linearDepthPassInputs = LinearDepthPass::Inputs(deferredFrameTransform, deferredFramebuffer).hasVarying();
    const auto linearDepthPassOutputs = addJob<LinearDepthPass>("LinearDepth", linearDepthPassInputs);
    const auto linearDepthTarget = linearDepthPassOutputs.getN<LinearDepthPass::Outputs>(0);
    
    // Curvature pass
    const auto surfaceGeometryPassInputs = SurfaceGeometryPass::Inputs(deferredFrameTransform, deferredFramebuffer, linearDepthTarget).hasVarying();
    const auto surfaceGeometryPassOutputs = addJob<SurfaceGeometryPass>("SurfaceGeometry", surfaceGeometryPassInputs);
    const auto surfaceGeometryFramebuffer = surfaceGeometryPassOutputs.getN<SurfaceGeometryPass::Outputs>(0);
    const auto curvatureFramebuffer = surfaceGeometryPassOutputs.getN<SurfaceGeometryPass::Outputs>(1);
    const auto midCurvatureNormalFramebuffer = surfaceGeometryPassOutputs.getN<SurfaceGeometryPass::Outputs>(2);
    const auto lowCurvatureNormalFramebuffer = surfaceGeometryPassOutputs.getN<SurfaceGeometryPass::Outputs>(3);

    // Simply update the scattering resource
    const auto scatteringResource = addJob<SubsurfaceScattering>("Scattering");

    // AO job
    const auto ambientOcclusionInputs = AmbientOcclusionEffect::Inputs(deferredFrameTransform, deferredFramebuffer, linearDepthTarget).hasVarying();
    const auto ambientOcclusionOutputs = addJob<AmbientOcclusionEffect>("AmbientOcclusion", ambientOcclusionInputs);
    const auto ambientOcclusionFramebuffer = ambientOcclusionOutputs.getN<AmbientOcclusionEffect::Outputs>(0);
    const auto ambientOcclusionUniforms = ambientOcclusionOutputs.getN<AmbientOcclusionEffect::Outputs>(1);

    
    // Draw Lights just add the lights to the current list of lights to deal with. NOt really gpu job for now.
    addJob<DrawLight>("DrawLight", lights);

    // Light Clustering
    // Create the cluster grid of lights, cpu job for now
    const auto lightClusteringPassInputs = LightClusteringPass::Inputs(deferredFrameTransform, lightingModel, linearDepthTarget).hasVarying();
    const auto lightClusters = addJob<LightClusteringPass>("LightClustering", lightClusteringPassInputs);
    
    
    // DeferredBuffer is complete, now let's shade it into the LightingBuffer
    const auto deferredLightingInputs = RenderDeferred::Inputs(deferredFrameTransform, deferredFramebuffer, lightingModel,
        surfaceGeometryFramebuffer, ambientOcclusionFramebuffer, scatteringResource, lightClusters).hasVarying();
    
    addJob<RenderDeferred>("RenderDeferred", deferredLightingInputs);

    // Use Stencil and draw background in Lighting buffer to complete filling in the opaque
    const auto backgroundInputs = DrawBackgroundDeferred::Inputs(background, lightingModel).hasVarying();
    addJob<DrawBackgroundDeferred>("DrawBackgroundDeferred", backgroundInputs);
   
    
    // Render transparent objects forward in LightingBuffer
    const auto transparentsInputs = DrawDeferred::Inputs(transparents, lightingModel).hasVarying();
    addJob<DrawDeferred>("DrawTransparentDeferred", transparentsInputs, shapePlumber);

    // LIght Cluster Grid Debuging job
    {
        const auto debugLightClustersInputs = DebugLightClusters::Inputs(deferredFrameTransform, deferredFramebuffer, lightingModel, linearDepthTarget, lightClusters).hasVarying();
        addJob<DebugLightClusters>("DebugLightClusters", debugLightClustersInputs);
    }
    
    const auto toneAndPostRangeTimer = addJob<BeginGPURangeTimer>("BeginToneAndPostRangeTimer", "PostToneOverlaysAntialiasing");

    // Lighting Buffer ready for tone mapping
    const auto toneMappingInputs = render::Varying(ToneMappingDeferred::Inputs(lightingFramebuffer, primaryFramebuffer));
    addJob<ToneMappingDeferred>("ToneMapping", toneMappingInputs);

    // Overlays
    const auto overlayOpaquesInputs = DrawOverlay3D::Inputs(overlayOpaques, lightingModel).hasVarying();
    const auto overlayTransparentsInputs = DrawOverlay3D::Inputs(overlayTransparents, lightingModel).hasVarying();
    addJob<DrawOverlay3D>("DrawOverlay3DOpaque", overlayOpaquesInputs, true);
    addJob<DrawOverlay3D>("DrawOverlay3DTransparent", overlayTransparentsInputs, false);

    
    // Debugging stages
    {


        // Bounds do not draw on stencil buffer, so they must come last
        addJob<DrawBounds>("DrawMetaBounds", metas);

        // Debugging Deferred buffer job
        const auto debugFramebuffers = render::Varying(DebugDeferredBuffer::Inputs(deferredFramebuffer, linearDepthTarget, surfaceGeometryFramebuffer, ambientOcclusionFramebuffer));
        addJob<DebugDeferredBuffer>("DebugDeferredBuffer", debugFramebuffers);

        const auto debugSubsurfaceScatteringInputs = DebugSubsurfaceScattering::Inputs(deferredFrameTransform, deferredFramebuffer, lightingModel,
            surfaceGeometryFramebuffer, ambientOcclusionFramebuffer, scatteringResource).hasVarying();
        addJob<DebugSubsurfaceScattering>("DebugScattering", debugSubsurfaceScatteringInputs);

        const auto debugAmbientOcclusionInputs = DebugAmbientOcclusion::Inputs(deferredFrameTransform, deferredFramebuffer, linearDepthTarget, ambientOcclusionUniforms).hasVarying();
        addJob<DebugAmbientOcclusion>("DebugAmbientOcclusion", debugAmbientOcclusionInputs);


        // Scene Octree Debugging job
        {
            addJob<DrawSceneOctree>("DrawSceneOctree", spatialSelection);
            addJob<DrawItemSelection>("DrawItemSelection", spatialSelection);
        }

        // Status icon rendering job
        {
            // Grab a texture map representing the different status icons and assign that to the drawStatsuJob
            auto iconMapPath = PathUtils::resourcesPath() + "icons/statusIconAtlas.svg";
            auto statusIconMap = DependencyManager::get<TextureCache>()->getImageTexture(iconMapPath);
            addJob<DrawStatus>("DrawStatus", opaques, DrawStatus(statusIconMap));
        }
    }


    // AA job to be revisited
    addJob<Antialiasing>("Antialiasing", primaryFramebuffer);

    // Draw 2DWeb non AA
    const auto nonAAOverlaysInputs = DrawOverlay3D::Inputs(nonAAOverlays, lightingModel).hasVarying();
    addJob<DrawOverlay3D>("Draw2DWebSurfaces", nonAAOverlaysInputs, false);

    addJob<EndGPURangeTimer>("ToneAndPostRangeTimer", toneAndPostRangeTimer);

    // Blit!
    addJob<Blit>("Blit", primaryFramebuffer);

 //   addJob<EndGPURangeTimer>("RangeTimer", fullFrameRangeTimer);

}

void BeginGPURangeTimer::run(const render::SceneContextPointer& sceneContext, const render::RenderContextPointer& renderContext, gpu::RangeTimerPointer& timer) {
    timer = _gpuTimer;
    gpu::doInBatch(renderContext->args->_context, [&](gpu::Batch& batch) {
        _gpuTimer->begin(batch);
    });
}

void EndGPURangeTimer::run(const render::SceneContextPointer& sceneContext, const render::RenderContextPointer& renderContext, const gpu::RangeTimerPointer& timer) {
    gpu::doInBatch(renderContext->args->_context, [&](gpu::Batch& batch) {
        timer->end(batch);
    });
    
    auto config = std::static_pointer_cast<Config>(renderContext->jobConfig);
    config->setGPUBatchRunTime(timer->getGPUAverage(), timer->getBatchAverage());
}


void DrawDeferred::run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const Inputs& inputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    auto config = std::static_pointer_cast<Config>(renderContext->jobConfig);

    const auto& inItems = inputs.get0();
    const auto& lightingModel = inputs.get1();

    RenderArgs* args = renderContext->args;

    gpu::doInBatch(args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;
        
        // Setup camera, projection and viewport for all items
        batch.setViewportTransform(args->_viewport);
        batch.setStateScissorRect(args->_viewport);

        glm::mat4 projMat;
        Transform viewMat;
        args->getViewFrustum().evalProjectionMatrix(projMat);
        args->getViewFrustum().evalViewTransform(viewMat);

        batch.setProjectionTransform(projMat);
        batch.setViewTransform(viewMat);

        // Setup lighting model for all items;
        batch.setUniformBuffer(render::ShapePipeline::Slot::LIGHTING_MODEL, lightingModel->getParametersBuffer());

        renderShapes(sceneContext, renderContext, _shapePlumber, inItems, _maxDrawn);
        args->_batch = nullptr;
    });

    config->setNumDrawn((int)inItems.size());
}

void DrawStateSortDeferred::run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const Inputs& inputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    auto config = std::static_pointer_cast<Config>(renderContext->jobConfig);

    const auto& inItems = inputs.get0();
    const auto& lightingModel = inputs.get1();

    RenderArgs* args = renderContext->args;

    gpu::doInBatch(args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;

        // Setup camera, projection and viewport for all items
        batch.setViewportTransform(args->_viewport);
        batch.setStateScissorRect(args->_viewport);

        glm::mat4 projMat;
        Transform viewMat;
        args->getViewFrustum().evalProjectionMatrix(projMat);
        args->getViewFrustum().evalViewTransform(viewMat);

        batch.setProjectionTransform(projMat);
        batch.setViewTransform(viewMat);

        // Setup lighting model for all items;
        batch.setUniformBuffer(render::ShapePipeline::Slot::LIGHTING_MODEL, lightingModel->getParametersBuffer());

        if (_stateSort) {
            renderStateSortShapes(sceneContext, renderContext, _shapePlumber, inItems, _maxDrawn);
        } else {
            renderShapes(sceneContext, renderContext, _shapePlumber, inItems, _maxDrawn);
        }
        args->_batch = nullptr;
    });

    config->setNumDrawn((int)inItems.size());
}

DrawOverlay3D::DrawOverlay3D(bool opaque) :
    _shapePlumber(std::make_shared<ShapePlumber>()),
    _opaquePass(opaque) {
    initOverlay3DPipelines(*_shapePlumber);
}

void DrawOverlay3D::run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const Inputs& inputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    auto config = std::static_pointer_cast<Config>(renderContext->jobConfig);

    const auto& inItems = inputs.get0();
    const auto& lightingModel = inputs.get1();
    
    config->setNumDrawn((int)inItems.size());
    emit config->numDrawnChanged();

    if (!inItems.empty()) {
        RenderArgs* args = renderContext->args;

        // Clear the framebuffer without stereo
        // Needs to be distinct from the other batch because using the clear call 
        // while stereo is enabled triggers a warning
        if (_opaquePass) {
            gpu::doInBatch(args->_context, [&](gpu::Batch& batch){
                batch.enableStereo(false);
                batch.clearFramebuffer(gpu::Framebuffer::BUFFER_DEPTH, glm::vec4(), 1.f, 0, true);
            });
        }

        // Render the items
        gpu::doInBatch(args->_context, [&](gpu::Batch& batch) {
            args->_batch = &batch;
            batch.setViewportTransform(args->_viewport);
            batch.setStateScissorRect(args->_viewport);

            glm::mat4 projMat;
            Transform viewMat;
            args->getViewFrustum().evalProjectionMatrix(projMat);
            args->getViewFrustum().evalViewTransform(viewMat);

            batch.setProjectionTransform(projMat);
            batch.setViewTransform(viewMat);

            // Setup lighting model for all items;
            batch.setUniformBuffer(render::ShapePipeline::Slot::LIGHTING_MODEL, lightingModel->getParametersBuffer());

            renderShapes(sceneContext, renderContext, _shapePlumber, inItems, _maxDrawn);
            args->_batch = nullptr;
        });
    }
}


gpu::PipelinePointer DrawStencilDeferred::getOpaquePipeline() {
    if (!_opaquePipeline) {
        const gpu::int8 STENCIL_OPAQUE = 1;
        auto vs = gpu::StandardShaderLib::getDrawUnitQuadTexcoordVS();
        auto ps = gpu::Shader::createPixel(std::string(drawOpaqueStencil_frag));
        auto program = gpu::Shader::createProgram(vs, ps);
        gpu::Shader::makeProgram((*program));

        auto state = std::make_shared<gpu::State>();
        state->setDepthTest(true, false, gpu::LESS_EQUAL);
        state->setStencilTest(true, 0xFF, gpu::State::StencilTest(STENCIL_OPAQUE, 0xFF, gpu::ALWAYS, gpu::State::STENCIL_OP_REPLACE, gpu::State::STENCIL_OP_REPLACE, gpu::State::STENCIL_OP_KEEP));
        state->setColorWriteMask(0);

        _opaquePipeline = gpu::Pipeline::create(program, state);
    }
    return _opaquePipeline;
}

void DrawStencilDeferred::run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const DeferredFramebufferPointer& deferredFramebuffer) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    // from the touched pixel generate the stencil buffer 
    RenderArgs* args = renderContext->args;
    doInBatch(args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;

        auto deferredFboColorDepthStencil = deferredFramebuffer->getDeferredFramebufferDepthColor();
        

        batch.enableStereo(false);

        batch.setFramebuffer(deferredFboColorDepthStencil);
        batch.setViewportTransform(args->_viewport);
        batch.setStateScissorRect(args->_viewport);

        batch.setPipeline(getOpaquePipeline());

        batch.draw(gpu::TRIANGLE_STRIP, 4);
        batch.setResourceTexture(0, nullptr);

    });
    args->_batch = nullptr;
}

void DrawBackgroundDeferred::run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const Inputs& inputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    const auto& inItems = inputs.get0();
    const auto& lightingModel = inputs.get1();
    if (!lightingModel->isBackgroundEnabled()) {
        return;
    }

    RenderArgs* args = renderContext->args;
    doInBatch(args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;
    //    _gpuTimer.begin(batch);

        batch.enableSkybox(true);
        
        batch.setViewportTransform(args->_viewport);
        batch.setStateScissorRect(args->_viewport);

        glm::mat4 projMat;
        Transform viewMat;
        args->getViewFrustum().evalProjectionMatrix(projMat);
        args->getViewFrustum().evalViewTransform(viewMat);

        batch.setProjectionTransform(projMat);
        batch.setViewTransform(viewMat);

        renderItems(sceneContext, renderContext, inItems);
     //   _gpuTimer.end(batch);
    });
    args->_batch = nullptr;

   // std::static_pointer_cast<Config>(renderContext->jobConfig)->gpuTime = _gpuTimer.getAverage();
}

void Blit::run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const gpu::FramebufferPointer& srcFramebuffer) {
    assert(renderContext->args);
    assert(renderContext->args->_context);

    RenderArgs* renderArgs = renderContext->args;
    auto blitFbo = renderArgs->_blitFramebuffer;

    if (!blitFbo) {
        return;
    }

    // Determine size from viewport
    int width = renderArgs->_viewport.z;
    int height = renderArgs->_viewport.w;

    // Blit primary to blit FBO
    auto primaryFbo = srcFramebuffer;

    gpu::doInBatch(renderArgs->_context, [&](gpu::Batch& batch) {
        batch.setFramebuffer(blitFbo);

        if (renderArgs->_renderMode == RenderArgs::MIRROR_RENDER_MODE) {
            if (renderArgs->_context->isStereo()) {
                gpu::Vec4i srcRectLeft;
                srcRectLeft.z = width / 2;
                srcRectLeft.w = height;

                gpu::Vec4i srcRectRight;
                srcRectRight.x = width / 2;
                srcRectRight.z = width;
                srcRectRight.w = height;

                gpu::Vec4i destRectLeft;
                destRectLeft.x = srcRectLeft.z;
                destRectLeft.z = srcRectLeft.x;
                destRectLeft.y = srcRectLeft.y;
                destRectLeft.w = srcRectLeft.w;

                gpu::Vec4i destRectRight;
                destRectRight.x = srcRectRight.z;
                destRectRight.z = srcRectRight.x;
                destRectRight.y = srcRectRight.y;
                destRectRight.w = srcRectRight.w;

                // Blit left to right and right to left in stereo
                batch.blit(primaryFbo, srcRectRight, blitFbo, destRectLeft);
                batch.blit(primaryFbo, srcRectLeft, blitFbo, destRectRight);
            } else {
                gpu::Vec4i srcRect;
                srcRect.z = width;
                srcRect.w = height;

                gpu::Vec4i destRect;
                destRect.x = width;
                destRect.y = 0;
                destRect.z = 0;
                destRect.w = height;

                batch.blit(primaryFbo, srcRect, blitFbo, destRect);
            }
        } else {
            gpu::Vec4i rect;
            rect.z = width;
            rect.w = height;

            batch.blit(primaryFbo, rect, blitFbo, rect);
        }
    });
}
