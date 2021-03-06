#include "first.h"

#include "base.h"
#include "log.h"
#include "buffer.h"
#include "fdevent.h"
#include "http_header.h"
#include "response.h"
#include "stat_cache.h"

#include "plugin.h"

#include "crc32.h"
#include "etag.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#if defined HAVE_ZLIB_H && defined HAVE_LIBZ
# define USE_ZLIB
# include <zlib.h>
#endif

#if defined HAVE_BZLIB_H && defined HAVE_LIBBZ2
# define USE_BZ2LIB
/* we don't need stdio interface */
# define BZ_NO_STDIO
# include <bzlib.h>
#endif

#if defined HAVE_SYS_MMAN_H && defined HAVE_MMAP && defined ENABLE_MMAP
#define USE_MMAP

#include "sys-mmap.h"
#include <setjmp.h>
#include <signal.h>

static volatile int sigbus_jmp_valid;
static sigjmp_buf sigbus_jmp;

static void sigbus_handler(int sig) {
	UNUSED(sig);
	if (sigbus_jmp_valid) siglongjmp(sigbus_jmp, 1);
	log_failed_assert(__FILE__, __LINE__, "SIGBUS");
}
#endif

/* request: accept-encoding */
#define HTTP_ACCEPT_ENCODING_IDENTITY BV(0)
#define HTTP_ACCEPT_ENCODING_GZIP     BV(1)
#define HTTP_ACCEPT_ENCODING_DEFLATE  BV(2)
#define HTTP_ACCEPT_ENCODING_COMPRESS BV(3)
#define HTTP_ACCEPT_ENCODING_BZIP2    BV(4)
#define HTTP_ACCEPT_ENCODING_X_GZIP   BV(5)
#define HTTP_ACCEPT_ENCODING_X_BZIP2  BV(6)

#ifdef __WIN32
# define mkdir(x,y) mkdir(x)
#endif

typedef struct {
    const array *compress;
    const buffer *compress_cache_dir;
    off_t compress_max_filesize; /** max filesize in kb */
    double max_loadavg;
    unsigned int allowed_encodings;
} plugin_config;

typedef struct {
    PLUGIN_DATA;
    plugin_config defaults;
    plugin_config conf;

    buffer *ofn;
    buffer *b;
} plugin_data;

INIT_FUNC(mod_compress_init) {
    plugin_data * const p = calloc(1, sizeof(plugin_data));
    p->ofn = buffer_init();
    p->b = buffer_init();
    return p;
}

FREE_FUNC(mod_compress_free) {
    plugin_data *p = p_d;
    buffer_free(p->ofn);
    buffer_free(p->b);
}

/* 0 on success, -1 for error */
static int mkdir_recursive(char *dir) {
	char *p = dir;

	if (!dir || !dir[0])
		return 0;

	while ((p = strchr(p + 1, '/')) != NULL) {

		*p = '\0';
		if ((mkdir(dir, 0700) != 0) && (errno != EEXIST)) {
			*p = '/';
			return -1;
		}

		*p++ = '/';
		if (!*p) return 0; /* Ignore trailing slash */
	}

	return (mkdir(dir, 0700) != 0) && (errno != EEXIST) ? -1 : 0;
}

/* 0 on success, -1 for error */
static int mkdir_for_file(char *filename) {
	char *p = filename;

	if (!filename || !filename[0])
		return -1;

	while ((p = strchr(p + 1, '/')) != NULL) {

		*p = '\0';
		if ((mkdir(filename, 0700) != 0) && (errno != EEXIST)) {
			*p = '/';
			return -1;
		}

		*p++ = '/';
		if (!*p) return -1; /* Unexpected trailing slash in filename */
	}

	return 0;
}

static void mod_compress_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* compress.filetype */
        pconf->compress = cpv->v.a;
        break;
      case 1: /* compress.allowed-encodings */
        pconf->allowed_encodings = cpv->v.u;
        break;
      case 2: /* compress.cache-dir */
        pconf->compress_cache_dir = cpv->v.b;
        break;
      case 3: /* compress.max-filesize */
        pconf->compress_max_filesize = cpv->v.o;
        break;
      case 4: /* compress.max-loadavg */
        pconf->max_loadavg = cpv->v.d;
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_compress_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_compress_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_compress_patch_config(request_st * const r, plugin_data * const p) {
    memcpy(&p->conf, &p->defaults, sizeof(plugin_config));
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(r, (uint32_t)p->cvlist[i].k_id))
            mod_compress_merge_config(&p->conf, p->cvlist + p->cvlist[i].v.u2[0]);
    }
}

