//============================================================================
//  AvatarExporter - Xbox 360 homebrew
//    * previews the signed-in profile's avatar with the real avatar shaders
//    * exports to a single folder: OBJ + skinned glTF (with system animations),
//      every facial-expression render, and every raw texture/emotion layer
//    * export runs on a worker thread with an on-screen progress bar
//============================================================================
#include <xtl.h>
#include <xboxmath.h>
#include <xavatar.h>
#include <xinputdefs.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "font_seg.h"
#include "bg_bliss.h"

extern "C" DWORD XInputGetState(DWORD,XINPUT_STATE*);
typedef struct _OBJ_STRING{ USHORT Length,MaximumLength; PCHAR Buffer; } OBJ_STRING;
extern "C" LONG ObCreateSymbolicLink(OBJ_STRING*,OBJ_STRING*);
extern "C" LONG ObDeleteSymbolicLink(OBJ_STRING*);
static void SetOS(OBJ_STRING* s,char* v){ s->Length=(USHORT)strlen(v); s->MaximumLength=(USHORT)(s->Length+1); s->Buffer=v; }
static void MountDrive(const char* d,const char* dev){ char l[64]; sprintf_s(l,sizeof(l),"\\??\\%s",d); OBJ_STRING sl,dv; SetOS(&sl,l); SetOS(&dv,(char*)dev); ObDeleteSymbolicLink(&sl); ObCreateSymbolicLink(&sl,&dv); }

//----------------------------------------------------------------------------
#define MAXC 16          // max components
#define MAXT 20          // max textures per component
#define MAXL 16          // max layers (emotions) per texture
#define FACE_RT 512

static IDirect3DDevice9*            g_d=NULL;
static IDirect3DVertexShader9*      g_vs3=NULL;
static IDirect3DPixelShader9*       g_psBody=NULL,*g_psHead=NULL,*g_psBg=NULL;
static IDirect3DVertexShader9*      g_vsBake=NULL; static IDirect3DPixelShader9* g_psHeadAlbedo=NULL;
static IDirect3DVertexShader9*      g_vs2=NULL; static IDirect3DPixelShader9* g_ps2=NULL;
static IDirect3DPixelShader9*       g_psCopy=NULL;
static IDirect3DVertexDeclaration9* g_declHead=NULL,*g_declBody=NULL,*g_decl2=NULL;
static IDirect3DTexture9*           g_font=NULL;
static IDirect3DTexture9*           g_white=NULL;     // 1x1 opaque white
static IDirect3DTexture9*           g_zeroA=NULL;     // 1x1 transparent black
static IDirect3DTexture9*           g_bg=NULL;        // bliss background
static IDirect3DSurface9*           g_rtSurf=NULL,*g_rtDepth=NULL;
static IDirect3DTexture9*           g_rtTex=NULL;
static bool                         g_headTexOK[MAXC]={false};

// System animations drive facial expression (texture layer) the same way they
// drive joints - XAvatarAnimation::GetPose() outputs a per-XAVATAR_ANIMATED_TEXTURE
// layer index alongside the pose. Rather than baking a combined texture per
// distinct expression combination, the raw per-feature layers (already
// exported to textures/) plus these indices are all Blender needs to
// composite the exact same result live - see face_keyframes.txt.

// all texture-layer headers, per component/texture/layer
static D3DTexture g_tex[MAXC][MAXT][MAXL];
static DWORD      g_texLayers[MAXC][MAXT]={{0}};
static DWORD      g_texN[MAXC]={0};
static BYTE       g_texUsage[MAXC][MAXT]={{0}};       // XAVATAR_SHADER_PARAM_USAGE per texture
static int        g_colorTexIdx[MAXC];                // color texture index or -1
static bool       g_isHead[MAXC]={false};
static float      g_skin[MAXC][4];                    // skin tone (for head material)
static float      g_headSkinTone[4]={0.8f,0.8f,0.8f,1}; static bool g_haveHeadSkinTone=false;
// all 8 head material tone constants (registers c10..c17, see ConstReg()),
// captured once from whichever head component has them - exported so Blender
// can replicate the exact compositing math instead of needing pre-baked textures
static float      g_headTone[8][4]={{0.8f,0.8f,0.8f,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1}};
static bool        g_haveHeadTone=false;
// CustomColor0/1/2 (registers c18-20) per component - psBody's IntensityMap
// tints a body part's "intensity" mask texture with these before lerping the
// "decal" texture on top (see psBody in g_shader). Captured for EVERY
// component (not just head) so Blender can replicate the exact same
// compositing for Shirt/Trousers/etc instead of only ever showing the flat
// "color" texture with intensity/decal missing.
static float      g_custom[MAXC][3][4]={{{0}}};
static bool       g_haveCustom[MAXC]={false};
static D3DXVECTOR3 g_cmin[MAXC],g_cmax[MAXC];         // per-component bbox
// per-render facial layer selection (index into each animated texture)
static int        g_faceLayer[XAVATAR_ANIMATED_TEXTURE_COUNT]={0,0,0,0,0};

static XAVATAR_METADATA g_meta;
static XAVATAR_ASSETS*  g_assets=NULL;
static int FindHeadComponent(){ if(!g_assets)return -1; for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c) if(g_isHead[c]) return (int)c; return -1; }
static BYTE*            g_gpu=NULL;
static XOVERLAPPED      g_ov;
static DWORD g_vtot=0,g_ttot=0;
static D3DXVECTOR3 g_bmin(1e9f,1e9f,1e9f), g_bmax(-1e9f,-1e9f,-1e9f);
static D3DXVECTOR3 g_hmin(1e9f,1e9f,1e9f), g_hmax(-1e9f,-1e9f,-1e9f);
static bool  g_haveHead=false;
enum State{ ST_LOADING,ST_READY,ST_EXPORTING,ST_DONE,ST_ERROR };
static State g_state=ST_LOADING;
static char  g_root[64]="";
static char  g_out[96]="";                            // export folder (g_root + "AvatarExtract\\")
static bool  g_smoothMode=true;
static bool  g_wantAnims=true;

// ---- system animations ----
static const GUID* g_animIds[]={
  &XAVATAR_ANIMATION_GENERIC_STAND_0,&XAVATAR_ANIMATION_GENERIC_STAND_1,&XAVATAR_ANIMATION_GENERIC_STAND_2,
  &XAVATAR_ANIMATION_GENERIC_STAND_3,&XAVATAR_ANIMATION_GENERIC_STAND_4,&XAVATAR_ANIMATION_GENERIC_STAND_5,
  &XAVATAR_ANIMATION_GENERIC_STAND_6,&XAVATAR_ANIMATION_GENERIC_STAND_7,
  &XAVATAR_ANIMATION_GENERIC_CLAP,&XAVATAR_ANIMATION_GENERIC_WAVE,&XAVATAR_ANIMATION_GENERIC_CELEBRATE,
  &XAVATAR_ANIMATION_FEMALE_IDLE_CHECK_NAILS,&XAVATAR_ANIMATION_FEMALE_IDLE_LOOK_AROUND,&XAVATAR_ANIMATION_FEMALE_IDLE_SHIFT_WEIGHT,
  &XAVATAR_ANIMATION_FEMALE_IDLE_FIXES_SHOE,&XAVATAR_ANIMATION_FEMALE_ANGRY,&XAVATAR_ANIMATION_FEMALE_CONFUSED,
  &XAVATAR_ANIMATION_FEMALE_LAUGH,&XAVATAR_ANIMATION_FEMALE_SAD_CRY,&XAVATAR_ANIMATION_FEMALE_SHOCKED_SURPRISED,
  &XAVATAR_ANIMATION_FEMALE_YAWN,
  &XAVATAR_ANIMATION_MALE_IDLE_LOOK_AROUND,&XAVATAR_ANIMATION_MALE_IDLE_STRETCH,&XAVATAR_ANIMATION_MALE_IDLE_SHIFTS_WEIGHT,
  &XAVATAR_ANIMATION_MALE_IDLE_CHECKS_HAND,&XAVATAR_ANIMATION_MALE_ANGRY,&XAVATAR_ANIMATION_MALE_CONFUSED,
  &XAVATAR_ANIMATION_MALE_LAUGH,&XAVATAR_ANIMATION_MALE_SAD_CRY,&XAVATAR_ANIMATION_MALE_SHOCKED_SURPRISED,
  &XAVATAR_ANIMATION_MALE_YAWN };
static const char* g_animNames[]={
  "generic_stand_0","generic_stand_1","generic_stand_2","generic_stand_3","generic_stand_4","generic_stand_5",
  "generic_stand_6","generic_stand_7","generic_clap","generic_wave","generic_celebrate",
  "female_idle_check_nails","female_idle_look_around","female_idle_shift_weight","female_idle_fixes_shoe",
  "female_angry","female_confused","female_laugh","female_sad_cry","female_shocked_surprised","female_yawn",
  "male_idle_look_around","male_idle_stretch","male_idle_shifts_weight","male_idle_checks_hand",
  "male_angry","male_confused","male_laugh","male_sad_cry","male_shocked_surprised","male_yawn" };
#define NANIM (int)(sizeof(g_animIds)/sizeof(g_animIds[0]))
static LPXAVATARANIMATION g_anim[NANIM]={0};

// ---- facial expression presets (layer indices; clamped to real LayerCount) ----
struct FacePreset{ const char* name; int mouth,eye,brow; };
static const FacePreset g_faces[]={
  {"neutral",0,0,0},{"happy",6,6,4},{"sad",1,1,1},{"angry",2,2,2},{"confused",3,3,3},
  {"laughing",4,4,0},{"shocked",5,5,4},{"yawning",7,7,0},{"sleeping",0,8,0},{"blink",0,13,0},
  {"look_up",0,9,0},{"look_down",0,10,0},{"look_outer",0,11,0},{"look_inner",0,12,0},
  {"phon_o",7,0,0},{"phon_ai",8,0,0},{"phon_ee",9,0,0},{"phon_fv",10,0,0},
  {"phon_w",11,0,0},{"phon_l",12,0,0},{"phon_dth",13,0,0} };
#define NFACE (int)(sizeof(g_faces)/sizeof(g_faces[0]))

// ---- export progress (worker writes, main reads) ----
static HANDLE        g_worker=NULL;
static volatile LONG g_pcur=0,g_ptot=1;
static char          g_pmsg[64]="";
static volatile LONG g_phase=0;   // 0 idle, 1 worker running, 2 bake faces, 3 save textures, 4 done
static volatile LONG g_workerOk=0;
static int           g_faceStep=0, g_texStep=0;

#define OFS_POS 0
#define OFS_WGT 16
#define OFS_BND 20
#define OFS_UV0 28
#define SCR_W 1280.0f
#define SCR_H 720.0f
#define AVX  360.0f

static float H2F(unsigned short h){ unsigned int s=(h>>15)&1,e=(h>>10)&0x1F,m=h&0x3FF,f;
    if(e==0){ if(m==0)f=s<<31; else{e=113;while(!(m&0x400)){m<<=1;--e;}m&=0x3FF;f=(s<<31)|(e<<23)|(m<<13);} }
    else if(e==0x1F)f=(s<<31)|(0xFF<<23)|(m<<13); else f=(s<<31)|((e+112)<<23)|(m<<13);
    float o; memcpy(&o,&f,4); return o; }

// glTF's baseColorFactor (and, in practice, values Blender's importer pokes
// directly into a shader socket) is LINEAR, but our shader constants are
// authored/used as plain display-space color like the game itself uses them
// (same space the baked PNG textures are in, which importers correctly
// sRGB-decode on load). Writing the raw 0..1 constant straight into
// baseColorFactor/Kd skips that decode, so a flat-color material ends up
// visibly lighter/less saturated than a textured one using the exact same
// number. Gamma-correct here so both paths land on the same displayed color.
static float SRGBToLinear(float c){ if(c<=0.0f)return 0.0f; if(c>=1.0f)return 1.0f;
    return (c<=0.04045f)? c/12.92f : powf((c+0.055f)/1.055f,2.4f); }

