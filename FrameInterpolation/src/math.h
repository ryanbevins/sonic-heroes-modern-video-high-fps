#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <emmintrin.h>
#include <smmintrin.h>
#include <intrin.h>
namespace motion {
struct V3 {float x,y,z;};
inline V3 operator+(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline V3 operator-(V3 a,V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline V3 operator*(V3 a,float t){return {a.x*t,a.y*t,a.z*t};}
inline float Dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline V3 Cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline bool Finite(V3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
inline V3 Unit(V3 a){float l=std::sqrt(Dot(a,a));return l>1e-8f?a*(1/l):V3{};}
struct Quat {float x,y,z,w;};
struct Matrix {float m[4][4];};
inline Matrix Identity(){Matrix r{};for(int i=0;i<4;++i)r.m[i][i]=1;return r;}
inline Quat Rotation(const Matrix& a){
    // Quaternion from transposed row-vector rotation matrix.
    float c[3][3];for(int i=0;i<3;i++)for(int j=0;j<3;j++)c[i][j]=a.m[j][i];
    Quat q{};float t=c[0][0]+c[1][1]+c[2][2];
    if(t>0){float s=std::sqrt(t+1)*2;q.w=s*.25f;q.x=(c[2][1]-c[1][2])/s;q.y=(c[0][2]-c[2][0])/s;q.z=(c[1][0]-c[0][1])/s;}
    else {int i=c[1][1]>c[0][0]?1:0;if(c[2][2]>c[i][i])i=2;int j=(i+1)%3,k=(i+2)%3;float s=std::sqrt(std::max(0.f,1+c[i][i]-c[j][j]-c[k][k]))*2;if(s<1e-7f)return {0,0,0,1};float v[3]{};v[i]=s*.25f;v[j]=(c[i][j]+c[j][i])/s;v[k]=(c[i][k]+c[k][i])/s;q={v[0],v[1],v[2],(c[k][j]-c[j][k])/s};}return q;
}
inline Quat Blend(Quat a,Quat b,float t){
    float dot=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;if(dot<0){b={-b.x,-b.y,-b.z,-b.w};dot=-dot;}
    float u=1-t,v=t;if(dot<.9995f){float theta=std::acos(std::clamp(dot,-1.f,1.f)),s=std::sin(theta);u=std::sin((1-t)*theta)/s;v=std::sin(t*theta)/s;}
    Quat q{a.x*u+b.x*v,a.y*u+b.y*v,a.z*u+b.z*v,a.w*u+b.w*v};float l=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);if(l<1e-7f)return {0,0,0,1};return {q.x/l,q.y/l,q.z/l,q.w/l};
}
inline Matrix FromQuat(Quat q){
    Matrix a=Identity();float x=q.x,y=q.y,z=q.z,w=q.w;
    a.m[0][0]=1-2*(y*y+z*z);a.m[0][1]=2*(x*y+z*w);a.m[0][2]=2*(x*z-y*w);
    a.m[1][0]=2*(x*y-z*w);a.m[1][1]=1-2*(x*x+z*z);a.m[1][2]=2*(y*z+x*w);
    a.m[2][0]=2*(x*z+y*w);a.m[2][1]=2*(y*z-x*w);a.m[2][2]=1-2*(x*x+y*y);return a;
}
inline V3 Row(const Matrix& a,int i){return {a.m[i][0],a.m[i][1],a.m[i][2]};}
inline void Row(Matrix& a,int i,V3 v){a.m[i][0]=v.x;a.m[i][1]=v.y;a.m[i][2]=v.z;}
inline bool Affine(const Matrix& a){
    for(auto& row:a.m)for(float x:row)if(!std::isfinite(x))return false;
    return std::abs(a.m[0][3])+std::abs(a.m[1][3])+std::abs(a.m[2][3])+std::abs(a.m[3][3]-1)<.001f;
}
inline bool Decompose(const Matrix& a,Matrix& rotation,V3& scale){
    if(!Affine(a))return false;V3 rows[3]={Row(a,0),Row(a,1),Row(a,2)};float s[3];rotation=Identity();
    for(int i=0;i<3;i++){s[i]=std::sqrt(Dot(rows[i],rows[i]));if(s[i]<1e-6f)return false;rows[i]=rows[i]*(1/s[i]);Row(rotation,i,rows[i]);}
    float handedness=Dot(Cross(rows[0],rows[1]),rows[2]);
    if(std::abs(Dot(rows[0],rows[1]))>.002f||std::abs(Dot(rows[0],rows[2]))>.002f||std::abs(Dot(rows[1],rows[2]))>.002f||std::abs(handedness)<.99f)return false;
    // Keep reflection in a signed scale; quaternions represent proper rotations.
    if(handedness<0){s[2]=-s[2];Row(rotation,2,rows[2]*-1);}
    scale={s[0],s[1],s[2]};return true;
}
inline bool Transform(const Matrix& a,const Matrix& b,float t,Matrix& out){
    Matrix ar{},br{};V3 as{},bs{};if(!Decompose(a,ar,as)||!Decompose(b,br,bs)||as.z*bs.z<0)return false;
    out=FromQuat(Blend(Rotation(ar),Rotation(br),t));V3 s=as+(bs-as)*t;
    Row(out,0,Row(out,0)*s.x);Row(out,1,Row(out,1)*s.y);Row(out,2,Row(out,2)*s.z);Row(out,3,Row(a,3)+(Row(b,3)-Row(a,3))*t);return true;
}
inline Matrix InverseRigid(const Matrix& a){Matrix r=Identity();for(int i=0;i<3;i++)for(int j=0;j<3;j++)r.m[i][j]=a.m[j][i];V3 p=Row(a,3);for(int i=0;i<3;i++)r.m[3][i]=-Dot(p,Row(a,i));return r;}
inline bool Camera(const Matrix& a,const Matrix& b,float t,Matrix& out,bool protect){
    // Retail RenderWare views are orthogonal but reflected (determinant -1).
    // Factor out a fixed camera-space reflection before quaternion interpolation,
    // then restore it. The translation column component must be reflected too.
    bool reflected=Dot(Cross(Row(a,0),Row(a,1)),Row(a,2))<0;
    if(reflected!=(Dot(Cross(Row(b,0),Row(b,1)),Row(b,2))<0))return false;
    Matrix av=a,bv=b;if(reflected)for(int i=0;i<4;i++){av.m[i][0]=-av.m[i][0];bv.m[i][0]=-bv.m[i][0];}
    Matrix ar{},br{};V3 as{},bs{};if(!Decompose(av,ar,as)||!Decompose(bv,br,bs))return false;
    if(std::abs(as.x-1)+std::abs(as.y-1)+std::abs(as.z-1)+std::abs(bs.x-1)+std::abs(bs.y-1)+std::abs(bs.z-1)>.01f)return false;
    Matrix aw=InverseRigid(av),bw=InverseRigid(bv),world{};V3 d=Row(aw,3)-Row(bw,3);
    if(protect&&(Dot(d,d)>250*250||Dot(Row(aw,2),Row(bw,2))<.5f))return false;
    if(!Transform(aw,bw,t,world))return false;out=InverseRigid(world);if(reflected)for(int i=0;i<4;i++)out.m[i][0]=-out.m[i][0];return true;
}
inline bool Projection(const Matrix& a,const Matrix& b,float t,Matrix& out,bool protect){
    if(std::abs(a.m[2][3]-b.m[2][3])>.001f||std::abs(a.m[2][3])<.5f||std::abs(a.m[3][3])+std::abs(b.m[3][3])>.001f)return false;
    if(a.m[0][0]<=0||a.m[1][1]<=0||b.m[0][0]<=0||b.m[1][1]<=0)return false;
    if(protect&&(b.m[0][0]/a.m[0][0]<.5f||b.m[0][0]/a.m[0][0]>2||b.m[1][1]/a.m[1][1]<.5f||b.m[1][1]/a.m[1][1]>2))return false;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++){if(!std::isfinite(a.m[i][j])||!std::isfinite(b.m[i][j]))return false;out.m[i][j]=a.m[i][j]+(b.m[i][j]-a.m[i][j])*t;}return true;
}
// FNV-1a 64 computed in 32-bit halves: the same result as h*=1099511628211 per byte without a 64-bit multiply,
// which a Win32 build turns into a call to _allmul for every byte. 1099511628211 = 2^40 + 0x1B3, so
// h*p = lo*0x1B3 + ((hi*0x1B3 + (lo<<8)) << 32) (mod 2^64).
inline uint64_t Hash(const void* data,size_t n,uint64_t h=14695981039346656037ull){auto p=static_cast<const uint8_t*>(data);uint32_t lo=uint32_t(h),hi=uint32_t(h>>32);for(size_t i=0;i<n;i++){lo^=p[i];const uint64_t m=uint64_t(lo)*0x1B3u;hi=hi*0x1B3u+uint32_t(m>>32)+(lo<<8);lo=uint32_t(m);}return (uint64_t(hi)<<32)|lo;}
inline bool Positions(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,unsigned stride,float t,bool normals,bool protect,std::vector<uint8_t>& out){
    if(stride<12||a.size()!=b.size()||b.size()%stride)return false;out=b;
    // Load/store precisely 12 bytes, including the final tightly packed vertex.
    // SSE2 keeps XYZ together while preserving the scalar operation order and
    // exact sqrt/divide normalization (no approximate reciprocal instructions).
    auto load=[](const uint8_t* p){return _mm_movelh_ps(_mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p))),_mm_load_ss(reinterpret_cast<const float*>(p+8)));};
    auto save=[](uint8_t* p,__m128 v){_mm_storel_epi64(reinterpret_cast<__m128i*>(p),_mm_castps_si128(v));_mm_store_ss(reinterpret_cast<float*>(p+8),_mm_movehl_ps(v,v));};
    auto finite=[](__m128 v){const auto exponent=_mm_set1_epi32(0x7f800000);return _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(v),exponent),exponent))==0;};
    auto lengthSquared=[](__m128 v){auto s=_mm_mul_ps(v,v);return _mm_add_ss(_mm_add_ss(s,_mm_shuffle_ps(s,s,_MM_SHUFFLE(1,1,1,1))),_mm_shuffle_ps(s,s,_MM_SHUFFLE(2,2,2,2)));};
    const auto alpha=_mm_set1_ps(t);
    for(size_t i=0;i<b.size();i+=stride){auto av=load(a.data()+i),bv=load(b.data()+i),d=_mm_sub_ps(bv,av);
        if(!finite(av)||!finite(bv)||(protect&&_mm_cvtss_f32(lengthSquared(d))>250*250))return false;
        save(out.data()+i,_mm_add_ps(av,_mm_mul_ps(d,alpha)));
        if(normals&&stride>=24){av=load(a.data()+i+12);bv=load(b.data()+i+12);if(!finite(av)||!finite(bv))return false;
            auto n=_mm_add_ps(av,_mm_mul_ps(_mm_sub_ps(bv,av),alpha));auto length=_mm_sqrt_ss(lengthSquared(n));
            if(_mm_cvtss_f32(length)>1e-8f){auto inverse=_mm_div_ss(_mm_set_ss(1),length);n=_mm_mul_ps(n,_mm_shuffle_ps(inverse,inverse,_MM_SHUFFLE(0,0,0,0)));}else n=_mm_setzero_ps();
            save(out.data()+i+12,n);
        }
    }return true;
}
inline bool StreamingReadSupported(){static const bool supported=[](){int features[4]{};__cpuid(features,1);return (features[2]&(1<<19))!=0;}();return supported;}
inline void ReadLockedBytes(void* destination,const void* source,size_t size,bool streaming){
    if(!streaming||size<16){memcpy(destination,source,size);return;}
    auto to=static_cast<uint8_t*>(destination);auto from=static_cast<const uint8_t*>(source);
    // Finish the native skin writer's WC stores before loading that mapping.
    // MOVNTDQA uses streaming-load buffers for WC pages; ordinary memcpy reads
    // from these pages were the dominant sampled cost in character select.
    _mm_mfence();
    while(size&&(uintptr_t(from)&15)){*to++=*from++;--size;}
    while(size>=16){auto block=_mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(from)));_mm_storeu_si128(reinterpret_cast<__m128i*>(to),block);from+=16;to+=16;size-=16;}
    if(size)memcpy(to,from,size);
}
// Merge initialized CPU-written spans; never read WRITEONLY buffers back from GPU.
struct Shadow {
    uint64_t generation=0,writes=0;bool dynamic=false;std::vector<uint8_t> bytes; // writes: Unlock count
    std::vector<std::pair<size_t,size_t>> valid;
    uint8_t* locked=nullptr;size_t offset=0,length=0;bool readonly=false,streaming=false;uintptr_t mirrorPage=~uintptr_t(0);
    void MarkWritten(size_t at,size_t size){
        if(at>bytes.size()||size>bytes.size()-at)return;
        std::pair<size_t,size_t> span{at,at+size};auto it=valid.begin();while(it!=valid.end()&&it->second<span.first)++it;
        while(it!=valid.end()&&it->first<=span.second){span.first=std::min(span.first,it->first);span.second=std::max(span.second,it->second);it=valid.erase(it);}valid.insert(it,span);
    }
    bool Contains(size_t at,size_t size)const{
        if(at>bytes.size()||size>bytes.size()-at)return false;
        for(auto span:valid)if(at>=span.first&&at+size<=span.second)return true;return false;
    }
    void Write(size_t at,const void* data,size_t size,bool stream=false){
        if(at>bytes.size()||size>bytes.size()-at)return;ReadLockedBytes(bytes.data()+at,data,size,stream);MarkWritten(at,size);++writes;
    }
    bool Read(size_t at,size_t size,std::vector<uint8_t>& out)const{
        if(at>bytes.size()||size>bytes.size()-at)return false;
        for(auto span:valid)if(at>=span.first&&at+size<=span.second){out.assign(bytes.begin()+at,bytes.begin()+at+size);return true;}return false;
    }
};
}
