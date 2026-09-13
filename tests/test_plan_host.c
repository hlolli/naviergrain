/* Exercise the private plan opcode through the actual native Csound API. */
#include <csound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"plan host %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static CSOUND *prepare(const char *module,int plan,unsigned block,unsigned frames) {
 CSOUND *h=csoundCreate(NULL,NULL);CHECK(h);
 char option[4096];snprintf(option,sizeof(option),"--opcode-lib=%s",module);CHECK(!csoundSetOption(h,option));
 char csd[8192],instruction[512];
 snprintf(instruction,sizeof(instruction),"kAddress, kStats[] naviergrain_plan giSource, sr, iConfig, kControl, %u\nchnset kAddress, \"address\"",frames);
 snprintf(csd,sizeof(csd),"<CsoundSynthesizer>\n<CsOptions>\n-n -d -m0\n</CsOptions>\n<CsInstruments>\n"
  "sr=48000\nksmps=%u\nnchnls=2\n0dbfs=1\n#include \"include/naviergrain.inc\"\n"
  "giSource ftgen 1, 0, -997, 10, 1, .2, .1\ninstr 1\n"
  "iConfig[] fillarray $NG_CONFIG_DEFAULTS\niConfig[$NG_CONFIG_GRID_SIZE]=16\niConfig[$NG_CONFIG_MAX_GRAINS]=128\n"
  "iConfig[$NG_CONFIG_SOURCE_LOOP]=1\nkControl[] fillarray $NG_CONTROL_DEFAULTS\n"
  "kControl[$NG_CONTROL_GRAIN_RATE] init 800\nkControl[$NG_CONTROL_GAIN] init .15\n"
  "kControl[$NG_CONTROL_RESET] chnget \"reset\"\n%s\nendin\n</CsInstruments>\n"
  "<CsScore>\ni 1 0 -1\nf 0 z\n</CsScore>\n</CsoundSynthesizer>\n",block,
  plan?instruction:
       "kRun chnget \"run\"\nif kRun==1 then\naL,aR,kStats[] naviergrain giSource,sr,iConfig,kControl\nouts aL,aR\nendif");
 CHECK(!csoundCompileCSD(h,csd,1,0));CHECK(!csoundStart(h));
 return h;
}
static uint32_t *mailbox(CSOUND *h) {
 int32_t error=0;double address=csoundGetControlChannel(h,"address",&error);
 CHECK(!error&&isfinite(address)&&address>0);
 uint32_t *m=(uint32_t *)(uintptr_t)address;CHECK(m[0]==0x4d50474eu&&m[1]==2);return m;
}
static void command(CSOUND *h,uint32_t *m,unsigned action,int valid) {
 m[3]=action;CHECK(!csoundPerformKsmps(h));CHECK((m[4]==0)==valid);
}
int main(int argc,char **argv) {
 CHECK(argc==2);csoundInitialize(CSOUNDINIT_NO_SIGNAL_HANDLER|CSOUNDINIT_NO_ATEXIT);
 for(unsigned cycle=0;cycle<3;++cycle) {
  unsigned frames=cycle==0?32:cycle==1?128:512;
  CSOUND *reference=prepare(argv[1],0,32,32),*split=prepare(argv[1],1,32,frames);
  CHECK(!csoundPerformKsmps(reference)&&!csoundPerformKsmps(split));
  uint32_t *m=mailbox(split);CHECK(m[6]==0&&m[9]>0&&(m[21]&1));
  double *output=(double *)((unsigned char *)m+m[11]);double peak=0;
  for(unsigned block=0;block<48;++block) {
   int reset=block>=20&&block<22;
   csoundSetControlChannel(reference,"reset",reset);csoundSetControlChannel(split,"reset",reset);
   csoundSetControlChannel(reference,"run",1);double expected[1024];
   for(unsigned f=0;f<frames;f+=32){CHECK(!csoundPerformKsmps(reference));memcpy(expected+2*f,csoundGetSpout(reference),64*sizeof(double));}
   /* Alternate packed and C-only plans. CPU capture must leave packet
    * storage untouched, including after a previous packed delivery. */
   unsigned action=block%2?4:1;
   unsigned char *packet=(unsigned char *)m+m[10];
   unsigned char sentinel[64];memset(sentinel,0xa5,sizeof(sentinel));
   if(action==4)memcpy(packet,sentinel,sizeof(sentinel));
   m[5]=frames;command(split,m,action,1);CHECK(m[6]==1&&m[16]==block*frames);
   if(action==4){CHECK(m[7]==0);CHECK(!memcmp(packet,sentinel,sizeof(sentinel)));}
   else {CHECK(m[7]>=192);CHECK(m[22]==((uint32_t *)packet)[8]&&m[23]==((uint32_t *)packet)[9]);}
   CHECK(m[23]<=128&&m[22]<=128*frames);
   CHECK(m[12]==block+1&&m[13]==0);
   command(split,m,1,0);command(split,m,4,0); /* cannot advance unresolved audio */
   m[14]=m[12]-1;m[15]=0;command(split,m,2,0);
   m[14]=m[12];command(split,m,2,1);double retry[1024];memcpy(retry,output,frames*2*sizeof(double));
   command(split,m,2,1);CHECK(!memcmp(retry,output,frames*2*sizeof(double)));
   command(split,m,3,1);CHECK(!m[6]);CHECK(!memcmp(expected,output,frames*2*sizeof(double)));
   command(split,m,3,0);
   for(unsigned i=0;i<frames*2;++i)peak=fmax(peak,fabs(output[i]));
  }
  CHECK(peak>.001);m[5]=0;command(split,m,1,0);m[5]=frames+1;command(split,m,1,0);
  m[5]=7;command(split,m,1,1);m[14]=m[12];command(split,m,2,1);
  /* Destroy with an unresolved plan, then create a genuinely fresh session. */
  csoundDestroy(split);csoundDestroy(reference);
 }
 CSOUND *invalid=prepare(argv[1],1,64,32);CHECK(!csoundPerformKsmps(invalid));
 int32_t error=0;CHECK(csoundGetControlChannel(invalid,"address",&error)==0);
 for(unsigned i=0;i<128;++i)CHECK(csoundGetSpout(invalid)[i]==0);
 csoundDestroy(invalid);
 puts("native plan binding: 144 exact batches at 32/128/512 frames, packed/C-only capture, tickets/retry, reset, pending teardown, invalid block size");return 0;
}
