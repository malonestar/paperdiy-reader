#include "fonts/vlw_fonts.h"
#include "hal/hal.h"
#include <map>
#include <string>
#include <cstring>
Hal hal;
static std::map<std::string, lgfx::IFont*> fm;
namespace vlw {
const lgfx::IFont* get(const char* n){ auto& p=fm[n]; if(!p){ int px=atoi(strrchr(n,'_')+1); p=new lgfx::IFont{px}; } return p; }
uint32_t utf8_next(const char* s, size_t len, size_t& i){
  unsigned char c=s[i]; if(c<0x80){i++;return c;} int extra; uint32_t cp;
  if((c&0xE0)==0xC0){cp=c&0x1F;extra=1;} else if((c&0xF0)==0xE0){cp=c&0x0F;extra=2;} else if((c&0xF8)==0xF0){cp=c&7;extra=3;} else {i++;return 0xFFFD;}
  if(i+extra>=len){i=len;return 0xFFFD;}
  for(int k=1;k<=extra;k++){unsigned char cc=s[i+k]; if((cc&0xC0)!=0x80){i++;return 0xFFFD;} cp=(cp<<6)|(cc&0x3F);} i+=extra+1; return cp; }
int width(const lgfx::IFont* f, const char* s, size_t len){ int w=0; size_t i=0; while(i<len){ utf8_next(s,len,i); w+= f->id/2; } return w; }
int height(const lgfx::IFont* f){ return f->id*5/4; }
bool has_glyph(const lgfx::IFont*, uint32_t cp){ return cp < 0x2000 || cp == 0x2026; }
}
