/***************************************************************************
 *   Copyright (C) 2007 PCSX-df Team                                       *
 *   Copyright (C) 2009 Wei Mingzhi                                        *
 *   Copyright (C) 2012 notaz                                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "psxcommon.h"
#include "cdrom.h"
#include "cdriso.h"
#include "ppf.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <zlib.h>
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#endif
#ifdef HAVE_CHD
#include <libchdr/chd.h>
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef USE_LIBRETRO_VFS
#include <streams/file_stream_transforms.h>
#undef fseeko
#undef ftello
#undef rewind
#define ftello rftell
#define fseeko rfseek
#define rewind(f_) rfseek(f_, 0, SEEK_SET)
#endif

#define OFF_T_MSB ((off_t)1 << (sizeof(off_t) * 8 - 1))

unsigned int cdrIsoMultidiskCount;
unsigned int cdrIsoMultidiskSelect;

static FILE *cdHandle = NULL;
static FILE *subHandle = NULL;

static boolean subChanMixed = FALSE;
static boolean subChanRaw = FALSE;

static boolean multifile = FALSE;

static unsigned char cdbuffer[CD_FRAMESIZE_RAW];

#ifndef RG_PSX_CDREAD_CACHE_SECTORS
#define RG_PSX_CDREAD_CACHE_SECTORS 16
#endif
#if RG_PSX_CDREAD_CACHE_SECTORS < 1
#undef RG_PSX_CDREAD_CACHE_SECTORS
#define RG_PSX_CDREAD_CACHE_SECTORS 1
#endif

static unsigned char cdread_cache[RG_PSX_CDREAD_CACHE_SECTORS * CD_FRAMESIZE_RAW] EXT_RAM_BSS_ATTR;
static FILE *cdread_cache_file;
static unsigned int cdread_cache_base;
static unsigned int cdread_cache_sector_size;
static int cdread_cache_start = -1;
static int cdread_cache_count;

static void cdriso_invalidate_read_cache(void)
{
	cdread_cache_file = NULL;
	cdread_cache_base = 0;
	cdread_cache_sector_size = 0;
	cdread_cache_start = -1;
	cdread_cache_count = 0;
}

static int cdread_cached(FILE *f, unsigned int base, void *dest, int sector,
	unsigned int sector_size, unsigned int dest_offset, unsigned int payload_size)
{
	unsigned char *dst = dest ? dest : cdbuffer;
	unsigned int cache_index;
	size_t read_bytes;

	if (!f || sector < 0 || sector_size == 0 || sector_size > CD_FRAMESIZE_RAW)
		return -1;
	if (dest_offset + payload_size > CD_FRAMESIZE_RAW || payload_size > sector_size)
		return -1;

	if (cdread_cache_file != f || cdread_cache_base != base ||
	    cdread_cache_sector_size != sector_size ||
	    sector < cdread_cache_start ||
	    sector >= cdread_cache_start + cdread_cache_count) {
		off_t offset = (off_t)base + (off_t)sector * sector_size;
		if (fseeko(f, offset, SEEK_SET))
			return -1;

		read_bytes = fread(cdread_cache, 1,
			(size_t)RG_PSX_CDREAD_CACHE_SECTORS * sector_size, f);
		cdread_cache_file = f;
		cdread_cache_base = base;
		cdread_cache_sector_size = sector_size;
		cdread_cache_start = sector;
		cdread_cache_count = (int)(read_bytes / sector_size);
		if (cdread_cache_count <= 0) {
			cdriso_invalidate_read_cache();
			return -1;
		}
	}

	cache_index = (unsigned int)(sector - cdread_cache_start) * sector_size;
	memcpy(dst + dest_offset, cdread_cache + cache_index, payload_size);
	return (int)(dest_offset + payload_size);
}

static boolean cddaBigEndian = FALSE;

// compressed image stuff
static struct {
	unsigned char buff_raw[16][CD_FRAMESIZE_RAW];
	unsigned char buff_compressed[CD_FRAMESIZE_RAW * 16 + 100];
	off_t *index_table;
	unsigned int index_len;
	unsigned int block_shift;
	unsigned int current_block;
	unsigned int sector_in_blk;
} *compr_img;

#ifdef HAVE_CHD
static struct {
	unsigned char *buffer;
	chd_file* chd;
	const chd_header* header;
	unsigned int sectors_per_hunk;
	unsigned int sector_size;
	unsigned int current_hunk[2];
	unsigned int current_buffer;
	unsigned int sector_in_hunk;
} *chd_img;
#else
#define chd_img 0
#endif

static int (*cdimg_read_func)(FILE *f, unsigned int base, void *dest, int sector);
static int (*cdimg_read_sub_func)(FILE *f, int sector, void *dest);

static void DecodeRawSubData(unsigned char *subbuffer);

struct trackinfo {
	enum cdrType type;
	unsigned int start;        // sector index in toc
	unsigned int length;       // readable sectors, excludes gaps
	unsigned int start_offset; // byte offset from start of file (chd: sector offset)
	FILE *handle;              // for multi-track images
};

#define MAXTRACKS 100 /* How many tracks can a CD hold? */

// 1-based array (indexed [1..numtracks])
static int numtracks = 0;
static struct trackinfo ti[MAXTRACKS];

// get a sector from a msf-array
static unsigned int msf2sec(const void *msf_) {
	const unsigned char *msf = msf_;
	return ((msf[0] * 60 + msf[1]) * 75) + msf[2];
}

static void sec2msf(unsigned int s, void *msf_) {
	unsigned char *msf = msf_;
	msf[0] = s / 75 / 60;
	s = s - msf[0] * 75 * 60;
	msf[1] = s / 75;
	s = s - msf[1] * 75;
	msf[2] = s;
}

// divide a string of xx:yy:zz into m, s, f
static void tok2msf(char *time, char *msf) {
	char *token;

	token = strtok(time, ":");
	if (token) {
		msf[0] = atoi(token);
	}
	else {
		msf[0] = 0;
	}

	token = strtok(NULL, ":");
	if (token) {
		msf[1] = atoi(token);
	}
	else {
		msf[1] = 0;
	}

	token = strtok(NULL, ":");
	if (token) {
		msf[2] = atoi(token);
	}
	else {
		msf[2] = 0;
	}
}

static off_t get_size(FILE *f)
{
	off_t old, size;

	if (f == NULL)
		return 0;
#if !defined(USE_LIBRETRO_VFS)
	struct stat st;
	if (fstat(fileno(f), &st) == 0 && st.st_size >= 0)
		return st.st_size;
#endif
	old = ftello(f);
	if (old < 0)
		return 0;
	if (fseeko(f, 0, SEEK_END) != 0)
		return 0;
	size = ftello(f);
	if (size < 0)
		size = 0;
	fseeko(f, old, SEEK_SET);
	return size;
}

// Some c libs like newlib default buffering to just 1k which is less than
// cd sector size which is bad for performance.
// Note that NULL setvbuf() is implemented differently by different libs
// (newlib mallocs a buffer of given size and glibc ignores size and uses it's own).
static void set_static_stdio_buffer(FILE *f)
{
/*
 * ESP-IDF/newlib keeps per-FILE locks inside the FILE object.  The stock
 * PCSX-ReARMed helper below is useful on desktop libc implementations, but on
 * this ESP32-S3 port it has repeatedly made early ISO/CUE probing much more
 * fragile: CD-open paths use several short-lived FILEs while Retro-Go logging
 * is active, and a lock assertion in newlib can turn a recoverable bad sidecar
 * probe into a full system panic.  The SD/FatFS path is already block buffered,
 * so prefer correctness here and leave stdio buffering alone on ESP-IDF.
 */
#if !defined(fopen) && !defined(ESP_PLATFORM) // no stdio redirect
	static char buf[16 * 1024];
	if (f) {
		int r;
		errno = 0;
		r = setvbuf(f, buf, _IOFBF, sizeof(buf));
		if (r)
			SysPrintf("cdriso: setvbuf %d %d\n", r, errno);
	}
#else
	(void)f;
#endif
}