static short mod_compress_encodings_to_flags(const array *encodings) {
    short allowed_encodings = 0;
    if (encodings->used) {
        for (uint32_t j = 0; j < encodings->used; ++j) {
          #if defined(USE_ZLIB) || defined(USE_BZ2LIB)
            data_string *ds = (data_string *)encodings->data[j];
          #endif
          #ifdef USE_ZLIB
            if (NULL != strstr(ds->value.ptr, "gzip"))
                allowed_encodings |= HTTP_ACCEPT_ENCODING_GZIP
                                  |  HTTP_ACCEPT_ENCODING_X_GZIP;
            if (NULL != strstr(ds->value.ptr, "x-gzip"))
                allowed_encodings |= HTTP_ACCEPT_ENCODING_X_GZIP;
            if (NULL != strstr(ds->value.ptr, "deflate"))
                allowed_encodings |= HTTP_ACCEPT_ENCODING_DEFLATE;
            /*
            if (NULL != strstr(ds->value.ptr, "compress"))
                allowed_encodings |= HTTP_ACCEPT_ENCODING_COMPRESS;
            */
          #endif
          #ifdef USE_BZ2LIB
            if (NULL != strstr(ds->value.ptr, "bzip2"))
                allowed_encodings |= HTTP_ACCEPT_ENCODING_BZIP2
                                  |  HTTP_ACCEPT_ENCODING_X_BZIP2;
            if (NULL != strstr(ds->value.ptr, "x-bzip2"))
                allowed_encodings |= HTTP_ACCEPT_ENCODING_X_BZIP2;
          #endif
        }
    }
    else {
        /* default encodings */
      #ifdef USE_ZLIB
        allowed_encodings |= HTTP_ACCEPT_ENCODING_GZIP
                          |  HTTP_ACCEPT_ENCODING_X_GZIP
                          |  HTTP_ACCEPT_ENCODING_DEFLATE;
      #endif
      #ifdef USE_BZ2LIB
        allowed_encodings |= HTTP_ACCEPT_ENCODING_BZIP2
                          |  HTTP_ACCEPT_ENCODING_X_BZIP2;
      #endif
    }
    return allowed_encodings;
}

SETDEFAULTS_FUNC(mod_compress_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("compress.filetype"),
        T_CONFIG_ARRAY_VLIST,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("compress.allowed-encodings"),
        T_CONFIG_ARRAY_VLIST,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("compress.cache-dir"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("compress.max-filesize"),
        T_CONFIG_SHORT,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("compress.max-loadavg"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_compress"))
        return HANDLER_ERROR;

    /* process and validate config directives
     * (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1]; i < p->nconfig; ++i) {
        config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
              case 0: /* compress.filetype */
                if (0 == cpv->v.a->used) cpv->v.a = NULL;
                break;
              case 1: /* compress.allowed-encodings */
                cpv->v.u = (unsigned int)
                  mod_compress_encodings_to_flags(cpv->v.a);
                cpv->vtype = T_CONFIG_INT;
                break;
              case 2: /* compress.cache-dir */
                if (!buffer_string_is_empty(cpv->v.b)) {
                    struct stat st;
                    mkdir_recursive(cpv->v.b->ptr);
                    if (0 != stat(cpv->v.b->ptr, &st)) {
                        log_perror(srv->errh, __FILE__, __LINE__,
                          "can't stat %s %s", cpk[cpv->k_id].k, cpv->v.b->ptr);
                        return HANDLER_ERROR;
                    }
                }
                break;
              case 3: /* compress.max-filesize */
                cpv->v.o = ((off_t)cpv->v.shrt) << 10; /* KB to bytes */
                break;
              case 4: /* compress.max-loadavg */
                cpv->v.d = (!buffer_string_is_empty(cpv->v.b))
                  ? strtod(cpv->v.b->ptr, NULL)
                  : 0.0;
                break;
              default:/* should not happen */
                break;
            }
        }
    }

    p->defaults.max_loadavg = 0.0;

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_compress_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}


