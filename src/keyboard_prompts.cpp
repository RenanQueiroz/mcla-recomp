// Binding-aware keyboard prompts. The control selector is the controller
// sprite's frame, not its translated action label. Native Flash rendering
// preserves the UI's transforms and animation. No game artwork is supplied.
#include "generated/default/mcla_init.h"
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <unordered_map>
#include <string>
#include <unordered_set>

REXCVAR_DEFINE_BOOL(mcla_keyboard_prompts, true, "MCLA", "Show current keyboard bindings in controller prompt sprites when mnk_mode is enabled");
REXCVAR_DEFINE_BOOL(mcla_key_prompt_probe, false, "MCLA", "Development: log resolved control IDs and key labels");
REXCVAR_DEFINE_INT32(mcla_key_prompt_gallery_frame, -1, "MCLA", "Development: controller icon frame gallery; -1 disables")
    .range(-1, 117);

namespace {
bool Range(uint32_t p, uint32_t n=256) { return p>=0x10000 && p<=0xC0000000u-n; }
uint32_t U32(const uint8_t* b, uint32_t p) {
  return (uint32_t(b[p])<<24)|(uint32_t(b[p+1])<<16)|(uint32_t(b[p+2])<<8)|b[p+3];
}
uint16_t U16(const uint8_t* b, uint32_t p) { return uint16_t((uint32_t(b[p])<<8)|b[p+1]); }
std::string String(const uint8_t* b, uint32_t p, size_t cap=512) {
  if(!Range(p,uint32_t(cap))) return {};
  std::string s;
  for(size_t i=0;i<cap;i++) { if(!b[p+i]) return s; s+=char(b[p+i]); }
  return {};
}
thread_local uint32_t g_node=0;
thread_local std::unordered_set<uint32_t> g_controller_defs;
struct Replacement {
  uint32_t node=0;
  bool composed=false;
  bool controller=false;
  std::array<uint8_t,96> state{};
};
thread_local Replacement* g_replacement=nullptr;
thread_local uint32_t g_font=0;
thread_local uint32_t g_font_height=0;
thread_local uint32_t g_font_file=0;
thread_local uint32_t g_font_table=0;
void W32(uint8_t* b,uint32_t p,uint32_t v) { b[p]=v>>24; b[p+1]=v>>16;b[p+2]=v>>8;b[p+3]=v; }
float F32(const uint8_t* b,uint32_t p) { return std::bit_cast<float>(U32(b,p)); }
void WF32(uint8_t* b,uint32_t p,float v) { W32(b,p,std::bit_cast<uint32_t>(v)); }

bool HasStrike(const uint8_t* b,uint32_t font,uint32_t height) {
  if(!Range(font,1024)) return false;
  const uint32_t count=U16(b,font+152);
  if(count>128) return false;
  for(uint32_t i=0;i<count;i++) {
    const uint32_t strike=U32(b,font+164+4*i);
    if(Range(strike) && U16(b,strike+8)==height) return true;
  }
  return false;
}

// The file's own font dictionary is authoritative even when a screen draws
// its other text as prebuilt artwork. Select a loaded strike, never a guessed
// font size: pure bitmap fonts have no vector fallback for a missing strike.
bool EnsureFont(const uint8_t* b) {
  const uint32_t file=U32(b,0x828FF990);
  if(!Range(file)) return false;
  const uint32_t table=U32(b,file+24), count=U16(b,file+50);
  if(!Range(table,4*count)) return false;
  if(g_font_file==file && g_font_table==table && g_font && b[g_font+8]==5 && HasStrike(b,g_font,g_font_height)) return true;
  g_font=0;g_font_height=0;g_font_file=file;g_font_table=table;
  int best=1000000;
  for(uint32_t i=0;i<count;i++) {
    const uint32_t font=U32(b,table+4*i);
    if(!Range(font,1024) || b[font+8]!=5) continue;
    const uint32_t strikes=U16(b,font+152);
    if(strikes>128 || !Range(U32(b,font+20),1024)) continue;
    const bool gothic=String(b,font+196,64)=="Dev Gothic";
    for(uint32_t j=0;j<strikes;j++) {
      const uint32_t strike=U32(b,font+164+4*j);
      if(!Range(strike)) continue;
      const int height=U16(b,strike+8);
      if(height<80 || height>4000) continue;
      const int score=(gothic?0:10000)+std::abs(height-600);
      if(score<best) { best=score;g_font=font;g_font_height=height; }
    }
  }
  if(g_font && REXCVAR_GET(mcla_key_prompt_probe))
    REXLOG_INFO("keyboard prompt font: file={:08X} font={:08X} name={} loaded height={}",file,g_font,String(b,g_font+196,64),g_font_height);
  return g_font!=0;
}

std::string Key(std::string name) {
  auto value=rex::cvar::GetFlagByName("keybind_"+name);
  value=value.substr(0,value.find(','));
  const auto first=value.find_first_not_of(" ");
  if(first==std::string::npos) return "--";
  value=value.substr(first,value.find_last_not_of(" ")-first+1);
  if(value.empty()) return "--";
  static const std::unordered_map<std::string,std::string> short_names={
    {"Space","SPC"},{"Backspace","BKSP"},{"Return","ENT"},{"Escape","ESC"},
    {"Left Shift","LSH"},{"Right Shift","RSH"},{"Left Ctrl","LCTL"},{"Right Ctrl","RCTL"},
    {"Left Alt","LALT"},{"Right Alt","RALT"},{"Up","UP"},{"Down","DN"},
    {"Left","LEFT"},{"Right","RGHT"},{"Tab","TAB"},{"Delete","DEL"}};
  if(auto i=short_names.find(value);i!=short_names.end()) return i->second;
  // Key names come from the same cvars as the keyboard input backend. Never
  // render arbitrary control bytes/markup as part of a native game string.
  for(unsigned char c:value) if(c<32 || c>=127) return "?";
  return value;
}
std::string Label(int frame) {
  static const char* keys[]={"a","b","x","y","left_shoulder","right_shoulder",
    "left_trigger","right_trigger","start","back"};
  if(frame>=49&&frame<98) frame-=49;
  if(frame>=0&&frame<10) return Key(keys[frame]);
  switch(frame) {
    case 12:return Key("lstick_left")+"/"+Key("lstick_right");
    case 13:return Key("lstick_up")+"/"+Key("lstick_down")+"/"+Key("lstick_left")+"/"+Key("lstick_right");
    case 14:case 15:case 16:case 17:
      if(rex::cvar::GetFlagByName("mnk_mouse")=="true") return "MOUSE";
      if(frame==15) return Key("rstick_up")+"/"+Key("rstick_down");
      if(frame==16) return Key("rstick_left")+"/"+Key("rstick_right");
      return Key("rstick_up")+"/"+Key("rstick_down")+"/"+Key("rstick_left")+"/"+Key("rstick_right");
    case 18:case 21:return Key("dpad_up")+"/"+Key("dpad_down")+"/"+Key("dpad_left")+"/"+Key("dpad_right");
    case 19:return Key("dpad_up")+"/"+Key("dpad_down");
    case 20:return Key("dpad_left")+"/"+Key("dpad_right");
    case 22:return Key("left_trigger")+"/"+Key("right_trigger");
    case 23:return Key("left_shoulder")+"/"+Key("right_shoulder");
    case 27:return Key("rstick_press"); case 28:return Key("lstick_press");
    case 29:return Key("lstick_right"); case 30:return Key("lstick_left"); case 31:return Key("lstick_up");
    case 32:return Key("lstick_down");
    case 33:case 34:case 35:case 36:
      if(rex::cvar::GetFlagByName("mnk_mouse")=="true") return "MOUSE";
      return Key(frame==33?"rstick_right":frame==34?"rstick_left":frame==35?"rstick_up":"rstick_down"); default:return {};
  }
}
float LabelWidth(const uint8_t* b,const std::string& label) {
  const uint32_t advances=U32(b,g_font+20);
  if(!Range(advances,1024) || label.size()>96) return 0;
  float width=0;
  for(unsigned char c:"["+label+"]") width+=F32(b,advances+4*b[g_font+24+c]);
  return width>0 && width<100000?width:0;
}
void DrawLine(PPCContext& entry,uint8_t* b,const Replacement& rep,const std::string& label,float row_y,float max_size) {
  if(!rep.composed || !rep.state[59] || label.size()>96) return;
  PPCContext draw=entry;
  draw.r1.u32-=0x800;
  uint32_t def=draw.r1.u32+0x100, state=def+0x80, text=state+0x80;
  std::memset(b+def,0,64); std::memcpy(b+state,rep.state.data(),rep.state.size());
  const std::string text_label="["+label+"]";
  std::memcpy(b+text,text_label.c_str(),text_label.size()+1);
  const float width=LabelWidth(b,label);
  if(!width) return;
  // The font's advance table and line breaker use em units. A loaded strike
  // selects glyph artwork; scale its matrix instead of requesting absent sizes.
  const float size=std::min(max_size,(rep.controller?48.0f:42.0f)*1024.0f/width);
  const float scale=size*20.0f/float(g_font_height);
  W32(b,def+12,text); WF32(b,def+32,width+4.0f);
  const float a=F32(b,state+24),bb=F32(b,state+28),c=F32(b,state+32),d=F32(b,state+36);
  WF32(b,state+24,a*scale);WF32(b,state+28,bb*scale);WF32(b,state+32,c*scale);WF32(b,state+36,d*scale);
  const float half_width=width*size/1024.0f*0.5f-(rep.controller?5.0f:0.0f);
  const float y=row_y-size*0.5f;
  WF32(b,state+40,F32(b,state+40)-half_width*a+y*c);
  WF32(b,state+44,F32(b,state+44)-half_width*bb+y*d);
  draw.r3.u64=def;draw.r4.u64=text;draw.r5.u64=g_font;
  draw.r6.u64=g_font_height;draw.r7.u64=0xFFFFFFFF;draw.r8.u64=0;draw.r9.u64=state;
  sub_82604548(draw,b);
}

void DrawKey(PPCContext& entry,uint8_t* b,const Replacement& rep,const std::string& label) {
  // Four-direction hints retain all four live bindings without shrinking a
  // whole line to an unreadable size. Two-direction hints stay on one line.
  const auto first=label.find('/');
  const auto second=first==std::string::npos?first:label.find('/',first+1);
  const auto third=second==std::string::npos?second:label.find('/',second+1);
  if(third!=std::string::npos) {
    DrawLine(entry,b,rep,label.substr(0,second),-7.0f,18.0f);
    DrawLine(entry,b,rep,label.substr(second+1),7.0f,18.0f);
  } else {
    DrawLine(entry,b,rep,label,0.0f,23.0f);
  }
}


bool Controller(const uint8_t* b, uint32_t node) {
  if(!Range(node)) return false;
  const uint32_t def=U32(b,node+4);
  if(!Range(def) || b[def+8]!=2) return false;
  const auto count=U16(b,def+16);
  if(count<98 || count>128) return false;
  if(g_controller_defs.contains(def)) return true;
  if(String(b,U32(b,node+196),32)!="icon") return false;
  g_controller_defs.insert(def);
  return true;
}

}  // namespace

