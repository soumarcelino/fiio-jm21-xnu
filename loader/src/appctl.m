#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <objc/message.h>
/* Operational launcher and artifact transfer for device development. */
int main(int argc,char **argv) { @autoreleasepool {
 if(argc==3 && !strcmp(argv[1],"--hex")) {
  NSData *data=[NSData dataWithContentsOfFile:@(argv[2])];if(!data || data.length>1048576)return 1;
  const unsigned char *p=data.bytes;
  for(NSUInteger i=0;i<data.length;i++){printf("%02x",p[i]);if(i%64==63)putchar('\n');}putchar('\n');return 0;
 }
 if(argc==4 && !strcmp(argv[1],"--filter")) {
  NSString *s=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:NULL];
  NSArray *terms=[@(argv[3]) componentsSeparatedByString:@"|"];
  for(NSString *line in [s componentsSeparatedByString:@"\n"]) {
   for(NSString *term in terms) if([line containsString:term]) { puts(line.UTF8String); break; }
  }
  return s?0:1;
 }
 if(argc==3 && !strcmp(argv[1],"--tail")) {
  NSFileHandle *f=[NSFileHandle fileHandleForReadingAtPath:@(argv[2])]; if(!f) return 1;
  unsigned long long n=[f seekToEndOfFile]; [f seekToFileOffset:n>16384?n-16384:0];
  NSData *data=[f readDataToEndOfFile]; fwrite(data.bytes,1,data.length,stdout); return 0;
 }
 if(argc==3 && !strcmp(argv[1],"--ls")) {
  for(NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(argv[2]) error:NULL]) puts(name.UTF8String);
  return 0;
 }
 if(argc==4 && !strcmp(argv[1],"--get")) {
  __block BOOL done=NO,ok=NO;
  NSURLSessionConfiguration *cfg=[NSURLSessionConfiguration ephemeralSessionConfiguration]; cfg.timeoutIntervalForRequest=20;
  [[[NSURLSession sessionWithConfiguration:cfg] dataTaskWithURL:[NSURL URLWithString:@(argv[2])] completionHandler:^(NSData *data,NSURLResponse *response,NSError *error) {
   ok=data && [data writeToFile:@(argv[3]) options:NSDataWritingAtomic error:&error];
   NSLog(@"appctl transfer: %lu bytes, %@",(unsigned long)data.length,error); done=YES;
  }] resume];
  NSDate *limit=[NSDate dateWithTimeIntervalSinceNow:25];
  while(!done && [limit timeIntervalSinceNow]>0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  return ok?0:1;
 }
 if(argc!=2) { fprintf(stderr,"usage: appctl BUNDLE_ID | --get URL PATH\n"); return 2; }
 NSString *bundle=@(argv[1]);
 void *lib=dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",RTLD_NOW);
 int (*launch)(CFStringRef,Boolean)=lib?dlsym(lib,"SBSLaunchApplicationWithIdentifier"):NULL;
 CFStringRef (*explain)(int)=lib?dlsym(lib,"SBSApplicationLaunchingErrorString"):NULL;
 int status=launch?launch((CFStringRef)bundle,false):-1;
 NSLog(@"SBS launch: %d %@",status,explain?(id)explain(status):@"");
 if(!status) return 0;
 dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices",RTLD_NOW);
 Class cls=NSClassFromString(@"FBSSystemService");
 id service=((id(*)(id,SEL))objc_msgSend)(cls,sel_registerName("sharedService"));
 SEL open=sel_registerName("openApplication:options:withResult:");
 if(service && [service respondsToSelector:open]) {
  __block BOOL done=NO,ok=NO;
  ((void(*)(id,SEL,id,id,id))objc_msgSend)(service,open,bundle,@{},^(NSError *error){ NSLog(@"FBS launch: %@",error); ok=error==nil; done=YES; });
  NSDate *limit=[NSDate dateWithTimeIntervalSinceNow:15];
  while(!done && [limit timeIntervalSinceNow]>0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  return ok?0:1;
 }
 return 1;
} }
