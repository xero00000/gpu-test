#include "../compat.h"
static inline u32 get_unaligned_le32(const void *p) {const u8 *b=p;return (u32)b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24;}