// this function tries to get the .toc file of the given .bin
// the necessary data is put into the ti (trackinformation)-array
static int parsetoc(const char *isofile) {
	static char		tocname[MAXPATHLEN];
	FILE			*fi;
	char			linebuf[256], tmp[256], name[256];
	char			*token;
	char			time[20], time2[20];
	unsigned int	t, sector_offs, sector_size;
	unsigned int	current_zero_gap = 0;

	numtracks = 0;

	// copy name of the iso and change extension from .bin to .toc
	strncpy(tocname, isofile, sizeof(tocname));
	tocname[MAXPATHLEN - 1] = '\0';
	if (strlen(tocname) >= 4) {
		strcpy(tocname + strlen(tocname) - 4, ".toc");
	}
	else {
		return -1;
	}

	if ((fi = fopen(tocname, "r")) == NULL) {
		// try changing extension to .cue (to satisfy some stupid tutorials)
		strcpy(tocname + strlen(tocname) - 4, ".cue");
		if ((fi = fopen(tocname, "r")) == NULL) {
			// if filename is image.toc.bin, try removing .bin (for Brasero)
			strcpy(tocname, isofile);
			t = strlen(tocname);
			if (t >= 8 && strcmp(tocname + t - 8, ".toc.bin") == 0) {
				tocname[t - 4] = '\0';
				if ((fi = fopen(tocname, "r")) == NULL) {
					return -1;
				}
			}
			else {
				return -1;
			}
		}
		// check if it's really a TOC named as a .cue
		if (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
		token = strtok(linebuf, " ");
		if (token && strncmp(token, "CD", 2) != 0) {
		       // && strcmp(token, "CATALOG") != 0) - valid for a real .cue
			fclose(fi);
			return -1;
		}
		}
		fseek(fi, 0, SEEK_SET);
	}

	memset(&ti, 0, sizeof(ti));
	cddaBigEndian = TRUE; // cdrdao uses big-endian for CD Audio

	sector_size = CD_FRAMESIZE_RAW;
	sector_offs = 2 * 75;

	// parse the .toc file
	while (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
		// search for tracks
		strncpy(tmp, linebuf, sizeof(linebuf));
		token = strtok(tmp, " ");

		if (token == NULL) continue;

		if (!strcmp(token, "TRACK")) {
			sector_offs += current_zero_gap;
			current_zero_gap = 0;

			// get type of track
			token = strtok(NULL, " ");
			numtracks++;

			if (!strncmp(token, "MODE2_RAW", 9)) {
				ti[numtracks].type = CDRT_DATA;
				ti[numtracks].start = 2 * 75; // assume data track on 0:2:0

				// check if this image contains mixed subchannel data
				token = strtok(NULL, " ");
				if (token != NULL && !strncmp(token, "RW", 2)) {
					sector_size = CD_FRAMESIZE_RAW + SUB_FRAMESIZE;
					subChanMixed = TRUE;
					if (!strncmp(token, "RW_RAW", 6))
						subChanRaw = TRUE;
				}
			}
			else if (!strncmp(token, "AUDIO", 5)) {
				ti[numtracks].type = CDRT_CDDA;
			}
		}
		else if (!strcmp(token, "DATAFILE")) {
			char msf[3];
			if (ti[numtracks].type == CDRT_CDDA) {
				sscanf(linebuf, "DATAFILE \"%[^\"]\" #%d %8s", name, &t, time2);
				ti[numtracks].start_offset = t;
				ti[numtracks].start = t / sector_size + sector_offs;
			}
			else {
				sscanf(linebuf, "DATAFILE \"%[^\"]\" %8s", name, time2);
			}
			tok2msf(time2, msf);
			ti[numtracks].length = msf2sec(msf);
		}
		else if (!strcmp(token, "FILE")) {
			char msf[3];
			sscanf(linebuf, "FILE \"%[^\"]\" #%d %8s %8s", name, &t, time, time2);
			tok2msf(time, msf);
			t += msf2sec(msf) * sector_size;
			ti[numtracks].start_offset = t;
			ti[numtracks].start = t / sector_size + sector_offs;
			tok2msf(time2, msf);
			ti[numtracks].length = msf2sec(msf);
		}
		else if (!strcmp(token, "ZERO") || !strcmp(token, "SILENCE")) {
			// skip unneeded optional fields
			while (token != NULL) {
				token = strtok(NULL, " ");
				if (strchr(token, ':') != NULL)
					break;
			}
			if (token != NULL) {
				tok2msf(token, tmp);
				current_zero_gap = msf2sec(tmp);
			}
			if (numtracks > 1) {
				t = ti[numtracks - 1].start_offset;
				t /= sector_size;
			}
		}
		else if (!strcmp(token, "START")) {
			token = strtok(NULL, " ");
			if (token != NULL && strchr(token, ':')) {
				tok2msf(token, tmp);
				t = msf2sec(tmp);
				ti[numtracks].start_offset += (t - current_zero_gap) * sector_size;
				ti[numtracks].start += t;
			}
		}
	}

	fclose(fi);

	return 0;
}


static void cdriso_close_track_handles(void)
{
	int i;

	for (i = 1; i < MAXTRACKS; i++) {
		if (ti[i].handle != NULL) {
			fclose(ti[i].handle);
			ti[i].handle = NULL;
		}
	}
}

static void cdriso_clear_toc(void)
{
	cdriso_close_track_handles();
	memset(ti, 0, sizeof(ti));
	numtracks = 0;
	multifile = FALSE;
}


static const char *cdriso_basename(const char *path)
{
	const char *slash, *backslash;

	if (path == NULL)
		return "";

	slash = strrchr(path, '/');
	backslash = strrchr(path, '\\');
	if (backslash != NULL && (slash == NULL || backslash > slash))
		slash = backslash;
	return slash != NULL ? slash + 1 : path;
}

static void cdriso_normalize_slashes(char *path)
{
	char *p;

	for (p = path; p != NULL && *p; p++)
		if (*p == '\\')
			*p = '/';
}

static int cdriso_dirname(const char *path, char *out, size_t out_size)
{
	const char *slash;
	size_t len;

	if (path == NULL || out == NULL || out_size == 0)
		return -1;

	slash = strrchr(path, '/');
	if (slash == NULL) {
		snprintf(out, out_size, ".");
		return 0;
	}

	len = slash - path;
	if (len == 0)
		len = 1;
	if (len >= out_size)
		len = out_size - 1;
	memcpy(out, path, len);
	out[len] = 0;
	return 0;
}

static int cdriso_join_path(char *out, size_t out_size, const char *dir, const char *name)
{
	int written;

	if (out == NULL || out_size == 0 || dir == NULL || name == NULL)
		return -1;

	if (dir[0] == 0 || strcmp(dir, ".") == 0)
		written = snprintf(out, out_size, "%s", name);
	else
		written = snprintf(out, out_size, "%s/%s", dir, name);

	return written >= 0 && (size_t)written < out_size ? 0 : -1;
}

static FILE *cdriso_fopen_rb_casefold(const char *path, char *resolved, size_t resolved_size)
{
	FILE *f;
#ifndef _WIN32
	char dir[MAXPATHLEN];
	const char *base;
	DIR *dh;
	struct dirent *de;
#endif

	if (path == NULL)
		return NULL;

	f = fopen(path, "rb");
	if (f != NULL) {
		if (resolved != NULL && resolved_size > 0)
			snprintf(resolved, resolved_size, "%s", path);
		return f;
	}

#ifndef _WIN32
	if (cdriso_dirname(path, dir, sizeof(dir)) != 0)
		return NULL;
	base = cdriso_basename(path);
	dh = opendir(dir);
	if (dh == NULL)
		return NULL;

	while ((de = readdir(dh)) != NULL) {
		char candidate[MAXPATHLEN];
		if (strcasecmp(de->d_name, base) != 0)
			continue;
		if (cdriso_join_path(candidate, sizeof(candidate), dir, de->d_name) != 0)
			continue;
		f = fopen(candidate, "rb");
		if (f != NULL) {
			if (resolved != NULL && resolved_size > 0)
				snprintf(resolved, resolved_size, "%s", candidate);
			closedir(dh);
			return f;
		}
	}
	closedir(dh);
#endif

	return NULL;
}

static FILE *cdriso_open_cue_reference(const char *cue_dir, const char *cue_ref,
	char *resolved, size_t resolved_size)
{
	char ref[MAXPATHLEN];
	char candidate[MAXPATHLEN];
	FILE *f;

	if (cue_ref == NULL || cue_ref[0] == 0)
		return NULL;

	snprintf(ref, sizeof(ref), "%s", cue_ref);
	cdriso_normalize_slashes(ref);

	if (ref[0] == '/')
		return cdriso_fopen_rb_casefold(ref, resolved, resolved_size);

	if (cdriso_join_path(candidate, sizeof(candidate), cue_dir, ref) == 0) {
		f = cdriso_fopen_rb_casefold(candidate, resolved, resolved_size);
		if (f != NULL)
			return f;
	}

	if (cdriso_join_path(candidate, sizeof(candidate), cue_dir, cdriso_basename(ref)) == 0)
		return cdriso_fopen_rb_casefold(candidate, resolved, resolved_size);

	return NULL;
}

static int cdriso_parse_cue_file_name(const char *line, char *out, size_t out_size)
{
	const char *p = line;
	size_t len = 0;

	if (line == NULL || out == NULL || out_size == 0)
		return -1;

	while (*p && isspace((unsigned char)*p))
		p++;
	if (strncasecmp(p, "FILE", 4) != 0 || !isspace((unsigned char)p[4]))
		return -1;
	p += 4;
	while (*p && isspace((unsigned char)*p))
		p++;

	if (*p == '"') {
		p++;
		while (p[len] && p[len] != '"' && len + 1 < out_size)
			len++;
	}
	else {
		while (p[len] && !isspace((unsigned char)p[len]) && len + 1 < out_size)
			len++;
	}

	if (len == 0)
		return -1;
	memcpy(out, p, len);
	out[len] = 0;
	cdriso_normalize_slashes(out);
	return 0;
}