REX_EXTERN(__imp__sub_825FB348);
REX_EXTERN(__imp__sub_825FB0D8);

extern "C" REX_FUNC(sub_825FB348) {
  const uint32_t previous=g_node;
  g_node=ctx.r3.u32;
  const bool controller=Controller(base,g_node);
  int control=controller?U16(base,g_node+164):-1;
  // These are the native menu-tab controls, separate from the shared icon
  // timeline. Match semantic instance names and class shape, never prose.
  if(control<0 && Range(g_node)) {
    const uint32_t def=U32(base,g_node+4);
    if(Range(def) && base[def+8]==2 && U16(base,def+16)<=3) {
      const auto name=String(base,U32(base,g_node+196),32);
      if(name=="btn_lb") control=4;
      else if(name=="btn_rb") control=5;
    }
  }
  std::string label;
  if(control>=0 && REXCVAR_GET(mcla_keyboard_prompts) &&
     rex::cvar::GetFlagByName("mnk_mode")=="true" && EnsureFont(base))
    label=Label(control);
  if(!label.empty() && !LabelWidth(base,label)) label.clear();
  if(!label.empty() && REXCVAR_GET(mcla_key_prompt_probe)) {
    static thread_local std::unordered_set<std::string> seen;
    const auto message=std::to_string(control)+":"+label;
    if(seen.size()<512 && seen.insert(message).second)
      REXLOG_INFO("keyboard prompt: control={} binding={} native sprite={:08X}",control,label,g_node);
  }
  Replacement rep; rep.node=g_node;rep.controller=controller;
  auto previous_rep=g_replacement;
  PPCContext entry=ctx;
  if(!label.empty()) g_replacement=&rep;
  __imp__sub_825FB348(ctx,base);
  if(!label.empty()) {
    if(rep.composed) { base[rep.node+67]=rep.state[59]; DrawKey(entry,base,rep,label); }
    g_replacement=previous_rep;
  }
  g_node=previous;
}

extern "C" REX_FUNC(sub_825FB0D8) {
  const int requested=REXCVAR_GET(mcla_key_prompt_gallery_frame);
  if(requested>=0 && Controller(base,ctx.r3.u32)) {
    const uint32_t def=U32(base,ctx.r3.u32+4);
    if(requested<U16(base,def+16)) {
      // Update owns the current movie's pools. Never seek a timeline from Draw.
      PPCContext seek=ctx;
      seek.r1.u32-=0x100;
      seek.r4.u64=uint32_t(requested);
      seek.r5.u64=0;
      sub_825F90C0(seek,base);
    }
  }
  __imp__sub_825FB0D8(ctx,base);
}

// Called after the native player composes world matrix/colour, before it draws
// children. Hiding only this draw leaves timeline state and action labels intact.
void MclaKeyboardPromptNode(PPCRegister& r30) {
  if(g_replacement && g_replacement->node==r30.u32) {
    uint8_t* b=rex::Runtime::instance()->virtual_membase();
    std::memcpy(g_replacement->state.data(),b+r30.u32+8,g_replacement->state.size());
    g_replacement->composed=true;
    b[r30.u32+67]=0;
  }
}
