#include "url_cache.h"

#include <cstdint>
#include <cassert>
#include <ctime>
#include <cstdio>

#include <algorithm>
#include <unordered_map>

#define NOMINMAX
#include <Windows.h>
#include <curl/curl.h>

#pragma warning(disable: 4505) // unreferenced local function has been removed

#define ensure(p) ((p) ? (void)0 : abort())

namespace urlcache {

const unsigned MAX_FETCH_SLOTS       = 8;
const unsigned MAX_USER_DATA_TYPES   = 4;
const unsigned MAX_NOTIFY_SINKS      = 16;

struct Handle {
	struct Entry *entry;
	short notify;
	unsigned short flags;
	void *user_data;
	unsigned data_size;
	Handle *next;
	Handle *prev;
};

struct Entry {
	uint64_t key;
	unsigned lock_count;
	unsigned flags;
	void *data;
	unsigned data_size;
	MimeType mime_type;
	UrlFetchState fetch_state;
	UrlFetchPriority priority;
	unsigned ttl_secs;
	clock_t last_used;
	struct ParsedUrl *url;
	Entry *fetch_prev;
	Entry *fetch_next;
	Handle *handles;
};

typedef std::unordered_map<uint64_t, Entry *> EntryHash;

struct FetchSlot {
	UrlFetchState state;
	uint64_t key;
	ParsedUrl url;
	void *curl_handle;
	char *buffer;
	unsigned capacity;
	unsigned data_size;
	Cache *cache; // For CURL callback.
};

struct NotifySink {
	NotifyCallback callback;
	void *user_data;
};

struct Cache {
	CRITICAL_SECTION lock;
	CURLM *curl_multi_handle;
	EntryHash entries;
	FetchSlot fetch_slots[MAX_FETCH_SLOTS];
	unsigned num_fetch_slots;
	Entry *fetch_head[NUM_PRIORITY_LEVELS];
	Entry *fetch_tail[NUM_PRIORITY_LEVELS];
	unsigned memory_limit;
	clock_t clock;
	LocalFetchCallback fetch_local;
	void *fetch_local_data;
	NotifySink sinks[MAX_NOTIFY_SINKS];
	unsigned num_notify_sinks;
};

const char * const MIME_TYPE_STRINGS[NUM_SUPPORTED_MIME_TYPES + 1] = {
	"application/octet-stream",
	"application/json",
	"text/plain",
	"text/html",
	"text/stacker",
	"text/template",
	"image/png",
	"image/jpeg",
	"image/gif",
	"MIMETYPE_NONE"
};

const struct {
	const char * extension;
	MimeType mime_type;
} MIME_TYPE_EXTENSIONS[] = {
	{ "png",      MIMETYPE_PNG      },
	{ "jpg",      MIMETYPE_JPEG     },
	{ "jpeg",     MIMETYPE_JPEG     },
	{ "gif",      MIMETYPE_GIF      },
	{ "stacker",  MIMETYPE_STACKER  },
	{ "template", MIMETYPE_TEMPLATE },
	{ "json",     MIMETYPE_JSON     },
	{ "txt",      MIMETYPE_TEXT     },
	{ "html",     MIMETYPE_HTML     },
	{ "xhtml",    MIMETYPE_HTML     }
};
const unsigned NUM_MIME_TYPE_EXTENSIONS = sizeof(MIME_TYPE_EXTENSIONS) / 
	sizeof(MIME_TYPE_EXTENSIONS[0]);

const char * const FETCH_STATE_STRINGS[NUM_FETCH_STATES] = 
	{ "idle", "queued", "failed", "in-progress", "successful", "disk" };

const char * const PRIORITY_STRINGS[NUM_PRIORITY_LEVELS] =
	{ "URLP_NO_FETCH", "URLP_NORMAL", "URLP_ELEVATED", "URLP_URGENT" };

const char * const MATCH_DELIMITERS = " \t\r\n,";

/*
 * Helpers
 */

inline bool ispathsep(char ch)
{
	return ch == '/' || ch == '\\';
}

const char *path_file_name(const char *path)
{
	const char *p = strchr(path, '\0');
	while (p != path && !ispathsep(*p)) --p;
	return p + ispathsep(*p);
}

const char *path_extension(const char *path)
{
	const char *end = strchr(path, '\0'), *p = end;
	while (p != path && *p != '.' && !ispathsep(*p)) --p;
	return *p == '.' ? p + 1 : end;
}

/* Converts a mime type string to a MimeType constant. */
MimeType find_mime_type_by_name(const char *s, int length)
{
	if (length < 0)
		length = (int)strlen(s);
	for (unsigned i = 0; i < NUM_SUPPORTED_MIME_TYPES; ++i) {
		if ((unsigned)length == strlen(MIME_TYPE_STRINGS[i]) && 
			0 == memcmp(MIME_TYPE_STRINGS[i], s, length))
			return (MimeType)i;
	}
	return MIMETYPE_NONE;
}

/* Returns the mime type corresponding to a file extension. */
MimeType guess_mime_type(const char *s, int length)
{
	if (length < 0)
		length = (int)strlen(s);
	for (unsigned i = 0; i < NUM_MIME_TYPE_EXTENSIONS; ++i) {
		if ((unsigned)length == strlen(MIME_TYPE_EXTENSIONS[i].extension) && 
			0 == memcmp(MIME_TYPE_EXTENSIONS[i].extension, s, length))
			return MIME_TYPE_EXTENSIONS[i].mime_type;
	}
	return MIMETYPE_NONE;
}

/* Given a comma- or space-delimited list of schemes, returns the 1-based
 * index of the first scheme that matches that of "url", or zero if none
 * match. */
int match_scheme(const ParsedUrl *url, const char *s, int slen)
{
	unsigned toklen;
	const char *end = s + ((slen < 0) ? strlen(s) : slen);
	for (int index = 1; s != end; s += toklen, ++index) {
		s += strspn(s, MATCH_DELIMITERS);
		toklen = strcspn(s, MATCH_DELIMITERS);
		if (toklen == url->scheme_length && 0 == memcmp(url->url, s, toklen))
			return index;
		if (toklen == 0)
			break;
	}
	return 0;
}

/* Given a comma- or space-delimited list of path extensions, returns the 
 * 1-based index of the extension that matches the n-th extension back in
 * 'url', with zero denoting the last extension. Returns zero if none match. */
int match_nth_extension(const ParsedUrl *url, unsigned n, const char *s, int slen)
{
	if (n >= PARSED_URL_MAX_EXTENSIONS)
		return 0;
	const char *end = s + ((slen < 0) ? strlen(s) : slen);
	const char *ext = url->url + url->extension_starts[n];
	unsigned extlen = url->extension_lengths[n];
	if (extlen == 0)
		return 0;
	unsigned toklen;
	for (int index = 1; s != end; s += toklen, ++index) {
		s += strspn(s, MATCH_DELIMITERS);
		toklen = strcspn(s, MATCH_DELIMITERS);
		if (toklen == extlen && 0 == memcmp(s, ext, extlen))
			return index;
		if (toklen == 0)
			break;
	}
	return 0;
}

/* Returns a pointer to a segment of a path based on the segment's 0-based 
 * index, optionally copying the segment into a null terminated buffer. Returns
 * NULL, and sets *out_length to -1, when the requested segment does not exist.
 * A segment is a part of the path ending in a separator, or the non-empty 
 * suffix of the string after its final separator. If the first character of
 * the string is a separator, it is ignored. Thus:
 * 
 *    - The path "/" has zero segments.
 *    - The path "//" consists of a single empty segment.
 *    - The path "abc/" has a single segment "abc".
 */
const char *path_segment(unsigned n, const char *s, int slen, 
	int *out_length, char *buffer, unsigned buffer_size)
{
	if (slen < 0)
		slen = (int)strlen(s);
	int seg = -1, i = int(slen && ispathsep(s[0])), j;
	for (j = i; seg < (int)n; ++j) {
		if (j == slen) {
			seg += (j > i);
			break;
		}
		if (ispathsep(s[j])) {
			if (++seg == (int)n)
				break;
			i = j + 1;
		}
	}
	if (seg == (int)n) {
		int length = j - i;
		if (out_length != NULL)
			*out_length = length;
		if (buffer != NULL) {
			if (length >= (int)buffer_size)
				length = (int)buffer_size - 1;
			memcpy(buffer, s + i, length);
			buffer[length] = '\0';
		}
		return s + i;
	} 
	if (out_length != NULL)
		*out_length = -1;
	if (buffer != NULL && buffer_size != 0)
		*buffer = '\0';
	return NULL;
}

/* True if 'ch' is a reserved character for the purposes of URL encoding. */
inline bool is_reserved(char ch)
{
	return !isalnum(ch) && ch != '-' && ch != '_' && ch != '.' && ch != '~';
}

/* Returns the value of a hexadecimal digit character, or a number >= 16 if
 * the character is not a hexadecimal digit. */
inline unsigned hex_digit_value(char ch)
{
	unsigned value = unsigned(ch - '0');
	if (value > 9)
		value = unsigned((ch & ~0x20) - 'A');
	return value;
}

/* Performs URL decoding in place. */
unsigned url_decode(char *s, int length, unsigned flags)
{
	if (length < 0)
		length = (int)strlen(s) + 1;
	unsigned j = 0;
	char plus_to = (flags & URLPARSE_DECODE_PLUS_TO_SPACE) != 0 ? ' ' : '+';
	for (unsigned i = 0; i < (unsigned)length; ++i) {
		char ch = s[i];
		if (ch == '%' && i + 3 <= (unsigned)length) {
			unsigned high = hex_digit_value(s[i + 1]);
			unsigned low = hex_digit_value(s[i + 2]);
			if ((low |  high) < 16) {
				ch = char(low + (high << 4));
				i += 2;
			}
		} else if (ch == '+') {
			ch = plus_to;
		}
		s[j++] = ch;
	}
	return j;
}

/* Decodes URL escapes. The input need not be null terminated, but the output
 * is guaranteed to be. The required output buffer size can be determined by
 * passing a NULL buffer. If URLPARSE_HEAP is not set, the output will be
 * truncated at an escape sequence boundary. */
char *url_encode(const char *s, int length, char *buffer, unsigned buffer_size, 
	unsigned *out_required, unsigned flags)
{
	static const char HEX_DIGITS[16 + 1] = "0123456789abcdef";

	unsigned required = 0, limit = 0;
	for (int i = 0; i < length; ++i) {
		required += 1 + 2 * (unsigned)is_reserved(s[i]);
		limit += (required + 1 <= buffer_size); /* OK to read up to s[i]. */
	}
	if (required + 1 > buffer_size && (flags & URLPARSE_HEAP) != 0) {
		buffer = new char[required + 1];
		limit = length;
	}
	unsigned j = 0;
	for (unsigned i = 0; i < limit; ++i) {
		char ch = s[i];
		if (is_reserved(ch)) {
			if (ch == ' ' && (flags & URLPARSE_ENCODE_SPACE_AS_PLUS) != 0) {
				buffer[j++] = '+';
			} else {
				buffer[j++] = '%';
				buffer[j++] = HEX_DIGITS[(ch >> 4) & 0xF];
				buffer[j++] = HEX_DIGITS[ch & 0xF];
			}
		} else {
			buffer[j++] = ch;
		}
	}
	if (buffer_size != 0)
		buffer[j] = '\0';
	if (out_required != NULL)
		*out_required = required;
	return buffer;
}

ParsedUrl *parse_url(const char *url, int length, void *buffer, 
	unsigned buffer_size, unsigned flags, const char *default_scheme)
{
	UrlParseCode code = URLPARSE_MALFORMED; // Assume the worst.

	/* Our canonicalization only changes case, so the output is always the same 
	 * length as the input. If we're breaking into parts, we might insert up
	 * to two extra terminators. */
	if (length < 0)
		length = (int)strlen(url);
	unsigned max_canonical_length = (unsigned)length + URL_MAX_EXTRA;
	unsigned max_length = MAX_URL_LENGTH;
	if (buffer != NULL) {
		ensure(buffer_size >= sizeof(ParsedUrl));
		max_length = buffer_size - sizeof(ParsedUrl);
		if (max_length > MAX_URL_LENGTH)
			max_length = MAX_URL_LENGTH;
	}
	bool too_long = max_canonical_length > max_length;
	if (too_long)
		max_canonical_length = 0;
	ParsedUrl *result = NULL;
	if (buffer == NULL) {
		size_t bytes_required = sizeof(ParsedUrl) + max_canonical_length; 
		result = (ParsedUrl *)(new char[bytes_required]);
	} else {
		result = (ParsedUrl *)buffer;
	}
	
	char *q = result->url;
	if (too_long) {
		code = URLPARSE_TOO_LONG;
		goto done;
	}

	/* Scan over what might be the scheme or the first part of the host. */
	const char *p = url, *end = url + length;
	bool treat_as_host_name = false;
	while (p != end && *p != ':' && *p != '/') {
		treat_as_host_name |= (*p == '.');
		++p;
	}

	const char *scheme = NULL;
	if (p + 3 <= end && *p == ':' && p[1] == '/' && p[2] == '/') {
		/* There must be something both before and after "://" for the URL to 
		 * make sense. */
		if (p == url || p + 3 == end)
			goto done; 
		scheme = url;
		result->scheme_length = (unsigned short)(p - url);
		p += 3;
	} else {
		/* What we've read so far isn't a scheme, so this is not a valid 
		 * absolute URL. Try to guess whether it's a host name, in which case
		 * we treat this as an absolute URL with an omitted scheme, or something
		 * else, in which case we treat it as a relative URL. */
		if (treat_as_host_name) {
			scheme = default_scheme;
			result->scheme_length = (unsigned short)strlen(default_scheme);
		} else {
			scheme = NULL;
			result->scheme_length = 0;
		}
		p = url; // Rewind to re-read what we've read so far as part of the
		         // host or path.
	}

	/* If the URL is absolute, write out the canonical form of the scheme. */
	if (scheme != NULL) {
		for (unsigned i = 0; i < result->scheme_length; ++i)
			*q++ = (char)tolower(scheme[i]);
		if ((flags & URLPARSE_TERMINATE_PARTS) != 0) {
			*q++ = '\0';
		} else {
			q[0] = ':';
			q[1] = '/';
			q[2] = '/';
			q += 3;
		}
	}

	/* Read the host part. */
	result->host_start = (unsigned short)(q - result->url);
	result->host_length = 0;
	result->port = 0;
	if (scheme != NULL && p != end && isalnum(*p)) {
		do {
			*q++ = (char)tolower(*p++);
			result->host_length++;
		} while (p != end && (isalnum(*p) || *p == '.' || *p == '-'));
		/* A port? */
		if (p != end && *p == ':') {
			if ((flags & URLPARSE_TERMINATE_PARTS) == 0)
				*q++ = ':';
			const char *port_end;
			char pbuf[6];
			unsigned plen = 0;
			for (++p; p != end && isdigit(*p) && plen != sizeof(pbuf); ++p)
				pbuf[plen++] = *p;
			if (plen == 0 || plen == sizeof(pbuf)) {
				code = URLPARSE_INVALID_PORT;
				goto done;
			}
			pbuf[plen] = '\0';
			unsigned long port_number = strtoul(pbuf, (char **)&port_end, 10);
			if (port_end != pbuf + plen || port_number > 65535u) {
				code = URLPARSE_INVALID_PORT;
				goto done;
			}
			if ((flags & URLPARSE_TERMINATE_PARTS) == 0) {
				memcpy(q, pbuf, plen);
				q += plen;
			}
			result->port = (unsigned short)port_number;
		}
	} else {
		/* No host part. */
		result->host_length = 0;
	}
	if ((flags & URLPARSE_TERMINATE_PARTS) != 0)
		*q++ = '\0';

	/* The rest of the URL is the path, which we don't try to interpret,
	 * perhaps followed by a query string. */
	result->path_start = (unsigned short)(q - result->url);
	result->num_extensions = 0;
	if (p != end) {
		/* If the URL is absolute and it has a path, the path must start 
		 * with '/'. */
		if (scheme != NULL && *p != '/') {
			code = URLPARSE_INVALID_HOST;
			goto done;
		}

		/* Write out the path. */
		for (result->path_length = 0; p + result->path_length != end &&
			p[result->path_length] != '?'; ++result->path_length);
		result->query_length = (unsigned short)(url + length - p - 
			result->path_length);
		memcpy(q, p, result->path_length);

		/* Read up to four extensions from the end of the path. */
		const char *dot = q + result->path_length;
		unsigned i;
		for (i = 0; i < PARSED_URL_MAX_EXTENSIONS; ) {
			int el;
			for (el = 0; dot - 1 - el >= q && !ispathsep(dot[-1 - el]); ++el) {
				if (dot[-1 - el] == '.') {
					result->extension_starts[i] = (unsigned short)
						(dot - el - result->url);
					result->extension_lengths[i] = (unsigned short)el;
					++i;
					break;
				}
			}
			dot -= el + 1;
			if (dot - 1 <= q || ispathsep(*dot))
				break;
		}
		result->num_extensions = (unsigned short)i;
		q += result->path_length;

		/* Write out the query string. */
		if ((flags & URLPARSE_TERMINATE_PARTS) != 0) 
			*q++ = '\0';
		result->query_start = (unsigned short)(q - result->url);
		memcpy(q, p + result->path_length, result->query_length);
		q += result->query_length;
	} else {
		/* There's no path. We normalize it to '/'. */
		*q++ = '/';
		result->path_length = 1;
		result->query_length = 0;
		if ((flags & URLPARSE_TERMINATE_PARTS) != 0)
			*q++ = '\0';
		result->query_start = (unsigned short)(q - result->url);
	}
	*q = '\0';

	code = URLPARSE_OK;

done:
	if (code != URLPARSE_OK) {
		result->url[0] = '\0';
	} else {
		result->length = (unsigned short)(q - result->url);
		url_decode(result->url, result->length + 1, flags);
		/* Set unused extension slots to an empty string. */
		for (unsigned i = result->num_extensions; 
			i != PARSED_URL_MAX_EXTENSIONS; ++i) {
			result->extension_starts[i] = result->length;
			result->extension_lengths[i] = 0;
		}
	}
	result->code = code;
	return result;
}

/* Duplicates a ParsedUrl object. If a buffer is supplied, the result is always
 * stored in the buffer. If the source URL is too long to fit, an empty URL
 * with code URLPARSE_TOO_LONG is returned. If no buffer is supplied, the result
 * is always heap allocated and the function does not fail. */
ParsedUrl *copy_parsed_url(const ParsedUrl *url, void *buffer, 
	unsigned buffer_size)
{
	ensure(buffer == NULL || buffer_size >= sizeof(ParsedUrl));
	unsigned total_size = sizeof(ParsedUrl) - 1 + url->length + 1;
	ParsedUrl *result = (ParsedUrl *)buffer;
	if (result != NULL) {
		if (total_size > buffer_size) {
			memset(result, 0, sizeof(ParsedUrl));
			result->code = URLPARSE_TOO_LONG;
			return result;
		}
	} else {
		result = (ParsedUrl *)(new char[total_size]);
	}
	memcpy(result, (const void *)url, total_size);
	return result;
}