//----------------------------------------------------------------------------
// Shaders. Sampler slot == texture UvIndex (matches the XDK avatar renderer).
// VS c0-3 = WVP^T, c4-7 = WV^T.
// PS c0=lightDir(view) c1=ambient.x c2=smooth.x c3=exposure.x
//   c10=SkinTone c11=SkinFeature1 c12=SkinFeature2 c13=MouthTone c14=IrisTone
//   c15=EyeBrowTone c16=EyeShadowTone c17=FacialHairTone c18..20=CustomColor1..3
static const char* g_shader =
"float4x4 g_wvp:register(c0); float4x4 g_wv:register(c4);\n"
"struct VI{ float3 p:POSITION; float3 nrm:NORMAL; float4 col:COLOR;\n"
"  float2 t0:TEXCOORD0; float2 t1:TEXCOORD1; float2 t2:TEXCOORD2;\n"
"  float2 t3:TEXCOORD3; float2 t4:TEXCOORD4; float2 t5:TEXCOORD5; };\n"
"struct VO{ float4 p:POSITION; float3 vp:TEXCOORD0; float3 vn:TEXCOORD1;\n"
"  float2 t0:TEXCOORD2; float2 t1:TEXCOORD3; float2 t2:TEXCOORD4;\n"
"  float2 t3:TEXCOORD5; float2 t4:TEXCOORD6; float2 t5:TEXCOORD7; float4 col:COLOR0; };\n"
"VO vs3d(VI i){ VO o; o.p=mul(float4(i.p,1),g_wvp); o.vp=mul(float4(i.p,1),g_wv).xyz;\n"
"  o.vn=mul(float4(i.nrm,0),g_wv).xyz; o.t0=i.t0;o.t1=i.t1;o.t2=i.t2;o.t3=i.t3;o.t4=i.t4;o.t5=i.t5; o.col=i.col; return o; }\n"
"float3 g_L:register(c0); float g_amb:register(c1); float g_sm:register(c2); float g_exp:register(c3);\n"
"float3 shade(float3 base,float3 vp,float3 vn){\n"
"  float3 nf=normalize(cross(ddx(vp),ddy(vp)));\n"
"  float3 n=lerp(nf,normalize(vn),g_sm); n = n.z<0 ? -n : n;\n"
"  float key=saturate(dot(n,g_L));\n"
"  float fill=saturate(dot(n,normalize(float3(-g_L.x,0.15,-g_L.z))))*0.35;\n"
"  float d=key*0.55 + fill + g_amb;\n"
"  return saturate(base*d*g_exp); }\n"
// ---- body ----
"sampler2D bC:register(s0); sampler2D bI:register(s1); sampler2D bD:register(s2);\n"
"float4 CC1:register(c18); float4 CC2:register(c19); float4 CC3:register(c20);\n"
"float4 psBody(VO i):COLOR{\n"
"  float4 r=tex2D(bC,i.t0); float4 im=tex2D(bI,i.t1);\n"
"  float3 mp=im.r*CC1.rgb+im.g*CC2.rgb+im.b*CC3.rgb; r.rgb=lerp(r.rgb,mp,im.a);\n"
"  float4 dc=tex2D(bD,i.t2); r.rgb=lerp(r.rgb,dc.rgb,dc.a);\n"
"  r.rgb*=i.col.rgb; return float4(shade(r.rgb,i.vp,i.vn),1); }\n"
// ---- head ----
"sampler2D h0:register(s0); sampler2D h1:register(s1); sampler2D h2:register(s2);\n"
"sampler2D h3:register(s3); sampler2D h4:register(s4); sampler2D h5:register(s5);\n"
"float4 SkinT:register(c10); float4 SF1:register(c11); float4 SF2:register(c12);\n"
"float4 MouthT:register(c13); float4 IrisT:register(c14); float4 BrowT:register(c15);\n"
"float4 ShadowT:register(c16); float4 FhairT:register(c17);\n"
"float4 psHead(VO i):COLOR{\n"
"  float4 sf=tex2D(h0,i.t0),fh=tex2D(h1,i.t1),eb=tex2D(h2,i.t2),ey=tex2D(h3,i.t3),mo=tex2D(h4,i.t4),es=tex2D(h5,i.t5);\n"
"  sf.rgb=sf.r*SF1.rgb+sf.g*SF2.rgb+sf.b*SkinT.rgb;\n"
"  eb.rgb=eb.r*BrowT.rgb+eb.g+eb.b*SkinT.rgb;\n"
"  ey.rgb=ey.r*IrisT.rgb+ey.g+ey.b*SkinT.rgb;\n"
"  mo.rgb=mo.r*MouthT.rgb+mo.g+mo.b*SkinT.rgb;\n"
"  fh.rgb=fh.r*FhairT.rgb+fh.g+fh.b*SkinT.rgb;\n"
"  es.rgb=es.r*ShadowT.rgb;\n"
"  float3 c=SkinT.rgb;\n"
"  c=lerp(c,sf.rgb,sf.a); c=lerp(c,es.rgb,es.a); c=lerp(c,mo.rgb,mo.a);\n"
"  c=lerp(c,ey.rgb,ey.a); c=lerp(c,fh.rgb,fh.a); c=lerp(c,eb.rgb,eb.a);\n"
"  c*=i.col.rgb; return float4(shade(c,i.vp,i.vn),1); }\n"
// ---- head UV-space bake: rasterize using uv0 as clip position instead of world
// position, so the SAME composited face color lands in the head's own texture
// space and can be exported as a normal baseColorTexture PNG (glTF/Blender have
// no equivalent of our multi-layer tinted-mask shader, so we bake it once here).
"VO vsBake(VI i){ VO o; o.p=float4(i.t0.x*2-1, 1-i.t0.y*2, 0, 1);\n"
"  o.vp=float3(0,0,0); o.vn=float3(0,0,1);\n"
"  o.t0=i.t0;o.t1=i.t1;o.t2=i.t2;o.t3=i.t3;o.t4=i.t4;o.t5=i.t5; o.col=i.col; return o; }\n"
"float4 psHeadAlbedo(VO i):COLOR{\n"
"  float4 sf=tex2D(h0,i.t0),fh=tex2D(h1,i.t1),eb=tex2D(h2,i.t2),ey=tex2D(h3,i.t3),mo=tex2D(h4,i.t4),es=tex2D(h5,i.t5);\n"
"  sf.rgb=sf.r*SF1.rgb+sf.g*SF2.rgb+sf.b*SkinT.rgb;\n"
"  eb.rgb=eb.r*BrowT.rgb+eb.g+eb.b*SkinT.rgb;\n"
"  ey.rgb=ey.r*IrisT.rgb+ey.g+ey.b*SkinT.rgb;\n"
"  mo.rgb=mo.r*MouthT.rgb+mo.g+mo.b*SkinT.rgb;\n"
"  fh.rgb=fh.r*FhairT.rgb+fh.g+fh.b*SkinT.rgb;\n"
"  es.rgb=es.r*ShadowT.rgb;\n"
"  float3 c=SkinT.rgb;\n"
"  c=lerp(c,sf.rgb,sf.a); c=lerp(c,es.rgb,es.a); c=lerp(c,mo.rgb,mo.a);\n"
"  c=lerp(c,ey.rgb,ey.a); c=lerp(c,fh.rgb,fh.a); c=lerp(c,eb.rgb,eb.a);\n"
// NOTE: deliberately no "c*=i.col.rgb" here (unlike psHead) - the vertex color
// carries baked per-vertex AO/shading meant to combine with realtime lighting.
// In this UNLIT albedo bake there is no lighting to compensate for it, so
// applying it just makes the baked face uniformly darker than the body's flat
// skin texture (which has no such AO baked into its own pixels).
"  return float4(c,1); }\n"
// ---- 2D UI ----
"sampler2D fs:register(s0);\n"
"struct V2I{float2 p:POSITION; float2 uv:TEXCOORD0; float4 c:COLOR;}; struct V2O{float4 p:POSITION; float2 uv:TEXCOORD0; float4 c:COLOR0;};\n"
"V2O vs2d(V2I i){ V2O o; o.p=float4(i.p,0,1); o.uv=i.uv; o.c=i.c; return o; }\n"
"float4 ps2d(V2O i):COLOR{ float a=tex2D(fs,i.uv).a; return float4(i.c.rgb, i.c.a*a); }\n"
"float4 psBg(V2O i):COLOR{ return float4(tex2D(fs,i.uv).rgb,1); }\n"
// straight passthrough (RGBA, no tint) - used to resolve a DXT-compressed
// animated-texture layer into a plain uncompressed A8R8G8B8 render target
// before saving, since D3DXSaveTextureToFileA silently drops real alpha
// when saving a compressed source directly (verified: psHead/psHeadAlbedo
// read ey.a/eb.a/etc from these exact same textures correctly via tex2D,
// hardware-decompressed, and that bake is pixel-correct - so the alpha is
// really there, it's just lost specifically by the raw-texture PNG save).
"float4 psCopy(V2O i):COLOR{ return tex2D(fs,i.uv); }\n";

static bool CompileOne(const char* ep,const char* prof,void** blob){
    ID3DXBuffer *b=NULL,*e=NULL;
    if(FAILED(D3DXCompileShader(g_shader,(UINT)strlen(g_shader),0,0,ep,prof,0,&b,&e,0))){ if(e)OutputDebugStringA((char*)e->GetBufferPointer()); return false; }
    *blob=b; return true; }
static bool CompileShaders(){
    ID3DXBuffer* b;
    if(!CompileOne("vs3d","vs_3_0",(void**)&b))return false; g_d->CreateVertexShader((DWORD*)b->GetBufferPointer(),&g_vs3); b->Release();
    if(!CompileOne("psBody","ps_3_0",(void**)&b))return false; g_d->CreatePixelShader((DWORD*)b->GetBufferPointer(),&g_psBody); b->Release();
    if(!CompileOne("psHead","ps_3_0",(void**)&b))return false; g_d->CreatePixelShader((DWORD*)b->GetBufferPointer(),&g_psHead); b->Release();
    if(!CompileOne("vsBake","vs_3_0",(void**)&b))return false; g_d->CreateVertexShader((DWORD*)b->GetBufferPointer(),&g_vsBake); b->Release();
    if(!CompileOne("psHeadAlbedo","ps_3_0",(void**)&b))return false; g_d->CreatePixelShader((DWORD*)b->GetBufferPointer(),&g_psHeadAlbedo); b->Release();
    if(!CompileOne("vs2d","vs_3_0",(void**)&b))return false; g_d->CreateVertexShader((DWORD*)b->GetBufferPointer(),&g_vs2); b->Release();
    if(!CompileOne("ps2d","ps_3_0",(void**)&b))return false; g_d->CreatePixelShader((DWORD*)b->GetBufferPointer(),&g_ps2); b->Release();
    if(!CompileOne("psBg","ps_3_0",(void**)&b))return false; g_d->CreatePixelShader((DWORD*)b->GetBufferPointer(),&g_psBg); b->Release();
    if(!CompileOne("psCopy","ps_3_0",(void**)&b))return false; g_d->CreatePixelShader((DWORD*)b->GetBufferPointer(),&g_psCopy); b->Release();
    static const D3DVERTEXELEMENT9 dh[]={
        {0,0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,12,D3DDECLTYPE_HEND3N,   D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,  0},
        {0,24,D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,   0},
        {0,28,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
        {0,32,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},
        {0,36,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,2},
        {0,40,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,3},
        {0,44,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,4},
        {0,48,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,5}, D3DDECL_END()};
    g_d->CreateVertexDeclaration(dh,&g_declHead);
    static const D3DVERTEXELEMENT9 db[]={
        {0,0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,12,D3DDECLTYPE_HEND3N,   D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,  0},
        {0,24,D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,   0},
        {0,28,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
        {0,32,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},
        {0,36,D3DDECLTYPE_FLOAT16_2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,2}, D3DDECL_END()};
    g_d->CreateVertexDeclaration(db,&g_declBody);
    static const D3DVERTEXELEMENT9 d2[]={
        {0,0, D3DDECLTYPE_FLOAT2,  D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,8, D3DDECLTYPE_FLOAT2,  D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
        {0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,   0}, D3DDECL_END()};
    g_d->CreateVertexDeclaration(d2,&g_decl2);
    return g_vs3&&g_psBody&&g_psHead&&g_psBg&&g_vsBake&&g_psHeadAlbedo&&g_vs2&&g_ps2&&g_psCopy&&g_declHead&&g_declBody&&g_decl2;
}
static void Create1x1(IDirect3DTexture9** t,DWORD argb){
    if(SUCCEEDED(g_d->CreateTexture(1,1,1,0,D3DFMT_LIN_A8R8G8B8,D3DPOOL_DEFAULT,t,NULL))){
        D3DLOCKED_RECT lr; if(SUCCEEDED((*t)->LockRect(0,&lr,NULL,0))){ *(DWORD*)lr.pBits=argb; (*t)->UnlockRect(0); } } }
static void CreateAux(){
    if(SUCCEEDED(g_d->CreateTexture(FA_W,FA_H,1,0,D3DFMT_LIN_A8,D3DPOOL_DEFAULT,&g_font,NULL))){
        D3DLOCKED_RECT lr; if(SUCCEEDED(g_font->LockRect(0,&lr,NULL,0))){
            for(int y=0;y<FA_H;++y) memcpy((BYTE*)lr.pBits+y*lr.Pitch, g_atlas+y*FA_W, FA_W);
            g_font->UnlockRect(0); } }
    Create1x1(&g_white,0xFFFFFFFF);
    Create1x1(&g_zeroA,0x00000000);
    D3DXCreateTextureFromFileInMemory(g_d,BG_JPG,BG_JPG_LEN,&g_bg);
    g_d->CreateTexture(FACE_RT,FACE_RT,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&g_rtTex,NULL);
    g_d->CreateRenderTarget(FACE_RT,FACE_RT,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&g_rtSurf,NULL);
    g_d->CreateDepthStencilSurface(FACE_RT,FACE_RT,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,FALSE,&g_rtDepth,NULL);
}

static int ConstReg(DWORD u){ switch(u){
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_SKIN:           return 10;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_SKIN_FEATURE_1: return 11;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_SKIN_FEATURE_2: return 12;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_MOUTH:          return 13;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_IRIS:           return 14;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_EYEBROW:        return 15;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_EYE_SHADOW:     return 16;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_FACIAL_HAIR:    return 17;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_CUSTOM_0:       return 18;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_CUSTOM_1:       return 19;
    case XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_CUSTOM_2:       return 20;
    default: return -1; } }
// which animated-texture slot a texture usage feeds (or -1)
static int AnimTexOf(DWORD u){ switch(u){
    case XAVATAR_SHADER_PARAM_USAGE_TEXTURE_MOUTH:        return XAVATAR_ANIMATED_TEXTURE_MOUTH;
    case XAVATAR_SHADER_PARAM_USAGE_TEXTURE_EYEBROW_LEFT: return XAVATAR_ANIMATED_TEXTURE_EYEBROW_LEFT;
    case XAVATAR_SHADER_PARAM_USAGE_TEXTURE_EYEBROW_RIGHT:return XAVATAR_ANIMATED_TEXTURE_EYEBROW_RIGHT;
    case XAVATAR_SHADER_PARAM_USAGE_TEXTURE_EYE_LEFT:     return XAVATAR_ANIMATED_TEXTURE_EYE_LEFT;
    case XAVATAR_SHADER_PARAM_USAGE_TEXTURE_EYE_RIGHT:    return XAVATAR_ANIMATED_TEXTURE_EYE_RIGHT;
    default: return -1; } }
static const char* UN(BYTE u){ switch(u){
    case 1:return"color"; case 2:return"intensity"; case 3:return"decal"; case 4:return"reflection";
    case 5:return"skinfeat"; case 6:return"facialhair"; case 7:return"browL"; case 8:return"browR";
    case 9:return"eyeL"; case 10:return"eyeR"; case 11:return"eyeshadow"; case 12:return"mouth"; default:return"tex"; } }

static void BuildPartTextures(){
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ const XAVATAR_MODEL* m=&g_assets->pComponentModels[c];
        g_colorTexIdx[c]=-1; g_isHead[c]=false; g_skin[c][0]=0.8f;g_skin[c][1]=0.8f;g_skin[c][2]=0.8f;g_skin[c][3]=1;
        g_texN[c]= m->TextureCount<MAXT? m->TextureCount:MAXT;
        for(DWORD ti=0;ti<g_texN[c] && m->pTextures;++ti){ const XAVATAR_TEXTURE& tx=m->pTextures[ti];
            DWORD lc= tx.LayerCount? tx.LayerCount:1; if(lc>MAXL)lc=MAXL; g_texLayers[c][ti]=lc; g_texUsage[c][ti]=0;
            for(DWORD li=0;li<lc;++li){ if(!tx.pBaseData||!tx.Width) continue; UINT bs=0,ms=0;
                XGSetTextureHeader(tx.Width,tx.Height,1,0,tx.Format,D3DPOOL_DEFAULT,0,0,0,&g_tex[c][ti][li],&bs,&ms);
                XGOffsetResourceAddress((IDirect3DResource9*)&g_tex[c][ti][li], tx.pBaseData + (size_t)li*tx.BaseSize); } }
        for(DWORD b=0;b<m->BatchCount;++b){ const XAVATAR_SHADER_INSTANCE& si=m->pBatches[b].ShaderInstance;
            if(si.Shader==XAVATAR_SHADER_HEAD_OPAQUE) g_isHead[c]=true;
            for(int pi=0;pi<XAVATAR_SHADER_INSTANCE_MAX_PARAMS;++pi){ const XAVATAR_SHADER_PARAM& p=si.Params[pi];
                if(p.Type==XAVATAR_SHADER_PARAM_TYPE_TEXTURE){ DWORD idx=p.Data.Texture.Index; if(idx<g_texN[c]) g_texUsage[c][idx]=(BYTE)p.Usage;
                    if(p.Usage==XAVATAR_SHADER_PARAM_USAGE_TEXTURE_COLOR) g_colorTexIdx[c]=(int)idx; }
                else if(p.Type==XAVATAR_SHADER_PARAM_TYPE_PIXEL_CONSTANT){
                    if(p.Usage==XAVATAR_SHADER_PARAM_USAGE_PIXEL_CONSTANT_COLOR_SKIN){
                        memcpy(g_skin[c],p.Data.Constant.Value,16);
                        if(g_isHead[c]){ memcpy(g_headSkinTone,p.Data.Constant.Value,16); g_haveHeadSkinTone=true; } }
                    if(g_isHead[c]){ int reg=ConstReg(p.Usage); if(reg>=10&&reg<=17){ memcpy(g_headTone[reg-10],p.Data.Constant.Value,16); g_haveHeadTone=true; } }
                    { int reg=ConstReg(p.Usage); if(reg>=18&&reg<=20){ memcpy(g_custom[c][reg-18],p.Data.Constant.Value,16); g_haveCustom[c]=true; } } } } } }
}

//----------------------------------------------------------------------------
static bool LoadAvatar(){
    if(FAILED(XAvatarInitialize(XAVATAR_COORDINATE_SYSTEM_RIGHT_HANDED,0,4,0,g_d))) return false;
    if(ERROR_SUCCESS!=XAvatarGetMetadataLocalUser(0,&g_meta,NULL)) return false;
    DWORD cpu=0,gpu=0; if(FAILED(XAvatarGetAssetsResultSize(XAVATAR_COMPONENT_MASK_ALL,&cpu,&gpu))) return false;
    g_assets=(XAVATAR_ASSETS*)malloc(cpu); g_gpu=(BYTE*)XPhysicalAlloc(gpu,MAXULONG_PTR,0,PAGE_READWRITE|PAGE_WRITECOMBINE);
    if(!g_assets||!g_gpu) return false;
    ZeroMemory(&g_ov,sizeof(g_ov));
    DWORD r=XAvatarGetAssets(&g_meta,XAVATAR_COMPONENT_MASK_ALL,0,cpu,g_assets,gpu,g_gpu,&g_ov);
    if(r!=ERROR_IO_PENDING&&r!=ERROR_SUCCESS) return false;
    while(!XHasOverlappedIoCompleted(&g_ov)) Sleep(4);
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ const XAVATAR_MODEL* m=&g_assets->pComponentModels[c];
        g_cmin[c]=D3DXVECTOR3(1e9f,1e9f,1e9f); g_cmax[c]=D3DXVECTOR3(-1e9f,-1e9f,-1e9f);
        for(DWORD b=0;b<m->BatchCount;++b){ const XAVATAR_TRIANGLE_BATCH& t=m->pBatches[b]; g_vtot+=t.VertexCount; g_ttot+=t.TriangleCount;
            for(DWORD v=0;v<t.VertexCount;++v){ const float* p=(const float*)(t.pVertices+(size_t)v*t.VertexStride+OFS_POS);
                for(int k=0;k<3;++k){ float* mn=&g_bmin.x+k,*mx=&g_bmax.x+k,*cn=&g_cmin[c].x+k,*cx=&g_cmax[c].x+k;
                    if(p[k]<*mn)*mn=p[k]; if(p[k]>*mx)*mx=p[k]; if(p[k]<*cn)*cn=p[k]; if(p[k]>*cx)*cx=p[k]; } } }
    }
    BuildPartTextures();
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ XAVATAR_COMPONENT_MASK mk=g_assets->pComponentInfo[c].ComponentMask;
        bool facePart = g_isHead[c] || mk==XAVATAR_COMPONENT_MASK_HAIR || mk==XAVATAR_COMPONENT_MASK_HAT
                        || mk==XAVATAR_COMPONENT_MASK_GLASSES || mk==XAVATAR_COMPONENT_MASK_EARRINGS;
        if(facePart && g_cmax[c].x>g_cmin[c].x){ g_haveHead=true;
            for(int k=0;k<3;++k){ float* hn=&g_hmin.x+k,*hx=&g_hmax.x+k;
                if((&g_cmin[c].x)[k]<*hn)*hn=(&g_cmin[c].x)[k]; if((&g_cmax[c].x)[k]>*hx)*hx=(&g_cmax[c].x)[k]; } } }
    return true;
}

