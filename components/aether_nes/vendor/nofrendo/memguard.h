#ifndef  _MEMGUARD_H_
#define  _MEMGUARD_H_

#include <stdlib.h>
#include <string.h>

/* 2026 Stone: memguard 已停用。
** 原实现通过宏劫持 malloc/free/strdup，并返回带 256 字节 guard 偏移的指针，
** 与现代工具链的头文件布局、IDF 堆管理、第三方库均不兼容。
** 越界/泄漏检测改用 IDF heap poisoning + heap tracing。
** 此处仅保留 no-op 存根，避免改动 nofrendo 各处调用点。 */

extern void mem_cleanup(void);
extern void mem_checkblocks(void);
extern void mem_checkleaks(void);

extern bool mem_debug;

#endif   /* _MEMGUARD_H_ */