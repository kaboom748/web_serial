/* switch core: VLAN forwarding, never-to-origin, up/down, token bucket, table */
#include <stdio.h>
#include <string.h>
static int fails=0;
static void check(const char*n,int ok){printf("  [%s] %s\n",ok?"PASS":"FAIL",n);if(!ok)fails++;}
#define NP 8
typedef struct{int used,type,vlan,up;unsigned rate,tok,tok_ms,rate_acc,tx,rx,drop;}Port;
static Port ports[NP];
static int deliver[NP];  /* test sink: how many bytes each port received */

static int rate_ok(Port*p,unsigned n,unsigned now){
  if(p->rate==0)return 1;
  if(p->tok_ms==0){p->tok_ms=now;p->tok=p->rate;p->rate_acc=0;}
  unsigned dt=now-p->tok_ms;
  /* fractional refill -- mirror of port_rate_ok_ (authoritative copy with
     full coverage: test_shaper.c). The old floor(rate*dt/1000) law starved
     small caps; the remainder now survives in rate_acc. */
  if(dt){
    if(dt>=1000){p->tok=p->rate;p->rate_acc=0;}
    else{unsigned long long a=(unsigned long long)p->rate*dt+p->rate_acc;
      unsigned whole=(unsigned)(a/1000);p->rate_acc=(unsigned)(a%1000);
      p->tok=(p->tok+whole>p->rate)?p->rate:p->tok+whole;}
    p->tok_ms=now;}
  if(p->tok>=n){p->tok-=n;return 1;}
  /* oversize chunk passes when the bucket is full (idle line), emptying it */
  if(n>p->rate&&p->tok>=p->rate){p->tok=0;return 1;}
  return 0;}
static void ingress(int src,unsigned n,unsigned now){
  Port*sp=&ports[src];
  if(!sp->up)return;
  if(!rate_ok(sp,n,now)){sp->drop+=n;return;}
  sp->rx+=n;
  for(int i=0;i<NP;i++){Port*q=&ports[i];
    if(!q->used||i==src||!q->up||q->vlan!=sp->vlan)continue;
    q->tx+=n;deliver[i]+=n;}}

/* cons_ring mirror (TRUTHCONS): variable records [src][len][bytes], loss
   record [0xFF][4][u32-LE]; a pending gap is confessed FIRST. */
static unsigned char R[1024]; static unsigned short W=0, Rr=0; static unsigned gap=0;
static unsigned short cfree(void){ return (unsigned short)((Rr - W - 1) & 1023); }
static void cpush(unsigned char b){ R[W]=b; W=(unsigned short)((W+1)&1023); }
static void crec(unsigned char src, const unsigned char *d, unsigned n){
  if (gap>0 && cfree()>=6){ unsigned g=gap; gap=0; cpush(0xFF);cpush(4);
    cpush((unsigned char)g);cpush((unsigned char)(g>>8));cpush((unsigned char)(g>>16));cpush((unsigned char)(g>>24)); }
  while(n>0){ unsigned c=n>64?64:n;
    if(cfree()<c+2){ gap+=n; return; }
    cpush(src); cpush((unsigned char)c);
    for(unsigned k=0;k<c;k++)cpush(d[k]); d+=c; n-=c; } }
static void test_cons_ring_mirror(void){
  unsigned char d[130]; for(int i=0;i<130;i++)d[i]=(unsigned char)i;
  crec(2,d,130);
  int ok = (R[0]==2 && R[1]==64 && R[66]==2 && R[67]==64 && R[132]==2 && R[133]==2);
  if(ok) printf("  [PASS] cons ring: 130 B -> records of 64+64+2\n");
  else { printf("  [FAIL] cons ring chunking\n"); fails++; }
  while(cfree()>=66) crec(3,d,64);          /* fill to the brim */
  crec(3,d,64);                              /* overflow -> gap */
  unsigned g1=gap;
  /* consume ONE record to free space, then push: the confession lands at
     the WRITE head -- first among the NEW records, exactly where the hole
     happened (index fix: the old check looked at the READ cursor). */
  unsigned short r2=(unsigned short)((Rr+2+R[(Rr+1)&1023])&1023); Rr=r2;
  unsigned short wpre=W;
  crec(4,d,1);
  int ok2 = (g1==64 && R[wpre]==0xFF && R[(wpre+1)&1023]==4 &&
             R[(wpre+2)&1023]==64 && R[(wpre+3)&1023]==0 && gap==0 &&
             R[(wpre+6)&1023]==4 && R[(wpre+7)&1023]==1);
  if(ok2) printf("  [PASS] cons ring: the gap is confessed FIRST (0xFF, 64 LE)\n");
  else { printf("  [FAIL] cons ring loss record (gap=%u first=%02X)\n", g1, R[r2]); fails++; }
}