 /* MurmurHash3 by Austin Appleby. */
static uint64_t murmur3_64(const void *key, const int len)
{
	static const unsigned SEED = 0;

	const int nblocks = len / 16;
	unsigned h1 = SEED, h2 = SEED, h3 = SEED, h4 = SEED;
	unsigned c1 = 0x239b961b; 
	unsigned c2 = 0xab0e9789;
	unsigned c3 = 0x38b34ae5; 
	unsigned c4 = 0xa1e38b93;

	#define rotl32(x, r) ((x << r) | (x >> (32 - r)))

	const unsigned *blocks = (const unsigned *)key + nblocks * 4;
	for (int i = -nblocks; i; i++) {
		unsigned k1 = blocks[i * 4 + 0];
		unsigned k2 = blocks[i * 4 + 1];
		unsigned k3 = blocks[i * 4 + 2];
		unsigned k4 = blocks[i * 4 + 3];
		k1 *= c1; k1  = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
		h1 = rotl32(h1,19); h1 += h2; h1 = h1 * 5 + 0x561ccd1b;
		k2 *= c2; k2  = rotl32(k2, 16); k2 *= c3; h2 ^= k2;
		h2 = rotl32(h2,17); h2 += h3; h2 = h2 * 5 + 0x0bcaa747;
		k3 *= c3; k3  = rotl32(k3, 17); k3 *= c4; h3 ^= k3;
		h3 = rotl32(h3,15); h3 += h4; h3 = h3 * 5 + 0x96cd1c35;
		k4 *= c4; k4  = rotl32(k4, 18); k4 *= c1; h4 ^= k4;
		h4 = rotl32(h4,13); h4 += h1; h4 = h4 * 5 + 0x32ac3b17;
	}

	const unsigned char *tail = (const unsigned char *)key + nblocks * 16;
	unsigned k1 = 0, k2 = 0, k3 = 0, k4 = 0;
	switch (len & 15) {
		case 15: k4 ^= tail[14] << 16;
		case 14: k4 ^= tail[13] << 8;
		case 13: k4 ^= tail[12] << 0; 
		         k4 *= c4; k4  = rotl32(k4,18); k4 *= c1; h4 ^= k4;
		case 12: k3 ^= tail[11] << 24;
		case 11: k3 ^= tail[10] << 16;
		case 10: k3 ^= tail[ 9] << 8;
		case  9: k3 ^= tail[ 8] << 0; 
		         k3 *= c3; k3  = rotl32(k3, 17); k3 *= c4; h3 ^= k3;
		case  8: k2 ^= tail[ 7] << 24;
		case  7: k2 ^= tail[ 6] << 16;
		case  6: k2 ^= tail[ 5] << 8;
		case  5: k2 ^= tail[ 4] << 0;
		         k2 *= c2; k2  = rotl32(k2,16); k2 *= c3; h2 ^= k2;
		case  4: k1 ^= tail[ 3] << 24;
		case  3: k1 ^= tail[ 2] << 16;
		case  2: k1 ^= tail[ 1] << 8;
		case  1: k1 ^= tail[ 0] << 0;
		         k1 *= c1; k1  = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
	}

	#undef rotl32

	h1 ^= len; h2 ^= len; h3 ^= len; h4 ^= len;
	h1 += h2; h1 += h3; h1 += h4;
	h2 += h1; h3 += h1; h4 += h1;

	#define fmix(h) h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; \
		h *= 0xc2b2ae35; h ^= h >> 16;

	fmix(h1);
	fmix(h2);
	fmix(h3);
	fmix(h4);

	#undef fmix

	h1 += h2; h1 += h3; h1 += h4;
	h2 += h1;

	return ((uint64_t)h1 << 32) | h2;
}

/*
 * Cache Implementation
 */

static bool default_local_fetch_callback(void *, const ParsedUrl *url, 
	void **out_data, unsigned *out_size, MimeType *out_mime_type)
{
	/* Is this a local URL? */
	if (url->scheme_length != 0 && !match_scheme(url, "file"))
		return false;

	/* A query only? */
	*out_mime_type = guess_mime_type(url->url + url->extension_starts[0],
		url->extension_lengths[0]);
	if (out_data == NULL)
		return true;

	/* Make a null terminated path string. */
	char path[FILENAME_MAX + 1];
	if (url->path_length + 1 > sizeof(path))
		return false;
	const char *start = url->url + url->path_start;
	unsigned length = url->path_length;
	if (length != 0 && *start == '/') {
		start++;
		length--;
	}
	memcpy(path, start, length);
	path[length] = '\0';

	/* Try to read the file into a heap buffer. */
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return false;
	fseek(f, 0, SEEK_END);
	uint32_t s = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = new char[s + 1];
	fread(b, 1, s, f);
	fclose(f);
	b[s] = '\0';
	*out_data = b;
	*out_size = s;
	return true;
}

static size_t handle_curl_write(char *data, size_t size, size_t count, 
	FetchSlot *slot);

static void cache_lock(Cache *cache)
{
	EnterCriticalSection(&cache->lock);
}

static void cache_unlock(Cache *cache)
{
	LeaveCriticalSection(&cache->lock);
}

static void cache_initialize_fetch_slots(Cache *cache)
{
	cache->curl_multi_handle = curl_multi_init();
	ensure(cache->curl_multi_handle != NULL);
	for (unsigned i = 0; i < cache->num_fetch_slots; ++i) {
		FetchSlot *slot = cache->fetch_slots + i;
		slot->buffer = NULL;
		slot->data_size = 0;
		slot->capacity = 0;
		slot->curl_handle = NULL;
		slot->key = 0ULL;
		slot->state = URL_FETCH_IDLE;
		slot->cache = cache;

		CURL *curl_handle = curl_easy_init();
		assert(curl_handle != NULL);
		if (curl_handle == NULL) {
			cache->num_fetch_slots = i;
			break;
		}
		curl_easy_setopt(curl_handle, CURLOPT_PRIVATE, (void *)i);
		curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, &handle_curl_write);
		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, slot);
		curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1);
		curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1);	
		slot->curl_handle = curl_handle;
	}
}