// this function tries to get the .cue file of the given .bin
// the necessary data is put into the ti (trackinformation)-array
static int parsecue(const char *isofile) {
	static char		cuename[MAXPATHLEN];
	static char		filepath[MAXPATHLEN];
	static char		cue_dir[MAXPATHLEN];
	FILE			*fi;
	FILE			*ftmp = NULL;
	char			*token;
	char			time[20];
	char			*tmp;
	static char		linebuf[256], tmpb[256], dummy[256];
	unsigned int	t, mode, toc_sector;
	static struct {
		int index[2], pregap, postgap, sector_size;
	} tmpinfo[MAXTRACKS];
	int i, missing_file = 0;

	numtracks = 0;

	// copy name of the iso and change extension from .bin to .cue
	strncpy(cuename, isofile, sizeof(cuename));
	cuename[MAXPATHLEN - 1] = '\0';
	if (strlen(cuename) < 4)
		return -1;
	if (strcasecmp(cuename + strlen(cuename) - 4, ".cue") == 0) {
		// it's already open as cdHandle
		fi = cdHandle;
	}
	else {
		// If 'isofile' is a '.cd<X>' file, use it as a .cue file
		//  and don't try to search the additional .cue file
		if (strncasecmp(cuename + strlen(cuename) - 4, ".cd", 3) != 0 )
			strcpy(cuename + strlen(cuename) - 4, ".cue");

		if ((ftmp = fopen(cuename, "r")) == NULL)
			return -1;
		fi = ftmp;
	}

	// Some stupid tutorials wrongly tell users to use cdrdao to rip a
	// "bin/cue" image, which is in fact a "bin/toc" image. So let's check
	// that...
	if (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
		if (!strncmp(linebuf, "CD_ROM_XA", 9)) {
			// Don't proceed further, as this is actually a .toc file rather
			// than a .cue file.
			if (ftmp)
				fclose(ftmp);
			return parsetoc(isofile);
		}
		rewind(fi);
	}

	// build a path for files referenced in .cue
	strncpy(cue_dir, cuename, sizeof(cue_dir));
	cue_dir[sizeof(cue_dir) - 1] = '\0';
	cdriso_normalize_slashes(cue_dir);
	tmp = strrchr(cue_dir, '/');
	if (tmp != NULL)
		*tmp = 0;
	else
		strcpy(cue_dir, ".");
	filepath[0] = 0;

	memset(&ti, 0, sizeof(ti));

	while (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
		char mode_name[32];

		strncpy(dummy, linebuf, sizeof(dummy) - 1);
		dummy[sizeof(dummy) - 1] = 0;
		token = strtok(dummy, " \t\r\n");

		if (token == NULL) {
			continue;
		}

		if (!strcasecmp(token, "TRACK")) {
			if (numtracks >= MAXTRACKS - 1) {
				SysPrintf(".cue: too many tracks?\n");
				break;
			}
			numtracks++;
			memset(&tmpinfo[numtracks], -1, sizeof(tmpinfo[numtracks]));

			if (sscanf(linebuf, " %*s %u", &t) == 1 && t != numtracks)
				SysPrintf(".cue: expected track %d, got %d?\n",
					  numtracks, t);

			if (sscanf(linebuf, " %*s %u %31s", &t, mode_name) == 2 &&
			    !strcasecmp(mode_name, "AUDIO")) {
				ti[numtracks].type = CDRT_CDDA;
				tmpinfo[numtracks].sector_size = 2352;
			}
			else if (sscanf(linebuf, " %*s %u %31s", &t, mode_name) == 2 &&
			         !strncasecmp(mode_name, "MODE", 4)) {
				char *slash = strchr(mode_name, '/');
				mode = (unsigned int)strtoul(mode_name + 4, NULL, 10);
				tmpinfo[numtracks].sector_size = slash ? (int)strtoul(slash + 1, NULL, 10) : 0;
				if (mode == 0 || tmpinfo[numtracks].sector_size <= 0)
					SysPrintf(".cue: unusual TRACK mode '%s'\n", mode_name);
				ti[numtracks].type = CDRT_DATA;
			}
			else {
				SysPrintf(".cue: failed to parse TRACK\n");
				ti[numtracks].type = numtracks == 1 ? CDRT_DATA : CDRT_CDDA;
			}
			if (tmpinfo[numtracks].sector_size <= 0)
				tmpinfo[numtracks].sector_size = 2352;
		}
		else if (!strcasecmp(token, "INDEX")) {
			unsigned int index;
			char msf[3];
			if (numtracks <= 0) {
				SysPrintf(".cue: INDEX before TRACK ignored\n");
				continue;
			}
			if (sscanf(linebuf, " %*s %u %19s", &index, time) != 2) {
				SysPrintf(".cue: failed to parse INDEX\n");
				continue;
			}
			if (index > 1u)
				continue;
			tok2msf(time, msf);
			tmpinfo[numtracks].index[index] = msf2sec(msf);
		}
		else if (!strcasecmp(token, "PREGAP")) {
			if (numtracks <= 0) {
				SysPrintf(".cue: PREGAP before TRACK ignored\n");
				continue;
			}
			if (sscanf(linebuf, " %*s %19s", time) != 1) {
				SysPrintf(".cue: failed to parse PREGAP\n");
				continue;
			}
			tok2msf(time, dummy);
			tmpinfo[numtracks].pregap = msf2sec(dummy);
		}
		else if (!strcasecmp(token, "POSTGAP")) {
			if (numtracks <= 0) {
				SysPrintf(".cue: POSTGAP before TRACK ignored\n");
				continue;
			}
			if (sscanf(linebuf, " %*s %19s", time) != 1) {
				SysPrintf(".cue: failed to parse POSTGAP\n");
				continue;
			}
			tok2msf(time, dummy);
			tmpinfo[numtracks].postgap = msf2sec(dummy);
		}
		else if (!strcasecmp(token, "FILE")) {
			if (cdriso_parse_cue_file_name(linebuf, tmpb, sizeof(tmpb)) != 0) {
				SysPrintf(".cue: failed to parse FILE line\n");
				missing_file = 1;
				continue;
			}

			ti[numtracks + 1].handle = cdriso_open_cue_reference(cue_dir, tmpb,
				filepath, sizeof(filepath));
			if (ti[numtracks + 1].handle == NULL) {
				SysMessage(_("CUE references missing BIN: %s"), tmpb);
				missing_file = 1;
				continue;
			}

			SysPrintf(".cue: FILE '%s' -> '%s'\n", tmpb, filepath);
			if (numtracks + 1 > 1)
				multifile = 1;
		}
	}

	if (ftmp)
		fclose(ftmp);

	// if there are no tracks detected, then it's not a cue file
	if (!numtracks) {
		cdriso_clear_toc();
		return -1;
	}
	if (missing_file) {
		cdriso_clear_toc();
		return -1;
	}

	for (i = 1; i <= numtracks; i++) {
		if (tmpinfo[i].index[1] == -1) {
			SysPrintf(".cue: no INDEX 01 for track %d?\n", i);
			tmpinfo[i].index[1] = 0;
			if (tmpinfo[i].index[0] != -1)
				tmpinfo[i].index[1] = tmpinfo[i].index[0] + 2 * 75;
		}
	}

	// complete the actual toc
	tmpinfo[0].postgap = -1;
	ftmp = cdHandle;
	toc_sector = 2*75;
	for (i = 1; i <= numtracks; i++)
	{
		unsigned int pregap = 0, sector_size = tmpinfo[i].sector_size;
		// various ways to specify pregap in a .cue
		if (tmpinfo[i].pregap != -1)
			pregap += tmpinfo[i].pregap;
		if (tmpinfo[i-1].postgap != -1)
			pregap += tmpinfo[i-1].postgap;
		if (ti[i].handle && tmpinfo[i].index[0] != -1)
			pregap += tmpinfo[i].index[1] - tmpinfo[i].index[0];

		toc_sector += pregap;
		ti[i].start = toc_sector;
		ti[i].start_offset = tmpinfo[i].index[1] * sector_size;

		if (ti[i].handle)
			ftmp = ti[i].handle;
		if (i+1 <= numtracks && ti[i+1].handle == NULL) {
			// this track and the next one share the backing file
			ti[i].length = tmpinfo[i+1].index[1] - tmpinfo[i].index[1];
		}
		else {
			off_t file_size = get_size(ftmp);
			off_t left = file_size - (off_t)ti[i].start_offset;
			if (left > 0)
				ti[i].length = (unsigned int)(left / sector_size);
			else
				ti[i].length = 0;
		}
		toc_sector += ti[i].length;
	}

	// the data track handle is always in cdHandle
	if (ti[1].handle) {
		fclose(cdHandle);
		cdHandle = ti[1].handle;
		ti[1].handle = NULL;
		set_static_stdio_buffer(cdHandle);
	}
	return 0;
}

// this function tries to get the .ccd file of the given .img
// the necessary data is put into the ti (trackinformation)-array
static int parseccd(const char *isofile) {
	static char		ccdname[MAXPATHLEN];
	FILE			*fi;
	static char		linebuf[256];
	unsigned int	t;

	numtracks = 0;

	// copy name of the iso and change extension from .img to .ccd
	strncpy(ccdname, isofile, sizeof(ccdname));
	ccdname[MAXPATHLEN - 1] = '\0';
	if (strlen(ccdname) >= 4) {
		strcpy(ccdname + strlen(ccdname) - 4, ".ccd");
	}
	else {
		return -1;
	}

	if ((fi = fopen(ccdname, "r")) == NULL) {
		return -1;
	}

	memset(&ti, 0, sizeof(ti));

	while (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
		if (!strncmp(linebuf, "[TRACK", 6)){
			numtracks++;
		}
		else if (!strncmp(linebuf, "MODE=", 5)) {
			sscanf(linebuf, "MODE=%d", &t);
			ti[numtracks].type = (t == 0) ? CDRT_CDDA : CDRT_DATA;
		}
		else if (!strncmp(linebuf, "INDEX 1=", 8)) {
			sscanf(linebuf, "INDEX 1=%d", &t);
			ti[numtracks].start = 2 * 75 + t;
			ti[numtracks].start_offset = t * 2352;

			// If we've already seen another track, this is its end
			if (numtracks > 1) {
				t = ti[numtracks].start - ti[numtracks - 1].start;
				ti[numtracks - 1].length = t;
			}
		}
	}

	fclose(fi);

	// Fill out the last track's end based on size
	if (numtracks >= 1) {
		t = get_size(cdHandle) / 2352 - ti[numtracks].start + 2 * 75;
		ti[numtracks].length = t;
	}

	return 0;
}

