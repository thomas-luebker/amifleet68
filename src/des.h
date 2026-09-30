/* des.h - DES block encryption for VNC authentication (see des.c). */
#ifndef AMIFLEET_DES_H
#define AMIFLEET_DES_H
void des_encrypt_block(const unsigned char key[8], const unsigned char in[8], unsigned char out[8]);
/* The 16-byte VNC auth response: DES-ECB with the bit-mirrored password. */
void vnc_auth_response(const char *password, const unsigned char challenge[16],
                       unsigned char response[16]);
#endif
