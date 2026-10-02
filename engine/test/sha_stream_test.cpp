#include "../src/sha256.hpp"
#include <vector>
#include <cstdio>
int main(){ // streaming in odd-sized pieces must equal one-shot
  for (size_t n : {0ul,1ul,63ul,64ul,65ul,1000ul,123457ul}) {
    std::vector<uint8_t> d(n); for (size_t i=0;i<n;i++) d[i]=uint8_t(i*7+3);
    t3::Sha256 h; size_t p=0, step=1; while(p<n){ size_t k=std::min(step,n-p); h.update(d.data()+p,k); p+=k; step=step*3+1; }
    printf("%zu %d\n", n, h.hex()==t3::sha256_hex(d.data(),n));
  }
}