#ifdef USE_ZLIB
static int deflate_file_to_buffer_gzip(plugin_data *p, char *start, off_t st_size, time_t mtime) {
	unsigned char *c;
	unsigned long crc;
	z_stream z;
	size_t outlen;

	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;

	if (Z_OK != deflateInit2(&z,
				 Z_DEFAULT_COMPRESSION,
				 Z_DEFLATED,
				 -MAX_WBITS,  /* suppress zlib-header */
				 8,
				 Z_DEFAULT_STRATEGY)) {
		return -1;
	}

	z.next_in = (unsigned char *)start;
	z.avail_in = st_size;
	z.total_in = 0;


	buffer_string_prepare_copy(p->b, (z.avail_in * 1.1) + 12 + 18);

	/* write gzip header */

	c = (unsigned char *)p->b->ptr;
	c[0] = 0x1f;
	c[1] = 0x8b;
	c[2] = Z_DEFLATED;
	c[3] = 0; /* options */
	c[4] = (mtime >>  0) & 0xff;
	c[5] = (mtime >>  8) & 0xff;
	c[6] = (mtime >> 16) & 0xff;
	c[7] = (mtime >> 24) & 0xff;
	c[8] = 0x00; /* extra flags */
	c[9] = 0x03; /* UNIX */

	outlen = 10;
	z.next_out = (unsigned char *)p->b->ptr + outlen;
	z.avail_out = p->b->size - outlen - 9;
	z.total_out = 0;

	if (Z_STREAM_END != deflate(&z, Z_FINISH)) {
		deflateEnd(&z);
		return -1;
	}

	/* trailer */
	outlen += z.total_out;

	crc = generate_crc32c(start, st_size);

	c = (unsigned char *)p->b->ptr + outlen;

	c[0] = (crc >>  0) & 0xff;
	c[1] = (crc >>  8) & 0xff;
	c[2] = (crc >> 16) & 0xff;
	c[3] = (crc >> 24) & 0xff;
	c[4] = (z.total_in >>  0) & 0xff;
	c[5] = (z.total_in >>  8) & 0xff;
	c[6] = (z.total_in >> 16) & 0xff;
	c[7] = (z.total_in >> 24) & 0xff;
	outlen += 8;
	buffer_commit(p->b, outlen);

	if (Z_OK != deflateEnd(&z)) {
		return -1;
	}

	return 0;
}

static int deflate_file_to_buffer_deflate(plugin_data *p, unsigned char *start, off_t st_size) {
	z_stream z;

	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;

	if (Z_OK != deflateInit2(&z,
				 Z_DEFAULT_COMPRESSION,
				 Z_DEFLATED,
				 -MAX_WBITS,  /* suppress zlib-header */
				 8,
				 Z_DEFAULT_STRATEGY)) {
		return -1;
	}

	z.next_in = start;
	z.avail_in = st_size;
	z.total_in = 0;

	buffer_string_prepare_copy(p->b, (z.avail_in * 1.1) + 12);

	z.next_out = (unsigned char *)p->b->ptr;
	z.avail_out = p->b->size - 1;
	z.total_out = 0;

	if (Z_STREAM_END != deflate(&z, Z_FINISH)) {
		deflateEnd(&z);
		return -1;
	}

	if (Z_OK != deflateEnd(&z)) {
		return -1;
	}

	/* trailer */
	buffer_commit(p->b, z.total_out);

	return 0;
}

#endif

#ifdef USE_BZ2LIB
static int deflate_file_to_buffer_bzip2(plugin_data *p, unsigned char *start, off_t st_size) {
	bz_stream bz;

	bz.bzalloc = NULL;
	bz.bzfree = NULL;
	bz.opaque = NULL;

	if (BZ_OK != BZ2_bzCompressInit(&bz,
					9, /* blocksize = 900k */
					0, /* no output */
					0)) { /* workFactor: default */
		return -1;
	}

	bz.next_in = (char *)start;
	bz.avail_in = st_size;
	bz.total_in_lo32 = 0;
	bz.total_in_hi32 = 0;

	buffer_string_prepare_copy(p->b, (bz.avail_in * 1.1) + 12);

	bz.next_out = p->b->ptr;
	bz.avail_out = p->b->size - 1;
	bz.total_out_lo32 = 0;
	bz.total_out_hi32 = 0;

	if (BZ_STREAM_END != BZ2_bzCompress(&bz, BZ_FINISH)) {
		BZ2_bzCompressEnd(&bz);
		return -1;
	}

	if (BZ_OK != BZ2_bzCompressEnd(&bz)) {
		return -1;
	}

	/* file is too large for now */
	if (bz.total_out_hi32) return -1;

	/* trailer */
	buffer_commit(p->b, bz.total_out_lo32);

	return 0;
}
#endif

