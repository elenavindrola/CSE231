# Entry point for the restorer-loader.
#
# On entry the kernel/QEMU has set sp to the initial process stack:
#   [sp+0]  argc
#   [sp+8]  argv[0], argv[1], ... NULL, envp...
# We must read argv before remapping the guest's stack region (which sits at
# the default QEMU stack address and gets overwritten during reconstruction),
# so restore_main copies the path out first. We switch to a private 1 MiB
# stack inside the loader image so our own frames survive the remap.

.section .text.start, "ax", @progbits
.globl _start
_start:
    mv      a0, sp              # a0 = pointer to initial stack (argc at [a0])
    la      sp, _stack_top      # switch to the loader's private stack
    li      s0, 0               # clear frame pointer
    call    restore_main        # restore_main(unsigned long *initial_sp)

    # restore_main returns once guest memory is rebuilt; hand control to the
    # host debugger (it restores registers + PC and continues the guest).
1:  ebreak
    j       1b
