#pragma once

#include "hf_common.hpp"
#include "hf_secrets.hpp"

#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/http/http_util.hpp"

namespace duckdb {

class DatabaseInstance;
struct FileOpenerInfo;

//! A paginated listing page: the raw body plus the `Link: <...>; rel="next"` continuation, if any.
struct HFListPage {
	string body;
	string next_url;
	bool has_next = false;
	//! Set when the listing asked for a path that does not exist (see `tolerate_not_found`)
	bool not_found = false;
};

//! Thin wrapper over duckdb's HTTPUtil that adds Hugging Face auth, error translation and pagination.
//! All methods throw duckdb exceptions; nothing here returns a raw status code to the caller unless stated.
class HFClient {
public:
	HFClient(DatabaseInstance &db, optional_ptr<FileOpener> opener, HFConnection connection_p, const string &path);

	//! GET a whole resource into memory. `throw_on_error` throws a descriptive exception on >=400.
	string Get(const string &url, const HTTPHeaders &headers = HTTPHeaders(), bool throw_on_error = true);
	//! GET a whole resource into memory and hand back the status without deciding what it means. `Get`'s
	//! `throw_on_error = false` cannot answer the question a Xet dedup query asks: it returns an empty body
	//! both when the resource is absent (404, "nobody has stored this chunk", which is a reason to upload)
	//! and when the server refuses to answer (401, "this token may not ask", which is a reason to stop).
	//! A transport failure still throws - there is no status to report in that case.
	int GetStatus(const string &url, string &body, const HTTPHeaders &headers = HTTPHeaders(),
	              bool with_auth = true);
	//! GET a byte range `[offset, offset + len)` appending to `out`. Returns false on a non-2xx response.
	bool GetRange(const string &url, idx_t offset, idx_t len, string &out, const HTTPHeaders &headers = HTTPHeaders());
	//! GET a byte range straight into a caller-provided buffer of exactly `len` bytes. Returns false on a
	//! non-2xx response, and tells how much actually arrived in `bytes_read`.
	bool GetRangeInto(const string &url, idx_t offset, idx_t len, data_ptr_t buffer, idx_t &bytes_read,
	                  const HTTPHeaders &headers = HTTPHeaders());
	//! GET streaming chunks of a byte range through a callback. Throws on failure.
	void GetRangeStreaming(const string &url, idx_t offset, idx_t len,
	                       const std::function<void(const_data_ptr_t, idx_t)> &callback,
	                       const HTTPHeaders &headers = HTTPHeaders());
	//! HEAD a resource; returns the response (never throws on 404, callers inspect the status)
	unique_ptr<HTTPResponse> Head(const string &url, const HTTPHeaders &headers = HTTPHeaders());
	//! POST a body. Returns the raw response (no throw). `with_auth = false` is for endpoints that
	//! authenticate some other way - the Xet CAS carries its own token, and sending the Hub one too
	//! gets the request refused.
	unique_ptr<HTTPResponse> Post(const string &url, const string &body, const HTTPHeaders &headers = HTTPHeaders(),
	                              bool with_auth = true);
	//! PUT a buffer. Returns the raw response (no throw). Presigned urls (git-LFS) must be PUT with
	//! `with_auth = false`: the signature in the url authenticates the request, an extra Authorization header
	//! makes S3 reject it.
	unique_ptr<HTTPResponse> Put(const string &url, const_data_ptr_t buffer, idx_t len,
	                             const HTTPHeaders &headers = HTTPHeaders(), bool with_auth = true);
	//! DELETE. Returns the raw response (no throw).
	unique_ptr<HTTPResponse> Delete(const string &url, const HTTPHeaders &headers = HTTPHeaders());

	//! List a "directory" of a repository (single page). `recursive` is honoured for every repo type.
	//! `tolerate_not_found` turns a 404 into `page.not_found` instead of an error: a folder entry in a listing
	//! can be newer than the revision being listed, and the walk of a directory must survive that.
	HFListPage ListPage(const HFUrl &url, idx_t limit, bool recursive, const string &next_url,
	                   bool tolerate_not_found = false);