static void cache_deinitialize_fetch_slots(Cache *cache)
{
	for (unsigned i = 0; i < cache->num_fetch_slots; ++i) {
		FetchSlot *slot = cache->fetch_slots + i;
		curl_multi_remove_handle(cache->curl_multi_handle, slot->curl_handle);
		curl_easy_cleanup(slot->curl_handle);
		delete [] slot->buffer;
	}
	curl_multi_cleanup(cache->curl_multi_handle);
}

static unsigned cache_notify_handle(const Cache *cache, const Handle *handle,
	UrlNotification type, unsigned defval = 0)
{
	unsigned rc = defval;
	int sink_id = handle->notify;
	if (sink_id != INVALID_NOTIFY_SINK_ID) {
		const NotifySink *sink = cache->sinks + sink_id;
		if (sink->callback != NULL) {
			rc = sink->callback((UrlHandle)handle, type, 
				handle->entry->key, sink->user_data, handle->user_data, 
				handle->entry->fetch_state);
		}
	}
	return rc;
}

static void cache_detach_entry_handles(Cache *cache, Entry *entry)
{
	/* First pass: notify each handle of the entry's eviction. The callback 
	 * may destroy the handle. */
	Handle *handle = entry->handles;
	while (handle != NULL) {
		Handle *next = handle->next;
		cache_notify_handle(cache, handle, URL_NOTIFY_EVICT);
		handle = next;
	}
	/* Second pass: detach any remaining handles from the entry. */
	handle = entry->handles;
	while (handle != NULL) {
		Handle *next = handle->next;
		handle->entry = NULL;
		handle->prev = NULL;
		handle->next = NULL;
		handle = next;
	}
	entry->handles = NULL;
}

static void cache_deallocate_entry(Cache *cache, Entry *entry)
{
	assert(entry->lock_count == 0);
	if (entry->lock_count != 0)
		return; // The assert didn't abort. The best we can do is leak it.
	cache_detach_entry_handles(cache, entry);
	if (entry->url != NULL)
		delete [] entry->url;
	if (entry->data != NULL)
		delete [] entry->data;
	delete entry;
}

static void cache_clear(Cache *cache)
{
	cache_lock(cache);
	EntryHash::const_iterator iter = cache->entries.begin();
	while (iter != cache->entries.end()) {
		Entry *entry = iter->second;
		cache_deallocate_entry(cache, entry);
		++iter;
	}
	cache->entries.clear();
	for (unsigned i = 0; i < NUM_PRIORITY_LEVELS; ++i) {
		cache->fetch_head[i] = NULL;
		cache->fetch_tail[i] = NULL;
	}
	cache_unlock(cache);
}