static void mod_compress_note_ratio(request_st * const r, off_t in, off_t out) {
    /* store compression ratio in environment
     * for possible logging by mod_accesslog
     * (late in response handling, so not seen by most other modules) */
    /*(should be called only at end of successful response compression)*/
    char ratio[LI_ITOSTRING_LENGTH];
    if (0 == in) return;
    http_header_env_set(r, CONST_STR_LEN("ratio"),
                        ratio, li_itostrn(ratio,sizeof(ratio),out*100/in));
}

static int deflate_file_to_file(request_st * const r, plugin_data *p, int ifd, buffer *fn, stat_cache_entry *sce, int type) {
	int ofd;
	int ret;
#ifdef USE_MMAP
	volatile int mapped = 0;/* quiet warning: might be clobbered by 'longjmp' */
#endif
	void *start;
	stat_cache_entry *sce_ofn;

	/* overflow */
	if ((off_t)(sce->st.st_size * 1.1) < sce->st.st_size) return -1;

	/* don't mmap files > 128Mb
	 *
	 * we could use a sliding window, but currently there is no need for it
	 */

	if (sce->st.st_size > 128 * 1024 * 1024) return -1;

	buffer_reset(p->ofn);
	buffer_copy_buffer(p->ofn, p->conf.compress_cache_dir);
	buffer_append_slash(p->ofn);

	if (0 == strncmp(r->physical.path.ptr, r->physical.doc_root.ptr, buffer_string_length(&r->physical.doc_root))) {
		buffer_append_string(p->ofn, r->physical.path.ptr + buffer_string_length(&r->physical.doc_root));
	} else {
		buffer_append_string_buffer(p->ofn, &r->uri.path);
	}

	switch(type) {
	case HTTP_ACCEPT_ENCODING_GZIP:
	case HTTP_ACCEPT_ENCODING_X_GZIP:
		buffer_append_string_len(p->ofn, CONST_STR_LEN("-gzip-"));
		break;
	case HTTP_ACCEPT_ENCODING_DEFLATE:
		buffer_append_string_len(p->ofn, CONST_STR_LEN("-deflate-"));
		break;
	case HTTP_ACCEPT_ENCODING_BZIP2:
	case HTTP_ACCEPT_ENCODING_X_BZIP2:
		buffer_append_string_len(p->ofn, CONST_STR_LEN("-bzip2-"));
		break;
	default:
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "unknown compression type %d", type);
		return -1;
	}

	const buffer *etag = stat_cache_etag_get(sce, r->conf.etag_flags);
	buffer_append_string_buffer(p->ofn, etag);

	sce_ofn = stat_cache_get_entry(p->ofn);
	if (sce_ofn) {
		if (0 == sce->st.st_size) return -1; /* cache file being created */
		/* cache-entry exists */
#if 0
		log_error(r->conf.errh, __FILE__, __LINE__, "%s compress-cache hit", p->ofn->ptr);
#endif
		mod_compress_note_ratio(r, sce->st.st_size, sce_ofn->st.st_size);
		buffer_copy_buffer(&r->physical.path, p->ofn);
		return 0;
	}

	if (0.0 < p->conf.max_loadavg && p->conf.max_loadavg < r->con->srv->loadavg[0]) {
		return -1;
	}

	if (-1 == mkdir_for_file(p->ofn->ptr)) {
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "couldn't create directory for file %s", p->ofn->ptr);
		return -1;
	}

	/*(note: follows symlinks in protected cache dir)*/
	if (-1 == (ofd = fdevent_open_cloexec(p->ofn->ptr, 1, O_WRONLY | O_CREAT | O_EXCL, 0600))) {
		if (errno == EEXIST) {
			return -1; /* cache file being created */
		}

		log_perror(r->conf.errh, __FILE__, __LINE__,
		  "creating cachefile %s failed", p->ofn->ptr);

		return -1;
	}
#if 0
	log_error(r->conf.errh, __FILE__, __LINE__, "%s compress-cache miss", p->ofn->ptr);
#endif

#ifdef USE_MMAP
	if (MAP_FAILED != (start = mmap(NULL, sce->st.st_size, PROT_READ, MAP_SHARED, ifd, 0))
	    || (errno == EINVAL && MAP_FAILED != (start = mmap(NULL, sce->st.st_size, PROT_READ, MAP_PRIVATE, ifd, 0)))) {
		mapped = 1;
		signal(SIGBUS, sigbus_handler);
		sigbus_jmp_valid = 1;
		if (0 != sigsetjmp(sigbus_jmp, 1)) {
			sigbus_jmp_valid = 0;

			log_error(r->conf.errh, __FILE__, __LINE__,
			  "SIGBUS in mmap: %s %d", fn->ptr, ifd);

			munmap(start, sce->st.st_size);
			close(ofd);

			/* Remove the incomplete cache file, so that later hits aren't served from it */
			if (-1 == unlink(p->ofn->ptr)) {
				log_perror(r->conf.errh, __FILE__, __LINE__,
				  "unlinking incomplete cachefile %s failed", p->ofn->ptr);
			}

			return -1;
		}
	} else