static const char* CN(XAVATAR_COMPONENT_MASK m){ switch(m){case XAVATAR_COMPONENT_MASK_HEAD:return"Head";case XAVATAR_COMPONENT_MASK_BODY:return"Body";
    case XAVATAR_COMPONENT_MASK_HAIR:return"Hair";case XAVATAR_COMPONENT_MASK_SHIRT:return"Shirt";case XAVATAR_COMPONENT_MASK_TROUSERS:return"Trousers";
    case XAVATAR_COMPONENT_MASK_SHOES:return"Shoes";case XAVATAR_COMPONENT_MASK_HAT:return"Hat";case XAVATAR_COMPONENT_MASK_GLOVES:return"Gloves";
    case XAVATAR_COMPONENT_MASK_GLASSES:return"Glasses";case XAVATAR_COMPONENT_MASK_WRISTWEAR:return"Wristwear";case XAVATAR_COMPONENT_MASK_EARRINGS:return"Earrings";
    case XAVATAR_COMPONENT_MASK_RING:return"Ring";case XAVATAR_COMPONENT_MASK_CARRYABLE:return"Carryable";default:return"Part";} }

static void PickRoot(){ static const char* r[]={"Usb:\\","Hdd:\\","Game:\\","Usb0:\\","Hdd1:\\","\\Device\\Harddisk0\\Partition1\\"};
    for(int i=0;i<6;++i){ char p[96]; sprintf_s(p,sizeof(p),"%s_avx.tmp",r[i]);
        HANDLE h=CreateFile(p,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
        if(h!=INVALID_HANDLE_VALUE){ CloseHandle(h); DeleteFile(p); strcpy_s(g_root,sizeof(g_root),r[i]); return; } } g_root[0]=0; }

static void MakeOutDirs(){
    sprintf_s(g_out,sizeof(g_out),"%sAvatarExtract\\",g_root);
    CreateDirectoryA(g_out,NULL);
    char p[128];
    sprintf_s(p,sizeof(p),"%sfaces",g_out);    CreateDirectoryA(p,NULL);
    sprintf_s(p,sizeof(p),"%stextures",g_out); CreateDirectoryA(p,NULL);
}

//---------------------------------------------------------------- offscreen face bake
static void SetFaceLayers(int mouth,int eye,int brow){
    g_faceLayer[XAVATAR_ANIMATED_TEXTURE_MOUTH]=mouth;
    g_faceLayer[XAVATAR_ANIMATED_TEXTURE_EYE_LEFT]=eye;  g_faceLayer[XAVATAR_ANIMATED_TEXTURE_EYE_RIGHT]=eye;
    g_faceLayer[XAVATAR_ANIMATED_TEXTURE_EYEBROW_LEFT]=brow; g_faceLayer[XAVATAR_ANIMATED_TEXTURE_EYEBROW_RIGHT]=brow;
}
static void BindHeadDefaults(){
    for(int s=0;s<6;++s) g_d->SetTexture(s,g_zeroA);
    float skin[4]={0.85f,0.68f,0.55f,1},zero[4]={0,0,0,0};
    g_d->SetPixelShaderConstantF(10,skin,1); for(int r=11;r<=17;++r) g_d->SetPixelShaderConstantF(r,zero,1);
}
static void BindBodyDefaults(){
    g_d->SetTexture(0,g_white); g_d->SetTexture(1,g_zeroA); g_d->SetTexture(2,g_zeroA);
    float zero[4]={0,0,0,0}; for(int r=18;r<=20;++r) g_d->SetPixelShaderConstantF(r,zero,1);
}
// bind every texture/constant param a batch's ShaderInstance declares (shared by
// the live preview, the face bake, and the head UV-space texture bake below)
static void BindBatchParams(DWORD c,const XAVATAR_SHADER_INSTANCE& si){
    for(int pi=0;pi<XAVATAR_SHADER_INSTANCE_MAX_PARAMS;++pi){ const XAVATAR_SHADER_PARAM& p=si.Params[pi];
        if(p.Type==XAVATAR_SHADER_PARAM_TYPE_PIXEL_CONSTANT){ int reg=ConstReg(p.Usage); if(reg>=0) g_d->SetPixelShaderConstantF(reg,p.Data.Constant.Value,1); }
        else if(p.Type==XAVATAR_SHADER_PARAM_TYPE_TEXTURE){ DWORD ti=p.Data.Texture.Index, uvi=p.Data.Texture.UvIndex;
            if(uvi==XAVATAR_INVALID_UV_INDEX||uvi>5||ti>=g_texN[c]) continue;
            int at=AnimTexOf(p.Usage), li=0;
            if(at>=0){ li=g_faceLayer[at]; DWORD lc=g_texLayers[c][ti]; if((DWORD)li>=lc) li=lc?lc-1:0; }
            g_d->SetTexture(uvi,(IDirect3DBaseTexture9*)&g_tex[c][ti][li]);
            g_d->SetSamplerState(uvi,D3DSAMP_ADDRESSU,(p.Data.Texture.Flags&XAVATAR_TEXTURE_FLAGS_WRAP_U)?D3DTADDRESS_WRAP:D3DTADDRESS_CLAMP);
            g_d->SetSamplerState(uvi,D3DSAMP_ADDRESSV,(p.Data.Texture.Flags&XAVATAR_TEXTURE_FLAGS_WRAP_V)?D3DTADDRESS_WRAP:D3DTADDRESS_CLAMP); } }
}
static void DrawComponent(DWORD c){
    const XAVATAR_MODEL* m=&g_assets->pComponentModels[c];
    for(DWORD b=0;b<m->BatchCount;++b){ const XAVATAR_TRIANGLE_BATCH& tb=m->pBatches[b]; const XAVATAR_SHADER_INSTANCE& si=tb.ShaderInstance;
        bool head=(si.Shader==XAVATAR_SHADER_HEAD_OPAQUE);
        g_d->SetPixelShader(head?g_psHead:g_psBody);
        g_d->SetVertexDeclaration(head?g_declHead:g_declBody);
        if(head) BindHeadDefaults(); else BindBodyDefaults();
        BindBatchParams(c,si);
        D3DFORMAT ifmt=(tb.IndexStride==4)?D3DFMT_INDEX32:D3DFMT_INDEX16;
        g_d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,tb.VertexCount,tb.TriangleCount,tb.pIndices,ifmt,tb.pVertices,tb.VertexStride);
    }
}
// Bake this head component's composited face color into ITS OWN uv0 texture
// space (unlit) so export can hand Blender/glTF a normal baseColorTexture
// instead of a flat skin-tone fill. Must run on the main thread (D3D), and
// BEFORE the export worker thread writes material JSON/MTL referencing it.
// Reuses the existing 512x512 face-bake render target (g_rtSurf/g_rtTex) rather
// than allocating a new one: a fresh 1024x1024 RT on top of the 1280x720
// backbuffer+depth and this RT+depth almost certainly blew the console's fixed
// ~10MB EDRAM budget (CreateRenderTarget's HRESULT was never checked, so it
// failed silently and every bake bailed out on the null-surface guard).
// Core UV-space head bake: rasterizes this head component's composited face
// color (unlit) into ITS OWN uv0 texture space and saves it to outPath, using
// whatever g_faceLayer[] currently holds (caller sets that up beforehand).
// Reused for: the default/neutral material texture, each of the 21 named
// expression presets, and each expression segment detected within a system
// animation. Must run on the main thread (D3D). Reuses the existing 512x512
// face-bake render target rather than allocating a new one - a fresh
// 1024x1024 RT on top of the 1280x720 backbuffer+depth and this RT+depth
// almost certainly blew the console's fixed ~10MB EDRAM budget.
static bool BakeHeadUV(DWORD c,const char* outPath){
    if(!g_isHead[c] || !g_rtSurf || !g_rtTex) return false;
    const XAVATAR_MODEL* m=&g_assets->pComponentModels[c];
    IDirect3DSurface9 *oldRT=NULL,*oldDS=NULL; g_d->GetRenderTarget(0,&oldRT); g_d->GetDepthStencilSurface(&oldDS);
    g_d->SetRenderTarget(0,g_rtSurf); g_d->SetDepthStencilSurface(NULL);
    D3DVIEWPORT9 vp={0,0,FACE_RT,FACE_RT,0.0f,1.0f}; g_d->SetViewport(&vp);
    // Fill the whole canvas with this head's actual skin tone before drawing the
    // face detail on top - any UV space NOT covered by a triangle (back of the
    // head, ears, etc. all share the same uv0 island layout as the face) then
    // reads as correct matching skin instead of an unrelated hardcoded fallback.
    BYTE cr=(BYTE)(g_skin[c][0]*255.0f+0.5f), cg=(BYTE)(g_skin[c][1]*255.0f+0.5f), cb=(BYTE)(g_skin[c][2]*255.0f+0.5f);
    DWORD clearCol = 0xFF000000 | (cr<<16) | (cg<<8) | cb;
    g_d->Clear(0,NULL,D3DCLEAR_TARGET,clearCol,1.0f,0);
    g_d->SetVertexShader(g_vsBake); g_d->SetPixelShader(g_psHeadAlbedo); g_d->SetVertexDeclaration(g_declHead);
    g_d->SetRenderState(D3DRS_ZENABLE,FALSE); g_d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); g_d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    for(int s=0;s<6;++s){ g_d->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR); g_d->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR); }
    bool any=false;
    for(DWORD b=0;b<m->BatchCount;++b){ const XAVATAR_TRIANGLE_BATCH& tb=m->pBatches[b]; const XAVATAR_SHADER_INSTANCE& si=tb.ShaderInstance;
        if(si.Shader!=XAVATAR_SHADER_HEAD_OPAQUE) continue;
        BindHeadDefaults(); BindBatchParams(c,si);
        D3DFORMAT ifmt=(tb.IndexStride==4)?D3DFMT_INDEX32:D3DFMT_INDEX16;
        g_d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,tb.VertexCount,tb.TriangleCount,tb.pIndices,ifmt,tb.pVertices,tb.VertexStride);
        any=true; }
    g_d->Resolve(D3DRESOLVE_RENDERTARGET0|D3DRESOLVE_ALLFRAGMENTS,NULL,g_rtTex,NULL,0,0,NULL,1.0f,0,NULL);
    g_d->SetRenderTarget(0,oldRT); if(oldRT)oldRT->Release();
    g_d->SetDepthStencilSurface(oldDS); if(oldDS)oldDS->Release();
    if(!any) return false;
    return SUCCEEDED(D3DXSaveTextureToFileA(outPath,D3DXIFF_PNG,(LPDIRECT3DBASETEXTURE9)g_rtTex,NULL));
}
static bool BakeHeadTexture(DWORD c){
    char p[176]; sprintf_s(p,sizeof(p),"%stextures\\%s_bakedface_%02u.png",g_out,CN(g_assets->pComponentInfo[c].ComponentMask),c);
    return BakeHeadUV(c,p);
}
static void BakeHeadTextures(){ for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c) g_headTexOK[c]=BakeHeadTexture(c); }
// Bakes the WHOLE composited head into its own uv0 space once per EYE layer
// (mouth/eyebrow held at their neutral layer 0), using the exact same unlit
// psHeadAlbedo pass as Head_bakedface_NN.png. The eye is the one feature that
// never looked right when Blender rebuilt it from IrisTone through its own
// lit shading (a low-saturation tone reads far duller under directional light
// than eyebrow's near-black does) - handing Blender the console's own already-
// composited pixels for that region, cropped by the raw eye layer's alpha,
// sidesteps the mismatch entirely instead of trying to tune it away.
static void BakeHeadEyeVariants(){
    char dbg[512]; int dn=0;
    dn+=_snprintf(dbg+dn,sizeof(dbg)-dn,"ComponentCount=%u\r\n",g_assets?g_assets->ComponentCount:0);
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){
        dn+=_snprintf(dbg+dn,sizeof(dbg)-dn,"c=%u isHead=%d headTexOK=%d texN=%u\r\n",c,g_isHead[c],g_headTexOK[c],g_texN[c]);
        if(!g_isHead[c] || !g_headTexOK[c]) continue;
        DWORD eyeTi=(DWORD)-1;
        for(DWORD ti=0;ti<g_texN[c];++ti) if(g_texUsage[c][ti]==XAVATAR_SHADER_PARAM_USAGE_TEXTURE_EYE_LEFT||g_texUsage[c][ti]==XAVATAR_SHADER_PARAM_USAGE_TEXTURE_EYE_RIGHT){ eyeTi=ti; break; }
        dn+=_snprintf(dbg+dn,sizeof(dbg)-dn,"  eyeTi=%d\r\n",(int)eyeTi);
        if(eyeTi==(DWORD)-1) continue;
        DWORD lc=g_texLayers[c][eyeTi];
        int ok=0,fail=0;
        for(DWORD L=0;L<lc;++L){
            SetFaceLayers(0,(int)L,0);
            char p[176]; sprintf_s(p,sizeof(p),"%stextures\\%s_bakedface_eye_%02u_%02u.png",g_out,CN(g_assets->pComponentInfo[c].ComponentMask),(unsigned)L,c);
            if(BakeHeadUV(c,p)) ok++; else fail++;
        }
        dn+=_snprintf(dbg+dn,sizeof(dbg)-dn,"  lc=%u ok=%d fail=%d\r\n",lc,ok,fail);
    }
    SetFaceLayers(0,0,0);
    char dp[160]; sprintf_s(dp,sizeof(dp),"%seye_bake_debug.txt",g_out);
    HANDLE h=CreateFile(dp,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    if(h!=INVALID_HANDLE_VALUE){ DWORD w; WriteFile(h,dbg,(DWORD)dn,&w,0); CloseHandle(h); }
}
static bool BakeFace(const FacePreset& fp){
    if(!g_haveHead||!g_rtSurf||!g_rtTex) return false;
    IDirect3DSurface9 *oldRT=NULL,*oldDS=NULL; g_d->GetRenderTarget(0,&oldRT); g_d->GetDepthStencilSurface(&oldDS);
    g_d->SetRenderTarget(0,g_rtSurf); if(g_rtDepth) g_d->SetDepthStencilSurface(g_rtDepth);
    D3DVIEWPORT9 vp={0,0,FACE_RT,FACE_RT,0.0f,1.0f}; g_d->SetViewport(&vp);
    g_d->Clear(0,NULL,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0x00000000,1.0f,0);
    D3DXVECTOR3 hc=(g_hmin+g_hmax)*0.5f, hd=g_hmax-g_hmin, up(0,1,0);
    float hh=hd.y>hd.x?hd.y:hd.x; if(hh<1e-3f)hh=0.3f;
    float fov=0.7f, dist=(0.5f*hh/tanf(fov*0.5f))*1.35f;
    D3DXVECTOR3 eye(hc.x, hc.y+hd.y*0.04f, hc.z+dist);
    D3DXMATRIX world,view,proj,wv,wvp,t;
    D3DXMatrixIdentity(&world); D3DXMatrixLookAtRH(&view,&eye,&hc,&up);
    D3DXMatrixPerspectiveFovRH(&proj,fov,1.0f,dist*0.05f,dist*4.0f);
    D3DXMatrixMultiply(&wv,&world,&view); D3DXMatrixMultiply(&wvp,&wv,&proj);
    g_d->SetVertexShader(g_vs3);
    D3DXMatrixTranspose(&t,&wvp); g_d->SetVertexShaderConstantF(0,(float*)&t,4);
    D3DXMatrixTranspose(&t,&wv);  g_d->SetVertexShaderConstantF(4,(float*)&t,4);
    float L[4]={-0.30f,0.45f,0.84f,0}; float ln=sqrtf(L[0]*L[0]+L[1]*L[1]+L[2]*L[2]); L[0]/=ln;L[1]/=ln;L[2]/=ln;
    float amb[4]={0.62f,0,0,0}, sm[4]={1,0,0,0}, ex[4]={1.12f,0,0,0};
    g_d->SetPixelShaderConstantF(0,L,1); g_d->SetPixelShaderConstantF(1,amb,1); g_d->SetPixelShaderConstantF(2,sm,1); g_d->SetPixelShaderConstantF(3,ex,1);
    g_d->SetRenderState(D3DRS_ZENABLE,TRUE); g_d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
    g_d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); g_d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    for(int s=0;s<6;++s){ g_d->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR); g_d->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR); }
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ XAVATAR_COMPONENT_MASK mk=g_assets->pComponentInfo[c].ComponentMask;
        bool facePart = g_isHead[c] || mk==XAVATAR_COMPONENT_MASK_HAIR || mk==XAVATAR_COMPONENT_MASK_HAT
                        || mk==XAVATAR_COMPONENT_MASK_GLASSES || mk==XAVATAR_COMPONENT_MASK_EARRINGS;
        if(facePart) DrawComponent(c); }
    g_d->Resolve(D3DRESOLVE_RENDERTARGET0|D3DRESOLVE_ALLFRAGMENTS,NULL,g_rtTex,NULL,0,0,NULL,1.0f,0,NULL);
    if(oldRT){ g_d->SetRenderTarget(0,oldRT); oldRT->Release(); }
    if(oldDS){ g_d->SetDepthStencilSurface(oldDS); oldDS->Release(); }
    char p[160]; sprintf_s(p,sizeof(p),"%sfaces\\face_%s.png",g_out,fp.name);
    return SUCCEEDED(D3DXSaveTextureToFileA(p,D3DXIFF_PNG,(LPDIRECT3DBASETEXTURE9)g_rtTex,NULL));
}

