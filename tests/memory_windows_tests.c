/* Windows guest memory: sparse backing, on-demand commitment, aliases, protection and release.
 * Includes the implementation so commitment can be inspected and failed on demand. */
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
#include <psapi.h>
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
static uint64_t system_commit(void) {
    PERFORMANCE_INFORMATION info;
    memset(&info,0,sizeof(info)); info.cb=sizeof(info);
    K32GetPerformanceInfo(&info,sizeof(info));
    return (uint64_t)info.CommitTotal*info.PageSize;
}
static DWORD state_at(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address,&info,sizeof(info)) ? info.State : 0;
}
static DWORD protect_at(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address,&info,sizeof(info)) ? info.Protect : 0;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    MEMORY_BASIC_INFORMATION info;
    fail_section_creation=1;
    CHECK(runtime_memory_prepare()!=0);
    CHECK(backing_base==NULL && section==NULL);
    fail_section_creation=0;
    fail_commit=1;
    CHECK(runtime_memory_prepare()!=0);
    fail_commit=0;
    CHECK(backing_base!=NULL);
    if (!backing_base) return 1;
    CHECK(VirtualQuery(backing_base,&info,sizeof(info)) && info.State==MEM_RESERVE);
    CHECK(runtime_memory_prepare()==0);
    CHECK(VirtualQuery(backing_base+POOL_SIZE+FLEX_SPAN-PAGE,&info,sizeof(info)) && info.State==MEM_RESERVE);

    void *at=NULL;
    CHECK(map_flexible(&at,4*PAGE,3,0)==0);
    if (!at) return 1;
    unsigned char *code=at;
    code[0]=0xb8; code[1]=42; code[2]=0; code[3]=0; code[4]=0; code[5]=0xc3;
    CHECK(kernel_mprotect(at,4*PAGE,7)==0);
    CHECK(((int (*)(void))at)()==42);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(kernel_mprotect(at,4*PAGE,7)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READWRITE);
    CHECK(kernel_mprotect(at,4*PAGE,5)==0);
    CHECK(protect_at(at)==PAGE_EXECUTE_READ);
    CHECK(((int (*)(void))at)()==42);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(state_at(backing_base+POOL_SIZE+4*PAGE)==MEM_RESERVE);

    const unsigned char tracked=23;
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(protect_at(at)==PAGE_NOACCESS);
    CHECK(runtime_memory_write_backing((uintptr_t)at+PAGE,&tracked,1));
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,0);
    CHECK(protect_at(at)==PAGE_READONLY);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(((unsigned char *)at)[PAGE]==tracked);
    runtime_memory_gpu_protect((uintptr_t)at+PAGE,PAGE,0,0);
    CHECK(protect_at(at)==PAGE_READWRITE && protect_at((char *)at+PAGE)==PAGE_NOACCESS && protect_at((char *)at+2*PAGE)==PAGE_READWRITE);
    runtime_memory_gpu_protect((uintptr_t)at+PAGE,PAGE,1,1);
    CHECK(protect_at((char *)at+PAGE)==PAGE_READWRITE);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_READONLY);
    CHECK(kernel_mprotect(at,4*PAGE,5)==0);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(protect_at(at)==PAGE_EXECUTE);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_EXECUTE_READ);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    const unsigned char marker=17;
    CHECK(runtime_memory_write_backing((uintptr_t)at,&marker,1));
    CHECK(*(unsigned char *)at==marker);
    CHECK(direct_unmap(at,4*PAGE)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.State==MEM_RESERVE);
    at=NULL; CHECK(map_flexible(&at,4*PAGE,3,0)==0); CHECK(*(unsigned char *)at==0); CHECK(direct_unmap(at,4*PAGE)==0);

    void *executable=NULL;
    CHECK(map_flexible(&executable,PAGE,7,0)==0);
    if (executable) { *(unsigned char *)executable=0xc3; CHECK(VirtualQuery(executable,&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READWRITE); CHECK(direct_unmap(executable,PAGE)==0); }
    const unsigned char ret=0xc3;
    CHECK(map_flexible(&executable,PAGE,5,0)==0);
    if (executable) {
        CHECK(protect_at(executable)==PAGE_EXECUTE_READ);
        CHECK(runtime_memory_write_backing((uintptr_t)executable,&ret,1));
        ((void (*)(void))executable)();
        CHECK(kernel_mprotect(executable,PAGE,7)==0 && protect_at(executable)==PAGE_EXECUTE_READWRITE);
        CHECK(kernel_mprotect(executable,PAGE,3)==0 && protect_at(executable)==PAGE_READWRITE);
        CHECK(direct_unmap(executable,PAGE)==0);
    }
    int64_t phys=-1;
    void *direct_code=NULL;
    CHECK(direct_allocate(0,INT64_MAX,PAGE,0,3,&phys)==0);
    CHECK(direct_map(&direct_code,PAGE,7,0,phys,0)==0);
    if (direct_code) {
        *(unsigned char *)direct_code=ret;
        CHECK(protect_at(direct_code)==PAGE_EXECUTE_READWRITE);
        ((void (*)(void))direct_code)();
        CHECK(kernel_mprotect(direct_code,PAGE,5)==0 && protect_at(direct_code)==PAGE_EXECUTE_READ);
        CHECK(kernel_mprotect(direct_code,PAGE,3)==0 && protect_at(direct_code)==PAGE_READWRITE);
        CHECK(direct_unmap(direct_code,PAGE)==0);
    }
    direct_code=NULL;
    CHECK(direct_map(&direct_code,PAGE,5,0,phys,0)==0);
    if (direct_code) {
        CHECK(protect_at(direct_code)==PAGE_EXECUTE_READ);
        ((void (*)(void))direct_code)();
        CHECK(kernel_mprotect(direct_code,PAGE,7)==0 && protect_at(direct_code)==PAGE_EXECUTE_READWRITE);
        CHECK(direct_unmap(direct_code,PAGE)==0);
    }
    CHECK(direct_release(phys,PAGE)==0);

    unsigned char *split=NULL;
    CHECK(map_flexible((void **)&split,8*PAGE,3,0)==0);
    if (split) {
        split[0]=0x11; split[7*PAGE]=0x77;
        CHECK(direct_unmap(split+4*PAGE,4*PAGE)==0);
        CHECK(split[0]==0x11);
        void *other=NULL; CHECK(map_flexible(&other,4*PAGE,3,0)==0);
        if (other) { memset(other,0x22,4*PAGE); CHECK(split[0]==0x11 && split[3*PAGE]==0); CHECK(direct_unmap(other,4*PAGE)==0); }
        CHECK(direct_unmap(split,2*PAGE)==0);
        const unsigned char patch=0x33;
        CHECK(runtime_memory_write_backing((uintptr_t)split+3*PAGE,&patch,1));
        CHECK(split[3*PAGE]==patch && split[2*PAGE]==0);
        CHECK(direct_unmap(split+2*PAGE,2*PAGE)==0);
        CHECK(flexible_bytes==0);
    }
    unsigned char *middle=NULL;
    CHECK(map_flexible((void **)&middle,8*PAGE,3,0)==0);
    if (middle) {
        void *hole=middle+3*PAGE;
        CHECK(map_flexible(&hole,2*PAGE,3,MAP_FIXED_FLAG)==0);
        CHECK(flexible_bytes==8*PAGE);
        CHECK(direct_unmap(middle,3*PAGE)==0 && direct_unmap(middle+5*PAGE,3*PAGE)==0);
        CHECK(direct_unmap(hole,2*PAGE)==0);
        CHECK(flexible_bytes==0);
    }

    const uint64_t charged=128u<<20;
    CHECK(direct_allocate(0,INT64_MAX,charged,0,3,&phys)==0);
    void *first=NULL,*second=NULL;
    const uint64_t commit_before=system_commit();
    CHECK(direct_map(&first,charged,3,0,phys,0)==0);
    const uint64_t commit_first=system_commit();
    CHECK(direct_map(&second,charged,3,0,phys,0)==0);
    const uint64_t commit_second=system_commit();
    CHECK((int64_t)(commit_first-commit_before)>=(int64_t)(charged*3/4));
    CHECK((int64_t)(commit_second-commit_first)<(int64_t)(charged/2));
    CHECK(direct_unmap(first,charged)==0); CHECK(direct_unmap(second,charged)==0); CHECK(direct_release(phys,charged)==0);

    CHECK(direct_allocate(0,INT64_MAX,8*PAGE,0,3,&phys)==0);
    first=NULL; second=NULL;
    CHECK(direct_map(&first,8*PAGE,3,0,phys,0)==0); CHECK(direct_map(&second,8*PAGE,3,0,phys,0)==0);
    CHECK(first && second && first!=second);
    ((uint64_t *)first)[3]=UINT64_C(0x1122334455667788);
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    ((uint64_t *)second)[5]=UINT64_C(0x8877665544332211);
    CHECK(((uint64_t *)first)[5]==UINT64_C(0x8877665544332211));

    void *failed=NULL; fail_commit=1;
    CHECK(map_flexible(&failed,PAGE,3,0)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    void *replacement=first; fail_commit=1;
    CHECK(map_flexible(&replacement,PAGE,3,MAP_FIXED_FLAG)==NO_MEMORY);
    replacement=first;
    CHECK(direct_map(&replacement,PAGE,3,MAP_FIXED_FLAG,phys,0)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    void *start=NULL,*end=NULL; uint32_t prot=0;
    CHECK(query_protection(first,&start,&end,&prot)==0 && start==first && end>=(void *)((char *)first+8*PAGE) && prot==3);
    replacement=first;
    CHECK(direct_map(&replacement,PAGE,3,MAP_FIXED_FLAG,phys,0)==0 && replacement==first);
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788) && ((uint64_t *)second)[5]==UINT64_C(0x8877665544332211));
    CHECK(map_flexible(&failed,PAGE,3,0)==0); CHECK(direct_unmap(failed,PAGE)==0);

    CHECK(kernel_mprotect(first,2*PAGE,1)==0);
    CHECK(direct_unmap((char *)first+2*PAGE,2*PAGE)==0);
    CHECK(VirtualQuery(first,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(direct_unmap(first,2*PAGE)==0); CHECK(direct_unmap((char *)first+4*PAGE,4*PAGE)==0);
    CHECK(direct_unmap(second,8*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);
    void *reused=NULL; CHECK(direct_allocate(0,INT64_MAX,8*PAGE,0,3,&phys)==0);
    CHECK(direct_map(&reused,8*PAGE,3,0,phys,0)==0);
    CHECK(((uint64_t *)reused)[3]==0);
    CHECK(direct_unmap(reused,8*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);

    const int64_t untouched=INT64_C(2)<<30;
    CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    void *partial=NULL; CHECK(direct_map(&partial,2*PAGE,3,0,phys,0)==0);
    if (partial) *(unsigned char *)partial=0x77;
    CHECK(state_at(backing_base+phys+2*PAGE)==MEM_RESERVE);
    CHECK(direct_unmap(partial,2*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);
    CHECK(state_at(backing_base+phys+2*PAGE)==MEM_RESERVE);
    partial=NULL; CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    CHECK(direct_map(&partial,2*PAGE,3,0,phys,0)==0 && partial && *(unsigned char *)partial==0);
    CHECK(direct_unmap(partial,2*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);

    unsigned char *whole=NULL,*alias=NULL;
    CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    CHECK(direct_map((void **)&whole,8*PAGE,3,0,phys,0)==0 && direct_map((void **)&alias,8*PAGE,3,0,phys,0)==0);
    if (whole && alias) {
        whole[0]=0x10; whole[3*PAGE]=0x13; whole[7*PAGE]=0x17;
        CHECK(direct_release(phys+2*PAGE,3*PAGE)==0);
        CHECK(state_at(whole+2*PAGE)==MEM_RESERVE && state_at(alias+2*PAGE)==MEM_RESERVE);
        CHECK(state_at(whole)==MEM_COMMIT && state_at(whole+5*PAGE)==MEM_COMMIT);
        CHECK(whole[0]==0x10 && alias[0]==0x10 && whole[7*PAGE]==0x17 && alias[7*PAGE]==0x17);
        alias[PAGE]=0x21;
        CHECK(whole[PAGE]==0x21);
        CHECK(direct_release(phys,8*PAGE)==INVALID);
        void *refilled=NULL; int64_t again=-1;
        CHECK(direct_allocate(phys+2*PAGE,INT64_MAX,3*PAGE,0,3,&again)==0 && again==phys+(int64_t)(2*PAGE));
        CHECK(direct_map(&refilled,3*PAGE,3,0,again,0)==0);
        if (refilled) {
            CHECK(((unsigned char *)refilled)[PAGE]==0);
            CHECK(direct_unmap(refilled,3*PAGE)==0);
        }
        CHECK(direct_release(again,3*PAGE)==0);
        CHECK(whole[0]==0x10 && alias[7*PAGE]==0x17);
        CHECK(direct_unmap(whole,2*PAGE)==0 && direct_unmap(whole+5*PAGE,3*PAGE)==0);
        CHECK(direct_unmap(alias,2*PAGE)==0 && direct_unmap(alias+5*PAGE,3*PAGE)==0);
        CHECK(direct_release(phys,2*PAGE)==0 && direct_release(phys+5*PAGE,3*PAGE)==0);
    }

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