#endif  /* FIXME: might attempt to read very large file completely into memory; see compress.max-filesize config option */
	if (NULL == (start = malloc(sce->st.st_size)) || sce->st.st_size != read(ifd, start, sce->st.st_size)) {
		log_perror(r->conf.errh, __FILE__, __LINE__,
		  "reading %s failed", fn->ptr);

		close(ofd);
		free(start);

		/* Remove the incomplete cache file, so that later hits aren't served from it */
		if (-1 == unlink(p->ofn->ptr)) {
			log_perror(r->conf.errh, __FILE__, __LINE__,
			  "unlinking incomplete cachefile %s failed", p->ofn->ptr);
		}

		return -1;
	}

	ret = -1;
	switch(type) {
#ifdef USE_ZLIB
	case HTTP_ACCEPT_ENCODING_GZIP:
	case HTTP_ACCEPT_ENCODING_X_GZIP:
		ret = deflate_file_to_buffer_gzip(p, start, sce->st.st_size, sce->st.st_mtime);
		break;
	case HTTP_ACCEPT_ENCODING_DEFLATE:
		ret = deflate_file_to_buffer_deflate(p, start, sce->st.st_size);
		break;
#endif
#ifdef USE_BZ2LIB
	case HTTP_ACCEPT_ENCODING_BZIP2:
	case HTTP_ACCEPT_ENCODING_X_BZIP2:
		ret = deflate_file_to_buffer_bzip2(p, start, sce->st.st_size);
		break;
#endif
	}

	if (ret == 0) {
		ssize_t wr = write(ofd, CONST_BUF_LEN(p->b));
		if (-1 == wr) {
			log_perror(r->conf.errh, __FILE__, __LINE__,
			  "writing cachefile %s failed", p->ofn->ptr);
			ret = -1;
		} else if ((size_t)wr != buffer_string_length(p->b)) {
			log_error(r->conf.errh, __FILE__, __LINE__,
			  "writing cachefile %s failed: not enough bytes written",
			  p->ofn->ptr);
			ret = -1;
		}
	}

#ifdef USE_MMAP
	if (mapped) {
		sigbus_jmp_valid = 0;
		munmap(start, sce->st.st_size);
	} else
#endif
		free(start);

	if (0 != close(ofd) || ret != 0) {
		if (0 == ret) {
			log_perror(r->conf.errh, __FILE__, __LINE__,
			  "writing cachefile %s failed", p->ofn->ptr);
		}

		/* Remove the incomplete cache file, so that later hits aren't served from it */
		if (-1 == unlink(p->ofn->ptr)) {
			log_perror(r->conf.errh, __FILE__, __LINE__,
			  "unlinking incomplete cachefile %s failed", p->ofn->ptr);
		}

		return -1;
	}

	buffer_copy_buffer(&r->physical.path, p->ofn);
	mod_compress_note_ratio(r, sce->st.st_size,
				(off_t)buffer_string_length(p->b));

	return 0;
}