/* drain mirror (CONSTRUTH): the verdict comes from send(), NEVER from
   buffer-size arithmetic -- in the field a synchronous flush made
   size-equality read success as refusal: the same gap re-confessed every
   pass (the [lost N] wall), records frozen, everything overflowed. */
static unsigned emitted_g[8]; static int emits=0;
static int drain2(int send_ok){
  int adv=0, budget=4;
  if(gap>0){ if(!send_ok) return 0; emitted_g[emits++]=gap; gap=0; }
  while(budget-->0 && Rr!=W){
    unsigned char len=R[(unsigned short)((Rr+1)&1023)];
    if(!send_ok) return adv;
    Rr=(unsigned short)((Rr+2+len)&1023); adv++;
  }
  return adv;
}
static void test_construth_drain(void){
  unsigned char d[8]={1,2,3,4,5,6,7,8};
  W=Rr=0; gap=0; emits=0;
  crec(3,d,8); crec(4,d,4);
  gap=100;                                     /* episode 1 */
  unsigned short r0=Rr;
  int a=drain2(0);
  if(a==0 && gap==100 && Rr==r0) printf("  [PASS] construth: refusal keeps the debt and the records\n");
  else { printf("  [FAIL] construth: refusal semantics\n"); fails++; }
  drain2(1);
  if(emits==1 && emitted_g[0]==100 && gap==0 && Rr==W) printf("  [PASS] construth: success confesses once, drains records\n");
  else { printf("  [FAIL] construth: success semantics (emits=%d)\n", emits); fails++; }
  drain2(1);
  if(emits==1) printf("  [PASS] construth: no debt, no new confession\n");
  else { printf("  [FAIL] construth: stutter is back (emits=%d)\n", emits); fails++; }
  gap=50;                                      /* episode 2 */
  drain2(1);
  if(emits==2 && emitted_g[1]==50) printf("  [PASS] construth: episodes stay distinct, never cumulative\n");
  else { printf("  [FAIL] construth: episode 2\n"); fails++; }
  W=Rr=0; gap=0; emits=0;   /* leave the shared mirror state virgin */
}

