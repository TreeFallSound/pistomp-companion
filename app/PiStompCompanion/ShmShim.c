#include <sys/mman.h>
#include <fcntl.h>

// shm_open is variadic and unavailable to Swift; expose a fixed-signature
// read-only variant. The daemon/HAL own the region — never open it for
// writing from the companion app.
int jb_shm_open_ro(const char *name) {
    return shm_open(name, O_RDONLY, 0);
}
