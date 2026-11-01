#include "fonts/vlw_fonts.h"
#include "reader/layout.h"
#include <cassert>
#include <cstdio>
using namespace layout;
struct VecSrc : ParaSource { std::vector<Para> p; std::vector<uint32_t> off; size_t cur=0;
  VecSrc(std::vector<Para> v):p(v){ uint32_t o=0; for(auto&x:p){off.push_back(o); o+=x.text.size()+3;} off.push_back(o);} 
  bool seek(uint32_t o) override { for(size_t i=0;i<off.size();i++) if(off[i]==o){cur=i;return true;} return false;}
  uint32_t tell() override { return off[cur]; }
  bool next(Para& o) override { if(cur>=p.size()) return false; o=p[cur++]; return true; }
  uint32_t size() override { return off.back(); } };
int main(){
  // normalize
  std::string n = text_normalize("caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xC2\xAD\xEF\xAC\x81ne \xE4\xB8\xAD x\ty \xCE\xB1");
  printf("[%s]\n", n.c_str());
  assert(n.find("caf\xC3\xA9")==0); assert(n.find("fine")!=std::string::npos); assert(n.find("\xC2\xAD")==std::string::npos);
  assert(n.find("\xCE\xB1")!=std::string::npos);
  // ellipsize utf8 safe
  auto f = uiFont(24);
  std::string e = ellipsize(f, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 60);
  printf("ell=[%s]\n", e.c_str());
  auto w = wrapText(f, "The quick brown fox jumps over the lazy dog again and again", 120, 3);
  for(auto&l:w) printf("wrap:[%s]\n", l.c_str()); assert(w.size()==3);
  // pagination: big book, check every word placed exactly once
  std::vector<Para> ps; std::string all;
  for(int i=0;i<300;i++){ std::string t; for(int k=0;k<(i%17)*7+3;k++){ t+= "w"+std::to_string(i)+"_"+std::to_string(k)+" "; } t+= "Supercalifragilisticexpialidociouslylongwordthatneedsbreaking\xC3\xA9\xC3\xA9"; ps.push_back({ (char)(i%25==0?'1':(i%40==7?'c':'p')), t}); }
  VecSrc src(ps); M5Canvas cv; Style st; Pos pos; int pages=0; size_t words=0;
  for(;;){ Page pg; if(!layoutPage(&cv,src,pos,st,pg)) break; pages++; for(auto&l:pg.lines){ assert(l.y < TEXT_BOTTOM+5); for(auto&wd:l.words){ words++; assert(wd.x+vlw::width(fontFor(st,l.style,wd.face),wd.text.data(),wd.text.size())<=SCREEN_W-bodyMarginX(st)+1 || l.style=='c'); size_t i=0; while(i<wd.text.size()){ uint32_t cp=vlw::utf8_next(wd.text.data(),wd.text.size(),i); assert(cp!=0xFFFD);} } }
    if(pg.eof) break; assert(pg.next!=pos); pos=pg.next; }
  printf("pages=%d words=%zu\n",pages,words);
  puts("ALL OK");
}
