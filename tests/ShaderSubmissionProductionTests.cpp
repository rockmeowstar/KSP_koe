#define NOMINMAX
#include <Windows.h>
#undef PropertySheet
#include "UnityAbi.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <initializer_list>
using namespace ShaderSubmission;
static unsigned checks;
static void check(bool value,const char* label) {++checks;if(!value)throw std::runtime_error(label);}
template<class T> static T symbol(HMODULE module,const char* name) {
    auto result=GetProcAddress(module,name);check(result!=nullptr,name);return reinterpret_cast<T>(result);
}
int wmain(int argc,wchar_t** argv) {
    try {
        check(argc==3,"Unity image and production DLL required");
        auto* unity=reinterpret_cast<uint8_t*>(LoadLibraryW(argv[1]));auto module=LoadLibraryW(argv[2]);
        check(unity&&module,"load production images");
        auto install=symbol<int(*)()>(module,"InstallShaderSubmission");
        auto select=symbol<int(*)(int)>(module,"SetShaderSubmissionMode");
        check(!GetProcAddress(module,"GetShaderSubmissionCounters"),"audit excluded from production");
        check(select(2)==-1,"mode rejected before install");
        auto* target=unity+0x456000;uint8_t before[16];memcpy(before,target,sizeof before);
        DWORD protection=0,ignored=0;check(VirtualProtect(target,16,PAGE_EXECUTE_READWRITE,&protection)!=0,"fixture code access");
        target[0]^=1;int rejected=install();memcpy(target,before,sizeof before);
        FlushInstructionCache(GetCurrentProcess(),target,16);VirtualProtect(target,16,protection,&ignored);
        check(rejected==-3,"unknown code rejected before hooking");
        check(install()==0,"verified image installed");check(install()==-1,"double installation rejected");
        auto info=symbol<void(*)(uint64_t*,int)>(module,"GetShaderSubmissionParallelInfo");uint64_t parallel[7]{};info(parallel,7);
        check(parallel[0]>0&&parallel[1]<parallel[0],"worker count follows available logical processors");
        printf("PARALLEL logical=%llu workers=%llu calibrated_copy_threshold=%llu\n",parallel[0],parallel[1],parallel[2]);
        PropertySheet empty{};auto root=reinterpret_cast<void(*)(uint8_t*,const PropertySheet*)>(target);
        for(int mode:{0,1,2}) {check(select(mode)==0,"supported production mode");root(nullptr,&empty);}
        check(select(3)==-1&&select(-1)==-1,"audit and invalid modes rejected");
        check(select(0)==0,"original path restored");
        printf("PASS production activation/guards/modes checks=%u\n",checks);return 0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
