#include <std/tst/ctx.h>

using namespace stl;

namespace {
    struct TestContext final: public Ctx {
        TestContext(int argc, char** argv);
    };
}

TestContext::TestContext(int count, char** arguments) {
    argc = count;
    argv = arguments;
}

int main(int argc, char** argv) {
    TestContext(argc, argv).run();
}
