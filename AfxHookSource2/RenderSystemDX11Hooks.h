#pragma once
#include "NadeCam.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#define ADVANCEDFX_STARTMOVIE_WAV_KEY "advancedfx-802bb089-972b-4841-bdf3-5108175ab59d"

#include <string>

bool AfxStreams_IsRcording();

/// Thread-safe, returns false if not set.
bool AfxStreams_GetStartMovieWavPath(std::wstring & outPath);

void AfxStreams_ShutDown();

void RenderSystemDX11_EngineThread_Prepare();
void RenderSystemDX11_EngineThread_BeforeRender(NadeCam::Pass pass, uint64_t shot = 0);

bool RenderSystemDX11_EngineThread_HasNextRenderPass();

void RenderSystemDX11_EngineThread_BeginNextRenderPass();
void RenderSystemDX11_EngineThread_EndNextRenderPass();

void RenderSystemDX11_EngineThread_BeginMainRenderPass(bool hasInset);
void RenderSystemDX11_EngineThread_EndMainRenderPass();

void Hook_RenderSystemDX11(void * hModule);

void Hook_SceneSystem(void * hModule);

void RenderSystemDX11_SupplyProjectionMatrix(const SOURCESDK::VMatrix & projectionMatrix);
