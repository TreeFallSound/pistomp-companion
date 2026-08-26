// Cross-process atomic stress test for the JackBridge shm IPC contract.
//
// Phase 2.2 verification: assert that std::atomic<uint64_t> stored in a POSIX
// shm region behaves correctly across processes on Apple Silicon. Two tests:
//
//  (1) No torn 64-bit reads. Producer writes values where the high 32 bits
//      equal the low 32 bits ((i<<32)|i). A torn read would produce
//      high != low. Consumer asserts equality and monotonicity.
//
//  (2) Acquire-release pairing. Producer writes a non-atomic data buffer then
//      publishes a seq counter with release. Consumer reads seq with acquire
//      then reads the buffer; asserts the buffer is fully consistent with the
//      seq it just read (i.e. the buffer publish happens-before the seq
//      publish from the consumer's perspective).
//
// Builds standalone:
//   clang++ -std=c++17 -O2 -arch arm64 -arch x86_64 -Wall -Wextra \
//       -o tools/stress_atomic tools/stress_atomic.cpp
//
// Run with no args. Exit 0 on success.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char* kShmPath = "/JackBridge-stress";
constexpr uint64_t    kIterations = 5'000'000;
constexpr uint32_t    kBufferLen  = 64;

struct Region {
    // Test 1: torn-read detection.
    std::atomic<uint64_t> mirrored;
    char _pad1[56];

    // Test 2: acquire-release pairing.
    std::atomic<uint64_t> seq;
    char _pad2[56];
    uint64_t buffer[kBufferLen];
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "std::atomic<uint64_t> must be lock-free");

Region* map_shm() {
    int fd = shm_open(kShmPath, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); std::exit(1); }
    if (ftruncate(fd, sizeof(Region)) < 0) { perror("ftruncate"); std::exit(1); }
    void* p = mmap(nullptr, sizeof(Region), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); std::exit(1); }
    return reinterpret_cast<Region*>(p);
}

int run_producer(Region* r) {
    for (uint64_t i = 1; i <= kIterations; ++i) {
        // Test 1: write (i<<32)|i for torn-read detection.
        r->mirrored.store((i << 32) | (i & 0xFFFFFFFFULL),
                          std::memory_order_release);

        // Test 2: fill the buffer with i, then publish seq=i.
        for (uint32_t k = 0; k < kBufferLen; ++k) {
            r->buffer[k] = i;
        }
        r->seq.store(i, std::memory_order_release);
    }
    return 0;
}

int run_consumer(Region* r) {
    uint64_t last_mirrored = 0;
    uint64_t last_seq = 0;
    uint64_t reads = 0;
    uint64_t torn = 0;
    uint64_t pair_violations = 0;

    while (true) {
        uint64_t m = r->mirrored.load(std::memory_order_acquire);
        uint32_t hi = static_cast<uint32_t>(m >> 32);
        uint32_t lo = static_cast<uint32_t>(m & 0xFFFFFFFFULL);
        if (hi != lo) ++torn;
        if (m < last_mirrored) {
            std::fprintf(stderr, "FAIL: mirrored regressed: %llx -> %llx\n",
                         (unsigned long long)last_mirrored,
                         (unsigned long long)m);
            return 2;
        }
        last_mirrored = m;

        uint64_t s = r->seq.load(std::memory_order_acquire);
        if (s < last_seq) {
            std::fprintf(stderr, "FAIL: seq regressed: %llu -> %llu\n",
                         (unsigned long long)last_seq, (unsigned long long)s);
            return 2;
        }
        // Acquire on seq must make the buffer writes that happened before the
        // producer's release-store visible. Buffer entries should all be >= s.
        if (s > 0) {
            for (uint32_t k = 0; k < kBufferLen; ++k) {
                if (r->buffer[k] < s) { ++pair_violations; break; }
            }
        }
        last_seq = s;
        ++reads;
        if (s >= kIterations && m >= ((kIterations << 32) | kIterations)) break;
    }

    std::printf("consumer: %llu reads, final seq=%llu, torn=%llu, pair_viol=%llu\n",
                (unsigned long long)reads, (unsigned long long)last_seq,
                (unsigned long long)torn, (unsigned long long)pair_violations);
    if (torn || pair_violations) return 3;
    return 0;
}

} // namespace

int main() {
    shm_unlink(kShmPath);
    Region* r = map_shm();
    new (r) Region{};

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) std::exit(run_producer(r));

    int consumer_rc = run_consumer(r);
    int status = 0;
    waitpid(pid, &status, 0);
    int producer_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    munmap(r, sizeof(Region));
    shm_unlink(kShmPath);

    if (producer_rc != 0) {
        std::fprintf(stderr, "FAIL: producer exited %d\n", producer_rc);
        return producer_rc;
    }
    if (consumer_rc != 0) return consumer_rc;
    std::printf("OK\n");
    return 0;
}
