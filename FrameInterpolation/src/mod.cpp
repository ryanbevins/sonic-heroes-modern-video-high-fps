#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h> // Shared POD/enumerations only. All COM slots below are D3D8.
#include <bcrypt.h>
#include <mmsystem.h>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <array>
#include <tuple>
#include "api.h"
#include "game_code.h"
#include "math.h"
#include "rates.h"
#include "diagnostics.h"

namespace fi {
using namespace motion;
template<int I,class... A> HRESULT D8(void* o,A... a){return reinterpret_cast<HRESULT(__stdcall*)(void*,A...)>((*static_cast<void***>(o))[I])(o,a...);}
template<class T> T& Mem(uintptr_t p){return *reinterpret_cast<T*>(p);}
struct Present8 {UINT width,height;D3DFORMAT format;UINT count;D3DMULTISAMPLE_TYPE samples;D3DSWAPEFFECT swap;HWND window;BOOL windowed,depth;D3DFORMAT depthFormat;DWORD flags,refresh,interval;};
static_assert(sizeof(Present8)==52&&sizeof(Matrix)==sizeof(D3DMATRIX),"Win32 D3D8 ABI");
struct SurfaceDesc8 {D3DFORMAT format;D3DRESOURCETYPE type;DWORD usage;D3DPOOL pool;UINT size;D3DMULTISAMPLE_TYPE samples;UINT width,height;};
struct BufferDesc8 {D3DFORMAT format;D3DRESOURCETYPE type;DWORD usage;D3DPOOL pool;UINT size;DWORD fvf;};
Settings settings;Stats counters;HMODULE module=nullptr;std::wstring ini;
FrameRates frameRates;Overlay overlay=nullptr;
void __cdecl GetStats(Stats* out);
bool shuttingDown=false;
void* device=nullptr;bool replaying=false,capturing=false,installed=false,paired=false,deviceReady=false,pacedTick=false;
uintptr_t skinOwner=0;bool hudPhase=false;uint64_t generation=0,epoch=0;uintptr_t stage=0;
double tickStart=0;bool clockValid=false;
// Exclusive fullscreen with V-Sync on: every image of a tick goes straight into the swap chain's FIFO, which shows
// one per refresh while the game works on the next tick, so a long tick only shows once it outlasts the queue.
bool queuedTick=false;
bool timerActive=false,menuFrame=false;unsigned warmup=0;
HANDLE pacingTimer=nullptr;bool timerAttempted=false;
double replayCost=.0005;uint64_t context=0;
void Timer(bool enable){
    if(enable){
        if(!timerActive)timerActive=timeBeginPeriod(1)==TIMERR_NOERROR;
        if(!timerAttempted){timerAttempted=true;pacingTimer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);}
    }else{
        if(timerActive){timeEndPeriod(1);timerActive=false;}
        if(pacingTimer){CloseHandle(pacingTimer);pacingTimer=nullptr;}timerAttempted=false;
    }
}
constexpr size_t FrameBudget=96*1024*1024,BufferBudget=128*1024*1024;
size_t shadowBytes=0;
double Now(){return timing::Clock();}
void Until(double target){
    timing::Scope timer(timing::Wait);
    // CRI pins the game thread to CPU 0 and disables its priority boost. At
    // normal priority it wakes behind whatever took CPU 0 while it slept,
    // often a full quantum late. Raise priority only while blocked; the final
    // spin, game and CRI code keep their original priorities.
    HANDLE self=GetCurrentThread();const int priority=GetThreadPriority(self);
    const bool raise=priority!=THREAD_PRIORITY_ERROR_RETURN&&priority<THREAD_PRIORITY_TIME_CRITICAL;
    auto block=[&](auto wait){bool raised=raise&&SetThreadPriority(self,THREAD_PRIORITY_TIME_CRITICAL);bool woke=wait();if(raised)SetThreadPriority(self,priority);return woke;};
    for(;;){double remain=target-Now();if(remain<=0)break;
        // Relative high-resolution timer for the coarse portion; QPC spin for
        // the final half millisecond. Keep a compatible Sleep fallback.
        if(pacingTimer&&remain>.001){LARGE_INTEGER due;due.QuadPart=-LONGLONG((remain-.0005)*1e7);
            if(SetWaitableTimer(pacingTimer,&due,0,nullptr,nullptr,FALSE)&&block([&]{return WaitForSingleObject(pacingTimer,INFINITE)==WAIT_OBJECT_0;}))continue;
        }
        if(remain>.002)block([]{Sleep(1);return true;});else YieldProcessor();
    }
}
void Ref(void* p){if(p)D8<1>(p);}void Release(void* p){if(p)D8<2>(p);}
bool Write(void* to,const void* data,size_t n){DWORD old=0;if(!VirtualProtect(to,n,PAGE_EXECUTE_READWRITE,&old))return false;memcpy(to,data,n);FlushInstructionCache(GetCurrentProcess(),to,n);DWORD unused;return VirtualProtect(to,n,old,&unused)!=FALSE;}
template<class F> bool Slot(void** table,int i,F next,F& previous){previous=reinterpret_cast<F>(table[i]);void* p=reinterpret_cast<void*>(next);return Write(table+i,&p,4);}

// Native commands remain captured in order. Only a fully cleared backbuffer
// may defer GPU draws; all read/mutation barriers materialize it first.
bool deferCandidate=false,deferArmed=false,deferredPending=false,nativeScene=false;
struct Command;bool FlushDeferred(uint32_t reason=0);bool ArmDeferred(const Command&);
void PixelShaderDeleted(DWORD shader);
#include "pipeline.h"
DWORD restoreToken=0;

// Shadow CPU buffer writes at Unlock. Reading back a WRITEONLY GPU buffer is
// deliberately avoided. Unobserved contents make that frame ineligible.
using LockFn=HRESULT(__stdcall*)(void*,UINT,UINT,BYTE**,DWORD);
using UnlockFn=HRESULT(__stdcall*)(void*);
using ReleaseFn=ULONG(__stdcall*)(void*);
struct BufferHooks {LockFn lock;UnlockFn unlock;ReleaseFn release;};
std::unordered_map<void**,BufferHooks> bufferHooks;
std::unordered_map<void*,Shadow> shadows;
HRESULT __stdcall Lock(void* b,UINT offset,UINT size,BYTE** data,DWORD flags){
    auto hooks=bufferHooks.at(*static_cast<void***>(b));HRESULT hr=hooks.lock(b,offset,size,data,flags);
    if(SUCCEEDED(hr)&&data){auto it=shadows.find(b);if(it!=shadows.end()){auto& s=it->second;
        if(flags&D3DLOCK_DISCARD)s.valid.clear();s.offset=offset;s.length=offset<=s.bytes.size()?(size?size:s.bytes.size()-offset):0;s.locked=*data;s.readonly=(flags&D3DLOCK_READONLY)!=0;
        uintptr_t page=reinterpret_cast<uintptr_t>(s.locked)>>12;
        if(page!=s.mirrorPage){s.mirrorPage=page;MEMORY_BASIC_INFORMATION info{};s.streaming=StreamingReadSupported()&&VirtualQuery(s.locked,&info,sizeof(info))&&bool(info.Protect&PAGE_WRITECOMBINE);}
    }}return hr;
}
HRESULT __stdcall Unlock(void* b){
    timing::Scope timer(timing::BufferMirror);
    auto it=shadows.find(b);if(it!=shadows.end()){auto& s=it->second;if(s.locked&&!s.readonly&&s.length){s.Write(s.offset,s.locked,s.length,s.streaming);if(s.streaming)timing::frame.reasons|=timing::StreamingBuffer;}s.locked=nullptr;}
    return bufferHooks.at(*static_cast<void***>(b)).unlock(b);
}
ULONG __stdcall BufferRelease(void* b){
    auto fn=bufferHooks.at(*static_cast<void***>(b)).release;ULONG left=fn(b);if(!left){auto it=shadows.find(b);if(it!=shadows.end()){shadowBytes-=it->second.bytes.size();shadows.erase(it);}}return left;
}
bool TrackBuffer(void* b,UINT size,bool dynamic=false){
    if(!b||!size||size>BufferBudget||shadowBytes>BufferBudget-size)return false;
    auto table=*static_cast<void***>(b);if(!bufferHooks.count(table)){
        BufferHooks h{reinterpret_cast<LockFn>(table[11]),reinterpret_cast<UnlockFn>(table[12]),reinterpret_cast<ReleaseFn>(table[2])};bufferHooks.emplace(table,h);
        void* l=reinterpret_cast<void*>(&Lock);void* u=reinterpret_cast<void*>(&Unlock);void* r=reinterpret_cast<void*>(&BufferRelease);
        if(!Write(table+11,&l,4)||!Write(table+12,&u,4)||!Write(table+2,&r,4))return false;
    }
    auto& s=shadows[b];shadowBytes-=s.bytes.size();s={};s.generation=++generation;s.dynamic=dynamic;s.bytes.resize(size);shadowBytes+=size;return true;
}
using CreateBufferFn=HRESULT(__stdcall*)(void*,UINT,DWORD,DWORD,D3DPOOL,void**);
CreateBufferFn oldCreateVB=nullptr,oldCreateIB=nullptr;
HRESULT __stdcall CreateVB(void* d,UINT n,DWORD usage,DWORD fvf,D3DPOOL pool,void** out){HRESULT hr=oldCreateVB(d,n,usage,fvf,pool,out);if(SUCCEEDED(hr)&&out)TrackBuffer(*out,n,(usage&D3DUSAGE_DYNAMIC)!=0);return hr;}
HRESULT __stdcall CreateIB(void* d,UINT n,DWORD usage,DWORD format,D3DPOOL pool,void** out){HRESULT hr=oldCreateIB(d,n,usage,format,pool,out);if(SUCCEEDED(hr)&&out)TrackBuffer(*out,n);return hr;}

