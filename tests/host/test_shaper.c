/* traffic shaper: fractional token buckets (ingress policer + egress shaper)
 *
 * MIRROR WARNING (LESSONS rule): the two laws below are line-for-line copies
 * of WebSerial::egress_room_/egress_spend_ and WebSerial::port_rate_ok_ in
 * components/web_serial/web_serial.cpp. If you change the law there, change
 * it HERE, and vice versa. The cpp carries the same cross-reference comment.
 *
 * The regression under test (field-observed): the old refill computed
 * add = cap*dt/1000 with integer division and advanced the timestamp even
 * when add truncated to 0. Any rate below ~1000/pass_period B/s starved to
 * ZERO ('out 10' on a hairpin = dead wire, tx frozen, drops exploding).
 * The fixed law keeps the truncation remainder in a milli-token
 * accumulator: long-run delivery equals the configured number, exactly.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
static int fails=0;
static void check(const char*n,int ok){printf("  [%s] %s\n",ok?"PASS":"FAIL",n);if(!ok)fails++;}

typedef struct {
  uint32_t out_cap, out_tok, out_tok_ms, out_acc;
  uint32_t rate_cap, tok, tok_ms, rate_acc;
} Port;

/* ---- egress law (mirror of egress_room_ / egress_spend_) ---- */
static size_t egress_room(Port*p, uint32_t now){
  if(p->out_cap==0) return (size_t)-1;
  if(p->out_tok_ms==0){ p->out_tok_ms=now; p->out_tok=p->out_cap; p->out_acc=0; }
  uint32_t dt = now - p->out_tok_ms;            /* unsigned: wrap-safe */
  if(dt){
    if(dt>=1000){ p->out_tok=p->out_cap; p->out_acc=0; }
    else {
      uint64_t a=(uint64_t)p->out_cap*dt + p->out_acc;
      uint32_t whole=(uint32_t)(a/1000);
      p->out_acc=(uint32_t)(a%1000);
      uint32_t cap=p->out_cap;
      p->out_tok=(p->out_tok+whole>cap)?cap:(p->out_tok+whole);
    }
    p->out_tok_ms=now;
  }
  return p->out_tok;
}
static void egress_spend(Port*p,size_t n){
  if(p->out_cap==0) return;
  p->out_tok = n>=p->out_tok ? 0 : (uint32_t)(p->out_tok-n);
}

/* ---- ingress law (mirror of port_rate_ok_) ---- */
static int rate_ok(Port*p,size_t n,uint32_t now){
  if(p->rate_cap==0) return 1;
  if(p->tok_ms==0){ p->tok_ms=now; p->tok=p->rate_cap; p->rate_acc=0; }
  uint32_t dt = now - p->tok_ms;
  if(dt>0){
    if(dt>=1000){ p->tok=p->rate_cap; p->rate_acc=0; }
    else {
      uint64_t a=(uint64_t)p->rate_cap*dt + p->rate_acc;
      uint32_t whole=(uint32_t)(a/1000);
      p->rate_acc=(uint32_t)(a%1000);
      uint32_t cap=p->rate_cap;
      p->tok=(p->tok+whole>cap)?cap:(p->tok+whole);
    }
    p->tok_ms=now;
  }
  if(p->tok>=n){ p->tok-=(uint32_t)n; return 1; }
  if(n>p->rate_cap && p->tok>=p->rate_cap){ p->tok=0; return 1; }
  return 0;
}

/* drive the egress bucket like port_wire_drain_ does: every 'period' ms,
   drain min(room, backlog) from an infinite backlog. Returns bytes let out
   AFTER the initial armed bucket (the burp is measured separately). */
static uint64_t run_egress(Port*p,uint32_t cap,uint32_t period,uint32_t ms,uint32_t start){
  p->out_cap=cap; p->out_tok=0; p->out_tok_ms=0; p->out_acc=0;
  uint32_t now=start; uint64_t sent=0;
  size_t r0=egress_room(p,now); egress_spend(p,r0);   /* arm + swallow the burp */
  for(uint32_t t=period;t<=ms;t+=period){
    now=start+t;
    size_t room=egress_room(p,now);
    if(room){ egress_spend(p,room); sent+=room; }     /* backlog always > room */
  }
  return sent;
}

