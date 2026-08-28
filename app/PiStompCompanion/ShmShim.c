#include <sys/mman.h>
#include <fcntl.h>

// shm_open is variadic and unavailable to Swift; expose a fixed-signature
// read-only variant. The daemon/HAL own the region — never open it for
// writing from the companion app.
int jb_shm_open_ro(const char *name) {
    return shm_open(name, O_RDONLY, 0);
}

// Companion-side write entry: exactly one caller (ShmWriter.pokeResync),
// exactly one field (JB_OFF_RESYNC_REQUEST). See the comment on ShmWriter
// for why this is a separate fd and not a mixed-mode ShmReader.
int jb_shm_open_rw(const char *name) {
    return shm_open(name, O_RDWR, 0);
}
