/* Link the restorer-loader at 252 GiB, in the reserved top slice of the Sv39
 * range ([248 GiB, 256 GiB), see kRestorerVaBase). qemu-user's -R places the
 * guest far below (~92 GiB), so the loader's code/data/stack (252 GiB) and its
 * blob buffer (248 GiB, see BLOB_ADDR in restorer.c) never collide with the
 * regions it reconstructs, and the capture cutoff (248 GiB) keeps a
 * re-checkpoint from grabbing the loader. Living inside the Sv39 range lets the
 * loader's QEMU use the *same* -R as the workload, so a resumed guest stays
 * Sv39-bounded.
 *
 * NB: prototype assumption -- a guest that mapped memory above 248 GiB would
 * collide. A production loader would punch a hole / use a CRIU-style
 * relocatable restorer blob instead.
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS {
    . = 0x3F00000000;          /* 252 GiB (reserved top slice of Sv39 range) */

    .text : {
        *(.text.start)
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : {
        *(.bss*)
        *(COMMON)
        /* Private stack lives *inside* .bss so the PT_LOAD memsz covers it
         * (advancing '.' outside an allocated section leaves it unmapped). */
        . = ALIGN(16);
        _stack_bottom = .;
        . += 0x100000;         /* 1 MiB private stack */
        . = ALIGN(16);
        _stack_top = .;
    }
}
