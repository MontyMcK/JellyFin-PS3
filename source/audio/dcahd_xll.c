// dca_xll.c gets its own translation unit: it and dca_core.c both define a
// static get_array(), which upstream never notices because they are separate
// TUs there too.  Everything they share across the boundary is ff_-prefixed
// and external, so splitting costs nothing and modifies no upstream file.
#include <string.h>
#include "mlp/ff/libavutil/internal.h"   /* SUINT */
#include "mlp/ff/libavcodec/avcodec.h"
#include "dcahd/dca_xll.c"
