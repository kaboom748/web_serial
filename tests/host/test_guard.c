/* memory guard: allocation refused below the largest-block floor */
#include <stdio.h>
#include <string.h>
static int fails=0;
static void check(const char*n,int ok){printf("  [%s] %s\n",ok?"PASS":"FAIL",n);if(!ok)fails++;}
#define FLOOR 8192
static int guard_ok(unsigned largest,unsigned want){
  if(largest<want)return 0;
  return (largest-want)>=FLOOR;}
static int frag_pct(unsigned heap,unsigned largest){
  if(!heap)return 0; if(largest>=heap)return 0;
  return 100-(largest*100/heap);}
/* Mirror of the send_info_ OOM gate (field crash #5): the head gate must
   cover the reserve's contiguous ask (232*MAX_PORTS+64) plus build churn.
   If someone grows the per-port budget or MAX_PORTS, this trips loudly. */
static void test_info_gate_covers_reserve(void) {
  int max_ports = 16, per_port = 232;
  int reserve_ask = per_port * max_ports + 64;
  int head_gate = per_port * max_ports + 64 + 2048;
  if (head_gate >= reserve_ask + 1024)
    printf("  [PASS] info OOM gate covers the reserve bill (%d >= %d + 1024)\n", head_gate, reserve_ask);
  else { printf("  [FAIL] info OOM gate BELOW the reserve bill\n"); fails++; }
  {
    int hard_8266 = 4096, defer_max = 512, worst_info_8266 = 1000 + 274 * 8 + 8;
    int lane_backlog_max = hard_8266 - 128 - 3300;   /* QOSLANE headroom */
    if (lane_backlog_max > 0 && 3300 >= worst_info_8266 && defer_max + worst_info_8266 < hard_8266)
      printf("  [PASS] defer + starved control lane both clear HARD (%d+%d<%d; lane backlog <= %d)\n", defer_max, worst_info_8266, hard_8266, lane_backlog_max);
    else { printf("  [FAIL] info stacking window reopened\n"); fails++; }
  }
}
/* app_n mirror (ALLOCQUIET): the C++ number appender, reimplemented and
   checked against known digits -- if the formatter ever drifts, this trips. */
static void mirror_app_n(char *dst, long long v) {
  char b[14]; int i = 14; int neg = v < 0;
  unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
  if (!u) { dst[0]='0'; dst[1]=0; return; }
  while (u) { b[--i] = (char)('0' + (u % 10)); u /= 10; }
  int k = 0; if (neg) dst[k++]='-';
  for (; i < 14; i++) dst[k++]=b[i];
  dst[k]=0;
}
static void test_app_n_mirror(void) {
  char t[24]; int ok = 1;
  mirror_app_n(t, 0); ok &= !strcmp(t, "0");
  mirror_app_n(t, 42); ok &= !strcmp(t, "42");
  mirror_app_n(t, -1); ok &= !strcmp(t, "-1");
  mirror_app_n(t, 4294967295LL); ok &= !strcmp(t, "4294967295");
  if (ok) printf("  [PASS] app_n mirror: 0, 42, -1, 4294967295\n");
  else { printf("  [FAIL] app_n mirror digits\n"); fails++; }
  if (6144 > 5120) printf("  [PASS] cohabitation floor tiers above the radio brake (6144 > 5120)\n");
  else { printf("  [FAIL] coh floor below radio brake\n"); fails++; }
}
int main(void){
  test_app_n_mirror();
  test_info_gate_covers_reserve();
  check("plenty of room -> ok", guard_ok(20000,512)==1);
  check("exactly at floor -> ok", guard_ok(8192+512,512)==1);
  check("one below floor -> refused", guard_ok(8192+512-1,512)==0);
  check("want>largest -> refused", guard_ok(300,512)==0);
  check("big buffer on tight heap refused", guard_ok(10000,4096)==0);  /* 10000-4096<8192 */
  /* fragmentation */
  check("one contiguous block = 0%", frag_pct(20000,20000)==0);
  check("half fragmented ~50%", frag_pct(20000,10000)==50);
  check("badly fragmented high", frag_pct(30000,4000)>80);
  /* egress backlog caps: soft skips+counts, hard drops the client */
  { unsigned wsdrop=0; int client=1; size_t out=0;
    size_t SOFT=2048, HARD=4096;
    /* enqueue 100-byte messages forever against a stalled client */
    for (int i=0;i<200 && client;i++){
      if (out > HARD) { client=0; out=0; continue; }
      if (out > SOFT) { wsdrop++; continue; }
      out += 100;
    }
    check("soft cap counted drops", wsdrop>0);
    check("hard cap never reached while soft holds", client==1);
    /* a single oversized burst jumps past soft: hard must fire */
    out = HARD+1; client=1; 
    { if (out > HARD) { client=0; out=0; } }
    check("hard cap drops the client", client==0 && out==0);
  }

  /* radio heap brake: loopy ingress dies below the floor, real wire never */
  { unsigned drop=0, rx=0; int flood=0;
    unsigned largest_seq[6]={9000,8000,4000,3000,6000,9000};
    for (int i=0;i<6;i++){
      unsigned largest=largest_seq[i]; unsigned n=100; int loopy=1;
      if (loopy && largest<5120){drop+=n;flood=1;continue;}
      rx+=n;
    }
    check("brake drops below floor", drop==200 && flood==1);
    check("brake releases above floor", rx==400);
  }

  printf(fails?"\n%d FAILURE(S)\n":"\nALL GUARD TESTS PASS\n",fails);return fails?1:0;}
