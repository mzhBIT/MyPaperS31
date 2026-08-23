#include "parser/gbk_decode.h"

#include "gbk_unicode_table.h"

uint32_t gbk_decode_codepoint(uint8_t high, uint8_t low)
{
    int index = gbk_table_index(high, low);
    if (index < 0) return 0xfffd;
    uint32_t codepoint = gbk_unicode_table[index];
    return codepoint != 0 ? codepoint : 0xfffd;
}
