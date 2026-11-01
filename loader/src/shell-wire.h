/* Dedicated 256-byte hostname mailbox, pinned kernel prototype only.
 * Frame type + 4 hex sequence + ':' + payload + ':' + FNV32 checksum. */
static unsigned wire_len(const char *s) {unsigned n=0;while(s[n])n++;return n;}
static char wire_hex(unsigned n) {return "0123456789abcdef"[n&15];}
static unsigned wire_digit(char c) {return c>='a'?c-'a'+10:c-'0';}
static unsigned wire_hash(const char *p,unsigned n) {unsigned h=2166136261u;while(n--)h=(h^(unsigned char)*p++)*16777619u;return h;}
static void wire_make(char *b,char type,unsigned seq,const char *p) {
 b[0]=type;for(unsigned i=0;i<4;i++)b[i+1]=wire_hex(seq>>(12-4*i));b[5]=':';
 unsigned n=6;while(*p && n<245)b[n++]=*p++;unsigned h=wire_hash(b,n);b[n++]=':';
 for(unsigned i=0;i<8;i++)b[n++]=wire_hex(h>>(28-i*4));
 b[n]=0;
}
static int wire_valid(const char *b) {
 unsigned n=0;while(n<256 && b[n])n++;if(n<15 || n==256 || b[5]!=':' || b[n-9]!=':')return 0;
 unsigned h=0;for(unsigned i=n-8;i<n;i++) {if(!((b[i]>='0'&&b[i]<='9')||(b[i]>='a'&&b[i]<='f')))return 0;h=(h<<4)|wire_digit(b[i]);}
 return h==wire_hash(b,n-9);
}
static unsigned wire_seq(const char *b) {unsigned v=0;for(unsigned i=1;i<5;i++)v=(v<<4)|wire_digit(b[i]);return v;}