/* Returns a key uniquely identifying a URL. */
static UrlKey cache_make_key(const char *url, int length = -1)
{
	char buffer[URL_PARSE_BUFFER_SIZE];
	ParsedUrl *parsed = parse_url(url, length, buffer, sizeof(buffer));
	return parsed->code != URLPARSE_OK ? INVALID_URL_KEY : 
		murmur3_64(parsed->url, parsed->length);
}

static Entry *cache_allocate_entry(Cache *cache, uint64_t key, 
	ParsedUrl *parsed_url, UrlFetchState fetch_state, 
	unsigned ttl_secs, unsigned flags)
{
	Entry *entry = new Entry();
	entry->key = key;
	entry->url = parsed_url;
	entry->fetch_state = fetch_state;
	entry->data = NULL;
	entry->data_size = 0;
	entry->lock_count = 0;
	entry->mime_type = MIMETYPE_NONE;
	entry->flags = flags;
	entry->fetch_prev = NULL;
	entry->fetch_next = NULL;
	entry->priority = URLP_UNSET;
	entry->last_used = cache->clock;
	entry->ttl_secs = ttl_secs;
	entry->handles = NULL;
	return entry;
}

static void cache_set_entry_data(Entry *entry, const void *data, unsigned size,
	MimeType mime_type, bool copy)
{
	if (entry->data == data)
		return;
	if (copy) {
		if (entry->data_size != size) {
			delete [] entry->data;
			entry->data = size != 0 ? new char[size] : NULL;
		}
		if (data != NULL)
			memcpy(entry->data, data, size);
	} else {
		if (entry->data != NULL)
			delete [] entry->data;
		entry->data = (void *)data;
	}
	entry->data_size = size;
	entry->mime_type = mime_type;
}

static void cache_remove_entry_from_fetch_queue(Cache *cache, Entry *entry)
{
	if (entry->priority == URLP_UNSET)
		return;
	if (entry->fetch_prev != NULL)
		entry->fetch_prev->fetch_next = entry->fetch_next;
	else
		cache->fetch_head[entry->priority] = entry->fetch_next;
	if (entry->fetch_next != NULL)
		entry->fetch_next->fetch_prev = entry->fetch_prev;
	else
		cache->fetch_tail[entry->priority] = entry->fetch_prev;
	entry->fetch_next = NULL;
	entry->fetch_prev = NULL;
	entry->priority = URLP_UNSET;
}

static void cache_add_entry_to_fetch_queue(Cache *cache, Entry *entry, 
	UrlFetchPriority priority)
{
	if (entry->priority == priority)
		return;
	if (entry->priority != URLP_UNSET)
		cache_remove_entry_from_fetch_queue(cache, entry);
	entry->fetch_prev = cache->fetch_tail[priority];
	entry->fetch_next = NULL;
	if (cache->fetch_tail[priority] != NULL) {
		cache->fetch_tail[priority]->fetch_next = entry;
	} else {
		cache->fetch_head[priority] = entry;
	}
	cache->fetch_tail[priority] = entry;
	entry->priority = priority;
	ensure(priority == URLP_NO_FETCH || entry->url != NULL); // We need a URL to be in the fetch queue.
}

static Entry *cache_get(Cache *cache, uint64_t key)
{
	EntryHash::iterator iter = cache->entries.find(key);
	return iter != cache->entries.end() ? iter->second : NULL;
}

/* Helper to retrieve a cache entry by URL or key. */
static Entry *cache_get(Cache *cache, const char *url, int length, uint64_t key,
	ParsedUrl **out_parsed_url, uint64_t *out_key)
{
	/* Parse the URL. */
	*out_parsed_url = NULL;
	*out_key = key;
	if (url != NULL) {
		ParsedUrl *parsed = parse_url(url, length);
		if (parsed->code != URLPARSE_OK) {
			delete [] parsed;
			return NULL;
		}
		key = murmur3_64(parsed->url, parsed->length);
		*out_parsed_url = parsed;
		*out_key = key;
	}
	return cache_get(cache, key);
}

static bool cache_is_local_url(Cache *cache, const ParsedUrl *url)
{
	MimeType mime_type;
	unsigned size;
	return cache->fetch_local != NULL ? cache->fetch_local(
		cache->fetch_local_data, url, NULL, &size, &mime_type) : false;
}

static Entry *cache_insert(Cache *cache, uint64_t key, 
	ParsedUrl *parsed_url, UrlFetchPriority priority, 
	unsigned ttl_secs, unsigned flags)
{
	UrlFetchState fetch_state;
	if (cache_is_local_url(cache, parsed_url)) {
		fetch_state = URL_FETCH_DISK;
		priority = URLP_NO_FETCH; // Disk entries live in the NO_FETCH queue.
	} else {
		if (priority != URLP_NO_FETCH)
			fetch_state = URL_FETCH_QUEUED;
		else
			fetch_state = URL_FETCH_IDLE;
	}
	Entry *entry = cache_allocate_entry(cache, key, parsed_url, fetch_state, ttl_secs, flags);
	cache_add_entry_to_fetch_queue(cache, entry, priority);
	cache->entries[key] = entry;
	return entry;
}

static void cache_delete_entry(Cache *cache, Entry *entry)
{
	ensure(entry->lock_count == 0);
	cache_remove_entry_from_fetch_queue(cache, entry);
	cache->entries.erase(entry->key);
	cache_deallocate_entry(cache, entry);
}

static Handle *cache_find_handle_by_sink(Entry *entry, int sink_id)
{
	for (Handle *handle = entry->handles; handle != NULL; handle = handle->next)
		if (handle->notify == sink_id)
			return handle;
	return NULL;
}

static Handle *cache_find_handle_by_user_data(Entry *entry, void *user_data)
{
	for (Handle *handle = entry->handles; handle != NULL; handle = handle->next)
		if (handle->user_data == user_data)
			return handle;
	return NULL;
}

/* Deletes an entry, or, if a handle requires that the URL be kept, clears the
 * entry's data leaving the entry itself (and its URL) intact. */
static void cache_evict_entry(Cache *cache, Entry *entry, unsigned handle_flags)
{
	if ((handle_flags & URL_FLAG_KEEP_URL) != 0 && entry->url != NULL)
		cache_set_entry_data(entry, NULL, 0, MIMETYPE_NONE, false);
	else
		cache_delete_entry(cache, entry);
}

static Handle *cache_add_handle(Cache *cache, Entry *entry, 
	void *user_data, unsigned user_data_size, int sink_id, unsigned flags)
{
	cache;
	
	Handle *handle = NULL;
	if ((flags & URL_FLAG_REUSE_DATA_HANDLE) != 0) {
		handle = cache_find_handle_by_user_data(entry, user_data);
	} else if ((flags & URL_FLAG_REUSE_SINK_HANDLE) != 0) {
		handle = cache_find_handle_by_sink(entry, sink_id);
	}
	if (handle == NULL) {
		handle = new Handle();
		handle->entry = entry;
		handle->next = entry->handles;
		handle->prev = NULL;
		if (entry->handles != NULL)
			entry->handles->prev = handle;
		entry->handles = handle;
		handle->notify = (short)sink_id;
		handle->flags = (unsigned short)flags;
		handle->user_data = user_data;
		handle->data_size = user_data_size;
	}
	return handle;
}

static void cache_notify_handles(const Cache *cache, Entry *entry, 
	UrlNotification type)
{
	for (Handle *handle = entry->handles; handle != NULL; handle = handle->next)
		cache_notify_handle(cache, handle, type);
}

/* Broadcasts a notification to all handles associated with a sink.*/
static void cache_notify_sink_handles(Cache *cache, int sink_id, 
	UrlNotification notification)
{
	for (unsigned priority = 0; priority < NUM_PRIORITY_LEVELS; ++priority) {
		for (const Entry *entry = cache->fetch_head[priority]; entry != NULL; 
			entry = entry->fetch_next) {
			for (const Handle *handle = entry->handles, *next; 
				handle != NULL; handle = next) {
				next = handle->next;
				if (handle->notify == sink_id)
					cache_notify_handle(cache, handle, notification);
			}
		}
	}
}

/* Returns the union of flag bits from an entry's handles. */
static unsigned cache_handle_flags(const Entry *entry)
{
	unsigned flags = 0;
	for (const Handle *handle = entry->handles; handle != NULL; 
		handle = handle->next) {
		flags |= handle->flags;
	}
	return flags;
}

static UrlFetchState cache_query_key(Cache *cache, UrlKey key, 
	unsigned *out_size, MimeType *out_mime_type, UrlFetchPriority *out_priority)
{
	UrlFetchState fetch_state = URL_FETCH_IDLE;
	UrlFetchPriority priority = URLP_NO_FETCH;
	MimeType mime_type = MIMETYPE_NONE;
	unsigned size = 0;
	cache_lock(cache);
	Entry *entry = cache_get(cache, key);
	if (entry != NULL) {
		fetch_state = entry->fetch_state;
		size = entry->data_size;
		mime_type = entry->mime_type;
		priority = entry->priority;
	}
	if (out_size != NULL)
		*out_size = size;
	if (out_mime_type != NULL)
		*out_mime_type = mime_type;
	if (out_priority != NULL)
		*out_priority = priority;
	cache_unlock(cache);
	return fetch_state;
}

