/* tapdump — diagnostic plops : dump N secondes du tap FX kernel
 * (/dev/imx-audio-tap-out, sorties DSP post-effets, 8 ch S32) vers un
 * fichier brut. Usage : tapdump <secondes> <fichier_sortie>
 * Protocole ring identique à anti-larsen.c (header 128 o, magic, wr@20). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define TAP_DEV   "/dev/imx-audio-tap-out"
#define TAP_TOTAL 0x40000u
#define TAP_HDR   128u
#define TAP_MAGIC 0x5441504Eu
#define NCHAN     8
#define FS        48000

static volatile uint8_t *g_tap;
static inline uint32_t tap_u32(uint32_t off)
{
    return *(volatile uint32_t *)(g_tap + off);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: tapdump <sec> <out>\n"); return 1; }
    int sec = atoi(argv[1]);
    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror("out"); return 1; }

    int fd = open(TAP_DEV, O_RDONLY);
    if (fd < 0) { perror(TAP_DEV); return 1; }
    void *m = mmap(NULL, TAP_TOTAL, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    g_tap = m;
    if (tap_u32(0) != TAP_MAGIC) { fprintf(stderr, "magic KO\n"); return 1; }

    uint32_t ring = tap_u32(8);
    const uint32_t fsz = NCHAN * 4;
    uint32_t rd = tap_u32(20);            /* part de « maintenant » */
    long total = (long)sec * FS, done = 0, lost = 0;
    static uint8_t buf[0x40000];          /* >= ring entier */

    while (done < total) {
        uint32_t wr = tap_u32(20);
        uint32_t avail = (wr - rd) % ring;
        avail -= avail % fsz;
        if (avail == 0) { usleep(1000); continue; }
        if (avail > ring - 2 * fsz) {     /* overrun : on repart de wr */
            lost += avail / fsz;
            rd = wr;
            continue;
        }
        /* copie en 1 ou 2 spans contigus (grands memcpy, mmap non-caché) */
        uint32_t first = ring - rd;
        if (first > avail) first = avail;
        memcpy(buf, (const void *)(g_tap + TAP_HDR + rd), first);
        if (avail > first)
            memcpy(buf + first, (const void *)(g_tap + TAP_HDR), avail - first);
        rd = (rd + avail) % ring;
        long n = avail / fsz;
        if (done + n > total) n = total - done;
        fwrite(buf, fsz, n, out);
        done += n;
    }
    fclose(out);
    fprintf(stderr, "dump: %ld frames (%d s), retard lecteur: %ld\n",
            done, sec, lost);
    return 0;
}
