// Local trust for this development port. Interface reference: akemin-dayo/AppSync,
// AppSyncUnified-FrontBoard.x (FBSSignatureValidationService, iOS14+).
// No global approval: only root-owned installer receipts with matching executable hashes.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static NSUInteger (*original)(id,SEL,id);
static NSUInteger localTrust(id self,SEL sel,id application) {
 @autoreleasepool {
  NSString *bid=nil;
  for(NSString *key in @[@"bundleIdentifier",@"applicationIdentifier"]) {
   @try {id value=[application valueForKey:key];if([value isKindOfClass:NSString.class]){bid=value;break;}} @catch(NSException *e) {}
  }
  NSString *receipt=@"/private/var/db/jm21-native-apps.plist";
  struct stat st;
  if(bid&&lstat(receipt.fileSystemRepresentation,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==0&&!(st.st_mode&022)) {
   NSDictionary *record=[NSDictionary dictionaryWithContentsOfFile:receipt][bid];
   NSString *path=record[@"Executable"],*expected=record[@"SHA256"];
   if([path hasPrefix:@"/private/var/containers/Bundle/Application/"]&&expected.length==64) {
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW);
    if(fd>=0) {
     if(fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==0&&!(st.st_mode&022)) {
      CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);unsigned char buffer[16384],digest[32];ssize_t n;
      while((n=read(fd,buffer,sizeof(buffer)))>0)CC_SHA256_Update(&ctx,buffer,(CC_LONG)n);
      CC_SHA256_Final(digest,&ctx);NSMutableString *actual=[NSMutableString new];
      for(unsigned i=0;i<32;i++)[actual appendFormat:@"%02x",digest[i]];
      if(n==0&&[actual isEqual:expected]) {close(fd);fprintf(stderr,"JM21 local trust: %s verified\n",bid.UTF8String);return 8;}
     }
     close(fd);
    }
   }
  }
  return original(self,sel,application);
 }
}
__attribute__((constructor)) static void installTrust(void) {
 Class cls=NSClassFromString(@"FBSSignatureValidationService");
 Method method=class_getInstanceMethod(cls,NSSelectorFromString(@"trustStateForApplication:"));
 if(method){original=(void*)method_setImplementation(method,(IMP)localTrust);fprintf(stderr,"JM21 local trust: ready\n");}
}