static UrlFetchState cache_query_handle(Cache *cache, Handle *handle, 
	unsigned *out_size, MimeType *out_mime_type, UrlFetchPriority *out_priority,
	void **out_user_data)
{
	UrlFetchState fetch_state = URL_FETCH_IDLE;
	UrlFetchPriority priority = URLP_NO_FETCH;
	MimeType mime_type = MIMETYPE_NONE;
	unsigned size = 0;
	if (handle != NULL) {
		cache_lock(cache);
		Entry *entry = handle->entry;
		if (entry != NULL) {
			fetch_state = entry->fetch_state;
			size = entry->data_size;
			mime_type = entry->mime_type;
			priority = entry->priority;
		}
		cache_unlock(cache);
	}
	if (out_size != NULL)
		*out_size = size;
	if (out_mime_type != NULL)
		*out_mime_type = mime_type;
	if (out_priority != NULL)
		*out_priority = priority;
	if (out_user_data != NULL)
		*out_user_data = handle->user_data;
	return fetch_state;
}

static Entry *cache_request_url(Cache *cache, const char *url, int length,
	UrlKey key, UrlFetchPriority priority, unsigned ttl_secs, unsigned flags)
{
	/* Update the entry, creating it if necessary. */
	ParsedUrl *parsed = NULL;
	Entry *entry = cache_get(cache, url, length, key, &parsed, &key);
	if (entry == NULL) {
		if (parsed != NULL) {
			entry = cache_insert(cache, key, parsed, priority, ttl_secs, flags);
			parsed = NULL; // The entry takes ownership of the ParsedUrl.
		}
	} else {
		entry->flags = flags;
		/* If the URL hasn't already been fetched, move it to the requested
		 * fetch queue. */
		if (entry->fetch_state == URL_FETCH_IDLE || 
		    entry->fetch_state == URL_FETCH_QUEUED) {
			/* An entry must have a URL to be in the fetch queue. */
			if (entry->url == NULL) {
				entry->url = parsed;
				parsed = NULL;
			}
			if (entry->url != NULL)
				cache_add_entry_to_fetch_queue(cache, entry, priority);
		}
		if (ttl_secs < entry->ttl_secs)
			entry->ttl_secs = ttl_secs;
	}
	if (parsed != NULL)
		delete [] parsed;
	return entry;
}

static UrlKey cache_request(Cache *cache, const char *url, int length, 
	UrlKey key, UrlFetchPriority priority, unsigned ttl_secs, 
	unsigned flags)
{
	cache_lock(cache);
	Entry *entry = cache_request_url(cache, url, length, INVALID_URL_KEY, 
		priority, ttl_secs, flags);
	key = entry != NULL ? entry->key : INVALID_URL_KEY;
	cache_unlock(cache);
	return key;
}

static void cache_load_from_disk(Cache *cache, Entry *entry)
{
	ensure(entry->url != NULL);
	ensure(entry->fetch_state == URL_FETCH_DISK);

	/* Is disk loading enabled? */
	if (cache->fetch_local == NULL)
		return;

	/* Drop the lock and read the data. */
	entry->lock_count++;
	cache_unlock(cache);
	void *data = NULL;
	unsigned size = 0;
	MimeType mime_type = MIMETYPE_NONE;
	bool success = cache->fetch_local(cache->fetch_local_data, 
		entry->url, &data, &size, &mime_type);
	cache_lock(cache);
	entry->lock_count--;

	/* Store the data in the entry. */
	if (success)
		cache_set_entry_data(entry, data, size, mime_type, false);
}

static const void *cache_lock_data(Cache *cache, Entry *entry, 
	unsigned *out_size, MimeType *out_mime_type)
{
	const void *data = NULL;
	unsigned size = 0;
	MimeType mime_type = MIMETYPE_NONE;
	if (entry != NULL) {
		if (entry->fetch_state == URL_FETCH_DISK)
			cache_load_from_disk(cache, entry);
		if (entry->data != NULL) {
			data = entry->data;
			size = entry->data_size;
			mime_type = entry->mime_type;
			entry->lock_count++;
		}
	}
	if (out_size != NULL)
		*out_size = size;
	if (out_mime_type != NULL)
		*out_mime_type = mime_type;
	return data;
}

static void cache_unlock_data(Cache *cache, Entry *entry, unsigned flags)
{
	cache_lock(cache);
	ensure(entry != NULL);
	ensure(entry->lock_count != 0);
	flags |= entry->flags;
	if (--entry->lock_count == 0) {
		/* Disk buffers are released as soon as they are unlocked. */
		if (entry->fetch_state == URL_FETCH_DISK || 
			(flags & URL_FLAG_DISCARD) != 0) {
			cache_set_entry_data(entry, NULL, 0, MIMETYPE_NONE, false);
			if (entry->fetch_state != URL_FETCH_DISK)
				entry->fetch_state = URL_FETCH_IDLE;
		}
		entry->last_used = cache->clock;
	}
	cache_unlock(cache);
}

static const void *cache_lock_key(Cache *cache, UrlKey key, unsigned *out_size,
	MimeType *out_mime_type)
{
	cache_lock(cache);
	const void *data = cache_lock_data(cache, cache_get(cache, key), 
		out_size, out_mime_type);
	cache_unlock(cache);
	return data;
}

static void cache_unlock_key(Cache *cache, UrlKey key)
{
	cache_lock(cache);
	cache_unlock_data(cache, cache_get(cache, key), 0);
	cache_unlock(cache);
}

static const void *cache_lock_handle(Cache *cache, UrlHandle handle,
	unsigned *out_size, MimeType *out_mime_type)
{
	cache_lock(cache);
	Entry *entry = handle != NULL ? ((Handle *)handle)->entry : NULL;
	const void *data = cache_lock_data(cache, entry, out_size, out_mime_type);
	cache_unlock(cache);
	return data;
}

static void cache_unlock_handle(Cache *cache, UrlHandle handle)
{
	cache_lock(cache);
	Entry *entry = NULL;
	unsigned flags = 0;
	if (handle != NULL) {
		entry = ((const Handle *)handle)->entry;
		flags = ((const Handle *)handle)->flags;
	}
	cache_unlock_data(cache, entry, flags);
	cache_unlock(cache);
}

/* Returns a handle's user data pointer. */
static void *cache_get_user_data(Cache *cache, UrlHandle handle)
{
	cache;
	return handle != NULL ? ((Handle *)handle)->user_data : NULL;
}

/* Atomically sets a handle's user data pointer and copies the value of 
 * URL_FLAG_PREVENT_EVICT from 'flags' into the handle. The suplied data size is
 * the handle's reported contribution to the cache's memory usage. */
static void *cache_set_user_data(Cache *cache, UrlHandle handle, 
	void *user_data, unsigned data_size, unsigned flags)
{
	cache_lock(cache);
	if (handle != NULL) {
		Handle *h = (Handle *)handle;
		std::swap(h->user_data, user_data);
		h->flags &= ~URL_FLAG_PREVENT_EVICT;
		h->flags |= flags & URL_FLAG_PREVENT_EVICT;
		h->data_size = data_size;
	} else {
		user_data = NULL;
	}
	cache_unlock(cache);
	return user_data;
}

/* Sets or clears a mask of handle flags. */
static void cache_set_handle_flags(Cache *cache, UrlHandle handle, 
	unsigned mask, bool value)
{
	if (handle == NULL)
		return;
	cache_lock(cache);
	Handle *h = (Handle *)handle;
	if (value)
		h->flags |= mask;
	else
		h->flags &= ~mask;
	cache_unlock(cache);
}

static ParsedUrl *cache_get_url(Cache *cache, UrlHandle handle, 
	void *buffer = NULL, unsigned buffer_size = 0)
{
	Handle *h = (Handle *)handle;
	if (h == NULL)
		return NULL;
	ParsedUrl *result = NULL;
	cache_lock(cache);
	Entry *entry = h->entry;
	if (entry != NULL && entry->url != NULL)
		result = copy_parsed_url(entry->url, buffer, buffer_size);
	cache_unlock(cache);
	return result;
}

static bool cache_set_data(Cache *cache, const char *url, int length,
	const void *data, unsigned size, MimeType mime_type,
	unsigned ttl_secs, bool copy = true)
{
	/* Replace the entry's data, creating a new entry if necessary. */
	bool success = false;
	cache_lock(cache);
	ParsedUrl *parsed = NULL;
	uint64_t key = INVALID_URL_KEY;
	Entry *entry = cache_get(cache, url, length, INVALID_URL_KEY, &parsed, &key);
	if (entry == NULL) {
		/* No need to create an entry if there's no data. */
		if (data == NULL || size == 0)
			goto done;
		entry = cache_insert(cache, key, parsed, URLP_NO_FETCH, ttl_secs, 0);
		parsed = NULL; // The entry takes ownership of the ParsedUrl buffer.	
	} else {
		cache_add_entry_to_fetch_queue(cache, entry, URLP_NO_FETCH);
		entry->fetch_state = URL_FETCH_IDLE; 
		entry->ttl_secs = ttl_secs;
	}
	cache_set_entry_data(entry, data, size, mime_type, copy);
	success = true;

done:
	cache_unlock(cache);
	if (parsed != NULL)
		delete [] parsed;
	return success;
}

static void cache_handle_response_data(Cache *cache, unsigned slot_number,
	const void *data, unsigned data_size)
{
	FetchSlot *slot = cache->fetch_slots + slot_number;
	unsigned required = slot->data_size + data_size;
	if (required > slot->capacity) {
		unsigned new_capacity = ((required * 3 / 2) + 4095) & -4096;
		char *block = new char[new_capacity];
		if (slot->buffer != NULL) {
			memcpy(block, slot->buffer, slot->data_size);
			delete [] slot->buffer;
		}
		slot->buffer = block;
		slot->capacity = new_capacity;
	}
	memcpy(slot->buffer + slot->data_size, data, data_size);
	slot->data_size = required;
}

