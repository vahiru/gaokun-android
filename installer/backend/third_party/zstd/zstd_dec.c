/* zstd 1.5.7 的解码部分，按官方 build/single_file_libs/zstddeclib-in.c 的同一套宏与文件清单编成一个翻译单元。
 * 源文件原样取自 https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
 * （sha256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3），只拷了 lib/ 下解码要用的文件，未做修改。
 * 许可：BSD（见 LICENSE），与 GPLv2 二选一。 */
#define DEBUGLEVEL 0
#define MEM_MODULE
#undef XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#undef XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_STRIP_ERROR_STRINGS
#define ZSTD_TRACE 0
#define ZSTD_DISABLE_ASM 1
#define ZSTD_DEPS_NEED_MALLOC
#include "lib/common/zstd_deps.h"
#include "lib/common/debug.c"
#include "lib/common/entropy_common.c"
#include "lib/common/error_private.c"
#include "lib/common/fse_decompress.c"
#include "lib/common/zstd_common.c"
#include "lib/decompress/huf_decompress.c"
#include "lib/decompress/zstd_ddict.c"
#include "lib/decompress/zstd_decompress.c"
#include "lib/decompress/zstd_decompress_block.c"