// Resolves a (possibly DXT-compressed) source texture into a plain
// uncompressed A8R8G8B8 render target of its own exact size, then saves
// THAT. D3DXSaveTextureToFileA silently drops real per-pixel alpha when
// saving a DXT-compressed source directly to PNG - confirmed via
// texture_debug.txt: these animated-texture layers are GPUTEXTUREFORMAT_
// DXT4_5 (format=438304852, decoded offline), and psHead/psHeadAlbedo
// already prove the alpha channel is real and correctly hardware-decoded
// (ey.a/eb.a/etc, sampled from these exact same D3D texture objects,
// produce the pixel-correct faces/*.png bake) - so routing the very same
// GPU sampling path through a tiny render target before saving carries
// that same correctly-decompressed alpha into the PNG, instead of relying
// on D3DX's own (buggy, for this format) compressed-to-file conversion.
struct CopyVert{ float x,y,u,v; DWORD c; };
static int g_resolveOK=0, g_resolveFailCreateTex=0, g_resolveFailCreateRT=0, g_resolveFailSave=0;
static HRESULT g_resolveFirstFailHr=S_OK; static char g_resolveFirstFailWhat[32]="";
// Resolves a (possibly DXT-compressed) source texture into a plain
// uncompressed A8R8G8B8 render target of its own exact size, then saves
// THAT. D3DXSaveTextureToFileA silently drops real per-pixel alpha when
// saving a DXT-compressed source directly to PNG - confirmed via
// texture_debug.txt/texture_debug2.txt: these animated-texture layers are
// GPUTEXTUREFORMAT_DXT4_5, and both psHead/psHeadAlbedo (the correct
// faces/*.png bake) and this same resolve step (checked via LockRect right
// before the save call) prove the alpha channel really is there and
// correctly hardware-decoded - so routing that same GPU sampling path
// through a tiny render target before saving carries the real alpha into
// the file, instead of relying on D3DX's own (buggy, for this format and
// this PNG writer) conversion. Returns true if outPath was actually
// written (by this path OR the raw-save fallback below).
static bool ResolveAndSaveTexture(IDirect3DTexture9* srcTex,DWORD w,DWORD h,const char* outPath){
    IDirect3DTexture9* tmpTex=NULL; IDirect3DSurface9* tmpSurf=NULL;
    HRESULT hr;
    hr=g_d->CreateTexture(w,h,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&tmpTex,NULL);
    if(FAILED(hr)){ g_resolveFailCreateTex++; if(g_resolveFirstFailHr==S_OK){g_resolveFirstFailHr=hr; strcpy(g_resolveFirstFailWhat,"CreateTexture");} goto fallback; }
    hr=g_d->CreateRenderTarget(w,h,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&tmpSurf,NULL);
    if(FAILED(hr)){ g_resolveFailCreateRT++; if(g_resolveFirstFailHr==S_OK){g_resolveFirstFailHr=hr; strcpy(g_resolveFirstFailWhat,"CreateRenderTarget");} tmpTex->Release(); goto fallback; }
    { IDirect3DSurface9 *oldRT=NULL,*oldDS=NULL; g_d->GetRenderTarget(0,&oldRT); g_d->GetDepthStencilSurface(&oldDS);
    g_d->SetRenderTarget(0,tmpSurf); g_d->SetDepthStencilSurface(NULL);
    D3DVIEWPORT9 vp={0,0,w,h,0.0f,1.0f}; g_d->SetViewport(&vp);
    g_d->Clear(0,NULL,D3DCLEAR_TARGET,0x00000000,1.0f,0);
    CopyVert q[6]={ {-1, 1,0,0,0xFFFFFFFF},{1, 1,1,0,0xFFFFFFFF},{1,-1,1,1,0xFFFFFFFF},
                     {-1, 1,0,0,0xFFFFFFFF},{1,-1,1,1,0xFFFFFFFF},{-1,-1,0,1,0xFFFFFFFF} };
    g_d->SetVertexShader(g_vs2); g_d->SetPixelShader(g_psCopy); g_d->SetVertexDeclaration(g_decl2);
    g_d->SetTexture(0,srcTex);
    // POINT, not LINEAR: an exact 1:1 texel copy, no blending across the
    // shape's hard edges (these are tiny masks, any filtering here would
    // just reintroduce the same edge-blur problems fought on the Blender
    // side all session).
    g_d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT); g_d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    g_d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); g_d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    g_d->SetRenderState(D3DRS_ZENABLE,FALSE); g_d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    g_d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); g_d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    g_d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,q,sizeof(CopyVert));
    g_d->Resolve(D3DRESOLVE_RENDERTARGET0|D3DRESOLVE_ALLFRAGMENTS,NULL,tmpTex,NULL,0,0,NULL,1.0f,0,NULL);
    g_d->SetRenderTarget(0,oldRT); if(oldRT)oldRT->Release();
    g_d->SetDepthStencilSurface(oldDS); if(oldDS)oldDS->Release(); }
    // D3DXIFF_DDS, not _TGA: TGA is a valid enum value but this SDK's save
    // path rejects it outright (D3DERR_INVALIDCALL, confirmed via
    // texture_resolve_summary.txt - 0 successes, every one of 55 textures
    // failed the same way). DDS is the most "native" format for a D3D
    // texture object and is what actually saves successfully. Blender-side
    // tooling parses the (uncompressed A8R8G8B8) DDS header/pixels itself
    // rather than depending on Blender having DDS codec support built in.
    hr=D3DXSaveTextureToFileA(outPath,D3DXIFF_DDS,(LPDIRECT3DBASETEXTURE9)tmpTex,NULL);
    if(FAILED(hr)){ tmpSurf->Release(); tmpTex->Release(); g_resolveFailSave++; if(g_resolveFirstFailHr==S_OK){g_resolveFirstFailHr=hr; strcpy(g_resolveFirstFailWhat,"SaveTexture(DDS)");} goto fallback; }
    // Also save a sibling .png from this SAME resolved (uncompressed, already
    // alpha-correct) texture. The glTF material embedding (see imgJS in the
    // export worker) references "<name>_color_00.png" directly for body-part
    // base color textures (Shirt/Trousers/etc, not routed through Blender's
    // manual per-feature .dds loader like the head layers are) - without this,
    // that .png is simply never rewritten once outPath switched to .dds, so it
    // silently goes stale and Blender keeps showing whatever was exported
    // before the DDS switch.
    { char pngPath[176]; size_t n=strlen(outPath); strcpy(pngPath,outPath);
      if(n>4 && _stricmp(pngPath+n-4,".dds")==0) strcpy(pngPath+n-4,".png");
      D3DXSaveTextureToFileA(pngPath,D3DXIFF_PNG,(LPDIRECT3DBASETEXTURE9)tmpTex,NULL); }
    tmpSurf->Release(); tmpTex->Release();
    g_resolveOK++; return true;
fallback:
    // Never leave a texture completely unwritten - fall back to the old
    // direct (alpha=255-only, but otherwise correct) PNG save so a resolve
    // failure degrades to the previous known-working behavior instead of
    // silently producing no file at all.
    { char pngPath[176]; size_t n=strlen(outPath); strcpy(pngPath,outPath);
      if(n>4 && _stricmp(pngPath+n-4,".dds")==0) strcpy(pngPath+n-4,".png");
      return SUCCEEDED(D3DXSaveTextureToFileA(pngPath,D3DXIFF_PNG,(LPDIRECT3DBASETEXTURE9)srcTex,NULL)); }
}

