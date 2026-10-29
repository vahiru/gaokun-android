/* Compile unchanged and diagnostic sources separately: identical valid contract. */
#include <assert.h>
#include "hexagonrpcd/apps_std.c"
int main(void) {
    struct apps_std_ctx ctx={0};ctx.rootfd=-1;ctx.adsp_library_dirfd=-1;
    char env[]="ADSP_LIBRARY_PATH",path[]="/nonexistent",mode[2]="w";uint32_t fd=123;
    struct fastrpc_io_buffer in[5]={{0},{.p=env,.s=sizeof(env)},{0},{.p=path,.s=sizeof(path)},{.p=mode,.s=sizeof(mode)}};
    struct fastrpc_io_buffer out[1]={{.p=&fd,.s=4}};
    assert(apps_std_fopen_with_env(&ctx,in,out)==20);assert(fd==123);
    mode[0]='a';assert(apps_std_fopen_with_env(&ctx,in,out)==20);assert(fd==123);
    mode[0]='r';assert(apps_std_fopen_with_env(&ctx,in,out)==1);assert(fd==123);
    puts("CONTRACT w=20 a=20 read-unavailable=1 out-unchanged");
}