// this function tries to get the .mds file of the given .mdf
// the necessary data is put into the ti (trackinformation)-array
static int parsemds(const char *isofile) {
	static char		mdsname[MAXPATHLEN];
	FILE			*fi;
	unsigned int	offset, extra_offset, l, i;
	unsigned short	s;

	numtracks = 0;

	// copy name of the iso and change extension from .mdf to .mds
	strncpy(mdsname, isofile, sizeof(mdsname));
	mdsname[MAXPATHLEN - 1] = '\0';
	if (strlen(mdsname) >= 4) {
		strcpy(mdsname + strlen(mdsname) - 4, ".mds");
	}
	else {
		return -1;
	}

	if ((fi = fopen(mdsname, "rb")) == NULL) {
		return -1;
	}

	memset(&ti, 0, sizeof(ti));

	// check if it's a valid mds file
	if (fread(&i, 1, sizeof(i), fi) != sizeof(i))
		goto fail_io;
	i = SWAP32(i);
	if (i != 0x4944454D) {
		// not an valid mds file
		fclose(fi);
		return -1;
	}

	// get offset to session block
	fseek(fi, 0x50, SEEK_SET);
	if (fread(&offset, 1, sizeof(offset), fi) != sizeof(offset))
		goto fail_io;
	offset = SWAP32(offset);

	// get total number of tracks
	offset += 14;
	fseek(fi, offset, SEEK_SET);
	if (fread(&s, 1, sizeof(s), fi) != sizeof(s))
		goto fail_io;
	s = SWAP16(s);
	numtracks = s;

	// get offset to track blocks
	fseek(fi, 4, SEEK_CUR);
	if (fread(&offset, 1, sizeof(offset), fi) != sizeof(offset))
		goto fail_io;
	offset = SWAP32(offset);

	// skip lead-in data
	while (1) {
		fseek(fi, offset + 4, SEEK_SET);
		if (fgetc(fi) < 0xA0) {
			break;
		}
		offset += 0x50;
	}

	// check if the image contains mixed subchannel data
	fseek(fi, offset + 1, SEEK_SET);
	subChanMixed = subChanRaw = (fgetc(fi) ? TRUE : FALSE);

	// read track data
	for (i = 1; i <= numtracks; i++) {
		char msf[3];
		fseek(fi, offset, SEEK_SET);

		// get the track type
		ti[i].type = (fgetc(fi) == 0xA9) ? CDRT_CDDA : CDRT_DATA;
		fseek(fi, 8, SEEK_CUR);

		// get the track starting point
		msf[0] = fgetc(fi);
		msf[1] = fgetc(fi);
		msf[2] = fgetc(fi);
		ti[i].start = msf2sec(msf);

		if (fread(&extra_offset, 1, sizeof(extra_offset), fi) != sizeof(extra_offset))
			goto fail_io;
		extra_offset = SWAP32(extra_offset);

		// get track start offset (in .mdf)
		fseek(fi, offset + 0x28, SEEK_SET);
		if (fread(&l, 1, sizeof(l), fi) != sizeof(l))
			goto fail_io;
		l = SWAP32(l);
		ti[i].start_offset = l;

		// get pregap
		fseek(fi, extra_offset, SEEK_SET);
		if (fread(&l, 1, sizeof(l), fi) != sizeof(l))
			goto fail_io;
		l = SWAP32(l);

		// get the track length
		if (fread(&l, 1, sizeof(l), fi) != sizeof(l))
			goto fail_io;
		ti[i].length = SWAP32(l);

		offset += 0x50;
	}
	fclose(fi);
	return 0;
fail_io:
#ifndef NDEBUG
	SysPrintf(_("File IO error in <%s:%s>.\n"), __FILE__, __func__);
#endif
	fclose(fi);
	return -1;
}

static int cdriso_has_ext(const char *fname, const char *ext)
{
	const char *p;

	if (fname == NULL || ext == NULL)
		return 0;
	p = strrchr(fname, '.');
	return p != NULL && strcasecmp(p, ext) == 0;
}

static const char *cdriso_metadata_tag;

static int cdriso_parse_metadata_for(const char *fname)
{
	cdriso_metadata_tag = NULL;

	/*
	 * The original desktop flow probes TOC, CCD, MDS and CUE for every image.
	 * On the ESP32-S3 port that means several extra FatFS/newlib FILE objects
	 * during the narrow boot path, and the latest crash happened while probing
	 * MDS before the selected CUE was even parsed.  Prefer the selected file's
	 * real container first, and only do sidecar probes that are plausible for
	 * the selected backing image.
	 */
	if (cdriso_has_ext(fname, ".cue") || cdriso_has_ext(fname, ".cd1") ||
	    cdriso_has_ext(fname, ".cd2") || cdriso_has_ext(fname, ".cd3") ||
	    cdriso_has_ext(fname, ".cd4")) {
		if (parsecue(fname) == 0) { cdriso_metadata_tag = "[+cue]"; return 1; }
		return 0;
	}

	if (cdriso_has_ext(fname, ".toc")) {
		if (parsetoc(fname) == 0) { cdriso_metadata_tag = "[+toc]"; return 1; }
		return 0;
	}

	if (cdriso_has_ext(fname, ".ccd")) {
		if (parseccd(fname) == 0) { cdriso_metadata_tag = "[+ccd]"; return 1; }
		return 0;
	}

	if (cdriso_has_ext(fname, ".mds") || cdriso_has_ext(fname, ".mdf")) {
		if (parsemds(fname) == 0) { cdriso_metadata_tag = "[+mds]"; return 1; }
		return 0;
	}

	if (cdriso_has_ext(fname, ".bin") || cdriso_has_ext(fname, ".img")) {
		if (parsecue(fname) == 0) {
			cdriso_metadata_tag = "[+cue]";
			return 1;
		}
		if (cdriso_has_ext(fname, ".img") && parseccd(fname) == 0) {
			cdriso_metadata_tag = "[+ccd]";
			return 1;
		}
		if (cdriso_has_ext(fname, ".bin") && parsetoc(fname) == 0) {
			cdriso_metadata_tag = "[+toc]";
			return 1;
		}
	}

	return 0;
}

