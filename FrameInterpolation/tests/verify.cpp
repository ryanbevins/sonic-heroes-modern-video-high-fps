#define MOTION_TEST
#include "../src/mod.cpp"
#include <iostream>
#include <cstdlib>
#include <thread>
using namespace fi;
int tests=0;
void Check(bool ok,const char* name){if(!ok){std::cerr<<"FAIL: "<<name<<"\n";std::exit(1);}++tests;std::cout<<"PASS: "<<name<<"\n";}
bool Near(float a,float b){return std::abs(a-b)<.0002f;}
uint64_t Pixels(){void* back=nullptr;Check(SUCCEEDED(D8<16>(device,UINT(0),D3DBACKBUFFER_TYPE_MONO,&back)),"get fixture backbuffer");D3DLOCKED_RECT lock{};Check(SUCCEEDED(D8<9>(back,&lock,static_cast<const RECT*>(nullptr),DWORD(D3DLOCK_READONLY))),"read actual rendered pixels");uint64_t h=14695981039346656037ull;for(int y=0;y<128;y++)h=Hash(static_cast<char*>(lock.pBits)+y*lock.Pitch,128*4,h);D8<10>(back);Release(back);return h;}
struct Vertex {float x,y,z,nx,ny,nz;DWORD color;};
constexpr DWORD Fvf=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE;
int updates=0;
PresentFn nativePresent=nullptr;
std::vector<double> presentTimes;
std::vector<uint64_t> presentPixels;bool samplePixels=false,stallPresent=false;
unsigned overlayCalls=0;
void __cdecl ProbeOverlay(void*,const Stats* stats){Check(!capturing&&stats->size==sizeof(Stats),"FPS callback runs outside frame capture");++overlayCalls;}
HRESULT __stdcall ProbePresent(void* d,const RECT* a,const RECT* b,HWND window,const RGNDATA* data){presentTimes.push_back(Now());if(samplePixels)presentPixels.push_back(Pixels());if(stallPresent){stallPresent=false;Sleep(20);}return nativePresent(d,a,b,window,data);}
void* vb=nullptr;void* ib=nullptr;
void Fixture(float amount,float hudShift=0){
    ++updates;PrepareLayoutHistory();current=Frame{};current.epoch=epoch;capturing=true;hudPhase=false;
    Matrix world=Identity(),view=Identity(),proj{};view.m[3][0]=-amount*.2f;
    for(int i=0;i<4;i++)view.m[i][0]=-view.m[i][0]; // Actual RenderWare camera handedness.
    proj.m[0][0]=1;proj.m[1][1]=1;proj.m[2][2]=10.f/9.9f;proj.m[2][3]=1;proj.m[3][2]=-1.f/9.9f;
    D8<37>(device,D3DTS_WORLD,&world);D8<37>(device,D3DTS_VIEW,&view);D8<37>(device,D3DTS_PROJECTION,&proj);
    D8<50>(device,D3DRS_LIGHTING,DWORD(FALSE));D8<50>(device,D3DRS_CULLMODE,DWORD(D3DCULL_NONE));D8<50>(device,D3DRS_ZENABLE,DWORD(D3DZB_TRUE));D8<50>(device,D3DRS_ALPHABLENDENABLE,DWORD(FALSE));
    D8<61>(device,DWORD(0),static_cast<void*>(nullptr));D8<63>(device,DWORD(0),DWORD(D3DTSS_COLOROP),DWORD(D3DTOP_SELECTARG1));D8<63>(device,DWORD(0),DWORD(D3DTSS_COLORARG1),DWORD(D3DTA_DIFFUSE));D8<76>(device,Fvf);
    Check(SUCCEEDED(D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER),DWORD(0xff101010),1.f,DWORD(0))),"capture full framebuffer clear");
    Check(SUCCEEDED(D8<34>(device)),"begin native fixture frame");
    Vertex verts[5]={{0,0,2,0,0,-1,0xffff0000},{0,0,2,0,0,-1,0xffff0000},
        {-.9f+amount*.8f,-.6f,2,0,0,-1,0xffff0000},{-.3f+amount*.8f,-.6f,2,0,0,-1,0xffff0000},{-.6f+amount*.8f,.2f+amount*.5f,2,0,0,-1,0xffff0000}};
    BYTE* data=nullptr;Check(SUCCEEDED(D8<11>(vb,UINT(0),UINT(sizeof(verts)),&data,DWORD(D3DLOCK_DISCARD))),"lock WRITEONLY dynamic skinned fixture buffer");memcpy(data,verts,sizeof(verts));D8<12>(vb);
    D8<83>(device,UINT(0),vb,UINT(sizeof(Vertex)));D8<85>(device,ib,UINT(1));skinOwner=0x1234;
    Check(SUCCEEDED(D8<71>(device,D3DPT_TRIANGLELIST,UINT(1),UINT(3),UINT(0),UINT(1))),"capture indexed skin with nonzero base and min vertex");skinOwner=0;
    Vertex particle[]={{.1f,-.4f+amount*.3f,2,0,0,-1,0xff00ff00},{.6f,-.4f+amount*.3f,2,0,0,-1,0xff00ff00},{.35f,.1f+amount*.3f,2,0,0,-1,0xff00ff00}};
    Check(SUCCEEDED(D8<72>(device,D3DPT_TRIANGLELIST,UINT(1),particle,UINT(sizeof(Vertex)))),"capture independent immediate geometry effect");
    struct HUD {float x,y,z,rhw;DWORD color;};DWORD alpha=DWORD(255-amount*100)<<24;HUD effect[]={{85+amount*10,80,.5f,.5f,alpha|0xff00ff},{100+amount*10,80,.5f,.5f,alpha|0xff00ff},{85+amount*10,100,.5f,.5f,alpha|0xff00ff}};
    D8<76>(device,DWORD(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));D8<50>(device,D3DRS_ALPHABLENDENABLE,DWORD(TRUE));D8<50>(device,D3DRS_SRCBLEND,DWORD(D3DBLEND_SRCALPHA));D8<50>(device,D3DRS_DESTBLEND,DWORD(D3DBLEND_INVSRCALPHA));
    Check(SUCCEEDED(D8<72>(device,D3DPT_TRIANGLELIST,UINT(1),effect,UINT(sizeof(HUD)))),"capture preprojected world effect with fade");
    struct HudVertex{float x,y,z,w;DWORD color;float u,v;};
    hudPhase=true;HudVertex hud[]={{3+hudShift,3,0,1,0xff0000ff,0,0},{23+hudShift,3,0,1,0xff0000ff,1,0},{3+hudShift,23,0,1,0xff0000ff,0,1}};D8<50>(device,D3DRS_ALPHABLENDENABLE,DWORD(FALSE));
    D8<76>(device,DWORD(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1));Check(SUCCEEDED(D8<72>(device,D3DPT_TRIANGLELIST,UINT(1),hud,UINT(sizeof(HudVertex)))),"capture HUD overlay");
    Check(SUCCEEDED(D8<35>(device)),"end native fixture frame");capturing=false;Check(current.valid&&current.commands.size()==5,"entire frame captured with four draw commands");
}
void BufferMirrorTests(){
    void* buffer=nullptr;Check(SUCCEEDED(D8<23>(device,UINT(64),DWORD(D3DUSAGE_DYNAMIC),DWORD(0),D3DPOOL_DEFAULT,&buffer)),"create real driver buffer for mirror semantics");
    std::array<DWORD,16> expected{};for(size_t i=0;i<expected.size();i++)expected[i]=DWORD(i*17+9);
    BYTE* data=nullptr;auto& shadow=shadows.at(buffer);
    Check(SUCCEEDED(D8<11>(buffer,UINT(0),UINT(0),&data,DWORD(0)))&&data==shadow.locked,"unknown buffer contents retain the original driver lock pointer");memcpy(data,expected.data(),64);D8<12>(buffer);
    Check(SUCCEEDED(D8<11>(buffer,UINT(16),UINT(8),&data,DWORD(D3DLOCK_NOOVERWRITE)))&&data==shadow.locked,"partial NOOVERWRITE lock retains the native driver pointer");
    Check((reinterpret_cast<uintptr_t>(data)&15)==0&&memcmp(data,expected.data()+4,8)==0,"native lock preserves prior contents and SIMD alignment");expected[4]=123;expected[5]=456;memcpy(data,expected.data()+4,8);D8<12>(buffer);
    auto hooks=bufferHooks.at(*static_cast<void***>(buffer));BYTE* original=nullptr;Check(SUCCEEDED(hooks.lock(buffer,0,0,&original,D3DLOCK_READONLY))&&memcmp(original,expected.data(),64)==0,"unlock uploads modified range and preserves all untouched driver bytes");hooks.unlock(buffer);
    Check(SUCCEEDED(D8<11>(buffer,UINT(0),UINT(64),&data,DWORD(D3DLOCK_READONLY)))&&data==shadow.locked,"read-only lock retains native pointer and behavior");D8<12>(buffer);
    Check(SUCCEEDED(D8<11>(buffer,UINT(0),UINT(64),&data,DWORD(D3DLOCK_DISCARD)))&&data==shadow.locked&&shadow.valid.empty(),"discard preserves native pointer and invalidates old mirror spans");memcpy(data,expected.data(),64);D8<12>(buffer);
    std::vector<uint8_t> captured;Check(shadow.Read(0,64,captured)&&memcmp(captured.data(),expected.data(),64)==0,"unlock mirrors exact bytes for interpolation capture");
    Check(SUCCEEDED(hooks.lock(buffer,0,0,&original,D3DLOCK_READONLY))&&memcmp(original,expected.data(),64)==0,"discard upload reaches the actual driver buffer unchanged");hooks.unlock(buffer);Release(buffer);
}
unsigned nativeDrawCount=0;
IndexedFn countedIndexed=nullptr;UpFn countedUp=nullptr;
HRESULT __stdcall CountIndexed(void* d,D3DPRIMITIVETYPE p,UINT min,UINT n,UINT start,UINT count){++nativeDrawCount;return countedIndexed(d,p,min,n,start,count);}
HRESULT __stdcall CountUp(void* d,D3DPRIMITIVETYPE p,UINT n,const void* data,UINT stride){++nativeDrawCount;return countedUp(d,p,n,data,stride);}
void DeferredTests(){
    ClearHistory();Fixture(1);uint64_t expected=Pixels();
    countedIndexed=oldIndexed;countedUp=oldUp;oldIndexed=&CountIndexed;oldUp=&CountUp;
    Fixture(0);previous=std::move(current);deferCandidate=true;nativeDrawCount=0;Fixture(1);
    Check(deferredPending&&deferArmed&&nativeDrawCount==0,"deferred native pass retains all four draws without submitting duplicates");
    void* stream=nullptr;UINT stride=0;D8<84>(device,UINT(0),&stream,&stride);Check(stream==nullptr,"deferred UP draws preserve native stream-unbind side effect");Release(stream);
    nativePresent=oldPresent;oldPresent=&ProbePresent;settings.targetHz=180;warmup=0;clockValid=true;capturing=true;previous.time=Now();tickStart=Now();replayCost=.00005;presentTimes.clear();presentPixels.clear();samplePixels=true;
    int before=updates;Check(SUCCEEDED(Present(device,nullptr,nullptr,nullptr,nullptr)),"deferred presentation succeeds on real hardware");samplePixels=false;
    Check(presentPixels.size()==3&&presentPixels[0]!=presentPixels[1]&&presentPixels[2]==expected,"deferred frame produces two distinct interpolants and exact native final image");
    Check(!deferredPending&&nativeDrawCount==0&&updates==before,"deferred presentation adds no native draw submissions or simulation updates");Wait();
    // A readback must see the completed native image, even before Present.
    deferCandidate=true;Fixture(1);Check(deferredPending,"readback fixture has genuinely pending GPU work");
    Check(Pixels()==expected&&!deferredPending&&!deferArmed,"surface readback barrier materializes the exact native image");
    // Failure in a later draw must recover the retained prefix while a native
    // BeginScene is active, and restore its state/scene contract for the caller.
    deferCandidate=true;Fixture(1);Check(deferredPending,"rejection fixture starts with pending draws");D8<34>(device);capturing=true;
    Invalidate(Unsupported,timing::StateRejected);Check(nativeScene&&!deferredPending&&!capturing,"capture rejection flushes pending prefix and resumes native scene");Check(SUCCEEDED(D8<35>(device))&&Pixels()==expected,"rejected capture still displays the complete native image");
    // Missing history / warmup must not present the blank initial clear.
    for(bool noHistory:{false,true}){Fixture(0);previous=std::move(current);deferCandidate=true;Fixture(1);previous.time=Now();if(noHistory)previous.valid=false;warmup=noHistory?0:2;capturing=true;tickStart=Now();presentTimes.clear();presentPixels.clear();samplePixels=true;
        Check(SUCCEEDED(Present(device,nullptr,nullptr,nullptr,nullptr)),"deferred warmup/history fallback presentation succeeds");samplePixels=false;
        Check(presentPixels.size()==1&&presentPixels.back()==expected,"deferred fallback presents native geometry instead of a blank image");Wait();}
    // Target switches force completion before changing the destination.
    deferCandidate=true;Fixture(1);void* rt=nullptr;void* ds=nullptr;D8<32>(device,&rt);D8<33>(device,&ds);
    Check(SUCCEEDED(D8<31>(device,rt,ds))&&deferredPending,"redundant framebuffer bind does not force duplicate native rendering");Check(SUCCEEDED(D8<31>(device,rt,static_cast<void*>(nullptr)))&&!deferredPending&&Pixels()==expected,"changed render-target barrier preserves the pending native image");D8<31>(device,rt,ds);Release(rt);Release(ds);
    // A texture mutation must not modify the image represented by queued draws.
    void* texture=nullptr;D8<20>(device,UINT(2),UINT(2),UINT(1),DWORD(0),D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture);ObserveTexture(texture);
    deferCandidate=true;Fixture(1);D3DLOCKED_RECT locked{};Check(deferredPending&&SUCCEEDED(D8<16>(texture,UINT(0),&locked,static_cast<const RECT*>(nullptr),DWORD(0))),"texture mutation flushes pending native work before locking");D8<17>(texture,UINT(0));Check(!deferredPending&&Pixels()==expected,"texture mutation barrier preserves the native prefix");Release(texture);
    deferCandidate=true;Fixture(1);D3DRECT area{0,0,64,128};D8<36>(device,DWORD(1),&area,DWORD(D3DCLEAR_TARGET),DWORD(0xff121212),1.f,DWORD(0));auto partial=Pixels();
    Fixture(1);D8<36>(device,DWORD(1),&area,DWORD(D3DCLEAR_TARGET),DWORD(0xff121212),1.f,DWORD(0));Check(!deferredPending&&Pixels()==partial,"partial-clear barrier preserves ordering against native rendering");
    // Character select clears depth between the background and 3D characters.
    auto layered=[](){capturing=true;D8<76>(device,DWORD(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_ZBUFFER),DWORD(0),1.f,DWORD(0));D8<34>(device);
        struct V{float x,y,z,w;DWORD color;};V v[]={{60,60,0,1,0xff00ffff},{110,60,0,1,0xff00ffff},{60,110,0,1,0xff00ffff}};D8<72>(device,D3DPT_TRIANGLELIST,UINT(1),v,UINT(sizeof(V)));D8<35>(device);
        D3DRECT rect{0,0,16,16};D8<36>(device,DWORD(1),&rect,DWORD(D3DCLEAR_TARGET),DWORD(0xff123456),1.f,DWORD(0));capturing=false;};
    Fixture(1);layered();auto layeredNative=Pixels();deferCandidate=true;Fixture(1);layered();
    Check(deferredPending&&deferArmed,"depth and partial clears retain deferred geometry in command order");Check(Pixels()==layeredNative,"multiple clears and later geometry match independently rendered native pixels");
    oldPresent=nativePresent;oldIndexed=countedIndexed;oldUp=countedUp;settings.targetHz=120;ClearHistory();
}
void ActionMenuTests(){
    auto saved=settings;settings.menus=1;settings.camera=settings.geometry=0;
    Check(ActionSupported(5)&&ActionSupported(6)&&ActionSupported(15),"gameplay, pause and results are supported render states");
    for(uint32_t state:{0u,1u,2u,3u,4u,7u,8u,9u,10u,11u,12u,13u,14u,16u,17u,18u,19u,20u})Check(!ActionSupported(state),"loading and other unverified action states remain excluded");
    settings.menus=0;Check(ActionSupported(5)&&!ActionSupported(6)&&!ActionSupported(15),"menu toggle controls pause/results without disabling gameplay");settings.menus=1;
    nativePresent=oldPresent;oldPresent=&ProbePresent;
    for(uint32_t state:{6u,15u}){
        BeginActionFrame(true,0x1234,5);Fixture(0);previous=std::move(current);auto oldContext=context;
        BeginActionFrame(true,0x1234,state);
        Check(previous.commands.empty()&&context!=oldContext&&!clockValid&&warmup==3&&menuFrame,"entering pause/results discards gameplay interpolation history");
        Fixture(1,0);auto first=Pixels();previous=std::move(current);Fixture(1,12);auto last=Pixels();
        Check(current.commands.back()->scene3d&&current.commands.back()->screenSpace,"pause/results overlay participates after the native HUD boundary");
        auto matches=Match(previous);replaying=true;
        Check(Replay(current,&matches,.5f)&&Pixels()!=first&&Pixels()!=last,"moving pause/results overlay renders a distinct midpoint over a frozen scene");
        Check(Replay(current,nullptr,1)&&Pixels()==last,"pause/results replay restores exact native final pixels");replaying=false;
        for(uint32_t target:{180u,0u}){
            Fixture(1,24);capturing=true;previous.time=Now();tickStart=Now();clockValid=true;warmup=0;settings.targetHz=target;replayCost=.00005;presentTimes.clear();int before=updates;
            Check(SUCCEEDED(Present(device,nullptr,nullptr,nullptr,nullptr))&&presentTimes.size()>1,"pause/results submit extra images at capped and unlocked FPS");
            Check(updates==before,"extra pause/results images do not advance simulation or menu logic");Wait();
        }
        BeginActionFrame(true,0x1234,5);Check(previous.commands.empty()&&!menuFrame&&!clockValid&&warmup==3,"resuming gameplay discards menu poses and restores HUD exclusion");
        Fixture(1);Check(!current.commands.back()->scene3d,"ordinary gameplay HUD stays outside motion interpolation");
    }
    oldPresent=nativePresent;settings=saved;ClearHistory();menuFrame=false;
}
void GameplayPrefixTests(){
    ClearHistory();void* back=nullptr;void* depth=nullptr;void* smallDepth=nullptr;
    D8<32>(device,&back);D8<33>(device,&depth);
    std::array<void*,2> textures{},surfaces{};
    for(size_t i=0;i<textures.size();++i){
        Check(SUCCEEDED(D8<20>(device,UINT(32),UINT(32),UINT(1),DWORD(D3DUSAGE_RENDERTARGET),D3DFMT_X8R8G8B8,D3DPOOL_DEFAULT,&textures[i])),"create gameplay prepass render texture");
        Check(SUCCEEDED(D8<15>(textures[i],UINT(0),&surfaces[i])),"obtain prepass target surface");
    }
    Check(SUCCEEDED(D8<26>(device,UINT(32),UINT(32),D3DFMT_D16,D3DMULTISAMPLE_NONE,&smallDepth)),"create depth surface shared by gameplay prepasses");
    struct V{float x,y,z,w;DWORD color;float u,v;};
    auto quad=[&](float x,float width,DWORD color){V vertices[]={{x,0,.5f,1,color,0,0},{x+width,0,.5f,1,color,1,0},{x,width,.5f,1,color,0,1},{x+width,width,.5f,1,color,1,1}};
        Check(SUCCEEDED(D8<72>(device,D3DPT_TRIANGLESTRIP,UINT(2),vertices,UINT(sizeof(V)))),"draw prepass fixture quad");};
    auto frame=[&](bool deferred,bool missingColor=false,bool missingDepth=false){
        ClearHistory();deferCandidate=deferred;capturing=true;hudPhase=true;menuFrame=false;
        Matrix identity=Identity();D8<37>(device,D3DTS_WORLD,&identity);D8<37>(device,D3DTS_VIEW,&identity);D8<37>(device,D3DTS_PROJECTION,&identity);
        D8<76>(device,DWORD(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1));D8<88>(device,DWORD(0));
        for(DWORD state:{DWORD(D3DRS_LIGHTING),DWORD(D3DRS_FOGENABLE),DWORD(D3DRS_ALPHABLENDENABLE),DWORD(D3DRS_ALPHATESTENABLE),DWORD(D3DRS_STENCILENABLE)})D8<50>(device,state,DWORD(FALSE));
        D8<50>(device,D3DRS_CULLMODE,DWORD(D3DCULL_NONE));D8<50>(device,D3DRS_ZENABLE,DWORD(TRUE));D8<50>(device,D3DRS_ZWRITEENABLE,DWORD(TRUE));
        D8<63>(device,DWORD(1),DWORD(D3DTSS_COLOROP),DWORD(D3DTOP_DISABLE));
        D8<63>(device,DWORD(0),DWORD(D3DTSS_COLOROP),DWORD(D3DTOP_SELECTARG1));D8<63>(device,DWORD(0),DWORD(D3DTSS_COLORARG1),DWORD(D3DTA_DIFFUSE));
        D8<63>(device,DWORD(0),DWORD(D3DTSS_TEXTURETRANSFORMFLAGS),DWORD(D3DTTFF_DISABLE));
        D8<61>(device,DWORD(0),static_cast<void*>(nullptr));
        for(unsigned pass=0;pass<3;++pass){
            Check(SUCCEEDED(D8<31>(device,surfaces[pass%2],smallDepth)),"bind native offscreen prepass");D3DVIEWPORT9 vp{0,0,32,32,0,1};D8<40>(device,&vp);
            DWORD flags=D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER;if(missingColor&&pass==0)flags&=~D3DCLEAR_TARGET;if(missingDepth&&pass==0)flags&=~D3DCLEAR_ZBUFFER;
            Check(SUCCEEDED(D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),flags,DWORD(0xff101010),1.f,DWORD(0))),"clear native prepass in order");
            D8<34>(device);quad(0,32,pass==0?0xffff0000:pass==1?0xff00ff00:0xff0000ff);D8<35>(device);
        }
        Check(!deferArmed&&!deferredPending,"offscreen prepasses retain native execution");
        Check(SUCCEEDED(D8<31>(device,back,depth)),"bind main backbuffer after prepasses");D3DVIEWPORT9 vp{0,0,128,128,0,1};D8<40>(device,&vp);
        D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER),DWORD(0xff101010),1.f,DWORD(0));
        Check(deferArmed==(deferred&&!missingColor&&!missingDepth),"defer only a main scene with replay-safe color and depth prefix");
        D8<63>(device,DWORD(0),DWORD(D3DTSS_COLORARG1),DWORD(D3DTA_TEXTURE));D8<34>(device);
        for(unsigned i=0;i<2;++i){D8<61>(device,DWORD(0),textures[i]);quad(float(i)*64,64,0xffffffff);}
        D8<35>(device);D8<61>(device,DWORD(0),static_cast<void*>(nullptr));capturing=false;
    };
    frame(false);auto native=Pixels();countedUp=oldUp;oldUp=&CountUp;nativeDrawCount=0;
    frame(true);Check(deferredPending&&nativeDrawCount==3,"main scene avoids duplicate submission after three native prepass draws");
    Check(Pixels()==native,"deferred prepasses and sampled shadow textures match independent native output exactly");
    frame(true);D8<34>(device);capturing=true;Invalidate(Unsupported,timing::StateRejected);D8<35>(device);
    Check(!deferredPending&&Pixels()==native,"rejected main-scene capture reconstructs prefix with shared depth and render textures");
    frame(true,true);Check(!deferredPending,"prefix depending on uncleared color cannot enable deferred rendering");
    frame(true,false,true);Check(!deferredPending,"prefix depending on uncleared depth cannot enable deferred rendering");
    oldUp=countedUp;ClearHistory();D8<31>(device,back,depth);Release(back);Release(depth);Release(smallDepth);for(auto surface:surfaces)Release(surface);for(auto texture:textures)Release(texture);
}
using CreateBlockFn=HRESULT(__stdcall*)(void*,D3DSTATEBLOCKTYPE,DWORD*);
CreateBlockFn originalCreateBlock=nullptr;unsigned createdBlocks=0;
HRESULT __stdcall CountCreateBlock(void* d,D3DSTATEBLOCKTYPE type,DWORD* token){++createdBlocks;return originalCreateBlock(d,type,token);}
void PipelineTests(){
    pipeline::State baseline;Check(pipeline::Capture(baseline),"capture initial fixed-function state cache");
    D3DMATERIAL9 material{};material.Diffuse={.2f,.4f,.6f,1};material.Ambient={.1f,.2f,.3f,1};material.Power=4;
    D3DLIGHT9 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Diffuse={1,.5f,.2f,1};light.Direction={0,0,-1};
    Matrix tex=Identity();tex.m[2][0]=.25f;float clip[4]={1,0,0,.5f};
    D8<42>(device,&material);D8<44>(device,DWORD(37),&light);D8<46>(device,DWORD(37),DWORD(TRUE));
    D8<48>(device,DWORD(0),clip);D8<50>(device,D3DRS_FOGCOLOR,DWORD(0xff225588));
    D8<63>(device,DWORD(0),DWORD(13) /* D3D8 ADDRESSU */,DWORD(D3DTADDRESS_CLAMP));
    D8<37>(device,D3DTS_TEXTURE0,&tex);
    pipeline::State chosen;Check(pipeline::Capture(chosen),"capture arbitrary light IDs, material, texture transform and sampler state");
    Check(pipeline::ApplyDraw(baseline)&&pipeline::ApplyDraw(chosen),"apply only changed fixed-function state");
    D3DMATERIAL9 got{};D3DLIGHT9 gotLight{};BOOL enabled=FALSE;DWORD fog=0,address=0;Matrix matrix{};float plane[4]{};
    D8<43>(device,&got);D8<45>(device,DWORD(37),&gotLight);D8<47>(device,DWORD(37),&enabled);D8<51>(device,D3DRS_FOGCOLOR,&fog);
    D8<62>(device,DWORD(0),DWORD(13) /* D3D8 ADDRESSU */,&address);D8<38>(device,D3DTS_TEXTURE0,&matrix);D8<49>(device,DWORD(0),plane);
    Check(!memcmp(&got,&material,sizeof(got))&&!memcmp(&gotLight,&light,sizeof(light))&&enabled,"material and arbitrary light state restore exactly");
    Check(fog==0xff225588&&address==D3DTADDRESS_CLAMP&&!memcmp(&matrix,&tex,sizeof(tex))&&!memcmp(clip,plane,sizeof(plane)),"render, texture, transform and clip states restore exactly");
    DWORD block=0;Check(SUCCEEDED(D8<52>(device)),"begin native state-block recording");
    D8<50>(device,D3DRS_FOGCOLOR,DWORD(0xffabcdef));D8<53>(device,&block);
    D8<51>(device,D3DRS_FOGCOLOR,&fog);Check(fog==0xff225588&&pipeline::Sync()&&pipeline::live.rs[D3DRS_FOGCOLOR]==fog,"recording state setters does not mutate live cache");
    D8<54>(device,block);Check(pipeline::Sync()&&pipeline::live.rs[D3DRS_FOGCOLOR]==0xffabcdef,"external state-block application invalidates and resynchronizes tracked values");D8<56>(device,block);
    DWORD all=0;Check(SUCCEEDED(D8<57>(device,D3DSBT_ALL,&all)),"create full native state block");
    Check((*static_cast<void***>(device))[61]==reinterpret_cast<void*>(&pipeline::SetTexture)&&(*static_cast<void***>(device))[50]==reinterpret_cast<void*>(&pipeline::RenderState),"full state-block creation preserves texture and render-state hook chains");
    D8<55>(device,all);D8<54>(device,all);D8<56>(device,all);
    Check((*static_cast<void***>(device))[61]==reinterpret_cast<void*>(&pipeline::SetTexture),"capture/apply of full blocks preserves setter hooks");
    Check(pipeline::ApplyDraw(baseline),"restore baseline after pipeline regression tests");
    Check(Slot(*static_cast<void***>(device),57,&CountCreateBlock,originalCreateBlock),"observe driver state-block allocations in fixture");
}
uint64_t TexturedMenuTests(bool deferred=false){
    struct V {float x,y,z,w;DWORD color;float u,v;};
    void* textures[2]{};
    for(int i=0;i<2;i++){
        Check(SUCCEEDED(D8<20>(device,UINT(2),UINT(2),UINT(1),DWORD(0),D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&textures[i])),"create distinct menu textures");
        D3DLOCKED_RECT lock{};D8<16>(textures[i],UINT(0),&lock,static_cast<const RECT*>(nullptr),DWORD(0));
        for(int y=0;y<2;y++)for(int x=0;x<2;x++)*reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch+x*4)=i?0xff00ffff:0xffff6600;
        D8<17>(textures[i],UINT(0));
    }
    ClearHistory();deferCandidate=deferred;menuFrame=true;capturing=true;D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER),DWORD(0xff202020),1.f,DWORD(0));D8<34>(device);
    D8<76>(device,DWORD(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1));
    D8<50>(device,D3DRS_LIGHTING,DWORD(FALSE));D8<50>(device,D3DRS_ZENABLE,DWORD(FALSE));D8<50>(device,D3DRS_CULLMODE,DWORD(D3DCULL_NONE));D8<50>(device,D3DRS_ALPHABLENDENABLE,DWORD(FALSE));
    D8<63>(device,DWORD(0),DWORD(D3DTSS_COLOROP),DWORD(D3DTOP_SELECTARG1));D8<63>(device,DWORD(0),DWORD(D3DTSS_COLORARG1),DWORD(D3DTA_TEXTURE));
    D8<63>(device,DWORD(0),DWORD(D3DTSS_TEXTURETRANSFORMFLAGS),DWORD(D3DTTFF_DISABLE));
    for(int i=0;i<2;i++){
        // Match UI's full state-block save/restore between differently textured
        // draws; this reproduces the driver vtable mutation missed by flat tests.
        DWORD token=0;D8<57>(device,D3DSBT_ALL,&token);D8<54>(device,token);D8<56>(device,token);
        if(i) pipeline::texture(device,0,textures[i]);else D8<61>(device,DWORD(0),textures[i]);
        float x=8.f+i*60;
        V v[]={{x,8,0,1,0xffffffff,0,0},{x+50,8,0,1,0xffffffff,1,0},{x,110,0,1,0xffffffff,0,1},{x+50,110,0,1,0xffffffff,1,1}};
        D8<72>(device,D3DPT_TRIANGLESTRIP,UINT(2),v,UINT(sizeof(V)));
    }
    D8<35>(device);capturing=false;if(deferred)Check(deferredPending,"textured/shaded fixture genuinely defers native GPU draws");auto native=Pixels();
    Check(current.valid&&current.commands.size()==3&&current.commands[1]->gpu.textures[0]==textures[0]&&current.commands[2]->gpu.textures[0]==textures[1],"each retained menu draw owns actual texture including saved-driver-pointer bypass");
    replaying=true;Check(Replay(current,nullptr,1)&&Pixels()==native,"replayed multi-texture menu is pixel identical to native frame");replaying=false;
    D8<61>(device,DWORD(0),static_cast<void*>(nullptr));ClearHistory();for(auto texture:textures)Release(texture);menuFrame=false;return native;
}
void ViewportReplayTests(){
    ClearHistory();capturing=true;hudPhase=true;D3DVIEWPORT9 full{0,0,128,128,0,1};D8<40>(device,&full);
    D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_TARGET),DWORD(0xff202020),1.f,DWORD(0));D8<34>(device);
    D8<76>(device,DWORD(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));D8<50>(device,D3DRS_ZENABLE,DWORD(FALSE));D8<50>(device,D3DRS_ALPHABLENDENABLE,DWORD(FALSE));
    D8<63>(device,DWORD(0),DWORD(D3DTSS_COLOROP),DWORD(D3DTOP_SELECTARG1));D8<63>(device,DWORD(0),DWORD(D3DTSS_COLORARG1),DWORD(D3DTA_DIFFUSE));
    struct V{float x,y,z,w;DWORD color;};for(UINT i=0;i<2;i++){
        D3DVIEWPORT9 part{i*64,0,64,128,0,1};D8<40>(device,&part);DWORD color=i?0xff00ff00:0xffff0000;
        V v[]={{0,0,0,1,color},{128,0,0,1,color},{0,128,0,1,color},{128,128,0,1,color}};D8<72>(device,D3DPT_TRIANGLESTRIP,UINT(2),v,UINT(sizeof(V)));
    }
    D8<35>(device);capturing=false;D8<40>(device,&full);auto native=Pixels();replaying=true;
    Check(Replay(current,nullptr,1)&&Pixels()==native,"viewport change within retained frame reconstructs native pixels");replaying=false;
    D3DVIEWPORT9 restored{};D8<41>(device,&restored);Check(!memcmp(&full,&restored,sizeof(full)),"viewport optimization restores the caller viewport");ClearHistory();
}
uint64_t PixelShaderTests(bool deferred=false){
    // ps_1_1: mov r0, c0. Constant changes must remain distinct in replay.
    const DWORD program[]={D3DPS_VERSION(1,1),D3DSIO_MOV,0x800f0000,0xa0e40000,D3DSIO_END};
    DWORD shader=0;Check(SUCCEEDED(D8<87>(device,program,&shader)),"create D3D8 pixel shader regression fixture");
    ClearHistory();deferCandidate=deferred;capturing=true;hudPhase=false;
    D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER),DWORD(0xff202020),1.f,DWORD(0));
    D8<34>(device);D8<76>(device,DWORD(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
    D8<50>(device,D3DRS_ZENABLE,DWORD(FALSE));D8<50>(device,D3DRS_ALPHABLENDENABLE,DWORD(FALSE));
    D8<88>(device,shader);
    struct V{float x,y,z,w;DWORD color;};
    for(int i=0;i<2;i++){
        float color[4]={i?.2f:1.f,i?1.f:.2f,.4f,1.f};
        if(i)pipeline::constants(device,0,color,1);else D8<91>(device,DWORD(0),color,DWORD(1));
        float x=8.f+i*60;V v[]={{x,8,0,1,0xffffffff},{x+50,8,0,1,0xffffffff},{x,110,0,1,0xffffffff},{x+50,110,0,1,0xffffffff}};
        D8<72>(device,D3DPT_TRIANGLESTRIP,UINT(2),v,UINT(sizeof(V)));
    }
    D8<88>(device,DWORD(0));D8<35>(device);capturing=false;if(deferred)Check(deferredPending,"textured/shaded fixture genuinely defers native GPU draws");auto native=Pixels();
    Check(current.valid&&current.commands.size()==3&&current.commands[1]->gpu.ps==shader,"pixel-shaded draw no longer invalidates the entire gameplay frame");
    Check(current.commands[1]->gpu.psConstants[0]!=current.commands[2]->gpu.psConstants[0],"retain per-draw pixel constants including driver-pointer bypass");
    replaying=true;Check(Replay(current,nullptr,1)&&Pixels()==native,"pixel-shader replay is pixel identical with distinct constants");replaying=false;
    DWORD active=1;D8<89>(device,&active);Check(active==0,"restore native fixed-function shading after shader replay");
    previous=std::move(current);current=Frame{};D8<90>(device,shader);
    Check(!previous.valid,"deleting a retained pixel shader retires unsafe history");ClearHistory();return native;
}
void LoggingTests(){
    static timing::Queue<4> tinyQueue;timing::Record r,out;
    for(uint64_t i=1;i<=4;i++){r.sequence=i;Check(tinyQueue.Push(r),"logger accepts available queue space");}
    Check(!tinyQueue.Push(r)&&tinyQueue.dropped==1,"full logger queue drops a record without blocking");
    for(uint64_t i=1;i<=4;i++)Check(tinyQueue.Pop(out)&&out.sequence==i,"logger preserves FIFO order");
    Check(!tinyQueue.Pop(out),"empty logger queue returns immediately");
    static timing::Queue<1024> stress;std::atomic<bool> done{false};bool ordered=true;uint64_t received=0;
    std::thread consumer([&](){timing::Record item;uint64_t last=0;while(!done.load()||stress.read.load()!=stress.write.load()){if(stress.Pop(item)){ordered=ordered&&item.sequence>last&&item.context==item.sequence*3;last=item.sequence;++received;}else std::this_thread::yield();}});
    for(uint64_t i=1;i<=100000;i++){r.sequence=i;r.context=i*3;stress.Push(r);}
    done=true;consumer.join();Check(ordered&&received+stress.dropped==100000,"concurrent producer/consumer preserves data and accounts for dropped records");
    double begin=Now();
    for(int i=0;i<100000;i++){tinyQueue.Push(r);tinyQueue.Pop(out);}
    std::cout<<"Logger queue round-trip: "<<(Now()-begin)*1e6/100000<<" us/record (synthetic CPU benchmark).\n";
    timing::enabled=false;auto before=timing::queue.write.load();timing::FinishTick(60,0);
    Check(timing::queue.write==before,"disabled logger does not enqueue frames");
    std::ofstream("build/logger-not-directory")<<"fixture";
    Check(!timing::Start("build/logger-not-directory")&&timing::ioError!=0,"logging directory failure is reported without blocking rendering");
    timing::ioError=0;Check(timing::Start("build/logging-test"),"background log writer starts");
    timing::enabled=true;timing::BeginDraw(false,12345,5);
    {timing::Scope scope(timing::Capture);Sleep(1);}
    timing::frame.generated=2;timing::frame.submissions=3;timing::frame.reasons=timing::EarlySlotMiss;
    timing::FinishTick(180,Active);timing::enabled=false;timing::Stop();
    std::ifstream log(timing::session+L"-0.csv");std::string header,line;std::getline(log,header);std::getline(log,line);
    Check(header.find("capture_ms")!=std::string::npos&&header.find("max_present_gap_ms")!=std::string::npos,"CSV contains phase and hitch columns");
    Check(!line.empty()&&std::count(line.begin(),line.end(),',')==std::count(header.begin(),header.end(),','),"background writer flushes a complete structured record");
    Check(line.find(",12345,0,5,180,2,")!=std::string::npos&&timing::ioError==0,"written CSV preserves gameplay context, FPS target and status");
    timing::previousSync=0;timing::frame={};
}
bool ScalarPositions(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,unsigned stride,float t,bool normals,bool protect,std::vector<uint8_t>& out){
    if(stride<12||a.size()!=b.size()||b.size()%stride)return false;out=b;
    for(size_t i=0;i<b.size();i+=stride){V3 av{},bv{};memcpy(&av,a.data()+i,12);memcpy(&bv,b.data()+i,12);V3 d=bv-av;
        if(!Finite(av)||!Finite(bv)||(protect&&Dot(d,d)>250*250))return false;V3 p=av+d*t;memcpy(out.data()+i,&p,12);
        if(normals&&stride>=24){memcpy(&av,a.data()+i+12,12);memcpy(&bv,b.data()+i+12,12);if(!Finite(av)||!Finite(bv))return false;p=Unit(av+(bv-av)*t);memcpy(out.data()+i+12,&p,12);}
    }return true;
}
__declspec(noinline) bool LegacyRebase(std::vector<uint8_t>& data,size_t width,UINT min,UINT vertices){
    for(size_t i=0;i<data.size();i+=width){uint32_t x=0;memcpy(&x,data.data()+i,width);if(x<min||x-min>=vertices)return false;x-=min;memcpy(data.data()+i,&x,width);}return true;
}
template<class Index> void IndexTests(UINT base,UINT count){
    std::vector<uint8_t> raw(120000*sizeof(Index));for(size_t i=0;i<120000;i++){Index value=Index(base+(i*7919)%count);memcpy(raw.data()+i*sizeof(Index),&value,sizeof(value));}
    auto scalar=raw,fast=raw;Check(LegacyRebase(scalar,sizeof(Index),base,count)&&RebaseIndices<Index>(fast,base,count)&&scalar==fast,"specialized index normalization matches legacy bytes exactly");
    Check(IndicesInRange<Index>(fast,count),"normalized indices remain inside vertex range");
    fast=raw;Check(!RebaseIndices<Index>(fast,base+1,count),"indices below min are rejected");
    fast=raw;Check(!RebaseIndices<Index>(fast,base,1),"indices beyond vertex range are rejected");
    fast=scalar;Index end=Index(count);memcpy(fast.data()+fast.size()-sizeof(Index),&end,sizeof(end));Check(!IndicesInRange<Index>(fast,count),"final index bounds check is preserved");
    fast.push_back(0);Check(!RebaseIndices<Index>(fast,base,count)&&!IndicesInRange<Index>(fast,count),"partial index elements cannot read beyond the buffer");
    bool ok=true;double start=Now();for(int i=0;i<40;i++){fast=raw;ok=LegacyRebase(fast,sizeof(Index),base,count)&&ok;}double legacy=Now()-start;
    start=Now();for(int i=0;i<40;i++){fast=raw;ok=RebaseIndices<Index>(fast,base,count)&&ok;}double specialized=Now()-start;Check(ok,"index benchmark preserves bounds checks");
    std::cout<<sizeof(Index)*8<<"-bit 120000-index normalization: legacy "<<legacy*25<<" ms, specialized "<<specialized*25<<" ms.\n";
}
void StreamingCopyTests(){
    if(!StreamingReadSupported())return;
    auto allocation=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(allocation!=nullptr,"allocate streaming-copy guard pages");DWORD old=0;VirtualProtect(allocation+4096,4096,PAGE_NOACCESS,&old);
    for(size_t i=0;i<4096;i++)allocation[i]=uint8_t(i*19);bool same=true;
    for(size_t n=0;n<=129;n++){std::vector<uint8_t> result(n+2,0xab);auto source=allocation+4096-n;ReadLockedBytes(result.data()+1,source,n,true);same=same&&result.front()==0xab&&result.back()==0xab&&memcmp(result.data()+1,source,n)==0;}
    Check(same,"streaming copy preserves unaligned bytes and tails without crossing a guard page");VirtualFree(allocation,0,MEM_RELEASE);
}
void VertexMathTests(){
    uint32_t seed=17;auto sample=[&](){seed=seed*1664525u+1013904223u;return float(int(seed>>16)-32768)/4096;};
    std::vector<uint8_t> scalar,simd;
    for(unsigned stride:{12u,16u,24u,28u,36u,48u}){
        std::vector<uint8_t> a(stride*257),b(a.size());for(size_t i=0;i<a.size();i+=4){float av=sample(),bv=sample();memcpy(a.data()+i,&av,4);memcpy(b.data()+i,&bv,4);}
        for(float alpha:{0.f,1.f/3,2.f/3,1.f}){
            bool x=ScalarPositions(a,b,stride,alpha,stride>=24,true,scalar),y=Positions(a,b,stride,alpha,stride>=24,true,simd);
            Check(x&&y&&scalar==simd,"SSE2 vertex/normal interpolation is byte-identical to scalar reference");
        }
    }
    std::vector<uint8_t> a(24),b(24);float extreme=251;memcpy(b.data(),&extreme,4);
    Check(!Positions(a,b,24,.5f,true,true,simd)&&Positions(a,b,24,.5f,true,false,simd),"SIMD preserves camera-cut distance guard and opt-out");
    float nan=std::numeric_limits<float>::quiet_NaN();memcpy(b.data()+20,&nan,4);
    Check(!Positions(a,b,24,.5f,true,false,simd),"SIMD rejects non-finite normal in final tightly packed vertex");
    memcpy(b.data(),&nan,4);Check(!Positions(a,b,24,.5f,false,false,simd),"SIMD rejects non-finite position");
    b.assign(24,0);Check(Positions(a,b,24,.5f,true,true,simd)&&simd==b,"SIMD keeps zero normals finite");
}
void LayoutTests(){
    ClearHistory();auto prior=std::make_unique<Command>();prior->fvf=D3DFVF_XYZ|D3DFVF_TEX1;prior->stride=20;prior->vertices=12000;prior->count=4000;prior->data.resize(prior->vertices*20);prior->indices.resize(prior->vertices*2);
    for(UINT i=0;i<prior->vertices;i++){float v[5]={float(i),0,2,float(i%17)/17.f,float(i%31)/31.f};memcpy(prior->data.data()+i*20,v,20);uint16_t n=uint16_t(i);memcpy(prior->indices.data()+i*2,&n,2);}
    Check(Finalize(*prior,123,0),"establish large indexed topology identity");auto key=prior->key,layout=prior->layout;
    previous.commands.push_back(std::move(prior));Command next=*previous.commands[0];float moved=99;memcpy(next.data.data(),&moved,4);
    double begin=Now();for(int i=0;i<100;i++){current.bytes=0;Finalize(next,123,0);}double baseline=Now()-begin;
    PrepareLayoutHistory();begin=Now();for(int i=0;i<100;i++){current.bytes=0;Finalize(next,123,0);}double cached=Now()-begin;
    Check(next.key==key&&next.layout==layout,"exact topology reuse preserves identity despite animated positions");
    std::cout<<"Topology capture, 12000 vertices: full hash "<<baseline*10<<" ms; exact cached comparison "<<cached*10<<" ms (synthetic CPU benchmark).\n";
    memcpy(next.data.data()+next.data.size()-4,&moved,4);current.bytes=0;Finalize(next,123,0);Check(next.layout!=layout,"changed final UV invalidates cached topology");
    next=*previous.commands[0];next.indices[0]=1;current.bytes=0;Finalize(next,123,0);Check(next.key!=key,"changed index invalidates cached topology");
    previous.commands.push_back(std::make_unique<Command>(*previous.commands[0]));PrepareLayoutHistory();Check(!PriorLayout(previous.commands[0]->layoutKey),"ambiguous topology keys never reuse an arbitrary draw");
    ClearHistory();Check(layoutHistory.empty(),"history reset removes cached command pointers");
}
// Cloned instances (each teammate's jump ball) share one key.
void DuplicatePairingTests(){
    auto frame=[](std::initializer_list<float> xs,uint64_t key=0x5a11){Frame f;for(float x:xs){auto c=std::make_unique<Command>();c->kind=Command::Draw;c->scene3d=true;c->key=key;c->layout=7;c->stride=sizeof(Vertex);c->fvf=Fvf;c->vertices=1;
        Vertex v{0,1,0,0,0,-1,0xffffffff};c->data.resize(sizeof(v));memcpy(c->data.data(),&v,sizeof(v));c->world=Identity();c->world.m[3][0]=x;c->view=Identity();f.commands.push_back(std::move(c));}return f;};
    auto pairs=[](const Frame& before,const Frame& now,std::initializer_list<int> expected){Matches m=Match(before);auto keys=Match(now);for(auto& p:keys)if(!p.second)m.erase(p.first);auto plan=PlanReplay(now,m,&before);size_t i=0;
        for(int e:expected){const Command* want=e<0?nullptr:before.commands[e].get();if(plan.draws[i++].prior!=want)return false;}return true;};
    // Spacing 15, motion 10: nearest-position pairing would swap teammates.
    auto a=frame({0,15,30}),b=frame({10,25,40});Check(pairs(a,b,{0,1,2}),"duplicate instances in formation keep their draw-order pairing");
    Matches m=Match(a);Check(!PlanReplay(b,m).draws[0].prior,"duplicate pairing needs the previous frame");
    a=frame({0,100,200});b=frame({205,5,105});Check(pairs(a,b,{2,0,1}),"reordered duplicate draws pair by position");
    b=frame({5,205});Check(pairs(a,b,{0,2}),"a despawned duplicate leaves the others paired");
    b=frame({5,105,205,150});Check(pairs(a,b,{0,1,2,-1}),"a newly spawned duplicate stays unmatched");
    a=frame({0,1000});b=frame({600,1600});Check(pairs(a,b,{-1,-1}),"duplicates that moved too far are not paired");
    a=frame({0,15});b=frame({2,25});b.commands[1]->layout=8;Check(pairs(a,b,{0,-1}),"incompatible duplicate geometry is never paired");
}
// Two moving copies of one quad must interpolate like the native midpoint.
void DuplicateMotionTests(){
    ClearHistory();
    auto frame=[&](float amount){current=Frame{};current.epoch=epoch;capturing=true;hudPhase=false;
        Matrix view=Identity(),proj{};for(int i=0;i<4;i++)view.m[i][0]=-view.m[i][0];
        proj.m[0][0]=1;proj.m[1][1]=1;proj.m[2][2]=10.f/9.9f;proj.m[2][3]=1;proj.m[3][2]=-1.f/9.9f;
        D8<37>(device,D3DTS_VIEW,&view);D8<37>(device,D3DTS_PROJECTION,&proj);
        D8<50>(device,D3DRS_LIGHTING,DWORD(FALSE));D8<50>(device,D3DRS_CULLMODE,DWORD(D3DCULL_NONE));D8<50>(device,D3DRS_ZENABLE,DWORD(D3DZB_TRUE));D8<50>(device,D3DRS_ALPHABLENDENABLE,DWORD(FALSE));
        D8<61>(device,DWORD(0),static_cast<void*>(nullptr));D8<63>(device,DWORD(0),DWORD(D3DTSS_COLOROP),DWORD(D3DTOP_SELECTARG1));D8<63>(device,DWORD(0),DWORD(D3DTSS_COLORARG1),DWORD(D3DTA_DIFFUSE));D8<76>(device,Fvf);
        D8<36>(device,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER),DWORD(0xff101010),1.f,DWORD(0));D8<34>(device);
        Vertex quad[4]={{-.15f,-.15f,3,0,0,-1,0xff40c0ff},{.15f,-.15f,3,0,0,-1,0xff40c0ff},{-.15f,.15f,3,0,0,-1,0xff40c0ff},{.15f,.15f,3,0,0,-1,0xff40c0ff}};
        // Copies 0.25 apart move 0.5 per tick: nearest pairing would cross them.
        for(int i=0;i<2;i++){Matrix world=Identity();world.m[3][0]=-.6f+.25f*i+amount*.5f;world.m[3][1]=.2f*i;D8<37>(device,D3DTS_WORLD,&world);D8<72>(device,D3DPT_TRIANGLESTRIP,UINT(2),static_cast<const void*>(quad),UINT(sizeof(Vertex)));}
        D8<35>(device);capturing=false;};
    frame(.5f);uint64_t midpoint=Pixels();ClearHistory();
    frame(0);uint64_t first=Pixels();previous=std::move(current);frame(1);uint64_t last=Pixels();
    Check(current.valid&&current.commands.size()==3&&current.commands[1]->key==current.commands[2]->key,"cloned quads share one geometry key");
    Matches matches=Match(previous);auto keys=Match(current);for(auto& p:keys)if(!p.second)matches.erase(p.first);auto plan=PlanReplay(current,matches,&previous);
    Check(plan.draws[1].prior==previous.commands[1].get()&&plan.draws[2].prior==previous.commands[2].get()&&plan.usable==2,"moving clones pair with their own previous instance");
    replaying=true;Check(Replay(current,&matches,.5f,&plan),"replay paired clones");uint64_t replayed=Pixels();
    Check(replayed!=first&&replayed!=last&&replayed==midpoint,"paired clones render exactly where the native midpoint draws them");
    Check(Replay(current,nullptr,1)&&Pixels()==last,"paired clones restore the native current frame");replaying=false;ClearHistory();
}
void HeavyReplayBench(){
    // Remove the allocation-counting wrapper: it deliberately forces driver
    // resynchronization and is not representative of the installed hook chain.
    void* create=reinterpret_cast<void*>(&pipeline::CreateBlock);Write(*static_cast<void***>(device)+57,&create,4);
    auto clone=[](const Command& c){auto p=std::make_unique<Command>(c);Ref(p->target);Ref(p->depth);Ref(p->source);for(auto t:p->gpu.textures)Ref(t);return p;};
    auto expand=[&](const Frame& source){Frame f;f.commands.push_back(clone(*source.commands[0]));for(int i=0;i<512;i++){
        auto c=clone(*source.commands[2]);auto data=c->data;c->data.clear();for(int j=0;j<16;j++)c->data.insert(c->data.end(),data.begin(),data.end());
        c->vertices*=16;c->count*=16;c->key=uint64_t(i+1);f.commands.push_back(std::move(c));}return f;};
    auto a=expand(previous),b=expand(current);auto matches=Match(a);replaying=true;bool ok=Replay(b,&matches,.5f);std::vector<double> times;
    for(int i=0;i<30;i++){double start=Now();ok=Replay(b,&matches,float(i%3+1)/3)&&ok;times.push_back((Now()-start)*1000);}
    replaying=false;Check(ok,"heavy 512-draw 24576-vertex interpolation fixture renders successfully");std::sort(times.begin(),times.end());
    std::cout<<"Heavy replay CPU time: p50 "<<times[15]<<" ms; p95 "<<times[28]<<" ms (synthetic, 8x MSAA).\n";
    auto geometry=settings.geometry;settings.geometry=0;replaying=true;times.clear();
    for(int i=0;i<30;i++){double start=Now();ok=Replay(b,&matches,.5f)&&ok;times.push_back((Now()-start)*1000);}
    replaying=false;settings.geometry=geometry;std::sort(times.begin(),times.end());
    std::cout<<"Heavy replay without vertex blend: p50 "<<times[15]<<" ms; p95 "<<times[28]<<" ms (diagnostic comparison only).\n";
}
void TimerTests(){
    Timer(true);Check(pacingTimer!=nullptr,"high-resolution deadline timer is available on this test system");
    bool early=false;std::vector<double> lateness;
    for(int i=0;i<100;i++){double target=Now()+1.0/180;Until(target);double late=Now()-target;early=early||late<0;lateness.push_back(late*1000);}
    std::sort(lateness.begin(),lateness.end());Check(!early,"hybrid wait never returns before its presentation deadline");
    std::cout<<"180 Hz synthetic wait lateness: p95 "<<lateness[95]<<" ms; maximum "<<lateness.back()<<" ms.\n";
    // Retail CRI state: game thread pinned to CPU 0 with priority boost disabled.
    HANDLE self=GetCurrentThread(),real=nullptr;DWORD_PTR mask=SetThreadAffinityMask(self,1);SetThreadPriorityBoost(self,TRUE);
    DuplicateHandle(GetCurrentProcess(),self,GetCurrentProcess(),&real,THREAD_QUERY_INFORMATION,FALSE,0);
    struct Observed {HANDLE thread;int priority;} observed{real,THREAD_PRIORITY_ERROR_RETURN};
    HANDLE observer=CreateThread(nullptr,0,[](void* p)->DWORD{auto& o=*static_cast<Observed*>(p);Sleep(4);o.priority=GetThreadPriority(o.thread);return 0;},&observed,0,nullptr);
    Until(Now()+.012);WaitForSingleObject(observer,INFINITE);CloseHandle(observer);
    Check(observed.priority==THREAD_PRIORITY_TIME_CRITICAL&&GetThreadPriority(self)==THREAD_PRIORITY_NORMAL,"pinned pacing wait is raised while blocked and restores normal priority");
    SetThreadPriority(self,THREAD_PRIORITY_BELOW_NORMAL);Until(Now()+.002);Until(Now()-.001);
    Check(GetThreadPriority(self)==THREAD_PRIORITY_BELOW_NORMAL,"pacing wait restores a non-default caller priority and skips elapsed deadlines");
    SetThreadPriority(self,THREAD_PRIORITY_NORMAL);SetThreadPriorityBoost(self,FALSE);SetThreadAffinityMask(self,mask);CloseHandle(real);
    Timer(false);Check(!pacingTimer&&!timerActive,"disabling interpolation releases deadline timer and timer-resolution request");
}
LONG WINAPI TestException(EXCEPTION_POINTERS* e){std::cerr<<"EXCEPTION "<<std::hex<<e->ExceptionRecord->ExceptionCode<<" at "<<e->ContextRecord->Eip<<" access "<<e->ExceptionRecord->ExceptionInformation[1]<<"\n";for(int i=0;i<24;i++)std::cerr<<reinterpret_cast<DWORD*>(e->ContextRecord->Esp)[i]<<" ";std::cerr<<"\n";return EXCEPTION_EXECUTE_HANDLER;}
void GameCodeTests(){
    std::vector<unsigned char> tiny(16,0);Check(!game_code::SupportedFile(tiny),"non-executable data is not a supported game");
    std::ifstream in(R"(C:\Program Files (x86)\SEGA\SONICHEROES\Tsonic_win.exe)",std::ios::binary);
    std::vector<unsigned char> exe((std::istreambuf_iterator<char>(in)),{});
    if(exe.empty()){std::cout<<"SKIP: installed Tsonic_win.exe not found for executable checks\n";return;}
    Check(game_code::SupportedFile(exe),"supported executable's game code is accepted");
    uint32_t pe=0;memcpy(&pe,&exe[0x3c],4);
    auto header=exe;header[pe+22]^=0x20;Check(game_code::SupportedFile(header),"header-only change (4GB patch flag) is still accepted");
    auto data=exe;data[0x4949b0]^=0xff;Check(game_code::SupportedFile(data),"data-only change (recoloured menu) is still accepted");
    auto code=exe;code[0x43169]^=0xff;Check(!game_code::SupportedFile(code),"changed game code is rejected");
    exe.resize(0x200000);Check(!game_code::SupportedFile(exe),"truncated executable is rejected");
}
int main(){SetUnhandledExceptionFilter(&TestException);
    Check(QueueFits(240,4)&&QueueFits(239,4)&&QueueFits(120,2)&&QueueFits(119,2),"V-Sync queue mode is used when the refresh rate equals the FPS setting");
    Check(!QueueFits(60,2)&&!QueueFits(144,4)&&!QueueFits(144,2)&&!QueueFits(240,2)&&!QueueFits(0,2),"V-Sync queue mode is refused when the display would slow or speed up the game");
    GameCodeTests();LoggingTests();IndexTests<uint16_t>(1000,64000);IndexTests<uint32_t>(100000,500000);StreamingCopyTests();VertexMathTests();LayoutTests();TimerTests();
    FrameRates meter;for(int i=0;i<=180;i++)meter.Presented(double(i)/180,i%3==0);
    Check(std::abs(meter.output.At(1)-180)<2.1f&&std::abs(meter.base.At(1)-60)<2.1f,"FPS meter distinguishes 180 submissions from 60 native frames");
    meter.Presented(2,true);Check(meter.output.At(2)<5&&meter.Peak(2)>=999,"one-second stall lowers measured FPS and retains the frame gap");for(int i=1;i<=90;i++)meter.Presented(2+double(i)/180,i%3==0);Check(meter.Peak(2.5)>=999,"recent hitch remains visible after smooth rendering resumes");
    Check(meter.output.At(4)==0,"idle meter does not show a stale target frame rate");
    Check(settings.enabled==0&&settings.targetHz==60&&settings.logging==1,"FPS defaults to native 60 with requested performance logging");
    FrameRates fast;for(int i=0;i<=5000;i++)fast.Presented(double(i)/5000,i%83==0);
    Check(std::abs(fast.output.At(1)-5000)<15,"FPS meter measures 5000 submissions per second without capacity clipping");
    for(auto rate:FpsOptions){Settings option;option.targetHz=rate;Check(Set(&option)&&settings.enabled==(rate!=60?1u:0u)&&settings.targetHz==rate,"API accepts each FPS target including unlocked");}
    Settings defaults;Set(&defaults);timing::enabled=false;
    Matrix a=Identity(),b=Identity(),out{};a.m[3][0]=-8;b.m[3][0]=4;Check(Transform(a,b,.5f,out)&&Near(out.m[3][0],-2),"world position interpolates");
    auto rotation=[](float degree){float angle=degree*3.14159265359f/180;return FromQuat({0,std::sin(angle*.5f),0,std::cos(angle*.5f)});};
    a=rotation(170);b=rotation(-170);Check(Transform(a,b,.5f,out)&&out.m[0][0]<-.999f&&Near(Dot(Row(out,0),Row(out,0)),1),"rotation takes shortest arc without shrinking skeleton roots");
    a=Identity();b=Identity();b.m[3][0]=10;Check(Camera(InverseRigid(a),InverseRigid(b),.5f,out,true)&&Near(out.m[3][0],-5),"camera blends eye position and reconstructs view matrix");
    b.m[3][0]=1000;Check(!Camera(a,b,.5f,out,true),"camera teleport rejects history");b=rotation(100);Check(!Camera(a,b,.5f,out,true),"camera cut rotation rejects history");
    a=Identity();b=Identity();a.m[0][0]=b.m[0][0]=-1;b.m[3][0]=10;
    Check(Camera(a,b,.5f,out,true)&&Near(out.m[3][0],5)&&Near(out.m[0][0],-1),"reflected camera interpolates without discarding the RenderWare basis");
    Check(Transform(a,b,.5f,out)&&Near(out.m[0][0],-1)&&Near(out.m[3][0],5),"mirrored character roots preserve handedness while moving");
    b=Identity();Check(!Camera(a,b,.5f,out,true)&&!Transform(a,b,.5f,out),"handedness changes invalidate history instead of collapsing geometry");
    a={{{.9999884963f,-.0024383736f,-.0041272687f,0},{-.0000000445f,.8609644771f,-.5086650848f,0},{-.0047937473f,-.5086592436f,-.8609545827f,0},{-42.04695892f,-2392.529541f,-3887.832031f,1}}};
    b={{{.9999884963f,-.0024268890f,-.0041340324f,0},{-.0000000443f,.8623754382f,-.5062693357f,0},{-.0047937473f,-.5062634945f,-.8623655438f,0},{-42.08987427f,-2386.033203f,-3902.215576f,1}}};
    Check(Camera(a,b,.5f,out,true),"exact consecutive live-game camera matrices are accepted");
    V3 expectedEye=(Row(InverseRigid(a),3)+Row(InverseRigid(b),3))*.5f;V3 error=Row(InverseRigid(out),3)-expectedEye;
    Check(Dot(error,error)<.00001f&&Dot(Cross(Row(out,0),Row(out,1)),Row(out,2))<-.999f,"live camera midpoint preserves reflection and the interpolated eye position");
    Check(Camera(a,b,0,out,true)&&std::abs(out.m[3][2]-a.m[3][2])<.002f&&Camera(a,b,1,out,true)&&std::abs(out.m[3][2]-b.m[3][2])<.002f,"live camera interpolation reproduces both endpoints");
    a=Identity();
    b=Identity();b.m[1][0]=.4f;Check(!Transform(a,b,.5f,out),"sheared transforms are not incorrectly quaternion blended");
    a={};a.m[0][0]=a.m[1][1]=a.m[2][2]=a.m[2][3]=1;a.m[3][2]=-.1f;b=a;b.m[0][0]=b.m[1][1]=1.5f;Check(Projection(a,b,.5f,out,true)&&Near(out.m[0][0],1.25f),"camera zoom/FOV interpolates with projection");b.m[0][0]=10;Check(!Projection(a,b,.5f,out,true),"large lens cut discards projection history");
    Shadow shadow;shadow.bytes.resize(16);uint32_t words[]={1,2,3,4};shadow.Write(0,words,8);shadow.Write(8,words+2,8);std::vector<uint8_t> read;
    Check(shadow.Read(0,16,read)&&!memcmp(read.data(),words,16),"partial CPU writes merge into initialized ranges");shadow.valid.clear();shadow.Write(4,words,4);Check(!shadow.Read(0,16,read)&&shadow.Read(4,4,read),"discard invalidates unwritten buffer contents");
    Settings bad;bad.enabled=2;Check(!Set(&bad)&&!settings.enabled,"API rejects invalid setting values");bad=Settings{};bad.targetHz=144;Check(!Set(&bad),"API rejects non-integral 60 Hz output rates");bad=Settings{};bad.menus=2;Check(!Set(&bad),"API validates menu toggle");installed=true;Check(HeroesMotionApi(Abi)==&api&&!HeroesMotionApi(1)&&!HeroesMotionApi(99),"expanded optional API rejects stale clients");
    wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);auto lib=LoadLibraryW((std::wstring(system)+L"\\d3d8.dll").c_str());Check(lib!=nullptr,"load actual system D3D8");
    auto create=reinterpret_cast<void*(__stdcall*)(UINT)>(GetProcAddress(lib,"Direct3DCreate8"));void* d3d=create(220);Check(d3d!=nullptr,"create Direct3D8 interface");
    HWND window=CreateWindowExW(0,L"STATIC",L"Interpolation tests",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Present8 pp{128,128,D3DFMT_X8R8G8B8,1,D3DMULTISAMPLE_NONE,D3DSWAPEFFECT_DISCARD,window,TRUE,TRUE,D3DFMT_D16,D3DPRESENTFLAG_LOCKABLE_BACKBUFFER,0,0};
    void* d=nullptr;Check(SUCCEEDED(D8<15>(d3d,UINT(0),D3DDEVTYPE_HAL,window,DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING),&pp,&d)),"create hidden hardware device");Check(InstallDevice(d),"install real capture hooks");PipelineTests();
    {D3DDISPLAYMODE mode{};D8<8>(d,&mode);void* image=nullptr;Check(SUCCEEDED(D8<27>(d,mode.Width,mode.Height,D3DFMT_A8R8G8B8,&image))&&image,"hooked device still creates image surfaces through slot 27");
        Check((*static_cast<void***>(d))[30]==reinterpret_cast<void*>(&FrontBuffer)&&SUCCEEDED(D8<30>(d,image)),"front-buffer readback barrier hooks GetFrontBuffer (slot 30)");Release(image);}
    Check(SUCCEEDED(D8<23>(d,UINT(sizeof(Vertex)*5),DWORD(D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY),Fvf,D3DPOOL_DEFAULT,&vb)),"create tracked vertex buffer");
    Check(SUCCEEDED(D8<24>(d,UINT(6),DWORD(D3DUSAGE_WRITEONLY),DWORD(D3DFMT_INDEX16),D3DPOOL_MANAGED,&ib)),"create tracked index buffer");
    BYTE* indices=nullptr;D8<11>(ib,UINT(0),UINT(6),&indices,DWORD(0));uint16_t indexValues[]={1,2,3};memcpy(indices,indexValues,6);D8<12>(ib);
    settings.enabled=1;settings.targetHz=120;Fixture(0);uint64_t first=Pixels();previous=std::move(current);Fixture(1);uint64_t last=Pixels();Check(first!=last,"native poses produce different actual images");Check(createdBlocks==0,"entire captured frames allocate no driver state blocks");
    auto matches=Match(previous);Check(matches.size()==3,"match skin, 3D effect and projected world effect; exclude HUD");
    Check(Replayable(current),"fixture fully initializes its render targets");
    current.commands[0]->flags&=~D3DCLEAR_TARGET;Check(!Replayable(current),"reject retained framebuffer feedback before modifying output");current.commands[0]->flags|=D3DCLEAR_TARGET;
    auto& posed=*current.commands[1];Check(posed.skinned&&posed.scene3d&&posed.vertices==3,"skinned pipeline identity retained");Check(posed.indices==std::vector<uint8_t>({0,0,1,0,2,0}),"normalize indexed draw range correctly");
    std::vector<uint8_t> blend;Check(Positions(previous.commands[1]->data,posed.data,sizeof(Vertex),.5f,true,true,blend),"interpolate post-skinning vertices");Vertex middle{};memcpy(&middle,blend.data()+2*sizeof(Vertex),sizeof(Vertex));Check(Near(middle.y,.45f)&&Near(middle.x,-.2f),"limb deformation and root movement both reach midpoint");
    auto& oldEffect=*previous.commands[3];auto& newEffect=*current.commands[3];Check(oldEffect.scene3d&&oldEffect.screenSpace&&!current.commands[4]->scene3d,"native HUD boundary separates projected effects from overlay");blend=newEffect.data;Check(Attributes(oldEffect,newEffect,.5f,blend)&&blend[19]==205,"effect opacity interpolates with its geometry");
    Matrix before{};D8<38>(d,D3DTS_VIEW,&before);DWORD shaderBefore=0;D8<77>(d,&shaderBefore);int originalUpdates=updates;
    replaying=true;Check(Replay(current,&matches,.5f),"render midpoint through real D3D8");uint64_t middlePixels=Pixels();Check(middlePixels!=first&&middlePixels!=last,"midpoint is a distinct rendered image, not duplicate presentation");
    settings.geometry=0;Check(Replay(current,&matches,.5f)&&Pixels()!=last,"camera-only interpolation visibly changes reflected-view fixture pixels");settings.geometry=1;
    settings.camera=0;Check(Replay(current,&matches,.5f)&&Pixels()!=last,"geometry-only interpolation still animates under reflected native cameras");settings.camera=1;
    Check(SupportedMotion(*previous.commands[1],posed),"motion eligibility accepts reflected retail camera");
    Matrix supportedView=posed.view;posed.view.m[0][0]*=2;Check(!SupportedMotion(*previous.commands[1],posed),"matching topology alone cannot report unsupported camera motion as interpolating");posed.view=supportedView;
    Matrix after{};DWORD shaderAfter=0;D8<38>(d,D3DTS_VIEW,&after);D8<77>(d,&shaderAfter);Check(!memcmp(&before,&after,sizeof(Matrix))&&shaderBefore==shaderAfter,"restore game view and FVF after replay");
    Check(Replay(current,nullptr,1),"restore authoritative current image by replay");Check(Pixels()==last,"replayed current image is pixel identical to native output");Check(updates==originalUpdates,"extra images do not advance fixture simulation");
    auto duplicate=std::make_unique<Command>();duplicate->kind=Command::Draw;duplicate->scene3d=true;duplicate->key=posed.key;previous.commands.push_back(std::move(duplicate));Check(Match(previous).at(posed.key)==nullptr,"ambiguous geometry identities do not blend unrelated draws");previous.commands.pop_back();
    auto originalLayout=posed.layout;posed.layout^=1;Check(!Compatible(*previous.commands[1],posed),"changed UV topology rejects history");posed.layout=originalLayout;
    auto planned=PlanReplay(current,matches);Check(planned.views.size()==1&&planned.usable>0,"prepared replay shares one camera across matched 3D draws");
    Check(Replay(current,&matches,1,&planned)&&Pixels()==last,"prepared replay produces the identical authoritative image");
    auto particleKey=current.commands[2]->key;current.commands[2]->key=~particleKey;auto unmatchedPlan=PlanReplay(current,matches);
    Check(!unmatchedPlan.draws[2].prior&&unmatchedPlan.draws[2].view>=0,"unmatched geometry still follows its shared interpolated camera");current.commands[2]->key=particleKey;
    replaying=false;warmup=0;nativePresent=oldPresent;oldPresent=&ProbePresent;previous.time=Now();tickStart=Now();clockValid=true;capturing=true;
    Check(SUCCEEDED(Present(d,nullptr,nullptr,nullptr,nullptr)),"full hooked presentation succeeds on real driver");
    Check(presentTimes.size()==2&&paired,"eligible frame presents midpoint and current image");
    Check(presentTimes[1]-presentTimes[0]>=.007&&presentTimes[1]-tickStart>=1.0/60-.0001,"two actual presents occupy separate 120 Hz deadlines inside a 60 Hz tick");
    Check(updates==originalUpdates,"full presentation path never calls simulation update");Wait();Check(!paired&&clockValid,"wait consumes paired tick without a second native delay");
    tickStart=Now();double fallbackStart=tickStart;pacedTick=true;Wait();Check(Now()-fallbackStart>=1.0/60-.0001&&!pacedTick,"unpaired fallback tick still waits for the 60 Hz boundary");
    double scheduled=Now()-1.0/60-.001;tickStart=scheduled;pacedTick=true;Wait();Check(std::abs(tickStart-(scheduled+1.0/60))<.0000001,"present and cleanup overhead do not shift the 60 Hz clock every tick");
    tickStart=Now()-.2;pacedTick=true;Wait();Check(Now()-tickStart<.01,"a long stall resynchronizes without catch-up simulation bursts");
    {auto saved=settings.enabled;settings.enabled=0;tickStart=Now();double nativeStart=tickStart;pacedTick=paired=false;Wait();
        Check(Now()-nativeStart>=1.0/60-.0001&&std::abs(tickStart-(nativeStart+1.0/60))<.0000001,"native-only and FPS 60 ticks use the precise 60 Hz clock instead of the CRI pulse");
        Check(pacingTimer!=nullptr,"deadline timer stays available for native ticks");settings.enabled=saved;}
    settings.targetHz=180;settings.showFps=1;SetOverlay(&ProbeOverlay);Fixture(1.2f);uint64_t authoritative180=Pixels();capturing=true;previous.time=Now();tickStart=Now();presentTimes.clear();presentPixels.clear();samplePixels=true;warmup=0;replayCost=.00005;
    uint64_t outputBefore=frameRates.output.total,baseBefore=frameRates.base.total;
    Check(SUCCEEDED(Present(d,nullptr,nullptr,nullptr,nullptr))&&presentTimes.size()==3&&paired,"180 Hz presents two interpolated images and one native image");samplePixels=false;
    Check(frameRates.output.total==outputBefore+3&&frameRates.base.total==baseBefore+1&&overlayCalls==3,"successful real presents drive FPS counts and the overlay exactly once each");settings.showFps=0;SetOverlay(nullptr);
    Check(presentPixels.size()==3&&presentPixels[0]!=presentPixels[1]&&presentPixels[1]!=presentPixels[2]&&presentPixels[2]==authoritative180,"one-third and two-thirds images are distinct and final image matches native rendering");
    Check(presentTimes[1]-presentTimes[0]>.0045&&presentTimes[2]-presentTimes[1]>.0045&&presentTimes[2]-tickStart<.022,"180 Hz frames are evenly separated within one simulation tick");Wait();
    Check(!commandPool.empty()&&pooledBytes>0,"frame retirement retains reusable capture commands and state blocks");auto reusable=Acquire();DWORD recycledFvf=reusable->gpu.fvf;commandPool.push_back(std::move(reusable));pooledBytes+=Storage(*commandPool.back());Check(recycledFvf!=0,"retired draw keeps reusable CPU pipeline snapshot storage");
    Fixture(1.3f);capturing=true;previous.time=Now();tickStart=Now();presentTimes.clear();warmup=3;Check(SUCCEEDED(Present(d,nullptr,nullptr,nullptr,nullptr))&&presentTimes.size()==1&&!paired&&warmup==2,"new level warm-up presents native frames without replay bursts");Wait();
    Fixture(1.4f);capturing=true;previous.time=Now();tickStart=Now();presentTimes.clear();warmup=0;stallPresent=true;replayCost=.00005;Check(SUCCEEDED(Present(d,nullptr,nullptr,nullptr,nullptr)),"transient slow driver presentation preserves valid current image");Wait();
    Fixture(1.5f);capturing=true;previous.time=Now();tickStart=Now();presentTimes.clear();Check(SUCCEEDED(Present(d,nullptr,nullptr,nullptr,nullptr))&&presentTimes.size()==3,"interpolation recovers on the next frame without a one-second cooldown");Wait();settings.targetHz=120;
    for(uint32_t rate:{240u,360u,480u,0u}){
        int beforeUpdates=updates;settings.targetHz=rate;Fixture(1.6f);capturing=true;previous.time=Now();tickStart=Now();presentTimes.clear();warmup=0;replayCost=.00005;
        Check(SUCCEEDED(Present(d,nullptr,nullptr,nullptr,nullptr))&&presentTimes.size()>1,"higher and unlocked targets submit extra images");
        Check(updates==beforeUpdates+1,"higher targets preserve one simulation update per fixture");
        Wait();
    }
    settings.targetHz=180;Fixture(1.7f);capturing=true;previous.time=Now();double pressureNow=Now();tickStart=_mm_cvtsd_f64(_mm_sub_sd(_mm_set_sd(pressureNow),_mm_set_sd(.004)));presentTimes.clear();warmup=0;replayCost=.002;
    Check(std::abs((pressureNow-tickStart)-.004)<.000001,"pressure fixture retains millisecond precision under native D3D FPU mode");auto pressureResult=Present(d,nullptr,nullptr,nullptr,nullptr);std::cout<<"Pressure fixture: hr="<<pressureResult<<" images="<<presentTimes.size()<<" reasons="<<timing::frame.reasons<<" plan="<<timing::frame.plannedImages<<" status="<<counters.status<<"\n";
    Check(SUCCEEDED(pressureResult)&&presentTimes.size()==3&&counters.status==Active,"first-slot pressure keeps all 180 Hz images instead of dropping the tick to 120");
    Check(presentTimes.size()==3&&presentTimes[0]-tickStart>.0058&&presentTimes[1]-presentTimes[0]>.0045&&presentTimes[1]-presentTimes[0]<.0058&&presentTimes[2]-presentTimes[1]>.0045&&presentTimes[2]-presentTimes[1]<.0058,"late first image is presented when ready and the rest are evenly spaced");Wait();
    Fixture(1.75f);capturing=true;previous.time=Now();pressureNow=Now();tickStart=_mm_cvtsd_f64(_mm_sub_sd(_mm_set_sd(pressureNow),_mm_set_sd(.007)));presentTimes.clear();warmup=0;replayCost=.002;
    pressureResult=Present(d,nullptr,nullptr,nullptr,nullptr);
    Check(SUCCEEDED(pressureResult)&&presentTimes.size()==2,"heavy first-slot pressure drops only images that would be closer than 3/4 interval");
    Check(presentTimes.size()==2&&presentTimes[0]-tickStart>.009&&presentTimes[1]-presentTimes[0]>.0065,"remaining image keeps a full gap before the native image");Wait();
    settings.targetHz=120;
    Fixture(1.1f);capturing=true;tickStart=Now()-.1;presentTimes.clear();Check(SUCCEEDED(Present(d,nullptr,nullptr,nullptr,nullptr))&&presentTimes.size()==1&&!paired,"late frame falls back to one native present without catch-up updates");
    oldPresent=nativePresent;
    // Menu draws use an orthographic view and interpolate independently of the
    // gameplay camera/geometry switches. Exercise actual retained render output.
    auto menuFixture=[&](float x){current=Frame{};capturing=true;menuFrame=true;hudPhase=false;Matrix unit=Identity();D8<37>(d,D3DTS_WORLD,&unit);D8<37>(d,D3DTS_VIEW,&unit);D8<37>(d,D3DTS_PROJECTION,&unit);D8<50>(d,D3DRS_ZENABLE,DWORD(FALSE));D8<76>(d,Fvf);D8<36>(d,DWORD(0),static_cast<const D3DRECT*>(nullptr),DWORD(D3DCLEAR_TARGET),DWORD(0xff101010),1.f,DWORD(0));D8<34>(d);Vertex triangle[]={{x-.3f,-.3f,.5f,0,0,-1,0xff00ffff},{x+.3f,-.3f,.5f,0,0,-1,0xff00ffff},{x,.3f,.5f,0,0,-1,0xff00ffff}};D8<72>(d,D3DPT_TRIANGLELIST,UINT(1),triangle,UINT(sizeof(Vertex)));D8<35>(d);capturing=false;};
    ClearHistory();menuFixture(-.3f);previous=std::move(current);menuFixture(.3f);uint64_t menuCurrent=Pixels();auto menuMatches=Match(previous);settings.camera=settings.geometry=0;replaying=true;Check(current.commands[1]->canvas&&SupportedMotion(*previous.commands[1],*current.commands[1]),"orthographic menu geometry is eligible independently of gameplay toggles");
    Check(Replay(current,&menuMatches,1.f/3)&&Pixels()!=menuCurrent,"native menu geometry renders a real intermediate image");Check(Replay(current,nullptr,1)&&Pixels()==menuCurrent,"menu replay restores exact native current frame");replaying=false;menuFrame=false;settings.camera=settings.geometry=1;ClearHistory();
    auto nativeMenu=TexturedMenuTests();Check(TexturedMenuTests(true)==nativeMenu,"deferred textured menu matches independently rendered native pixels");ViewportReplayTests();BufferMirrorTests();auto nativeShader=PixelShaderTests();Check(PixelShaderTests(true)==nativeShader,"deferred pixel-shaded geometry matches independently rendered native pixels");DeferredTests();ActionMenuTests();GameplayPrefixTests();DuplicatePairingTests();DuplicateMotionTests();
    void* texture=nullptr;Check(SUCCEEDED(D8<20>(d,UINT(4),UINT(4),UINT(1),DWORD(0),D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture)),"create mutable effect texture");Check(ObserveTexture(texture),"observe texture write boundary");capturing=true;current.valid=true;D3DLOCKED_RECT texLock{};Check(SUCCEEDED(D8<16>(texture,UINT(0),&texLock,static_cast<const RECT*>(nullptr),DWORD(0))),"native effect texture writes remain permitted");D8<17>(texture,UINT(0));Check(!capturing&&!current.valid&&counters.status==Unsupported,"texture rewritten after an earlier draw rejects replay");Release(texture);
    replaying=false;capturing=false;Release(vb);Release(ib);vb=ib=nullptr;ClearHistory();D8<83>(d,UINT(0),static_cast<void*>(nullptr),UINT(0));D8<85>(d,static_cast<void*>(nullptr),UINT(0));
    Check(SUCCEEDED(D8<14>(d,&pp)),"device Reset releases retained frames and stateblocks");Check(previous.commands.empty()&&current.commands.empty()&&!clockValid,"reset invalidates all interpolation history");
    Check(SUCCEEDED(D8<11>(d3d,UINT(0),D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,TRUE,D3DMULTISAMPLE_8_SAMPLES)),"fixture hardware supports user's 8x MSAA mode");
    pp.samples=D3DMULTISAMPLE_8_SAMPLES;pp.flags=0;Check(SUCCEEDED(D8<14>(d,&pp)),"reset to non-lockable 8x MSAA backbuffer");
    Check(SUCCEEDED(D8<23>(d,UINT(sizeof(Vertex)*5),DWORD(D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY),Fvf,D3DPOOL_DEFAULT,&vb)),"recreate MSAA fixture vertex buffer");Check(SUCCEEDED(D8<24>(d,UINT(6),DWORD(D3DUSAGE_WRITEONLY),DWORD(D3DFMT_INDEX16),D3DPOOL_MANAGED,&ib)),"recreate MSAA fixture index buffer");
    D8<11>(ib,UINT(0),UINT(6),&indices,DWORD(0));memcpy(indices,indexValues,6);D8<12>(ib);
    Fixture(0);previous=std::move(current);Fixture(1);auto msaaMatches=Match(previous);replaying=true;Check(Replayable(current)&&Replay(current,&msaaMatches,.5f)&&Replay(current,nullptr,1),"midpoint and current geometry render with 8x MSAA without framebuffer readback");replaying=false;
    deferCandidate=true;Fixture(1.1f);Check(deferredPending,"multisampled frame uses deferred capture without readback");capturing=true;clockValid=true;warmup=0;previous.time=Now();tickStart=Now();settings.targetHz=180;replayCost=.00005;nativePresent=oldPresent;oldPresent=&ProbePresent;presentTimes.clear();
    Check(SUCCEEDED(Present(device,nullptr,nullptr,nullptr,nullptr))&&presentTimes.size()==3&&!deferredPending,"deferred frame submits all three images with 8x MSAA");oldPresent=nativePresent;Wait();
    Fixture(1.2f);HeavyReplayBench();Release(vb);Release(ib);vb=ib=nullptr;ClearHistory();D8<83>(d,UINT(0),static_cast<void*>(nullptr),UINT(0));D8<85>(d,static_cast<void*>(nullptr),UINT(0));
    Release(d);Release(device);device=nullptr;Release(d3d);DestroyWindow(window);Timer(false);Check(shadows.empty()&&shadowBytes==0,"resource destruction retires buffer identities and memory");
    std::cout<<tests<<" checks passed; synthetic render fixtures, not gameplay validation.\n";return 0;
}
