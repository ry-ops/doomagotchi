// compat_newlib.c — fill the handful of hosted-libc symbols that bare-metal
// newlib (riscv32-esp-elf) does not provide but the vendored engine references.
//
// Keeping these here (weak) instead of patching the submodule honors ADR 0001:
// the vendored tree stays byte-identical to upstream.

#include <errno.h>

// The only caller of system() in the vendored engine is i_system.c's Linux
// "zenity error box" fallback in I_Error — dead code on this target. Reporting
// "no shell available" (-1 / ENOSYS) is the correct behavior; I_Error then
// falls through to its normal abort path.
__attribute__((weak)) int system(const char *command)
{
    (void)command;
    errno = ENOSYS;
    return -1;
}