	const HFConnection &Connection() const {
		return connection;
	}
	DatabaseInstance &Db() const {
		return db;
	}
	HTTPParams &Params() {
		return *params;
	}
	HTTPUtil &Util() {
		return *http_util;
	}

	//! Build the headers used for file/asset requests (auth only)
	HTTPHeaders AuthHeaders() const;
	//! Add the Authorization header when the connection has a token and the caller did not set one
	HTTPHeaders WithAuth(const HTTPHeaders &headers) const;
	//! Translate a failed response into a duckdb exception mentioning the HF error, code and request id
	[[noreturn]] void ThrowHTTPError(const HTTPResponse &response, const string &what, const string &url) const;
	//! True when the status should be retried
	static bool ShouldRetry(const HTTPResponse &response);
	//! Decide whether `attempt` (1-based) should be followed by a retry, and for how long to wait
	static bool ShouldRetryResponse(const HTTPResponse &response, idx_t attempt, idx_t &delay_ms);
	//! Run `func` (which must build a fresh request, and reset any buffer it fills, on every call) until the
	//! response is no longer retryable - the Hub throttles aggressively, so this is what keeps big
	//! listings/scans from falling over on a single 429.
	template <class FUNC>
	unique_ptr<HTTPResponse> RunWithRetry(FUNC &&func) {
		unique_ptr<HTTPResponse> response;
		for (idx_t attempt = 1;; attempt++) {
			response = func();
			idx_t delay_ms = 0;
			if (response && ShouldRetryResponse(*response, attempt, delay_ms)) {
				SleepMs(delay_ms);
				continue;
			}
			return response;
		}
	}
	static void SleepMs(idx_t ms);

	//! Send through the transport session (leased, pooled connection) when one is available
	unique_ptr<HTTPResponse> SendRequest(BaseRequest &request);

private:
	DatabaseInstance &db;
	optional_ptr<FileOpener> opener;
	HFConnection connection;
	optional_ptr<HTTPUtil> http_util;
	//! A transport session leases connections from duckdb's pool. Going through it matters: without a session
	//! every range request of a scan handshakes a fresh https connection (~200ms), which makes reading a big
	//! parquet file 10x slower. `params` points into the session, or into `owned_params` as a fallback.
	unique_ptr<HTTPTransportManager::Session> session;
	unique_ptr<HTTPParams> owned_params;
	HTTPParams *params = nullptr;
};

//! Helpers for the JSON returned by the Hub API. DuckDB's json extension is not a hard dependency, so these
//! use a tiny tolerant scanner (the same approach httpfs uses for its listings).
struct HFJson {
	//! First "key":"value" string occurrence, `found` set accordingly
	//! The value of `key` exactly as it appears - quotes included for a string, bare for a number,
//! `true`/`false`, or `null` - to feed AsString/AsInt/AsBool. GetString above only matches quoted
//! values, which is all the Hub's git routes return; the Xet routes answer with booleans and unix
//! timestamps, where reading a missing key as "absent" and defaulting it is how a token silently
//! stops being refreshed.
	static string GetRaw(const string &json, const string &key, bool &found);
	static string GetString(const string &json, const string &key, idx_t from, idx_t &end, bool &found);
	static string GetString(const string &json, const string &key, bool &found);
	//! Parse a `Link` header into a continuation url
	static string NextLinkUrl(const string &link_header, bool &found);
	//! Iterate the top-level members of a JSON object (`obj` must start at its opening brace)
	static void ForEachMember(const string &obj, const std::function<void(const string &, const string &)> &callback);
	//! `"a"` -> `a`, `123` -> `123`
	static string AsString(const string &raw);
	static int64_t AsInt(const string &raw);
	static bool AsBool(const string &raw);
	//! Find the array member named `key` and return its raw `[...]` content ("" when missing)
	static string FindArray(const string &json, const string &key);
	//! Call `callback` with the raw text of every object in a json array (`[...]` or `{...}` of objects)
	static void ForEachObject(const string &json, const std::function<void(const string &)> &callback);
};

} // namespace duckdb