int main(void){
  Port p={0};

  /* THE regression: cap=10, drained every 3 ms (the flooded-hairpin case).
     Old law: 10*3/1000==0 forever -> 0 bytes. Fixed law: 600 +/- bucket. */
  uint64_t got=run_egress(&p,10,3,60000,5);
  check("out 10 B/s at 3 ms cadence delivers ~600 B/min (was 0)", got>=590 && got<=610);

  /* cap=1: the harshest quantization -- exactly 1 B/s */
  got=run_egress(&p,1,3,60000,5);
  check("out 1 B/s is exact over a minute", got>=59 && got<=61);

  /* cap=250 at 7 ms: the systematic-underdelivery case (was ~143 B/s) */
  got=run_egress(&p,250,7,60000,5);
  check("out 250 B/s at 7 ms is exact (was ~57% under)", got>=14800 && got<=15100);

  /* cap=9600 at 5 ms: a real wire speed, high-rate sanity */
  got=run_egress(&p,9600,5,10000,5);
  check("out 9600 B/s at 5 ms is exact", got>=95500 && got<=96500);

  /* never exceeds the asked number: hard upper bound cap*t + one bucket */
  got=run_egress(&p,100,3,60000,5);
  check("delivery never exceeds cap*t + bucket", got<=6000u+100u);

  /* idle burst: after 5 s of silence, at most ONE full bucket comes out */
  p.out_cap=50; p.out_tok=0; p.out_tok_ms=0; p.out_acc=0;
  (void)egress_room(&p,1000); egress_spend(&p,egress_room(&p,1000));
  size_t burst=egress_room(&p,6000);           /* 5 s later */
  check("5 s idle refills ONE bucket, not five", burst==50);

  /* millis() wrap: a delta crossing 0xFFFFFFFF must not mint phantom tokens */
  p.out_cap=100; p.out_tok=0; p.out_tok_ms=0; p.out_acc=0;
  (void)egress_room(&p,0xFFFFFFF0u); egress_spend(&p,egress_room(&p,0xFFFFFFF0u));
  size_t after=egress_room(&p,0x00000004u);    /* 20 ms across the wrap */
  check("millis wrap: 20 ms credits 2 tokens, no phantom bucket", after==2);

  /* accumulator actually carries: 3 polls of 3 ms at cap=100 = 0.9 tokens
     released as 0,0,0 then the 4th poll (12 ms total) crosses 1.2 -> 1 */
  p.out_cap=100; p.out_tok=0; p.out_tok_ms=0; p.out_acc=0;
  (void)egress_room(&p,10); egress_spend(&p,egress_room(&p,10));
  size_t s1=egress_room(&p,13), s2=egress_room(&p,16), s3=egress_room(&p,19), s4=egress_room(&p,22);
  check("remainder survives across zero-yield polls", s1==0&&s2==0&&s3==0&&s4==1);

  /* ---- ingress policer ---- */
  /* cap=10, 4-byte frames offered every 3 ms for 60 s: ~150 accepted (600 B) */
  p.rate_cap=10; p.tok=0; p.tok_ms=0; p.rate_acc=0;
  { uint64_t acc_bytes=0; uint32_t now=5;
    (void)rate_ok(&p,0,now);                    /* arm */
    p.tok=0;                                    /* swallow the armed bucket */
    for(uint32_t t=3;t<=60000;t+=3){ if(rate_ok(&p,4,5+t)) acc_bytes+=4; }
    check("rate 10 B/s polices 4 B frames to ~600 B/min (was: all dropped)", acc_bytes>=584 && acc_bytes<=616);
  }
  /* oversize mercy clause still alive: a 64 B frame vs cap 10 passes on a full bucket */
  p.rate_cap=10; p.tok=0; p.tok_ms=0; p.rate_acc=0;
  (void)rate_ok(&p,0,100);                      /* arm: tok=10 */
  check("oversize frame passes when the bucket is full", rate_ok(&p,64,100)==1);
  check("and it emptied the bucket", p.tok==0);
  check("second oversize is refused (bucket not full)", rate_ok(&p,64,101)==0);

  /* unlimited stays unlimited */
  p.out_cap=0; check("out 0 = unlimited untouched", egress_room(&p,123)==(size_t)-1);
  p.rate_cap=0; check("rate 0 = unlimited untouched", rate_ok(&p,99999,123)==1);

  if(fails){ printf("%d FAILURE(S)\n",fails); return 1; }
  printf("ALL SHAPER TESTS PASS\n"); return 0;
}
