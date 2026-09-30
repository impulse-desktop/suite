#pragma once

// `im view <file|dir>...`: the image viewer. A directory lists its images,
// a file selects itself among its directory's; the gallery is a scrollable
// panel on the left, the image on the right, decoded on a thread pool by
// the sandboxed ImageMagick. Returns the process exit code.
int mainView(int argc, char** argv);
