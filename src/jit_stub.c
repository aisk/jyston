/* Code generation is only implemented for x86-64. On other architectures
 * every code object is rejected and runs in the interpreter. */

#include "jit.h"

int
jy_jit_compile(PyCodeObject *co, JyCodeInfo *info)
{
    (void)co;
    (void)info;
    return -1;
}

void
jy_jit_free(JyCodeInfo *info)
{
    (void)info;
}