static int deflate_file_to_buffer(request_st * const r, plugin_data *p, int ifd, buffer *fn, stat_cache_entry *sce, int type) {
	int ret = -1;
#ifdef USE_MMAP
	volatile int mapped = 0;/* quiet warning: might be clobbered by 'longjmp' */
#endif
	void *start;

	/* overflow */
	if ((off_t)(sce->st.st_size * 1.1) < sce->st.st_size) return -1;

	/* don't mmap files > 128M
	 *
	 * we could use a sliding window, but currently there is no need for it
	 */

	if (sce->st.st_size > 128 * 1024 * 1024) return -1;

	if (0.0 < p->conf.max_loadavg && p->conf.max_loadavg < r->con->srv->loadavg[0]) {
		return -1;
	}

	if (-1 == ifd) {
		/* not called; call exists to de-optimize and avoid "clobbered by 'longjmp'" compiler warning */
		log_error(r->conf.errh, __FILE__, __LINE__, " ");
		return -1;
	}

#ifdef USE_MMAP
	if (MAP_FAILED != (start = mmap(NULL, sce->st.st_size, PROT_READ, MAP_SHARED, ifd, 0))
	    || (errno == EINVAL && MAP_FAILED != (start = mmap(NULL, sce->st.st_size, PROT_READ, MAP_PRIVATE, ifd, 0)))) {
		mapped = 1;
		signal(SIGBUS, sigbus_handler);
		sigbus_jmp_valid = 1;
		if (0 != sigsetjmp(sigbus_jmp, 1)) {
			sigbus_jmp_valid = 0;
			log_error(r->conf.errh, __FILE__, __LINE__,
			  "SIGBUS in mmap: %s %d", fn->ptr, ifd);
			munmap(start, sce->st.st_size);
			return -1;
		}
	} else
#endif  /* FIXME: might attempt to read very large file completely into memory; see compress.max-filesize config option */
	if (NULL == (start = malloc(sce->st.st_size)) || sce->st.st_size != read(ifd, start, sce->st.st_size)) {
		log_perror(r->conf.errh, __FILE__, __LINE__, "reading %s failed", fn->ptr);
		free(start);
		return -1;
	}

	switch(type) {
#ifdef USE_ZLIB
	case HTTP_ACCEPT_ENCODING_GZIP:
	case HTTP_ACCEPT_ENCODING_X_GZIP:
		ret = deflate_file_to_buffer_gzip(p, start, sce->st.st_size, sce->st.st_mtime);
		break;
	case HTTP_ACCEPT_ENCODING_DEFLATE:
		ret = deflate_file_to_buffer_deflate(p, start, sce->st.st_size);
		break;
#endif
#ifdef USE_BZ2LIB
	case HTTP_ACCEPT_ENCODING_BZIP2:
	case HTTP_ACCEPT_ENCODING_X_BZIP2:
		ret = deflate_file_to_buffer_bzip2(p, start, sce->st.st_size);
		break;
#endif
	default:
		ret = -1;
		break;
	}

#ifdef USE_MMAP
	if (mapped) {
		sigbus_jmp_valid = 0;
		munmap(start, sce->st.st_size);
	} else
#endif
		free(start);

	if (ret != 0) return -1;

	mod_compress_note_ratio(r, sce->st.st_size,
				(off_t)buffer_string_length(p->b));
	chunkqueue_reset(r->write_queue);
	chunkqueue_append_buffer(r->write_queue, p->b);

	buffer_reset(&r->physical.path);

	r->resp_body_finished = 1;
	r->resp_body_started  = 1;

	return 0;
}


static int mod_compress_contains_encoding(const char *headervalue, const char *encoding, size_t len) {
	const char *m = headervalue;
	do {
		while (*m == ',' || *m == ' ' || *m == '\t') {
			++m;
		}
		if (buffer_eq_icase_ssn(m, encoding, len)) {
			/*(not a full HTTP field parse: not parsing for q-values and not handling q=0)*/
			m += len;
			if (*m == '\0' || *m == ',' || *m == ';' || *m == ' ' || *m == '\t')
				return 1;
		} else if (*m != '\0') {
			++m;
		}
	} while ((m = strchr(m, ',')));
	return 0;
}