static int handlepbp(const char *isofile) {
	struct {
		unsigned int sig;
		unsigned int dontcare[8];
		unsigned int psar_offs;
	} pbp_hdr;
	struct {
		unsigned char type;
		unsigned char pad0;
		unsigned char track;
		char index0[3];
		char pad1;
		char index1[3];
	} toc_entry;
	struct {
		unsigned int offset;
		unsigned int size;
		unsigned int dontcare[6];
	} index_entry;
	char psar_sig[11];
	off_t psisoimg_offs, cdimg_base;
	unsigned int cd_length;
	unsigned int offsettab[8];
	unsigned int psar_offs, index_entry_size, index_entry_offset;
	const char *ext = NULL;
	int i, ret;

	if (strlen(isofile) >= 4)
		ext = isofile + strlen(isofile) - 4;
	if (ext == NULL || strcasecmp(ext, ".pbp") != 0)
		return -1;

	numtracks = 0;

	ret = fread(&pbp_hdr, 1, sizeof(pbp_hdr), cdHandle);
	if (ret != sizeof(pbp_hdr)) {
		SysPrintf("failed to read pbp\n");
		goto fail_io;
	}

	psar_offs = SWAP32(pbp_hdr.psar_offs);

	ret = fseeko(cdHandle, psar_offs, SEEK_SET);
	if (ret != 0) {
		SysPrintf("failed to seek to %x\n", psar_offs);
		goto fail_io;
	}

	psisoimg_offs = psar_offs;
	if (fread(psar_sig, 1, sizeof(psar_sig), cdHandle) != sizeof(psar_sig))
		goto fail_io;
	psar_sig[10] = 0;
	if (strcmp(psar_sig, "PSTITLEIMG") == 0) {
		// multidisk image?
		ret = fseeko(cdHandle, psar_offs + 0x200, SEEK_SET);
		if (ret != 0) {
			SysPrintf("failed to seek to %x\n", psar_offs + 0x200);
			goto fail_io;
		}

		if (fread(&offsettab, 1, sizeof(offsettab), cdHandle) != sizeof(offsettab)) {
			SysPrintf("failed to read offsettab\n");
			goto fail_io;
		}

		for (i = 0; i < sizeof(offsettab) / sizeof(offsettab[0]); i++) {
			if (offsettab[i] == 0)
				break;
		}
		cdrIsoMultidiskCount = i;
		if (cdrIsoMultidiskCount == 0) {
			SysPrintf("multidisk eboot has 0 images?\n");
			goto fail_io;
		}

		if (cdrIsoMultidiskSelect >= cdrIsoMultidiskCount)
			cdrIsoMultidiskSelect = 0;

		psisoimg_offs += SWAP32(offsettab[cdrIsoMultidiskSelect]);

		ret = fseeko(cdHandle, psisoimg_offs, SEEK_SET);
		if (ret != 0) {
			SysPrintf("failed to seek to %llx\n", (long long)psisoimg_offs);
			goto fail_io;
		}

		if (fread(psar_sig, 1, sizeof(psar_sig), cdHandle) != sizeof(psar_sig))
			goto fail_io;
		psar_sig[10] = 0;
	}

	if (strcmp(psar_sig, "PSISOIMG00") != 0) {
		SysPrintf("bad psar_sig: %s\n", psar_sig);
		goto fail_io;
	}

	// seek to TOC
	ret = fseeko(cdHandle, psisoimg_offs + 0x800, SEEK_SET);
	if (ret != 0) {
		SysPrintf("failed to seek to %llx\n", (long long)psisoimg_offs + 0x800);
		goto fail_io;
	}

	// first 3 entries are special
	fseek(cdHandle, sizeof(toc_entry), SEEK_CUR);
	if (fread(&toc_entry, 1, sizeof(toc_entry), cdHandle) != sizeof(toc_entry))
		goto fail_io;
	numtracks = btoi(toc_entry.index1[0]);

	if (fread(&toc_entry, 1, sizeof(toc_entry), cdHandle) != sizeof(toc_entry))
		goto fail_io;
	cd_length = btoi(toc_entry.index1[0]) * 60 * 75 +
		btoi(toc_entry.index1[1]) * 75 + btoi(toc_entry.index1[2]);

	for (i = 1; i <= numtracks; i++) {
		char msf[3];
		if (fread(&toc_entry, 1, sizeof(toc_entry), cdHandle) != sizeof(toc_entry))
			goto fail_io;

		ti[i].type = (toc_entry.type == 1) ? CDRT_CDDA : CDRT_DATA;

		ti[i].start_offset = btoi(toc_entry.index0[0]) * 60 * 75 +
			btoi(toc_entry.index0[1]) * 75 + btoi(toc_entry.index0[2]);
		ti[i].start_offset *= 2352;
		msf[0] = btoi(toc_entry.index1[0]);
		msf[1] = btoi(toc_entry.index1[1]);
		msf[2] = btoi(toc_entry.index1[2]);
		ti[i].start = msf2sec(msf);

		if (i > 1)
			ti[i - 1].length = ti[i].start - ti[i - 1].start;
	}
	ti[numtracks].length = cd_length - ti[numtracks].start_offset / 2352;

	// seek to ISO index
	ret = fseeko(cdHandle, psisoimg_offs + 0x4000, SEEK_SET);
	if (ret != 0) {
		SysPrintf("failed to seek to ISO index\n");
		goto fail_io;
	}

	compr_img = calloc(1, sizeof(*compr_img));
	if (compr_img == NULL)
		goto fail_io;

	compr_img->block_shift = 4;
	compr_img->current_block = (unsigned int)-1;

	compr_img->index_len = (0x100000 - 0x4000) / sizeof(index_entry);
	compr_img->index_table = malloc((compr_img->index_len + 1) * sizeof(compr_img->index_table[0]));
	if (compr_img->index_table == NULL)
		goto fail_io;

	cdimg_base = psisoimg_offs + 0x100000;
	for (i = 0; i < compr_img->index_len; i++) {
		ret = fread(&index_entry, 1, sizeof(index_entry), cdHandle);
		if (ret != sizeof(index_entry)) {
			SysPrintf("failed to read index_entry #%d\n", i);
			goto fail_index;
		}

		index_entry_size = SWAP32(index_entry.size);
		index_entry_offset = SWAP32(index_entry.offset);

		if (index_entry_size == 0)
			break;

		compr_img->index_table[i] = cdimg_base + index_entry_offset;
	}
	compr_img->index_table[i] = cdimg_base + index_entry_offset + index_entry_size;

	return 0;

fail_index:
	free(compr_img->index_table);
	compr_img->index_table = NULL;
	goto done;

fail_io:
	SysPrintf(_("File IO error in <%s:%s>.\n"), __FILE__, __func__);
	rewind(cdHandle);

done:
	if (compr_img != NULL) {
		free(compr_img);
		compr_img = NULL;
	}
	return -1;
}

static int handlecbin(const char *isofile) {
	struct
	{
		char magic[4];
		unsigned int header_size;
		unsigned long long total_bytes;
		unsigned int block_size;
		unsigned char ver;		// 1
		unsigned char align;
		unsigned char rsv_06[2];
	} ciso_hdr;
	const char *ext = NULL;
	unsigned int *index_table = NULL;
	unsigned int index = 0, plain;
	int i, ret;

	if (strlen(isofile) >= 5)
		ext = isofile + strlen(isofile) - 5;
	if (ext == NULL || (strcasecmp(ext + 1, ".cbn") != 0 && strcasecmp(ext, ".cbin") != 0))
		return -1;

	ret = fread(&ciso_hdr, 1, sizeof(ciso_hdr), cdHandle);
	if (ret != sizeof(ciso_hdr)) {
		SysPrintf("failed to read ciso header\n");
		goto fail_io;
	}

	if (strncmp(ciso_hdr.magic, "CISO", 4) != 0 || ciso_hdr.total_bytes <= 0 || ciso_hdr.block_size <= 0) {
		SysPrintf("bad ciso header\n");
		goto fail_io;
	}
	if (ciso_hdr.header_size != 0 && ciso_hdr.header_size != sizeof(ciso_hdr)) {
		ret = fseeko(cdHandle, ciso_hdr.header_size, SEEK_SET);
		if (ret != 0) {
			SysPrintf("failed to seek to %x\n", ciso_hdr.header_size);
			goto fail_io;
		}
	}

	compr_img = calloc(1, sizeof(*compr_img));
	if (compr_img == NULL)
		goto fail_io;

	compr_img->block_shift = 0;
	compr_img->current_block = (unsigned int)-1;

	compr_img->index_len = ciso_hdr.total_bytes / ciso_hdr.block_size;
	index_table = malloc((compr_img->index_len + 1) * sizeof(index_table[0]));
	if (index_table == NULL)
		goto fail_io;

	ret = fread(index_table, sizeof(index_table[0]), compr_img->index_len, cdHandle);
	if (ret != compr_img->index_len) {
		SysPrintf("failed to read index table\n");
		goto fail_index;
	}

	compr_img->index_table = malloc((compr_img->index_len + 1) * sizeof(compr_img->index_table[0]));
	if (compr_img->index_table == NULL)
		goto fail_index;

	for (i = 0; i < compr_img->index_len + 1; i++) {
		index = index_table[i];
		plain = index & 0x80000000;
		index &= 0x7fffffff;
		compr_img->index_table[i] = (off_t)index << ciso_hdr.align;
		if (plain)
			compr_img->index_table[i] |= OFF_T_MSB;
	}

	// fixup the toc and the last track length
	if (numtracks == 0) {
		numtracks = 1;
		ti[1].type = CDRT_DATA;
		ti[1].start_offset = 0;
		ti[1].start = 2 * 75;
	}
	ti[numtracks].length = (ciso_hdr.total_bytes - ti[numtracks].start_offset) / 2352;
	free(index_table);
	return 0;

fail_index:
	free(index_table);
fail_io:
	if (compr_img != NULL) {
		free(compr_img);
		compr_img = NULL;
	}
	rewind(cdHandle);
	return -1;
}

