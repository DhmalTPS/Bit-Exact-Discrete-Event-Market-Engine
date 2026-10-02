#include "../src/sha256.hpp"
#include <vector>
#include <cstdio>
int main(){
  for (size_t n : {0ul,1ul,3ul,55ul,56ul,63ul,64ul,65ul,119ul,128ul,1000ul,100000ul}) {
    std::vector<uint8_t> d(n); for (size_t i=0;i<n;i++) d[i]=uint8_t((i*131+7)^(i>>3));
    printf("%zu %s %s\n", n, t3::sha256_hex(d.data(),n,false).c_str(), t3::sha256_hex(d.data(),n,true).c_str());
  }
}
