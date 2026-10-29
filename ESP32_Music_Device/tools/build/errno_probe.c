#include <stdio.h>
#include <errno.h>
int main(void){
  printf("ENOMEM=%d ENOBUFS=%d EMFILE=%d ECONNRESET=%d ETIMEDOUT=%d EHOSTUNREACH=%d\n",
         ENOMEM, ENOBUFS, EMFILE, ECONNRESET, ETIMEDOUT, EHOSTUNREACH);
  return 0;
}
