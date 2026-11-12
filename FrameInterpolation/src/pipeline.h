#pragma once
// Included inside fi. Track fixed-function state after downstream mods have
// applied their overrides. No per-draw driver state-block allocation/capture.
namespace pipeline {
struct Light {DWORD id=0;D3DLIGHT9 value{};BOOL enabled=FALSE;};
struct State {
    std::array<DWORD,256> rs{};std::array<bool,256> hasRs{};
    std::array<std::array<DWORD,33>,8> tss{};
    std::array<std::array<bool,33>,8> hasTss{};
    std::array<void*,8> textures{};
    std::array<Matrix,11> transforms{};
    D3DMATERIAL9 material{};
    std::array<std::array<float,4>,6> clips{};
    std::array<bool,6> hasClip{};
    std::vector<Light> lights;
    DWORD fvf=0,ps=0,palette=0;
    std::array<std::array<float,4>,8> psConstants{};
};
inline State live;inline bool dirty=true,recording=false;
inline std::unordered_set<DWORD> lightIds{0,1,2,3,4,5,6,7};
inline int TransformIndex(DWORD type){return type==D3DTS_WORLD?0:type==D3DTS_VIEW?1:type==D3DTS_PROJECTION?2:type>=16&&type<24?int(type)-13:-1;}
inline DWORD TransformType(int index){return index==0?D3DTS_WORLD:index==1?D3DTS_VIEW:index==2?D3DTS_PROJECTION:DWORD(index+13);}
inline void VerifyHooks();
inline bool Sync(){
    VerifyHooks();
    if(recording)return false;if(!dirty)return true;
    if(lightIds.size()>64)return false;
    live.hasRs.fill(false);for(DWORD i=1;i<256;i++)live.hasRs[i]=SUCCEEDED(D8<51>(device,i,&live.rs[i]));
    for(DWORD i=0;i<8;i++){
        for(DWORD j=1;j<33;j++)live.hasTss[i][j]=SUCCEEDED(D8<62>(device,i,j,&live.tss[i][j]));
        void* texture=nullptr;if(FAILED(D8<60>(device,i,&texture)))return false;live.textures[i]=texture;Release(texture);
    }
    for(int i=0;i<11;i++)if(FAILED(D8<38>(device,TransformType(i),&live.transforms[i])))return false;
    if(FAILED(D8<43>(device,&live.material))||FAILED(D8<77>(device,&live.fvf))||FAILED(D8<89>(device,&live.ps)))return false;
    D8<69>(device,&live.palette);
    for(DWORD i=0;i<6;i++)live.hasClip[i]=SUCCEEDED(D8<49>(device,i,live.clips[i].data()));
    live.lights.clear();for(auto id:lightIds){Light l;l.id=id;if(SUCCEEDED(D8<45>(device,id,&l.value))&&SUCCEEDED(D8<47>(device,id,&l.enabled)))live.lights.push_back(l);}
    dirty=false;return true;
}
using Pair=HRESULT(__stdcall*)(void*,DWORD,DWORD);
using Triple=HRESULT(__stdcall*)(void*,DWORD,DWORD,DWORD);
using Pointer=HRESULT(__stdcall*)(void*,DWORD,const void*);
using Material=HRESULT(__stdcall*)(void*,const D3DMATERIAL9*);
using Texture=HRESULT(__stdcall*)(void*,DWORD,void*);
using Single=HRESULT(__stdcall*)(void*,DWORD);
using Begin=HRESULT(__stdcall*)(void*);
using End=HRESULT(__stdcall*)(void*,DWORD*);
using Constants=HRESULT(__stdcall*)(void*,DWORD,const void*,DWORD);
inline Pair rs=nullptr,enable=nullptr;inline Triple tss=nullptr;
inline Pointer transform=nullptr,multiply=nullptr,light=nullptr,clip=nullptr;
inline Material material=nullptr;inline Texture texture=nullptr;
inline Single vs=nullptr,ps=nullptr,palette=nullptr,apply=nullptr;
inline Begin begin=nullptr;inline End end=nullptr;
using Create=HRESULT(__stdcall*)(void*,D3DSTATEBLOCKTYPE,DWORD*);
inline Create create=nullptr;inline Single capture=nullptr;
inline Constants constants=nullptr;inline Single deletePs=nullptr;
struct Hook {int slot;void* ours;void* original;};
inline std::vector<Hook> hooks;
inline void VerifyHooks(){
    if(recording||!device)return;auto table=*static_cast<void***>(device);
    for(const auto& h:hooks)if(table[h.slot]!=h.ours){
        // Some D3D8 state-block paths replace setters internally, bypassing COM
        // Begin/End hooks. Repair only our known original, preserve other mods.
        dirty=true;if(table[h.slot]==h.original)Write(table+h.slot,&h.ours,4);
    }
}
struct PreserveTable {
    void* d;std::array<void*,97> saved{};
    explicit PreserveTable(void* value):d(value){std::copy_n(*static_cast<void***>(d),saved.size(),saved.begin());}
    ~PreserveTable(){auto table=*static_cast<void***>(d);for(size_t i=0;i<saved.size();i++)if(table[i]!=saved[i])Write(table+i,&saved[i],4);}
};

inline HRESULT __stdcall RenderState(void* d,DWORD type,DWORD value){auto hr=rs(d,type,value);if(SUCCEEDED(hr)&&!recording){if(type<256){live.rs[type]=value;live.hasRs[type]=true;}else dirty=true;}return hr;}
inline HRESULT __stdcall TextureState(void* d,DWORD unit,DWORD type,DWORD value){auto hr=tss(d,unit,type,value);if(SUCCEEDED(hr)&&!recording){if(unit<8&&type<33){live.tss[unit][type]=value;live.hasTss[unit][type]=true;}else dirty=true;}return hr;}
inline HRESULT __stdcall Transform(void* d,DWORD type,const void* value){auto hr=transform(d,type,value);int i=TransformIndex(type);if(SUCCEEDED(hr)&&!recording&&i>=0)memcpy(&live.transforms[i],value,sizeof(Matrix));return hr;}
inline HRESULT __stdcall Multiply(void* d,DWORD type,const void* value){auto hr=multiply(d,type,value);if(SUCCEEDED(hr)&&!recording)dirty=true;return hr;}
inline HRESULT __stdcall SetMaterial(void* d,const D3DMATERIAL9* value){auto hr=material(d,value);if(SUCCEEDED(hr)&&!recording)live.material=*value;return hr;}
inline HRESULT __stdcall SetTexture(void* d,DWORD unit,void* value){auto hr=texture(d,unit,value);if(SUCCEEDED(hr)&&!recording&&unit<8)live.textures[unit]=value;return hr;}
inline Light* KnownLight(DWORD id){for(auto& l:live.lights)if(l.id==id)return &l;return nullptr;}
inline HRESULT __stdcall SetLight(void* d,DWORD id,const void* value){
    auto hr=light(d,id,value);if(SUCCEEDED(hr)){lightIds.insert(id);
        if(recording)dirty=true;
        else if(!dirty){auto old=KnownLight(id);if(old)memcpy(&old->value,value,sizeof(D3DLIGHT9));else dirty=true;}
    }return hr;
}
inline HRESULT __stdcall EnableLight(void* d,DWORD id,DWORD value){
    auto hr=enable(d,id,value);if(SUCCEEDED(hr)){lightIds.insert(id);
        if(recording)dirty=true;
        else if(!dirty){auto old=KnownLight(id);if(old)old->enabled=BOOL(value);else dirty=true;}
    }return hr;
}
inline HRESULT __stdcall Clip(void* d,DWORD id,const void* value){auto hr=clip(d,id,value);if(SUCCEEDED(hr)&&!recording&&id<6){memcpy(live.clips[id].data(),value,16);live.hasClip[id]=true;}return hr;}
inline HRESULT __stdcall VertexShader(void* d,DWORD value){auto hr=vs(d,value);if(SUCCEEDED(hr)&&!recording)live.fvf=value;return hr;}
inline HRESULT __stdcall PixelShader(void* d,DWORD value){auto hr=ps(d,value);if(SUCCEEDED(hr)&&!recording)live.ps=value;return hr;}
inline HRESULT __stdcall PixelConstants(void* d,DWORD first,const void* value,DWORD count){auto hr=constants(d,first,value,count);if(SUCCEEDED(hr)&&!recording&&first<=8&&count<=8-first)memcpy(live.psConstants.data()+first,value,count*16);return hr;}
inline HRESULT __stdcall DeletePixel(void* d,DWORD value){if(!replaying&&!FlushDeferred())return D3DERR_INVALIDCALL;auto hr=deletePs(d,value);if(SUCCEEDED(hr))PixelShaderDeleted(value);return hr;}
inline HRESULT __stdcall Palette(void* d,DWORD value){auto hr=palette(d,value);if(SUCCEEDED(hr)&&!recording)live.palette=value;return hr;}
// An ALL state block captured while the tracked copy was exact puts back exactly that copy, so applying it needs no
// read-back of every state on the next draw (the restore after each replay, the FPS box, other mods).
inline std::unordered_map<DWORD,State> blockStates;
inline HRESULT __stdcall Apply(void* d,DWORD value){PreserveTable keep(d);auto hr=apply(d,value);
    if(SUCCEEDED(hr)){auto it=blockStates.find(value);if(it!=blockStates.end()&&!recording){live=it->second;dirty=false;}else dirty=true;}return hr;}
inline std::array<void*,97> beforeRecording{};
inline Pointer recordLight=nullptr;inline Pair recordEnable=nullptr;
inline HRESULT __stdcall RecordLight(void* d,DWORD id,const void* value){auto hr=recordLight(d,id,value);if(SUCCEEDED(hr))lightIds.insert(id);return hr;}
inline HRESULT __stdcall RecordEnable(void* d,DWORD id,DWORD value){auto hr=recordEnable(d,id,value);if(SUCCEEDED(hr))lightIds.insert(id);return hr;}
inline HRESULT __stdcall BeginBlock(void* d){
    if(recording)return begin(d);
    auto table=*static_cast<void***>(d);std::copy_n(table,beforeRecording.size(),beforeRecording.begin());
    auto hr=begin(d);if(SUCCEEDED(hr)){
        recording=true;table=*static_cast<void***>(d);
        // D3D8 replaces setter entries while recording and restores its own
        // originals at EndStateBlock. Preserve the complete existing mod chain.
        recordLight=reinterpret_cast<Pointer>(table[44]);recordEnable=reinterpret_cast<Pair>(table[46]);
        void* l=reinterpret_cast<void*>(&RecordLight);void* e=reinterpret_cast<void*>(&RecordEnable);
        Write(table+44,&l,4);Write(table+46,&e,4);
    }return hr;
}
inline HRESULT __stdcall EndBlock(void* d,DWORD* value){
    auto hr=end(d,value);if(SUCCEEDED(hr)){
        auto table=*static_cast<void***>(d);
        for(size_t i=0;i<beforeRecording.size();i++)if(table[i]!=beforeRecording[i])Write(table+i,&beforeRecording[i],4);
        recording=false;dirty=true;if(value)blockStates.erase(*value);
    }return hr;
}
inline HRESULT __stdcall CreateBlock(void* d,D3DSTATEBLOCKTYPE type,DWORD* token){PreserveTable keep(d);auto hr=create(d,type,token);
    if(SUCCEEDED(hr)&&token){if(type==D3DSBT_ALL&&!dirty&&!recording)blockStates[*token]=live;else blockStates.erase(*token);}return hr;}
inline HRESULT __stdcall CaptureBlock(void* d,DWORD token){PreserveTable keep(d);auto hr=capture(d,token);
    if(SUCCEEDED(hr)){auto it=blockStates.find(token);if(it!=blockStates.end()){if(!dirty&&!recording)it->second=live;else blockStates.erase(it);}}return hr;}
template<class F> bool HookSlot(void** table,int i,F next,F& old){if(!Slot(table,i,next,old))return false;hooks.push_back({i,reinterpret_cast<void*>(next),reinterpret_cast<void*>(old)});return true;}
inline bool Install(void** table){
    dirty=true;hooks.clear();
    return HookSlot(table,50,&RenderState,rs)&&HookSlot(table,63,&TextureState,tss)&&HookSlot(table,37,&Transform,transform)&&HookSlot(table,39,&Multiply,multiply)&&HookSlot(table,42,&SetMaterial,material)&&HookSlot(table,44,&SetLight,light)&&HookSlot(table,46,&EnableLight,enable)&&HookSlot(table,48,&Clip,clip)&&HookSlot(table,61,&SetTexture,texture)&&HookSlot(table,76,&VertexShader,vs)&&HookSlot(table,88,&PixelShader,ps)&&HookSlot(table,91,&PixelConstants,constants)&&HookSlot(table,90,&DeletePixel,deletePs)&&HookSlot(table,68,&Palette,palette)&&HookSlot(table,54,&Apply,apply)&&HookSlot(table,52,&BeginBlock,begin)&&HookSlot(table,53,&EndBlock,end)&&HookSlot(table,55,&CaptureBlock,capture)&&HookSlot(table,57,&CreateBlock,create);
}
inline bool Capture(State& state){
    if(!Sync()||(live.fvf&1)||live.rs[D3DRS_VERTEXBLEND]||live.rs[D3DRS_INDEXEDVERTEXBLENDENABLE])return false;
    // Pixel shaders affect shading, not CPU-skinned geometry correspondence.
    // D3D8 has eight float4 PS constants. Read actual values for shaded draws,
    // including native code using previously cached driver setter pointers.
    if(live.ps&&FAILED(D8<92>(device,DWORD(0),live.psConstants.data(),DWORD(8))))return false;
    state=live;return true;
}
inline bool ApplyDraw(const State& s,const Matrix* world=nullptr,const Matrix* view=nullptr,const Matrix* projection=nullptr){
    if(!Sync())return false;
    bool ok=true;
    if(live.rs!=s.rs||live.hasRs!=s.hasRs)
        for(DWORD i=1;i<256;i++)if(s.hasRs[i]&&(!live.hasRs[i]||live.rs[i]!=s.rs[i]))ok=SUCCEEDED(RenderState(device,i,s.rs[i]))&&ok;
    for(DWORD i=0;i<8;i++){
        if(live.textures[i]!=s.textures[i])ok=SUCCEEDED(SetTexture(device,i,s.textures[i]))&&ok;
        if(live.tss[i]!=s.tss[i]||live.hasTss[i]!=s.hasTss[i])
            for(DWORD j=1;j<33;j++)if(s.hasTss[i][j]&&(!live.hasTss[i][j]||live.tss[i][j]!=s.tss[i][j]))ok=SUCCEEDED(TextureState(device,i,j,s.tss[i][j]))&&ok;
    }
    for(int i=0;i<11;i++){const Matrix* value=i==0&&world?world:i==1&&view?view:i==2&&projection?projection:&s.transforms[i];
        if(memcmp(&live.transforms[i],value,sizeof(Matrix)))ok=SUCCEEDED(Transform(device,TransformType(i),value))&&ok;
    }
    if(memcmp(&live.material,&s.material,sizeof(s.material)))ok=SUCCEEDED(SetMaterial(device,&s.material))&&ok;
    for(DWORD i=0;i<6;i++)if(s.hasClip[i]&&(!live.hasClip[i]||memcmp(live.clips[i].data(),s.clips[i].data(),16)))ok=SUCCEEDED(Clip(device,i,s.clips[i].data()))&&ok;
    // Disable lights absent from the captured draw. Original arbitrary light IDs
    // are retained; no assumption that IDs equal hardware light-slot indices.
    for(const auto& l:live.lights)if(l.enabled){bool found=false;for(const auto& v:s.lights)if(v.id==l.id){found=true;break;}if(!found)ok=SUCCEEDED(enable(device,l.id,FALSE))&&ok;}
    for(const auto& l:s.lights){const Light* old=nullptr;for(const auto& v:live.lights)if(v.id==l.id){old=&v;break;}
        if(!old||memcmp(&old->value,&l.value,sizeof(l.value)))ok=SUCCEEDED(light(device,l.id,&l.value))&&ok;
        if(!old||old->enabled!=l.enabled)ok=SUCCEEDED(enable(device,l.id,DWORD(l.enabled)))&&ok;
    }live.lights=s.lights;
    if(live.fvf!=s.fvf)ok=SUCCEEDED(VertexShader(device,s.fvf))&&ok;
    if(live.ps!=s.ps)ok=SUCCEEDED(PixelShader(device,s.ps))&&ok;
    // A state-block restore can replace constants without our setter running.
    // Set all eight on shader draws; 128 bytes avoids a stale-cache dependency.
    if(s.ps)ok=SUCCEEDED(PixelConstants(device,0,s.psConstants.data(),8))&&ok;
    if(live.palette!=s.palette)ok=SUCCEEDED(Palette(device,s.palette))&&ok;
    return ok;
}
}