int main(void){
  test_cons_ring_mirror();
  memset(ports,0,sizeof ports);memset(deliver,0,sizeof deliver);
  /* 4 ports: 0,1 in vlan1; 2,3 in vlan2 */
  for(int i=0;i<4;i++){ports[i].used=1;ports[i].up=1;ports[i].vlan=(i<2)?1:2;}
  ingress(0,10,1000);
  check("vlan1 peer received", deliver[1]==10);
  check("never to origin", deliver[0]==0);
  check("vlan2 isolated", deliver[2]==0&&deliver[3]==0);
  memset(deliver,0,sizeof deliver);
  ingress(2,5,2000);
  check("vlan2 peer received", deliver[3]==5);
  check("vlan1 does not hear vlan2", deliver[0]==0&&deliver[1]==0);
  /* down port: no forward in, no forward out */
  memset(deliver,0,sizeof deliver);
  ports[1].up=0; ingress(0,7,3000);
  check("down port receives nothing", deliver[1]==0);
  ports[1].up=1; ports[0].up=0; memset(deliver,0,sizeof deliver);
  ingress(0,7,4000);
  check("down source forwards nothing", deliver[1]==0);
  ports[0].up=1;
  /* token bucket: cap 100 B/s, burst 100 ok then dropped within same second */
  ports[0].rate=100; ports[0].tok=0; ports[0].tok_ms=0;
  int ok1=rate_ok(&ports[0],80,10000);   /* first fills to 100, takes 80 -> ok */
  int ok2=rate_ok(&ports[0],80,10010);   /* 10ms later ~1 token added, 20 left -> deny */
  check("token bucket passes burst", ok1==1);
  check("token bucket blocks excess", ok2==0);
  /* table: alloc/free reuse lowest index */
  memset(ports,0,sizeof ports);
  ports[0].used=ports[1].used=1;
  int fr=-1; for(int i=0;i<NP;i++)if(!ports[i].used){fr=i;break;}
  check("first free is index 2", fr==2);
  ports[0].used=0;
  fr=-1; for(int i=0;i<NP;i++)if(!ports[i].used){fr=i;break;}
  check("after free, reuse index 0", fr==0);
  /* oversize policer: a 512B chunk on a 100 B/s cap must not livelock */
  memset(&ports[0],0,sizeof(Port));ports[0].used=1;ports[0].up=1;ports[0].rate=100;
  check("oversize passes on idle bucket", rate_ok(&ports[0],512,20000)==1);
  check("oversize emptied the bucket",    rate_ok(&ports[0],512,20010)==0);
  check("oversize passes again after refill", rate_ok(&ports[0],512,21100)==1);

  /* enqueue drop counter: dropping the whole old buffer counts the OLD size */
  { unsigned buf_n=100,drop=0,cap=100,n=150;
    unsigned need=buf_n+n>cap?buf_n+n-cap:0;
    if(need>=buf_n){drop+=buf_n;buf_n=0;}   /* count BEFORE zeroing (the old code added 0) */
    unsigned k=n>cap?cap:n; drop+=n-k;
    check("drop counts evicted backlog", drop==150); /* 100 evicted + 50 truncated head */
  }

  /* udp sendto failure run: success resets; 5th consecutive hard failure
     recreates the socket (mirror of the latched-ICMP recovery) */
  { unsigned txfail=0,txerr=0; int recreated=0;
    int results[9]={-1,-1,1,-1,-1,-1,-1,-1,-1}; /* fail fail OK then 6 fails */
    for(int i=0;i<9;i++){
      if(results[i]>0){txfail=0;continue;}
      txerr++; if(++txfail>=5){recreated=1;txfail=0;}
    }
    check("success resets the failure run", txerr==8);
    check("5 consecutive failures recreate", recreated==1);
    check("run counter cleared after recreate", txfail==1); /* the 9th fail restarts */
  }

  /* udp egress under network outage: gate freezes txfail; recreation arms
     the 5s creation backoff (never a tight recreate loop) */
  { int txfail=0, recreations=0, net=0; unsigned tok=0, tok_ms=0, now=1000;
    for (int pass=0; pass<200; pass++, now+=20) {
      int sock = 1;
      if (tok_ms!=0 && now-tok_ms<5000 && tok>=1) sock=0;  /* creation gated */
      if (!sock) continue;
      if (!net) continue;                    /* network gate: no sendto at all */
      if (++txfail>=5){recreations++;txfail=0;tok=1;tok_ms=now;}
    }
    check("network down: zero recreations", recreations==0 && txfail==0);
    net=1;                                    /* network back, peer still dead */
    for (int pass=0; pass<600; pass++, now+=20) {
      int sock = !(tok_ms!=0 && now-tok_ms<5000 && tok>=1);
      if (!sock) continue;
      if (++txfail>=5){recreations++;txfail=0;tok=1;tok_ms=now;}
    }
    check("hard-fail peer: recreations spaced (<=3 in 12 s)", recreations>=1 && recreations<=3);
  }

  /* CPU governor: per-pass budget + fairness cursor -- under permanent
     overload every port still gets served (round-robin), and each pass
     serves a bounded number of ports */
  { int served_count[8]={0}; int cursor=0; int PORTS=8;
    for (int pass=0; pass<80; pass++) {
      int budget = 3;                      /* only 3 ports fit per pass */
      for (int k=0; k<PORTS; k++) {
        int i=(cursor+k)%PORTS;
        served_count[i]++;
        if (--budget==0){cursor=(i+1)%PORTS;break;}
        if (k==PORTS-1) cursor=0;
      }
    }
    int mn=served_count[0],mx=served_count[0];
    for(int i=1;i<8;i++){if(served_count[i]<mn)mn=served_count[i];if(served_count[i]>mx)mx=served_count[i];}
    check("governor: nobody starves under overload", mn>0);
    check("governor: fair within one serving", mx-mn<=1);
    check("governor: pass work bounded (3x80 total)", mn*8<=3*80 && mx*8>=3*80-8);
  }

  /* duty pool: whatever the loop period, occupancy stays <= DUTY% */
  { unsigned pool=30000, worked=0, wall=0; int skipped=0;
    for (int pass=0; pass<2000; pass++) {
      unsigned period=1000;                 /* 1 ms loop: the fast regime */
      wall+=period;
      pool+=period*35/100; if(pool>30000)pool=30000;
      if(pool<2000){skipped++;continue;}
      unsigned spend=pool<20000?pool:20000; /* wants 20 ms, pool decides */
      worked+=spend; pool-=spend;
    }
    check("duty bounded ~35%", worked*100/wall <= 36);
    check("radio slack exists (phases skipped)", skipped > 0);
    check("work still flows", worked > 0);
  }

  /* egress shaper: a 9600 cap on a 19200 flow passes HALF, lazily */
  { unsigned cap=9600, tok=0, tok_ms=1000, sent=0, offered=0, now=1000;
    for (int pass=0; pass<100; pass++, now+=20) {           /* 2 s sim, pre-armed */
      unsigned want=384;                                    /* 19200 B/s in 20 ms */
      offered+=want;
      if (tok_ms==0){tok_ms=now;tok=cap;}
      unsigned dt=now-tok_ms; tok_ms=now;
      unsigned long add=(unsigned long)cap*dt/1000;
      tok=(unsigned)((tok+add)>cap?cap:tok+add);
      unsigned k=want<tok?want:tok; tok-=k; sent+=k;
    }
    check("shaper halves a double-rate flow", sent*100/offered>=45 && sent*100/offered<=55);
    check("shaper never exceeds its cap", sent<=cap*2+cap/10);
  }

  /* TRUNK wire laws: (1) the origin rule traverses the wire -- fanout from
     a wire injection NEVER returns into the pair (no ping-pong); (2) the
     drain is bounded by min(buf, room, 256); (3) bytes are conserved:
     enqueued = injected + evicted. Logic mirrors of port_wire_drain_. */
  { int A=5, B=6, SRC=1;
    int receivers[8], nrecv=0, wire_src=A;
    for (int i=0;i<8;i++){                 /* fanout at B: same-vlan ports */
      if(i==B) continue;                    /* origin */
      if(i==wire_src) continue;             /* the wire rule */
      receivers[nrecv++]=i;
    }
    int ping=0; for(int i=0;i<nrecv;i++) if(receivers[i]==A) ping=1;
    check("wire: no ping-pong back into the pair", ping==0 && nrecv==6);
    (void)SRC;
    unsigned buf=900, room=300, cap=256;
    unsigned k=buf; if(k>room)k=room; if(k>cap)k=cap;
    check("wire: drain bounded by min(buf,room,256)", k==256);
    unsigned enq=1000, inj=0, evict=0, bufn=0, bcap=256;
    for(unsigned x=0;x<enq;x+=50){ if(bufn+50>bcap){evict+=50;} else bufn+=50; }
    while(bufn){ unsigned d=bufn<256?bufn:256; inj+=d; bufn-=d; }
    check("wire: conservation ledger balances", inj+evict==enq);
  }

  /* HAIRPIN law: the self-wired port is the ONLY one allowed to receive
     its own origin -- ring of one; a normal port still never does. */
  { int recv_self_hairpin=0, recv_self_normal=0;
    for (int mode=0; mode<2; mode++) {
      int self=3, loop_peer = mode? self : 4;   /* mode1 = hairpin */
      int src=self, wire_src=self;
      /* fanout decision for i==self: skip unless self-wired */
      int skip_origin = (self==src && loop_peer!=self);
      int skip_wire   = (self==wire_src && loop_peer!=self);
      int received = !(skip_origin || skip_wire);
      if (mode) recv_self_hairpin=received; else recv_self_normal=received;
    }
    check("hairpin receives its own origin", recv_self_hairpin==1);
    check("normal wired port never does",    recv_self_normal==0);
  }

  test_construth_drain();
  printf(fails?"\n%d FAILURE(S)\n":"\nALL SWITCH TESTS PASS\n",fails);return fails?1:0;}