// one texture layer per call, iterated by g_texStep
static int TexTotal(){ int n=0; for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c) for(DWORD ti=0;ti<g_texN[c];++ti) n+=g_texLayers[c][ti]; return n; }
static void SaveTexStep(int step){ int n=0;
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ const char* nm=CN(g_assets->pComponentInfo[c].ComponentMask);
        const XAVATAR_MODEL* m=&g_assets->pComponentModels[c];
        for(DWORD ti=0;ti<g_texN[c];++ti){ for(DWORD li=0;li<g_texLayers[c][ti];++li){
            if(n++==step){
                DWORD w=0,h=0;
                if(m->pTextures && ti<m->TextureCount){ w=m->pTextures[ti].Width; h=m->pTextures[ti].Height; }
                // .dds, not .png: ResolveAndSaveTexture saves via D3DXIFF_DDS
                // (the PNG writer silently drops alpha on this SDK - see its
                // comment). The w==0/h==0 fallback can't happen in practice
                // (every XAVATAR_TEXTURE has real dimensions) but keeps the
                // old .png/raw-save path rather than assuming DDS works for
                // a texture ResolveAndSaveTexture never got to touch.
                if(w && h){ char p[176]; sprintf_s(p,sizeof(p),"%stextures\\%s_%s_%02u.dds",g_out,nm,UN(g_texUsage[c][ti]),li);
                    ResolveAndSaveTexture(&g_tex[c][ti][li],w,h,p); }
                else{ char p[176]; sprintf_s(p,sizeof(p),"%stextures\\%s_%s_%02u.png",g_out,nm,UN(g_texUsage[c][ti]),li);
                    D3DXSaveTextureToFileA(p,D3DXIFF_PNG,(LPDIRECT3DBASETEXTURE9)&g_tex[c][ti][li],NULL); }
                if(step==TexTotal()-1){
                    char dbgp[176]; sprintf_s(dbgp,sizeof(dbgp),"%stexture_resolve_summary.txt",g_out);
                    FILE* f=fopen(dbgp,"w");
                    if(f){ fprintf(f,"resolved-ok=%d failCreateTex=%d failCreateRT=%d failSave=%d\nfirstFail: %s hr=0x%08X\n",
                        g_resolveOK,g_resolveFailCreateTex,g_resolveFailCreateRT,g_resolveFailSave,
                        g_resolveFirstFailWhat,(unsigned)g_resolveFirstFailHr); fclose(f); }
                }
                return; } } } } }

//---------------------------------------------------------------- little-endian glTF
static BYTE* g_bin; static int g_binN,g_binCap;
static char* g_js;  static int g_jsN, g_jsCap;
static void binEnsure(int add){ if(g_binN+add>g_binCap){ g_binCap=(g_binN+add)*2; g_bin=(BYTE*)realloc(g_bin,g_binCap);} }
static void wU32(DWORD v){ binEnsure(4); BYTE* p=g_bin+g_binN; p[0]=(BYTE)v;p[1]=(BYTE)(v>>8);p[2]=(BYTE)(v>>16);p[3]=(BYTE)(v>>24); g_binN+=4; }
static void wF (float f){ DWORD v; memcpy(&v,&f,4); wU32(v); }
static void wB (BYTE b){ binEnsure(1); g_bin[g_binN++]=b; }
static void binPad4(){ while(g_binN&3) wB(0); }
static void JS(const char* fmt,...){ char t[600]; va_list a; va_start(a,fmt); int n=_vsnprintf(t,sizeof(t),fmt,a); va_end(a); if(n<0)n=(int)strlen(t);
    if(g_jsN+n+1>g_jsCap){ g_jsCap=(g_jsN+n+1)*2; g_js=(char*)realloc(g_js,g_jsCap);} memcpy(g_js+g_jsN,t,n); g_jsN+=n; g_js[g_jsN]=0; }
// Append an ALREADY-BUILT (potentially large) string verbatim, with no format
// buffer in between. NEVER pass a big/variable blob through JS()'s "%s" - its
// scratch buffer is a small fixed stack array, and old-style _vsnprintf does
// NOT null-terminate on truncation, so an oversized %s silently reads/copies
// garbage past the end of that stack buffer and corrupts the output.
static void RawAppend(char** pbuf,int* pn,int* pcap,const char* s){ if(!s)return; int n=(int)strlen(s);
    if(*pn+n+1>*pcap){ *pcap=(*pn+n+1)*2; *pbuf=(char*)realloc(*pbuf,*pcap);} memcpy(*pbuf+*pn,s,n); *pn+=n; (*pbuf)[*pn]=0; }
static void JSRaw(const char* s){ RawAppend(&g_js,&g_jsN,&g_jsCap,s); }

static char* g_bvJS; static int g_bvN,g_bvCap,g_bvCount;
static char* g_acJS; static int g_acN,g_acCap,g_acCount;
static void bvJS(const char* fmt,...){ char t[256]; va_list a; va_start(a,fmt); int n=_vsnprintf(t,sizeof(t),fmt,a); va_end(a); if(n<0)n=(int)strlen(t);
    if(g_bvN+n+1>g_bvCap){ g_bvCap=(g_bvN+n+1)*2; g_bvJS=(char*)realloc(g_bvJS,g_bvCap);} memcpy(g_bvJS+g_bvN,t,n); g_bvN+=n; g_bvJS[g_bvN]=0; }
static void acJS(const char* fmt,...){ char t[256]; va_list a; va_start(a,fmt); int n=_vsnprintf(t,sizeof(t),fmt,a); va_end(a); if(n<0)n=(int)strlen(t);
    if(g_acN+n+1>g_acCap){ g_acCap=(g_acN+n+1)*2; g_acJS=(char*)realloc(g_acJS,g_acCap);} memcpy(g_acJS+g_acN,t,n); g_acN+=n; g_acJS[g_acN]=0; }
static int addAcc(int off,int len,int target,int comp,int count,const char* type){
    bvJS("%s{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d%s}", g_bvCount?",":"", off,len,
         target==34962?",\"target\":34962":target==34963?",\"target\":34963":""); g_bvCount++;
    acJS("%s{\"bufferView\":%d,\"componentType\":%d,\"count\":%d,\"type\":\"%s\"}", g_acCount?",":"",
         g_bvCount-1,comp,count,type); return g_acCount++;
}
static int addAccMM3(int off,int len,int count,const float* mn,const float* mx){
    bvJS("%s{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d,\"target\":34962}", g_bvCount?",":"", off,len); g_bvCount++;
    acJS("%s{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\",\"min\":[%.6f,%.6f,%.6f],\"max\":[%.6f,%.6f,%.6f]}",
         g_acCount?",":"", g_bvCount-1,count, mn[0],mn[1],mn[2],mx[0],mx[1],mx[2]); return g_acCount++; }
static int addAccScalarMM(int off,int len,int count,float mn,float mx){
    bvJS("%s{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d}", g_bvCount?",":"", off,len); g_bvCount++;
    acJS("%s{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"SCALAR\",\"min\":[%.6f],\"max\":[%.6f]}",
         g_acCount?",":"", g_bvCount-1,count,mn,mx); return g_acCount++; }
static void WriteMat(const XMMATRIX& M){ XMFLOAT4X4 f; XMStoreFloat4x4(&f,M); const float* v=&f._11; for(int i=0;i<16;++i) wF(v[i]); }

// growable scratch buffers for glTF animation assembly (per-anim samplers/channels, plus the accumulated animations array)
static char* g_sampBuf=NULL; static int g_sampN=0,g_sampCap=0;
static char* g_chanBuf=NULL; static int g_chanN=0,g_chanCap=0;
static char* g_animAllBuf=NULL; static int g_animAllN=0,g_animAllCap=0;
static void SAMP(const char* fmt,...){ char t[256]; va_list a; va_start(a,fmt); int n=_vsnprintf(t,sizeof(t),fmt,a); va_end(a); if(n<0)n=(int)strlen(t);
    if(g_sampN+n+1>g_sampCap){ g_sampCap=(g_sampN+n+1)*2; g_sampBuf=(char*)realloc(g_sampBuf,g_sampCap);} memcpy(g_sampBuf+g_sampN,t,n); g_sampN+=n; g_sampBuf[g_sampN]=0; }
static void CHAN(const char* fmt,...){ char t[256]; va_list a; va_start(a,fmt); int n=_vsnprintf(t,sizeof(t),fmt,a); va_end(a); if(n<0)n=(int)strlen(t);
    if(g_chanN+n+1>g_chanCap){ g_chanCap=(g_chanN+n+1)*2; g_chanBuf=(char*)realloc(g_chanBuf,g_chanCap);} memcpy(g_chanBuf+g_chanN,t,n); g_chanN+=n; g_chanBuf[g_chanN]=0; }
static void ALLANIM(const char* fmt,...){ char t[700]; va_list a; va_start(a,fmt); int n=_vsnprintf(t,sizeof(t),fmt,a); va_end(a); if(n<0)n=(int)strlen(t);
    if(g_animAllN+n+1>g_animAllCap){ g_animAllCap=(g_animAllN+n+1)*2; g_animAllBuf=(char*)realloc(g_animAllBuf,g_animAllCap);} memcpy(g_animAllBuf+g_animAllN,t,n); g_animAllN+=n; g_animAllBuf[g_animAllN]=0; }

// generic growable text buffer - used to build whole files in RAM so export does
// ONE WriteFile per file instead of one per line (the latter was the actual cost:
// a USB/FATX write syscall per vertex/face line, tens of thousands of them).
struct DynBuf{ char* buf; int n,cap; };
static void DBApp(DynBuf& d,const char* fmt,...){ char t[400]; va_list a; va_start(a,fmt); int n=_vsnprintf(t,sizeof(t),fmt,a); va_end(a); if(n<0)n=(int)strlen(t);
    if(d.n+n+1>d.cap){ d.cap=(d.n+n+1>d.cap*2)?(d.n+n+1)*2:d.cap*2; if(d.cap<4096)d.cap=4096; d.buf=(char*)realloc(d.buf,d.cap); }
    memcpy(d.buf+d.n,t,n); d.n+=n; d.buf[d.n]=0; }
static void DBWrite(DynBuf& d,const char* path){ HANDLE h=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    if(h!=INVALID_HANDLE_VALUE){ DWORD w; WriteFile(h,d.buf,d.n,&w,0); CloseHandle(h); } free(d.buf); d.buf=NULL; d.n=d.cap=0; }

static void ExportOBJ_worker(){
    char path[160];
    DynBuf obj={0,0,0};
    DBApp(obj,"# Xbox 360 Avatar export\nmtllib avatar.mtl\n"); DWORD base=1;
    for(DWORD c=0;c<g_assets->ComponentCount;++c){ const XAVATAR_MODEL* md=&g_assets->pComponentModels[c]; const char* nm=CN(g_assets->pComponentInfo[c].ComponentMask);
        DBApp(obj,"o %s_%u\nusemtl %s_%u\n",nm,c,nm,c);
        for(DWORD b=0;b<md->BatchCount;++b){ const XAVATAR_TRIANGLE_BATCH& t=md->pBatches[b];
            for(DWORD v=0;v<t.VertexCount;++v){ const float* p=(const float*)(t.pVertices+(size_t)v*t.VertexStride+OFS_POS); DBApp(obj,"v %.6f %.6f %.6f\n",p[0],p[1],p[2]); }
            for(DWORD v=0;v<t.VertexCount;++v){ const unsigned short* uv=(const unsigned short*)(t.pVertices+(size_t)v*t.VertexStride+OFS_UV0); DBApp(obj,"vt %.6f %.6f\n",H2F(uv[0]),1.0f-H2F(uv[1])); }
            if(t.IndexStride==4){ const DWORD* x=(const DWORD*)t.pIndices; for(DWORD rr=0;rr<t.TriangleCount;++rr){DWORD a=base+x[rr*3],bb=base+x[rr*3+1],cc=base+x[rr*3+2];DBApp(obj,"f %u/%u %u/%u %u/%u\n",a,a,bb,bb,cc,cc);} }
            else { const WORD* x=(const WORD*)t.pIndices; for(DWORD rr=0;rr<t.TriangleCount;++rr){DWORD a=base+x[rr*3],bb=base+x[rr*3+1],cc=base+x[rr*3+2];DBApp(obj,"f %u/%u %u/%u %u/%u\n",a,a,bb,bb,cc,cc);} }
            base+=t.VertexCount; } }
    sprintf_s(path,sizeof(path),"%savatar.obj",g_out); DBWrite(obj,path);

    DynBuf mtl={0,0,0};
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ const char* nm=CN(g_assets->pComponentInfo[c].ComponentMask);
        if(g_isHead[c] && g_headTexOK[c]) DBApp(mtl,"newmtl %s_%u\nKd 1 1 1\nmap_Kd textures/%s_bakedface_%02u.png\n\n",nm,c,nm,c);
        else if(g_assets->pComponentInfo[c].ComponentMask==XAVATAR_COMPONENT_MASK_BODY && g_haveHeadSkinTone)
            DBApp(mtl,"newmtl %s_%u\nKd %.3f %.3f %.3f\n\n",nm,c,SRGBToLinear(g_headSkinTone[0]),SRGBToLinear(g_headSkinTone[1]),SRGBToLinear(g_headSkinTone[2]));
        else if(g_colorTexIdx[c]>=0) DBApp(mtl,"newmtl %s_%u\nKd 0.8 0.8 0.8\nmap_Kd textures/%s_color_00.png\n\n",nm,c,nm);
        else DBApp(mtl,"newmtl %s_%u\nKd %.3f %.3f %.3f\n\n",nm,c,SRGBToLinear(g_skin[c][0]),SRGBToLinear(g_skin[c][1]),SRGBToLinear(g_skin[c][2])); }
    sprintf_s(path,sizeof(path),"%savatar.mtl",g_out); DBWrite(mtl,path);

    DynBuf rpt={0,0,0};
    DBApp(rpt,"out=%s parts=%u verts=%u tris=%u\n",g_out,g_assets->ComponentCount,g_vtot,g_ttot);
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ const XAVATAR_MODEL* mm=&g_assets->pComponentModels[c];
        DBApp(rpt,"[%u] %-10s batches=%u textures=%u head=%d\n",c,CN(g_assets->pComponentInfo[c].ComponentMask),mm->BatchCount,mm->TextureCount,g_isHead[c]?1:0);
        for(DWORD ti=0;ti<g_texN[c];++ti) DBApp(rpt,"      tex%u %-10s layers=%u\n",ti,UN(g_texUsage[c][ti]),g_texLayers[c][ti]); }
    sprintf_s(path,sizeof(path),"%savatar_report.txt",g_out); DBWrite(rpt,path);
}