#ifdef HAVE_CHD
static int handlechd(const char *isofile) {
	int frame_offset = 150;
	int file_offset = 0;
	int is_chd_ext = 0;
	chd_error err;

	if (strlen(isofile) >= 3) {
		const char *ext = isofile + strlen(isofile) - 3;
		is_chd_ext = !strcasecmp(ext, "chd");
	}
	chd_img = calloc(1, sizeof(*chd_img));
	if (chd_img == NULL)
		goto fail_io;

	err = chd_open_file(cdHandle, CHD_OPEN_READ, NULL, &chd_img->chd);
	if (err != CHDERR_NONE) {
		if (is_chd_ext)
			SysPrintf("chd_open: %d\n", err);
		goto fail_io;
	}

	if (Config.CHD_Precache && (chd_precache(chd_img->chd) != CHDERR_NONE))
		goto fail_io;

	chd_img->header = chd_get_header(chd_img->chd);
	chd_img->sector_size = chd_img->header->unitbytes;
	if (chd_img->sector_size == 0)
		chd_img->sector_size = CD_FRAMESIZE_RAW + SUB_FRAMESIZE;
	if (chd_img->sector_size < CD_FRAMESIZE_RAW ||
	    chd_img->header->hunkbytes < chd_img->sector_size ||
	    chd_img->header->hunkbytes % chd_img->sector_size != 0) {
		SysPrintf("chd: unsupported hunk/unit size hunk=%u unit=%u\n",
			(unsigned)chd_img->header->hunkbytes, (unsigned)chd_img->sector_size);
		goto fail_io;
	}

	chd_img->buffer = malloc(chd_img->header->hunkbytes * 2);
	if (chd_img->buffer == NULL)
		goto fail_io;

	chd_img->sectors_per_hunk = chd_img->header->hunkbytes / chd_img->sector_size;
	if (chd_img->sectors_per_hunk == 0)
		goto fail_io;
	chd_img->current_hunk[0] = (unsigned int)-1;
	chd_img->current_hunk[1] = (unsigned int)-1;

	cddaBigEndian = TRUE;

	numtracks = 0;
	memset(ti, 0, sizeof(ti));

	while (1)
	{
		struct {
			char type[64];
			char subtype[32];
			char pgtype[32];
			char pgsub[32];
			uint32_t track;
			uint32_t frames;
			uint32_t pregap;
			uint32_t postgap;
		} md = {};
		char meta[256];
		uint32_t meta_size = 0;
		int track_i = 0;
		int frames_i = 0;
		int pregap_i = 0;
		int postgap_i = 0;
		int parsed = 0;

		if (chd_get_metadata(chd_img->chd, CDROM_TRACK_METADATA2_TAG, numtracks, meta, sizeof(meta), &meta_size, NULL, NULL) == CHDERR_NONE) {
			parsed = sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
				&track_i, md.type, md.subtype, &frames_i, &pregap_i, md.pgtype, md.pgsub, &postgap_i);
			if (parsed != 8) {
				SysPrintf("chd: malformed v2 track metadata '%s' parsed=%d\n", meta, parsed);
				goto fail_io;
			}
		}
		else if (chd_get_metadata(chd_img->chd, CDROM_TRACK_METADATA_TAG, numtracks, meta, sizeof(meta), &meta_size, NULL, NULL) == CHDERR_NONE) {
			parsed = sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%31s FRAMES:%d",
				&track_i, md.type, md.subtype, &frames_i);
			if (parsed != 4) {
				SysPrintf("chd: malformed track metadata '%s' parsed=%d\n", meta, parsed);
				goto fail_io;
			}
		}
		else
			break;

		if (track_i < 0 || frames_i < 0 || pregap_i < 0 || postgap_i < 0) {
			SysPrintf("chd: negative track metadata in '%s'\n", meta);
			goto fail_io;
		}
		md.track = (uint32_t)track_i;
		md.frames = (uint32_t)frames_i;
		md.pregap = (uint32_t)pregap_i;
		md.postgap = (uint32_t)postgap_i;

		SysPrintf("chd: %s\n", meta);

		if (md.track == 0 || md.track >= MAXTRACKS) {
			SysPrintf("chd: invalid track number %u\n", (unsigned)md.track);
			goto fail_io;
		}
		if (md.frames == 0) {
			SysPrintf("chd: track %u has zero frames\n", (unsigned)md.track);
			goto fail_io;
		}
		if (md.track != (uint32_t)numtracks + 1)
			SysPrintf("chd: non-sequential track metadata, expected %d got %u\n", numtracks + 1, (unsigned)md.track);

		if (md.track == 1) {
			if (!strncmp(md.subtype, "RW", 2)) {
				subChanMixed = TRUE;
				if (!strcmp(md.subtype, "RW_RAW"))
					subChanRaw = TRUE;
			}
		}

		ti[md.track].type = !strncmp(md.type, "AUDIO", 5) ? CDRT_CDDA : CDRT_DATA;

		ti[md.track].start = frame_offset + md.pregap;
		ti[md.track].length = md.frames;

		ti[md.track].start_offset = file_offset + md.pregap;

		// XXX: what about postgap?
		frame_offset += md.frames;
		file_offset += md.frames;
		numtracks++;
	}

	if (numtracks)
		return 0;

fail_io:
	if (chd_img != NULL) {
		if (chd_img->chd != NULL)
			chd_close(chd_img->chd);
		free(chd_img->buffer);
		free(chd_img);
		chd_img = NULL;
	}
	return -1;
}
#endif

// this function tries to get the .sub file of the given .img
static int opensubfile(const char *isoname) {
	static char	subname[MAXPATHLEN];

	// copy name of the iso and change extension from .img to .sub
	strncpy(subname, isoname, sizeof(subname));
	subname[MAXPATHLEN - 1] = '\0';
	if (strlen(subname) >= 4) {
		strcpy(subname + strlen(subname) - 4, ".sub");
	}
	else {
		return -1;
	}

	subHandle = fopen(subname, "rb");
	if (subHandle == NULL)
		return -1;

	return 0;
}

static int opensbifile(const char *isoname) {
	static char	sbiname[MAXPATHLEN];
	char		disknum[MAXPATHLEN] = "0";

	strncpy(sbiname, isoname, sizeof(sbiname));
	sbiname[MAXPATHLEN - 1] = '\0';
	if (strlen(sbiname) >= 4) {
		if (cdrIsoMultidiskCount > 1) {
			sprintf(disknum, "_%i.sbi", cdrIsoMultidiskSelect + 1);
			strcpy(sbiname + strlen(sbiname) - 4, disknum);
		}
		else
			strcpy(sbiname + strlen(sbiname) - 4, ".sbi");
	}
	else {
		return -1;
	}

	return LoadSBI(sbiname, ti[1].length);
}

static int cdread_normal(FILE *f, unsigned int base, void *dest, int sector)
{
	int ret = cdread_cached(f, base, dest, sector, CD_FRAMESIZE_RAW, 0, CD_FRAMESIZE_RAW);
	if (ret > 0)
		return ret;

	// often happens in cdda gaps of a split cue/bin, so not logged
	SysPrintf("File IO error errno=%d ferror=%d base=%u sector=%d\n", errno, f ? ferror(f) : -1, base, sector);
	return -1;
}

static int cdread_sub_mixed(FILE *f, unsigned int base, void *dest, int sector)
{
	int ret;

	if (!f)
		return -1;
	if (!dest)
		dest = cdbuffer;
	if (fseeko(f, base + sector * (CD_FRAMESIZE_RAW + SUB_FRAMESIZE), SEEK_SET))
		goto fail_io;
	ret = fread(dest, 1, CD_FRAMESIZE_RAW, f);
	if (ret <= 0)
		goto fail_io;
	return ret;

fail_io:
	//SysPrintf("File IO error errno=%d ferror=%d base=%u sector=%d\n", errno, ferror(f), base, sector);
	return -1;
}

static int cdread_sub_sub_mixed(FILE *f, int sector, void *buffer)
{
	if (!f)
		return -1;
	if (fseeko(f, sector * (CD_FRAMESIZE_RAW + SUB_FRAMESIZE) + CD_FRAMESIZE_RAW, SEEK_SET))
		goto fail_io;
	if (fread(buffer, 1, SUB_FRAMESIZE, f) != SUB_FRAMESIZE)
		goto fail_io;

	return 0;

fail_io:
	SysPrintf("subchannel: file IO error %d, sector %d\n", errno, sector);
	return -1;
}

static int uncompress2_pcsx(void *out, unsigned long *out_size, void *in, unsigned long in_size)
{
	static z_stream z;
	int ret = 0;

	if (z.zalloc == NULL) {
		// XXX: one-time leak here..
		z.next_in = Z_NULL;
		z.avail_in = 0;
		z.zalloc = Z_NULL;
		z.zfree = Z_NULL;
		z.opaque = Z_NULL;
		ret = inflateInit2(&z, -15);
	}
	else
		ret = inflateReset(&z);
	if (ret != Z_OK)
		return ret;

	z.next_in = in;
	z.avail_in = in_size;
	z.next_out = out;
	z.avail_out = *out_size;

	ret = inflate(&z, Z_NO_FLUSH);
	//inflateEnd(&z);

	*out_size -= z.avail_out;
	return ret == 1 ? 0 : ret;
}

static int cdread_compressed(FILE *f, unsigned int base, void *dest, int sector)
{
	unsigned long cdbuffer_size, cdbuffer_size_expect;
	unsigned int size;
	int is_compressed;
	off_t start_byte;
	int ret, block;

	if (!cdHandle)
		return -1;
	if (base)
		sector += base / 2352;

	block = sector >> compr_img->block_shift;
	compr_img->sector_in_blk = sector & ((1 << compr_img->block_shift) - 1);

	if (block == compr_img->current_block) {
		//printf("hit sect %d\n", sector);
		goto finish;
	}

	if (sector >= compr_img->index_len * 16) {
		SysPrintf("sector %d is past img end\n", sector);
		return -1;
	}

	start_byte = compr_img->index_table[block] & ~OFF_T_MSB;
	if (fseeko(cdHandle, start_byte, SEEK_SET) != 0) {
		SysPrintf("seek error for block %d at %llx: ",
			block, (long long)start_byte);
		perror(NULL);
		return -1;
	}

	is_compressed = !(compr_img->index_table[block] & OFF_T_MSB);
	size = (compr_img->index_table[block + 1] & ~OFF_T_MSB) - start_byte;
	if (size > sizeof(compr_img->buff_compressed)) {
		SysPrintf("block %d is too large: %u\n", block, size);
		return -1;
	}

	if (fread(is_compressed ? compr_img->buff_compressed : compr_img->buff_raw[0],
				1, size, cdHandle) != size) {
		SysPrintf("read error for block %d at %lx: ", block, (long)start_byte);
		perror(NULL);
		return -1;
	}

	if (is_compressed) {
		cdbuffer_size_expect = sizeof(compr_img->buff_raw[0]) << compr_img->block_shift;
		cdbuffer_size = cdbuffer_size_expect;
		ret = uncompress2_pcsx(compr_img->buff_raw[0], &cdbuffer_size, compr_img->buff_compressed, size);
		if (ret != 0) {
			SysPrintf("uncompress failed with %d for block %d, sector %d\n",
					ret, block, sector);
			return -1;
		}
		if (cdbuffer_size != cdbuffer_size_expect)
			SysPrintf("cdbuffer_size: %lu != %lu, sector %d\n", cdbuffer_size,
					cdbuffer_size_expect, sector);
	}

	// done at last!
	compr_img->current_block = block;

finish:
	if (dest != NULL)
		memcpy(dest, compr_img->buff_raw[compr_img->sector_in_blk],
			CD_FRAMESIZE_RAW);
	return CD_FRAMESIZE_RAW;
}

