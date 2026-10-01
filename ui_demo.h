#pragma once

namespace stl {
    class ObjPool;
}

// `im ui`: the tool runtime's skeleton, one window with one button that
// ends it. Returns the process exit code.
int mainUiDemo(stl::ObjPool& pool, int argc, char** argv);