/* CURL calls this function when new data arrives. */
static size_t handle_curl_write(char *data, size_t size, size_t count, 
	FetchSlot *slot)
{
	unsigned data_size = unsigned(size * count);
	cache_handle_response_data(slot->cache, slot - slot->cache->fetch_slots, 
		data, data_size);
	return data_size;
}

static void cache_handle_request_complete(Cache *cache, unsigned slot_number, 
	CURLcode code)
{
	/* Remove the slot's CURL easy handle from the multi stack. */
	FetchSlot *slot = cache->fetch_slots + slot_number;
	curl_multi_remove_handle(cache->curl_multi_handle, slot->curl_handle);
	
	/* Get the entry for the URL the slot is downloading. */
	Entry *entry = cache_get(cache, slot->key);
	if (entry == NULL || entry->lock_count != 0)
		return; /* Entry has been deleted or is locked. Discard the data. */

	/* If the request succeeded, copy the data into the entry. */
	long http_status;
	curl_easy_getinfo(slot->curl_handle, CURLINFO_RESPONSE_CODE, &http_status);
	if (code == CURLE_OK) {
		const char *content_type;
		curl_easy_getinfo(slot->curl_handle, CURLINFO_CONTENT_TYPE, &content_type);
		MimeType mime_type = find_mime_type_by_name(content_type);
		cache_set_entry_data(entry, slot->buffer, slot->data_size, mime_type, true);
		entry->fetch_state = URL_FETCH_SUCCESSFUL;
		entry->last_used = cache->clock;
	} else {
		entry->fetch_state = URL_FETCH_FAILED;
	}
	slot->state = URL_FETCH_IDLE;

	/* Notify the entry of its new status. */
	cache_notify_handles(cache, entry, URL_NOTIFY_FETCH);
	
	/* We no longer need the URL. */
	unsigned flags = entry->flags | cache_handle_flags(entry);
	if ((flags & URL_FLAG_KEEP_URL) == 0) {
		delete [] (char *)entry->url;
		entry->url = NULL;
	}
}

static void cache_update_fetch_slots(Cache *cache)
{
	/* Dispatch write calls. */
	int running_handles, queue_size;
	CURLMcode rc;
	do {
		rc = curl_multi_perform(cache->curl_multi_handle, &running_handles);
	} while (rc == CURLM_CALL_MULTI_PERFORM);

	/* Dequeue completion messages and notify the corresponding HttpDownloader 
	 * instance. */
	CURLMsg *message;
	do {
		message = curl_multi_info_read(cache->curl_multi_handle, &queue_size);
		if (message != NULL && message->msg == CURLMSG_DONE) {
			intptr_t slot_number;
			CURLcode rc = curl_easy_getinfo(message->easy_handle, 
				CURLINFO_PRIVATE, &slot_number);
			if (rc == CURLE_OK) {
				cache_handle_request_complete(cache, slot_number, 
					message->data.result);
			}
		}
	} while (message != NULL);
}

static void cache_populate_fetch_slots(Cache *cache)
{
	for (unsigned i = 0; i < cache->num_fetch_slots; ++i) {
		/* Is this slot idle? */
		FetchSlot *slot = cache->fetch_slots + i;
		if (slot->state != URL_FETCH_IDLE)
			continue;

		/* Dequeue the most urgent entry. */
		Entry *entry = NULL;
		for (unsigned j = NUM_PRIORITY_LEVELS - 1; j != URLP_NO_FETCH; --j) {
			entry = cache->fetch_head[j];
			if (entry != NULL && entry->lock_count == 0)
				break;
		}
		if (entry == NULL)
			break;
		cache_add_entry_to_fetch_queue(cache, entry, URLP_NO_FETCH);

		/* Start the download. */
		ensure(entry->url != NULL);
		curl_easy_setopt(slot->curl_handle, CURLOPT_URL, entry->url->url);
		curl_easy_setopt(slot->curl_handle, CURLOPT_FOLLOWLOCATION, 1);
		CURLMcode rc = curl_multi_add_handle(cache->curl_multi_handle, slot->curl_handle);
		if (rc == CURLM_OK) {
			slot->key = entry->key;
			slot->data_size = 0;
			slot->state = URL_FETCH_IN_PROGRESS;
			entry->fetch_state = URL_FETCH_IN_PROGRESS;
		} else {
			entry->fetch_state = URL_FETCH_FAILED;
			i--;
		}
	}
}

static void cache_evict_lru(Cache *cache)
{
	const unsigned MAX_EVICTABLE = 32;

	/* Scan for evictable entries. */
	struct Evictable {
		Entry *entry;
		unsigned size;
		unsigned flags;
	} evictable[MAX_EVICTABLE];
	unsigned num_evictable = 0, memory_used = 0;
	clock_t now = cache->clock;
	Entry *entry, *next;
	for (entry = cache->fetch_head[URLP_NO_FETCH]; entry != NULL; entry = next) {
		next = entry->fetch_next;

		/* Calculate the memory cost of the entry, accounting for the user
		 * data in each handle. */
		unsigned data_size = sizeof(Entry) + entry->data_size;
		unsigned handle_flags = 0;
		for (Handle *handle = entry->handles; handle != NULL; 
			handle = handle->next) {
			handle_flags |= handle->flags;
			data_size += handle->data_size;
		}
		memory_used += data_size;

		/* Do nothing if a handle is preventing eviction. */
		if ((handle_flags & URL_FLAG_PREVENT_EVICT) != 0)
			continue;

		/* If the entry has no data and we need to keep the entry itself
		 * because a handle requires the URL, there's nothing to do. */
		if (entry->data_size == 0 && entry->url != NULL && 
			(handle_flags & URL_FLAG_KEEP_URL) != 0)
			continue;

		/* Locked entries can't be evicted. */
		if (entry->lock_count != 0)
			continue;

		/* If the entry is past its TTL, evict it immediately. If not, add it to
		 * the eviction list.*/
		clock_t age = now - entry->last_used;
		if (entry->ttl_secs != 0 && age > clock_t(entry->ttl_secs * CLOCKS_PER_SEC)) {
			memory_used -= data_size;
			cache_evict_entry(cache, entry, handle_flags);
		} else {
			evictable[num_evictable].entry = entry;
			evictable[num_evictable].size = data_size;
			evictable[num_evictable].flags = handle_flags;
			if (++num_evictable == MAX_EVICTABLE)
				break;
		}
	}
	if (cache->memory_limit == 0 || memory_used <= cache->memory_limit ||
		num_evictable == 0)
		return;
	
	/* Put the eviction list into heap order, with the entry we want to evict
	 * most, first. */
	static const unsigned SIZE_BLOCK_BYTES     = 0x40000;
	static const unsigned LRU_GRANULARITY_MSEC = 10 * 1000;
	struct {
		bool operator () (const Evictable &a, const Evictable &b) const { 
			unsigned block_a = a.size / SIZE_BLOCK_BYTES;
			unsigned block_b = b.size / SIZE_BLOCK_BYTES;
			if (block_a != block_b) return block_a < block_b;
			int dt = int(a.entry->last_used - b.entry->last_used);
			if (abs(dt) > LRU_GRANULARITY_MSEC) return dt > 0;
			return a.size < b.size;
		}
	} prefer;
	std::make_heap(evictable, evictable + num_evictable, prefer);

	/* Evict entries until we're under the memory limit. */
	do {
		memory_used -= evictable[0].size;
		/* FIXME (TJM): DEBUG */
		fprintf(stderr, "Evicting %llX to save %uKB. "
			"Memory used now %uKB, limit %uKB.\n", 
			evictable[0].entry->key, evictable[0].size / 1024, 
			memory_used / 1024, cache->memory_limit / 1024);
		cache_evict_entry(cache, evictable[0].entry, evictable[0].flags);
		if (memory_used <= cache->memory_limit)
			break;
		std::pop_heap(evictable, evictable + num_evictable, prefer);
	} while (num_evictable--);
}

static UrlHandle cache_create_handle(Cache *cache, const char *url, int length, 
	UrlKey key, UrlFetchPriority priority, unsigned ttl_secs, 
	void *user_data, unsigned user_data_size, 
	int notify_sink_id, unsigned flags)
{
	Handle *handle = NULL;
	cache_lock(cache);
	Entry *entry = cache_request_url(cache, url, length, key, priority, 
		ttl_secs, 0);
	if (entry != NULL)
		handle = cache_add_handle(cache, entry,	user_data, user_data_size,
			notify_sink_id, flags);
	cache_unlock(cache);
	return (UrlHandle)handle;
}

/* Looks up the entry for a URL or key and searches its handle list for a handle 
 * matching either a user data pointer or a notify sink ID. */
static Handle *cache_find_handle(Cache *cache, const char *url, int length,
	UrlKey key, void *user_data, int sink_id)
{
	cache_lock(cache);
	Handle *handle = NULL;
	ParsedUrl *parsed = NULL;
	Entry *entry = cache_get(cache, url, length, key, &parsed, &key);
	if (entry != NULL) {
		if (sink_id != INVALID_NOTIFY_SINK_ID) {
			handle = cache_find_handle_by_sink(entry, sink_id);
		} else if (user_data != NULL) {
			handle = cache_find_handle_by_user_data(entry, user_data);
		}
	}
	cache_unlock(cache);
	return handle;
}

