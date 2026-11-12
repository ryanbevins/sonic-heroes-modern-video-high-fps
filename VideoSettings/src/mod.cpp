#include "core.h"
#include "draw.h"
#include "art.h"
#include "native_menu.h"
#include "camera.h"
#include "shadows.h"
#include "shadow_models.h"
#include "shadow_compare.h"
#include "enemy_shadows.h"
#include "object_shadows.h"
#include "self_shadows.h"
#include "wall_shadows.h"
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <string>
#include <cstdio>
#include <tlhelp32.h>
#include "../../FrameInterpolation/src/api.h"
#include "../../FrameInterpolation/src/game_code.h"

namespace {
using namespace video;
Settings current,edit,previous;
const motion::Api* motionApi=nullptr;
ULONGLONG lastMotionScan=0;
int TabCount(){return 3;}
int DisplayActionRow(){return motionApi?6:5;}
int SelectableRows(int category){return category==0?DisplayActionRow()+1:category==1?8:motionApi?2:1;}
// Object shadows (Graphics row 6) and self shadows (row 7) are disabled: their
// hooks are not installed, and the rows are greyed and skipped by navigation.
bool RowDisabled(int category,int r){return category==1&&(r==6||r==7);}
int StepRow(int category,int from,int direction){
    int count=SelectableRows(category),r=from;
    for(int i=0;i<count;i++){r=(r+direction+count)%count;if(!RowDisabled(category,r))return r;}
    return from;
}
float TabStep(){return 35.f;}
float RowStep(int category){return category==1?34.f:(category==0&&motionApi)?36.f:42.f;}
void __cdecl DrawFps(void* device,const motion::Stats* stats);
HMODULE fpsModule=nullptr; // Modern Video: bundled FPS DLL loaded by ReloadedStart
bool UseMotion(HMODULE source){
    auto get=reinterpret_cast<motion::GetApi>(GetProcAddress(source,"HeroesMotionApi"));if(!get)return false;
    auto candidate=get(motion::Abi);
    if(!candidate||candidate->size!=sizeof(motion::Api)||candidate->version!=motion::Abi||!candidate->get||!candidate->set||!candidate->stats||!candidate->overlay)return false;
    motionApi=candidate;motionApi->overlay(&DrawFps);return true;
}
void DiscoverMotion(){
    if(motionApi)return;
    if(fpsModule){UseMotion(fpsModule);return;}
    auto now=GetTickCount64();if(now-lastMotionScan<1000)return;lastMotionScan=now;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());if(snapshot==INVALID_HANDLE_VALUE)return;
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Module32FirstW(snapshot,&entry))do{
        if(wcsncmp(entry.szModule,L"FrameInterpolation.",19)!=0)continue;
        if(UseMotion(entry.hModule))break;
    }while(Module32NextW(snapshot,&entry));CloseHandle(snapshot);
}
Confirmation confirmation;
MenuTransition pageMotion;
HMODULE module;
std::wstring configPath;
bool ready=false,initFailed=false,open=false,focused=false,rootVisible=false,pending=false,rollback=false,startup=false;
bool saveExists=false,saveError=false,applying=false;
bool keyboardHints=false;
constexpr float rootX=690,rootY=337.5f;
bool rootEntering=false;
native_menu::Hexes nativeHexes;
ui::NativeTransform rootPose;
std::array<ui::NativeTransform,4> buttonPoses{};
std::array<void*,5> buttonShapes{};
void* renderingShape=nullptr;
DWORD renderingTint=0xffffffff;
using ShapeFn=void*(__cdecl*)(void*,void*,void*);
ShapeFn originalShape=nullptr;
void* menuObject=nullptr;
int rootChoice=0,nativeChoice=0;
bool entrancePlaying=false,audioOpen=false;
int audioMotion=0,audioChoice=0;
float animationFrame=1;
ULONGLONG lastMenu=0;
unsigned frames=0;
int tab=0,row=0,lastDirection=0;
ULONGLONG repeatAt=0;
std::string message;
std::vector<D3DDISPLAYMODE> modes;
UINT adapter=0;D3DDEVTYPE deviceType=D3DDEVTYPE_HAL;DWORD maxAniso=1,filterCaps=0;
D3DDISPLAYMODE desktop{};
Present8 previousPresent{};
RECT previousRect{};LONG previousStyle=0,previousExStyle=0,windowedStyle=0;
using ExecFn=void(__fastcall*)(void*,void*);
using ShowFn=int(__cdecl*)(void*,void*,int);
using EndFn=HRESULT(__stdcall*)(void*);
using DrawFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,UINT);
using IndexedFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,UINT,UINT,UINT);
using DrawUPFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
using IndexedUPFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
using RwDrawFn=int(__cdecl*)(int,ui::Vertex*,int);
using RwIndexedFn=int(__cdecl*)(int,ui::Vertex*,int,unsigned short*,int);
RwDrawFn originalRwDraw=nullptr;
RwIndexedFn originalRwIndexed=nullptr;
ExecFn originalExec=nullptr;ShowFn originalShow=nullptr;EndFn originalEnd=nullptr;
DrawFn originalDraw=nullptr;IndexedFn originalIndexed=nullptr;
DrawUPFn originalDrawUP=nullptr;IndexedUPFn originalIndexedUP=nullptr;

using ResetFn=HRESULT(__stdcall*)(void*,Present8*);ResetFn originalReset=nullptr;
HRESULT __stdcall SelfShadowReset(void* device,Present8* params){self_shadows::Release();return originalReset(device,params);}
void Sound(int id=0xe007){
#ifndef VIDEO_TEST
    void* manager=Mem<void*>(0xa2f8b0);if(!manager)return;
    // Retail menu ABI: ESI = sound manager, EBX = cue; callee pops two args.
    __asm {
        push esi
        push ebx
        mov esi,manager
        mov ebx,id
        push 0
        push 0
        mov eax,440720h
        call eax
        pop ebx
        pop esi
    }
#else
    (void)id;
#endif
}

// Only the successful Options-save call at 0x451d65 reaches this stub.
// Its caller already waited for the save worker, checked the error result,
// closed the progress window, and set the normal exit flag. Continue exactly
// as acknowledgement would, without allocating the success popup.
// Native factory ABI: ESI=Options, six stack args, callee pops 0x18 bytes.
__declspec(naked) void SkipSaveSuccess(){
    __asm {
        mov dword ptr [esi+2ch],2
        ret 18h
    }
}
constexpr unsigned char saveSuccessCall[]={0xe8,0x26,0x02,0,0};

constexpr unsigned char cameraPrologue[]={0x83,0xec,0x18,0x8b,0x15,0xe8,0x0b,0xa6,0x00};
void* originalCamera=nullptr;
void __stdcall ApplyFov(void* object){
#ifndef VIDEO_TEST
    if(!ready||current.fov==100||!object)return;
    int index=Mem<int>(0xa60be8);if(index<0||index>3)return;
    int mode=Mem<int>(0xa62f00+index*0x2324);
    if(mode<0||mode>2)return; // Other modes leave the view unchanged; never compound it.
    void* camera=*reinterpret_cast<void**>(static_cast<char*>(object)+0x28);
    ApplyCameraView(camera,current.fov,reinterpret_cast<CameraView*>(0xa779c0),reinterpret_cast<SetCameraViewFn>(0x64ac80));
#else
    (void)object;
#endif
}
// TObjCamera ABI: this in ESI, no stack arguments. Preserve native results,
// registers and flags after running the existing camera routine.
__declspec(naked) void CameraUpdate(){
    __asm {
        call dword ptr [originalCamera]
        pushfd
        pushad
        push esi
        call ApplyFov
        popad
        popfd
        ret
    }
}

