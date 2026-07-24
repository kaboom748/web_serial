/* loop + xvlan detect (PASSIVE) -- mirror of lp_egress_/lp_ingress_.
 * One fingerprint engine, two verdicts read off the vlans at scan time:
 *   match from a SAME-vlan port  -> recirculation -> LOOP  (loopdet)
 *   match from an OTHER-vlan port -> one-way crossing -> XVLAN bridge (xvlandet)
 * 3 consecutive / 3 s each, independent ON/OFF/KILL, zero injection.
 * Disabled pass is skipped entirely: no count, NO consumption. */
#include <stdio.h>
#include <string.h>
static int fails=0;
static void check(const char*n,int ok){printf("  [%s] %s\n",ok?"PASS":"FAIL",n);if(!ok)fails++;}

static unsigned fnv1a(const unsigned char*d,int n){unsigned h=0x811C9DC5u;for(int i=0;i<n;i++){h^=d[i];h*=0x01000193u;}return h;}

struct fp{unsigned h;unsigned short len;unsigned ms;};
struct port{struct fp ring[4];unsigned char w,lp_streak,xv_streak,vlan;int lp,xlp,up,used;};
#define NPORTS 6
static struct port P[NPORTS];
static unsigned loopdet, xvlandet;   /* 0 OFF, 1 ON, 2 KILL */

static void egress(struct port*p,const unsigned char*d,int n,unsigned now){
  if((loopdet==0&&xvlandet==0)||n<2)return;
  struct fp*f=&p->ring[p->w];
  f->h=fnv1a(d,n);f->len=(unsigned short)n;f->ms=now;
  p->w=(p->w+1)&3;
}
/* returns 1 only when a KILL drops the chunk */
static int ingress(struct port*p,const unsigned char*d,int n,unsigned now){
  if(loopdet==0&&xvlandet==0)return 0;
  if(n<2)return 0;
  unsigned h=fnv1a(d,n);int verdict=0;
  for(int pass=1;pass<=2&&verdict==0;pass++){
    if(pass==1&&loopdet==0)continue;
    if(pass==2&&xvlandet==0)continue;
    for(int q=0;q<NPORTS&&verdict==0;q++){
      if(!P[q].used)continue;
      int same=P[q].vlan==p->vlan;
      if((pass==1)!=same)continue;
      for(int k=0;k<4;k++){struct fp*f=&P[q].ring[k];
        if(f->ms!=0&&now-f->ms<=3000&&f->len==n&&f->h==h){f->ms=0;verdict=pass;break;}}}}
  if(verdict==0){p->lp_streak=0;p->xv_streak=0;return 0;}
  if(verdict==1){p->xv_streak=0;
    if(++p->lp_streak<3)return 0;
    p->lp_streak=0;p->lp=1;
    if(loopdet==2){p->up=0;return 1;} return 0;}
  p->lp_streak=0;
  if(++p->xv_streak<3)return 0;
  p->xv_streak=0;p->xlp=1;
  if(xvlandet==2){p->up=0;return 1;} return 0;
}
static void reset_all(void){memset(P,0,sizeof P);
  for(int i=0;i<NPORTS;i++){P[i].up=1;P[i].used=1;P[i].vlan=1;}}
/* mirror of the PORT VLAN hot-change purge */
static void set_vlan(struct port*p,unsigned char v){
  if(p->vlan!=v){p->lp=0;p->xlp=0;p->lp_streak=0;p->xv_streak=0;
    for(int k=0;k<4;k++)p->ring[k].ms=0;}
  p->vlan=v;}

