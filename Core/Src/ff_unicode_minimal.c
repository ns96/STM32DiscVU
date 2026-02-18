#include "ff.h"

/**
  * @brief  Minimal Unicode support for FatFs LFN.
  * Replicated from reference project for professional long-filename support.
  */

#if _USE_LFN != 0

/* OEM-Unicode bidirectional conversion */
WCHAR ff_convert (WCHAR chr, UINT dir)
{
    /* Minimal implementation: assume 1:1 mapping for ASCII. */
    if (chr < 0x80) {
        return chr;
    }
    return chr; 
}

/* Unicode upper-case conversion */
WCHAR ff_wtoupper (WCHAR chr)
{
    if (chr >= 'a' && chr <= 'z') {
        return chr - ('a' - 'A');
    }
    return chr;
}

#endif