static void cache_request_handle(Cache *cache, UrlHandle handle, 
	UrlFetchPriority priority)
{
	if (handle == INVALID_URL_HANDLE)
		return;
	Handle *h = (Handle *)handle;
	cache_lock(cache);
	Entry *entry = h->entry;
	if (entry != NULL && entry->url != NULL) {
		if (entry->fetch_state == URL_FETCH_IDLE || 
		    entry->fetch_state == URL_FETCH_QUEUED)
			cache_add_entry_to_fetch_queue(cache, entry, priority);
	} 
	cache_unlock(cache);
}

static void cache_destroy_handle(Cache *cache, Handle *handle)
{
	if (handle == NULL)
		return;
	cache_lock(cache);
	Entry *entry = handle->entry;
	if (entry != NULL) {
		if (handle->next != NULL)
			handle->next->prev = handle->prev;
		if (handle->prev != NULL)
			handle->prev->next = handle->next;
		else
			entry->handles = handle->next;
	}
	delete handle;
	cache_unlock(cache);
}

static int cache_add_notify_sink(Cache *cache, NotifyCallback callback, 
	void *user_data)
{
	cache_lock(cache);
	unsigned sink_id;
	for (sink_id = 0; sink_id < MAX_NOTIFY_SINKS; ++sink_id)
		if (cache->sinks[sink_id].callback == NULL)
			break;
	ensure(sink_id != MAX_NOTIFY_SINKS);
	cache->sinks[sink_id].callback = callback;
	cache->sinks[sink_id].user_data = user_data;
	cache_unlock(cache);
	return sink_id;
}

static void cache_remove_notify_sink(Cache *cache, int sink_id)
{
	if (sink_id == INVALID_NOTIFY_SINK_ID)
		return;
	cache_lock(cache);
	assert(sink_id >= 0 && sink_id < MAX_NOTIFY_SINKS);
	assert(cache->sinks[sink_id].callback != NULL);
	cache_notify_sink_handles(cache, sink_id, URL_NOTIFY_EVICT);
	cache->sinks[sink_id].callback = NULL;
	cache->sinks[sink_id].user_data = NULL;
	cache_unlock(cache);
}

static void cache_set_notify_sink_data(Cache *cache, int sink_id, 
	NotifyCallback callback, void *user_data)
{
	cache_lock(cache);
	assert((unsigned)sink_id < MAX_NOTIFY_SINKS);
	assert(cache->sinks[sink_id].callback != NULL);
	cache->sinks[sink_id].callback = callback;
	cache->sinks[sink_id].user_data = user_data;
	cache_unlock(cache);
}

static void cache_update(Cache *cache)
{
	cache_lock(cache);
	cache->clock = clock();
	cache_update_fetch_slots(cache);
	cache_populate_fetch_slots(cache);
	cache_evict_lru(cache);
	cache_unlock(cache);
}

static void cache_initialize(Cache *cache, unsigned memory_limit,
	unsigned num_fetch_slots)
{
	InitializeCriticalSection(&cache->lock);
	cache->num_fetch_slots = num_fetch_slots;
	cache->memory_limit = memory_limit;
	cache->fetch_local = &default_local_fetch_callback;
	cache->fetch_local_data = NULL;
	for (unsigned i = 0; i < NUM_PRIORITY_LEVELS; ++i) {
		cache->fetch_head[i] = NULL;
		cache->fetch_tail[i] = NULL;
	}
	cache_initialize_fetch_slots(cache);
	for (unsigned i = 0; i < MAX_NOTIFY_SINKS; ++i) {
		cache->sinks[i].callback = NULL;
		cache->sinks[i].user_data = NULL;
	}
	cache->num_notify_sinks = 0;
	cache->clock = clock();
}

static void cache_deinitialize(Cache *cache)
{
	cache_lock(cache);
	for (unsigned i = 0; i < cache->num_notify_sinks; ++i)
		cache_remove_notify_sink(cache, (int)i);
	cache_clear(cache);
	cache_deinitialize_fetch_slots(cache);
	cache_unlock(cache);
	DeleteCriticalSection(&cache->lock);
}

/*
 * Interface
 */

UrlCache::UrlCache(unsigned memory_limit, unsigned num_fetch_slots)
{
	cache = new Cache();
	cache_initialize(cache, memory_limit, num_fetch_slots);
}

UrlCache::~UrlCache()
{
	cache_deinitialize(cache);
	delete cache;
}

void UrlCache::update(void)
{
	cache_update(cache);
}

void UrlCache::lock_cache(void)
{
	cache_lock(cache);
}

void UrlCache::unlock_cache(void)
{
	cache_unlock(cache);
}

UrlFetchState UrlCache::query(UrlKey key, unsigned *out_size, 
	MimeType *out_mime_type, UrlFetchPriority *out_priority)
{
	return cache_query_key(cache, key, out_size, out_mime_type, 
		out_priority);
}

UrlKey UrlCache::request(const char *url, int length, UrlFetchPriority priority, 
	unsigned ttl_secs, unsigned flags)
{
	return cache_request(cache, url, length, INVALID_URL_KEY, priority, 
		ttl_secs, flags);
}

void UrlCache::request(UrlKey key, UrlFetchPriority priority, 
	unsigned ttl_secs, unsigned flags)
{
	cache_request(cache, NULL, 0, key, priority, ttl_secs, flags);
}

void UrlCache::request(UrlHandle handle, UrlFetchPriority priority)
{
	cache_request_handle(cache, handle, priority);
}

const void *UrlCache::lock(UrlKey key, unsigned *out_size, 
	MimeType *out_mime_type)
{
	return cache_lock_key(cache, key, out_size, out_mime_type);
}

void UrlCache::unlock(UrlKey key)
{
	return cache_unlock_key(cache, key);
}

bool UrlCache::insert(const char *url, int url_length, const void *data, 
	unsigned size, MimeType mime_type, unsigned ttl_secs, bool copy)
{
	return cache_set_data(cache, url, url_length, data, size, mime_type, 
		ttl_secs, copy);
}

void UrlCache::set_local_fetch_callback(LocalFetchCallback callback, void *data)
{
	cache->fetch_local = callback;
	cache->fetch_local_data = data;
}

UrlHandle UrlCache::create_handle(const char *url, int length,
	UrlFetchPriority priority, unsigned ttl_secs, 
	void *user_data, unsigned user_data_size,
	int notify_sink_id, unsigned flags)
{
	return cache_create_handle(cache, url, length, INVALID_URL_KEY, 
		priority, ttl_secs, user_data, user_data_size, notify_sink_id, flags);
}

UrlHandle UrlCache::create_handle(UrlKey key, UrlFetchPriority priority, 
	unsigned ttl_secs, void *user_data, unsigned user_data_size,
	int notify_sink_id, unsigned flags)
{
	return cache_create_handle(cache, NULL, 0, key, priority, ttl_secs, 
		user_data, user_data_size, notify_sink_id, flags);
}

void UrlCache::destroy_handle(UrlHandle handle)
{
	cache_destroy_handle(cache, (Handle *)handle);
}

UrlFetchState UrlCache::query(UrlHandle handle, unsigned *out_size, 
	MimeType *out_mime_type, UrlFetchPriority *out_priority, 
	void **out_user_data)
{
	return cache_query_handle(cache, (Handle *)handle, out_size, out_mime_type, 
		out_priority, out_user_data);
}

const void *UrlCache::lock(UrlHandle handle, unsigned *out_size,
	MimeType *out_mime_type)
{
	return cache_lock_handle(cache, handle, out_size, out_mime_type);
}

void UrlCache::unlock(UrlHandle handle)
{
	cache_unlock_handle(cache, handle);
}

UrlKey UrlCache::key(const char *url, int length) const
{
	return cache_make_key(url, length);
}

UrlKey UrlCache::key(UrlHandle handle) const
{
	Handle *internal = (Handle *)handle;
	return internal != NULL && internal->entry != NULL ? 
		internal->entry->key : INVALID_URL_KEY;
}

void *UrlCache::user_data(UrlHandle handle)
{
	return cache_get_user_data(cache, handle);
}

void *UrlCache::set_user_data(UrlHandle handle, void *data, unsigned size, 
	unsigned flags)
{
	return cache_set_user_data(cache, handle, data, size, flags);
}

ParsedUrl *UrlCache::url(UrlHandle handle, void *buffer, unsigned buffer_size)
{
	return cache_get_url(cache, handle, buffer, buffer_size);
}

void UrlCache::set_notify(UrlHandle handle, int sink_id)
{
	if (handle != NULL)
		((Handle *)handle)->notify = (short)sink_id;
}

int UrlCache::add_notify_sink(NotifyCallback callback, void *user_data)
{
	return cache_add_notify_sink(cache, callback, user_data);
}

void UrlCache::remove_notify_sink(int sink_id)
{
	return cache_remove_notify_sink(cache, sink_id);
}

void UrlCache::set_notify_sink_data(int sink_id, NotifyCallback callback, 
	void *user_data)
{
	cache_set_notify_sink_data(cache, sink_id, callback, user_data);
}

bool UrlCache::is_local_url(const char *url, int length)
{
	char buf[URL_PARSE_BUFFER_SIZE];
	ParsedUrl *parsed = parse_url(url, length, buf, sizeof(buf));
	return (parsed->code == URLPARSE_OK) &&  cache_is_local_url(cache, parsed);
}

