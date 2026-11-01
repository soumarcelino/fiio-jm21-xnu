#include <stdint.h>
void boot_error(void);
void line(const char *s);
void hex(const char *s,uint64_t v);
void putc_screen(char c);
uint64_t load_xnu(void);
void gold_main(void);

void usb_screen_status(const char *stage,uint64_t address,uint32_t value,unsigned fault);

void usb_screen_detail(unsigned row,const char *message);
