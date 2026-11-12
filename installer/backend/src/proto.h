/* 行协议的输出端。格式见 scripts/live/installer-lib.sh 文件头，前端解析器是 installer/common/lib/backend/protocol.dart：
 *   stdout：一行一条记录 `TYPE k=v k=v …`；
 *   stderr：`PROGRESS <百分比> <代码> [k=v …]`、`ERR code=<代码> [k=v …]`，以及以 `!! ` 开头给人看的说明。
 * 值一律做百分号编码。比 shell 版的 gk3__enc 多编码换行、回车和其他控制字符（否则卷标里的换行会把一条记录断成两行）；
 * 前端的 pctDecode 是通用的 %XX 解码，多编码的字符照样能解。 */
#ifndef GK3W_PROTO_H
#define GK3W_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    char *buf;
    size_t len, cap;
    int oom;            /* 追加时内存不足：之后的追加都忽略，emit 时报失败 */
} gk3w_line;

typedef enum {
    GK3W_TOUCHED_NA = 0,    /* 不是写盘入口：ERR 不带 touched */
    GK3W_TOUCHED_NO,
    GK3W_TOUCHED_YES,
} gk3w_touched;

/* 百分号编码一个值，写进 out（含结尾 NUL）。返回需要的长度（不含 NUL）；cap 不够时 out 被截断但仍以 NUL 结尾。 */
size_t gk3w_enc(const char *v, char *out, size_t cap);

void gk3w_line_begin(gk3w_line *l, const char *type);
void gk3w_line_kv(gk3w_line *l, const char *k, const char *v);
void gk3w_line_u64(gk3w_line *l, const char *k, uint64_t v);
void gk3w_line_hex64(gk3w_line *l, const char *k, uint64_t v);
/* 整行写出并 fflush。之前内存不足或写失败返回 -1。 */
int gk3w_line_emit(gk3w_line *l, FILE *f);
void gk3w_line_free(gk3w_line *l);

/* 失败：l 是调用方用 gk3w_line_begin(l, "ERR") 起头、已经加了 code= 和其他字段的一行。
 * 这里按需补上 touched=，写到 stderr，再写一行 `!! <human>`。返回 -1，调用方可以直接 return。 */
int gk3w_fail(gk3w_line *l, gk3w_touched t, const char *human);

/* 进度：`PROGRESS <pct> <code>` 写到 stderr。 */
void gk3w_progress(unsigned pct, const char *code);

#endif
