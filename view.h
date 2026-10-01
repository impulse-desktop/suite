#pragma once

namespace stl {
    class ObjPool;
}

struct Ui;

// `im view <file|dir>...`: the image viewer. A directory lists its images,
// a file selects itself among its directory's; the list of thumbnails on
// the left, the image in the middle, its properties on the right, all
// decoded from memory by the sandboxed ImageMagick. A tool of the runtime
// (ui.h): runTool runs it. Returns the process exit code.
int mainView(stl::ObjPool& pool, Ui& ui, int argc, char** argv);