#ifdef HAVE_CHD
static unsigned char *chd_get_sector(unsigned int current_buffer, unsigned int sector_in_hunk)
{
	return chd_img->buffer
		+ current_buffer * chd_img->header->hunkbytes
		+ sector_in_hunk * chd_img->sector_size;
}

static int cdread_chd(FILE *f, unsigned int base, void *dest, int sector)
{
	unsigned int lba;
	int hunk;

	(void)f;
	if (sector < 0 || chd_img == NULL || chd_img->buffer == NULL ||
	    chd_img->sectors_per_hunk == 0)
		return -1;

	lba = (unsigned int)sector + base;
	hunk = lba / chd_img->sectors_per_hunk;
	chd_img->sector_in_hunk = lba % chd_img->sectors_per_hunk;

	if (hunk == chd_img->current_hunk[0])
		chd_img->current_buffer = 0;
	else if (hunk == chd_img->current_hunk[1])
		chd_img->current_buffer = 1;
	else
	{
		chd_error err = chd_read(chd_img->chd, hunk, chd_img->buffer +
			chd_img->current_buffer * chd_img->header->hunkbytes);
		if (err != CHDERR_NONE) {
			SysPrintf("chd_read failed for hunk %d: %d\n", hunk, err);
			return -1;
		}
		chd_img->current_hunk[chd_img->current_buffer] = hunk;
	}

	if (dest != NULL)
		memcpy(dest, chd_get_sector(chd_img->current_buffer, chd_img->sector_in_hunk),
			CD_FRAMESIZE_RAW);
	return CD_FRAMESIZE_RAW;
}

static int cdread_sub_chd(FILE *f, int sector, void *buffer_ptr)
{
	unsigned int sector_in_hunk;
	unsigned int buffer;
	int hunk;

	(void)f;
	if (sector < 0 || !subChanMixed || chd_img == NULL || chd_img->buffer == NULL ||
	    chd_img->sectors_per_hunk == 0 ||
	    chd_img->sector_size < CD_FRAMESIZE_RAW + SUB_FRAMESIZE)
		return -1;

	hunk = (unsigned int)sector / chd_img->sectors_per_hunk;
	sector_in_hunk = (unsigned int)sector % chd_img->sectors_per_hunk;

	if (hunk == chd_img->current_hunk[0])
		buffer = 0;
	else if (hunk == chd_img->current_hunk[1])
		buffer = 1;
	else
	{
		chd_error err;
		buffer = chd_img->current_buffer ^ 1;
		err = chd_read(chd_img->chd, hunk, chd_img->buffer +
			buffer * chd_img->header->hunkbytes);
		if (err != CHDERR_NONE) {
			SysPrintf("chd_read sub failed for hunk %d: %d\n", hunk, err);
			return -1;
		}
		chd_img->current_hunk[buffer] = hunk;
	}

	memcpy(buffer_ptr, chd_get_sector(buffer, sector_in_hunk) + CD_FRAMESIZE_RAW, SUB_FRAMESIZE);
	return 0;
}
#endif

static int cdread_2048(FILE *f, unsigned int base, void *dest, int sector)
{
	unsigned char *dst = dest ? dest : cdbuffer;
	int ret;

	ret = cdread_cached(f, base, dst, sector, 2048, 12 * 2, 2048);
	if (ret <= 0) {
		SysPrintf("2048-sector read failed errno=%d ferror=%d base=%u sector=%d\n",
			errno, f ? ferror(f) : -1, base, sector);
		return -1;
	}

	// not really necessary, fake mode 2 header
	memset(dst, 0, 12 * 2);
	sec2msf(sector + 2 * 75, dst + 12);
	dst[12 + 0] = itob(dst[12 + 0]);
	dst[12 + 1] = itob(dst[12 + 1]);
	dst[12 + 2] = itob(dst[12 + 2]);
	dst[12 + 3] = 1;

	return ret;
}

static void * ISOgetBuffer_normal(void) {
       return cdbuffer + 12;
}

static void * ISOgetBuffer_compr(void) {
       return compr_img->buff_raw[compr_img->sector_in_blk] + 12;
}

#ifdef HAVE_CHD
static void * ISOgetBuffer_chd(void) {
       return chd_get_sector(chd_img->current_buffer, chd_img->sector_in_hunk) + 12;
}
#endif

void * (*ISOgetBuffer)(void) = ISOgetBuffer_normal;

static void PrintTracks(void) {
	unsigned char msfe[3];
	int i;

	for (i = 1; i <= numtracks; i++) {
		unsigned char start[3], length[3];
		sec2msf(ti[i].start, start);
		sec2msf(ti[i].length, length);
		SysPrintf(_("Track %.2d %s - Start %.2d:%.2d:%.2d, Length %.2d:%.2d:%.2d\n"),
			i,  (ti[i].type == CDRT_DATA ? "DATA " :
			    (ti[i].type == CDRT_CDDA ? "AUDIO" : "UNKNOWN")),
			start[0], start[1], start[2], length[0], length[1], length[2]);
	}
	i = ti[numtracks].start + ti[numtracks].length;
	sec2msf(i, msfe);
	SysPrintf("End %.2d:%.2d:%.2d (sector %d)\n", msfe[0], msfe[1], msfe[2], i);
}

// This function is invoked by the front-end when opening an ISO
// file for playback
int ISOopen(const char *fname)
{
	boolean isMode1ISO = FALSE;
	boolean parsed_metadata = FALSE;
	boolean used_raw_fallback = FALSE;
	boolean is_metadata_file = FALSE;
	boolean is_chd_file = FALSE;
	boolean is_pbp_file = FALSE;
	char alt_bin_filename[MAXPATHLEN];
	const char *bin_filename;
	const char *ext;
	char image_str[1024];
	off_t size_main;

	if (fname == NULL || fname[0] == 0) {
		SysMessage("No CD image filename supplied.");
		return -1;
	}

	if (cdHandle || subHandle || compr_img || chd_img || numtracks > 0) {
		SysPrintf("cdriso: closing stale image before opening %s\n", fname);
		ISOclose();
	}
	cdriso_invalidate_read_cache();

	ext = strrchr(fname, '.');
	is_chd_file = ext && strcasecmp(ext, ".chd") == 0;
	is_pbp_file = ext && strcasecmp(ext, ".pbp") == 0;
	is_metadata_file = ext && (
		strcasecmp(ext, ".cue") == 0 ||
		strcasecmp(ext, ".cd1") == 0 ||
		strcasecmp(ext, ".cd2") == 0 ||
		strcasecmp(ext, ".cd3") == 0 ||
		strcasecmp(ext, ".cd4") == 0 ||
		strcasecmp(ext, ".toc") == 0 ||
		strcasecmp(ext, ".ccd") == 0 ||
		strcasecmp(ext, ".mds") == 0);

	if (ext && strcasecmp(ext, ".ecm") == 0) {
		SysMessage("ECM-compressed PS1 images are not supported; decompress to BIN/CUE first: %s", fname);
		return -1;
	}

#ifndef HAVE_CHD
	if (is_chd_file) {
		SysMessage("CHD support is not enabled in this PSX build. Use BIN/CUE for now.");
		return -1;
	}
#endif

	cdHandle = fopen(fname, "rb");
	if (cdHandle == NULL) {
		SysMessage(_("Could't open '%s' for reading: %s\n"),
			fname, strerror(errno));
		return -1;
	}
	set_static_stdio_buffer(cdHandle);
	size_main = get_size(cdHandle);

	snprintf(image_str, sizeof(image_str) - 6*4 - 1,
		"Loaded CD Image: %s", fname);

	cddaBigEndian = FALSE;
	subChanMixed = FALSE;
	subChanRaw = FALSE;
	cdrIsoMultidiskCount = 1;
	multifile = 0;

	ISOgetBuffer = ISOgetBuffer_normal;
	cdimg_read_func = cdread_normal;
	cdimg_read_sub_func = NULL;

	if (cdriso_parse_metadata_for(fname)) {
		strcat(image_str, cdriso_metadata_tag ? cdriso_metadata_tag : "[+meta]");
		parsed_metadata = TRUE;
	}

	if (parsed_metadata && cdHandle)
		size_main = get_size(cdHandle);

	if (is_pbp_file && handlepbp(fname) == 0) {
		strcat(image_str, "[+pbp]");
		ISOgetBuffer = ISOgetBuffer_compr;
		cdimg_read_func = cdread_compressed;
	}
	else if (is_pbp_file) {
		SysMessage("Failed to open PBP image: %s", fname);
		goto fail;
	}
	else if (handlecbin(fname) == 0) {
		strcat(image_str, "[+cbin]");
		ISOgetBuffer = ISOgetBuffer_compr;
		cdimg_read_func = cdread_compressed;
	}
#ifdef HAVE_CHD
	else if (is_chd_file && handlechd(fname) == 0) {
		strcat(image_str, "[+chd]");
		ISOgetBuffer = ISOgetBuffer_chd;
		cdimg_read_func = cdread_chd;
		cdimg_read_sub_func = cdread_sub_chd;
	}
	else if (is_chd_file) {
		SysMessage("Failed to open CHD image: %s", fname);
		goto fail;
	}
#endif

	if (!subChanMixed && opensubfile(fname) == 0) {
		strcat(image_str, "[+sub]");
	}
	if (opensbifile(fname) == 0) {
		strcat(image_str, "[+sbi]");
	}

	// Maybe user selected a metadata file instead of the main .bin/.img.
	// Only do this when no TOC/CUE/CCD/MDS parser already claimed the image;
	// otherwise a valid .cue can be accidentally overwritten by a same-basename .bin.
	bin_filename = fname;
	if (!is_metadata_file && !parsed_metadata && cdHandle && size_main < 2352 * 0x10) {
		static const char *exts[] = { ".bin", ".BIN", ".img", ".IMG" };
		FILE *tmpf = NULL;
		size_t i;
		char *p;

		strncpy(alt_bin_filename, bin_filename, sizeof(alt_bin_filename));
		alt_bin_filename[MAXPATHLEN - 1] = '\0';
		if (strlen(alt_bin_filename) >= 4) {
			p = alt_bin_filename + strlen(alt_bin_filename) - 4;
			for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
				strcpy(p, exts[i]);
				tmpf = fopen(alt_bin_filename, "rb");
				if (tmpf != NULL)
					break;
			}
		}
		if (tmpf != NULL) {
			bin_filename = alt_bin_filename;
			fclose(cdHandle);
			cdHandle = tmpf;
			cdriso_invalidate_read_cache();
			set_static_stdio_buffer(cdHandle);
			size_main = get_size(cdHandle);
			used_raw_fallback = TRUE;
			cdriso_clear_toc();
			strcat(image_str, "[raw-fallback]");
		}
	}

	if (is_metadata_file && !parsed_metadata && !used_raw_fallback) {
		SysMessage("Disc metadata file did not resolve to a readable BIN/IMG: %s", fname);
		goto fail;
	}

	// guess whether it is mode1/2048
	if (cdHandle && cdimg_read_func == cdread_normal && size_main % 2048 == 0) {
		unsigned int modeTest = 0;
		rewind(cdHandle);
		if (!fread(&modeTest, sizeof(modeTest), 1, cdHandle)) {
			SysPrintf(_("File IO error in <%s:%s>.\n"), __FILE__, __func__);
		}
		if (SWAP32(modeTest) != 0xffffff00) {
			strcat(image_str, "[2048]");
			isMode1ISO = TRUE;
		}
	}
	if (cdHandle && numtracks == 0) {
		// assume some metadata-less format
		numtracks = 1;
		ti[1].type = CDRT_DATA;
		ti[1].start_offset = 0;
		ti[1].start = 2 * 75;
		ti[1].length = isMode1ISO ? size_main / 2048u : size_main / 2352u;
	}

	if (numtracks <= 0 || ti[1].length == 0) {
		SysMessage("Disc image has no readable data track: %s", fname);
		goto fail;
	}

	if (subChanMixed && cdimg_read_func == cdread_normal) {
		cdimg_read_func = cdread_sub_mixed;
		cdimg_read_sub_func = cdread_sub_sub_mixed;
	}
	else if (isMode1ISO) {
		cdimg_read_func = cdread_2048;
		cdimg_read_sub_func = NULL;
	}

	SysPrintf("%s (%lld bytes).\n", image_str, (long long)size_main);
	PrintTracks();

	return 0;