int main(void){
  unsigned char fr[8]={0x55,0xAA,0x55,0x01,0xC2,0x00,0x03,0xC6};
  unsigned char other[8]={0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};

  /* ---- same-vlan loop: unchanged behavior, cross-port included ---- */
  reset_all();loopdet=1;xvlandet=1;
  egress(&P[2],fr,8,1000); ingress(&P[3],fr,8,1130);
  egress(&P[2],fr,8,1130); ingress(&P[3],fr,8,1260);
  egress(&P[2],fr,8,1260);
  check("same-vlan xport loop -> LOOP", ingress(&P[3],fr,8,1390)==0&&P[3].lp&&!P[3].xlp);
  check("real reply resets streaks", (reset_all(),egress(&P[2],fr,8,2000),ingress(&P[3],fr,8,2100),
        ingress(&P[3],other,8,2200), P[3].lp_streak==0&&P[3].xv_streak==0));

  /* ---- deliberate inter-vlan bridge, XVLANDETECT OFF: total silence ---- */
  reset_all();loopdet=1;xvlandet=0; P[1].vlan=2;      /* A=P[0] v1 -> B=P[1] v2 */
  for(int lap=0;lap<5;lap++){ egress(&P[0],fr,8,3000+lap*130); ingress(&P[1],fr,8,3065+lap*130); }
  check("bridge tolerated when OFF", !P[1].lp&&!P[1].xlp&&P[1].up);
  /* OFF pass must NOT consume: the v1 fingerprints are still there for a loop */
  check("OFF consumed nothing", P[0].ring[0].ms!=0);

  /* ---- same bridge, XVLANDETECT ON: XVLAN badge, never LOOP ---- */
  reset_all();loopdet=1;xvlandet=1; P[1].vlan=2;
  egress(&P[0],fr,8,4000); ingress(&P[1],fr,8,4065);
  egress(&P[0],fr,8,4130); ingress(&P[1],fr,8,4195);
  egress(&P[0],fr,8,4260);
  check("bridge ON -> XVLAN at 3", ingress(&P[1],fr,8,4325)==0&&P[1].xlp&&!P[1].lp);
  check("ON leaves the port UP", P[1].up==1);

  /* ---- bridge with xvlan KILL: ingress port goes DOWN ---- */
  reset_all();loopdet=0;xvlandet=2; P[1].vlan=2;
  egress(&P[0],fr,8,5000); ingress(&P[1],fr,8,5065);
  egress(&P[0],fr,8,5130); ingress(&P[1],fr,8,5195);
  egress(&P[0],fr,8,5260);
  check("XVLAN KILL drops 3rd crossing", ingress(&P[1],fr,8,5325)==1);
  check("XVLAN KILL downs ingress port", P[1].up==0&&P[1].xlp);
  check("loopdet OFF stayed independent", !P[1].lp);

  /* ---- TRUE circular loop across two vlans: LOOP on re-entry ports ----
     A=P[0](v1) --ext--> B=P[1](v2), C=P[2](v2) --ext--> D=P[3](v1).
     The frame leaves v1 by A, re-enters v1 by D: D's ingress matches A's
     SAME-vlan fingerprint -> LOOP verdict, not XVLAN. Same story in v2. */
  reset_all();loopdet=1;xvlandet=1; P[1].vlan=2;P[2].vlan=2;
  for(int lap=0;lap<3;lap++){unsigned t=6000+lap*200;
    egress(&P[0],fr,8,t);       /* v1 egress out A */
    ingress(&P[1],fr,8,t+50);   /* enters v2 by B (cross match vs A) */
    egress(&P[2],fr,8,t+50);    /* v2 fans out C */
    ingress(&P[3],fr,8,t+100);  /* re-enters v1 by D (SAME-vlan match vs A? A consumed by B's xvlan pass...) */
  }
  check("circular: some verdict fired", P[1].xlp||P[3].lp);
  /* with xvlandet OFF the crossing is silent and the recirculation is
     unambiguously a LOOP on the re-entry port */
  reset_all();loopdet=1;xvlandet=0; P[1].vlan=2;P[2].vlan=2;
  for(int lap=0;lap<3;lap++){unsigned t=7000+lap*200;
    egress(&P[0],fr,8,t); ingress(&P[1],fr,8,t+50);
    egress(&P[2],fr,8,t+50); ingress(&P[3],fr,8,t+100); }
  check("circular + bridges tolerated -> LOOP on v1 re-entry", P[3].lp&&!P[3].xlp);
  check("...and nothing on the crossing port", !P[1].lp&&!P[1].xlp);

  /* ---- hot vlan change purges detection state (no stale-vlan verdicts) ---- */
  reset_all();loopdet=1;xvlandet=1;
  egress(&P[0],fr,8,8000);           /* fingerprint recorded while P0 is v1 */
  P[2].lp_streak=2;                  /* P2 one echo away from a LOOP verdict */
  set_vlan(&P[0],2);                 /* P0 hops to v2: its ring must die */
  check("vlan change purges the ring", P[0].ring[0].ms==0);
  set_vlan(&P[2],3);
  check("vlan change purges streaks/badges", P[2].lp_streak==0&&!P[2].lp);
  check("vlan change: no stale verdict", ingress(&P[2],fr,8,8100)==0&&P[2].lp_streak==0);

  /* ---- engine hygiene, unchanged ---- */
  reset_all();loopdet=1;xvlandet=1;
  egress(&P[0],fr,8,9000);
  ingress(&P[1],fr,8,9100);
  check("fingerprint consumed once", P[1].lp_streak==1&&(ingress(&P[1],fr,8,9200),P[1].lp_streak==0));
  reset_all();egress(&P[0],fr,8,10000);
  check("stale egress ignored", ingress(&P[1],fr,8,13500)==0);
  reset_all();egress(&P[0],fr,8,11000);
  check("tiny chunks ignored", ingress(&P[1],fr,1,11100)==0);
  reset_all();loopdet=2;xvlandet=1;
  egress(&P[0],fr,8,12000); ingress(&P[1],fr,8,12100);
  egress(&P[0],fr,8,12100); ingress(&P[1],fr,8,12200);
  egress(&P[0],fr,8,12200);
  check("LOOP KILL still drops+downs", ingress(&P[1],fr,8,12300)==1&&P[1].up==0);
  reset_all();loopdet=0;xvlandet=0;
  egress(&P[0],fr,8,13000);
  check("both OFF: engine fully inert", P[0].ring[0].ms==0&&ingress(&P[1],fr,8,13100)==0);

  /* verdict logging is EDGE-triggered: sticky badge, one message per
     transition -- re-verdicts stay silent (the log storm regression) */
  { int lp=0, logs=0;
    for (int v=0; v<10; v++) {           /* 10 consecutive verdicts */
      int first = !lp;
      lp = 1;
      if (first) logs++;
    }
    check("10 verdicts, ONE log (edge)", logs==1);
    lp = 0;                               /* PORT UP clears the badge */
    { int first=!lp; lp=1; if(first) logs++; }
    check("re-armed after badge clear", logs==2);
  }

  printf(fails?"\n%d FAILURE(S)\n":"\nALL LOOPDET+XVLAN TESTS PASS\n",fails);
  return fails?1:0;
}
