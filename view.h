#pragma once

// `im view <file|dir>...`: the image viewer. A directory lists its images,
// a file selects itself among its directory's; the list of thumbnails on
// the left, the image in the middle, its properties on the right, all
// decoded from memory by the sandboxed ImageMagick. A tool of the runtime
// (ui.h): runTool runs it. Returns the process exit code.
int mainView(int argc, char** argv);
