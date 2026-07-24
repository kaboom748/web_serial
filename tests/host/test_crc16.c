/* Modbus CRC16 (poly A001 reflected) -- mirror of wser_crc16 + JS crc16 */
#include <stdio.h>
static int fails=0;
static void check(const char*n,int ok){printf("  [%s] %s\n",ok?"PASS":"FAIL",n);if(!ok)fails++;}
static unsigned crc16(const unsigned char*d,int n){unsigned c=0xFFFF;for(int i=0;i<n;i++){c^=d[i];for(int j=0;j<8;j++)c=(c&1)?((c>>1)^0xA001):(c>>1);}return c;}
int main(void){
  /* canonical Modbus example: 01 03 00 00 00 02 -> CRC C4 0B (LSB first on wire) */
  unsigned char q[6]={0x01,0x03,0x00,0x00,0x00,0x02};
  unsigned c=crc16(q,6);
  check("Modbus 01 03 00 00 00 02 -> 0BC4", c==0x0BC4);
  check("wire order LSB first", (c&0xFF)==0xC4 && (c>>8)==0x0B);
  unsigned char f[8]={0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};
  unsigned got=f[6]|(f[7]<<8);
  check("full frame verifies", crc16(f,6)==got);
  f[3]^=1; check("corruption detected", crc16(f,6)!=got);
  printf(fails?"\n%d FAILURE(S)\n":"\nALL CRC16 TESTS PASS\n",fails);return fails?1:0;}
