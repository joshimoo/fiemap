/*
 * Copyright (C) 2010-2020 Canonical
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

/*
 *  Author Colin Ian King,  colin.king@canonical.com
 *
 *  Add support of multi-chunk fiemaps. Oleksandr Suvorov, cryosay@gmail.com
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <linux/fs.h>
#include <linux/fiemap.h>

void syntax(char **argv)
{
	fprintf(stderr, "%s [filename]...\n",argv[0]);
}

void dump_extents(struct fiemap *fiemap, int chunk, long long elapsed)
{
	if (chunk == 0) {
		printf("#\tLogical          Physical         Length           Flags\n");
	}

	for (int i = 0; i < fiemap->fm_mapped_extents; i++) {
		printf("%d:\t%-16.16llx %-16.16llx %-16.16llx %-4.4x\n",
			chunk + i,
			fiemap->fm_extents[i].fe_logical,
			fiemap->fm_extents[i].fe_physical,
			fiemap->fm_extents[i].fe_length,
			fiemap->fm_extents[i].fe_flags);
	}
	printf("retrieved %d extents in %lld seconds\n", fiemap->fm_mapped_extents, elapsed / 1000000000L);
	printf("\n");
}

void dump_fiemap(struct fiemap *fiemap, char *filename)
{
	printf("File %s has %d extents:\n",filename, fiemap->fm_mapped_extents);
	// dump_extents(fiemap, 0)
}

long long get_time_delta(struct timespec start, struct timespec end) {
	return ( end.tv_nsec - start.tv_nsec ) + 
		( end.tv_sec - start.tv_sec ) * 1000000000L;
}

struct fiemap *read_fiemap(int fd)
{
	struct fiemap *fiemap = NULL;
	struct fiemap *result_fiemap = NULL;
	struct fiemap *fm_tmp; /* need to store pointer on realloc */
	int extents_size;
	struct stat statinfo;
	struct timespec fileStart, chunkStart, chunkEnd;
	__u32 result_extents = 0;
	__u64 fiemap_start = 0, fiemap_length;

	const __u32 MAX_EXTENTS = 1024;
	const ulong SIZE_OF_EXTENTS = sizeof(struct fiemap_extent) * MAX_EXTENTS;

	clock_gettime(CLOCK_MONOTONIC, &fileStart);
	if (fstat(fd, &statinfo) != 0) {
		fprintf(stderr, "Cannot determine file size, errno=%d (%s)\n",
				errno, strerror(errno));
		return NULL;
	}
	fiemap_length = statinfo.st_size;

	fiemap = malloc(sizeof(struct fiemap) + SIZE_OF_EXTENTS);
	if (fiemap == NULL) {
		fprintf(stderr, "Out of memory allocating fiemap\n");
		return NULL;
	}

	result_fiemap = malloc(sizeof(struct fiemap));
	if (result_fiemap == NULL) {
		fprintf(stderr, "Out of memory allocating fiemap\n");
		goto fail_cleanup;
	}

	/*  XFS filesystem has incorrect implementation of fiemap ioctl and
	 *  returns extents for only one block-group at a time, so we need
	 *  to handle it manually, starting the next fiemap call from the end
	 *  of the last extent
	 */
	while (fiemap_start < fiemap_length) {
		memset(fiemap, 0, sizeof(struct fiemap));
		memset(fiemap->fm_extents, 0, SIZE_OF_EXTENTS);

		fiemap->fm_start = fiemap_start;
		fiemap->fm_length = fiemap_length;
		fiemap->fm_flags = FIEMAP_FLAG_SYNC;
		fiemap->fm_extent_count = MAX_EXTENTS;

		clock_gettime(CLOCK_MONOTONIC, &chunkStart);
		/* Find out how many extents there are */
		if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
			fprintf(stderr, "fiemap ioctl() FS_IOC_FIEMAP failed, errno=%d (%s)\n",
				errno, strerror(errno));
			goto fail_cleanup;
		}

		clock_gettime(CLOCK_MONOTONIC, &chunkEnd);
		dump_extents(fiemap, result_extents, get_time_delta(chunkStart, chunkEnd));

		/* Nothing to process */
		if (fiemap->fm_mapped_extents == 0)
			break;

		/* Result fiemap have to hold all the extents for the hole file */
		extents_size = sizeof(struct fiemap_extent) * (result_extents + fiemap->fm_mapped_extents);

		/* Resize result_fiemap to allow us to copy over the extents */
		fm_tmp = realloc(result_fiemap, sizeof(struct fiemap) + extents_size);
		if (!fm_tmp) {
			fprintf(stderr, "Out of memory allocating fiemap\n");
			goto fail_cleanup;
		}
		result_fiemap = fm_tmp;

		memcpy(result_fiemap->fm_extents + result_extents,
		       fiemap->fm_extents,
		       sizeof(struct fiemap_extent) *
		       fiemap->fm_mapped_extents);

		result_extents += fiemap->fm_mapped_extents;

		/* Highly unlikely that it is zero */
		if (fiemap->fm_mapped_extents) {
			const __u32 i = fiemap->fm_mapped_extents - 1;

			fiemap_start = fiemap->fm_extents[i].fe_logical +
				       fiemap->fm_extents[i].fe_length;

			if (fiemap->fm_extents[i].fe_flags & FIEMAP_EXTENT_LAST)
				break;
		}
	}

	result_fiemap->fm_mapped_extents = result_extents;
	free(fiemap);
	clock_gettime(CLOCK_MONOTONIC, &chunkEnd);
	long long elapsed = get_time_delta(fileStart, chunkEnd);
	printf("fiemap done retrieved %d extents in %lld seconds\n", result_fiemap->fm_mapped_extents, elapsed / 1000000000L);

	return result_fiemap;

fail_cleanup:
	if (result_fiemap)
		free(result_fiemap);

	if (fiemap)
		free(fiemap);

	return NULL;
}

int main(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		syntax(argv);
		exit(EXIT_FAILURE);
	}

	for (i = 1; i < argc; i++) {
		int fd;

		if ((fd = open(argv[i], O_RDONLY)) < 0) {
			fprintf(stderr, "Cannot open file %s, errno=%d (%s)\n",
				argv[i], errno, strerror(errno));
		}
		else {
			struct fiemap *fiemap;

			if ((fiemap = read_fiemap(fd)) != NULL)
				dump_fiemap(fiemap, argv[i]);
			close(fd);
		}
	}
	exit(EXIT_SUCCESS);
}

