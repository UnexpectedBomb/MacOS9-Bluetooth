/* Scratch: prove bt_inject.c's KeyTranslate input expression equals Apple's. */
#include <stdio.h>
typedef unsigned int   UInt32;
typedef unsigned short UInt16;

/* Apple, KeyIn.c:471 -- virtualKeycode is UInt16, OR'd in place. */
static UInt16 apple(UInt16 vk, UInt32 km1, int up) {
    vk |= ((km1 << 9) & 0x00FE00) | ((km1 >> 7) & 0x0100) | (up ? 0x080 : 0);
    return vk;
}

/* Mine, bt_inject.c -- built into a separate UInt16 rather than mutating vk. */
static UInt16 mine(unsigned char vk, UInt32 km1, int up) {
    return (UInt16)(vk
                    | ((km1 << 9) & 0x00FE00)
                    | ((km1 >> 7) & 0x000100)
                    | (up ? 0x0080 : 0));
}

int main(void) {
    unsigned v, k; int u, bad = 0; long n = 0;

    /* every virtual keycode x every single-bit KeyMap[1] x both directions */
    for (v = 0; v <= 0x7F; v++)
        for (k = 0; k < 32; k++)
            for (u = 0; u < 2; u++) {
                UInt32 km1 = 1u << k;
                n++;
                if (apple((UInt16)v, km1, u) != mine((unsigned char)v, km1, u)) {
                    if (bad < 5)
                        printf("  MISMATCH vk=%02X km1=%08X up=%d: apple=%04X mine=%04X\n",
                               v, km1, u, apple((UInt16)v, km1, u),
                               mine((unsigned char)v, km1, u));
                    bad++;
                }
            }

    /* dense multi-bit modifier patterns, which is the realistic case */
    for (v = 0; v <= 0x7F; v++)
        for (k = 0; k <= 0xFFFFu; k += 7)
            for (u = 0; u < 2; u++) {
                n++;
                if (apple((UInt16)v, (UInt32)k, u) != mine((unsigned char)v, (UInt32)k, u))
                    bad++;
            }

    printf("%ld comparisons, %d mismatches\n", n, bad);
    return bad ? 1 : 0;
}
