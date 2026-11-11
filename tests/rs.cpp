#include <cstdio>
#include <cmath>
#include <cstdint>
#include <initializer_list>
static float expt[256], mant[1024];
int main(){
  for (int i = 0; i < 256; ++i) { union { float f; int32_t l; } u; u.l = (i ? (i == 255 ? 254 : i) : 1) << 23; expt[i] = (float)(1.0/std::sqrt((double)u.f)); }
  for (int i = 0; i < 1024; ++i) mant[i] = (float)(1.0/std::sqrt(1.0 + i/1024.0));
  for (float x : {1e-20f, 1e-30f, 1.0f, 4.0f, 12345.f, 1e10f}) { union { float f; int32_t l; } u; u.f=x; float g=expt[(u.l>>23)&0xff]*mant[(u.l>>13)&0x3ff]; printf("%g -> %g (exact %g)\n", x, 1.5f*g-0.5f*g*g*g*x, 1/std::sqrt(x)); }
}
