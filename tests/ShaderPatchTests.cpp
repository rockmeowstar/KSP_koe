#include "ShaderPatch.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
static std::vector<unsigned char> Read(const char* path){std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error(path);auto size=file.tellg();std::vector<unsigned char> data(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(data.data()),size);return data;}
static void Check(bool condition){if(!condition)throw std::runtime_error("Shader recipe assertion");}
int main(int argc,char** argv)
{
    if(argc!=4)return 2;
    auto patch=Read(argv[1]),source=Read(argv[2]),expected=Read(argv[3]);std::vector<unsigned char> result;
    Check(KoeShaderPatch::Apply(patch,source.data(),source.size(),result)&&result==expected);
    for(size_t size=0;size<patch.size();size++)
    {std::vector<unsigned char> truncated(patch.begin(),patch.begin()+size);Check(!KoeShaderPatch::Apply(truncated,source.data(),source.size(),result));}
    source[0]^=1;Check(!KoeShaderPatch::Apply(patch,source.data(),source.size(),result));source[0]^=1;
    auto corrupt=patch;corrupt[0]^=1;Check(!KoeShaderPatch::Apply(corrupt,source.data(),source.size(),result));
    corrupt=patch;corrupt[88]=255;Check(!KoeShaderPatch::Apply(corrupt,source.data(),source.size(),result));
    corrupt=patch;uint32_t tooLarge=0xffffffff;std::memcpy(corrupt.data()+16,&tooLarge,4);Check(!KoeShaderPatch::Apply(corrupt,source.data(),source.size(),result));
    corrupt=patch;corrupt.back()^=0x7f;Check(!KoeShaderPatch::Apply(corrupt,source.data(),source.size(),result));
    std::cout<<"SHADER_RECIPE_NATIVE_PASS: accepted output byte-identical; all truncations, wrong source, magic, opcode, size and output corruption rejected\n";
}