static void ExportSkeleton_worker(){ const XAVATAR_SKELETON* sk=g_assets->pSkeleton; if(!sk||!sk->pJoints) return;
    char path[160],ln[224]; sprintf_s(path,sizeof(path),"%savatar_skeleton.txt",g_out);
    HANDLE h=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0); if(h==INVALID_HANDLE_VALUE)return;
    DWORD w; int n=sprintf_s(ln,sizeof(ln),"# joint parent  worldPos(x y z)  worldRot(x y z w)\njoints %u\n",sk->Count); WriteFile(h,ln,n,&w,0);
    for(DWORD i=0;i<sk->Count;++i){ const XAVATAR_SKELETON_JOINT& j=sk->pJoints[i]; XMFLOAT4 wp,wr; XMStoreFloat4(&wp,j.BindPose.Position); XMStoreFloat4(&wr,j.BindPose.Rotation);
        n=sprintf_s(ln,sizeof(ln),"%u %d  %.5f %.5f %.5f  %.5f %.5f %.5f %.5f\n",i,(int)j.Hierarchy.Parent,wp.x,wp.y,wp.z,wr.x,wr.y,wr.z,wr.w); WriteFile(h,ln,n,&w,0); }
    CloseHandle(h); }

// full skinned glTF + system animations
static void ExportGLTF_worker(){
    g_bin=(BYTE*)malloc(1<<21); g_binN=0; g_binCap=1<<21;
    g_js=(char*)malloc(1<<16); g_jsN=0; g_jsCap=1<<16; g_js[0]=0;
    g_bvJS=(char*)malloc(1<<15); g_bvN=0; g_bvCap=1<<15; g_bvCount=0; g_bvJS[0]=0;
    g_acJS=(char*)malloc(1<<15); g_acN=0; g_acCap=1<<15; g_acCount=0; g_acJS[0]=0;

    const XAVATAR_SKELETON* sk=g_assets->pSkeleton; DWORD J=(sk&&sk->pJoints)? sk->Count:0;

    char* meshJS=(char*)malloc(1<<15); int meshN=0,meshCap=1<<15; meshJS[0]=0; int meshCount=0;
    #define MJ(...) { char _t[512]; int _n=_snprintf(_t,sizeof(_t),__VA_ARGS__); if(_n<0)_n=(int)strlen(_t); \
        if(meshN+_n+1>meshCap){ meshCap=(meshN+_n+1)*2; meshJS=(char*)realloc(meshJS,meshCap); } \
        memcpy(meshJS+meshN,_t,_n); meshN+=_n; meshJS[meshN]=0; }

    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ const XAVATAR_MODEL* md=&g_assets->pComponentModels[c];
        MJ("%s{\"name\":\"%s_%u\",\"primitives\":[", meshCount?",":"", CN(g_assets->pComponentInfo[c].ComponentMask), c);
        int primCount=0;
        for(DWORD b=0;b<md->BatchCount;++b){ const XAVATAR_TRIANGLE_BATCH& t=md->pBatches[b];
            DWORD vc=t.VertexCount, tc=t.TriangleCount; if(!vc||!tc) continue;
            float* nrm=(float*)malloc(vc*3*sizeof(float)); memset(nrm,0,vc*3*sizeof(float));
            for(DWORD r2=0;r2<tc;++r2){ DWORD i0,i1,i2;
                if(t.IndexStride==4){ const DWORD* x=(const DWORD*)t.pIndices; i0=x[r2*3];i1=x[r2*3+1];i2=x[r2*3+2]; }
                else { const WORD* x=(const WORD*)t.pIndices; i0=x[r2*3];i1=x[r2*3+1];i2=x[r2*3+2]; }
                const float* p0=(const float*)(t.pVertices+(size_t)i0*t.VertexStride);
                const float* p1=(const float*)(t.pVertices+(size_t)i1*t.VertexStride);
                const float* p2=(const float*)(t.pVertices+(size_t)i2*t.VertexStride);
                float ax=p1[0]-p0[0],ay=p1[1]-p0[1],az=p1[2]-p0[2], bx=p2[0]-p0[0],by=p2[1]-p0[1],bz=p2[2]-p0[2];
                float fx=ay*bz-az*by, fy=az*bx-ax*bz, fz=ax*by-ay*bx;
                nrm[i0*3]+=fx;nrm[i0*3+1]+=fy;nrm[i0*3+2]+=fz; nrm[i1*3]+=fx;nrm[i1*3+1]+=fy;nrm[i1*3+2]+=fz; nrm[i2*3]+=fx;nrm[i2*3+1]+=fy;nrm[i2*3+2]+=fz; }
            binPad4(); int posOff=g_binN; float mn[3]={1e9f,1e9f,1e9f},mx[3]={-1e9f,-1e9f,-1e9f};
            for(DWORD v=0;v<vc;++v){ const float* p=(const float*)(t.pVertices+(size_t)v*t.VertexStride);
                for(int k=0;k<3;++k){ wF(p[k]); if(p[k]<mn[k])mn[k]=p[k]; if(p[k]>mx[k])mx[k]=p[k]; } }
            int accPos=addAccMM3(posOff,g_binN-posOff,vc,mn,mx);
            binPad4(); int nOff=g_binN;
            for(DWORD v=0;v<vc;++v){ float x=nrm[v*3],y=nrm[v*3+1],z=nrm[v*3+2]; float l=sqrtf(x*x+y*y+z*z); if(l<1e-8f)l=1; wF(x/l);wF(y/l);wF(z/l); }
            int accN=addAcc(nOff,g_binN-nOff,34962,5126,vc,"VEC3");
            binPad4(); int uOff=g_binN;
            for(DWORD v=0;v<vc;++v){ const unsigned short* uv=(const unsigned short*)(t.pVertices+(size_t)v*t.VertexStride+OFS_UV0); wF(H2F(uv[0])); wF(H2F(uv[1])); }
            int accU=addAcc(uOff,g_binN-uOff,34962,5126,vc,"VEC2");
            // Head batches carry 6 independent UV sets (uv0..uv5, one per face
            // feature texture - skin/facial-hair/eyebrow/eye/mouth/eyeshadow);
            // body batches carry 3 (uv0=color, uv1=intensity, uv2=decal - see
            // psBody/g_declBody). Export whichever extra sets this batch's own
            // vertex declaration actually has real data for (matching what
            // DrawComponent binds: g_declHead vs g_declBody), so Blender can
            // composite the real per-layer textures live instead of only ever
            // seeing the flat "color" texture with intensity/decal missing.
            int accU2345[5]={-1,-1,-1,-1,-1};
            bool isHeadBatch=(t.ShaderInstance.Shader==XAVATAR_SHADER_HEAD_OPAQUE);
            int extraUV = isHeadBatch?5:2;
            for(int uvi=1;uvi<=extraUV;++uvi){ int ofs=28+uvi*4;
                binPad4(); int o2=g_binN;
                for(DWORD v=0;v<vc;++v){ const unsigned short* uv=(const unsigned short*)(t.pVertices+(size_t)v*t.VertexStride+ofs); wF(H2F(uv[0])); wF(H2F(uv[1])); }
                accU2345[uvi-1]=addAcc(o2,g_binN-o2,34962,5126,vc,"VEC2"); }
            int accJ=-1,accW=-1;
            if(J){ binPad4(); int jOff=g_binN;
                for(DWORD v=0;v<vc;++v){ const BYTE* bd=t.pVertices+(size_t)v*t.VertexStride+OFS_BND; for(int k=0;k<4;++k){ BYTE ji=bd[k]; if(ji>=J)ji=0; wB(ji);} }
                accJ=addAcc(jOff,g_binN-jOff,34962,5121,vc,"VEC4");
                binPad4(); int wOff=g_binN;
                for(DWORD v=0;v<vc;++v){ const BYTE* wd=t.pVertices+(size_t)v*t.VertexStride+OFS_WGT; float s=(float)(wd[0]+wd[1]+wd[2]+wd[3]);
                    if(s<1){ wF(1);wF(0);wF(0);wF(0);} else { wF(wd[0]/s);wF(wd[1]/s);wF(wd[2]/s);wF(wd[3]/s);} }
                accW=addAcc(wOff,g_binN-wOff,34962,5126,vc,"VEC4"); }
            binPad4(); int iOff=g_binN;
            if(t.IndexStride==4){ const DWORD* x=(const DWORD*)t.pIndices; for(DWORD k=0;k<tc*3;++k) wU32(x[k]); }
            else { const WORD* x=(const WORD*)t.pIndices; for(DWORD k=0;k<tc*3;++k) wU32(x[k]); }
            int accI=addAcc(iOff,g_binN-iOff,34963,5125,tc*3,"SCALAR");
            MJ("%s{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,\"TEXCOORD_0\":%d", primCount?",":"", accPos,accN,accU);
            for(int uvi=1;uvi<=extraUV;++uvi) if(accU2345[uvi-1]>=0) MJ(",\"TEXCOORD_%d\":%d",uvi,accU2345[uvi-1]);
            if(J) MJ(",\"JOINTS_0\":%d,\"WEIGHTS_0\":%d",accJ,accW);
            MJ("},\"indices\":%d,\"material\":%u}", accI, c);
            primCount++; free(nrm);
        }
        MJ("]}"); meshCount++;
    }

    // world / local bind matrices
    XMMATRIX* world=NULL; XMMATRIX* localBind=NULL; int ibmAcc=-1;
    if(J){ world=(XMMATRIX*)malloc(sizeof(XMMATRIX)*J); localBind=(XMMATRIX*)malloc(sizeof(XMMATRIX)*J);
        for(DWORD i=0;i<J;++i){ const XAVATAR_SKELETON_JOINT& j=sk->pJoints[i];
            world[i]=XMMatrixMultiply(XMMatrixRotationQuaternion(j.BindPose.Rotation),XMMatrixTranslationFromVector(j.BindPose.Position)); }
        for(DWORD i=0;i<J;++i){ int par=(int)sk->pJoints[i].Hierarchy.Parent;
            localBind[i]= (par<0||par>=(int)J)? world[i] : XMMatrixMultiply(world[i],XMMatrixInverse(NULL,world[par])); }
        binPad4(); int ibmOff=g_binN;
        for(DWORD i=0;i<J;++i){ WriteMat(XMMatrixInverse(NULL,world[i])); }
        ibmAcc=addAcc(ibmOff,g_binN-ibmOff,0,5126,J,"MAT4");
    }

    // ---- animations (each system animation becomes one glTF "animations" entry) ----
    int animCount=0;
    g_sampN=0; if(g_sampBuf)g_sampBuf[0]=0;
    g_chanN=0; if(g_chanBuf)g_chanBuf[0]=0;
    g_animAllN=0; if(g_animAllBuf)g_animAllBuf[0]=0;
    DynBuf faceKf={0,0,0};      // "<anim> <startFrame> <relative texture path>\n" per detected expression change
    int headComp = FindHeadComponent();
    if(g_wantAnims && J){
        XAVATAR_SKELETON_POSE_JOINT* pose=(XAVATAR_SKELETON_POSE_JOINT*)malloc(sizeof(XAVATAR_SKELETON_POSE_JOINT)*XAVATAR_MAX_SKELETON_JOINTS);
        float* T=(float*)malloc(sizeof(float)*256*3);
        float* R=(float*)malloc(sizeof(float)*256*4);
        for(int ai=0;ai<NANIM;++ai){ LPXAVATARANIMATION an=g_anim[ai]; if(!an) continue;
            float len=0; DWORD ajc=0,cjc=0,mdc=0,tcc=0;
            if(FAILED(an->GetAttributes(&len,&ajc,&cjc,&mdc,&tcc)) || len<=0.0f || ajc==0){ InterlockedIncrement(&g_pcur); continue; }
            int frames=(int)(len*24.0f)+1; if(frames<2)frames=2; if(frames>240)frames=240;
            float dt=len/(float)(frames-1);
            XAVATAR_ANIMATION_CURSOR cur; an->InitializeCursor(1.0f,XAVATAR_ANIMATION_PLAYMODE_ONCE,XAVATAR_MOTION_MEASUREMODE_RELATIVE,&cur);
            DWORD jc = ajc<J?ajc:J;
            binPad4(); int tOff=g_binN; for(int f=0;f<frames;++f) wF(f*dt);
            int accT=addAccScalarMM(tOff,g_binN-tOff,frames,0.0f,(frames-1)*dt);
            g_sampN=0; if(g_sampBuf)g_sampBuf[0]=0; int sampCount=0;
            g_chanN=0; if(g_chanBuf)g_chanBuf[0]=0;
            for(DWORD j=0;j<jc;++j){
                XAVATAR_ANIMATION_CURSOR c2=cur;
                float tmn[3]={1e9f,1e9f,1e9f},tmx[3]={-1e9f,-1e9f,-1e9f}; bool tVaries=false;
                for(int f=0;f<frames;++f){
                    an->GetPose(&c2,1.0f,ajc,pose,0,NULL,0,NULL,0,NULL);
                    XMFLOAT4 p,q; XMStoreFloat4(&p,pose[j].Position); XMStoreFloat4(&q,pose[j].Rotation);
                    T[f*3]=p.x;T[f*3+1]=p.y;T[f*3+2]=p.z; R[f*4]=q.x;R[f*4+1]=q.y;R[f*4+2]=q.z;R[f*4+3]=q.w;
                    for(int k=0;k<3;++k){ float val=(&p.x)[k]; if(val<tmn[k])tmn[k]=val; if(val>tmx[k])tmx[k]=val; }
                    an->IncrementCursor(dt,&c2);
                }
                for(int k=0;k<3;++k){ if(fabsf(tmx[k]-tmn[k])>1e-5f) tVaries=true; }
                binPad4(); int rOff=g_binN; for(int f=0;f<frames;++f){ wF(R[f*4]);wF(R[f*4+1]);wF(R[f*4+2]);wF(R[f*4+3]); }
                int accR=addAcc(rOff,g_binN-rOff,34962,5126,frames,"VEC4");
                SAMP("%s{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}", sampCount?",":"", accT,accR);
                int sR=sampCount++;
                CHAN("%s{\"sampler\":%d,\"target\":{\"node\":%u,\"path\":\"rotation\"}}", sR?",":"", sR, j);
                if(tVaries){
                    binPad4(); int toff=g_binN; for(int f=0;f<frames;++f){ wF(T[f*3]);wF(T[f*3+1]);wF(T[f*3+2]); }
                    int accTr=addAccMM3(toff,g_binN-toff,frames,tmn,tmx);
                    SAMP(",{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}", accT,accTr);
                    int sT=sampCount++;
                    CHAN(",{\"sampler\":%d,\"target\":{\"node\":%u,\"path\":\"translation\"}}", sT, j);
                }
            }
            // ---- facial-expression track: GetPose() also reports which texture
            // layer (eyebrow/eye/mouth) is active per XAVATAR_ANIMATED_TEXTURE slot,
            // in lockstep with the joint pose. Record the raw layer indices
            // (mouth, eyebrow, eye) whenever they change - no baking needed at
            // all, since Blender composites the already-exported raw per-layer
            // textures live from these same three numbers. Left/right eyebrow
            // and eye normally move together (one shared texture serves both
            // sides via UV placement, same as the console's own shader); the
            // *_LEFT slot is used as that shared value.
            if(headComp>=0 && tcc>0){
                XAVATAR_ANIMATION_CURSOR c3=cur; bool first=true; BYTE prevM=0,prevB=0,prevE=0;
                DWORD tc2 = tcc<XAVATAR_ANIMATED_TEXTURE_COUNT? tcc:XAVATAR_ANIMATED_TEXTURE_COUNT;
                for(int f=0; f<frames; ++f){
                    DWORD layers[XAVATAR_ANIMATED_TEXTURE_COUNT]={0,0,0,0,0};
                    an->GetPose(&c3,1.0f,0,NULL,0,NULL,0,NULL,tc2,layers);
                    BYTE m=(BYTE)layers[XAVATAR_ANIMATED_TEXTURE_MOUTH], b=(BYTE)layers[XAVATAR_ANIMATED_TEXTURE_EYEBROW_LEFT], e=(BYTE)layers[XAVATAR_ANIMATED_TEXTURE_EYE_LEFT];
                    if(first || m!=prevM || b!=prevB || e!=prevE){
                        first=false; prevM=m; prevB=b; prevE=e;
                        DBApp(faceKf,"%s %d %u %u %u\n", g_animNames[ai], f, (unsigned)m,(unsigned)b,(unsigned)e);
                    }
                    an->IncrementCursor(dt,&c3);
                }
            }
            ALLANIM("%s{\"name\":\"%s\",\"samplers\":[", animCount?",":"", g_animNames[ai]);
            RawAppend(&g_animAllBuf,&g_animAllN,&g_animAllCap, g_sampBuf);
            ALLANIM("],\"channels\":[");
            RawAppend(&g_animAllBuf,&g_animAllN,&g_animAllCap, g_chanBuf);
            ALLANIM("]}");
            animCount++;
            InterlockedIncrement(&g_pcur);
        }
        free(pose); free(T); free(R);
    }
    if(faceKf.n>0){ char fkPath[160]; sprintf_s(fkPath,sizeof(fkPath),"%sface_keyframes.txt",g_out); DBWrite(faceKf,fkPath); }
    else if(faceKf.buf) free(faceKf.buf);

    // ---- write .bin ----
    char path[160]; sprintf_s(path,sizeof(path),"%savatar.bin",g_out);
    HANDLE hb=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    if(hb!=INVALID_HANDLE_VALUE){ DWORD wn; WriteFile(hb,g_bin,g_binN,&wn,0); CloseHandle(hb); }

    // ---- JSON ----
    JS("{\"asset\":{\"version\":\"2.0\",\"generator\":\"Xbox360 AvatarExporter\"},");
    JS("\"buffers\":[{\"uri\":\"avatar.bin\",\"byteLength\":%d}],",g_binN);
    JS("\"bufferViews\":["); JSRaw(g_bvJS); JS("],");
    JS("\"accessors\":["); JSRaw(g_acJS); JS("],");
    JS("\"materials\":[");
    char imgJS[4096]; int imgN=0; imgJS[0]=0; int imgCount=0;
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ const char* nm=CN(g_assets->pComponentInfo[c].ComponentMask);
        JS("%s{\"name\":\"%s_%u\",\"pbrMetallicRoughness\":{",c?",":"",nm,c);
        if(g_isHead[c] && g_headTexOK[c]){ JS("\"baseColorTexture\":{\"index\":%d},\"metallicFactor\":0,\"roughnessFactor\":1}",imgCount);
            int n=_snprintf(imgJS+imgN,sizeof(imgJS)-imgN,"%s{\"uri\":\"textures/%s_bakedface_%02u.png\"}",imgCount?",":"",nm,c); imgN+=n; imgCount++; }
        else if(g_assets->pComponentInfo[c].ComponentMask==XAVATAR_COMPONENT_MASK_BODY && g_haveHeadSkinTone)
            JS("\"baseColorFactor\":[%.4f,%.4f,%.4f,1],\"metallicFactor\":0,\"roughnessFactor\":1}",SRGBToLinear(g_headSkinTone[0]),SRGBToLinear(g_headSkinTone[1]),SRGBToLinear(g_headSkinTone[2]));
        else if(!g_isHead[c] && g_colorTexIdx[c]>=0){ JS("\"baseColorTexture\":{\"index\":%d},\"metallicFactor\":0,\"roughnessFactor\":1}",imgCount);
            int n=_snprintf(imgJS+imgN,sizeof(imgJS)-imgN,"%s{\"uri\":\"textures/%s_color_00.png\"}",imgCount?",":"",nm); imgN+=n; imgCount++; }
        else JS("\"baseColorFactor\":[%.4f,%.4f,%.4f,1],\"metallicFactor\":0,\"roughnessFactor\":1}",SRGBToLinear(g_skin[c][0]),SRGBToLinear(g_skin[c][1]),SRGBToLinear(g_skin[c][2]));
        JS(",\"doubleSided\":true}"); }
    JS("],");
    if(imgCount){ JS("\"images\":["); JSRaw(imgJS); JS("],\"samplers\":[{}],\"textures\":[");
        for(int i=0;i<imgCount;++i) JS("%s{\"source\":%d,\"sampler\":0}",i?",":"",i); JS("],"); }
    JS("\"meshes\":["); JSRaw(meshJS); JS("],");
    JS("\"nodes\":[");
    int firstRoot=-1;
    if(J){ for(DWORD i=0;i<J;++i){ XMFLOAT4X4 f; XMStoreFloat4x4(&f,localBind[i]); const float* v=&f._11;
            JS("%s{\"name\":\"j%u\",\"matrix\":[",i?",":"",i); for(int k=0;k<16;++k) JS("%s%.6f",k?",":"",v[k]); JS("]");
            int firstCh=1; for(DWORD cc=0;cc<J;++cc){ if((int)sk->pJoints[cc].Hierarchy.Parent==(int)i){ if(firstCh){ JS(",\"children\":["); firstCh=0; } else JS(","); JS("%u",cc); } }
            if(!firstCh) JS("]"); JS("}");
            if(((int)sk->pJoints[i].Hierarchy.Parent<0||(int)sk->pJoints[i].Hierarchy.Parent>=(int)J) && firstRoot<0) firstRoot=(int)i; } }
    for(int c=0;c<meshCount;++c) JS("%s{\"mesh\":%d%s}", (J||c)?",":"", c, J?",\"skin\":0":"");
    JS("],");
    if(J){ JS("\"skins\":[{\"inverseBindMatrices\":%d,\"skeleton\":%d,\"joints\":[",ibmAcc,firstRoot<0?0:firstRoot);
        for(DWORD i=0;i<J;++i) JS("%s%u",i?",":"",i); JS("]}],"); }
    if(animCount && g_animAllBuf){ JS("\"animations\":["); JSRaw(g_animAllBuf); JS("],"); }
    JS("\"scenes\":[{\"nodes\":[");
    { int first=1;
      if(J){ for(DWORD i=0;i<J;++i){ int par=(int)sk->pJoints[i].Hierarchy.Parent; if(par<0||par>=(int)J){ JS("%s%u",first?"":",",i); first=0; } } }
      for(int c=0;c<meshCount;++c){ JS("%s%d",first?"":",",J+c); first=0; } }
    JS("]}],\"scene\":0}");

    sprintf_s(path,sizeof(path),"%savatar.gltf",g_out);
    HANDLE hg=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    if(hg!=INVALID_HANDLE_VALUE){ DWORD wn; WriteFile(hg,g_js,g_jsN,&wn,0); CloseHandle(hg); }
    if(world)free(world); if(localBind)free(localBind); free(meshJS);
    free(g_bin); free(g_js); free(g_bvJS); free(g_acJS);
    free(g_sampBuf); g_sampBuf=NULL; g_sampN=g_sampCap=0;
    free(g_chanBuf); g_chanBuf=NULL; g_chanN=g_chanCap=0;
    free(g_animAllBuf); g_animAllBuf=NULL; g_animAllN=g_animAllCap=0;
}

