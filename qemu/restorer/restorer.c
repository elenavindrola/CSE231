// restorer-loader: rebuilds a guest address space from an RVCKPT01 checkpoint
// blob, then traps (ebreak) back to the host, which restores registers + PC
// through the GDB stub and resumes the guest.
//
// This is the guest-side half of Tier 2 restore. Its job is the one thing the
// host cannot do from outside the guest: *create* the mappings (mmap), rather
// than relying on the original binary to re-run and recreate them. Freestanding
// (no libc); raw syscalls via ecall; alignment-safe blob parsing.
//
// Blob layout (must match ExecutionCheckpoint::serialize):
//   magic[8]="RVCKPT01"
//   pc u64, gpr[32] u64, fpr[32] u64, fcsr u32
//   csr: present u8, mstatus u64, mepc u64, satp u64, reserved[8] u64
//   guest_base u64
//   nregions u64
//   per region: addr u64, prot u32, anonymous u8, file_offset u64,
//               path_len u32, path[path_len], data_len u64, data[data_len]

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef long               i64;

// ---- RISC-V Linux syscall numbers (generic ABI) ----
#define SYS_openat   56
#define SYS_close    57
#define SYS_lseek    62
#define SYS_read     63
#define SYS_write    64
#define SYS_exit     93
#define SYS_mmap     222
#define SYS_munmap   215
#define SYS_mprotect 226

#define AT_FDCWD     (-100)
#define O_RDONLY     0
#define SEEK_END     2

#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define MAP_PRIVATE  0x02
#define MAP_FIXED    0x10
#define MAP_ANON     0x20

// Fixed buffer for the checkpoint blob, at 248 GiB -- in the reserved top slice
// of the Sv39 range, below the loader image (252 GiB) and clear of every region
// we reconstruct (see .ld). munmap'd before handoff.
#define BLOB_ADDR    0x3E00000000UL   // 248 GiB
#define MAX_REGIONS  4096

static i64 syscall6(long n, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    register u64 x10 __asm__("a0") = a0;
    register u64 x11 __asm__("a1") = a1;
    register u64 x12 __asm__("a2") = a2;
    register u64 x13 __asm__("a3") = a3;
    register u64 x14 __asm__("a4") = a4;
    register u64 x15 __asm__("a5") = a5;
    register u64 x17 __asm__("a7") = (u64)n;
    __asm__ volatile("ecall"
                     : "+r"(x10)
                     : "r"(x11), "r"(x12), "r"(x13), "r"(x14), "r"(x15), "r"(x17)
                     : "memory");
    return (i64)x10;
}

static void sys_exit(int code) { syscall6(SYS_exit, (u64)code, 0, 0, 0, 0, 0); }

// Alignment-safe little-endian reads (blob fields are not naturally aligned).
static u64 rd_u64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v |= (u64)p[i] << (8 * i);
    return v;
}
static u32 rd_u32(const u8* p) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) v |= (u32)p[i] << (8 * i);
    return v;
}

static void copy_bytes(u8* dst, const u8* src, u64 n) {
    for (u64 i = 0; i < n; i++) dst[i] = src[i];
}

// restore_main(initial_sp): initial_sp[0]=argc, [1..]=argv. argv[1] is the
// checkpoint path. Returns to start.S (which ebreaks) on success; exits
// nonzero on any failure so the host sees an abnormal stop.
void restore_main(u64* initial_sp) {
    long argc = (long)initial_sp[0];
    char** argv = (char**)(initial_sp + 1);   // argv[0] = program, argv[1] = blob
    if (argc < 2) sys_exit(2);
    const char* path = argv[1];

    int fd = (int)syscall6(SYS_openat, (u64)AT_FDCWD, (u64)path, O_RDONLY, 0, 0, 0);
    if (fd < 0) sys_exit(3);

    i64 size = syscall6(SYS_lseek, (u64)fd, 0, SEEK_END, 0, 0, 0);
    if (size <= 0) sys_exit(4);

    // Map the blob read-only at a fixed high address (out of the guest's way).
    i64 m = syscall6(SYS_mmap, BLOB_ADDR, (u64)size, PROT_READ,
                     MAP_PRIVATE | MAP_FIXED, (u64)fd, 0);
    if (m != (i64)BLOB_ADDR) sys_exit(5);
    syscall6(SYS_close, (u64)fd, 0, 0, 0, 0, 0);

    const u8* base = (const u8*)BLOB_ADDR;
    static const char magic[8] = {'R','V','C','K','P','T','0','1'};
    for (int i = 0; i < 8; i++)
        if (base[i] != (u8)magic[i]) sys_exit(6);

    // Skip the fixed header to the region table.
    const u8* p = base + 8;
    p += 8;          // pc
    p += 32 * 8;     // gpr
    p += 32 * 8;     // fpr
    p += 4;          // fcsr
    p += 1;          // csr.present
    p += 8 * 3;      // mstatus, mepc, satp
    p += 8 * 8;      // csr.reserved[8]
    p += 8;          // guest_base

    u64 nregions = rd_u64(p); p += 8;
    if (nregions > MAX_REGIONS) sys_exit(7);

    // Saved for the second (mprotect) pass.
    static u64 r_addr[MAX_REGIONS];
    static u64 r_len[MAX_REGIONS];
    static u32 r_prot[MAX_REGIONS];

    // Pass 1: create each region writable and copy its bytes in.
    for (u64 i = 0; i < nregions; i++) {
        u64 addr     = rd_u64(p); p += 8;
        u32 prot     = rd_u32(p); p += 4;
        p += 1;                   // anonymous (snapshot semantics: always anon-map)
        p += 8;                   // file_offset
        u32 path_len = rd_u32(p); p += 4;
        p += path_len;            // path (skipped)
        u64 data_len = rd_u64(p); p += 8;
        const u8* data = p;       p += data_len;

        if (data_len == 0) { r_len[i] = 0; continue; }

        i64 r = syscall6(SYS_mmap, addr, data_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON | MAP_FIXED, (u64)(-1), 0);
        if (r != (i64)addr) sys_exit(8);

        copy_bytes((u8*)addr, data, data_len);

        r_addr[i] = addr;
        r_len[i]  = data_len;
        r_prot[i] = prot;          // PROT_* bit values match our snapshot flags
    }

    // Pass 2: drop each region to its real protection (e.g. r-x text, r-- rodata).
    for (u64 i = 0; i < nregions; i++) {
        if (r_len[i] == 0) continue;
        syscall6(SYS_mprotect, r_addr[i], r_len[i], r_prot[i], 0, 0, 0);
    }

    // Drop the blob buffer; everything is reconstructed.
    syscall6(SYS_munmap, BLOB_ADDR, (u64)size, 0, 0, 0, 0);
    // Return -> start.S ebreak; host restores registers/PC and resumes.
}