bool Write(void* destination,const void* source,size_t size){
    DWORD old;if(!VirtualProtect(destination,size,PAGE_EXECUTE_READWRITE,&old))return false;
    memcpy(destination,source,size);FlushInstructionCache(GetCurrentProcess(),destination,size);
    DWORD ignored;return VirtualProtect(destination,size,old,&ignored)!=FALSE;
}
template<class F> bool Slot(void** table,int index,F fn,F& original){
    original=reinterpret_cast<F>(table[index]);void* replacement=reinterpret_cast<void*>(fn);
    return Write(table+index,&replacement,sizeof(replacement));
}
bool KnownExecutable(){return game_code::Supported();}
// Names the first failed startup check, so a report screenshot says why.
const wchar_t* StartupProblem(bool knownExecutable,uintptr_t optionsExec,bool showHook,bool saveHook,bool cameraHook){
    if(!knownExecutable)return L"this Tsonic_win.exe has different game code from the supported PC version.";
    if(optionsExec!=0x4060a0)return L"another mod has already replaced the Options menu (0x74F530).";
    if(!showHook)return L"another mod has already hooked the display update (0x658D40).";
    if(!saveHook)return L"another mod has already changed the Options save step (0x451D65).";
    if(!cameraHook)return L"another mod has already hooked the camera (0x61E4C0).";
    return nullptr;
}
using ShadowInitFn=void(__cdecl*)();ShadowInitFn originalShadowInit=nullptr;
using LoadClumpsFn=void*(__stdcall*)(void*,const char* const*,int);
LoadClumpsFn originalLoadClumps=nullptr;
constexpr unsigned char loadClumpsPrologue[]={0x64,0xa1,0,0,0,0};
void* __stdcall LoadShadowModels(void* player,const char* const* names,int count){
    if(!player||!names||count<1||count>shadow_models::MaxFiles)return originalLoadClumps(player,names,count);
    auto plan=shadow_models::Select(names,count,shadow_quality::compatible&&shadow_quality::requested>0);
    plan.count=count;if(shadow_compare::installed)shadow_models::KeepOriginals(plan,names,count);
    void* result=originalLoadClumps(player,plan.names.data(),plan.count);
    void* list=shadow_models::Field<void*>(player,0x920);
    if(list)for(int i=0;i<count;++i)if(plan.replaced[i]){
        void* clump=shadow_models::Field<void*>(list,size_t(i)*8+4);
        shadow_models::Silhouette(clump,reinterpret_cast<shadow_models::SetTexture>(0x66d610),reinterpret_cast<shadow_models::SetEffects>(0x677340));
    }
    if(list)for(int i=0;i<count;++i)if(names[i]&&_strnicmp(names[i],"k_",2))self_shadows::Register(shadow_models::Field<void*>(list,size_t(i)*8+4));
    if(list&&shadow_compare::installed)shadow_compare::Register(player,plan,count,list);
    return result;
}
bool InstallShadowModels(){
    if(memcmp(reinterpret_cast<void*>(0x57fc90),loadClumpsPrologue,sizeof(loadClumpsPrologue)))return false;
    auto stub=static_cast<unsigned char*>(VirtualAlloc(nullptr,11,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!stub)return false;
    memcpy(stub,loadClumpsPrologue,6);stub[6]=0xe9;*reinterpret_cast<int32_t*>(stub+7)=int32_t(0x57fc96-reinterpret_cast<uintptr_t>(stub+11));
    DWORD old;if(!VirtualProtect(stub,11,PAGE_EXECUTE_READ,&old)){VirtualFree(stub,0,MEM_RELEASE);return false;}
    FlushInstructionCache(GetCurrentProcess(),stub,11);unsigned char jump[6]={0xe9,0,0,0,0,0x90};
    *reinterpret_cast<int32_t*>(jump+1)=int32_t(reinterpret_cast<uintptr_t>(&LoadShadowModels)-0x57fc95);
    originalLoadClumps=reinterpret_cast<LoadClumpsFn>(stub);
    if(!Write(reinterpret_cast<void*>(0x57fc90),jump,6)){originalLoadClumps=nullptr;VirtualFree(stub,0,MEM_RELEASE);return false;}
    return true;
}
constexpr unsigned char shadowInitPrologue[]={0x83,0xec,0x14,0x56,0x57};
constexpr unsigned char shadowFilterCall[]={0xe8,0x98,0x18,0x01,0x00};
int __cdecl FilterShadow(int type,ui::Vertex* vertices,int count){
    ui::Vertex copy[4];
    if(type==4&&shadow_quality::compatible&&shadow_quality::FilterQuad(vertices,copy,count,shadow_quality::active))vertices=copy;
    // Invoke the engine entry, preserving Video Settings and other renderer
    // hooks. Never modify the game's retained quad or bypass the hook chain.
    return reinterpret_cast<RwDrawFn>(0x64ca10)(type,vertices,count);
}
bool InstallShadowFilter(){
    if(memcmp(reinterpret_cast<void*>(0x63b173),shadowFilterCall,sizeof(shadowFilterCall)))return false;
    unsigned char call[5]={0xe8};*reinterpret_cast<int32_t*>(call+1)=int32_t(reinterpret_cast<uintptr_t>(&FilterShadow)-0x63b178);
    return Write(reinterpret_cast<void*>(0x63b173),call,5);
}
void __cdecl InitShadows(){
    if(shadow_quality::compatible){
        int quality=std::min(shadow_quality::requested,shadow_quality::maxQuality);
        shadow_quality::compatible=shadow_quality::Apply(quality,uint32_t(reinterpret_cast<uintptr_t>(&shadow_quality::blurStep)),
            [](uintptr_t at){return Mem<uint32_t>(at);},[](uintptr_t at,uint32_t value){return Write(reinterpret_cast<void*>(at),&value,4);});
    }
    originalShadowInit();
}
bool InstallShadowQuality(){
    if(memcmp(reinterpret_cast<void*>(0x63aa30),shadowInitPrologue,sizeof(shadowInitPrologue)))return false;
    for(const auto& site:shadow_quality::sites)if(Mem<uint32_t>(site.address)!=site.original)return false;
    auto stub=static_cast<unsigned char*>(VirtualAlloc(nullptr,10,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!stub)return false;
    memcpy(stub,shadowInitPrologue,5);stub[5]=0xe9;*reinterpret_cast<int32_t*>(stub+6)=int32_t(0x63aa35-reinterpret_cast<uintptr_t>(stub+10));
    DWORD old;if(!VirtualProtect(stub,10,PAGE_EXECUTE_READ,&old)){VirtualFree(stub,0,MEM_RELEASE);return false;}
    FlushInstructionCache(GetCurrentProcess(),stub,10);unsigned char jump[5]={0xe9};*reinterpret_cast<int32_t*>(jump+1)=int32_t(reinterpret_cast<uintptr_t>(&InitShadows)-0x63aa35);
    originalShadowInit=reinterpret_cast<ShadowInitFn>(stub);
    if(!Write(reinterpret_cast<void*>(0x63aa30),jump,5)){originalShadowInit=nullptr;VirtualFree(stub,0,MEM_RELEASE);return false;}
    return true;
}
void* originalCompareRender=nullptr;
void* originalCompareEnd=nullptr;
void* originalCompareDestroy=nullptr;
void PollShadowCompare(){
#ifdef MODERN_VIDEO
    constexpr bool down=false; // F6 Original/Ultra comparison is a development tool; not in user builds.
#else
    bool down=(GetAsyncKeyState(VK_F6)&0x8000)!=0;
#endif
    bool press=down&&!shadow_compare::keyDown;shadow_compare::keyDown=down;
    if(!press||GetForegroundWindow()!=Window()||!ready)return;
    if(!shadow_quality::compatible||(!shadow_compare::original&&shadow_quality::active!=2)||shadow_compare::casters.empty()||!Mem<void*>(0xa75368)){
        shadow_compare::Notify("F6: select Ultra, then load a level");return;
    }
    int target=shadow_compare::original?2:0,size=int(shadow_quality::Quality(target).size);
    if(!shadow_compare::Create(shadow_compare::sharedInactive,size)){shadow_compare::Notify("Shadow comparison unavailable");return;}
    for(auto& entry:shadow_compare::casters){
        if(!shadow_models::Field<void*>(entry.first,0x20))continue;
        if(!shadow_compare::Create(entry.second.inactive,size)){shadow_compare::Notify("Shadow comparison unavailable");return;}
    }
    if(!shadow_quality::Apply(target,uint32_t(reinterpret_cast<uintptr_t>(&shadow_quality::blurStep)),
        [](uintptr_t at){return Mem<uint32_t>(at);},[](uintptr_t at,uint32_t value){return Write(reinterpret_cast<void*>(at),&value,4);})){shadow_compare::Notify("Shadow comparison unavailable");return;}
    shadow_compare::Swap(shadow_compare::sharedInactive,Mem<void*>(0xa75368),Mem<void*>(0xa75364));
    for(auto& entry:shadow_compare::casters){
        using shadow_models::Field;void* effect=entry.first;
        if(!Field<void*>(effect,0x20))continue;
        shadow_compare::Swap(entry.second.inactive,Field<void*>(effect,0x20),Field<void*>(effect,0x24));
        shadow_compare::Bind(effect);
    }
    shadow_compare::original=!shadow_compare::original;
    shadow_compare::Notify(shadow_compare::original?"Shadows: Original  |  F6: Ultra":"Shadows: Ultra  |  F6: Original");
}
int __cdecl CompareRender(){
    self_shadows::enabled=current.selfShadows&&!shadow_compare::original;
    enemy_shadows::BeginFrame();
    PollShadowCompare();shadow_compare::PrepareModels();shadow_compare::drawing=true;
    int result=reinterpret_cast<int(__cdecl*)()>(originalCompareRender)();
    shadow_compare::drawing=false;shadow_compare::RestoreModels();return result;
}
void __cdecl CompareEnd(){self_shadows::playerCount=0;self_shadows::Invalidate();object_shadows::Clear();shadow_compare::End();reinterpret_cast<void(__cdecl*)()>(originalCompareEnd)();}
__declspec(naked) void CompareDestroy(){
    __asm {
        pushfd
        pushad
        push esi
        call shadow_compare::Remove
        popad
        popfd
        jmp dword ptr [originalCompareDestroy]
    }
}
bool CompareHook(uintptr_t at,const unsigned char* expected,size_t length,void* replacement,void*& original){
    if(length<5||length>8||memcmp(reinterpret_cast<void*>(at),expected,length))return false;
    auto stub=static_cast<unsigned char*>(VirtualAlloc(nullptr,length+5,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!stub)return false;
    memcpy(stub,expected,length);stub[length]=0xe9;*reinterpret_cast<int32_t*>(stub+length+1)=int32_t(at+length-reinterpret_cast<uintptr_t>(stub+length+5));
    DWORD old;if(!VirtualProtect(stub,length+5,PAGE_EXECUTE_READ,&old)){VirtualFree(stub,0,MEM_RELEASE);return false;}
    FlushInstructionCache(GetCurrentProcess(),stub,length+5);unsigned char patch[8]={0xe9,0,0,0,0,0x90,0x90,0x90};
    *reinterpret_cast<int32_t*>(patch+1)=int32_t(reinterpret_cast<uintptr_t>(replacement)-at-5);original=stub;
    if(!Write(reinterpret_cast<void*>(at),patch,length)){original=nullptr;VirtualFree(stub,0,MEM_RELEASE);return false;}return true;
}
bool InstallShadowCompare(){
    const unsigned char render[]={0xa1,0xe4,0x77,0xa7,0},end[]={0x33,0xc0,0x33,0xc9,0x33,0xd2},destroy[]={0x8a,0x06,0x57,0x33,0xff};
    if(memcmp(reinterpret_cast<void*>(0x63b2a0),render,5)||memcmp(reinterpret_cast<void*>(0x63aaf0),end,6)||memcmp(reinterpret_cast<void*>(0x63ad00),destroy,5))return false;
    return CompareHook(0x63ad00,destroy,5,reinterpret_cast<void*>(&CompareDestroy),originalCompareDestroy)&&
        CompareHook(0x63aaf0,end,6,reinterpret_cast<void*>(&CompareEnd),originalCompareEnd)&&
        CompareHook(0x63b2a0,render,5,reinterpret_cast<void*>(&CompareRender),originalCompareRender);
}
void* originalEnemyOpaque=nullptr,*originalEnemyTransparent=nullptr,*originalEnemyClump=nullptr;
void* originalSelfPlayerAtomic=nullptr;
void* originalEntryFloat=nullptr,*originalEntryAngle=nullptr,*originalEntryShaped=nullptr;
bool EnemyModelsEnabled(){return enemy_shadows::installed&&enemy_shadows::enabled&&!shadow_compare::original;}
void SelfShadowGroup(void* const* clumps,unsigned count);
// Hit enemies blink: TObjEnemy's timers (+0x191..+0x198, read by 0x5e42a0)
// make their draw skip every model on alternate frames, while the native
// shadow keeps drawing. With nothing recorded the silhouette fell back to the
// blob on those frames. Re-run the draw with timers cleared and model
// rendering suppressed, only to learn which clumps it would draw.
using BlinkFn=int(__thiscall*)(void*);BlinkFn enemyBlink=reinterpret_cast<BlinkFn>(0x5e42a0);
constexpr size_t EnemyBlinkTimers=0x191,EnemyBlinkTimerCount=8;
int __fastcall EnemyOpaque(void* actor,void*){
    auto before=enemy_shadows::recording;
    enemy_shadows::recording=(EnemyModelsEnabled()||self_shadows::enabled)?enemy_shadows::Record(actor):nullptr;
    auto caster=enemy_shadows::recording;
    auto disp=reinterpret_cast<int(__thiscall*)(void*)>(originalEnemyOpaque);
    int result=disp(actor);
    const bool drawn=caster&&caster->count;
    if(caster&&!drawn&&caster->supported&&EnemyModelsEnabled()&&enemyBlink(actor)>0){
        auto timers=static_cast<unsigned char*>(actor)+EnemyBlinkTimers;unsigned char saved[EnemyBlinkTimerCount];
        memcpy(saved,timers,sizeof(saved));memset(timers,0,sizeof(saved));
        enemy_shadows::probing=true;disp(actor);enemy_shadows::probing=false;
        memcpy(timers,saved,sizeof(saved));
    }
    enemy_shadows::recording=before;
    if(drawn&&caster->supported)SelfShadowGroup(caster->clumps.data(),caster->count);return result;
}
int __fastcall EnemyTransparent(void* actor,void*){
    auto before=enemy_shadows::casting;bool replaced=enemy_shadows::replaced;
    enemy_shadows::casting=EnemyModelsEnabled()?enemy_shadows::Find(actor):nullptr;enemy_shadows::replaced=false;
    int result=reinterpret_cast<int(__thiscall*)(void*)>(originalEnemyTransparent)(actor);
    enemy_shadows::casting=before;enemy_shadows::replaced=replaced;return result;
}
void* __cdecl EnemyClump(void* clump){
    if(enemy_shadows::probing){if(enemy_shadows::recording)enemy_shadows::recording->Add(clump);return clump;} // RpClumpRender returns its clump
    if(enemy_shadows::recording&&!enemy_shadows::drawing)enemy_shadows::recording->Add(clump);
    auto result=reinterpret_cast<void*(__cdecl*)(void*)>(originalEnemyClump)(clump);
    return result;
}
self_shadows::Bounds SelfBounds(void* const* clumps,unsigned count){
    self_shadows::Bounds bounds;
#ifndef VIDEO_TEST
    struct Sphere {self_shadows::Vec center;float radius;};
    for(unsigned i=0;i<count;i++){
        auto clump=reinterpret_cast<uintptr_t>(clumps[i]);if(!clump)continue;
        auto end=clump+8,link=Mem<uintptr_t>(end);unsigned visited=0;
        while(link!=end&&visited++<128){
            auto atomic=link-0x40;
            if(Mem<unsigned char>(atomic+2)&4){auto sphere=reinterpret_cast<const Sphere*(__cdecl*)(void*)>(0x66af40)(reinterpret_cast<void*>(atomic));if(sphere)bounds.Add(sphere->center,sphere->radius);}
            link=Mem<uintptr_t>(link);
        }
    }
#else
    (void)clumps;(void)count;
#endif
    return bounds;
}
void SelfShadowGroup(void* const* clumps,unsigned count){
    self_shadows::enabled=current.selfShadows&&!shadow_compare::original;
    if(!self_shadows::enabled||self_shadows::busy||shadow_compare::drawing||enemy_shadows::drawing||!count)return;
    self_shadows::RenderGroup(clumps,count,SelfBounds(clumps,count),reinterpret_cast<self_shadows::Render>(originalEnemyClump));
}
void* __cdecl SelfPlayerAtomic(void* atomic){
    auto native=reinterpret_cast<self_shadows::Render>(originalSelfPlayerAtomic);
    auto result=native(atomic);
#ifndef VIDEO_TEST
    self_shadows::enabled=current.selfShadows&&!shadow_compare::original;
    if(self_shadows::enabled&&!self_shadows::busy&&!shadow_compare::drawing&&!enemy_shadows::drawing&&atomic){
        void* clump=shadow_models::Field<void*>(atomic,0x3c);
        auto game=Mem<uintptr_t>(0xa777e4);
        if(game&&self_shadows::Player(clump))self_shadows::RenderAtomic(clump,atomic,Mem<uint32_t>(game+0x30),SelfBounds(&clump,1),reinterpret_cast<self_shadows::Render>(originalEnemyClump),native);
    }
#endif
    return result;
}
bool InstallSelfShadows(){
    const unsigned char expected[]={0xa1,0x60,0xbd,0x8b,0};
    return CompareHook(0x581e60,expected,5,reinterpret_cast<void*>(&SelfPlayerAtomic),originalSelfPlayerAtomic);
}
bool DrawModelShadow(enemy_shadows::Caster& caster,enemy_shadows::Vec ground,enemy_shadows::Vec degrees,float opacity){
    using namespace enemy_shadows;
    if(drawing||!caster.count||!caster.supported||!std::isfinite(opacity)||opacity<=0)return false;
    if(!Project(ground,degrees,projection)||!HasStencil(Device()))return false;
    tint=DWORD(std::clamp(opacity,0.f,1.f)*112.f)<<24;
    if(!ClearCoverage(Device()))return false;
    submitted=0;drawing=true;
    for(unsigned i=0;i<caster.count;++i)reinterpret_cast<void*(__cdecl*)(void*)>(originalEnemyClump)(caster.clumps[i]);
    drawing=false;ClearCoverage(Device());return submitted!=0;
}
int __stdcall TryEnemyShadow(const void* angles,const enemy_shadows::Vec* ground,float opacity,int integerAngles){
    using namespace enemy_shadows;
    if(object_shadows::suppressBlob)return 1;
    if(!casting||!EnemyModelsEnabled()||drawing)return 0;
    if(replaced)return 1;
    if(!angles||!ground)return 0;
    Vec degrees;
    if(integerAngles){auto a=static_cast<const int*>(angles);degrees={float(a[0]&65535)*360.f/65536,float(a[1]&65535)*360.f/65536,float(a[2]&65535)*360.f/65536};}
    else degrees=*static_cast<const Vec*>(angles);
    replaced=DrawModelShadow(*casting,*ground,degrees,opacity);return replaced?1:0;
}
// Native usercall: EAX=angles; stack=position, scale, opacity; caller pops 12.
// pushfd/pushad preserve the original arguments and all registers on fallback.
__declspec(naked) void EnemyEntryFloat(){
    __asm {
        pushfd
        pushad
        mov edx,esp
        push 0
        push dword ptr [edx+48]
        push dword ptr [edx+40]
        push dword ptr [edx+28]
        call TryEnemyShadow
        test eax,eax
        jz native
        popad
        popfd
        ret
    native:
        popad
        popfd
        jmp dword ptr [originalEntryFloat]
    }
}
__declspec(naked) void EnemyEntryAngle(){
    __asm {
        pushfd
        pushad
        mov edx,esp
        push 1
        push dword ptr [edx+48]
        push dword ptr [edx+40]
        push dword ptr [edx+28]
        call TryEnemyShadow
        test eax,eax
        jz native
        popad
        popfd
        ret
    native:
        popad
        popfd
        jmp dword ptr [originalEntryAngle]
    }
}
// Egg Flapper, Klagen and E-series shaped shadows: float angles like 0x63b7b0,
// plus a fixed quad offset. Same usercall arguments, so the same stub layout.
__declspec(naked) void EnemyEntryShaped(){
    __asm {
        pushfd
        pushad
        mov edx,esp
        push 0
        push dword ptr [edx+48]
        push dword ptr [edx+40]
        push dword ptr [edx+28]
        call TryEnemyShadow
        test eax,eax
        jz native
        popad
        popfd
        ret
    native:
        popad
        popfd
        jmp dword ptr [originalEntryShaped]
    }
}
bool InstallEnemyShadows(){
    const unsigned char actor[]={0x83,0xec,0x0c,0x56,0x8b,0xf1},clump[]={0x8b,0x44,0x24,0x04,0x53},entry[]={0x83,0xec,0x70,0x56,0x8b,0xf0};
    const unsigned char shaped[]={0xd9,0x44,0x24,0x0c,0x83,0xec,0x7c},blink[]={0x0f,0xbe,0x91,0x91,0x01,0x00,0x00};
    // Verify every site before installing any of this optional feature.
    if(memcmp(reinterpret_cast<void*>(0x5e2740),actor,6)||memcmp(reinterpret_cast<void*>(0x5e2790),actor,6)||
       memcmp(reinterpret_cast<void*>(0x66b4f0),clump,5)||memcmp(reinterpret_cast<void*>(0x63b7b0),entry,6)||memcmp(reinterpret_cast<void*>(0x63b900),entry,6)||
       memcmp(reinterpret_cast<void*>(0x63bcd0),shaped,7)||memcmp(reinterpret_cast<void*>(0x5e42a0),blink,7))return false;
    return CompareHook(0x5e2740,actor,6,reinterpret_cast<void*>(&EnemyOpaque),originalEnemyOpaque)&&
        CompareHook(0x5e2790,actor,6,reinterpret_cast<void*>(&EnemyTransparent),originalEnemyTransparent)&&
        CompareHook(0x66b4f0,clump,5,reinterpret_cast<void*>(&EnemyClump),originalEnemyClump)&&
        CompareHook(0x63b7b0,entry,6,reinterpret_cast<void*>(&EnemyEntryFloat),originalEntryFloat)&&
        CompareHook(0x63b900,entry,6,reinterpret_cast<void*>(&EnemyEntryAngle),originalEntryAngle)&&
        CompareHook(0x63bcd0,shaped,7,reinterpret_cast<void*>(&EnemyEntryShaped),originalEntryShaped);
}
ExecFn originalSignalShadow=nullptr,originalRingShadow=nullptr;
bool ObjectModelsEnabled(){return object_shadows::installed&&current.objectShadows&&!shadow_compare::original;}
bool PropShadow(void* actor,int positionOffset,int clumpOffset,float radius){
    if(!ObjectModelsEnabled()||enemy_shadows::drawing)return false;
#ifndef VIDEO_TEST
    auto game=Mem<uintptr_t>(0xa777e4),scene=Mem<uintptr_t>(0xa78d1c);
    if(!game||!scene||Mem<unsigned char>(game+0x20))return false;
    auto address=reinterpret_cast<uintptr_t>(actor);
    auto origin=Mem<enemy_shadows::Vec>(address+positionOffset);
    void* clump=Mem<void*>(address+clumpOffset);if(!clump)return false;
    struct Sphere {enemy_shadows::Vec position;float radius;} sphere{origin,radius};
    auto camera=Mem<void*>(scene+0x28);
    if(!camera||!reinterpret_cast<int(__cdecl*)(void*,const Sphere*)>(0x64ab40)(camera,&sphere))return false;
    auto& contact=object_shadows::Ground(actor,origin);if(!contact.valid)return false;
    enemy_shadows::Caster caster;caster.actor=actor;caster.Add(clump);
    float opacity=std::clamp(1.f-(origin.y-contact.ground.y)/300.f,.25f,1.f);
    return DrawModelShadow(caster,contact.ground,contact.degrees,opacity);
#else
    (void)actor;(void)positionOffset;(void)clumpOffset;(void)radius;return false;
#endif
}
void __fastcall RingShadow(void* actor,void* edx){
    originalRingShadow(actor,edx);
    // +0x60 is the solid booster. +0x64 is its glow, not a solid caster.
    if(PropShadow(actor,0x40,0x60,60.f))++object_shadows::ringDraws;
}
void __fastcall SignalShadow(void* actor,void* edx){
    bool priorSuppression=object_shadows::suppressBlob;
    if(PropShadow(actor,0xbc,0xe0,25.f)){object_shadows::suppressBlob=true;++object_shadows::signalDraws;}
    // Keep the authored colored light pool. Suppress only the old oval when
    // the model silhouette was actually drawn, including unflagged signs.
    originalSignalShadow(actor,edx);object_shadows::suppressBlob=priorSuppression;
}
bool InstallObjectShadows(){
    // SH:FE's SignalFlick fix relocates this routine by 0x14 bytes. Keep its
    // renderer as the chained original instead of rejecting the installed fix.
    constexpr unsigned char signalStart[]={0xa1,0xe4,0x77,0xa7,0,0x83,0xec,0x1c,0x57,0x8b,0xf9,0x8a,0x48,0x20,0x84,0xc9};
    auto signal=Mem<uintptr_t>(0x752c0c);
    if(Mem<uintptr_t>(0x751370)!=0x545ae0||(signal!=0x486d70&&signal!=0x486d84)||
       memcmp(reinterpret_cast<void*>(signal),signalStart,sizeof(signalStart)))return false;
    return Slot(reinterpret_cast<void**>(0x751364),3,&RingShadow,originalRingShadow)&&
        Slot(reinterpret_cast<void**>(0x752c00),3,&SignalShadow,originalSignalShadow);
}
unsigned stoneReceivers=0;
void* originalStoneRender=nullptr;
bool stoneRendering=false;
int __stdcall RestoreReceiverState(int state,DWORD value,uintptr_t material){
    int result=reinterpret_cast<int(__cdecl*)(int,DWORD)>(0x647980)(state,value);
    if(stoneRendering){
        // ShadowRenderFast restores RS_ALPHABLENDENABLE but leaves the last
        // shadow texture bound and RenderWare's texture-alpha cache set. A
        // following cutout then skips enabling blending. Rebind via RenderWare
        // so both its alpha cache and the device follow the base material.
        auto texture=Mem<void*>(material);
        reinterpret_cast<int(__cdecl*)(void*,int)>(0x648f50)(texture,0);
    }
    return result;
}
__declspec(naked) void ReceiverStateBridge(){
    __asm {
        mov eax,[esi-10h]
        push eax
        push dword ptr [esp+0ch]
        push dword ptr [esp+0ch]
        call RestoreReceiverState
        ret
    }
}
void EnsureStoneReceiver(uintptr_t atomic){
    if(!atomic||!current.shadows||enemy_shadows::drawing)return;
    auto effect=Mem<uintptr_t>(0xa75320),pipeline=Mem<uintptr_t>(0x8e2020);
    int plugin=Mem<int>(0x79dad0),objectPlugin=Mem<int>(0x8bbd60);
    if(!effect||!pipeline||plugin<=0||objectPlugin<=0)return;
    // Only the native default atomic renderer is eligible. Preserve skin,
    // material effects and other mods' custom pipelines and callbacks.
    auto existing=Mem<uintptr_t>(atomic+0x6c);
    if((existing&&existing!=pipeline)||Mem<uintptr_t>(atomic+objectPlugin+0x1c)!=0x66add0)return;
    auto geometry=Mem<uintptr_t>(atomic+0x18);if(!geometry)return;
    auto materials=Mem<uintptr_t>(geometry+0x20);int count=Mem<int>(geometry+0x24);
    if(!materials||count<=0||count>128)return;
    std::array<bool,128> opaque{};bool anyOpaque=false,attach=existing!=pipeline;
    int rasterPlugin=Mem<int>(0xaa503c);if(rasterPlugin<=0)return;
    for(int i=0;i<count;++i){
        auto material=Mem<uintptr_t>(materials+i*4);if(!material)return;
        auto texture=Mem<uintptr_t>(material),raster=texture?Mem<uintptr_t>(texture):0;
        // Same texture-alpha metadata tested by RenderWare's SetRaster (648a60).
        // The receiver's extra shadow passes don't sample the base alpha mask,
        // so a cutout flower/foliage quad must not become a solid receiver.
        opaque[i]=Mem<unsigned char>(material+7)==255&&(!raster||!Mem<unsigned char>(raster+rasterPlugin+8));
        anyOpaque|=opaque[i];
        attach|=Mem<int>(material+plugin)!=int(opaque[i])||Mem<uintptr_t>(material+plugin+4)!=(opaque[i]?effect:0);
    }
    if(!anyOpaque){
        // A replacement texture can introduce alpha after a previous attachment.
        if(existing==pipeline){
            Mem<uintptr_t>(atomic+0x6c)=0;
            for(int i=0;i<count;++i){auto material=Mem<uintptr_t>(materials+i*4);Mem<int>(material+plugin)=0;Mem<uintptr_t>(material+plugin+4)=0;}
        }
        return;
    }
    if(attach){
        reinterpret_cast<int(__cdecl*)(void*,void*)>(0x6b7470)(reinterpret_cast<void*>(effect),reinterpret_cast<void*>(atomic));
        for(int i=0;i<count;++i)if(!opaque[i]){
            auto material=Mem<uintptr_t>(materials+i*4);
            Mem<int>(material+plugin)=0;Mem<uintptr_t>(material+plugin+4)=0;
        }
        ++stoneReceivers;
    }
}
void* __cdecl StoneRender(void* atomic){
    // Bind the actual world instance. Template loading alone did not repair
    // the reported surfaces. Keep native distance culling/rendering intact.
    EnsureStoneReceiver(reinterpret_cast<uintptr_t>(atomic));
    bool previousStone=stoneRendering;stoneRendering=true;
    auto result=reinterpret_cast<void*(__cdecl*)(void*)>(originalStoneRender)(atomic);
    stoneRendering=previousStone;return result;
}
bool InstallStoneReceivers(){
    constexpr unsigned char prologue[]={0x51,0xa1,0x60,0xbd,0x8b,0};
    constexpr unsigned char restoreCall[]={0xe8,0xa2,0xfd,0xf8,0xff};
    if(memcmp(reinterpret_cast<void*>(0x42c5d0),prologue,sizeof(prologue))||
       memcmp(reinterpret_cast<void*>(0x6b7bd9),restoreCall,sizeof(restoreCall)))return false;
    unsigned char patch[5]={0xe8};*reinterpret_cast<int32_t*>(patch+1)=int32_t(reinterpret_cast<uintptr_t>(&ReceiverStateBridge)-0x6b7bde);
    if(!Write(reinterpret_cast<void*>(0x6b7bd9),patch,sizeof(patch)))return false;
    return CompareHook(0x42c5d0,prologue,sizeof(prologue),reinterpret_cast<void*>(&StoneRender),originalStoneRender);
}
int Read(const wchar_t* key,int fallback){return int(GetPrivateProfileIntW(L"Video",key,fallback,configPath.c_str()));}
void Load(){
    saveExists=std::filesystem::exists(configPath);if(!saveExists)return;
#define LOAD(f,k) current.f=Read(L##k,current.f)
    LOAD(mode,"WindowMode");LOAD(width,"Width");LOAD(height,"Height");LOAD(refresh,"RefreshRate");LOAD(vsync,"VSync");LOAD(aa,"AntiAliasing");
    LOAD(filter,"Filtering");LOAD(mip,"MipBias");LOAD(fog,"Fog");LOAD(shadows,"DetailedShadows");LOAD(shadowQuality,"ShadowQuality");LOAD(enemyShadows,"EnemyShadows");LOAD(objectShadows,"ObjectShadows");LOAD(selfShadows,"SelfShadows");LOAD(brightness,"Brightness");LOAD(fov,"FieldOfView");
#undef LOAD
    Sanitize(current);current.objectShadows=0;current.selfShadows=0;
}
void Save(){
    std::wstring tmp=configPath+L".tmp";std::ofstream out(tmp,std::ios::trunc);
    out<<"[Video]\nWindowMode="<<current.mode<<"\nWidth="<<current.width<<"\nHeight="<<current.height<<"\nRefreshRate="<<current.refresh<<"\nVSync="<<current.vsync<<"\nAntiAliasing="<<current.aa<<"\nFiltering="<<current.filter<<"\nMipBias="<<current.mip<<"\nFog="<<current.fog<<"\nDetailedShadows="<<current.shadows<<"\nBrightness="<<current.brightness<<"\n";
    out<<"FieldOfView="<<current.fov<<"\nShadowQuality="<<current.shadowQuality<<"\nEnemyShadows="<<current.enemyShadows<<"\nObjectShadows="<<current.objectShadows<<"\nSelfShadows="<<current.selfShadows<<"\n";
    out.close();saveError=!out||!MoveFileExW(tmp.c_str(),configPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(saveError)message="Settings could not be saved. Changes last for this session.";
}
void VisualSettings(){self_shadows::enabled=current.selfShadows&&!shadow_compare::original;enemy_shadows::enabled=current.enemyShadows!=0;Mem<int>(0x8caeb8)=current.fog;Mem<int>(0x8caef0)=!current.shadows;shadow_quality::requested=current.shadows?current.shadowQuality:0;}

std::vector<int> Samples(const Settings& s){
    std::vector<int> result{0};void* d3d=nullptr;if(FAILED(D8<6>(Device(),&d3d)))return result;
    for(int n:{2,4,8})if(SUCCEEDED(D8<11>(d3d,adapter,deviceType,desktop.Format,BOOL(s.mode!=2),D3DMULTISAMPLE_TYPE(n)))&&SUCCEEDED(D8<11>(d3d,adapter,deviceType,Present().depthFormat,BOOL(s.mode!=2),D3DMULTISAMPLE_TYPE(n))))result.push_back(n);
    D8<2>(d3d);return result;
}
std::vector<int> Rates(const Settings& s){std::vector<int> result{0};for(const auto& m:modes)if(int(m.Width)==s.width&&int(m.Height)==s.height&&m.RefreshRate>=50)result.push_back(int(m.RefreshRate));std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;}
void Enumerate(){
    D3DDEVICE_CREATION_PARAMETERS creation{};D8<9>(Device(),&creation);adapter=creation.AdapterOrdinal;deviceType=creation.DeviceType;
    // D3DCAPS8 shares this prefix with D3DCAPS9; storage is larger than either ABI.
    D3DCAPS9 caps{};D8<7>(Device(),&caps);maxAniso=std::max(DWORD(1),caps.MaxAnisotropy);filterCaps=caps.TextureFilterCaps;
    DWORD maxShadow=std::min(caps.MaxTextureWidth,caps.MaxTextureHeight);shadow_quality::maxQuality=maxShadow>=1024?2:maxShadow>=512?1:0;
    void* d3d=nullptr;if(FAILED(D8<6>(Device(),&d3d)))return;
    D8<8>(d3d,adapter,&desktop);
    UINT count=UINT(D8<6>(d3d,adapter));for(UINT i=0;i<count;i++){D3DDISPLAYMODE m{};if(SUCCEEDED(D8<7>(d3d,adapter,i,&m))&&m.Format==desktop.Format&&m.Width>=640&&m.Height>=480&&m.Width<=7680&&m.Height<=4320)modes.push_back(m);}
    D8<2>(d3d);
}
void Call(uintptr_t p){reinterpret_cast<void(__cdecl*)()>(p)();}
bool RestoreResources(){
    auto d=Device();void* surface=nullptr;
    if(FAILED(D8<32>(d,&surface))||!surface)return false;Mem<void*>(0x8e0bf0)=surface;D8<2>(surface);
    surface=nullptr;if(FAILED(D8<33>(d,&surface))||!surface)return false;Mem<void*>(0x8e0bec)=surface;D8<2>(surface);
    for(uintptr_t p:{0x65d9a0u,0x657cd0u,0x657d90u,0x657df0u})if(!reinterpret_cast<int(__cdecl*)()>(p)())return false;
    Call(0x647f40);
    for(uintptr_t p:{0x6576f0u,0x6625b0u,0x664f80u,0x655cf0u})if(!reinterpret_cast<int(__cdecl*)()>(p)())return false;
    return true;
}
void ReleaseResources(){
    self_shadows::Release();
    auto d=Device();if(Mem<int>(0x8e0c14)){originalEnd(d);Mem<int>(0x8e0c14)=0;}
    for(uintptr_t p:{0x799538u,0x79953cu,0x799540u,0x799544u,0x799548u})Mem<DWORD>(p)=0xffffffff;
    Mem<int>(0x8e0c18)=0;Call(0x655db0);memset(reinterpret_cast<void*>(0x8e0ba0),0,0x44);
    for(DWORD s=0;s<8;s++)D8<61>(d,s,static_cast<void*>(nullptr));
    D8<83>(d,UINT(0),static_cast<void*>(nullptr),UINT(0));D8<85>(d,static_cast<void*>(nullptr),UINT(0));
    for(uintptr_t p:{0x662670u,0x664f60u,0x657d40u,0x6576b0u,0x655c90u,0x657660u,0x65d920u,0x64a6c0u})Call(p);
}
void RestoreWindow(){
    SetWindowLongW(Window(),GWL_STYLE,previousStyle);SetWindowLongW(Window(),GWL_EXSTYLE,previousExStyle);
    SetWindowPos(Window(),HWND_NOTOPMOST,previousRect.left,previousRect.top,previousRect.right-previousRect.left,previousRect.bottom-previousRect.top,SWP_FRAMECHANGED|SWP_NOACTIVATE);
    Mem<int>(0x8caedc)=previous.mode==2;
}
bool PlaceWindow(Settings& s){
    HWND w=Window();MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi))return false;
    LONG style=s.mode==0?windowedStyle:(WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS);
    LONG ex=GetWindowLongW(w,GWL_EXSTYLE)&~(WS_EX_TOPMOST|WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE);
    int width=s.width,height=s.height;
    if(s.mode==1){width=mi.rcMonitor.right-mi.rcMonitor.left;height=mi.rcMonitor.bottom-mi.rcMonitor.top;}
    RECT rect{0,0,width,height};if(s.mode==0)AdjustWindowRectEx(&rect,DWORD(style),FALSE,DWORD(ex));
    int x=mi.rcMonitor.left,y=mi.rcMonitor.top;
    if(s.mode==0){x=mi.rcWork.left+std::max(0L,(mi.rcWork.right-mi.rcWork.left-(rect.right-rect.left))/2);y=mi.rcWork.top+std::max(0L,(mi.rcWork.bottom-mi.rcWork.top-(rect.bottom-rect.top))/2);}
    SetWindowLongW(w,GWL_STYLE,style);SetWindowLongW(w,GWL_EXSTYLE,ex);
    return SetWindowPos(w,HWND_NOTOPMOST,x,y,rect.right-rect.left,rect.bottom-rect.top,SWP_FRAMECHANGED|SWP_NOACTIVATE)!=FALSE;
}
bool ResetDisplay(Settings target,bool reverting){
    if(!reverting){previous=current;previousPresent=Present();GetWindowRect(Window(),&previousRect);previousStyle=GetWindowLongW(Window(),GWL_STYLE);previousExStyle=GetWindowLongW(Window(),GWL_EXSTYLE);}
    Sanitize(target);
    if(target.mode==2){bool supported=false;for(const auto& m:modes)if(int(m.Width)==target.width&&int(m.Height)==target.height&&(target.refresh==0||int(m.RefreshRate)==target.refresh))supported=true;if(!supported){message="This fullscreen mode is unavailable. Choose another resolution.";return false;}}
    auto samples=Samples(target);if(std::find(samples.begin(),samples.end(),target.aa)==samples.end()){message="This anti-aliasing level is unavailable in the selected mode.";return false;}
    // Do not retain a game-owned default-pool texture across RenderWare's reset.
    art::ClearNative();ReleaseResources();
    if(reverting)RestoreWindow();else if(!PlaceWindow(target)){RestoreWindow();message="Windows could not change the display mode.";target=previous;}
    Present8 pp=reverting?previousPresent:Present();
    if(!reverting){
        RECT client{};GetClientRect(Window(),&client);pp.width=UINT(target.mode==2?target.width:client.right);pp.height=UINT(target.mode==2?target.height:client.bottom);
        pp.window=Window();pp.windowed=target.mode!=2;pp.format=desktop.Format;pp.count=target.mode==2&&target.vsync?2:1; // fullscreen V-Sync: room to queue interpolated imagespp.samples=D3DMULTISAMPLE_TYPE(target.aa);
        pp.swap=target.aa||target.mode==2?D3DSWAPEFFECT_DISCARD:D3DSWAPEFFECT(target.vsync?4:3);
        pp.refresh=target.mode==2?DWORD(target.refresh):0;pp.interval=target.mode==2?(target.vsync?1:0x80000000u):0;
        if(target.aa)pp.flags&=~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    }
    Present8 requested=pp;HRESULT hr=D8<14>(Device(),&pp);
    // Exiting exclusive fullscreen restores the desktop mode. Size borderless
    // against that restored monitor, not against the old exclusive resolution.
    if(SUCCEEDED(hr)&&!reverting&&target.mode!=2){
        PlaceWindow(target);RECT client{};GetClientRect(Window(),&client);
        if(requested.width!=UINT(client.right)||requested.height!=UINT(client.bottom)){
            requested.width=UINT(client.right);requested.height=UINT(client.bottom);pp=requested;hr=D8<14>(Device(),&pp);
        }
    }
    bool accepted=SUCCEEDED(hr);
    if(!accepted){RestoreWindow();requested=previousPresent;pp=requested;hr=D8<14>(Device(),&pp);target=previous;message="Display change failed. Previous settings restored.";}
    if(FAILED(hr)){Present()=previousPresent;message="Display device unavailable. Return focus to the game to recover.";return false;}
    // Reset may zero the caller's windowed dimensions: retain the requested mode for RenderWare.
    Present()=requested;Mem<int>(0x8caedc)=target.mode==2;
    if(!RestoreResources()){message="The renderer could not restore all resources. Restart the game.";return false;}
    current=target;edit=current;VisualSettings();
    if(accepted&&!reverting&&!startup){confirmation.Begin(GetTickCount64());message.clear();}
    if(reverting){confirmation.End();message="Previous display settings restored.";}
    return accepted;
}

struct Samplers {
    void* device;DWORD saved[2][5]{};bool active=false;
    Samplers(void* d):device(d){
        if(d!=Device()||ui::drawing||(!current.filter&&!current.mip))return;
        DWORD shader=0;D8<77>(d,&shader);
        if((shader&D3DFVF_POSITION_MASK)==D3DFVF_XYZRHW)return;
        active=true;DWORD kinds[]={16,17,18,19,21};
        for(DWORD s=0;s<2;s++){
            for(int i=0;i<5;i++)D8<62>(d,s,kinds[i],&saved[s][i]);
            if(current.filter){DWORD min=current.filter>=3?D3DTEXF_ANISOTROPIC:D3DTEXF_LINEAR;DWORD aniso=current.filter>=3?DWORD(1<<(current.filter-2)):1;
                TS(d,s,16,D3DTEXF_LINEAR);TS(d,s,17,min);TS(d,s,18,current.filter>=2?D3DTEXF_LINEAR:D3DTEXF_POINT);TS(d,s,21,std::min(aniso,maxAniso));}
            float bias=current.mip*.25f;DWORD bits;memcpy(&bits,&bias,4);TS(d,s,19,bits);
        }
    }
    ~Samplers(){if(active){DWORD kinds[]={16,17,18,19,21};for(DWORD s=0;s<2;s++)for(int i=0;i<5;i++)TS(device,s,kinds[i],saved[s][i]);}}
};
float NativeOpacity(void* d){
    if(ui::drawing||d!=Device()||(!rootVisible&&!audioOpen)||GetTickCount64()-lastMenu>=200)return 1;
    void* texture=nullptr;if(FAILED(D8<60>(d,DWORD(0),&texture))||!texture)return 1;
    art::ObservePrompt(texture);
    bool title=std::find(art::nativeTitles.begin(),art::nativeTitles.end(),texture)!=art::nativeTitles.end();
    bool foreground=std::find(art::foreground.begin(),art::foreground.end(),texture)!=art::foreground.end();
    D8<2>(texture);
    if(audioOpen)return (texture==art::unsupportedAudio[0]||texture==art::unsupportedAudio[1])?0.f:1.f;
    if(focused&&(title||texture==art::selectionGlow))return 0;
    return foreground?RootOpacity(pageMotion.At(GetTickCount64())):1;
}
// The selected lit hex REPLACES the unlit hex in the native scene. Keep its
// geometry but borrow the idle texture when TV owns selection; hiding it would
// leave the previous button without a frame.
struct NativeReplacement {
    void* d;void* texture=nullptr;
    explicit NativeReplacement(void* device):d(device){
        if(ui::drawing||d!=Device()||!focused||!rootVisible||!art::hex.texture||GetTickCount64()-lastMenu>=200)return;
        void* bound=nullptr;if(FAILED(D8<60>(d,DWORD(0),&bound))||!bound)return;
        if(bound==art::litHex.texture){texture=bound;D8<61>(d,DWORD(0),art::hex.texture);}else D8<2>(bound);
    }
    ~NativeReplacement(){if(texture){D8<61>(d,DWORD(0),texture);D8<2>(texture);}}
};
struct NativeFade {
    void* device;bool active;DWORD stage[6]{},state[5]{};
    static constexpr DWORD stages[]={1,2,3,4,5,6};
    static constexpr D3DRENDERSTATETYPE states[]={D3DRS_TEXTUREFACTOR,D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_ALPHATESTENABLE};
    NativeFade(void* d,float alpha):device(d),active(alpha<1){
        if(!active)return;
        for(int i=0;i<6;i++)D8<62>(d,DWORD(1),stages[i],&stage[i]);
        for(int i=0;i<5;i++)D8<51>(d,states[i],&state[i]);
        // Add a final alpha modulation to the native sprite pipeline. Keep its
        // texture/diffuse alpha and restore every touched state after this draw.
        TS(d,1,1,D3DTOP_SELECTARG1);TS(d,1,2,D3DTA_CURRENT);TS(d,1,3,D3DTA_CURRENT);
        TS(d,1,4,D3DTOP_MODULATE);TS(d,1,5,D3DTA_CURRENT);TS(d,1,6,D3DTA_TFACTOR);
        RS(d,D3DRS_TEXTUREFACTOR,(DWORD(alpha*255)<<24)|0xffffff);RS(d,D3DRS_ALPHABLENDENABLE,TRUE);
        RS(d,D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);RS(d,D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);RS(d,D3DRS_ALPHATESTENABLE,FALSE);
    }
    ~NativeFade(){if(active){for(int i=0;i<6;i++)TS(device,1,stages[i],stage[i]);for(int i=0;i<5;i++)RS(device,states[i],state[i]);}}
};
// Character shadows on walls: the native receiver pass smears them into
// vertical stripes. Original-shadow comparison keeps the native result.
const wall_shadows::Entry* ReceiverPlan(void* d,D3DPRIMITIVETYPE t,bool indexed,UINT min,UINT n,UINT first,UINT count){
    if(d!=Device()||!current.shadows||shadow_compare::original||ui::drawing)return nullptr;
    return wall_shadows::Prepare(d,t,indexed,min,n,first,count);
}
void CaptureRetained(void* d);
HRESULT __stdcall Draw(void* d,D3DPRIMITIVETYPE t,UINT first,UINT count){if(enemy_shadows::probing)return D3D_OK;if(self_shadows::pass){if(!self_shadows::EligibleDraw(d))return D3D_OK;self_shadows::DrawState state(d);return originalDraw(d,t,first,count);}enemy_shadows::ObserveDraw(d);if(enemy_shadows::drawing){enemy_shadows::DrawScope scope(d);return originalDraw(d,t,first,count);}CaptureRetained(d);float alpha=NativeOpacity(d);if(alpha<=0)return D3D_OK;NativeFade fade(d,alpha);NativeReplacement replacement(d);Samplers scoped(d);if(auto plan=ReceiverPlan(d,t,false,0,0,first,count))return wall_shadows::Submit(d,*plan,false,0,0,originalIndexed);return originalDraw(d,t,first,count);}
HRESULT __stdcall Indexed(void* d,D3DPRIMITIVETYPE t,UINT min,UINT n,UINT start,UINT count){if(enemy_shadows::probing)return D3D_OK;if(self_shadows::pass){if(!self_shadows::EligibleDraw(d))return D3D_OK;self_shadows::DrawState state(d);return originalIndexed(d,t,min,n,start,count);}enemy_shadows::ObserveDraw(d);if(enemy_shadows::drawing){enemy_shadows::DrawScope scope(d);return originalIndexed(d,t,min,n,start,count);}CaptureRetained(d);float alpha=NativeOpacity(d);if(alpha<=0)return D3D_OK;NativeFade fade(d,alpha);NativeReplacement replacement(d);Samplers scoped(d);if(auto plan=ReceiverPlan(d,t,true,min,n,start,count))return wall_shadows::Submit(d,*plan,true,min,n,originalIndexed);return originalIndexed(d,t,min,n,start,count);}
HRESULT __stdcall DrawUP(void* d,D3DPRIMITIVETYPE t,UINT count,const void* vertices,UINT stride){if(enemy_shadows::probing)return D3D_OK;if(self_shadows::pass){if(!self_shadows::EligibleDraw(d))return D3D_OK;self_shadows::DrawState state(d);return originalDrawUP(d,t,count,vertices,stride);}enemy_shadows::ObserveDraw(d);if(enemy_shadows::drawing){enemy_shadows::DrawScope scope(d);return originalDrawUP(d,t,count,vertices,stride);}float alpha=NativeOpacity(d);if(alpha<=0)return D3D_OK;NativeFade fade(d,alpha);NativeReplacement replacement(d);Samplers scoped(d);return originalDrawUP(d,t,count,vertices,stride);}
HRESULT __stdcall IndexedUP(void* d,D3DPRIMITIVETYPE t,UINT min,UINT n,UINT count,const void* indices,D3DFORMAT format,const void* vertices,UINT stride){if(enemy_shadows::probing)return D3D_OK;if(self_shadows::pass){if(!self_shadows::EligibleDraw(d))return D3D_OK;self_shadows::DrawState state(d);return originalIndexedUP(d,t,min,n,count,indices,format,vertices,stride);}enemy_shadows::ObserveDraw(d);if(enemy_shadows::drawing){enemy_shadows::DrawScope scope(d);return originalIndexedUP(d,t,min,n,count,indices,format,vertices,stride);}float alpha=NativeOpacity(d);if(alpha<=0)return D3D_OK;NativeFade fade(d,alpha);NativeReplacement replacement(d);Samplers scoped(d);return originalIndexedUP(d,t,min,n,count,indices,format,vertices,stride);}
bool IsNativeHexDraw(){
#ifndef VIDEO_TEST
    if(!ui::drawing&&rootVisible&&GetTickCount64()-lastMenu<200&&art::hex.texture){
        void* raster=nullptr;using GetState=int(__cdecl*)(int,void*);
        if(reinterpret_cast<GetState>(0x64ca00)(1,&raster)&&raster){
            int offset=Mem<int>(0xaa503c);
            return offset>=0&&offset<=4096&&(Mem<void*>(reinterpret_cast<uintptr_t>(raster)+offset)==art::hex.texture||Mem<void*>(reinterpret_cast<uintptr_t>(raster)+offset)==art::litHex.texture);
        }
    }
#endif
    return false;
}
void CaptureButton(const native_menu::Hexes& captured){
    if(!captured.count)return;
    int index=-1;for(int i=0;i<4;i++)if(renderingShape==buttonShapes[i]&&renderingShape)index=i;
    if(renderingShape&&renderingShape==buttonShapes[4])index=nativeChoice;
    if(index<0||index>3)return;
    const auto& p=captured.items[0];
    buttonPoses[index]={true,p.ax/1.125f,p.ay/1.125f,p.bx/1.125f,p.by/1.125f,p.x,p.y,rootX,rootY,p.tint};
}
void CaptureRetained(void* d){
    if(ui::drawing||d!=Device()||!rootVisible||!renderingShape||GetTickCount64()-lastMenu>=200)return;
    if(std::find(buttonShapes.begin(),buttonShapes.end(),renderingShape)==buttonShapes.end())return;
    void* texture=nullptr;if(FAILED(D8<60>(d,DWORD(0),&texture))||!texture)return;
    bool hex=texture==art::hex.texture||texture==art::litHex.texture;D8<2>(texture);if(!hex)return;
    DWORD fvf=0;if(FAILED(D8<77>(d,&fvf))||(fvf&D3DFVF_POSITION_MASK)!=D3DFVF_XYZ)return;
    auto shape=reinterpret_cast<uintptr_t>(renderingShape);
    // __rt2dShapeRenderChild's retained branch (0x6b1e8e) takes RpGeometry
    // from shape->representation+8. The CPU morph vertices/UVs stay available;
    // do not lock a write-only GPU buffer or mutate the game's retained mesh.
    if(!Mem<uintptr_t>(shape+0xb4))return;
    auto representation=Mem<uintptr_t>(shape+0xac);if(!representation)return;
    auto geometry=Mem<uintptr_t>(representation+8);if(!geometry)return;
    int n=Mem<int>(geometry+0x14);if(n<3||n>4096||Mem<int>(geometry+0x18)<1||Mem<int>(geometry+0x1c)<1)return;
    auto morph=Mem<uintptr_t>(geometry+0x5c);auto uv=Mem<const float*>(geometry+0x34);if(!morph||!uv)return;
    auto positions=Mem<const float*>(morph+0x14);if(!positions)return;
    D3DMATRIX world{},view{},projection{};D3DVIEWPORT9 viewport{};
    if(FAILED(D8<38>(d,D3DTS_WORLD,&world))||FAILED(D8<38>(d,D3DTS_VIEW,&view))||FAILED(D8<38>(d,D3DTS_PROJECTION,&projection))||FAILED(D8<41>(d,&viewport)))return;
    ui::Vertex first{},a{},b{};if(!native_menu::Project(positions,uv,world,view,projection,viewport,renderingTint,first))return;
    for(int i=1;i+1<n;i++){
        if(!native_menu::Project(positions+i*3,uv+i*2,world,view,projection,viewport,renderingTint,a)||!native_menu::Project(positions+(i+1)*3,uv+(i+1)*2,world,view,projection,viewport,renderingTint,b))continue;
        native_menu::Hexes captured;if(native_menu::Pose(first,a,b,captured.items[0])){captured.count=1;CaptureButton(captured);return;}
    }
}
void* __cdecl ObserveShape(void* shape,void* mult,void* add){
    void* previousShape=renderingShape;DWORD previousTint=renderingTint;renderingShape=shape;
    auto bytes=static_cast<char*>(shape);
    renderingTint=native_menu::ShapeTint(reinterpret_cast<float*>(bytes+0x8c),reinterpret_cast<float*>(bytes+0x9c),static_cast<float*>(mult),static_cast<float*>(add));
    void* result=originalShape(shape,mult,add);renderingShape=previousShape;renderingTint=previousTint;return result;
}
int __cdecl ObserveNativeDraw(int type,ui::Vertex* vertices,int count){
    if(IsNativeHexDraw()){native_menu::Hexes captured;captured.Observe(type,vertices,count);CaptureButton(captured);nativeHexes.Observe(type,vertices,count);}
    // The driver is downstream of both widescreen and ScreenScaleFix. Observe
    // their final vertices, then preserve their chain and caller-owned buffers.
    return originalRwDraw(type,vertices,count);
}
int __cdecl ObserveNativeIndexed(int type,ui::Vertex* vertices,int count,unsigned short* indices,int total){
    if(IsNativeHexDraw()){native_menu::Hexes captured;captured.ObserveIndexed(type,vertices,count,indices,total);CaptureButton(captured);nativeHexes.ObserveIndexed(type,vertices,count,indices,total);}
    return originalRwIndexed(type,vertices,count,indices,total);
}

const char* labels[][8]={{"Window type","Resolution","Refresh rate","V-Sync","Anti-aliasing","FPS","Apply display changes"},{"Texture filtering","Mipmap detail","Distance fog","Character shadows","Field of view","Enemy shadows","Object shadows","Self shadows"},{"Brightness","FPS display"}};
const char* Label(int t,int r){return t==0&&r==DisplayActionRow()?"Apply display changes":labels[t][r];}
std::string Value(int t,int r){
    if(t==0&&r==DisplayActionRow())return DisplayChanged(edit,current)?"Preview":"Saved";
    if(motionApi&&((t==0&&r==5)||(t==2&&r>0))){
        motion::Settings s;motionApi->get(&s);
        if(t==0)return s.targetHz?std::to_string(s.targetHz):"Unlocked";
        return s.showFps?"On":"Off";
    }
    if(t==0)switch(r){case 0:return std::array<const char*,3>{"Windowed","Borderless","Fullscreen"}[edit.mode];case 1:return edit.mode==1?"Desktop":std::to_string(edit.width)+" x "+std::to_string(edit.height);case 2:return edit.mode!=2?"Desktop":edit.refresh?std::to_string(edit.refresh)+" Hz":"Default";case 3:return edit.mode!=2&&edit.aa?"Desktop":edit.vsync?"On":"Off";case 4:return edit.aa?std::to_string(edit.aa)+"x MSAA":"Off";}
    if(t==1)switch(r){case 0:return std::array<const char*,7>{"Game default","Bilinear","Trilinear","2x Anisotropic","4x Anisotropic","8x Anisotropic","16x Anisotropic"}[current.filter];case 1:{char b[32];sprintf_s(b,"%+.2f",current.mip*.25f);return b;}case 2:return current.fog?"On":"Off";case 3:return !current.shadows?"Simple":current.shadowQuality&&!shadow_quality::compatible?"Unavailable":std::array<const char*,3>{"Original","High (512)","Ultra (1024)"}[current.shadowQuality];case 4:return current.fov==100?"100% (Default)":std::to_string(current.fov)+"%";case 5:return !enemy_shadows::installed?"Unavailable":current.enemyShadows?"Animated model":"Original";case 6:case 7:return "Disabled";}
    if(t==2&&r==0)return std::to_string(50+current.brightness*5)+"%";
    return {};
}
// The FPS mod presents only native frames on a synchronized swap chain. Explain
// that on the Display page instead of leaving a higher target silently at 60.
std::string FpsNote(){
    if(!motionApi)return {};
    motion::Settings s;motionApi->get(&s);if(!s.enabled)return {};
    motion::Stats stats;motionApi->stats(&stats);
    if(stats.status==motion::Incompatible)return "FPS interpolation could not start with this graphics setup.";
    const auto& shown=DisplayChanged(edit,current)?edit:current;
    if(shown.vsync&&(shown.mode==2||!shown.aa))return "V-Sync is on, so FPS stays at 60. Turn V-Sync off to raise it.";
    return {};
}
void AdjustShadowChoice(int direction){
    std::vector<int> choices{-1,0};if(shadow_quality::compatible)for(int q=1;q<=shadow_quality::maxQuality;++q)choices.push_back(q);
    int chosen=Cycle(choices,current.shadows?current.shadowQuality:-1,direction);current.shadows=chosen>=0;current.shadowQuality=std::max(chosen,0);
}
void Adjust(int direction){
    if((tab==0&&row==DisplayActionRow())||RowDisabled(tab,row))return;
    std::string before=Value(tab,row);
    if(motionApi&&((tab==0&&row==5)||(tab==2&&row>0))){
        motion::Settings s;motionApi->get(&s);
        if(tab==0){std::vector<uint32_t> rates(std::begin(motion::FpsOptions),std::end(motion::FpsOptions));s.targetHz=Cycle(rates,s.targetHz,direction);s.enabled=s.targetHz!=60;}
        else s.showFps=!s.showFps;
        message.clear();if(!motionApi->set(&s))message="FPS settings could not be saved.";else if(before!=Value(tab,row))Sound();return;
    }
    message.clear();if(tab==0){
        switch(row){
        case 0:{edit.mode=(edit.mode+direction+3)%3;edit.refresh=0;
            if(edit.mode==2&&!modes.empty()){
                auto nearest=std::min_element(modes.begin(),modes.end(),[](const auto& a,const auto& b){
                    return abs(int(a.Width)-edit.width)+abs(int(a.Height)-edit.height)<abs(int(b.Width)-edit.width)+abs(int(b.Height)-edit.height);});
                edit.width=int(nearest->Width);edit.height=int(nearest->Height);
            }break;}
        case 1:{if(edit.mode==1)return;std::vector<std::pair<int,int>> sizes;for(const auto& m:modes)sizes.emplace_back(m.Width,m.Height);sizes.emplace_back(current.width,current.height);std::sort(sizes.begin(),sizes.end());sizes.erase(std::unique(sizes.begin(),sizes.end()),sizes.end());auto chosen=Cycle(sizes,std::pair<int,int>{edit.width,edit.height},direction);edit.width=chosen.first;edit.height=chosen.second;edit.refresh=0;break;}
        case 2:if(edit.mode==2)edit.refresh=Cycle(Rates(edit),edit.refresh,direction);break;
        case 3:if(edit.mode==2||!edit.aa)edit.vsync=!edit.vsync;break;
        case 4:edit.aa=Cycle(Samples(edit),edit.aa,direction);break;
        default:break;
        }
        auto samples=Samples(edit);if(std::find(samples.begin(),samples.end(),edit.aa)==samples.end())edit.aa=0;
    }else{
        if(tab==1)switch(row){case 0:{std::vector<int> filters{0,1,2};if(filterCaps&D3DPTFILTERCAPS_MINFANISOTROPIC)for(int i=3;i<=6;i++)if(DWORD(1<<(i-2))<=maxAniso)filters.push_back(i);current.filter=Cycle(filters,current.filter,direction);break;}case 1:current.mip=std::clamp(current.mip+direction,-4,4);break;case 2:current.fog=!current.fog;break;case 3:AdjustShadowChoice(direction);break;case 4:current.fov=std::clamp(current.fov+direction*5,50,150);break;case 5:if(enemy_shadows::installed)current.enemyShadows=!current.enemyShadows;break;case 6:if(object_shadows::installed)current.objectShadows=!current.objectShadows;break;case 7:if(!self_shadows::installed)break;current.selfShadows=!current.selfShadows;self_shadows::enabled=current.selfShadows;break;}
        else current.brightness=std::clamp(current.brightness+direction,-10,10);
        // Preserve staged display choices when changing other tabs.
        edit.filter=current.filter;edit.mip=current.mip;edit.fog=current.fog;edit.shadows=current.shadows;edit.shadowQuality=current.shadowQuality;edit.enemyShadows=current.enemyShadows;edit.objectShadows=current.objectShadows;edit.selfShadows=current.selfShadows;edit.brightness=current.brightness;edit.fov=current.fov;
        VisualSettings();Save();
    }
    if(before!=Value(tab,row))Sound();
}
// sGamePeri +0x44 is the D-pad held word plus left-stick bits past the
// native +/-32 threshold (up 0x1000000, down 0x2000000, left 0x4000000,
// right 0x8000000), the same word native menus test via +0x48.
int PadDirection(DWORD held){
    if(held&0x1000010)return 1;if(held&0x2000020)return 2;
    if(held&0x4000040)return 4;if(held&0x8000080)return 8;return 0;
}
DWORD Input(){
    if(GetForegroundWindow()!=Window()){lastDirection=0;return 0;}
    int player=std::clamp(Mem<int>(0x7c7418),0,3);uintptr_t pad=0xa236c0+player*0x4c;
    DWORD held=Mem<DWORD>(pad)|Mem<DWORD>(pad+0x44),pressed=Mem<DWORD>(pad+8),result=0;
    bool keyboardDown=false;for(int k=VK_BACK;k<256;k++)if(GetAsyncKeyState(k)&0x8000){keyboardDown=true;break;}
    if(keyboardDown)keyboardHints=true;else if(pressed)keyboardHints=false;
    if(pressed&8)result|=512;if(pressed&0x4001)result|=16;if(pressed&6)result|=32;if(pressed&0x100)result|=128;if(pressed&0x200)result|=256;
    static std::array<bool,256> keys{};auto key=[&](int k){bool down=(GetAsyncKeyState(k)&0x8000)!=0,edge=down&&!keys[k];keys[k]=down;return edge;};
    if(key(VK_RETURN))result|=16;if(key(VK_ESCAPE))result|=32;
    if(key(VK_PRIOR)||key('Q'))result|=256;if(key(VK_NEXT)||key('E'))result|=128;
    int dir=PadDirection(held);
    if(!dir){if(GetAsyncKeyState(VK_UP)&0x8000)dir=1;else if(GetAsyncKeyState(VK_DOWN)&0x8000)dir=2;else if(GetAsyncKeyState(VK_LEFT)&0x8000)dir=4;else if(GetAsyncKeyState(VK_RIGHT)&0x8000)dir=8;}
    auto now=GetTickCount64();if(dir!=lastDirection){lastDirection=dir;repeatAt=now+350;result|=dir;}else if(dir&&now>=repeatAt){result|=dir;repeatAt=now+110;}
    return result;
}
void Mouse(DWORD& input){
    static bool wasDown=false,wasBack=false;
    bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0,back=(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
    bool click=down&&!wasDown,backClick=back&&!wasBack;wasDown=down;wasBack=back;
    if(GetForegroundWindow()!=Window())return;
    if(click||backClick)keyboardHints=true;
    if(backClick)input|=32;
    POINT p{};RECT rc{};GetCursorPos(&p);ScreenToClient(Window(),&p);GetClientRect(Window(),&rc);
    static POINT lastPoint{};bool moved=p.x!=lastPoint.x||p.y!=lastPoint.y;lastPoint=p;
    float scale=std::min(rc.right/960.0f,rc.bottom/540.0f);
    if(scale<=0)return;
    float x=(p.x-(rc.right-960*scale)*.5f)/scale,y=(p.y-(rc.bottom-540*scale)*.5f)/scale;
    if(!open){
        int hit=-1;
        for(int i=0;i<4;i++)if(native_menu::Hit(buttonPoses[i],float(p.x),float(p.y)))hit=i;
        if(native_menu::Hit(rootPose,float(p.x),float(p.y)))hit=4;
        if(hit>=0&&(moved||click))input|=DWORD(hit+1)<<16;
        if(hit>=0&&click)input|=16;
        return;
    }
    if(confirmation.active){if(click&&x>=222&&x<=738&&y>=295&&y<=345)input|=x<480?16:32;return;}
    if(click&&y>=333&&y<333+TabStep()*TabCount()&&x>=161&&x<=347){int next=std::clamp(int((y-333)/TabStep()),0,TabCount()-1);if(tab!=next){tab=next;row=0;Sound();}return;}
    if((moved||click)&&x>=405&&x<=849&&y>=164&&y<164+RowStep(tab)*SelectableRows(tab)){
        int next=std::clamp(int((y-164)/RowStep(tab)),0,SelectableRows(tab)-1);if(RowDisabled(tab,next))return;
        if(next!=row){row=next;Sound();}
        if(click){if(tab==0&&row==DisplayActionRow())input|=16;else input|=x<626?4:8;}
    }
}
bool RootShown(void* object){
    auto p=static_cast<char*>(object);int state=*reinterpret_cast<int*>(p+0x28),next=*reinterpret_cast<int*>(p+0x2c),target=*reinterpret_cast<int*>(p+0x60);
    return next==5||state==5||((state==1||next==1)&&target==6)||(state==6&&next==6);
}
using FindLabelFn=void*(__cdecl*)(void*,int,const char*,int*);
FindLabelFn findLabel=reinterpret_cast<FindLabelFn>(0x6b51a0);
void EvaluateNative(void* animation,float frame){
#ifndef VIDEO_TEST
    // Verified MAESTRO_DATA_ADV::Exec ABI: ESI=this, two floats, ret 8.
    __asm {
        push esi
        mov esi,animation
        push 3f800000h
        push frame
        mov eax,44c8e0h
        call eax
        pop esi
    }
#else
    (void)animation;(void)frame;
#endif
}
using EvaluateFn=void(*)(void*,float);
EvaluateFn evaluateNative=&EvaluateNative;
template<class T> T& Field(void* object,int offset){return *reinterpret_cast<T*>(static_cast<char*>(object)+offset);}
float LabelFrame(void* object,const char* name){
    void* animation=Field<void*>(object,0x38);if(!animation)return -1;
    void* maestro=Field<void*>(animation,12);if(!maestro)return -1;
    void* label=findLabel(maestro,2,name,nullptr);if(!label)return -1;
    int index=Field<int>(label,8);return index>=0&&index<100000?float(index+1):-1;
}
void EvaluateFrame(void* object,float frame){Field<float>(object,0x34)=frame;evaluateNative(Field<void*>(object,0x38),frame);}
const char* RootLabel(int choice){static const char* names[]={"audio","vib","file","lang"};return names[std::clamp(choice,0,3)];}
void BindButtons(void* object){
    buttonShapes.fill(nullptr);
    if(!art::fixedOptions)return;
    auto animation=Field<uintptr_t>(object,0x38);if(!animation)return;
    auto maestro=Mem<uintptr_t>(animation+12);if(!maestro)return;
    auto scene=Mem<uintptr_t>(maestro+0x24);if(!scene||Mem<int>(scene)!=1)return;
    auto list=Mem<uintptr_t>(scene+0xac);if(!list)return;
    // Installed FE OPTION.ANM: 43 inline root objects; the four hex scenes
    // are 2/4/7/10 and the reusable selection glow is 13. Validate the topology
    // and shared hex shape before using identity (never animated position).
    if(Mem<int>(list+4)!=43||Mem<int>(list+12)!=0x100)return;
    auto base=Mem<uintptr_t>(list);if(!base)return;
    constexpr int children[]={2,4,7,10,13};uintptr_t shared=0;
    std::array<void*,5> result{};
    for(int i=0;i<5;i++){
        uintptr_t node=base+children[i]*0x100;if(Mem<int>(node)!=1)return;
        uintptr_t nested=Mem<uintptr_t>(node+0xac);if(!nested||Mem<int>(nested+4)!=1||Mem<int>(nested+12)!=0x100)return;
        uintptr_t shape=Mem<uintptr_t>(nested);if(!shape||Mem<int>(shape)!=2)return;
        if(i<4){uintptr_t rep=Mem<uintptr_t>(shape+0xac);if(!rep||(shared&&rep!=shared))return;shared=rep;}
        result[i]=reinterpret_cast<void*>(shape);
    }
    buttonShapes=result;
}
void SelectRoot(void* object,int choice){
    rootChoice=choice;focused=choice==4;
    if(!focused){
        nativeChoice=choice;Field<int>(object,0x94)=choice;
        int count=Field<int>(object,0x34c);
        if(count>0&&count<=32)for(int i=0;i<count;i++)if(Field<int>(object,0x2cc+i*4)==choice){Field<int>(object,0x350)=i;break;}
    }
    // Reset the PREVIOUS selected loop to its first settled pose, not to the
    // unfinished entrance. Suppression removes its glow/title while TV owns focus.
    animationFrame=LabelFrame(object,RootLabel(nativeChoice));
    if(animationFrame>0)EvaluateFrame(object,animationFrame);
}
void StepRoot(void* object){
    float first=LabelFrame(object,RootLabel(nativeChoice));
    std::string end=std::string(RootLabel(nativeChoice))+"ed";float last=LabelFrame(object,end.c_str());
    if(first<0||last<first)return;
    if(!focused){animationFrame+=1;if(animationFrame<first||animationFrame>last)animationFrame=first;}
    EvaluateFrame(object,focused?first:animationFrame);
}
void BeginNativePage(void* object,int choice){
    Field<int>(object,0x94)=choice;
    Field<int>(object,0x2c)=1;Field<int>(object,0x60)=choice==2?2:choice+7;
    Field<int>(object,0x6c)=choice==2?0:1;
    if(choice==2){Field<int>(object,0x64)=23;Field<int>(object,0x68)=24;Field<int>(object,0x44)=8;}
    else {Field<int>(object,0x64)=Mem<int>(0x743d3c+choice*8);Field<int>(object,0x68)=Mem<int>(0x743d40+choice*8);}
    rootVisible=false;rootPose={};
}
void AudioFrame(void* object,DWORD input){
    rootVisible=audioMotion<0;rootEntering=rootVisible;
    if(audioMotion){
        const char* start=audioMotion>0?"audio2":"audio2ed";
        const char* end=audioMotion>0?"audio21":"audio2eded";
        float first=LabelFrame(object,start),last=LabelFrame(object,end);
        if(first<0||last<first){audioOpen=false;audioMotion=0;SelectRoot(object,0);return;}
        animationFrame=std::min(animationFrame+1,last);EvaluateFrame(object,animationFrame);
        if(animationFrame>=last){if(audioMotion<0){audioOpen=false;rootEntering=false;SelectRoot(object,0);}audioMotion=0;}
        return;
    }
    if(input&(1|4)){audioChoice=1-audioChoice;Sound();}
    if(input&(2|8)){audioChoice=1-audioChoice;Sound();}
    if(input&(16|32)){
        if(input&16){
#ifndef VIDEO_TEST
            auto config=Mem<uintptr_t>(0xa777e4);
            if(config){Mem<unsigned char>(config+0x12)=static_cast<unsigned char>(audioChoice);Field<int>(object,0x84)=audioChoice;reinterpret_cast<void(__cdecl*)(int)>(0x43ef60)(audioChoice);}
#endif
            Sound(0xe008);
        }else Sound(0xe009);
        audioMotion=-1;rootVisible=true;rootEntering=true;animationFrame=LabelFrame(object,"audio2ed");EvaluateFrame(object,animationFrame);return;
    }
    // The PC audio backend implements stereo/mono, not console Dolby Pro Logic II.
    const char* selections[]={"audio21","audio22"};
    EvaluateFrame(object,LabelFrame(object,selections[audioChoice]));
}
void UpdateRootPresentation(void* object){
    rootVisible=RootShown(object);auto p=static_cast<char*>(object);
    rootEntering=rootVisible&&(*reinterpret_cast<int*>(p+0x28)!=6||*reinterpret_cast<int*>(p+0x2c)!=6);
    if(!rootVisible||*reinterpret_cast<int*>(p+0x28)==5||*reinterpret_cast<int*>(p+0x2c)==5)rootPose={};
}
void __fastcall Options(void* object,void*){
    DiscoverMotion();
    lastMenu=GetTickCount64();
    if(menuObject!=object){menuObject=object;entrancePlaying=false;audioOpen=false;buttonShapes.fill(nullptr);rootPose={};}
    struct ClearDrawPoses {~ClearDrawPoses(){for(auto& pose:buttonPoses)pose={};}} clearDrawPoses;
    nativeHexes.Clear();UpdateRootPresentation(object);
    if(rootVisible&&ready&&!art::hex.texture)art::Discover();
    if(ready&&(Field<int>(object,0x28)==5||Field<int>(object,0x2c)==5)){
        originalExec(object,nullptr);
        art::Discover();BindButtons(object);
        if(LabelFrame(object,"vib")>0){
            rootChoice=Field<int>(object,0x94);if(rootChoice<0||rootChoice>3||(rootChoice==0&&LabelFrame(object,"audio")<0))rootChoice=1;
            nativeChoice=rootChoice;focused=false;open=false;pageMotion.Snap(false);
            Field<int>(object,0x28)=6;Field<int>(object,0x2c)=6;
            entrancePlaying=true;animationFrame=LabelFrame(object,"opt");
            rootVisible=true;rootEntering=true;EvaluateFrame(object,animationFrame);return;
        }
    }
    if(audioOpen){AudioFrame(object,Input());return;}
    if(entrancePlaying){
        float end=LabelFrame(object,LabelFrame(object,"audio")>0?"audio":"vib");
        animationFrame=std::min(animationFrame+1,end);EvaluateFrame(object,animationFrame);
        rootVisible=true;rootEntering=true;
        if(animationFrame>=end){entrancePlaying=false;rootEntering=false;SelectRoot(object,rootChoice);}
        return;
    }
    bool acceptsInput=Field<int>(object,0x28)==6&&Field<int>(object,0x2c)==6;
    if(!ready||!acceptsInput){
        open=false;focused=false;pageMotion.Snap(false);originalExec(object,nullptr);UpdateRootPresentation(object);
        if(rootVisible&&ready&&!art::hex.texture)art::Discover();
        if(Field<int>(object,0x28)==6&&Field<int>(object,0x2c)==6){BindButtons(object);SelectRoot(object,Field<int>(object,0x94));}
        return;
    }
    DWORD input=Input();if(pageMotion.Active(GetTickCount64()))return;Mouse(input);
    if(confirmation.active){if(input&16){Sound(0xe008);confirmation.End();Save();message="Display settings saved.";}else if(input&32){Sound();rollback=true;pending=true;}return;}
    if(pending)return;
    if(open){
        if(input&32){Sound();open=false;pageMotion.Begin(GetTickCount64(),false);focused=true;edit=current;return;}
        if(input&256){tab=(tab+TabCount()-1)%TabCount();row=0;Sound();}if(input&128){tab=(tab+1)%TabCount();row=0;Sound();}
        if(input&1){row=StepRow(tab,row,-1);Sound();}if(input&2){row=StepRow(tab,row,1);Sound();}
        if(input&4)Adjust(-1);if(input&8)Adjust(1);
        if(input&16){if(tab==0&&row==DisplayActionRow()){if(DisplayChanged(edit,current)){Sound(0xe008);pending=true;rollback=false;}}else Adjust(1);}
        return;
    }
    int chosen=int((input>>16)&7)-1;
    if(chosen<0)chosen=native_menu::Navigate(rootChoice,input,LabelFrame(object,"audio")>0);
    if(chosen!=rootChoice){Sound();SelectRoot(object,chosen);}else StepRoot(object);
    if(input&16){
        Sound(0xe008);
        if(rootChoice==4){open=true;pageMotion.Begin(GetTickCount64(),true);focused=true;tab=0;row=0;edit=current;message.clear();}
        else if(rootChoice==0){rootPose={};audioOpen=true;audioMotion=1;audioChoice=Field<int>(object,0x84)==1?1:0;animationFrame=LabelFrame(object,"audio2");rootVisible=false;EvaluateFrame(object,animationFrame);}
        else BeginNativePage(object,rootChoice);
    }else if((input&512)&&rootChoice!=4){
        int help=Mem<int>(0x743de8+rootChoice*4);
        if(help>=0){Field<int>(object,0x58)=help;Field<int>(object,0x54)=6;Field<int>(object,0x2c)=4;rootVisible=false;}
    }else if(input&32){
        // Keep the game's own exit/save flow; no native directional input is
        // delegated while the extended root is active.
        originalExec(object,nullptr);UpdateRootPresentation(object);
    }

}

void Render(int width,int height){
    if(!ui::Begin(Device(),width,height))return;
    art::LoadTitle();
    const bool visible=rootVisible&&GetTickCount64()-lastMenu<200;
    if(current.brightness){float x=-ui::ox/ui::sx,y=-ui::oy/ui::sy;DWORD alpha=DWORD(abs(current.brightness)*9);ui::Quad(x,y,width/ui::sx,height/ui::sy,(alpha<<24)|(current.brightness>0?0xffffff:0));}
    float amount=pageMotion.At(GetTickCount64());
    if(visible){
        float phase=float(GetTickCount64()%1400)/1400;
        float icon=IconProgress(amount),hover=focused?1-Smooth(amount/.3f):0;
        ui::NativeTransform observed;
        if(amount==0){
            if(buttonShapes[3]){
                if(buttonPoses[3].enabled){rootPose=buttonPoses[3];rootPose.x+=135*1.125f*ui::sx;}
                // Retain the last pose during a missed draw in this lifecycle.
            }else if(nativeHexes.TVTransform(ui::sx,rootX,rootY,art::fixedOptions?4u:3u,observed))rootPose=observed;
        }
        auto pose=rootPose;
        if(!pose.enabled)pose={true,ui::sx,0,0,ui::sy,rootX*ui::sx+ui::ox-.5f,rootY*ui::sy+ui::oy-.5f,rootX,rootY,0xffffffff};
        ui::nativeTransform=native_menu::ToPage(pose,icon,ui::sx,ui::ox,ui::oy);
        if(rootPose.enabled||(!rootEntering&&!buttonShapes[3]))art::TV(rootX,rootY,1.125f+(1.32f-1.125f)*icon,hover,phase);
        ui::nativeTransform={};
        if(focused&&amount<.5f){ui::opacity=RootOpacity(amount);art::Title(176-32*amount,131,32);ui::opacity=1;}
    }
    if(visible&&amount>0){
        // Keep the live native background and header. Only the option foreground
        // crossfades; no recreated wallpaper, watermark, or alternate scaling.
        float left=-ui::ox/ui::sx,top=-ui::oy/ui::sy;
        ui::opacity=PageOpacity(amount);ui::translateX=32*(1-amount);
        art::Title(176,42,32);
        const char* names[]={"Display","Graphics","Interface"};
        art::Banner(146,293,219,34,"Category");
        for(int i=0;i<TabCount();i++){float y=333+i*TabStep();art::Bar(160,y,188,30,i==tab);float h=23;ui::Text(names[i],254-ui::TextWidth(names[i],h)*.5f,y+3,h,i==tab?0xffffffff:0xffa4a5af,false);}
        art::Banner(405,119,441,35,names[tab]);
        for(int i=0;i<SelectableRows(tab);i++){
            float y=164+i*RowStep(tab),barHeight=RowStep(tab)-7;bool disabled=RowDisabled(tab,i),active=row==i&&!disabled;
            float opacity=ui::opacity;if(disabled)ui::opacity=opacity*.6f;
            art::Bar(405,y,439,barHeight,active);
            ui::Text(Label(tab,i),419,y+(barHeight-19)*.5f,19,active?0xffffffff:disabled?0xffd0d2d8:0xffadb0b8,false);
            std::string value=Value(tab,i);float size=std::min(23.f,191.f/std::max(1.f,ui::TextWidth(value,1)));
            ui::Right(value,826,y+(barHeight-size)*.5f+1,size,active?0xffffffff:disabled?0xffd0d2d8:0xffb4b8bd,false);
            ui::opacity=opacity;
            if(active){ui::Triangle(392,y+17,380,y+9,380,y+25,0xffef3900);ui::Triangle(856,y+17,868,y+9,868,y+25,0xffef3900);}
        }
        if(tab==2){
            ui::Text("Brightness preview",406,319,16,0xff624507,false);
            for(int i=0;i<8;i++){
                float value=i*255.0f/7;float blend=abs(current.brightness)*9.0f/255;
                value=current.brightness>0?value*(1-blend)+255*blend:value*(1-blend);
                DWORD v=DWORD(value);ui::Quad(407+i*55.f,349,51,40,0xff000000u|(v<<16)|(v<<8)|v);
            }
        }
        // Errors stay actionable; routine descriptions and saved notices are omitted.
        if(!message.empty()&&message!="Display settings saved."&&message!="Previous display settings restored.")ui::Text(message,132,466,12,0xffa72e0c,false);
        else if(tab==0){auto note=FpsNote();if(!note.empty())ui::Text(note,132,466,12,0xff755b18,false);}
        ui::Text(keyboardHints?"Q / E  Category":"LB / RB  Category",132,504,15,0xff755b18,false);
        art::Prompts(605,500,keyboardHints);
        if(confirmation.active){
            ui::Quad(left,top,width/ui::sx,height/ui::sy,0xb000164a);ui::Quad(215,181,530,172,0xffffb033);ui::Quad(222,188,516,158,0xfffff0b7);
            ui::Text("KEEP THESE SETTINGS?",247,204,25);int seconds=int((confirmation.deadline-std::min(confirmation.deadline,GetTickCount64())+999)/1000);
            ui::Text("Reverting in "+std::to_string(seconds)+" seconds",273,256,18,0xff856422,false);
            art::Prompts(362,304,keyboardHints);
        }
    }
    ui::opacity=1;ui::translateX=0;
    ui::End();
}
void __cdecl DrawFps(void* d,const motion::Stats* stats){
    if(!ready||applying||!stats||d!=Device())return;
    D3DVIEWPORT9 vp{};if(FAILED(D8<41>(d,&vp))||!vp.Width||!vp.Height||FAILED(D8<34>(d)))return;
    if(ui::Begin(d,int(vp.Width),int(vp.Height))){
        motion::Settings settings;if(motionApi)motionApi->get(&settings);
        float target=float(settings.enabled&&settings.targetHz?settings.targetHz:60);
        DWORD color=stats->renderFps>=target*.9f?0xffffffff:stats->renderFps>=target*.65f?0xffffda6c:0xffff896f;
        float x=(float(vp.Width)-ui::ox)/ui::sx-182,y=-ui::oy/ui::sy+8;
        art::Bar(x,y,174,28,true);art::Bar(x,y+30,174,22,false);
        char line[96];sprintf_s(line,"RENDER %.0f FPS",stats->renderFps);ui::Text(line,x+9,y+5,16,color,false);
        if(stats->status==motion::NeedVsyncOff)ui::Text("V-SYNC ON: 60 FPS ONLY",x+9,y+36,10,0xffffda6c,false);
        else{sprintf_s(line,"BASE %.0f   MAX GAP %.1f ms",stats->baseFps,stats->peakMs);ui::Text(line,x+9,y+36,10,stats->peakMs>33?0xffff896f:0xffd1d9e6,false);}
        ui::End();
    }
    D8<40>(d,&vp);
    // This runs after all capture/replay, once for every real submission. Calling
    // our EndScene hook would redraw the settings page over an interpolated menu.
    if(originalEnd)originalEnd(d);else D8<35>(d);
}
HRESULT __stdcall EndScene(void* d){
    if(ready&&!applying&&!shadow_compare::drawing&&GetTickCount64()<shadow_compare::noticeUntil){
        D3DVIEWPORT9 vp{};if(SUCCEEDED(D8<41>(d,&vp))&&vp.Width==Present().width&&vp.Height==Present().height&&vp.Width&&vp.Height&&ui::Begin(d,int(vp.Width),int(vp.Height))){
            art::Bar(280,18,400,30,true);ui::Text(shadow_compare::notice,296,25,16,0xffffffff,false);ui::End();
        }
    }
    bool visible=rootVisible&&GetTickCount64()-lastMenu<200;
    if(d==Device()&&ready&&!applying&&(visible||current.brightness)){D3DVIEWPORT9 vp{};if(SUCCEEDED(D8<41>(d,&vp))){RECT rc{};GetClientRect(Window(),&rc);int w=Present().width?int(Present().width):rc.right,h=Present().height?int(Present().height):rc.bottom;if(w>0&&h>0)Render(w,h);}}
    return originalEnd(d);
}
void Initialize(){
    auto table=*reinterpret_cast<void***>(Device());
    if(!Slot(table,14,&SelfShadowReset,originalReset)||!Slot(table,35,&EndScene,originalEnd)||!Slot(table,70,&Draw,originalDraw)||!Slot(table,71,&Indexed,originalIndexed)||!Slot(table,72,&DrawUP,originalDrawUP)||!Slot(table,73,&IndexedUP,originalIndexedUP)){initFailed=true;return;}
#ifndef VIDEO_TEST
    auto engine=Mem<void**>(0x8e0a4c);
    if(!engine||!Slot(engine,12,&ObserveNativeDraw,originalRwDraw)||!Slot(engine,13,&ObserveNativeIndexed,originalRwIndexed)){initFailed=true;return;}
#endif
    Enumerate();RECT client{};GetClientRect(Window(),&client);windowedStyle=GetWindowLongW(Window(),GWL_STYLE);
    if(!(windowedStyle&WS_CAPTION))windowedStyle=WS_OVERLAPPEDWINDOW|WS_VISIBLE;
    current.mode=Present().windowed?((GetWindowLongW(Window(),GWL_STYLE)&WS_CAPTION)?0:1):2;
    current.width=client.right;current.height=client.bottom;current.refresh=int(Present().refresh);current.vsync=Present().windowed?Present().swap==D3DSWAPEFFECT(4):Present().interval==1;
    current.aa=int(Present().samples);current.fog=Mem<int>(0x8caeb8)!=0;current.shadows=Mem<int>(0x8caef0)==0;
    Settings boot=current;Load();edit=current;current=boot;ready=true;DiscoverMotion();
    if(saveExists){startup=true;pending=true;}else edit=current;
}
int __cdecl Show(void* a,void* b,int flags){
    if(!ready&&!initFailed&&Device()&&++frames>20)Initialize();
    if(ready){
        if(confirmation.Expired(GetTickCount64())){pending=true;rollback=true;}
        if(pending){
            applying=true;Settings target=rollback?previous:edit;
            bool ok=ResetDisplay(target,rollback);
            if(startup){if(ok){current=target;edit=current;VisualSettings();}startup=false;}
            pending=false;rollback=false;applying=false;
        }
        // Match RenderWare's synchronization flag to the selected mode; this prevents
        // its normal Present path from undoing our V-sync choice each frame.
        flags=current.vsync?flags|1:flags&~1;
        if(GetTickCount64()-lastMenu>=200){rootVisible=false;open=false;focused=false;rootPose={};pageMotion.Snap(false);if(!art::retained.empty())art::ClearNative();}
    }
    return originalShow(a,b,flags);
}
}

extern "C" __declspec(dllexport) void __cdecl ReloadedStart(){
    static bool started=false;if(started)return;started=true;
#ifdef MODERN_VIDEO
    // One Reloaded-II mod: start the bundled FPS module first, matching the old
    // separate-mod order (Video listed FPS as an optional dependency).
    {wchar_t self[MAX_PATH];GetModuleFileNameW(module,self,MAX_PATH);
     auto fps=std::filesystem::path(self).parent_path()/L"ModernVideo.Fps.dll";
     fpsModule=LoadLibraryW(fps.c_str());
     if(fpsModule)if(auto start=reinterpret_cast<void(__cdecl*)()>(GetProcAddress(fpsModule,"ReloadedStart")))start();}
#endif
#ifdef MODERN_VIDEO
    constexpr const wchar_t* title=L"Sonic Heroes: Modern Video and High FPS";
#else
    constexpr const wchar_t* title=L"Sonic Heroes Video Settings";
#endif
    bool knownExecutable=GetModuleHandleW(nullptr)==reinterpret_cast<HMODULE>(0x400000)&&KnownExecutable();
    if(auto problem=knownExecutable
        ?StartupProblem(true,Mem<uintptr_t>(0x74f530),!memcmp(reinterpret_cast<void*>(0x658d40),"\xa1\x14\x0c\x8e\x00",5),!memcmp(reinterpret_cast<void*>(0x451d65),saveSuccessCall,5),!memcmp(reinterpret_cast<void*>(0x61e4c0),cameraPrologue,sizeof(cameraPrologue)))
        :StartupProblem(false,0,false,false,false)){
        std::wstring text=std::wstring(title)+L" could not load: "+problem;
        MessageBoxW(nullptr,text.c_str(),title,MB_ICONERROR);return;
    }
    shadow_quality::compatible=InstallShadowQuality();
    if(shadow_quality::compatible)shadow_quality::compatible=InstallShadowModels();
    if(shadow_quality::compatible)shadow_quality::compatible=InstallShadowFilter();
    if(shadow_quality::compatible)shadow_compare::installed=InstallShadowCompare();
    if(shadow_compare::installed)enemy_shadows::installed=InstallEnemyShadows();
    // Self and object shadows are disabled (see RowDisabled); leave their hooks out.
    InstallStoneReceivers();
    // Shape identity survives overlapping/hidden entrance sprites. This hook is
    // upstream of the existing RenderWare driver chain and does not edit vertices.
    const unsigned char shapePrologue[]={0x81,0xec,0x98,0,0,0};
    if(memcmp(reinterpret_cast<void*>(0x6b1bd0),shapePrologue,6)){MessageBoxW(nullptr,L"Video Settings found an incompatible native shape hook.",L"Video Settings",MB_ICONERROR);return;}
    auto shapeStub=static_cast<unsigned char*>(VirtualAlloc(nullptr,16,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));if(!shapeStub)return;
    memcpy(shapeStub,shapePrologue,6);shapeStub[6]=0xe9;*reinterpret_cast<int32_t*>(shapeStub+7)=int32_t(0x6b1bd6-reinterpret_cast<uintptr_t>(shapeStub+11));
    originalShape=reinterpret_cast<ShapeFn>(shapeStub);
    unsigned char shapeJump[6]={0xe9,0,0,0,0,0x90};*reinterpret_cast<int32_t*>(shapeJump+1)=int32_t(reinterpret_cast<uintptr_t>(&ObserveShape)-0x6b1bd5);
    DWORD shapeOld;VirtualProtect(shapeStub,16,PAGE_EXECUTE_READ,&shapeOld);FlushInstructionCache(GetCurrentProcess(),shapeStub,16);
    if(!Write(reinterpret_cast<void*>(0x6b1bd0),shapeJump,6))return;
    unsigned char saveJump[5]={0xe8};
    *reinterpret_cast<int32_t*>(saveJump+1)=int32_t(reinterpret_cast<uintptr_t>(&SkipSaveSuccess)-0x451d6a);
    if(!Write(reinterpret_cast<void*>(0x451d65),saveJump,5)){MessageBoxW(nullptr,L"Video Settings could not install the Options save acknowledgement fix.",L"Video Settings",MB_ICONERROR);return;}
    auto cameraStub=static_cast<unsigned char*>(VirtualAlloc(nullptr,16,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));if(!cameraStub)return;
    memcpy(cameraStub,cameraPrologue,sizeof(cameraPrologue));cameraStub[9]=0xe9;
    *reinterpret_cast<int32_t*>(cameraStub+10)=int32_t(0x61e4c9-reinterpret_cast<uintptr_t>(cameraStub+14));
    originalCamera=cameraStub;
    unsigned char cameraJump[9]={0xe9,0,0,0,0,0x90,0x90,0x90,0x90};
    *reinterpret_cast<int32_t*>(cameraJump+1)=int32_t(reinterpret_cast<uintptr_t>(&CameraUpdate)-0x61e4c5);
    DWORD cameraOld;VirtualProtect(cameraStub,16,PAGE_EXECUTE_READ,&cameraOld);FlushInstructionCache(GetCurrentProcess(),cameraStub,16);
    if(!Write(reinterpret_cast<void*>(0x61e4c0),cameraJump,9)){MessageBoxW(nullptr,L"Video Settings could not install the camera hook.",L"Video Settings",MB_ICONERROR);return;}
    wchar_t path[MAX_PATH];GetModuleFileNameW(module,path,MAX_PATH);
    art::titlePath=(std::filesystem::path(path).parent_path()/L"assets"/L"video-title.png").wstring();
    auto modsFolder=std::filesystem::path(path).parent_path().parent_path();
    art::promptPath=(modsFolder/L"sonicheroes.guis.widescreenrevamp"/L"Redirector"/L"dvdroot"/L"advertise"/L"E"/L"adv_bar.one").wstring();
    wchar_t gamePath[MAX_PATH];GetModuleFileNameW(nullptr,gamePath,MAX_PATH);
    ui::fontPath=(std::filesystem::path(gamePath).parent_path()/L"dvdroot"/L"font"/L"a_euascii_0.png").wstring();
    auto userMods=std::filesystem::path(path).parent_path().parent_path().parent_path()/L"User"/L"Mods";
#ifdef MODERN_VIDEO
    auto folder=userMods/L"sonicheroes.graphics.modern-video";
#else
    auto folder=userMods/L"sonicheroes.menu.video";
#endif
    std::error_code ec;std::filesystem::create_directories(folder,ec);configPath=(folder/L"Video.ini").wstring();
#ifdef MODERN_VIDEO
    if(!std::filesystem::exists(configPath,ec))std::filesystem::copy_file(userMods/L"sonicheroes.menu.video"/L"Video.ini",configPath,ec);
#endif
    auto trampoline=static_cast<unsigned char*>(VirtualAlloc(nullptr,16,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));if(!trampoline)return;
    memcpy(trampoline,reinterpret_cast<void*>(0x658d40),5);trampoline[5]=0xe9;*reinterpret_cast<int32_t*>(trampoline+6)=int32_t(0x658d45-reinterpret_cast<uintptr_t>(trampoline+10));
    originalShow=reinterpret_cast<ShowFn>(trampoline);unsigned char jump[5]={0xe9};*reinterpret_cast<int32_t*>(jump+1)=int32_t(reinterpret_cast<uintptr_t>(&Show)-0x658d45);
    DWORD old;VirtualProtect(trampoline,16,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),trampoline,16);
    if(!Write(reinterpret_cast<void*>(0x658d40),jump,5)||!Slot(reinterpret_cast<void**>(0x74f52c),1,&Options,originalExec))MessageBoxW(nullptr,L"Video Settings could not install its hooks. Restart the game.",L"Video Settings",MB_ICONERROR);
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){module=instance;DisableThreadLibraryCalls(instance);}return TRUE;}