static void WriteReadme_worker(){
    char path[160]; sprintf_s(path,sizeof(path),"%sREADME.txt",g_out);
    HANDLE h=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0); if(h==INVALID_HANDLE_VALUE) return;
    static const char* txt=
"Xbox 360 Avatar export\r\n"
"=======================\r\n"
"avatar.gltf + avatar.bin  - the mesh, skinned + rigged, ready to import into Blender.\r\n"
"                            includes the system animations baked in as glTF animations\r\n"
"                            (if 'Anims: On' was set when you exported).\r\n"
"avatar.obj + avatar.mtl   - the same mesh in plain OBJ form (no rig/animation).\r\n"
"avatar_skeleton.txt       - bind-pose joint dump (parent index + world pos/rot).\r\n"
"avatar_report.txt         - part/texture inventory.\r\n"
"\r\n"
"faces\\face_<name>.png     - the avatar's head, fully rendered with lighting for each\r\n"
"                            named facial expression (this is what a 'face' looks like).\r\n"
"\r\n"
"textures\\*.png            - the RAW per-part texture layers straight from the console.\r\n"
"                            Eyes/eyebrows/mouth ones are NOT plain color photos - they are\r\n"
"                            blend-weight masks (red/green/blue channels each drive a\r\n"
"                            different tint, alpha is coverage), so they look like odd\r\n"
"                            colored blobs when opened directly. That is expected; use the\r\n"
"                            faces\\ folder for the actual composited look.\r\n"
"\r\n"
"head_material.txt          - the 8 tint colors (skin, mouth, iris, eyebrow, ...) the head\r\n"
"                            shader uses. Blender composites the raw layer textures above\r\n"
"                            live with these, matching the console's own shader exactly -\r\n"
"                            no baked combined texture needed for any expression.\r\n"
"face_keyframes.txt         - per system-animation facial track: '<anim> <frame> <mouth>\r\n"
"                            <eyebrow> <eye>' triples of raw layer indices, one line per\r\n"
"                            point the expression actually changes.\r\n"
"face_presets.txt           - the 21 named expressions (happy/sad/angry/...) as the same\r\n"
"                            '<name> <mouth> <eye> <eyebrow>' triples, for quick picking.\r\n";
    DWORD w; WriteFile(h,txt,(DWORD)strlen(txt),&w,0); CloseHandle(h);
}

// The 8 tone constants the head shader tints its texture layers with (skin,
// skin-feature 1/2, mouth, iris, eyebrow, eye-shadow, facial-hair - see
// AvatarCommon.hlsl's FaceShader in the XDK). Blender-side tooling uses these
// to composite the raw per-feature layers live instead of needing this
// console to pre-bake a texture for every possible expression combination.
static const char* g_toneNames[8]={"skin","skinfeature1","skinfeature2","mouth","iris","eyebrow","eyeshadow","facialhair"};
static void WriteHeadMaterial_worker(){
    if(!g_haveHeadTone) return;
    char path[160]; sprintf_s(path,sizeof(path),"%shead_material.txt",g_out);
    HANDLE h=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0); if(h==INVALID_HANDLE_VALUE) return;
    char ln[128]; DWORD w;
    for(int i=0;i<8;++i){ int n=sprintf_s(ln,sizeof(ln),"%s %.6f %.6f %.6f\n",g_toneNames[i],g_headTone[i][0],g_headTone[i][1],g_headTone[i][2]); WriteFile(h,ln,n,&w,0); }
    CloseHandle(h);
}

// Per-component CustomColor0/1/2, one line per component that has them (see
// psBody's IntensityMap in g_shader): "<name>_<index> c0r c0g c0b c1r c1g c1b
// c2r c2g c2b". Lets Blender's body material reproduce the console's
// intensity-tint + decal compositing instead of showing only the flat base
// color texture (decal always missing otherwise).
static void WriteBodyMaterial_worker(){
    char path[160]; sprintf_s(path,sizeof(path),"%sbody_material.txt",g_out);
    HANDLE h=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0); if(h==INVALID_HANDLE_VALUE) return;
    char ln[192]; DWORD w;
    for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c){ if(!g_haveCustom[c]) continue;
        int n=sprintf_s(ln,sizeof(ln),"%s_%u %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
            CN(g_assets->pComponentInfo[c].ComponentMask),c,
            g_custom[c][0][0],g_custom[c][0][1],g_custom[c][0][2],
            g_custom[c][1][0],g_custom[c][1][1],g_custom[c][1][2],
            g_custom[c][2][0],g_custom[c][2][1],g_custom[c][2][2]);
        WriteFile(h,ln,n,&w,0); }
    CloseHandle(h);
}

// The 21 named-expression shortcuts (mouth/eye/eyebrow layer index triples) -
// same table BakeFace() uses for the faces/*.png portraits, exported as plain
// data so Blender can offer them as instant property-set shortcuts with no
// texture baking needed for them either.
static void WriteFacePresets_worker(){
    char path[160]; sprintf_s(path,sizeof(path),"%sface_presets.txt",g_out);
    HANDLE h=CreateFile(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0); if(h==INVALID_HANDLE_VALUE) return;
    char ln[96]; DWORD w;
    for(int i=0;i<NFACE;++i){ const FacePreset& fp=g_faces[i];
        int n=sprintf_s(ln,sizeof(ln),"%s %d %d %d\n",fp.name,fp.mouth,fp.eye,fp.brow); WriteFile(h,ln,n,&w,0); }
    CloseHandle(h);
}

static DWORD WINAPI ExportWorker(LPVOID){
    strcpy_s(g_pmsg,sizeof(g_pmsg),"Writing OBJ / mesh");
    ExportOBJ_worker(); InterlockedIncrement(&g_pcur);
    ExportSkeleton_worker();
    WriteReadme_worker();
    WriteHeadMaterial_worker();
    WriteBodyMaterial_worker();
    WriteFacePresets_worker();
    strcpy_s(g_pmsg,sizeof(g_pmsg),"Building glTF + animations");
    ExportGLTF_worker(); InterlockedIncrement(&g_pcur);
    InterlockedExchange(&g_phase,2);
    return 0;
}

//----------------------------------------------------------------------------
struct UIV{ float x,y,u,v; DWORD c; };
static UIV g_ui[16000]; static int g_uiN=0;
static inline float NX(float p){return p*(2.0f/SCR_W)-1.0f;}
static inline float NY(float p){return 1.0f-p*(2.0f/SCR_H);}
static void QuadUV(float x,float y,float w,float h,float u0,float v0,float u1,float v1,DWORD col){ if(g_uiN+6>16000)return;
    UIV a={NX(x),NY(y),u0,v0,col},b={NX(x+w),NY(y),u1,v0,col},c={NX(x+w),NY(y+h),u1,v1,col},d={NX(x),NY(y+h),u0,v1,col};
    g_ui[g_uiN++]=a;g_ui[g_uiN++]=b;g_ui[g_uiN++]=c;g_ui[g_uiN++]=a;g_ui[g_uiN++]=c;g_ui[g_uiN++]=d; }
