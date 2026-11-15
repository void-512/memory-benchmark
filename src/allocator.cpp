// Include exactly once to route all application C++ new/delete allocations,
// including STL containers and Trase objects, through the static mimalloc build.
#include <mimalloc-new-delete.h>
