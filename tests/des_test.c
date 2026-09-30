/* Host test for src/des.c:  cc -o /tmp/des_test tests/des_test.c src/des.c && /tmp/des_test */
#include <stdio.h>
#include <string.h>
#include "../src/des.h"

static int hex_eq(const unsigned char *b, int n, const char *hex)
{
    char s[64]; int i;
    for (i = 0; i < n; i++) sprintf(s + 2 * i, "%02x", b[i]);
    printf("  %s %s\n", s, strcmp(s, hex) ? "!= expected" : "ok");
    return strcmp(s, hex) == 0;
}

int main(int argc, char **argv)
{
    int ok = 1;
    /* The textbook vector (FIPS 46 worked example). */
    const unsigned char k[8] = {0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1};
    const unsigned char p[8] = {0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
    unsigned char c[8], r[16], ch[16];
    int i;
    des_encrypt_block(k, p, c);
    ok &= hex_eq(c, 8, "85e813540f0ab405");
    /* VNC response for a known challenge; expected value from argv[1] (openssl). */
    for (i = 0; i < 16; i++) ch[i] = (unsigned char)(i * 17);
    vnc_auth_response("amiga", ch, r);
    if (argc > 1) ok &= hex_eq(r, 16, argv[1]);
    puts(ok ? "PASS" : "FAIL");
    return !ok;
}