bool UrlCache::is_local_url(const ParsedUrl *url)
{
	return cache_is_local_url(cache, url);
}

UrlHandle UrlCache::find_data_handle(UrlKey key, void *user_data)
{
	return (UrlHandle)cache_find_handle(cache, NULL, -1, key, user_data, 
		INVALID_NOTIFY_SINK_ID);
}

UrlHandle UrlCache::find_data_handle(const char *url, int length, 
	void *user_data)
{
	return (UrlHandle)cache_find_handle(cache, url, length, INVALID_URL_KEY, 
		user_data, INVALID_NOTIFY_SINK_ID);
}

UrlHandle UrlCache::find_sink_handle(UrlKey key, int sink_id)
{
	return (UrlHandle)cache_find_handle(cache, NULL, -1, key, NULL, sink_id);
}

UrlHandle UrlCache::find_sink_handle(const char *url, int length, int sink_id)
{
	return (UrlHandle)cache_find_handle(cache, url, length, INVALID_URL_KEY, 
		NULL, sink_id);
}

void UrlCache::set_handle_flags(UrlHandle handle, unsigned flags, bool value)
{
	cache_set_handle_flags(cache, handle, flags, value);
}

/*
 * Unit Tests
 */

static void unit_test_url_parser(FILE *os)
{
	static const char * const TEST_URLS[] = {
		"https://www.example.org",
		"www.example.org:80/example_path/",
		"www.example.org:80/a%20large+cat/",
		"/.html..",
		"/.",
		"file:///x:/some_path/data/text.template.stacker.template",
		"example.com/abc.json?a=1&b=2",
		"c:\\myfile.txt",
		"/example/path",
		"example.com"
	};
	static const unsigned NUM_TEST_URLS = sizeof(TEST_URLS) / sizeof(TEST_URLS[0]);

	char buffer[URL_PARSE_BUFFER_SIZE];
	for (unsigned i = 0; i < NUM_TEST_URLS; ++i) {
		const char *url = TEST_URLS[i];
		ParsedUrl *parsed = parse_url(url, -1, buffer, sizeof(buffer), 
			URLPARSE_DECODE_PLUS_TO_SPACE, "http");
		fprintf(os, 
			"Url \"%s\" parsed with code %u:\n"
			"\tscheme=%.*s\n"
			"\tcanonical=\"%.*s\"\n"
			"\thost=\"%.*s\"\n"
			"\tport=%d\n"
			"\tpath=\"%.*s\"\n"
			"\tquery=\"%.*s\"\n"
			"\tpath_file_name()=\"%s\"\n"
			"\tpath_extension()=\"%s\"\n"
			"\turl_extensions={\"%.*s\", \"%.*s\", \"%.*s\", \"%.*s\"}\n"
			"\tnum_extensions=%d\n", 
			url, 
			parsed->code, parsed->scheme_length, parsed->url,
			parsed->length, parsed->url,
			parsed->host_length, parsed->url + parsed->host_start,
			parsed->port,
			parsed->path_length, parsed->url + parsed->path_start,
			parsed->query_start, parsed->url + parsed->query_start,
			path_file_name(parsed->url),
			path_extension(parsed->url),
			parsed->extension_lengths[0], 
			parsed->url + parsed->extension_starts[0],
			parsed->extension_lengths[1], 
			parsed->url + parsed->extension_starts[1],
			parsed->extension_lengths[2], 
			parsed->url + parsed->extension_starts[2],
			parsed->extension_lengths[3], 
			parsed->url + parsed->extension_starts[3],
			parsed->num_extensions
		);
	}
}

static void unit_test_path_segmenter(FILE *os)
{
	static const char * const TEST_PATHS[] = {
		"",
		"/",
		"//",
		"a_single_word",
		"/apple/orange/pear",
		"/trailing/separator/",
		"a/non/rooted/path",
		"/empty///segments"
	};
	static const unsigned NUM_TEST_PATHS = sizeof(TEST_PATHS) / sizeof(TEST_PATHS[0]);

	for (unsigned i = 0; i < NUM_TEST_PATHS; ++i) {
		const char *path = TEST_PATHS[i];
		fprintf(os, "Path segments of \"%s\":\n", path);
		for (unsigned j = 0; j < 7; ++j) {
			char segment_buffer[32];
			int segment_length;
			path_segment(j, path, -1, &segment_length, 
				segment_buffer, sizeof(segment_buffer));
			fprintf(os, "%3d: length=%d buffer=%s\n", j, 
				segment_length, segment_buffer); 
		}
	}
}

static void unit_test_cache(FILE *os)
{
	static const char * const TEST_URLS[] = {
		"http://www.google.com",
		"www.slashdot.org",
		"anandtech.com",
		"microsoft.com",
		"www.youtube.com",
		"www.twitter.com",
		"www.facebook.com",
		"www.mozilla.org",
		"http://www.gutenberg.org/files/45128/45128-0.txt",
		"http://www.gutenberg.org/files/23042/23042-0.txt",
		"http://www.gutenberg.org/ebooks/1128",
		"http://upload.wikimedia.org/wikipedia/commons/4/43/07._Camel_Profile%2C_near_Silverton%2C_NSW%2C_07.07.2007.jpg",
		"http://upload.wikimedia.org/wikipedia/commons/3/36/Eryops_-_National_Museum_of_Natural_History_-_IMG_1974.JPG",
		"http://en.wikipedia.org/wiki/File:Russet_potato_cultivar_with_sprouts.jpg"
	};
	static const unsigned NUM_TEST_URLS = sizeof(TEST_URLS) / sizeof(TEST_URLS[0]);
	static const unsigned REPEAT_COUNT = 1;
	static const unsigned TOTAL_URLS = REPEAT_COUNT * NUM_TEST_URLS;
	const unsigned POLL_INTERVAL_MSEC = 100;
	const unsigned RUN_TIME_MSEC      = 1000 * 1000;
	const unsigned EXCERPT_LENGTH     = 16;

	UrlCache *cache = new UrlCache(0x80000, 3);

	/* Create keys. */
	fprintf(os, "Requesting %u URLs.\n", TOTAL_URLS);
	UrlKey keys[TOTAL_URLS];
	
	for (unsigned i = 0; i < REPEAT_COUNT; ++i) {
		for (unsigned j = 0; j < NUM_TEST_URLS; ++j) {
			const char *url = TEST_URLS[j];
			UrlFetchPriority priority = UrlFetchPriority(rand() % NUM_PRIORITY_LEVELS);
			unsigned ttl_secs = 5000;
			unsigned index = i * NUM_TEST_URLS + j;
			keys[index] = cache->request(url, -1, priority, ttl_secs);
			fprintf(os, "Url %s requested with priority=%u, ttl=%us. "
				"Handle is %ull.\n", url, priority, ttl_secs, keys[index]);
		}
	}

	UrlFetchState fetch_states[TOTAL_URLS];
	std::fill(fetch_states, fetch_states + TOTAL_URLS, URL_FETCH_IDLE);
	unsigned poll_count;
	for (poll_count = 0; poll_count * POLL_INTERVAL_MSEC <= 
		RUN_TIME_MSEC; poll_count++) {
		float elapsed_secs = float(poll_count * POLL_INTERVAL_MSEC) * 1e-3f;
		for (unsigned i = 0; i < REPEAT_COUNT; ++i) {
			for (unsigned j = 0; j < NUM_TEST_URLS; ++j) {
				const char *url = TEST_URLS[j];
				unsigned index = i * NUM_TEST_URLS + j;
				UrlKey key = keys[index];
				UrlFetchState old_fetch_state = fetch_states[index];

				/* Query. */
				unsigned data_size;
				MimeType data_mime_type;
				UrlFetchPriority data_priority;
				UrlFetchState new_fetch_state = cache->query(key, &data_size, 
					&data_mime_type, &data_priority);
				if (new_fetch_state == old_fetch_state)
					continue;
				fprintf(os, 
					"[%3.1fs] Fetch state of %s changed from %s to %s.\n\t"
					"Query reports size=%u, mime=%s, priority=%s\n",
					elapsed_secs, url, 
					FETCH_STATE_STRINGS[old_fetch_state],
					FETCH_STATE_STRINGS[new_fetch_state],
					data_size, 
					MIME_TYPE_STRINGS[data_mime_type],
					PRIORITY_STRINGS[data_priority]);
				fetch_states[index] = new_fetch_state;
				
				/* Lock. */
				const void *data = cache->lock(key, &data_size, &data_mime_type);
				if (data != NULL) {
					const char *excerpt = (const char *)data;
					unsigned excerpt_length = std::min(EXCERPT_LENGTH, data_size);
					fprintf(os, "\tLock yielded data=%x, size=%u, mime=%s\n\t"
						"First %u bytes: [%.*s]\n", 
						data, data_size, MIME_TYPE_STRINGS[data_mime_type],
						excerpt_length, excerpt_length, excerpt);
					cache->unlock(key);
				} else {
					fprintf(os, "\tLock failed.\n");
				}
			}
		}
		cache->update();
		Sleep(POLL_INTERVAL_MSEC);
	}
	delete cache;
}

void unit_test(void)
{
	FILE *os = stdout;
	unit_test_url_parser(os);
	//unit_test_path_segmenter(os);
	//unit_test_cache(os);
}

}; // namespace urlcache


