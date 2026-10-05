/*
 *  test_utf8.c -- BT_Utf8ToMacRoman, against bytes MEASURED ON HARDWARE.
 *
 *  ⭐ The first case is not synthetic. Driver 14.8 stored the A1016's own name verbatim
 *  and the CSM drew "fw800,Aos keyboard" -- the user's screenshot. The keyboard is called
 *  "fw800's keyboard" with a U+2019 curly apostrophe, which UTF-8 encodes as E2 80 99 and
 *  MacRoman renders as three separate glyphs. This test is that exact string.
 */
#include <stdio.h>
#include <string.h>
#include "../src/bt_bootreport.h"

static int fails = 0;

static void expect(const char *what, const unsigned char *in, int inLen,
                   const unsigned char *want, int wantLen)
{
    unsigned char out[64];
    int n, i;

    memset(out, 0xEE, sizeof(out));
    n = BT_Utf8ToMacRoman(in, (unsigned short)inLen, out, (unsigned short)sizeof(out));
    if (n != wantLen) {
        printf("  FAIL %s: wrote %d bytes, wanted %d\n", what, n, wantLen);
        fails++;
        return;
    }
    for (i = 0; i < wantLen; i++) {
        if (out[i] != want[i]) {
            printf("  FAIL %s: byte %d is 0x%02X, wanted 0x%02X\n",
                   what, i, out[i], want[i]);
            fails++;
            return;
        }
    }
    printf("  ok   %s\n", what);
}

int main(void)
{
    printf("=== THE MEASURED CASE: the A1016's own name ===\n");
    {
        /* "fw800" + U+2019 + "s keyboard" as the controller sends it */
        static const unsigned char in[] = {
            'f','w','8','0','0', 0xE2,0x80,0x99, 's',' ','k','e','y','b','o','a','r','d'
        };
        /* the same, with a real MacRoman right single quote at 0xD5 */
        static const unsigned char want[] = {
            'f','w','8','0','0', 0xD5, 's',' ','k','e','y','b','o','a','r','d'
        };
        expect("fw800's keyboard -> 0xD5, not E2 80 99",
               in, (int)sizeof(in), want, (int)sizeof(want));
    }

    printf("\n=== plain ASCII is untouched ===\n");
    {
        static const unsigned char in[] = "Apple Wireless Keyboard";
        expect("ASCII passes through", in, 23, in, 23);
    }

    printf("\n=== the rest of the punctuation table ===\n");
    {
        static const unsigned char in[]   = { 0xE2,0x80,0x98, 0xE2,0x80,0x9C,
                                              0xE2,0x80,0x9D, 0xE2,0x80,0x93,
                                              0xE2,0x80,0x94, 0xE2,0x80,0xA6 };
        static const unsigned char want[] = { 0xD4, 0xD2, 0xD3, 0xD0, 0xD1, 0xC9 };
        expect("' \" \" en em ellipsis", in, (int)sizeof(in), want, (int)sizeof(want));
    }

    printf("\n=== two-byte Latin-1 letters ===\n");
    {
        /* "Jose" with an acute e (U+00E9 = C3 A9), and u-umlaut (U+00FC = C3 BC) */
        static const unsigned char in[]   = { 'J','o','s', 0xC3,0xA9, ' ', 0xC3,0xBC };
        static const unsigned char want[] = { 'J','o','s', 0x8E, ' ', 0x9F };
        expect("e-acute -> 0x8E, u-umlaut -> 0x9F",
               in, (int)sizeof(in), want, (int)sizeof(want));
    }

    printf("\n=== an unmapped code point becomes a VISIBLE '?', never a raw byte ===\n");
    {
        /* U+4E2D, a CJK ideograph: E4 B8 AD. MacRoman has no such character. */
        static const unsigned char in[]   = { 'a', 0xE4,0xB8,0xAD, 'b' };
        static const unsigned char want[] = { 'a', '?', 'b' };
        expect("CJK -> '?'", in, (int)sizeof(in), want, (int)sizeof(want));
    }

    printf("\n=== malformed input cannot loop, overrun, or eat the name ===\n");
    {
        /* A 3-byte lead with only one continuation byte available. */
        static const unsigned char in[]   = { 'a', 0xE2, 0x80 };
        /* ⚠ THREE bytes in, THREE out: 'a', then the 0xE2 lead is consumed alone
         * because its sequence is incomplete, then the orphaned 0x80. The first draft
         * of this test expected four and was WRONG -- there are only two bytes after
         * 'a' to consume. The code was right; the expectation was not. */
        static const unsigned char want[] = { 'a', '?', '?' };
        expect("truncated sequence: one '?' per consumed byte",
               in, (int)sizeof(in), want, (int)sizeof(want));
    }
    {
        /* A continuation byte with no lead. */
        static const unsigned char in[]   = { 0x99, 'x' };
        static const unsigned char want[] = { '?', 'x' };
        expect("orphan continuation byte", in, (int)sizeof(in), want, (int)sizeof(want));
    }
    {
        /* A lead byte promising a continuation that is NOT one. */
        static const unsigned char in[]   = { 0xC3, 'A', 'B' };
        static const unsigned char want[] = { '?', 'A', 'B' };
        expect("lead with a bad continuation", in, (int)sizeof(in), want, (int)sizeof(want));
    }
    {
        unsigned char out[4];
        int n = BT_Utf8ToMacRoman((const unsigned char *)"", 0, out, sizeof(out));
        printf("  %-4s zero-length input returns 0 (no underflow)\n", n == 0 ? "ok" : "FAIL");
        if (n != 0) fails++;
    }
    {
        unsigned char out[4];
        int n = BT_Utf8ToMacRoman(NULL, 5, out, sizeof(out));
        printf("  %-4s NULL source is refused\n", n == 0 ? "ok" : "FAIL");
        if (n != 0) fails++;
    }

    printf("\n=== dstMax is honoured (the name buffer is only 24 bytes) ===\n");
    {
        unsigned char out[4];
        static const unsigned char in[] = "abcdefgh";
        int n = BT_Utf8ToMacRoman(in, 8, out, 4);
        printf("  %-4s 8 bytes into a 4-byte buffer writes exactly 4\n",
               n == 4 ? "ok" : "FAIL");
        if (n != 4) fails++;
    }

    if (fails) { printf("\n%d FAILURE(S)\n", fails); return 1; }
    printf("\nall UTF-8 conversion tests passed\n");
    return 0;
}
