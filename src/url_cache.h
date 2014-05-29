#pragma once

namespace urlcache {

enum MimeType {
	MIMETYPE_OCTET_STREAM, // application/octet-stream
	MIMETYPE_JSON,         // application/json
	MIMETYPE_TEXT,         // text/plain
	MIMETYPE_HTML,         // text/html
	MIMETYPE_STACKER,      // text/stacker
	MIMETYPE_TEMPLATE,     // text/template
	MIMETYPE_PNG,          // image/png
	MIMETYPE_JPEG,         // image/jpeg
	MIMETYPE_GIF,          // image/gif
	MIMETYPE_NONE,
	NUM_SUPPORTED_MIME_TYPES = MIMETYPE_NONE
};

enum UrlScheme {
	SCHEME_HTTP,
	SCHEME_HTTPS,
	SCHEME_STACKER,
	SCHEME_FILE,
	SCHEME_NONE,
	NUM_SUPPORTED_SCHEMES = SCHEME_NONE,
};

enum UrlFetchPriority {
	URLP_UNSET = -1,
	URLP_NO_FETCH,
	URLP_NORMAL,
	URLP_ELEVATED,
	URLP_URGENT,
	NUM_PRIORITY_LEVELS
};

enum UrlFetchState {
	URL_FETCH_IDLE,
	URL_FETCH_QUEUED,
	URL_FETCH_FAILED,
	URL_FETCH_IN_PROGRESS,
	URL_FETCH_SUCCESSFUL,
	URL_FETCH_DISK,
	NUM_FETCH_STATES
};

enum UrlNotification {
	URL_NOTIFY_FETCH,
	URL_NOTIFY_EVICT,
	URL_QUERY_EVICT
};

enum UrlFlag {
	URL_FLAG_DISCARD           = 1 << 0, // Evict data when the handle is unlocked.
	URL_FLAG_REUSE_SINK_HANDLE = 1 << 1, // If there's an existing handle with the same notification sink, return it instead of creating a new one.
	URL_FLAG_REUSE_DATA_HANDLE = 1 << 2, // If there's an existing handle with the same user data, return it instead of creating a new one.
	URL_FLAG_KEEP_URL          = 1 << 3, // Keep the parsed URL string in an entry after fetch completion.
	URL_FLAG_PREVENT_EVICT     = 1 << 4  // Set on a a handle, the entry will not be evicted as long as the handle exists.
};

enum UrlParseCode {
	URLPARSE_OK,
	URLPARSE_TOO_LONG,
	URLPARSE_MALFORMED,
	URLPARSE_INVALID_HOST,
	URLPARSE_INVALID_PORT
};

struct ParsedUrl {
	UrlParseCode code;
	UrlScheme scheme;
	char mime_types[4];
	unsigned short length;
	unsigned short host_start;
	unsigned short host_length;
	unsigned short port;
	unsigned short path_start;
	unsigned short path_length;
	unsigned short query_start;
	unsigned short query_length;
	char url[1];
};

typedef unsigned long long UrlKey;
typedef void *UrlHandle;

typedef unsigned (*NotifyCallback)(UrlHandle handle, UrlNotification type, 
	UrlKey key, void *sink_data, void *handle_data, UrlFetchState fetch_state);
typedef bool (*LocalFetchCallback)(void *data, const ParsedUrl *url, 
	void **buffer, unsigned *size, MimeType *mime_type);

const unsigned DEFAULT_MEMORY_LIMIT = 0x800000;
const unsigned DEFAULT_FETCH_SLOTS  = 5;
const unsigned DEFAULT_TTL_SECS     = 5 * 60;
const unsigned PREVENT_EVICT        = unsigned(-1);
const int INVALID_NOTIFY_SINK_ID    = -1;
const UrlHandle INVALID_URL_HANDLE  = 0;
const UrlKey INVALID_URL_KEY        = 0ULL;

/* The maximum number of characters we could extend a URL by in the process
 * of normalizing it. We might add a scheme, the separater "://", three extra
 * nulls to separate the parts, and a '/' in lieu of an omitted path. */
const unsigned URL_MAX_EXTRA = 3 + 3 + 1 + 8;
const unsigned MAX_URL_LENGTH = 2047;

/* A reasonable size for stack buffers passed to parse_url(). */
const unsigned URL_PARSE_BUFFER_SIZE = sizeof(ParsedUrl) + 
	MAX_URL_LENGTH + URL_MAX_EXTRA;

class UrlCache 
{
public:

	UrlCache(unsigned memory_limit = DEFAULT_MEMORY_LIMIT, 
		unsigned num_fetch_slots = DEFAULT_FETCH_SLOTS);
	~UrlCache();

