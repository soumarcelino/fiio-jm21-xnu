/* Minimal static Darwin shell: quotes, $VAR/$?, pipes, < > >>, ; && || &,
 * builtins cd/exit/export. No job control: jobs share the shell's process group;
 * the shell ignores SIGINT/SIGQUIT and its children restore them, so ^C stops the
 * job only. (Moving the tty between groups stopped the shell with SIGTTOU on the
 * device.) No globals: state lives in struct sh. */
#include "ulib.h"
enum {W,PIPE,SEMI,AND,OR,AMP,LT,GT,GTGT};
struct sh {
 int status,tty;
 char *env[48];char envbuf[3072];unsigned envused;
 char line[1024];char words[2048];char *tok[96];char typ[96];unsigned ntok;
};
static const char *getenv_(struct sh *s,const char *name,unsigned n) {
 for(unsigned i=0;s->env[i];i++){const char *e=s->env[i];unsigned k=0;while(k<n&&e[k]==name[k])k++;if(k==n&&e[k]=='=')return e+n+1;}
 return "";
}
static void setenv_(struct sh *s,const char *kv) {
 unsigned n=0;while(kv[n]&&kv[n]!='=')n++;if(!kv[n])return;
 unsigned len=slen(kv)+1;if(s->envused+len>sizeof(s->envbuf))return;
 char *d=s->envbuf+s->envused;memcpy(d,kv,len);s->envused+=len;
 unsigned i=0;for(;s->env[i];i++){const char *e=s->env[i];unsigned k=0;while(k<n&&e[k]==kv[k])k++;if(k==n&&e[k]=='='){s->env[i]=d;return;}}
 if(i<47){s->env[i]=d;s->env[i+1]=0;}
}
/* Split s->line into words and operators; expand $NAME and $? outside single quotes. */
static int lex(struct sh *s) {
 char *p=s->line,*o=s->words,*end=s->words+sizeof(s->words)-64;s->ntok=0;
 while(*p) {
  while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
  if(!*p||*p=='#')break;
  if(s->ntok>=95)return -1;
  int t=-1;
  if(*p=='|'){t=p[1]=='|'?OR:PIPE;p+=t==OR?2:1;}
  else if(*p=='&'){t=p[1]=='&'?AND:AMP;p+=t==AND?2:1;}
  else if(*p==';'){t=SEMI;p++;}
  else if(*p=='<'){t=LT;p++;}
  else if(*p=='>'){t=p[1]=='>'?GTGT:GT;p+=t==GTGT?2:1;}
  if(t>=0){s->typ[s->ntok]=(char)t;s->tok[s->ntok++]=0;continue;}
  s->typ[s->ntok]=W;s->tok[s->ntok++]=o;char q=0;
  while(*p&&o<end) {
   char c=*p;
   if(!q&&(c==' '||c=='\t'||c=='\n'||c=='|'||c=='&'||c==';'||c=='<'||c=='>'))break;
   p++;
   if(q!='\''&&c=='\\'&&*p){*o++=*p++;continue;}
   if(!q&&(c=='\''||c=='"')){q=c;continue;}
   if(q&&c==q){q=0;continue;}
   if(q!='\''&&c=='$') {
    if(*p=='?'){p++;char b[12];unsigned i=11;b[i]=0;unsigned v=(unsigned)s->status;do{b[--i]=(char)('0'+v%10);v/=10;}while(v);for(char *x=b+i;*x&&o<end;)*o++=*x++;continue;}
    const char *n=p;while((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_')p++;
    if(p==n){*o++='$';continue;}
    for(const char *v=getenv_(s,n,(unsigned)(p-n));*v&&o<end;)*o++=*v++;
    continue;
   }
   *o++=c;
  }
  if(q)return -2;
  *o++=0;
 }
 return 0;
}
static int decode(long st,int report) {
 if((st&0x7f)==0)return (int)((st>>8)&0xff);
 if(report){put(2,"[signal ");putn(2,st&0x7f);put(2,"]\n");}
 return 128+(int)(st&0x7f);
}
static void exec_path(struct sh *s,char **argv) {
 char b[256];
 if(argv[0][0]=='/'||argv[0][0]=='.'){SC(SYS_execve,argv[0],argv,s->env,0,0,0);}
 else for(unsigned d=0;d<3;d++) {
  const char *dir=d==0?"/bin/":d==1?"/sbin/":"/usr/bin/";
  unsigned n=0;while(dir[n]){b[n]=dir[n];n++;}
  for(unsigned i=0;argv[0][i]&&n<255;i++){b[n++]=argv[0][i];}
  b[n]=0;
  SC(SYS_execve,b,argv,s->env,0,0,0);
 }
 put(2,"sh: ");put(2,argv[0]);put(2,": not found\n");SC(SYS_exit,127,0,0,0,0,0);
}
static int builtin(struct sh *s,char **argv,int argc) {
 if(seq(argv[0],"cd")){long r=SC(SYS_chdir,argc>1?argv[1]:getenv_(s,"HOME",4),0,0,0,0,0);if(r<0){put(2,"cd: errno ");putn(2,-r);put(2,"\n");return 1;}return 0;}
 if(seq(argv[0],"exit"))SC(SYS_exit,argc>1?atol_(argv[1]):s->status,0,0,0,0,0);
 if(seq(argv[0],"export")){for(int i=1;i<argc;i++)setenv_(s,argv[i]);return 0;}
 return -1;
}
/* Run tokens [a,b) as one pipeline. */
static int pipeline(struct sh *s,unsigned a,unsigned b,int bg) {
 long pids[16];unsigned np=0;int prev=-1;long first=0;
 while(a<b) {
  unsigned e=a;while(e<b&&s->typ[e]!=PIPE)e++;
  char *argv[40];int argc=0;char *in=0,*out=0;int app=0;
  for(unsigned i=a;i<e;i++) {
   if(s->typ[i]==W){if(argc<39)argv[argc++]=s->tok[i];}
   else if(i+1<e&&s->typ[i+1]==W){if(s->typ[i]==LT)in=s->tok[i+1];else{out=s->tok[i+1];app=s->typ[i]==GTGT;}i++;}
   else {put(2,"sh: syntax error\n");return 2;}
  }
  argv[argc]=0;
  if(!argc){put(2,"sh: syntax error\n");return 2;}
  if(np==0&&e==b&&!in&&!out&&!bg){int r=builtin(s,argv,argc);if(r>=0)return r;}
  int fd[2]={-1,-1};
  if(e<b&&sys_pipe(fd)<0){put(2,"sh: pipe failed\n");return 1;}
  long pid=sys_fork();
  if(pid==0) {
   sig(2,0);sig(3,0);
   if(prev>=0){SC(SYS_dup2,prev,0,0,0,0,0);SC(SYS_close,prev,0,0,0,0,0);}
   if(fd[1]>=0){SC(SYS_dup2,fd[1],1,0,0,0,0);SC(SYS_close,fd[1],0,0,0,0,0);SC(SYS_close,fd[0],0,0,0,0,0);}
   if(in){long f=SC(SYS_open,in,O_RDONLY,0,0,0,0);if(f<0){put(2,"sh: cannot open ");put(2,in);put(2,"\n");SC(SYS_exit,1,0,0,0,0,0);}SC(SYS_dup2,f,0,0,0,0,0);SC(SYS_close,f,0,0,0,0,0);}
   if(out){long f=SC(SYS_open,out,O_WRONLY|O_CREAT|(app?O_APPEND:O_TRUNC),0644,0,0,0);if(f<0){put(2,"sh: cannot create ");put(2,out);put(2,"\n");SC(SYS_exit,1,0,0,0,0,0);}SC(SYS_dup2,f,1,0,0,0,0);SC(SYS_close,f,0,0,0,0,0);}
   int r=builtin(s,argv,argc);if(r>=0)SC(SYS_exit,r,0,0,0,0,0);
   exec_path(s,argv);
  }
  if(pid<0){put(2,"sh: fork failed\n");break;}
  if(!first)first=pid;
  if(np<16)pids[np++]=pid;
  if(prev>=0)SC(SYS_close,prev,0,0,0,0,0);
  if(fd[1]>=0)SC(SYS_close,fd[1],0,0,0,0,0);
  prev=fd[0];a=e<b?e+1:e;
 }
 if(prev>=0)SC(SYS_close,prev,0,0,0,0,0);
 if(bg){put(2,"[");putn(2,first);put(2,"]\n");return 0;}
 int st=0;
 for(unsigned i=0;i<np;i++){int w=0;while(SC(SYS_wait4,pids[i],&w,0,0,0,0)==-4){}if(i==np-1)st=decode(w,1);}
 return st;
}
static void run(struct sh *s) {
 if(lex(s)<0){put(2,"sh: syntax error\n");s->status=2;return;}
 unsigned a=0;int skip=0;
 while(a<s->ntok) {
  unsigned e=a;while(e<s->ntok&&s->typ[e]!=SEMI&&s->typ[e]!=AND&&s->typ[e]!=OR&&s->typ[e]!=AMP)e++;
  int op=e<s->ntok?s->typ[e]:SEMI;
  if(e>a&&!skip)s->status=pipeline(s,a,e,op==AMP);
  skip=(op==AND&&s->status!=0)||(op==OR&&s->status==0);
  a=e+1;
 }
}
int umain(long *sp) {
 struct sh s;memset(&s,0,sizeof(s));
 int argc=(int)sp[0];char **argv=(char **)(sp+1);char **envp=argv+argc+1;
 for(unsigned i=0;envp[i]&&i<40;i++)setenv_(&s,envp[i]);
 if(argc>2&&seq(argv[1],"-c")) {
  unsigned n=slen(argv[2]);if(n>=sizeof(s.line))n=sizeof(s.line)-1;memcpy(s.line,argv[2],n);s.line[n]=0;
  run(&s);return s.status;
 }
 long ta[9];s.tty=SC(SYS_ioctl,0,TIOCGETA,ta,0,0,0)>=0;
 if(s.tty){sig(2,1);sig(3,1);}
 for(;;) {
  int w;long pid;while((pid=SC(SYS_wait4,-1,&w,1,0,0,0))>0){put(2,"[");putn(2,pid);put(2," done]\n");}
  if(s.tty)put(1,"jm21# ");
  unsigned n=0;long r;
  while(n<sizeof(s.line)-1&&(r=SC(SYS_read,0,s.line+n,1,0,0,0))!=0) {
   if(r==-4)continue;
   if(r<0)break;
   if(s.line[n++]=='\n')break;
  }
  if(!n)return s.status;
  s.line[n]=0;run(&s);
 }
}