static void Rect(float x,float y,float w,float h,DWORD col){ QuadUV(x,y,w,h,FSOLID_U,FSOLID_V,FSOLID_U,FSOLID_V,col); }
static void Text(float x,float y,float s,DWORD col,const char* str){ float pen=x;
    for(;*str;++str){ char ch=*str; if(ch=='\n'){pen=x;y+=FC_H*s;continue;} int ci=ch-32; if(ci<0||ci>=95){pen+=6*s;continue;}
        int cx=ci%FC_COLS, cy=ci/FC_COLS;
        float u0=(cx*FC_W)/(float)FA_W,v0=(cy*FC_H)/(float)FA_H,u1=((cx+1)*FC_W)/(float)FA_W,v1=((cy+1)*FC_H)/(float)FA_H;
        QuadUV(pen,y,FC_W*s,FC_H*s,u0,v0,u1,v1,col); pen+=g_adv[ci]*s; } }

static void DrawBackground(){
    if(!g_bg) return;
    UIV q[6]={ {-1, 1,0,0,0xFFFFFFFF},{1, 1,1,0,0xFFFFFFFF},{1,-1,1,1,0xFFFFFFFF},
               {-1, 1,0,0,0xFFFFFFFF},{1,-1,1,1,0xFFFFFFFF},{-1,-1,0,1,0xFFFFFFFF} };
    g_d->SetVertexShader(g_vs2); g_d->SetPixelShader(g_psBg); g_d->SetVertexDeclaration(g_decl2);
    g_d->SetTexture(0,g_bg);
    g_d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR); g_d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    g_d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); g_d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    g_d->SetRenderState(D3DRS_ZENABLE,FALSE); g_d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    g_d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); g_d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    g_d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,2,q,sizeof(UIV));
}

static void Render(float yaw,float zoom){
    g_d->Clear(0,NULL,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF10151C,1.0f,0);
    D3DVIEWPORT9 full={0,0,(DWORD)SCR_W,(DWORD)SCR_H,0.0f,1.0f}; g_d->SetViewport(&full);
    DrawBackground();
    bool shown=false;
    if(g_assets && g_bmax.x>g_bmin.x){
        D3DVIEWPORT9 vp={(DWORD)AVX,0,(DWORD)(SCR_W-AVX),(DWORD)SCR_H,0.0f,1.0f}; g_d->SetViewport(&vp);
        float aspect=(float)vp.Width/(float)vp.Height;
        D3DXVECTOR3 ctr=(g_bmin+g_bmax)*0.5f, diag=g_bmax-g_bmin, up(0,1,0);
        float radius=0.5f*D3DXVec3Length(&diag), fov=0.75f, dist=(radius/sinf(fov*0.5f))*1.2f/zoom;
        D3DXVECTOR3 eye(ctr.x,ctr.y,ctr.z+dist);
        D3DXMATRIX world,view,proj,wv,wvp,t;
        D3DXMatrixRotationY(&world,yaw); D3DXMatrixLookAtRH(&view,&eye,&ctr,&up);
        D3DXMatrixPerspectiveFovRH(&proj,fov,aspect,dist*0.05f,dist*4.0f);
        D3DXMatrixMultiply(&wv,&world,&view); D3DXMatrixMultiply(&wvp,&wv,&proj);
        g_d->SetVertexShader(g_vs3);
        D3DXMatrixTranspose(&t,&wvp); g_d->SetVertexShaderConstantF(0,(float*)&t,4);
        D3DXMatrixTranspose(&t,&wv);  g_d->SetVertexShaderConstantF(4,(float*)&t,4);
        float L[4]={-0.30f,0.45f,0.84f,0}; float ln=sqrtf(L[0]*L[0]+L[1]*L[1]+L[2]*L[2]); L[0]/=ln;L[1]/=ln;L[2]/=ln;
        float amb[4]={0.62f,0,0,0}, sm[4]={ g_smoothMode?1.0f:0.0f,0,0,0}, ex[4]={1.12f,0,0,0};
        g_d->SetPixelShaderConstantF(0,L,1); g_d->SetPixelShaderConstantF(1,amb,1); g_d->SetPixelShaderConstantF(2,sm,1); g_d->SetPixelShaderConstantF(3,ex,1);
        g_d->SetRenderState(D3DRS_ZENABLE,TRUE); g_d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
        g_d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); g_d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        for(int s=0;s<6;++s){ g_d->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR); g_d->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR); }
        for(DWORD c=0;c<g_assets->ComponentCount && c<MAXC;++c) DrawComponent(c);
        shown=true;
    }

    // ---- UI ----
    g_d->SetViewport(&full); g_uiN=0;
    Rect(0,0,AVX,SCR_H,0x8C0A0E14);
    DWORD sc= g_state==ST_READY?0xFF43C443: g_state==ST_LOADING?0xFFFFB020: g_state==ST_EXPORTING?0xFF2E80FF: g_state==ST_DONE?0xFF57FF57:0xFFE83A3A;
    const char* ss= g_state==ST_READY?"Ready": g_state==ST_LOADING?"Loading": g_state==ST_EXPORTING?"Exporting": g_state==ST_DONE?"Exported!":"Error";
    Text(28,26,1.7f,0xFFFFFFFF,"Avatar Exporter"); Rect(28,64,300,2,0xFF3A4653);
    char b[96];
    sprintf_s(b,sizeof(b),"Parts   %u",g_assets?g_assets->ComponentCount:0); Text(28,80,1.0f,0xFFAEB9C6,b);
    sprintf_s(b,sizeof(b),"Verts   %u",g_vtot); Text(28,104,1.0f,0xFFAEB9C6,b);
    sprintf_s(b,sizeof(b),"Tris    %u",g_ttot); Text(28,128,1.0f,0xFFAEB9C6,b);
    Rect(28,162,12,12,sc); Text(48,158,1.0f,sc,ss);
    Text(28,206,1.0f,0xFF7E8B9A,"Controls"); Rect(28,232,300,1,0xFF2A343F);
    Text(28,242,1.0f,0xFFD5DCE4,"Stick / Dpad   Rotate (analog)");
    Text(28,266,1.0f,0xFFD5DCE4,"LT  RT         Zoom out / in");
    sprintf_s(b,sizeof(b),"Y              Shading: %s",g_smoothMode?"Smooth":"Flat"); Text(28,290,1.0f,0xFFD5DCE4,b);
    sprintf_s(b,sizeof(b),"X              Anims: %s",g_wantAnims?"On":"Off"); Text(28,314,1.0f,0xFFD5DCE4,b);
    Text(28,338,1.0f,0xFFD5DCE4,"A              Export");
    Text(28,362,1.0f, g_state==ST_EXPORTING?0xFF54606C:0xFFD5DCE4,
         g_state==ST_EXPORTING?"Back           (disabled while exporting)":"Back           Quit");
    Text(28,404,1.0f,0xFF7E8B9A,"Output"); Rect(28,430,300,1,0xFF2A343F);
    if(g_root[0]){ sprintf_s(b,sizeof(b),"%sAvatarExtract\\",g_root); Text(28,440,0.82f,0xFFAEB9C6,b);
        Text(28,460,0.82f,0xFF7E8B9A,"obj + gltf/bin  faces/  textures/"); }
    else Text(28,440,0.9f,0xFFE83A3A,"No writable disk");
    if(!shown && g_state!=ST_ERROR) Text(48,186,0.85f,0xFF7E8B9A,"loading...");

    // progress bar during export
    if(g_state==ST_EXPORTING){
        float bx=AVX+60, by=SCR_H*0.5f-30, bw=SCR_W-AVX-120;
        Rect(bx-4,by-4,bw+8,52,0xB2000000);
        const char* pm = g_phase==2?"Baking facial expressions":
                         g_phase==3?"Saving textures / emotions": g_pmsg;
        char line[96];
        long cur=g_pcur, tot=g_ptot>0?g_ptot:1; if(cur>tot)cur=tot;
        sprintf_s(line,sizeof(line),"%s   %ld / %ld",pm,cur,tot);
        Text(bx,by,1.0f,0xFFFFFFFF,line);
        Rect(bx,by+26,bw,14,0xFF243040);
        Rect(bx,by+26,bw*((float)cur/(float)tot),14,0xFF2E80FF);
    }

    if(g_uiN && g_font){
        g_d->SetVertexShader(g_vs2); g_d->SetPixelShader(g_ps2); g_d->SetVertexDeclaration(g_decl2);
        g_d->SetTexture(0,g_font);
        g_d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR); g_d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        g_d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); g_d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        g_d->SetRenderState(D3DRS_ZENABLE,FALSE); g_d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        g_d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        g_d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA); g_d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        g_d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,g_uiN/3,g_ui,sizeof(UIV));
    }
    g_d->Present(0,0,0,0);
}

//----------------------------------------------------------------------------
static void BeginExport(){
    MakeOutDirs();
    BakeHeadTextures();   // main-thread GPU work; must finish before the worker writes materials referencing it
    BakeHeadEyeVariants(); // ditto - one whole-face bake per eye layer, for Blender's eye compositing
    if(g_wantAnims){ strcpy_s(g_pmsg,sizeof(g_pmsg),"Loading system animations");
        for(int i=0;i<NANIM;++i){ if(g_anim[i]){ g_anim[i]->Release(); g_anim[i]=NULL; }
            if(FAILED(XAvatarLoadAnimation(g_animIds[i],0,&g_anim[i]))) g_anim[i]=NULL; } }
    g_faceStep=0; g_texStep=0;
    bool haveSkel = g_assets && g_assets->pSkeleton && g_assets->pSkeleton->pJoints && g_assets->pSkeleton->Count>0;
    g_pcur=0; g_ptot = 2 + ((g_wantAnims&&haveSkel)?NANIM:0) + (g_haveHead?NFACE:0) + TexTotal();
    g_phase=1; g_state=ST_EXPORTING;
    g_worker=CreateThread(NULL,0,ExportWorker,NULL,0,NULL);
}
static void PumpExport(){
    if(g_phase==2){
        // Only the camera-angle faces/*.png portraits get baked here now (nice
        // icons for a picker) - the actual per-expression material no longer
        // needs baking at all: Blender composites live from the raw per-layer
        // textures using head_material.txt + face_keyframes.txt/face_presets.txt.
        if(g_faceStep<NFACE){ const FacePreset& fp=g_faces[g_faceStep];
            if(g_haveHead){ SetFaceLayers(fp.mouth,fp.eye,fp.brow);   // stays set through this tick's Render(),
                BakeFace(fp); }                                      // so the live view visibly cycles expressions
            g_faceStep++; InterlockedIncrement(&g_pcur);
        } else { SetFaceLayers(0,0,0); g_phase=3; }
    } else if(g_phase==3){
        int tt=TexTotal();
        if(g_texStep<tt){ SaveTexStep(g_texStep); g_texStep++; InterlockedIncrement(&g_pcur); }
        else g_phase=4;
    } else if(g_phase==4){
        if(g_worker){ WaitForSingleObject(g_worker,INFINITE); CloseHandle(g_worker); g_worker=NULL; }
        for(int i=0;i<NANIM;++i){ if(g_anim[i]){ g_anim[i]->Release(); g_anim[i]=NULL; } }
        g_state=ST_DONE; g_phase=0;
    }
}

void __cdecl main(){
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp,sizeof(pp));
    pp.BackBufferWidth=1280; pp.BackBufferHeight=720; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.BackBufferCount=1; pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D24S8;
    pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    d3d->CreateDevice(0,D3DDEVTYPE_HAL,NULL,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&g_d);

    if(!CompileShaders()) g_state=ST_ERROR;
    CreateAux();
    g_state=ST_LOADING; Render(0.0f,1.0f);
    g_state=LoadAvatar()? ST_READY:ST_ERROR;
    MountDrive("Hdd:","\\Device\\Harddisk0\\Partition1"); MountDrive("Usb:","\\Device\\Mass0"); PickRoot();

    float yaw=0.0f,zoom=1.0f; bool pa=false,py=false,px=false;
    for(;;){ XINPUT_STATE st; ZeroMemory(&st,sizeof(st));
        if(XInputGetState(0,&st)==ERROR_SUCCESS){ const XINPUT_GAMEPAD& g=st.Gamepad;
            if((g.wButtons&XINPUT_GAMEPAD_BACK) && g_state!=ST_EXPORTING) break;
            float lx=g.sThumbLX/32768.0f;
            float mag=lx<0?-lx:lx;
            if(mag>0.18f){ float s=(mag-0.18f)/0.82f; yaw += (lx>0?1.0f:-1.0f)*s*s*0.13f; }   // analog speed, reversed
            if(g.wButtons&XINPUT_GAMEPAD_DPAD_RIGHT) yaw+=0.045f;
            if(g.wButtons&XINPUT_GAMEPAD_DPAD_LEFT)  yaw-=0.045f;
            zoom+=(g.bRightTrigger/255.0f)*0.03f; zoom-=(g.bLeftTrigger/255.0f)*0.03f;   // LT out, RT in
            if(zoom<0.5f)zoom=0.5f; if(zoom>2.5f)zoom=2.5f;
            bool y=(g.wButtons&XINPUT_GAMEPAD_Y)!=0; if(y&&!py&&g_state!=ST_EXPORTING) g_smoothMode=!g_smoothMode; py=y;
            bool x=(g.wButtons&XINPUT_GAMEPAD_X)!=0; if(x&&!px&&g_state!=ST_EXPORTING) g_wantAnims=!g_wantAnims; px=x;
            bool a=(g.wButtons&XINPUT_GAMEPAD_A)!=0;
            if(a&&!pa&&g_state==ST_READY&&g_root[0]) BeginExport();
            pa=a; }
        if(g_state==ST_EXPORTING) PumpExport();
        Render(yaw,zoom);
    }
    for(int i=0;i<NANIM;++i) if(g_anim[i]) g_anim[i]->Release();
    if(g_gpu)XPhysicalFree(g_gpu); if(g_assets)free(g_assets); XAvatarShutdown();
    if(g_rtTex)g_rtTex->Release(); if(g_rtSurf)g_rtSurf->Release(); if(g_rtDepth)g_rtDepth->Release();
    if(g_bg)g_bg->Release(); if(g_zeroA)g_zeroA->Release(); if(g_white)g_white->Release(); if(g_font)g_font->Release();
    if(g_vs3)g_vs3->Release(); if(g_psBody)g_psBody->Release(); if(g_psHead)g_psHead->Release(); if(g_psBg)g_psBg->Release();
    if(g_vsBake)g_vsBake->Release(); if(g_psHeadAlbedo)g_psHeadAlbedo->Release();
    if(g_vs2)g_vs2->Release(); if(g_ps2)g_ps2->Release(); if(g_psCopy)g_psCopy->Release();
    if(g_declHead)g_declHead->Release(); if(g_declBody)g_declBody->Release(); if(g_decl2)g_decl2->Release();
    if(g_d)g_d->Release(); if(d3d)d3d->Release();
}