PHYSICALPATH_FUNC(mod_compress_physical) {
	plugin_data *p = p_d;
	uint32_t m;
	stat_cache_entry *sce = NULL;
	const buffer *mtime = NULL;
	buffer *content_type_trunc;
	const buffer *content_type;

	if (NULL != r->handler_module || r->http_status) return HANDLER_GO_ON;

	/* only GET and POST can get compressed */
	if (r->http_method != HTTP_METHOD_GET &&
	    r->http_method != HTTP_METHOD_POST) {
		return HANDLER_GO_ON;
	}

	if (buffer_string_is_empty(&r->physical.path)) {
		return HANDLER_GO_ON;
	}

	mod_compress_patch_config(r, p);

	if (NULL == p->conf.compress) {
		return HANDLER_GO_ON;
	}

	if (r->conf.log_request_handling) {
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "-- handling file as static file");
	}

	sce = stat_cache_get_entry(&r->physical.path);
	if (NULL == sce) {
		r->http_status = 403;
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "not a regular file: %s -> %s",
		  r->uri.path.ptr, r->physical.path.ptr);
		return HANDLER_FINISHED;
	}

	/* we only handle regular files */
	if (!S_ISREG(sce->st.st_mode)) {
		return HANDLER_GO_ON;
	}

	/* don't compress files that are too large as we need to much time to handle them */
	off_t max_fsize = p->conf.compress_max_filesize;
	if (max_fsize && sce->st.st_size > max_fsize) return HANDLER_GO_ON;

	/* don't try to compress files less than 128 bytes
	 *
	 * - extra overhead for compression
	 * - mmap() fails for st_size = 0 :)
	 */
	if (sce->st.st_size < 128) return HANDLER_GO_ON;

	const buffer *etag = stat_cache_etag_get(sce, r->conf.etag_flags);
	if (buffer_string_is_empty(etag)) etag = NULL;

	/* check if mimetype is in compress-config */
	content_type_trunc = NULL;
	content_type = stat_cache_content_type_get(sce, r);
	if (!buffer_is_empty(content_type)) {
		char *c;
		if ( (c = strchr(content_type->ptr, ';')) != NULL) {
			content_type_trunc = r->tmp_buf;
			buffer_copy_string_len(content_type_trunc, content_type->ptr, c - content_type->ptr);
		}
	}
	else {
		content_type = content_type_trunc = r->tmp_buf;
		buffer_copy_string_len(content_type_trunc, CONST_STR_LEN(""));
	}

	for (m = 0; m < p->conf.compress->used; m++) {
		data_string *compress_ds = (data_string *)p->conf.compress->data[m];

		if (buffer_is_equal(&compress_ds->value, content_type)
		    || (content_type_trunc && buffer_is_equal(&compress_ds->value, content_type_trunc))) {
			break;
		}
	}
	if (m == p->conf.compress->used) return HANDLER_GO_ON; /* not found */


	{
			/* mimetype found */
			const buffer *vb;

			/* the response might change according to Accept-Encoding */
			http_header_response_append(r, HTTP_HEADER_VARY, CONST_STR_LEN("Vary"), CONST_STR_LEN("Accept-Encoding"));

			if (NULL == (vb = http_header_request_get(r, HTTP_HEADER_ACCEPT_ENCODING, CONST_STR_LEN("Accept-Encoding")))) {
				return HANDLER_GO_ON;
			}


			{
				int accept_encoding = 0;
				char *value = vb->ptr;
				int matched_encodings = 0;

				/* get client side support encodings */
#ifdef USE_ZLIB
				if (mod_compress_contains_encoding(value, CONST_STR_LEN("gzip"))) accept_encoding |= HTTP_ACCEPT_ENCODING_GZIP;
				if (mod_compress_contains_encoding(value, CONST_STR_LEN("x-gzip"))) accept_encoding |= HTTP_ACCEPT_ENCODING_X_GZIP;
				if (mod_compress_contains_encoding(value, CONST_STR_LEN("deflate"))) accept_encoding |= HTTP_ACCEPT_ENCODING_DEFLATE;
				if (mod_compress_contains_encoding(value, CONST_STR_LEN("compress"))) accept_encoding |= HTTP_ACCEPT_ENCODING_COMPRESS;
#endif
#ifdef USE_BZ2LIB
				if (mod_compress_contains_encoding(value, CONST_STR_LEN("bzip2"))) accept_encoding |= HTTP_ACCEPT_ENCODING_BZIP2;
				if (mod_compress_contains_encoding(value, CONST_STR_LEN("x-bzip2"))) accept_encoding |= HTTP_ACCEPT_ENCODING_X_BZIP2;
#endif
				if (mod_compress_contains_encoding(value, CONST_STR_LEN("identity"))) accept_encoding |= HTTP_ACCEPT_ENCODING_IDENTITY;

				/* find matching entries */
				matched_encodings = accept_encoding & p->conf.allowed_encodings;

				if (matched_encodings) {
					static const char dflt_gzip[] = "gzip";
					static const char dflt_x_gzip[] = "x-gzip";
					static const char dflt_deflate[] = "deflate";
					static const char dflt_bzip2[] = "bzip2";
					static const char dflt_x_bzip2[] = "x-bzip2";

					const char *compression_name = NULL;
					int compression_type = 0;

					if (!r->conf.follow_symlink
					    && 0 != stat_cache_path_contains_symlink(&r->physical.path, r->conf.errh)) {
						return HANDLER_GO_ON;
					}

					const int fd = fdevent_open_cloexec(r->physical.path.ptr, r->conf.follow_symlink, O_RDONLY, 0);
					if (fd < 0) {
						log_perror(r->conf.errh, __FILE__, __LINE__,
						  "opening plain-file %s failed", r->physical.path.ptr);
						return HANDLER_GO_ON;
					}

					mtime = strftime_cache_get(sce->st.st_mtime);

					/* try matching original etag of uncompressed version */
					if (etag) {
						etag_mutate(&r->physical.etag, etag);
						if (HANDLER_FINISHED == http_response_handle_cachable(r, mtime)) {
							http_header_response_set(r, HTTP_HEADER_CONTENT_TYPE, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(content_type));
							http_header_response_set(r, HTTP_HEADER_LAST_MODIFIED, CONST_STR_LEN("Last-Modified"), CONST_BUF_LEN(mtime));
							http_header_response_set(r, HTTP_HEADER_ETAG, CONST_STR_LEN("ETag"), CONST_BUF_LEN(&r->physical.etag));
							close(fd);
							return HANDLER_FINISHED;
						}
					}

					/* select best matching encoding */
					if (matched_encodings & HTTP_ACCEPT_ENCODING_BZIP2) {
						compression_type = HTTP_ACCEPT_ENCODING_BZIP2;
						compression_name = dflt_bzip2;
					} else if (matched_encodings & HTTP_ACCEPT_ENCODING_X_BZIP2) {
						compression_type = HTTP_ACCEPT_ENCODING_X_BZIP2;
						compression_name = dflt_x_bzip2;
					} else if (matched_encodings & HTTP_ACCEPT_ENCODING_GZIP) {
						compression_type = HTTP_ACCEPT_ENCODING_GZIP;
						compression_name = dflt_gzip;
					} else if (matched_encodings & HTTP_ACCEPT_ENCODING_X_GZIP) {
						compression_type = HTTP_ACCEPT_ENCODING_X_GZIP;
						compression_name = dflt_x_gzip;
					} else {
						force_assert(matched_encodings & HTTP_ACCEPT_ENCODING_DEFLATE);
						compression_type = HTTP_ACCEPT_ENCODING_DEFLATE;
						compression_name = dflt_deflate;
					}

					if (etag) {
						/* try matching etag of compressed version */
						buffer * const tb = r->tmp_buf;
						buffer_copy_buffer(tb, etag);
						buffer_append_string_len(tb, CONST_STR_LEN("-"));
						buffer_append_string(tb, compression_name);
						etag_mutate(&r->physical.etag, tb);
					}

					if (HANDLER_FINISHED == http_response_handle_cachable(r, mtime)) {
						http_header_response_set(r, HTTP_HEADER_CONTENT_ENCODING, CONST_STR_LEN("Content-Encoding"), compression_name, strlen(compression_name));
						http_header_response_set(r, HTTP_HEADER_CONTENT_TYPE, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(content_type));
						http_header_response_set(r, HTTP_HEADER_LAST_MODIFIED, CONST_STR_LEN("Last-Modified"), CONST_BUF_LEN(mtime));
						if (etag) {
							http_header_response_set(r, HTTP_HEADER_ETAG, CONST_STR_LEN("ETag"), CONST_BUF_LEN(&r->physical.etag));
						}
						close(fd);
						return HANDLER_FINISHED;
					}

					/* deflate it */
					if (etag && !buffer_string_is_empty(p->conf.compress_cache_dir)) {
						if (0 != deflate_file_to_file(r, p, fd, &r->physical.path, sce, compression_type)) {
							close(fd);
							return HANDLER_GO_ON;
						}
					} else {
						if (0 != deflate_file_to_buffer(r, p, fd, &r->physical.path, sce, compression_type)) {
							close(fd);
							return HANDLER_GO_ON;
						}
					}
					close(fd);
					http_header_response_set(r, HTTP_HEADER_CONTENT_ENCODING, CONST_STR_LEN("Content-Encoding"), compression_name, strlen(compression_name));
					http_header_response_set(r, HTTP_HEADER_LAST_MODIFIED, CONST_STR_LEN("Last-Modified"), CONST_BUF_LEN(mtime));
					if (etag) {
						http_header_response_set(r, HTTP_HEADER_ETAG, CONST_STR_LEN("ETag"), CONST_BUF_LEN(&r->physical.etag));
					}
					http_header_response_set(r, HTTP_HEADER_CONTENT_TYPE, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(content_type));
					/* let mod_staticfile handle the cached compressed files, physical path was modified */
					return (etag && !buffer_string_is_empty(p->conf.compress_cache_dir)) ? HANDLER_GO_ON : HANDLER_FINISHED;
				}
			}
	}

	return HANDLER_GO_ON;
}

int mod_compress_plugin_init(plugin *p);
int mod_compress_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = "compress";

	p->init        = mod_compress_init;
	p->set_defaults = mod_compress_set_defaults;
	p->handle_subrequest_start  = mod_compress_physical;
	p->cleanup     = mod_compress_free;

	return 0;
}