	/*
	 * Cache API
	 */
	void update(void);
	void lock_cache(void);
	void unlock_cache(void);
	int add_notify_sink(NotifyCallback callback, void *user_data);
	void remove_notify_sink(int sink_id);
	void set_notify_sink_data(int sink_id, NotifyCallback callback, void *user_data);
	void set_local_fetch_callback(LocalFetchCallback callback, void *data = 0);
	bool insert(const char *url, int url_length,
		const void *data, unsigned size, 
		MimeType mime_type = MIMETYPE_NONE, 
		unsigned ttl_secs = DEFAULT_TTL_SECS, 
		bool copy = true);
	bool is_local_url(const char *url, int length = -1);
	bool is_local_url(const ParsedUrl *url);
	
	/*
	 * Key API
	 */
	UrlKey key(const char *url, int length = -1) const;
	UrlKey key(UrlHandle handle) const;
	UrlFetchState query(UrlKey key, unsigned *out_size = 0, 
		MimeType *out_mime_type = 0, UrlFetchPriority *out_priority = 0);
	UrlKey request(const char *url, int length = -1,
		UrlFetchPriority priority = URLP_NORMAL, 
		unsigned ttl_secs = DEFAULT_TTL_SECS,
		unsigned flags = 0);
	void request(UrlKey key, 
		UrlFetchPriority priority = URLP_NORMAL, 
		unsigned ttl_secs = DEFAULT_TTL_SECS,
		unsigned flags = 0);
	const void *lock(UrlKey key, unsigned *out_size = 0, 
		MimeType *out_mime_type = 0);
	void unlock(UrlKey key);

	/*
	 * Handle API
	 */
	UrlHandle create_handle(const char *url, int length = -1,
		UrlFetchPriority priority = URLP_NORMAL,
		unsigned ttl_secs = DEFAULT_TTL_SECS, void *user_data = 0, 
		int notify_sink = INVALID_NOTIFY_SINK_ID, unsigned flags = 0);
	UrlHandle create_handle(UrlKey key, 
		UrlFetchPriority priority = URLP_NORMAL,
		unsigned ttl_secs = DEFAULT_TTL_SECS, void *user_data = 0, 
		int notify_sink = INVALID_NOTIFY_SINK_ID, unsigned flags = 0);
	void destroy_handle(UrlHandle handle);
	const void *lock(UrlHandle handle, unsigned *out_size = 0, 
		MimeType *out_mime_type = 0);
	void unlock(UrlHandle handle);
	UrlFetchState query(UrlHandle handle, unsigned *out_size = 0,
		MimeType *out_mime_type = 0, UrlFetchPriority *out_priority = 0,
		void **out_user_data = 0);
	void request(UrlHandle handle, UrlFetchPriority priority = URLP_NORMAL);
	void *user_data(UrlHandle handle);
	void *set_user_data(UrlHandle handle, void *user_data);
	void set_notify(UrlHandle handle, int sink_id);
	ParsedUrl *url(UrlHandle handle, void *buffer = 0, 
		unsigned buffer_size = 0);
	
private:

	struct Cache *cache;
};

/*
 * URL Handling
 */

enum UrlParseFlag {
	/* If a caller-supplied buffer is too small, return the result in a new
	 * heap buffer for which the caller will assume responsibility. */
	URLPARSE_HEAP                 = 1 << 0, 
	/* Separate the scheme, host, path and query with null terminators. */
	URLPARSE_TERMINATE_PARTS      = 1 << 1, 
	/* Encode space characters as '+' instead of %20. */
	URLPARSE_ENCODE_SPACE_AS_PLUS = 1 << 2, 
	/* Decode '+' to space. */
	URLPARSE_DECODE_PLUS_TO_SPACE = 1 << 3
};

ParsedUrl *parse_url(const char *url, int length = -1, void *buffer = 0, 
	unsigned buffer_size = 0, unsigned flags = URLPARSE_DECODE_PLUS_TO_SPACE, 
	UrlScheme default_scheme = SCHEME_HTTP);
ParsedUrl *copy_parsed_url(const ParsedUrl *url, void *buffer = 0,
	unsigned buffer_size = 0);
unsigned url_decode(char *s, int length = -1, 
	unsigned flags = URLPARSE_DECODE_PLUS_TO_SPACE);
char *url_encode(const char *s, int length = -1, char *buffer = 0,
	unsigned buffer_size = 0, unsigned *out_length = 0, unsigned flags = 0);

/*
 * Path Utilities
 */
const char *path_file_name(const char *path);
const char *path_extension(const char *path);
const char *path_segment(unsigned n, const char *s, int slen = -1, 
	int *out_length = 0, char *buffer = 0, unsigned buffer_size = 0);

/*
 * Tests
 */
void unit_test(void);

}; // namespace urlcache

using urlcache::UrlKey;
using urlcache::UrlHandle;
using urlcache::UrlCache;
