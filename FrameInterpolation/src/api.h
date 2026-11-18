#pragma once
#include <cstdint>
namespace motion {
constexpr uint32_t Abi=4;
inline constexpr uint32_t FpsOptions[]={60,120,180,240,360,480,0};
inline bool ValidFps(uint32_t rate){for(auto value:FpsOptions)if(value==rate)return true;return false;}
enum Status : uint32_t { Off, Waiting, Active, Unsupported, TooSlow, NeedVsyncOff, DeviceLost, Incompatible };
struct Settings { uint32_t size=sizeof(Settings),enabled=0,camera=1,geometry=1,cutProtection=1,targetHz=60,menus=1,showFps=0,logging=1; };
struct Stats {
    uint32_t size=sizeof(Stats),status=Off;
    float renderFps=0,baseFps=0,frameMs=0,peakMs=0;
    uint32_t loggingError=0;
};
using Overlay=void(__cdecl*)(void*,const Stats*);
struct Api {
    uint32_t size,version;
    void (__cdecl* get)(Settings*);
    int (__cdecl* set)(const Settings*);
    void (__cdecl* stats)(Stats*);
    void (__cdecl* overlay)(Overlay);
};
using GetApi=const Api* (__cdecl*)(uint32_t);
// Optional export HeroesMotionRegisterShader: a mod whose vertex shader's constants follow the fixed-function
// transforms registers it, and replays ask it for each image's constants.
using ShaderConstants=void(__cdecl*)(uint32_t handle,const float* world,const float* view,const float* projection,float* constants,uint32_t count);
using RegisterShader=int(__cdecl*)(uint32_t handle,uint32_t fvf,uint32_t count,ShaderConstants constants);
}
