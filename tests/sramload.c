// SPDX-License-Identifier: MIT
/*
 * sramload: put one program's text in SRAM.
 *
 *   sramload [PROGRAM [MTD [NAME]]]   default /bin/busybox, /dev/mtd0,
 *                                     NAME: the file name in the romfs
 *                                     (default: PROGRAM's, at most 15)
 *
 * Writes a romfs holding a single file to the sram-text RAM MTD
 * (rp2350.dts). The file is PROGRAM cut after its last PT_LOAD segment;
 * section headers and the rest are not needed to run it, and SRAM is
 * small. Then:
 *
 *   mount -t romfs mtd0 /sram && mount --bind /sram/busybox /bin/busybox
 *
 * and every new exec of a busybox applet maps its text in place from SRAM
 * (ELF-FDPIC shares read-only text; romfs on a RAM MTD maps it directly).
 * Processes already running keep the copy they have.
 *
 * romfs: big-endian words; superblock "-rom1fs-", size, checksum, volume
 * name; then 16-byte aligned file headers (next|type, spec, size, checksum,
 * name). Words over the first 512 bytes, and over each header, sum to 0.
 */
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define ROMFH_DIR	1
#define ROMFH_REG	2
#define ROMFH_EXEC	8

static void be32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t sum(const unsigned char *p, size_t n)
{
	uint32_t s = 0;
	size_t i;

	for (i = 0; i + 4 <= n; i += 4)
		s += (uint32_t)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3];
	return s;
}

/* a 32-byte header (16 + name padded to 16) at p */
static void header(unsigned char *p, uint32_t next, uint32_t spec, uint32_t size, const char *name)
{
	memset(p, 0, 32);
	be32(p, next);
	be32(p + 4, spec);
	be32(p + 8, size);
	strcpy((char *)p + 16, name);
	be32(p + 12, -sum(p, 32));
}

int main(int argc, char **argv)
{
	const char *prog = argc > 1 ? argv[1] : "/bin/busybox";
	const char *dev = argc > 2 ? argv[2] : "/dev/mtd0";
	const char *name = argc > 3 ? argv[3] : strrchr(prog, '/') ? strrchr(prog, '/') + 1 : prog;
	unsigned char *img;
	Elf32_Ehdr eh;
	Elf32_Phdr ph;
	uint32_t len = 0, total;
	int fd, i;

	if (strlen(name) > 15) {
		fprintf(stderr, "sramload: name too long\n");
		return 1;
	}
	fd = open(prog, O_RDONLY);
	if (fd < 0 || read(fd, &eh, sizeof(eh)) != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG)) {
		perror(prog);
		return 1;
	}
	for (i = 0; i < eh.e_phnum; i++) {
		if (pread(fd, &ph, sizeof(ph), eh.e_phoff + i * sizeof(ph)) != sizeof(ph))
			return 1;
		if (ph.p_type == PT_LOAD && ph.p_offset + ph.p_filesz > len)
			len = ph.p_offset + ph.p_filesz;
	}
	total = (96 + len + 1023) & ~1023u;
	img = calloc(1, total);
	if (!img)
		return 1;
	if (pread(fd, img + 96, len, 0) != (ssize_t)len) {
		perror("read");
		return 1;
	}
	close(fd);

	memcpy(img, "-rom1fs-", 8);
	be32(img + 8, total);
	strcpy((char *)img + 16, "sram-text");
	header(img + 32, ROMFH_DIR | ROMFH_EXEC, 64, 0, ".");		/* root: children at 64 */
	header(img + 64, ROMFH_REG | ROMFH_EXEC, 0, len, name);		/* data at 96 */
	be32(img + 12, -sum(img, total < 512 ? total : 512));

	fd = open(dev, O_WRONLY);
	if (fd < 0 || write(fd, img, total) != (ssize_t)total) {
		perror(dev);
		return 1;
	}
	close(fd);
	printf("sramload: %s, %u of its bytes (through the last PT_LOAD), romfs %u bytes on %s\n",
	       prog, len, total, dev);
	return 0;
}
