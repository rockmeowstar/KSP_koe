#include "EncoderLocator.h"
#include <vector>
#include <iostream>
#include <stdexcept>
#include <algorithm>
static void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(){try{
 const unsigned char signature[]={1,2,3};std::vector<unsigned char> bytes(100);
 Check(EncoderLocator::Find(nullptr,0,signature,3).count==0,"absent module bytes");
 Check(EncoderLocator::Find(bytes.data(),bytes.size(),signature,3).count==0,"absent function");
 std::copy(signature,signature+3,bytes.begin()+17);
 auto match=EncoderLocator::Find(bytes.data(),bytes.size(),signature,3);Check(match.count==1&&match.offset==17,"resolve from arbitrary base-relative location");
 std::copy(signature,signature+3,bytes.begin()+80);
 Check(EncoderLocator::Find(bytes.data(),bytes.size(),signature,3).count==2,"ambiguous code must reject");
 Check(EncoderLocator::Find(bytes.data(),2,signature,3).count==0,"truncated section must reject");
 Check(EncoderLocator::Find(bytes.data(),100,signature,0).count==0,"empty signature must reject");
 std::cout<<"ENCODER_LOCATOR_PASS checks=6\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