struct Command {
    enum Kind {Draw,Clear,Copy} kind=Draw;
    pipeline::State gpu;void* dev=nullptr;void* target=nullptr;void* depth=nullptr;void* source=nullptr;
    D3DVIEWPORT9 viewport{};Matrix world{},view{},projection{};
    DWORD fvf=0;UINT stride=0,count=0,vertices=0;D3DPRIMITIVETYPE primitive=D3DPT_TRIANGLELIST;
    std::vector<uint8_t> data,indices;D3DFORMAT indexFormat=D3DFMT_INDEX16;
    std::vector<D3DRECT> clearRects;std::vector<RECT> copyRects;std::vector<POINT> points;
    DWORD flags=0,color=0,stencil=0;float z=1;
    uint64_t key=0,layout=0,layoutKey=0;uintptr_t texture=0;std::array<uintptr_t,8> textures{};uintptr_t targetOwner=0,sourceOwner=0;bool scene3d=false,skinned=false,screenSpace=false,canvas=false;
    // The game's own buffers and draw parameters: a replay of unmoved geometry draws from them as the game did.
    void* nativeVb=nullptr;void* nativeIb=nullptr;UINT nativeStart=0,nativeMin=0,nativeBase=0;uint64_t vbWrites=0,ibWrites=0;
    void Reuse(){
        Release(nativeVb);Release(nativeIb);nativeVb=nativeIb=nullptr;nativeStart=nativeMin=nativeBase=0;vbWrites=ibWrites=0;
        Release(target);Release(depth);Release(source);for(auto t:gpu.textures)Release(t);gpu.textures.fill(nullptr);target=depth=source=nullptr;
        kind=Draw;viewport={};world=view=projection={};fvf=0;stride=count=vertices=0;primitive=D3DPT_TRIANGLELIST;
        data.clear();indices.clear();indexFormat=D3DFMT_INDEX16;clearRects.clear();copyRects.clear();points.clear();flags=color=stencil=0;z=1;
        key=layout=layoutKey=0;texture=targetOwner=sourceOwner=0;textures={};scene3d=skinned=screenSpace=canvas=false;
    }
    ~Command(){if(shuttingDown)return;Release(nativeVb);Release(nativeIb);for(auto t:gpu.textures)Release(t);Release(target);Release(depth);Release(source);}
};
struct Frame {std::vector<std::unique_ptr<Command>> commands;size_t bytes=0;bool valid=true;uint64_t epoch=0;double time=0;};
Frame previous,current;
std::vector<std::pair<uint64_t,const Command*>> layoutHistory;
void PrepareLayoutHistory(){
    layoutHistory.clear();if(!previous.valid)return;
    for(const auto& c:previous.commands)if(c->kind==Command::Draw)layoutHistory.emplace_back(c->layoutKey,c.get());
    std::sort(layoutHistory.begin(),layoutHistory.end(),[](const auto& a,const auto& b){return a.first<b.first;});
}
const Command* PriorLayout(uint64_t key){
    auto it=std::lower_bound(layoutHistory.begin(),layoutHistory.end(),key,[](const auto& a,uint64_t b){return a.first<b;});
    if(it==layoutHistory.end()||it->first!=key||(it+1!=layoutHistory.end()&&(it+1)->first==key))return nullptr;
    return it->second;
}
std::vector<std::unique_ptr<Command>> commandPool;size_t pooledBytes=0;
size_t Storage(const Command& c){return c.data.capacity()+c.indices.capacity()+c.gpu.lights.capacity()*sizeof(pipeline::Light)+sizeof(Command);}
std::unique_ptr<Command> Acquire(){if(commandPool.empty())return std::make_unique<Command>();auto c=std::move(commandPool.back());commandPool.pop_back();pooledBytes-=Storage(*c);c->Reuse();return c;}
void Recycle(Frame& f){timing::Scope timer(timing::Cleanup);if(&f==&previous)layoutHistory.clear();for(auto& c:f.commands){size_t n=Storage(*c);if(commandPool.size()<2048&&n<=FrameBudget-pooledBytes){pooledBytes+=n;commandPool.push_back(std::move(c));}}f=Frame{};}
std::unordered_set<void*> texturesRead;
void Invalidate(Status why,uint32_t reason=timing::GeometryRejected){FlushDeferred();timing::frame.reasons|=timing::Unsupported|reason;capturing=false;current.valid=false;counters.status=why;}
void PixelShaderDeleted(DWORD shader){
    auto uses=[&](const Frame& f){return std::any_of(f.commands.begin(),f.commands.end(),[&](const auto& c){return c->kind==Command::Draw&&c->gpu.ps==shader;});};
    if(uses(previous))previous.valid=false;
    if(uses(current))Invalidate(Unsupported,timing::StateRejected);
    pipeline::dirty=true;
}
// Release the D3D references of captured commands and keep their memory pooled; freeing thousands of large
// vertex blocks at once took 25 ms on the game thread at the goal ring.
void Park(Frame& f){for(auto& c:f.commands){c->Reuse();size_t n=Storage(*c);if(commandPool.size()<2048&&n<=FrameBudget-pooledBytes){pooledBytes+=n;commandPool.push_back(std::move(c));}}f=Frame{};}
void ClearHistory(){FlushDeferred();deferCandidate=false;timing::Scope timer(timing::Cleanup);layoutHistory.clear();for(auto& c:commandPool)c->Reuse();Park(current);Park(previous);texturesRead.clear();if(restoreToken&&device){D8<56>(device,restoreToken);restoreToken=0;}pipeline::dirty=true;++epoch;warmup=3;}
using TextureLockFn=HRESULT(__stdcall*)(void*,UINT,D3DLOCKED_RECT*,const RECT*,DWORD);
std::unordered_map<void**,TextureLockFn> textureLocks;
HRESULT __stdcall TextureLock(void* texture,UINT level,D3DLOCKED_RECT* data,const RECT* rect,DWORD flags){
    if(!replaying&&!FlushDeferred(timing::BarrierResource))return D3DERR_INVALIDCALL;
    if(capturing&&!replaying&&!(flags&D3DLOCK_READONLY)&&texturesRead.count(texture))Invalidate(Unsupported,timing::TextureMutation);
    return textureLocks.at(*static_cast<void***>(texture))(texture,level,data,rect,flags);
}
bool ObserveTexture(void* texture){
    if(!texture)return true;auto table=*static_cast<void***>(texture);
    auto type=reinterpret_cast<D3DRESOURCETYPE(__stdcall*)(void*)>(table[10])(texture);
    if(type!=D3DRTYPE_TEXTURE)return false;
    if(!textureLocks.count(table)){textureLocks.emplace(table,reinterpret_cast<TextureLockFn>(table[16]));void* fn=reinterpret_cast<void*>(&TextureLock);if(!Write(table+16,&fn,4))return false;}
    texturesRead.insert(texture);return true;
}
UINT VertexCount(D3DPRIMITIVETYPE p,UINT n){if(n>1000000)return 0;switch(p){case D3DPT_POINTLIST:return n;case D3DPT_LINELIST:return n*2;case D3DPT_LINESTRIP:return n+1;case D3DPT_TRIANGLELIST:return n*3;case D3DPT_TRIANGLESTRIP:case D3DPT_TRIANGLEFAN:return n+2;default:return 0;}}
bool Room(size_t n){if(current.commands.size()>12000||n>FrameBudget||current.bytes>FrameBudget-n){Invalidate(Unsupported,timing::CapacityRejected);return false;}current.bytes+=n;return true;}
const GUID UnknownId={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
uintptr_t SurfaceOwner(void* surface){void* owner=nullptr;if(surface)D8<7>(surface,&UnknownId,&owner);auto result=reinterpret_cast<uintptr_t>(owner);Release(owner);return result;}
bool Targets(Command& c){c.dev=device;HRESULT depth=D8<33>(device,&c.depth);bool ok=SUCCEEDED(D8<32>(device,&c.target))&&(SUCCEEDED(depth)||depth==D3DERR_NOTFOUND)&&SUCCEEDED(D8<41>(device,&c.viewport));if(ok)c.targetOwner=SurfaceOwner(c.target);return ok;}
std::array<uintptr_t,4> lastTextureMismatch{};
bool State(Command& c){
    timing::Scope timer(timing::State);
    if(!Targets(c)||!pipeline::Capture(c.gpu))return false;
    // Only the stages this draw uses are verified and kept: up to the first disabled colour stage, or all 8 with a
    // pixel shader. The fixed-function pipeline ignores the rest, so they are recorded as empty.
    DWORD used=8;
    auto trim=[&](){used=8;if(!c.gpu.ps)for(DWORD i=0;i<8;i++)if(c.gpu.hasTss[i][D3DTSS_COLOROP]&&c.gpu.tss[i][D3DTSS_COLOROP]==D3DTOP_DISABLE){used=i;break;}
        for(DWORD i=used;i<8;i++)c.gpu.textures[i]=nullptr;};
    trim();
    for(auto t:c.gpu.textures)Ref(t);
    c.fvf=c.gpu.fvf;c.world=c.gpu.transforms[0];c.view=c.gpu.transforms[1];c.projection=c.gpu.transforms[2];
    for(DWORD i=used;i<8;i++)c.textures[i]=0;
    for(DWORD i=0;i<used;i++){void* tex=nullptr;if(SUCCEEDED(D8<60>(device,i,&tex))){if(tex!=c.gpu.textures[i]){
            lastTextureMismatch={i,reinterpret_cast<uintptr_t>(c.gpu.textures[i]),reinterpret_cast<uintptr_t>(tex),uintptr_t(current.commands.size())};
            Release(tex);pipeline::dirty=true;timing::frame.reasons|=timing::TextureResync;
            for(auto t:c.gpu.textures)Release(t);c.gpu.textures.fill(nullptr);
            if(!pipeline::Capture(c.gpu))return false;{DWORD before=used;trim();for(DWORD k=used;k<before;k++)c.textures[k]=0;}for(auto t:c.gpu.textures)Ref(t);
            c.fvf=c.gpu.fvf;c.world=c.gpu.transforms[0];c.view=c.gpu.transforms[1];c.projection=c.gpu.transforms[2];
            tex=nullptr;if(FAILED(D8<60>(device,i,&tex))||tex!=c.gpu.textures[i]){Release(tex);return false;}
        }bool supported=ObserveTexture(tex);void* identity=nullptr;if(tex)D8<0>(tex,&UnknownId,&identity);c.textures[i]=reinterpret_cast<uintptr_t>(identity);Release(identity);Release(tex);if(!supported)return false;}}c.texture=c.textures[0];
    // D3D8 shader handles use bit 0. CPU skinning in retail Heroes uses FVF XYZ.
    if((c.fvf&1)||((c.fvf&D3DFVF_POSITION_MASK)!=D3DFVF_XYZ&&(c.fvf&D3DFVF_POSITION_MASK)!=D3DFVF_XYZRHW))return false;
    c.screenSpace=(c.fvf&D3DFVF_POSITION_MASK)==D3DFVF_XYZRHW;
    c.canvas=menuFrame&&!c.screenSpace&&std::abs(c.projection.m[2][3])<.001f&&std::abs(c.projection.m[3][3]-1)<.001f;
    c.scene3d=menuFrame||(!hudPhase&&(c.screenSpace||(std::abs(c.projection.m[2][3])>.5f&&std::abs(c.projection.m[3][3])<.001f)));
    c.skinned=skinOwner!=0;return true;
}
template<class Index> bool RebaseIndices(std::vector<uint8_t>& data,UINT min,UINT vertices){
    if(data.size()%sizeof(Index))return false;
    for(size_t i=0;i<data.size();i+=sizeof(Index)){Index raw;memcpy(&raw,data.data()+i,sizeof(raw));uint32_t value=raw;
        if(value<min||value-min>=vertices)return false;raw=Index(value-min);memcpy(data.data()+i,&raw,sizeof(raw));}
    return true;
}
template<class Index> bool IndicesInRange(const std::vector<uint8_t>& data,UINT vertices){
    if(data.size()%sizeof(Index))return false;
    for(size_t i=0;i<data.size();i+=sizeof(Index)){Index value;memcpy(&value,data.data()+i,sizeof(value));if(value>=vertices)return false;}return true;
}
// Per-vertex UV identity; FVF strides and offsets are multiples of 4 (anything else takes the byte path).
bool SameStrided(const uint8_t* a,const uint8_t* b,size_t size,size_t stride,size_t uv){
    if((stride|uv)&3){for(size_t i=0;i<size;i+=stride)if(memcmp(a+i+uv,b+i+uv,stride-uv))return false;return true;}
    const size_t words=(stride-uv)/4;
    for(size_t i=0;i<size;i+=stride){const uint32_t* x=reinterpret_cast<const uint32_t*>(a+i+uv);const uint32_t* y=reinterpret_cast<const uint32_t*>(b+i+uv);uint32_t d=0;for(size_t k=0;k<words;k++)d|=x[k]^y[k];if(d)return false;}
    return true;
}
// Layout identity hash; only compared within a session, so it hashes four bytes at a time.
uint64_t LayoutHash(const uint8_t* data,size_t size,size_t stride,size_t uv){
    uint64_t h=14695981039346656037ull;if(uv>=stride)return h;
    if((stride|uv)&3){for(size_t i=0;i<size;i+=stride)h=Hash(data+i+uv,stride-uv,h);return h;}
    uint32_t lo=0x811C9DC5u,hi=0x9E3779B9u;const size_t words=(stride-uv)/4;
    for(size_t i=0;i<size;i+=stride){const uint32_t* x=reinterpret_cast<const uint32_t*>(data+i+uv);for(size_t k=0;k<words;k++){lo=(lo^x[k])*0x01000193u;hi=(hi+x[k])*0x85EBCA6Bu;hi^=hi>>13;}}
    return (uint64_t(hi)<<32)|lo;
}
bool Finalize(Command& c,uint64_t resource,UINT offset){
    if(c.stride<12||c.stride>256||!c.vertices||c.data.size()!=size_t(c.vertices)*c.stride)return false;
    if(c.indices.size()){size_t width=c.indexFormat==D3DFMT_INDEX16?2:c.indexFormat==D3DFMT_INDEX32?4:0;if(!width||c.indices.size()!=size_t(VertexCount(c.primitive,c.count))*width)return false;
        if(!(width==2?IndicesInRange<uint16_t>(c.indices,c.vertices):IndicesInRange<uint32_t>(c.indices,c.vertices)))return false;}
    if(skinOwner){resource=0;offset=0;} // CPU skin buffers may rotate; atomic identity does not.
    uint64_t h=Hash(&resource,sizeof(resource));h=Hash(&offset,4,h);h=Hash(&skinOwner,sizeof(skinOwner),h);h=Hash(c.textures.data(),sizeof(c.textures),h);h=Hash(&c.target,sizeof(c.target),h);h=Hash(&c.fvf,4,h);h=Hash(&c.stride,4,h);h=Hash(&c.vertices,4,h);h=Hash(&c.primitive,4,h);c.layoutKey=h;
    // Topology and UV coordinates anchor correspondence. Color changes may be
    // effect fades, so retain the current color instead of using it as identity.
    size_t uv=12+((c.fvf&D3DFVF_POSITION_MASK)==D3DFVF_XYZRHW?4:0)+(c.fvf&D3DFVF_NORMAL?12:0)+(c.fvf&D3DFVF_PSIZE?4:0)+(c.fvf&D3DFVF_DIFFUSE?4:0)+(c.fvf&D3DFVF_SPECULAR?4:0);
    // Exact byte comparisons retain the existing hash/identity semantics while
    // avoiding byte-at-a-time 64-bit multiplication over unchanged topology on
    // x86. Animated positions/colors are deliberately outside the UV identity.
    const auto prior=PriorLayout(c.layoutKey);
    bool same=prior&&prior->stride==c.stride&&prior->fvf==c.fvf&&prior->data.size()==c.data.size()&&prior->indices==c.indices;
    // Unchanged geometry compares in one memcmp; otherwise a word loop instead of a memcmp call per vertex.
    if(same&&uv<c.stride&&memcmp(prior->data.data(),c.data.data(),c.data.size()))same=SameStrided(prior->data.data(),c.data.data(),c.data.size(),c.stride,uv);
    if(same){c.key=prior->key;c.layout=prior->layout;}
    else {c.key=Hash(c.indices.data(),c.indices.size(),h);c.layout=LayoutHash(c.data.data(),c.data.size(),c.stride,uv);if(menuFrame)c.key=Hash(&c.layout,sizeof(c.layout),c.key);}
    return Room(c.data.size()+c.indices.size()+sizeof(Command)+c.gpu.lights.size()*sizeof(pipeline::Light));
}
void CaptureUP(D3DPRIMITIVETYPE p,UINT count,const void* data,UINT stride,UINT min,UINT vertices,const void* indices,D3DFORMAT format){
    if(!capturing||replaying)return;timing::Scope timer(timing::Capture);
    auto c=Acquire();c->primitive=p;c->count=count;c->stride=stride;c->vertices=vertices;c->indexFormat=format;
    if(!State(*c)){Invalidate(Unsupported,timing::StateRejected);return;}
    if(!data||!vertices||stride>256||vertices>1000000||min>1000000||!stride||size_t(vertices)*stride>FrameBudget-current.bytes){Invalidate(Unsupported);return;}
    auto bytes=static_cast<const uint8_t*>(data);c->data.assign(bytes+size_t(min)*stride,bytes+size_t(min+vertices)*stride);
    if(indices){if(format!=D3DFMT_INDEX16&&format!=D3DFMT_INDEX32){Invalidate(Unsupported);return;}size_t width=format==D3DFMT_INDEX16?2:4;UINT n=VertexCount(p,count);auto source=static_cast<const uint8_t*>(indices);c->indices.assign(source,source+size_t(n)*width);
        if(!(width==2?RebaseIndices<uint16_t>(c->indices,min,vertices):RebaseIndices<uint32_t>(c->indices,min,vertices))){Invalidate(Unsupported);return;}}
    if(!Finalize(*c,0,0)){Invalidate(Unsupported);return;}current.commands.push_back(std::move(c));
}
void CaptureVB(D3DPRIMITIVETYPE p,UINT count,UINT start,UINT min,UINT vertices,bool indexed){
    if(!capturing||replaying)return;timing::Scope timer(timing::Capture);if(!vertices||vertices>1000000){Invalidate(Unsupported);return;}auto c=Acquire();c->primitive=p;c->count=count;c->vertices=vertices;
    if(!State(*c)){Invalidate(Unsupported,timing::StateRejected);return;}
    void* vb=nullptr;UINT stride=0;if(FAILED(D8<84>(device,UINT(0),&vb,&stride))||!vb){Release(vb);Invalidate(Unsupported,timing::BufferRejected);return;}
    c->stride=stride;auto it=shadows.find(vb);uint64_t id=it==shadows.end()?0:it->second.generation;UINT base=0;bool ok=true;
    if(indexed){void* ib=nullptr;ok=SUCCEEDED(D8<86>(device,&ib,&base))&&ib;auto ii=shadows.find(ib);BufferDesc8 desc{};ok=ok&&ii!=shadows.end()&&SUCCEEDED(D8<13>(ib,&desc));
        if(ok){c->indexFormat=desc.format;size_t width=desc.format==D3DFMT_INDEX16?2:desc.format==D3DFMT_INDEX32?4:0;ok=width&&ii->second.Read(size_t(start)*width,size_t(VertexCount(p,count))*width,c->indices);
            if(ok)ok=width==2?RebaseIndices<uint16_t>(c->indices,min,vertices):RebaseIndices<uint32_t>(c->indices,min,vertices);}
        if(ok){Ref(ib);c->nativeIb=ib;c->ibWrites=ii->second.writes;c->nativeBase=base;c->nativeMin=min;}
        Release(ib);
    }
    size_t offset=(size_t(base)+(indexed?min:start))*stride;
    ok=ok&&stride>=12&&stride<=256&&size_t(vertices)*stride<=FrameBudget-current.bytes&&it!=shadows.end()&&it->second.Read(offset,size_t(vertices)*stride,c->data);bool dynamic=it!=shadows.end()&&it->second.dynamic;
    if(ok&&!dynamic){Ref(vb);c->nativeVb=vb;c->vbWrites=it->second.writes;c->nativeStart=start;}else{Release(c->nativeIb);c->nativeIb=nullptr;}
    Release(vb);
    // Dynamic allocator offsets are transient. Only unambiguous material/topology
    // matches are accepted; duplicated keys are excluded by Match().
    if(!ok||offset>UINT_MAX){Invalidate(Unsupported,timing::BufferRejected);return;}
    if(!Finalize(*c,dynamic?0:id,dynamic?0:UINT(offset))){Invalidate(Unsupported);return;}current.commands.push_back(std::move(c));
}
using DrawFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,UINT);
using IndexedFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,UINT,UINT,UINT);
using UpFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
using IndexedUpFn=HRESULT(__stdcall*)(void*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
using ClearFn=HRESULT(__stdcall*)(void*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
using CopyFn=HRESULT(__stdcall*)(void*,void*,const RECT*,UINT,void*,const POINT*);
using PresentFn=HRESULT(__stdcall*)(void*,const RECT*,const RECT*,HWND,const RGNDATA*);
using ResetFn=HRESULT(__stdcall*)(void*,Present8*);
using UpdateTextureFn=HRESULT(__stdcall*)(void*,void*,void*);UpdateTextureFn oldUpdateTexture=nullptr;
HRESULT __stdcall UpdateTexture(void* d,void* src,void* dst){if(!replaying&&!FlushDeferred(timing::BarrierResource))return D3DERR_INVALIDCALL;if(capturing&&!replaying&&texturesRead.count(dst))Invalidate(Unsupported,timing::TextureMutation);return oldUpdateTexture(d,src,dst);}
DrawFn oldDraw=nullptr;IndexedFn oldIndexed=nullptr;UpFn oldUp=nullptr;IndexedUpFn oldIndexedUp=nullptr;ClearFn oldClear=nullptr;CopyFn oldCopy=nullptr;PresentFn oldPresent=nullptr;ResetFn oldReset=nullptr;
HRESULT __stdcall Draw(void* d,D3DPRIMITIVETYPE p,UINT start,UINT count){if(!capturing&&!replaying&&!FlushDeferred(timing::BarrierUncaptured))return D3DERR_INVALIDCALL;CaptureVB(p,count,start,0,VertexCount(p,count),false);if(deferArmed&&capturing&&!replaying&&nativeScene){deferredPending=true;timing::frame.reasons|=timing::NativeDeferred;return D3D_OK;}return oldDraw(d,p,start,count);}
HRESULT __stdcall Indexed(void* d,D3DPRIMITIVETYPE p,UINT min,UINT n,UINT start,UINT count){if(!capturing&&!replaying&&!FlushDeferred(timing::BarrierUncaptured))return D3DERR_INVALIDCALL;CaptureVB(p,count,start,min,n,true);if(deferArmed&&capturing&&!replaying&&nativeScene){deferredPending=true;timing::frame.reasons|=timing::NativeDeferred;return D3D_OK;}return oldIndexed(d,p,min,n,start,count);}
HRESULT __stdcall Up(void* d,D3DPRIMITIVETYPE p,UINT n,const void* data,UINT stride){if(!capturing&&!replaying&&!FlushDeferred(timing::BarrierUncaptured))return D3DERR_INVALIDCALL;CaptureUP(p,n,data,stride,0,VertexCount(p,n),nullptr,D3DFMT_INDEX16);if(deferArmed&&capturing&&!replaying&&nativeScene){deferredPending=true;timing::frame.reasons|=timing::NativeDeferred;if(FAILED(D8<83>(d,UINT(0),static_cast<void*>(nullptr),UINT(0)))){FlushDeferred();return D3DERR_INVALIDCALL;}return D3D_OK;}return oldUp(d,p,n,data,stride);}
HRESULT __stdcall IndexedUp(void* d,D3DPRIMITIVETYPE p,UINT min,UINT n,UINT count,const void* idx,D3DFORMAT fmt,const void* data,UINT stride){if(!capturing&&!replaying&&!FlushDeferred(timing::BarrierUncaptured))return D3DERR_INVALIDCALL;CaptureUP(p,count,data,stride,min,n,idx,fmt);if(deferArmed&&capturing&&!replaying&&nativeScene){deferredPending=true;timing::frame.reasons|=timing::NativeDeferred;if(FAILED(D8<83>(d,UINT(0),static_cast<void*>(nullptr),UINT(0)))){FlushDeferred();return D3DERR_INVALIDCALL;}if(FAILED(D8<85>(d,static_cast<void*>(nullptr),UINT(0)))){FlushDeferred();return D3DERR_INVALIDCALL;}return D3D_OK;}return oldIndexedUp(d,p,min,n,count,idx,fmt,data,stride);}
HRESULT __stdcall Clear(void* d,DWORD n,const D3DRECT* rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil){
    if(!capturing&&!replaying&&deferArmed&&!FlushDeferred(timing::BarrierClear))return D3DERR_INVALIDCALL;
    if(capturing&&!replaying){auto c=Acquire();c->kind=Command::Clear;c->flags=flags;c->color=color;c->z=z;c->stencil=stencil;
        if(!Targets(*c)||n>1024||(n&&!rects)||!Room(sizeof(Command)+n*sizeof(D3DRECT)))Invalidate(Unsupported);else{if(n&&rects)c->clearRects.assign(rects,rects+n);current.commands.push_back(std::move(c));}}
    // Subsequent depth/partial clears are commands too. Keep them ordered
    // with pending geometry instead of submitting the entire native prefix.
    if(deferArmed&&capturing&&!replaying){deferredPending=true;timing::frame.reasons|=timing::NativeDeferred;return D3D_OK;}
    auto hr=oldClear(d,n,rects,flags,color,z,stencil);
    if(SUCCEEDED(hr)&&deferCandidate&&capturing&&!replaying&&!current.commands.empty())deferArmed=ArmDeferred(*current.commands.back());
    return hr;
}
HRESULT __stdcall Copy(void* d,void* source,const RECT* rects,UINT n,void* target,const POINT* points){
    if(!replaying&&!FlushDeferred(timing::BarrierResource))return D3DERR_INVALIDCALL;
    if(capturing&&!replaying){if(n>1024||!Room(sizeof(Command)+n*(sizeof(RECT)+sizeof(POINT))))Invalidate(Unsupported);else{auto c=Acquire();c->kind=Command::Copy;c->source=source;c->target=target;Ref(source);Ref(target);c->sourceOwner=SurfaceOwner(source);c->targetOwner=SurfaceOwner(target);c->count=n;if(n&&rects)c->copyRects.assign(rects,rects+n);if(n&&points)c->points.assign(points,points+n);current.commands.push_back(std::move(c));}}
    return oldCopy(d,source,rects,n,target,points);
}
// Call at the bottom of the draw-hook chain: all earlier mods have already
// contributed vertices and state. Re-entering those hooks would filter twice.
UpFn driverUp=nullptr;IndexedUpFn driverIndexedUp=nullptr;DrawFn driverDraw=nullptr;IndexedFn driverIndexed=nullptr;
using SceneFn=HRESULT(__stdcall*)(void*);SceneFn driverBegin=nullptr,driverEnd=nullptr;
struct Restore {
    DWORD token=0;void* rt=nullptr;void* ds=nullptr;D3DVIEWPORT9 vp{};
    // Native replay draws call SetIndices, and d3d8to9 keeps the D3D8 base vertex index in its wrapper, outside
    // state blocks: put the game's index buffer and base back through SetIndices.
    void* indices=nullptr;UINT base=0;bool indicesOk=false;
    bool Capture(){indicesOk=SUCCEEDED(D8<86>(device,&indices,&base));HRESULT depth=D8<33>(device,&ds);HRESULT state=restoreToken?D8<55>(device,restoreToken):D8<57>(device,D3DSBT_ALL,&restoreToken);token=restoreToken;return SUCCEEDED(state)&&SUCCEEDED(D8<32>(device,&rt))&&(SUCCEEDED(depth)||depth==D3DERR_NOTFOUND)&&SUCCEEDED(D8<41>(device,&vp));}
    ~Restore(){if(indicesOk){D8<85>(device,indices,base);Release(indices);}if(rt)D8<31>(device,rt,ds);if(token){D8<54>(device,token);D8<40>(device,&vp);}Release(rt);Release(ds);}
};
using Matches=std::unordered_map<uint64_t,const Command*>;
Matches Match(const Frame& f){Matches result;for(auto& c:f.commands)if(c->kind==Command::Draw&&c->scene3d){auto p=result.emplace(c->key,c.get());if(!p.second)p.first->second=nullptr;}return result;}
bool Compatible(const Command& a,const Command& b){return a.canvas==b.canvas&&a.layout==b.layout&&a.stride==b.stride&&a.fvf==b.fvf&&a.data.size()==b.data.size()&&a.indices==b.indices&&!memcmp(&a.viewport,&b.viewport,sizeof(b.viewport));}
bool ViewMotion(const Matrix& pv,const Matrix& cv,const Matrix& pp,const Matrix& cp,bool canvas,float alpha,Matrix& view,Matrix& projection){
    if(!canvas)return Camera(pv,cv,alpha,view,settings.cutProtection!=0)&&Projection(pp,cp,alpha,projection,settings.cutProtection!=0);
    if(!Affine(pp)||!Affine(cp)||!Transform(pv,cv,alpha,view))return false;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)projection.m[i][j]=pp.m[i][j]+(cp.m[i][j]-pp.m[i][j])*alpha;
    return true;
}
bool SupportedMotion(const Command& p,const Command& c){
    if(!Compatible(p,c))return false;if(c.screenSpace)return settings.geometry!=0||menuFrame;
    Matrix view{},projection{},world{};
    if(!ViewMotion(p.view,c.view,p.projection,c.projection,c.canvas,.5f,view,projection))return false;
    return (c.canvas&&settings.menus)||settings.camera||(settings.geometry&&Transform(p.world,c.world,.5f,world));
}
bool Replayable(const Frame& f){
    // Reject feedback/postprocess paths that depend on pre-frame GPU contents.
    // Every written surface must first be fully initialized within this frame.
    std::unordered_map<void*,bool> initialized;std::unordered_map<uintptr_t,bool> writtenTextures;
    for(auto& c:f.commands){initialized.emplace(c->target,false);if(c->targetOwner)writtenTextures.emplace(c->targetOwner,false);}
    for(auto& p:f.commands){const auto& c=*p;
        if(c.kind==Command::Clear){SurfaceDesc8 desc{};bool full=SUCCEEDED(D8<8>(c.target,&desc))&&c.viewport.X==0&&c.viewport.Y==0&&c.viewport.Width==desc.width&&c.viewport.Height==desc.height;if((c.flags&D3DCLEAR_TARGET)&&c.clearRects.empty()&&full)initialized[c.target]=true;}
        else if(c.kind==Command::Copy){auto source=initialized.find(c.source);if(source!=initialized.end()&&!source->second)return false;if(c.copyRects.empty()&&c.points.empty())initialized[c.target]=true;else if(!initialized[c.target])return false;}
        else {if(!initialized[c.target])return false;for(auto texture:c.textures){auto found=writtenTextures.find(texture);if(texture&&found!=writtenTextures.end()&&!found->second)return false;}}
        if(initialized[c.target]&&c.targetOwner)writtenTextures[c.targetOwner]=true;
    }return !f.commands.empty();
}
bool Attributes(const Command& p,const Command& c,float alpha,std::vector<uint8_t>& mixed){
    size_t offset=c.screenSpace?16:12;if(c.fvf&D3DFVF_NORMAL)offset+=12;
    for(size_t i=0;i<mixed.size();i+=c.stride){
        if(c.screenSpace){float a=0,b=0;memcpy(&a,p.data.data()+i+12,4);memcpy(&b,c.data.data()+i+12,4);if(!std::isfinite(a)||!std::isfinite(b)||a<=0||b<=0)return false;float w=a+(b-a)*alpha;memcpy(mixed.data()+i+12,&w,4);}
        size_t at=offset;if(c.fvf&D3DFVF_PSIZE){if(at+4>c.stride)return false;float a=0,b=0;memcpy(&a,p.data.data()+i+at,4);memcpy(&b,c.data.data()+i+at,4);if(!std::isfinite(a)||!std::isfinite(b))return false;float size=a+(b-a)*alpha;memcpy(mixed.data()+i+at,&size,4);at+=4;}
        for(DWORD flag:{DWORD(D3DFVF_DIFFUSE),DWORD(D3DFVF_SPECULAR)})if(c.fvf&flag){if(at+4>c.stride)return false;
            if(memcmp(p.data.data()+i+at,c.data.data()+i+at,4))for(size_t channel=0;channel<4;channel++){float a=p.data[i+at+channel],b=c.data[i+at+channel];mixed[i+at+channel]=uint8_t(std::clamp(a+(b-a)*alpha,0.f,255.f)+.5f);}at+=4;}
    }return true;
}
std::vector<uint8_t> mixedScratch;
struct ViewPair {Matrix current,previous,projection,previousProjection;bool canvas;Matrix camera{},lens{};bool valid=false;};
struct PlannedDraw {const Command* prior=nullptr;int view=-1;bool unchanged=false,worldOk=false;};
struct ReplayPlan {std::vector<PlannedDraw> draws;std::vector<ViewPair> views;size_t usable=0;};
// Instances of one shared geometry (cloned clumps such as each teammate's jump
// ball, enemies of one type, rings) have equal keys, so Match() rejects them.
// Pair them by draw order while that order still fits their positions;
// otherwise by nearest position. Anything farther than PairDistance stays
// unmatched and renders at its current position, as before.
constexpr float PairDistance=250;constexpr size_t PairSearch=64;
V3 Anchor(const Command& c){V3 v;memcpy(&v,c.data.data(),sizeof(v));return Row(c.world,0)*v.x+Row(c.world,1)*v.y+Row(c.world,2)*v.z+Row(c.world,3);}
struct DuplicateGroup {std::vector<size_t> current;std::vector<const Command*> previous;};
void PairDuplicates(const Frame& f,const Frame& before,ReplayPlan& plan){
    auto eligible=[](const Command& c){return c.kind==Command::Draw&&c.scene3d&&!c.screenSpace&&!c.canvas&&c.data.size()>=sizeof(V3);};
    std::unordered_map<uint64_t,DuplicateGroup> groups;
    for(size_t i=0;i<f.commands.size();i++){const auto& c=*f.commands[i];if(eligible(c)&&!plan.draws[i].prior)groups[c.key].current.push_back(i);}
    if(groups.empty())return;
    for(auto& p:before.commands)if(eligible(*p)){auto g=groups.find(p->key);if(g!=groups.end())g->second.previous.push_back(p.get());}
    std::vector<V3> now,old;std::vector<std::tuple<float,size_t,size_t>> pairs;std::vector<std::pair<size_t,size_t>> greedy;std::vector<uint8_t> takenNow,takenBefore;
    for(auto& entry:groups){auto& g=entry.second;
        if(g.previous.empty()||(g.current.size()<2&&g.previous.size()<2))continue;
        // Compatible() is field equality: keep members equal to one reference.
        const Command& reference=*f.commands[g.current[0]];
        g.current.erase(std::remove_if(g.current.begin(),g.current.end(),[&](size_t i){return !Compatible(reference,*f.commands[i]);}),g.current.end());
        g.previous.erase(std::remove_if(g.previous.begin(),g.previous.end(),[&](const Command* p){return !Compatible(*p,reference);}),g.previous.end());
        const size_t n=g.current.size(),m=g.previous.size();if(!n||!m)continue;
        now.resize(n);old.resize(m);bool finite=true;
        for(size_t i=0;i<n;i++){now[i]=Anchor(*f.commands[g.current[i]]);finite=finite&&Finite(now[i]);}
        for(size_t j=0;j<m;j++){old[j]=Anchor(*g.previous[j]);finite=finite&&Finite(old[j]);}
        if(!finite)continue;
        auto distance=[&](size_t i,size_t j){V3 d=now[i]-old[j];return std::sqrt(Dot(d,d));};
        bool ordered=n==m;float orderedCost=0;
        for(size_t i=0;ordered&&i<n;i++){float d=distance(i,i);ordered=d<=PairDistance;orderedCost+=d;}
        greedy.clear();float greedyCost=0;
        if(!(ordered&&orderedCost<=.01f*n)&&n*m<=PairSearch*PairSearch){
            pairs.clear();for(size_t i=0;i<n;i++)for(size_t j=0;j<m;j++){float d=distance(i,j);if(d<=PairDistance)pairs.emplace_back(d,i,j);}
            std::sort(pairs.begin(),pairs.end());takenNow.assign(n,0);takenBefore.assign(m,0);
            for(auto& [d,i,j]:pairs)if(!takenNow[i]&&!takenBefore[j]){takenNow[i]=takenBefore[j]=1;greedy.emplace_back(i,j);greedyCost+=d;}
        }
        // A much shorter nearest assignment means the draw order changed.
        if(ordered&&(greedy.size()<n||orderedCost<=1.5f*greedyCost+.01f*n))for(size_t i=0;i<n;i++)plan.draws[g.current[i]].prior=g.previous[i];
        else for(auto [i,j]:greedy)plan.draws[g.current[i]].prior=g.previous[j];
    }
}
// before: the frame matches came from; enables pairing of duplicate keys.
ReplayPlan PlanReplay(const Frame& f,const Matches& matches,const Frame* before=nullptr){
    ReplayPlan plan;plan.draws.resize(f.commands.size());
    for(size_t i=0;i<f.commands.size();i++){const auto& c=*f.commands[i];if(c.kind!=Command::Draw||!c.scene3d)continue;
        auto it=matches.find(c.key);if(it!=matches.end()&&it->second&&Compatible(*it->second,c))plan.draws[i].prior=it->second;}
    if(before)PairDuplicates(f,*before,plan);
    for(size_t i=0;i<f.commands.size();i++){const auto& c=*f.commands[i];auto& draw=plan.draws[i];if(!draw.prior)continue;
        draw.unchanged=draw.prior->data==c.data;
        V3 delta=Row(draw.prior->world,3)-Row(c.world,3);draw.worldOk=!settings.cutProtection||Dot(delta,delta)<=250*250;
        if(!c.screenSpace){bool found=false;for(const auto& v:plan.views)if(!memcmp(&v.current,&c.view,sizeof(Matrix))&&!memcmp(&v.projection,&c.projection,sizeof(Matrix))){found=true;break;}
            if(!found)plan.views.push_back({c.view,draw.prior->view,c.projection,draw.prior->projection,c.canvas});}
    }
    // Validate a shared view once instead of decomposing the same camera for
    // every object during matching and again for every intermediate image.
    for(auto& v:plan.views)v.valid=ViewMotion(v.previous,v.current,v.previousProjection,v.projection,v.canvas,.5f,v.camera,v.lens);
    for(size_t i=0;i<f.commands.size();i++){const auto& c=*f.commands[i];auto& draw=plan.draws[i];if(c.kind!=Command::Draw||!c.scene3d)continue;
        if(!c.screenSpace)for(size_t v=0;v<plan.views.size();v++)if(!memcmp(&plan.views[v].current,&c.view,sizeof(Matrix))&&!memcmp(&plan.views[v].projection,&c.projection,sizeof(Matrix))){draw.view=int(v);break;}
        if(!draw.prior)continue;
        if(c.screenSpace){if(settings.geometry||menuFrame)++plan.usable;}
        else if(draw.view>=0&&plan.views[draw.view].valid){Matrix world{};if((c.canvas&&settings.menus)||settings.camera||(settings.geometry&&Transform(draw.prior->world,c.world,.5f,world)))++plan.usable;}
    }
    return plan;
}
// The game's buffers still hold what was captured (no Unlock since).
bool NativeCurrent(const Command& c){auto v=shadows.find(c.nativeVb);if(v==shadows.end()||v->second.writes!=c.vbWrites)return false;if(!c.nativeIb)return true;auto i=shadows.find(c.nativeIb);return i!=shadows.end()&&i->second.writes==c.ibWrites;}
bool Replay(const Frame& f,const Matches* matches,float alpha,const ReplayPlan* prepared=nullptr){
    timing::Scope timer(matches?timing::Replay:timing::Restore);
    Restore restore;if(!restore.Capture())return false;bool inScene=false,ok=true;auto& mixed=mixedScratch;
    ReplayPlan local;if(matches&&!prepared){local=PlanReplay(f,*matches);prepared=&local;}
    auto views=prepared?prepared->views:std::vector<ViewPair>{};
    bool cameraCut=false;for(auto& v:views){v.valid=ViewMotion(v.previous,v.current,v.previousProjection,v.projection,v.canvas,alpha,v.camera,v.lens);if(!v.valid)cameraCut=true;}
    auto end=[&](){if(inScene){if(FAILED(driverEnd(device)))ok=false;inScene=false;}};
    void* rt=nullptr;void* ds=nullptr;D3DVIEWPORT9 viewport{};bool viewportValid=false;
    for(size_t commandIndex=0;commandIndex<f.commands.size();commandIndex++){const auto& c=*f.commands[commandIndex];
        if(c.kind==Command::Copy){end();if(FAILED(oldCopy(device,c.source,c.copyRects.empty()?nullptr:c.copyRects.data(),c.count,c.target,c.points.empty()?nullptr:c.points.data())))ok=false;continue;}
        if(rt!=c.target||ds!=c.depth){end();if(FAILED(D8<31>(device,c.target,c.depth))){ok=false;break;}rt=c.target;ds=c.depth;viewportValid=false;}
        if(!viewportValid||memcmp(&viewport,&c.viewport,sizeof(viewport))){if(FAILED(D8<40>(device,&c.viewport))){ok=false;break;}viewport=c.viewport;viewportValid=true;}
        if(c.kind==Command::Clear){end();if(FAILED(oldClear(device,DWORD(c.clearRects.size()),c.clearRects.empty()?nullptr:c.clearRects.data(),c.flags,c.color,c.z,c.stencil)))ok=false;continue;}
        if(!inScene){if(FAILED(driverBegin(device))){ok=false;break;}inScene=true;}
        const void* vertices=c.data.data();Matrix world=c.world,camera=c.view,projection=c.projection;
        const PlannedDraw* draw=prepared?&prepared->draws[commandIndex]:nullptr;
        const ViewPair* view=draw&&draw->view>=0?&views[draw->view]:nullptr;
        // Shared camera interpolation is calculated once per distinct view, not
        // twice for every object. Unmatched geometry uses that same viewpoint.
        if(view&&view->valid&&(settings.camera||c.canvas)){camera=view->camera;projection=view->lens;}
        if(draw&&draw->prior){
            const auto& p=*draw->prior;
            bool cameraOk=c.screenSpace?!cameraCut:view&&view->valid;
            if(cameraOk&&draw->worldOk&&(settings.geometry||menuFrame)){
                bool transformOk=c.screenSpace||!memcmp(&p.world,&c.world,sizeof(Matrix))||Transform(p.world,c.world,alpha,world);
                if(transformOk){bool unchanged=draw->unchanged;
                    if(!unchanged&&Positions(p.data,c.data,c.stride,alpha,(c.fvf&D3DFVF_NORMAL)!=0,settings.cutProtection!=0,mixed)&&Attributes(p,c,alpha,mixed))vertices=mixed.data();
                    else if(!unchanged)world=c.world;
                }
            }
        }
        // Apply final interpolated transforms directly; avoid restoring the
        // native camera and then immediately replacing it for every draw.
        if(!pipeline::ApplyDraw(c.gpu,&world,&camera,&projection)){ok=false;break;}
        HRESULT hr;
        if(c.nativeVb&&vertices==c.data.data()&&NativeCurrent(c)){
            hr=D8<83>(device,UINT(0),c.nativeVb,c.stride);
            if(SUCCEEDED(hr))hr=c.nativeIb?(SUCCEEDED(hr=D8<85>(device,c.nativeIb,c.nativeBase))?driverIndexed(device,c.primitive,c.nativeMin,c.vertices,c.nativeStart,c.count):hr):driverDraw(device,c.primitive,c.nativeStart,c.count);
        }else hr=c.indices.empty()?driverUp(device,c.primitive,c.count,vertices,c.stride):driverIndexedUp(device,c.primitive,0,c.vertices,c.count,c.indices.data(),c.indexFormat,vertices,c.stride);
        if(FAILED(hr)){ok=false;break;}
    }
    end();return ok;
}
#include "deferred.h"
bool Synchronized(){
#ifdef MOTION_TEST
    return false;
#else
    const auto& p=Mem<Present8>(0xaa5140);return p.windowed?p.swap==D3DSWAPEFFECT(4):p.interval!=D3DPRESENT_INTERVAL_IMMEDIATE;
#endif
}
HRESULT Submit(void* d,const RECT* src,const RECT* dst,HWND hwnd,const RGNDATA* dirty,bool native){
    if(settings.showFps&&overlay){timing::Scope timer(timing::Overlay);Stats sample;GetStats(&sample);overlay(d,&sample);}
    HRESULT hr;{timing::Scope timer(timing::Present);hr=oldPresent(d,src,dst,hwnd,dirty);}
    if(SUCCEEDED(hr)){frameRates.Presented(Now(),native);timing::frame.maxPresentGapMs=std::max(timing::frame.maxPresentGapMs,double(frameRates.lastMs));++timing::frame.submissions;if(!native)++timing::frame.generated;}
    else timing::frame.reasons|=timing::Failed;
    return hr;
}
HRESULT __stdcall Present(void* d,const RECT* src,const RECT* dst,HWND hwnd,const RGNDATA* dirty){
    bool needNative=deferredPending;deferredPending=deferArmed=deferCandidate=false;
    timing::BeforePresent();timing::frame.draws=uint32_t(current.commands.size());timing::frame.bytes=uint32_t(current.bytes);
    bool eligible=capturing&&current.valid&&!src&&!dst&&!hwnd&&!dirty;capturing=false;
    {timing::Scope timer(timing::Match);if(eligible&&!Replayable(current)){eligible=false;counters.status=Unsupported;timing::frame.reasons|=timing::Unsupported|timing::FeedbackRejected;}}
    if(!eligible){if(needNative){replaying=true;bool restored=Replay(current,nullptr,1);replaying=false;if(!restored)timing::frame.reasons|=timing::Failed;}timing::frame.reasons|=timing::Inactive;previous=Frame{};return Submit(d,src,dst,hwnd,dirty,true);}
    current.time=Now();current.epoch=epoch;
    bool history=previous.valid&&!previous.commands.empty()&&previous.epoch==epoch&&current.time-previous.time<.05;
    if(!history)timing::frame.reasons|=timing::NoHistory;
    const bool unlocked=settings.targetHz==0;
    const unsigned requested=unlocked?0:std::max(1u,settings.targetHz/60);
    bool sync=Synchronized();bool queued=false;
    if(sync){const auto& pp=Mem<Present8>(0xaa5140);queued=!pp.windowed&&pp.interval==D3DPRESENT_INTERVAL_ONE&&requested>1;
        if(!queued){counters.status=NeedVsyncOff;history=false;timing::frame.reasons|=timing::Synchronized;}}
    queuedTick=queued;
    unsigned divisions=requested;double interval=divisions?1.0/(60*divisions):0;
    bool unmatched=false;Matches matches;ReplayPlan plan;
    {timing::Scope timer(timing::Match);if(history){matches=Match(previous);auto currentKeys=Match(current);for(auto& p:currentKeys)if(!p.second)matches.erase(p.first);plan=PlanReplay(current,matches,&previous);if(!plan.usable){history=false;unmatched=true;timing::frame.reasons|=timing::Unmatched;}}}
    replayCost=std::max(.00005,replayCost*.9);
    timing::frame.warmup=warmup;if(warmup){--warmup;history=false;timing::frame.reasons|=timing::Warming;}
    unsigned images=divisions?divisions-1:0;double first=tickStart+interval,step=interval;const double end=tickStart+1.0/60;
    if(history&&clockValid&&!unlocked&&!queued){
        // Keep the requested slots when the first one is reachable. A late native
        // frame presents its first image as soon as it can be replayed and spaces
        // the rest evenly up to the native image; alpha follows each image's time.
        // Dropping the whole tick to fewer slots made 180 alternate with 120.
        const double ready=Now()+replayCost+.0005;first=std::max(first,ready);
        while(images&&(end-first)/images<interval*.75)--images;
        if(images)step=(end-first)/images;
        if(images+1<requested){timing::frame.reasons|=timing::PacingReduced;timing::frame.skipped+=requested-1-images;}
    }
    timing::frame.plannedImages=images+1;
    unsigned presented=0;bool overwritten=false,ok=true;double lastPresentation=0;
    if(history&&clockValid){
        replaying=true;
        if(unlocked){
            // No presentation-rate cap. Leave time for the authoritative image
            // and never run simulation more than once per native 60 Hz tick.
            float lastAlpha=0;
            while(Now()+replayCost*2+.00025<end){
                float alpha=float(std::clamp((Now()+replayCost-tickStart)*60,0.0,.999));
                if(alpha<=lastAlpha)break;
                double start=Now();overwritten=true;ok=Replay(current,&matches,alpha,&plan);
                double cost=Now()-start;replayCost=std::clamp(replayCost*.75+cost*.25,.00005,.02);
                if(!ok)break;
                if(FAILED(Submit(d,nullptr,nullptr,nullptr,nullptr,false))){ok=false;break;}
                lastAlpha=alpha;++presented;
            }
        }else if(queued){
            // No deadlines: Present waits only while the FIFO is full, and the display paces the images.
            for(unsigned image=0;image<images;image++){
                double start=Now();overwritten=true;ok=Replay(current,&matches,float(image+1)/float(divisions),&plan);
                double cost=Now()-start;replayCost=std::clamp(replayCost*.75+cost*.25,.00005,.02);
                if(!ok)break;
                lastPresentation=Now();if(FAILED(Submit(d,nullptr,nullptr,nullptr,nullptr,false))){ok=false;break;}++presented;
            }
        }else for(unsigned image=0;image<images;image++){
            double deadline=first+image*step;
            if(Now()+replayCost>deadline-.0001||deadline-lastPresentation<interval*.75){++timing::frame.skipped;timing::frame.reasons|=timing::EarlySlotMiss;continue;}
            double start=Now();overwritten=true;ok=Replay(current,&matches,float(std::min(.999,(deadline-tickStart)*60)),&plan);
            double cost=Now()-start;replayCost=std::clamp(replayCost*.75+cost*.25,.00005,.02);
            if(!ok)break;
            // A finished image that is late is still shown unless the native image is under half a slot away;
            // dropping it left a gap of two or more slots for work already done.
            if(Now()>deadline+.0005&&end-Now()<interval*.5){++timing::frame.skipped;timing::frame.reasons|=timing::LateReplay;continue;}
            Until(deadline);lastPresentation=Now();HRESULT extra=Submit(d,nullptr,nullptr,nullptr,nullptr,false);
            if(FAILED(extra)){ok=false;break;}++presented;
        }
        if((overwritten||needNative)&&!Replay(current,nullptr,1))ok=false;needNative=false;
        replaying=false;
    }
    if(needNative){replaying=true;if(!Replay(current,nullptr,1))ok=false;replaying=false;}
    timing::frame.estimateMs=replayCost*1000;if(!ok)timing::frame.reasons|=timing::Failed;
    paired=presented>0&&ok;
    if(!sync||queued)counters.status=unmatched?Unsupported:paired&&(unlocked||presented+1==requested)?Active:history?TooSlow:Waiting;
    if(clockValid&&!queued)Until(tickStart+1.0/60);
    HRESULT hr=Submit(d,src,dst,hwnd,dirty,true);
    if(FAILED(hr)){ClearHistory();counters.status=DeviceLost;clockValid=false;}
    else {Recycle(previous);previous=std::move(current);current=Frame{};}
    return hr;
}
HRESULT __stdcall Reset(void* d,Present8* p){deferredPending=deferArmed=deferCandidate=false;nativeScene=false;capturing=false;ClearHistory();clockValid=false;paired=false;pacedTick=false;HRESULT hr=oldReset(d,p);pipeline::dirty=true;pipeline::recording=false;counters.status=FAILED(hr)?DeviceLost:Waiting;return hr;}
bool InstallDevice(void* d){
    if(!d)return false;if(device==d)return deviceReady;
    if(device){capturing=false;ClearHistory();Release(device);clockValid=false;paired=false;}
    device=d;Ref(device);auto t=*static_cast<void***>(d);
    // Some backends share a vtable across replacement devices. Do not insert
    // our hook twice or turn the saved chain into recursive self-calls.
    if(t[15]==reinterpret_cast<void*>(&Present)){deviceReady=true;return true;}
    // Capture before Video Settings installs its later hooks; those hooks still
    // flow through ours, and replay uses the driver functions saved here.
    driverUp=reinterpret_cast<UpFn>(t[72]);driverDraw=reinterpret_cast<DrawFn>(t[70]);driverIndexed=reinterpret_cast<IndexedFn>(t[71]);driverIndexedUp=reinterpret_cast<IndexedUpFn>(t[73]);driverBegin=reinterpret_cast<SceneFn>(t[34]);driverEnd=reinterpret_cast<SceneFn>(t[35]);
    bool ok=Slot(t,23,&CreateVB,oldCreateVB)&&Slot(t,24,&CreateIB,oldCreateIB)&&Slot(t,70,&Draw,oldDraw)&&Slot(t,71,&Indexed,oldIndexed)&&Slot(t,72,&Up,oldUp)&&Slot(t,73,&IndexedUp,oldIndexedUp)&&Slot(t,36,&Clear,oldClear)&&Slot(t,28,&Copy,oldCopy)&&Slot(t,29,&UpdateTexture,oldUpdateTexture)&&Slot(t,15,&Present,oldPresent)&&Slot(t,14,&Reset,oldReset);
    ok=ok&&pipeline::Install(t)&&InstallDeferred(t);deviceReady=ok;if(!ok)counters.status=Incompatible;return ok;
}
using ActionDrawFn=void(__stdcall*)(void*);ActionDrawFn oldActionDraw=nullptr;
bool ActionMenu(uint32_t state){return state==6||state==0xf;}
bool ActionSupported(uint32_t state){return state==5||(settings.menus&&ActionMenu(state));}
void BeginFrame(bool eligible,bool menu,uint64_t identity){
    FlushDeferred();timing::BeginDraw(menu,identity,0);pipeline::dirty=true;
    if(!eligible||context!=identity){ClearHistory();context=identity;clockValid=false;replayCost=.0005;}
    deferCandidate=eligible&&clockValid&&!warmup&&previous.valid&&!previous.commands.empty();
    Recycle(current);PrepareLayoutHistory();texturesRead.clear();capturing=eligible;pacedTick=eligible&&clockValid;hudPhase=false;menuFrame=menu;current.epoch=epoch;
    if(!settings.enabled)counters.status=Off;else if(!eligible)counters.status=device&&!deviceReady?Incompatible:Waiting;
}
void BeginActionFrame(bool eligible,uintptr_t nextStage,uint32_t state){
    // The native loop still owns pause/result updates. Only drawing is captured.
    // Never blend a frozen pause pose or results UI across a gameplay transition.
    uint64_t identity=(uint64_t(nextStage)<<32)|(state==5?1u:(state<<8)|1u);
    BeginFrame(eligible,ActionMenu(state),identity);timing::frame.action=state;
}
void __stdcall ActionDraw(void* action){
#ifndef MOTION_TEST
    auto config=Mem<uintptr_t>(0xa777e4);uintptr_t nextStage=Mem<uintptr_t>(0x9dd8b0);
    uint32_t state=Mem<uint32_t>(reinterpret_cast<uintptr_t>(action)+8);
    bool eligible=installed&&settings.enabled&&deviceReady&&device&&config&&ActionSupported(state)&&Mem<uint8_t>(config+0x1e)==0&&Mem<int>(0xa60b80)==Mem<int>(0xa60b84)&&Mem<int>(0x7c8bf4)==1;
    stage=nextStage;BeginActionFrame(eligible,nextStage,state);
#endif
    oldActionDraw(action);capturing=false;
}
void __cdecl BeginMenu(void* object){
#ifndef MOTION_TEST
    uintptr_t at=reinterpret_cast<uintptr_t>(object);
    bool eligible=installed&&settings.enabled&&settings.menus&&deviceReady&&device&&Mem<int>(at+4)==4&&Mem<int>(at+0xc)!=0xf&&Mem<uint8_t>(at+0x64)!=0&&Mem<int>(at+0x68)!=0;
    BeginFrame(eligible,true,(uint64_t(at)<<32)|uint32_t(Mem<int>(at+0xc))<<8|2);timing::frame.action=Mem<uint32_t>(at+4);
#else
    (void)object;
#endif
}
void __cdecl EndMenu(){capturing=false;}
void* oldMenuDraw=reinterpret_cast<void*>(0x4055e0);
// Advertise::Disp takes its object in EDI, no stack arguments. Preserve that
// contract through the original call; all input/Exec work remains outside it.
__declspec(naked) void MenuDraw(){
    __asm {
        push edi
        call BeginMenu
        add esp,4
        call dword ptr[oldMenuDraw]
        call EndMenu
        ret
    }
}
using SkinFn=int(__cdecl*)(void*,void*);SkinFn oldSkin=nullptr;
int __cdecl Skin(void* node,void* params){auto saved=skinOwner;skinOwner=params?*static_cast<uintptr_t*>(params):0;int result=oldSkin(node,params);skinOwner=saved;return result;}
using HudFn=void(__stdcall*)(void*);HudFn oldHud=nullptr;
void __stdcall Hud(void* action){hudPhase=true;oldHud(action);}
void __cdecl Wait(){
#ifndef MOTION_TEST
    if(installed&&Mem<void*>(0x8e0be8)!=device)InstallDevice(Mem<void*>(0x8e0be8));
#endif
    // Every tick after the first uses our 60 Hz clock, including native-only
    // frames, FPS 60 and V-Sync. Retail waits on CRI's PulseEvent (0x6c575e); a
    // pulse sent before the wait begins is lost, doubling that frame. The CRI
    // timer and its sound processing keep running; only this wait is replaced.
    bool paced=tickStart>0;double boundary=tickStart+(queuedTick?.995:1.0)/60;
    if(paced){
        Timer(true);Until(boundary);
#ifndef MOTION_TEST
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(0x8e2118));
#endif
    }else{
#ifndef MOTION_TEST
        {timing::Scope timer(timing::Wait);reinterpret_cast<void(__cdecl*)()>(0x6c4fc0)();}
#endif
    }
    // The surrounding native sync routine still polls peripherals/counters.
    paired=false;pacedTick=false;double now=Now();
    // Do not add Present/cleanup/input overhead to every simulation tick. Keep
    // the scheduled boundary, abandoning it after a genuinely missed frame.
    tickStart=paced&&now-boundary<1.0/120?boundary:now;clockValid=true;
    timing::FinishTick(settings.targetHz,uint32_t(counters.status));
}
bool Save(){
    if(ini.empty())return true;auto tmp=ini+L".tmp";std::ofstream out(tmp,std::ios::trunc);out<<"[Motion]\nEnabled="<<settings.enabled<<"\nCamera="<<settings.camera<<"\nGeometry="<<settings.geometry<<"\nCutProtection="<<settings.cutProtection<<"\nTargetHz="<<settings.targetHz<<"\nMenus="<<settings.menus<<"\nShowFps="<<settings.showFps<<"\nLogging="<<settings.logging<<"\n";out.close();return out&&MoveFileExW(tmp.c_str(),ini.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
void __cdecl Get(Settings* out){if(out&&out->size==sizeof(Settings))*out=settings;}
int __cdecl Set(const Settings* input){if(!input||input->size!=sizeof(Settings)||input->enabled>1||input->camera>1||input->geometry>1||input->cutProtection>1||input->menus>1||input->showFps>1||input->logging>1||!ValidFps(input->targetHz))return 0;auto saved=settings;settings=*input;settings.enabled=settings.targetHz!=60;if(!Save()){settings=saved;return 0;}timing::enabled.store(settings.logging!=0,std::memory_order_relaxed);bool renderingChanged=saved.enabled!=settings.enabled||saved.camera!=settings.camera||saved.geometry!=settings.geometry||saved.cutProtection!=settings.cutProtection||saved.targetHz!=settings.targetHz||saved.menus!=settings.menus;if(renderingChanged){ClearHistory();clockValid=false;paired=pacedTick=false;counters.status=settings.enabled?Waiting:Off;}return 1;}
void __cdecl GetStats(Stats* out){if(out&&out->size==sizeof(Stats)){*out=counters;double now=Now();out->renderFps=frameRates.output.At(now);out->baseFps=frameRates.base.At(now);out->frameMs=frameRates.lastMs;out->peakMs=frameRates.Peak(now);out->loggingError=timing::ioError.load(std::memory_order_relaxed);}}
void __cdecl SetOverlay(Overlay draw){overlay=draw;}
const Api api{sizeof(Api),Abi,&Get,&Set,&GetStats,&SetOverlay};
void* Trampoline(uintptr_t address,const std::vector<uint8_t>& bytes){
    if(memcmp(reinterpret_cast<void*>(address),bytes.data(),bytes.size()))return nullptr;
    auto p=static_cast<uint8_t*>(VirtualAlloc(nullptr,bytes.size()+5,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!p)return nullptr;memcpy(p,bytes.data(),bytes.size());p[bytes.size()]=0xe9;*reinterpret_cast<int32_t*>(p+bytes.size()+1)=int32_t(address+bytes.size()-reinterpret_cast<uintptr_t>(p+bytes.size()+5));DWORD old;VirtualProtect(p,bytes.size()+5,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),p,bytes.size()+5);return p;
}
bool Jump(uintptr_t address,void* to,size_t n,uint8_t op=0xe9){std::vector<uint8_t> bytes(n,0x90);bytes[0]=op;*reinterpret_cast<int32_t*>(bytes.data()+1)=int32_t(reinterpret_cast<uintptr_t>(to)-address-5);return Write(reinterpret_cast<void*>(address),bytes.data(),n);}
bool KnownExe(){return game_code::Supported();}
}
extern "C" __declspec(dllexport) const motion::Api* __cdecl HeroesMotionApi(uint32_t version){return version==motion::Abi&&fi::installed?&fi::api:nullptr;}
extern "C" __declspec(dllexport) void __cdecl ReloadedStart(){
    using namespace fi;static bool started=false;if(started)return;started=true;
    if(GetModuleHandleW(nullptr)!=reinterpret_cast<HMODULE>(0x400000)||!KnownExe()){counters.status=Incompatible;return;}
    const uint8_t call[]={0xe8,0x52,0x1e,0x28,0}; // 443169 -> 6c4fc0
    if(memcmp(reinterpret_cast<void*>(0x443169),call,5)){counters.status=Incompatible;return;}
    const uint8_t menuCall[]={0xe8,0xae,0,0,0}; // 40552d -> 4055e0, EDI object
    if(memcmp(reinterpret_cast<void*>(0x40552d),menuCall,5)){counters.status=Incompatible;return;}
    oldActionDraw=reinterpret_cast<ActionDrawFn>(Trampoline(0x403430,{0x83,0xec,0x0c,0x53,0x55}));
    oldSkin=reinterpret_cast<SkinFn>(Trampoline(0x6a08d0,{0x83,0xec,0x20,0x8b,0x54,0x24,0x28}));
    oldHud=reinterpret_cast<HudFn>(Trampoline(0x403350,{0xa1,0xb0,0xd8,0x9d,0}));
    if(!oldActionDraw||!oldSkin||!oldHud){counters.status=Incompatible;return;}
    wchar_t path[MAX_PATH];GetModuleFileNameW(module,path,MAX_PATH);auto userMods=std::filesystem::path(path).parent_path().parent_path().parent_path()/L"User"/L"Mods";
#ifdef MODERN_VIDEO
    auto folder=userMods/L"sonicheroes.graphics.modern-video";constexpr bool allowLogging=false; // no logging setting in the user build
#else
    auto folder=userMods/L"sonicheroes.graphics.frame-interpolation";constexpr bool allowLogging=true;
#endif
    std::error_code ec;std::filesystem::create_directories(folder,ec);ini=(folder/L"Motion.ini").wstring();
#ifdef MODERN_VIDEO
    if(!std::filesystem::exists(ini,ec))std::filesystem::copy_file(userMods/L"sonicheroes.graphics.frame-interpolation"/L"Motion.ini",ini,ec);
#endif
    auto load=[&](const wchar_t* key,uint32_t fallback){return GetPrivateProfileIntW(L"Motion",key,fallback,ini.c_str())?1u:0u;};settings.enabled=load(L"Enabled",0);settings.camera=load(L"Camera",1);settings.geometry=load(L"Geometry",1);settings.cutProtection=load(L"CutProtection",1);
    settings.targetHz=GetPrivateProfileIntW(L"Motion",L"TargetHz",60,ini.c_str());if(!settings.enabled||!ValidFps(settings.targetHz))settings.targetHz=60;settings.enabled=settings.targetHz!=60;settings.menus=load(L"Menus",1);
    settings.showFps=load(L"ShowFps",0);settings.logging=allowLogging?load(L"Logging",1):0;timing::enabled=settings.logging!=0;if(allowLogging)timing::Start(folder/L"logs");
    installed=Jump(0x403430,reinterpret_cast<void*>(&ActionDraw),5)&&Jump(0x6a08d0,reinterpret_cast<void*>(&Skin),7)&&Jump(0x403350,reinterpret_cast<void*>(&Hud),5)&&Jump(0x443169,reinterpret_cast<void*>(&Wait),5,0xe8)&&Jump(0x40552d,reinterpret_cast<void*>(&MenuDraw),5,0xe8);
    counters.status=installed?(settings.enabled?Waiting:Off):Incompatible;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){fi::module=h;DisableThreadLibraryCalls(h);}else if(reason==DLL_PROCESS_DETACH){fi::shuttingDown=true;timing::stop.store(true,std::memory_order_release);}return TRUE;}
