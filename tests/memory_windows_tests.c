/* Windows counterpart of runtime_tests.c memory checks, plus sparse backing regressions.
 * Include the implementation so commitment is inspected without exposing test APIs. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_log_flush(void) {}
int32_t *runtime_errno(void) { static int32_t value; return &value; }
#include <windows.h>
static int fail_section_creation, fail_commit;
static LPVOID WINAPI test_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
    if (fail_commit && (type & MEM_COMMIT)) { SetLastError(ERROR_COMMITMENT_LIMIT); return NULL; }
    return VirtualAlloc(address,size,type,protection);
}
static HANDLE WINAPI test_create_section(HANDLE file, SECURITY_ATTRIBUTES *security, ULONG access, ULONG protection,
                                         ULONG attributes, ULONG64 size, PCWSTR name,
                                         MEM_EXTENDED_PARAMETER *parameters, ULONG count) {
    if (fail_section_creation) { SetLastError(ERROR_COMMITMENT_LIMIT); return NULL; }
    return CreateFileMapping2(file,security,access,protection,attributes,size,name,parameters,count);
}
#define CreateFileMapping2 test_create_section
#define VirtualAlloc test_virtual_alloc
#include "../src/runtime_memory.c"
#undef CreateFileMapping2
#undef VirtualAlloc
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s (WinError %lu)\n",__LINE__,#x,GetLastError()); } } while(0)
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    /* Startup can handle resource failure before any GPU/guest worker is started. */
    fail_section_creation=1;
    CHECK(runtime_memory_prepare()!=0);
    CHECK(backing_base==NULL && section==NULL);
    fail_section_creation=0;
    /* Must reserve the retail 6080MiB pool without charging all those pages. */
    int prepared=pool();
    CHECK(prepared==0);
    if (prepared) return 1;
    fail_commit=1;
    CHECK(runtime_memory_prepare()!=0);
    fail_commit=0;
    CHECK(runtime_memory_prepare()==0);
    MEMORY_BASIC_INFORMATION info;
    CHECK(VirtualQuery(backing_base+POOL_SIZE+FLEX_SPAN-PAGE,&info,sizeof(info))!=0);
    CHECK(info.State==MEM_RESERVE);
    /* Same mapping/protection/alias cases as the existing Linux runtime tests. */
    void *at=NULL;
    CHECK(map_flexible(&at,4*PAGE,3,0)==0);
    if (!at) return 1;
    unsigned char *code=at;
    code[0]=0xb8; code[1]=42; code[2]=0; code[3]=0; code[4]=0; code[5]=0xc3;
    CHECK(kernel_mprotect(at,4*PAGE,7)==0);
    CHECK(((int (*)(void))at)()==42);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    const unsigned char marker=17;
    CHECK(runtime_memory_write_backing((uintptr_t)at,&marker,1));
    CHECK(*(unsigned char *)at==marker);
    CHECK(direct_unmap(at,4*PAGE)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.State==MEM_RESERVE);
    at=NULL; CHECK(map_flexible(&at,4*PAGE,3,0)==0); CHECK(*(unsigned char *)at==0); CHECK(direct_unmap(at,4*PAGE)==0);
    int64_t phys=-1; CHECK(direct_allocate(0,INT64_MAX,8*PAGE,0,3,&phys)==0);
    void *first=NULL,*second=NULL;
    CHECK(direct_map(&first,8*PAGE,3,0,phys,0)==0); CHECK(direct_map(&second,8*PAGE,3,0,phys,0)==0);
    CHECK(first && second && first!=second);
    ((uint64_t *)first)[3]=UINT64_C(0x1122334455667788);
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    /* A later commit failure returns a guest error without destroying existing aliases. */
    void *failed=NULL; fail_commit=1;
    CHECK(map_flexible(&failed,PAGE,3,0)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    /* OOM while replacing an existing fixed view must preserve both live aliases. */
    void *replacement=first; fail_commit=1;
    CHECK(map_flexible(&replacement,PAGE,3,MAP_FIXED_FLAG)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    CHECK(map_flexible(&failed,PAGE,3,0)==0); CHECK(direct_unmap(failed,PAGE)==0);
    /* Splitting/remapping a protected alias must preserve both content and protection. */
    CHECK(kernel_mprotect(first,2*PAGE,1)==0);
    CHECK(direct_unmap((char *)first+2*PAGE,2*PAGE)==0);
    CHECK(VirtualQuery(first,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(direct_unmap(first,2*PAGE)==0); CHECK(direct_unmap((char *)first+4*PAGE,4*PAGE)==0);
    CHECK(direct_unmap(second,8*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);
    /* Release an allocation never mapped: discarding it must not commit unused backing. */
    int64_t high=(int64_t)(POOL_SIZE-65536); CHECK(direct_allocate(high,(int64_t)POOL_SIZE,65536,65536,3,&phys)==0);
    CHECK(direct_release(phys,65536)==0);
    CHECK(VirtualQuery(backing_base+high,&info,sizeof(info)) && info.State==MEM_RESERVE);
    CHECK(pool_expand(0,INT64_MAX,65536,65536,&phys)==0);
    void *reserved=NULL; CHECK(pool_reserve(NULL,65536,65536,0,&reserved)==0);
    CHECK(pool_commit(reserved,65536,3,3,0)==0); memset(reserved,0x5a,65536);
    CHECK(pool_decommit(reserved,65536,0)==0);
    CHECK(VirtualQuery(reserved,&info,sizeof(info)) && info.State==MEM_RESERVE);
    CHECK(pool_commit(reserved,65536,3,3,0)==0);
    CHECK(pool_decommit(reserved,65536,0)==0);
    printf("Windows memory: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
