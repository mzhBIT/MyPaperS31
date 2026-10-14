// Host build: add tools/host_stubs/navigation and main to the include path.
#include "activities/ActivityManager.h"
#include <cassert>
#include <cstdio>
struct Counts { int show=0,deleted=0; };
class Page final : public Activity {
    Counts &counts;
public:
    explicit Page(Counts &c):counts(c) {}
    ~Page() override { ++counts.deleted; }
    void on_create() override {}
    void on_show() override { ++counts.show; }
};
int main() {
    auto &am=ActivityManager::instance();
    Counts system,shelf,files,home,shelf2,files2;
    am.push(new Page(system)); am.push(new Page(shelf)); am.push(new Page(files));
    auto *reading_home=new Page(home);
    am.show_above_root(reading_home);
    assert(am.depth()==2 && am.current()==reading_home);
    assert(shelf.deleted==1 && files.deleted==1);
    assert(shelf.show==1 && files.show==1 && system.show==1);
    am.push(new Page(shelf2)); am.push(new Page(files2));
    assert(am.pop_to(reading_home));
    assert(home.show==2 && am.depth()==2);
    assert(shelf2.show==1 && files2.show==1);
    assert(shelf2.deleted==1 && files2.deleted==1);
    assert(am.pop_to(reading_home) && home.show==2);
    assert(!am.pop_to(nullptr));
    am.pop_to_root();
    assert(home.deleted==1 && system.show==2 && am.depth()==1);
    am.pop(); assert(system.deleted==1);
    std::puts("Activity navigation: PASS (no intermediate on_show)");
}