fail:
	ISOclose();
	return -1;
}

int ISOclose(void)
{
	if (cdHandle != NULL) {
		fclose(cdHandle);
		cdHandle = NULL;
	}
	if (subHandle != NULL) {
		fclose(subHandle);
		subHandle = NULL;
	}

	if (compr_img != NULL) {
		free(compr_img->index_table);
		free(compr_img);
		compr_img = NULL;
	}

#ifdef HAVE_CHD
	if (chd_img != NULL) {
		chd_close(chd_img->chd);
		free(chd_img->buffer);
		free(chd_img);
		chd_img = NULL;
	}
#endif

	cdriso_close_track_handles();
	numtracks = 0;
	ti[1].type = CDRT_UNKNOWN;
	UnloadSBI();

	memset(cdbuffer, 0, sizeof(cdbuffer));
	cdriso_invalidate_read_cache();
	ISOgetBuffer = ISOgetBuffer_normal;

	return 0;
}

int ISOinit(void)
{
	if (cdHandle || subHandle || compr_img || chd_img) {
		SysPrintf("ISOinit: closing stale CD image state\n");
		ISOclose();
	}
	numtracks = 0;

	return 0; // do nothing
}

int ISOshutdown(void)
{
	return ISOclose();
}

// return Starting and Ending Track
// buffer:
//  byte 0 - start track
//  byte 1 - end track
int ISOgetTN(unsigned char *buffer)
{
	buffer[0] = 1;

	if (numtracks > 0) {
		buffer[1] = numtracks;
	}
	else {
		buffer[1] = 1;
	}

	return 0;
}

// return Track Time
// buffer:
//  byte 0 - minute
//  byte 1 - second
//  byte 2 - frame
int ISOgetTD(int track, unsigned char *buffer)
{
	if (track == 0) {
		sec2msf(ti[numtracks].start + ti[numtracks].length, buffer);
	}
	else if (numtracks > 0 && track <= numtracks) {
		sec2msf(ti[track].start, buffer);
	}
	else {
		buffer[2] = 0;
		buffer[1] = 2;
		buffer[0] = 0;
	}

	return 0;
}

// decode 'raw' subchannel data ripped by cdrdao
static void DecodeRawSubData(unsigned char *subbuffer) {
	unsigned char subQData[12];
	int i;

	memset(subQData, 0, sizeof(subQData));

	for (i = 0; i < 8 * 12; i++) {
		if (subbuffer[i] & (1 << 6)) { // only subchannel Q is needed
			subQData[i >> 3] |= (1 << (7 - (i & 7)));
		}
	}

	memcpy(&subbuffer[12], subQData, 12);
}

// read track
// time: byte 0 - minute; byte 1 - second; byte 2 - frame (non-bcd)
// buf: if NULL, data is kept in internal buffer accessible by ISOgetBuffer()
int ISOreadTrack(const unsigned char *time, void *buf)
{
	int sector = msf2sec(time);
	long ret;

	if (!cdHandle && !chd_img)
		return -1;

	if (sector >= ti[1].start + ti[1].length &&
	    numtracks > 1 && ti[2].type == CDRT_CDDA) {
		return ISOreadCDDA(time, buf);
	}

	if (sector < 2 * 75) {
		SysPrintf("Invalid CD read before data pregap at %02d:%02d:%02d\n",
			time[0], time[1], time[2]);
		return -1;
	}
	sector -= 2 * 75;

	ret = cdimg_read_func(cdHandle, ti[1].start_offset, buf, sector);
	if (ret < 12*2 + 2048) {
		SysPrintf("Short CD read at %02d:%02d:%02d sector=%d ret=%d\n",
			time[0], time[1], time[2], sector, ret);
		return -1;
	}

	return 0;
}

// read subchannel data
int ISOreadSub(const unsigned char *time, void *buffer)
{
	int ret, sector = msf2sec(time);

	if (sector >= ti[1].start + ti[1].length) {
		// for tracks 2+ use the fake data, otherwise
		// it becomes too messy to handle all the gap stuff
		return -1;
	}
	sector -= 2 * 75;

	if (cdimg_read_sub_func != NULL) {
		if ((ret = cdimg_read_sub_func(cdHandle, sector, buffer)))
			return ret;
	}
	else if (subHandle != NULL) {
		if (fseeko(subHandle, sector * SUB_FRAMESIZE, SEEK_SET))
			return -1;
		if (fread(buffer, 1, SUB_FRAMESIZE, subHandle) != SUB_FRAMESIZE)
			return -1;
	}
	else {
		return -1;
	}

	if (subChanRaw)
		DecodeRawSubData(buffer);
	return 0;
}

int ISOgetStatus(struct CdrStat *stat)
{
	CDR__getStatus(stat);
	
	// BIOS - boot ID (CD type)
	stat->Type = ti[1].type;
	
	return 0;
}

// read CDDA sector into buffer
int ISOreadCDDA(const unsigned char *time, void *buffer)
{
	unsigned int track, track_start = 0;
	FILE *handle = cdHandle;
	unsigned int cddaCurPos;
	int ret, ret_clear = -1;

	cddaCurPos = msf2sec(time);

	// find current track index
	for (track = numtracks; ; track--) {
		track_start = ti[track].start;
		if (track_start <= cddaCurPos)
			break;
		if (track == 1)
			break;
	}

	if (track == numtracks && cddaCurPos >= ti[track].start + ti[track].length)
		return -1;
	if (ti[track].type != CDRT_CDDA) {
		// data tracks play silent
		ret_clear = 0;
		goto clear_return;
	}
	if (track < numtracks && cddaCurPos >= ti[track].start + ti[track].length) {
		// gap
		ret_clear = 0;
		goto clear_return;
	}

	if (multifile) {
		// find the file that contains this track
		unsigned int file;
		for (file = track; file > 1; file--) {
			if (ti[file].handle != NULL) {
				handle = ti[file].handle;
				break;
			}
		}
	}
	if (!handle && !chd_img)
		goto clear_return;

	ret = cdimg_read_func(handle, ti[track].start_offset,
		buffer, cddaCurPos - track_start);
	if (ret != CD_FRAMESIZE_RAW)
		goto clear_return;

	if (cddaBigEndian && buffer) {
		unsigned char tmp, *buf = buffer;
		int i;

		for (i = 0; i < CD_FRAMESIZE_RAW / 2; i++) {
			tmp = buf[i * 2];
			buf[i * 2] = buf[i * 2 + 1];
			buf[i * 2 + 1] = tmp;
		}
	}

	return 0;

clear_return:
	if (buffer)
		memset(buffer, 0, CD_FRAMESIZE_RAW);
	return ret_clear;
}
