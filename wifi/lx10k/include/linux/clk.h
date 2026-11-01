#ifndef LX10K_CLK_H
#define LX10K_CLK_H
#include <linux/err.h>
struct clk;
struct clk_bulk_data { const char *id; struct clk *clk; };
static inline int devm_clk_bulk_get_optional(void *d, int n, struct clk_bulk_data *c) { return 0; }
static inline int clk_bulk_prepare_enable(int n, struct clk_bulk_data *c) { return 0; }
static inline void clk_bulk_disable_unprepare(int n, struct clk_bulk_data *c) {}
#endif
