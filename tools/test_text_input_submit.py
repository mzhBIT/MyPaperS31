"""Native regression for the shared overlay's validated submit lifecycle."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
source=(root/'main/ui/TextInputOverlay.cpp').read_text(encoding='utf-8')
start=source.index('    void submit()');end=source.index('{',start)+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
program=r'''
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
struct Keyboard {std::string value;const std::string& text()const{return value;}};
struct Engine {std::string composition;void set_composition(const char *s){composition=s;}void clear(){composition.clear();}};
struct Editor {
    struct {std::string fixed_suffix;std::function<std::string(const std::string&)> validate;} options;
    bool shown=true,chinese=false;
    std::unique_ptr<Keyboard> keyboard{new Keyboard},nine_keyboard{new Keyboard};
    Engine engine,nine_engine;
    std::string committed,message,composition,candidate;
    unsigned draws=0,commits=0;
    std::function<void(const std::string&)> submit_callback;
    std::function<void()> cancel_callback;
    const std::string& active_composition()const{return composition;}
    void commit_candidate(int slot){assert(slot==0);++commits;committed+=candidate;composition.clear();}
    void draw(){++draws;}
'''+source[start:end]+r'''
};
int main(){
    Editor url;unsigned submitted=0,cancelled=0;
    url.keyboard->value="bad URL";
    url.options.validate=[](const std::string &s){return s.starts_with("https://") ? std::string{} : std::string("invalid address");};
    url.submit_callback=[&](const std::string &s){++submitted;assert(s=="https://example.com/rss");assert(!url.shown && !url.keyboard && !url.nine_keyboard);};
    url.cancel_callback=[&]{++cancelled;};
    url.submit();assert(url.shown && url.keyboard && submitted==0 && url.draws==1 && url.message=="invalid address");
    assert(url.keyboard->value=="bad URL");
    url.keyboard->value="https://example.com/rss";url.submit();
    assert(submitted==1 && cancelled==0 && !url.cancel_callback);
    url.submit();assert(submitted==1);
    Editor name;name.chinese=true;name.composition="xinwen";name.candidate="新闻";
    name.engine.composition="xinwen";name.nine_engine.composition="946936";
    name.options.fixed_suffix=".txt";
    name.options.validate=[](const std::string &s){return s=="新闻.txt" ? std::string{} : std::string("invalid name");};
    name.submit_callback=[&](const std::string &s){++submitted;assert(s=="新闻.txt" && name.committed.empty());};
    name.submit();assert(submitted==2 && name.commits==1 && name.engine.composition.empty() && name.nine_engine.composition.empty());
    puts("Text input submit: invalid value retained, valid result once, candidate/suffix, resources released before callback PASS");
}
'''
(out/'test.cpp').write_text(program,encoding='utf-8')
