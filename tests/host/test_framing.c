/* gap/delimiter framing + direction-fair throttle -- logic mirrors */
#include <stdio.h>
#include <string.h>
static int fails=0;
static void check(const char*n,int ok){printf("  [%s] %s\n",ok?"PASS":"FAIL",n);if(!ok)fails++;}
#define SNIP 32
typedef struct{int dir,n;unsigned tot;unsigned char b[SNIP];}Run;
typedef struct{int open,nr,byd;unsigned total;Run r[6];}Fr;
static Fr f; static unsigned observed=0,GAP=10000; static unsigned last_us=0;
static void closef(void){if(f.open){observed++;f.open=0;}}
static void byte(unsigned char v,int dir,unsigned us,unsigned char delim){
  if(f.open&&us-last_us>GAP)closef();
  last_us=us;
  if(!f.open){memset(&f,0,sizeof f);f.open=1;}
  f.total++;
  if(f.nr&&f.r[f.nr-1].dir==dir){Run*r=&f.r[f.nr-1];r->tot++;if(r->n<SNIP)r->b[r->n++]=v;}
  else{Run*r=&f.r[f.nr++];r->dir=dir;r->tot=1;r->n=1;r->b[0]=v;}
  if(delim&&v==delim){f.byd=1;closef();}}
int main(void){
  /* two bytes 5ms apart = one frame; 15ms gap = two frames */
  byte('A',0,1000,0); byte('B',0,6000,0);
  check("5ms apart: same frame", f.open&&f.total==2);
  byte('C',0,21000,0);
  check("15ms gap closed the first", observed==1&&f.total==1);
  closef();
  /* delimiter framing */
  observed=0; byte('h',1,100000,0x0A); byte('i',1,101000,0x0A); byte(0x0A,1,102000,0x0A);
  check("LF delimiter closes", observed==1);
  byte('x',1,103000,0x0A); check("next frame reopens", f.open&&f.total==1);
  closef();
  /* direction fairness: same dir inside 20ms gap drops; other dir passes */
  { int lastdir=2; unsigned lastemit=0;
    #define EMIT(d,us) (!( (d)==lastdir && (us)-lastemit<20000 ) ? (lastdir=(d),lastemit=(us),1) : 0)
    check("TX first passes", EMIT(0,1000)==1);
    check("RX reply 3ms later PASSES", EMIT(1,4000)==1);   /* Modbus poll+answer */
    check("second RX 2ms later drops", EMIT(1,6000)==0);
    check("TX again passes", EMIT(0,8000)==1);
  }
  /* DMX frame shape: start code 00 + channels, closed by gap */
  observed=0; unsigned us=200000;
  byte(0x00,1,us,0); for(int i=0;i<24;i++)byte((unsigned char)i,1,us+=44,0);
  byte(0x00,1,us+=5000,0); /* next frame after 5ms gap>4ms? GAP=10ms here: same frame */
  check("DMX 25 slots one frame under gap", f.total>=25);
  printf(fails?"\n%d FAILURE(S)\n":"\nALL FRAMING TESTS PASS\n",fails);return fails?1:0;}
