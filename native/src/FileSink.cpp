// FileSink.cpp — no symbols emitted.
//
// `FileSink.h` is a header-only template library (the streaming loop is
// inlined per translation unit so it can specialize on each transport's
// `ReadFn`). We still keep this .cpp on disk so the CMake target list
// stays explicit about the translation units in `wfd_core`, and so future
// non-template helpers have somewhere to live.
#include "FileSink.h"
