#include "hf_http.hpp"

#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/common/file_opener.hpp"

#include <cctype>
#include <chrono>
#include <thread>

#include <cstdint>
#include <cstring>

namespace duckdb {

string HFJson::GetString(const string &json, const string &key, idx_t from, idx_t &end, bool &found) {
	found = false;
	string needle = "\"" + key + "\":\"";
	auto pos = json.find(needle, from);
	if (pos == string::npos) {
		return "";
	}
	pos += needle.size();
	string result;
	while (pos < json.size()) {
		auto c = json[pos];
		if (c == '\\' && pos + 1 < json.size()) {
			auto next = json[pos + 1];
			if (next == '"' || next == '\\') {
				result += next;
				pos += 2;
				continue;
			}
			if (next == 'n') {
				result += '\n';
				pos += 2;
				continue;
			}
			if (next == 't') {
				result += '\t';
				pos += 2;
				continue;
			}
			if (next == 'u') {
				// only handle BMP characters, which is all the Hub API returns
				auto hex = json.substr(pos + 2, 4);
				if (hex.size() == 4) {
					auto code = static_cast<int>(std::stoul(hex, nullptr, 16));
					if (code < 0x80) {
						result += static_cast<char>(code);
					} else if (code < 0x800) {
						result += static_cast<char>(0xC0 | (code >> 6));
						result += static_cast<char>(0x80 | (code & 0x3F));
					} else {
						result += static_cast<char>(0xE0 | (code >> 12));
						result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
						result += static_cast<char>(0x80 | (code & 0x3F));
					}
					pos += 6;
					continue;
				}
			}
			result += c;
			pos++;
			continue;
		}
		if (c == '"') {
			end = pos;
			found = true;
			return result;
		}
		result += c;
		pos++;
	}
	end = pos;
	return result;
}

string HFJson::GetString(const string &json, const string &key, bool &found) {
	idx_t end = 0;
	return GetString(json, key, 0, end, found);
}

namespace {

//! Skip whitespace
void SkipJSONWS(const string &s, idx_t &i) {
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
		i++;
	}
}

//! Read a JSON string literal starting at `i` (which must point at the opening quote), unescaped
string ReadJSONString(const string &s, idx_t &i) {
	D_ASSERT(i < s.size() && s[i] == '"');
	i++;
	string result;
	while (i < s.size()) {
		auto c = s[i];
		if (c == '\\' && i + 1 < s.size()) {
			auto next = s[i + 1];
			switch (next) {
			case 'n':
				result += '\n';
				break;
			case 't':
				result += '\t';
				break;
			case 'r':
				result += '\r';
				break;
			case 'b':
				result += '\b';
				break;
			case 'f':
				result += '\f';
				break;
			case 'u': {
				auto hex = s.substr(i + 2, 4);
				if (hex.size() == 4) {
					auto code = static_cast<int>(std::stoul(hex, nullptr, 16));
					if (code < 0x80) {
						result += static_cast<char>(code);
					} else if (code < 0x800) {
						result += static_cast<char>(0xC0 | (code >> 6));
						result += static_cast<char>(0x80 | (code & 0x3F));
					} else {
						result += static_cast<char>(0xE0 | (code >> 12));
						result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
						result += static_cast<char>(0x80 | (code & 0x3F));
					}
					i += 4;
				} else {
					result += next;
				}
				break;
			}
			default:
				result += next;
				break;
			}
			i += 2;
			continue;
		}
		if (c == '"') {
			i++;
			return result;
		}
		result += c;
		i++;
	}
	return result;
}

//! Index just after the JSON value starting at `i` (strings, objects, arrays and bare literals)
idx_t SkipJSONValue(const string &s, idx_t i) {
	SkipJSONWS(s, i);
	if (i >= s.size()) {
		return i;
	}
	if (s[i] == '"') {
		ReadJSONString(s, i);
		return i;
	}
	idx_t depth = 0;
	bool in_string = false;
	bool escaped = false;
	for (; i < s.size(); i++) {
		auto c = s[i];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (c == '\\') {
				escaped = true;
			} else if (c == '"') {
				in_string = false;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
		} else if (c == '{' || c == '[') {
			depth++;
		} else if (c == '}' || c == ']') {
			depth--;
			if (depth == 0) {
				return i + 1;
			}
		} else if (depth == 0 && (c == ',' || c == ':')) {
			return i;
		}
	}
	return i;
}

} // namespace

void HFJson::ForEachMember(const string &obj, const std::function<void(const string &, const string &)> &callback) {
	// idx_t is uint64_t (typedefs.hpp), and on Apple's libc++ that is `unsigned long long` while
	// string::size_type is `unsigned long`: same width, different type, so a value coming out of
	// string::find() cannot bind to the `idx_t &` these helpers take. On Linux both spellings are
	// `unsigned long`, so this only fails on the macOS build - which is why the declarations below name
	// idx_t explicitly instead of taking the type from `auto`.
	idx_t i = obj.find('{');
	if (i == string::npos) {
		return;
	}
	i++;
	while (i < obj.size()) {
		SkipJSONWS(obj, i);
		if (i >= obj.size() || obj[i] == '}') {
			return;
		}
		if (obj[i] != '"') {
			return;
		}
		auto key = ReadJSONString(obj, i);
		SkipJSONWS(obj, i);
		if (i >= obj.size() || obj[i] != ':') {
			return;
		}
		i++;
		SkipJSONWS(obj, i);
		auto value_start = i;
		auto value_end = SkipJSONValue(obj, i);
		if (value_end <= value_start) {
			return;
		}
		callback(key, obj.substr(value_start, value_end - value_start));
		auto next = obj.find_first_of(",}", value_end);
		if (next == string::npos || obj[next] == '}') {
			return;
		}
		i = next + 1;
	}
}

string HFJson::GetRaw(const string &json, const string &key, bool &found) {
	found = false;
	string needle = "\"" + key + "\"";
	idx_t pos = json.find(needle);
	if (pos == string::npos) {
		return "";
	}
	pos += needle.size();
	SkipJSONWS(json, pos);
	if (json[pos] != ':') {
		// the key appeared as a value or a substring rather than as a member - keep looking
		return pos + 1 < json.size() ? GetRaw(json.substr(pos + 1), key, found) : "";
	}
	pos++;
	SkipJSONWS(json, pos);
	if (pos >= json.size()) {
		return "";
	}
	if (json[pos] == '"') {
		// the quoted form, quotes kept: HFJson::AsString takes it apart the same way everywhere else
		auto start = pos;
		pos++;
		while (pos < json.size()) {
			if (json[pos] == '\\') {
				pos += 2;
				continue;
			}
			if (json[pos] == '"') {
				pos++;
				break;
			}
			pos++;
		}
		found = true;
		return json.substr(start, pos - start);
	}
	if (json[pos] == '{' || json[pos] == '[') {
		// a nested value: hand back the whole object or array, whose own keys are then read the same way
		auto start = pos;
		idx_t depth = 0;
		bool in_string = false;
		for (; pos < json.size(); pos++) {
			auto c = json[pos];
			if (in_string) {
				if (c == '\\') {
					pos++;
				} else if (c == '"') {
					in_string = false;
				}
			} else if (c == '"') {
				in_string = true;
			} else if (c == '{' || c == '[') {
				depth++;
			} else if (c == '}' || c == ']') {
				if (--depth == 0) {
					pos++;
					break;
				}
			}
		}
		found = depth == 0;
		return json.substr(start, pos - start);
	}
	// a number, a boolean or null: the scalar runs to the next separator
	auto start = pos;
	while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']' && json[pos] != '\n') {
		pos++;
	}
	auto end = pos;
	while (end > start && isspace(static_cast<unsigned char>(json[end - 1]))) {
		end--;
	}
	found = end > start;
	return json.substr(start, end - start);
}

string HFJson::AsString(const string &raw) {
	idx_t i = 0;
	SkipJSONWS(raw, i);
	if (i < raw.size() && raw[i] == '"') {
		return ReadJSONString(raw, i);
	}
	return raw;
}

int64_t HFJson::AsInt(const string &raw) {
	try {
		return std::stoll(raw);
	} catch (std::exception &) {
		return 0;
	}
}

bool HFJson::AsBool(const string &raw) {
	return raw == "true";
}

string HFJson::FindArray(const string &json, const string &key) {
	string needle = "\"" + key + "\":[";
	auto pos = json.find(needle);
	if (pos == string::npos) {
		return "";
	}
	auto start = pos + needle.size() - 1;
	auto end = SkipJSONValue(json, start);
	return json.substr(start, end - start);
}

void HFJson::ForEachObject(const string &json, const std::function<void(const string &)> &callback) {
	idx_t i = 0;
	while (i < json.size()) {
		auto open = json.find('{', i);
		if (open == string::npos) {
			return;
		}
		auto end = SkipJSONValue(json, open);
		if (end <= open) {
			return;
		}
		callback(json.substr(open, end - open));
		i = end;
	}
}

string HFJson::NextLinkUrl(const string &link_header, bool &found) {
	found = false;
	for (auto &chunk : StringUtil::Split(link_header, ',')) {
		StringUtil::Trim(chunk);
		auto parts = StringUtil::Split(chunk, ';');
		if (parts.size() != 2) {
			continue;
		}
		auto rel = parts[1];
		StringUtil::Trim(rel);
		if (rel != "rel=\"next\"" && rel != "rel=next") {
			continue;
		}
		auto url = parts[0];
		StringUtil::Trim(url);
		if (url.size() >= 2 && url.front() == '<' && url.back() == '>') {
			found = true;
			return url.substr(1, url.size() - 2);
		}
	}
	return "";
}

namespace {

//! Transports that cannot talk https to the Hub: the empty placeholder, and the built-in httplib
//! client, which ships without TLS in the official DuckDB builds (an https request through it fails
//! with "'https' scheme is not supported.", raised inside third_party/httplib).
bool NeedsHttpfs(const string &transport_name) {
	return transport_name.empty() || transport_name == "none" || transport_name == "Built-In";
}

//! Turn the low-level "there is no https-capable HTTP client" failures into something actionable.
void RethrowWithTransportHint(const std::exception &ex, const string &transport_name, const string &url) {
	string message = ex.what();
	if (message.find("scheme is not supported") != string::npos || message.find("no HTTP client for") != string::npos) {
		throw IOException("reading Hugging Face assets requires an HTTP transport that supports https://, and the "
		                  "registered one ('%s') does not - provide it with `INSTALL httpfs; LOAD httpfs;`. This "
		                  "extension normally loads httpfs by itself, which settings like "
		                  "autoinstall_known_extensions or autoload_known_extensions can block. Failed request: %s (%s)",
		                  transport_name, url, message);
	}
	std::rethrow_exception(std::current_exception());
}

} // namespace

HFClient::HFClient(DatabaseInstance &db_p, optional_ptr<FileOpener> opener_p, HFConnection connection_p,
                   const string &path)
    : db(db_p), opener(opener_p), connection(std::move(connection_p)) {
	auto &util = HTTPUtil::Get(db);
	// httpfs is the transport this extension is designed to run on. The built-in client is compiled
	// without TLS in the official DuckDB distributions, so "Built-In" cannot serve the https:// Hub
	// endpoints either - it has to be upgraded too. TryAutoLoadExtension is a silent no-op when the
	// extension is unavailable (autoinstall switched off), in which case requests fail with the
	// actionable error from RethrowWithTransportHint below.
	if (NeedsHttpfs(util.GetName())) {
		ExtensionHelper::TryAutoLoadExtension(db, "httpfs");
	}
	http_util = &HTTPUtil::Get(db);
	FileOpenerInfo info;
	info.file_path = path;
	// Lease connections through a transport session: one https connection is then reused for every range
	// request of a scan instead of being renegotiated each time.
	try {
		session = make_uniq<HTTPTransportManager::Session>(
		    DBConfig::GetConfig(db).GetHTTPTransportManager().CreateSession(opener, &info));
		params = &session->Parameters();
	} catch (std::exception &) {
		session.reset();
		owned_params = http_util->InitializeParameters(opener, &info);
		params = owned_params.get();
	}
}

unique_ptr<HTTPResponse> HFClient::SendRequest(BaseRequest &request) {
	try {
		if (session) {
			return session->Request(request);
		}
		// No transport session (older/unavailable transport manager): drive HTTPUtil directly. The client
		// slot is deliberately local, so a request to a different host (the API host vs a presigned LFS
		// host) can never be served by a client that was opened for another one.
		unique_ptr<HTTPClient> client;
		auto response = http_util->SendRequest(request, client);
		http_util->CloseClient(std::move(client));
		return response;
	} catch (std::exception &ex) {
		RethrowWithTransportHint(ex, http_util->GetName(), request.url);
		return nullptr; // RethrowWithTransportHint always throws, this only keeps the compiler certain of it
	}
}

HTTPHeaders HFClient::WithAuth(const HTTPHeaders &headers) const {
	if (!connection.HasToken() || headers.HasHeader("Authorization")) {
		return headers;
	}
	HTTPHeaders result = headers;
	result.Insert("Authorization", "Bearer " + connection.token);
	return result;
}

HTTPHeaders HFClient::AuthHeaders() const {
	HTTPHeaders headers;
	if (connection.HasToken()) {
		headers.Insert("Authorization", "Bearer " + connection.token);
	}
	return headers;
}

bool HFClient::ShouldRetry(const HTTPResponse &response) {
	if (response.HasRequestError()) {
		return true;
	}
	auto code = static_cast<int>(response.status);
	return code == 408 || code == 429 || code >= 500;
}

bool HFClient::ShouldRetryResponse(const HTTPResponse &response, idx_t attempt, idx_t &delay_ms) {
	if (attempt >= HF_MAX_HTTP_ATTEMPTS || !ShouldRetry(response)) {
		return false;
	}
	auto seconds = 1 << (attempt - 1); // 1s, 2s, 4s, ...
	if (response.HasHeader("Retry-After")) {
		try {
			auto hinted = std::stoll(response.GetHeaderValue("Retry-After"));
			if (hinted >= 0) {
				seconds = MaxValue<int64_t>(seconds, MinValue<int64_t>(hinted, 10));
			}
		} catch (std::exception &) {
			// an HTTP-date Retry-After is rare on the Hub, exponential backoff is good enough
		}
	}
	delay_ms = NumericCast<idx_t>(seconds) * 1000;
	return true;
}

void HFClient::SleepMs(idx_t ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static string ExtractJSONError(const string &body) {
	bool found = false;
	auto error = HFJson::GetString(body, "error", found);
	if (found && !error.empty()) {
		return error;
	}
	auto trimmed = body;
	StringUtil::Trim(trimmed);
	if (trimmed.size() > 300) {
		trimmed = trimmed.substr(0, 300) + "...";
	}
	return trimmed;
}

void HFClient::ThrowHTTPError(const HTTPResponse &response, const string &what, const string &url) const {
	auto status = static_cast<int>(response.status);
	string message;
	if (response.HasRequestError()) {
		message = response.GetRequestError();
	} else {
		message = ExtractJSONError(response.body);
	}
	string suffix;
	if (response.HasHeader("x-error-code")) {
		suffix += " [x-error-code: " + response.GetHeaderValue("x-error-code") + "]";
	}
	if (response.HasHeader("x-request-id")) {
		suffix += " [x-request-id: " + response.GetHeaderValue("x-request-id") + "]";
	}
	if (!connection.token.empty() && message.find(connection.token) != string::npos) {
		message = HFUtil::Redact(message, connection.token);
	}
	string auth_hint;
	if (status == 401 || status == 403) {
		auth_hint = "\nTry `CREATE SECRET (TYPE HUGGINGFACE, PROVIDER credential_chain)` or `SET hf_token = ...`";
	}
	throw IOException("HTTP %d on %s '%s': %s%s%s", status, what, url, message, suffix, auth_hint);
}

string HFClient::Get(const string &url, const HTTPHeaders &headers, bool throw_on_error) {
	string body;
	unique_ptr<HTTPResponse> result;
	HTTPHeaders final_headers = WithAuth(headers);

	auto response = RunWithRetry([&]() {
		body.clear();
		GetRequestInfo get(url, final_headers, *params,
		                   [&](const HTTPResponse &response) {
			                   result = make_uniq<HTTPResponse>(response.status);
			                   result->headers = response.headers;
			                   result->body = response.body;
			                   result->url = response.url;
			                   result->success = response.success;
			                   result->request_error = response.request_error;
			                   result->reason = response.reason;
			                   return true;
		                   },
		                   [&](const_data_ptr_t data, idx_t data_length) {
			                   body.append(const_char_ptr_cast(data), data_length);
			                   return true;
		                   });
		get.try_request = true;
		return SendRequest(get);
	});
	if (response->HasRequestError()) {
		if (!throw_on_error) {
			return "";
		}
		ThrowHTTPError(*response, "GET", url);
	}
	if (static_cast<int>(response->status) >= 400) {
		if (!throw_on_error) {
			return "";
		}
		ThrowHTTPError(*response, "GET", url);
	}
	return body;
}

bool HFClient::GetRange(const string &url, idx_t offset, idx_t len, string &out, const HTTPHeaders &headers) {
	HTTPHeaders final_headers = WithAuth(headers);
	final_headers.Insert("Range", StringUtil::Format("bytes=%llu-%llu", offset, offset + len - 1));
	bool ok = false;
	auto response = RunWithRetry([&]() {
		out.clear();
		ok = false;
		GetRequestInfo get(url, final_headers, *params,
		                   [&](const HTTPResponse &response) {
			                   ok = static_cast<int>(response.status) == 206 || static_cast<int>(response.status) == 200;
			                   return true;
		                   },
		                   [&](const_data_ptr_t data, idx_t data_length) {
			                   out.append(const_char_ptr_cast(data), data_length);
			                   return true;
		                   });
		get.try_request = true;
		return SendRequest(get);
	});
	return ok && !response->HasRequestError();
}

bool HFClient::GetRangeInto(const string &url, idx_t offset, idx_t len, data_ptr_t buffer, idx_t &bytes_read,
                           const HTTPHeaders &headers) {
	HTTPHeaders final_headers = WithAuth(headers);
	final_headers.Insert("Range", StringUtil::Format("bytes=%llu-%llu", offset, offset + len - 1));
	bool ok = false;
	idx_t written = 0;
	auto response = RunWithRetry([&]() {
		ok = false;
		written = 0;
		GetRequestInfo get(url, final_headers, *params,
		                   [&](const HTTPResponse &response) {
			                   ok = static_cast<int>(response.status) == 206 || static_cast<int>(response.status) == 200;
			                   return true;
		                   },
		                   [&](const_data_ptr_t data, idx_t data_length) {
			                   if (written + data_length > len) {
				                   // the server sent more than the requested range, the buffer would overflow
				                   return false;
			                   }
			                   memcpy(buffer + written, data, data_length);
			                   written += data_length;
			                   return true;
		                   });
		get.try_request = true;
		return SendRequest(get);
	});
	bytes_read = written;
	return ok && !response->HasRequestError();
}

void HFClient::GetRangeStreaming(const string &url, idx_t offset, idx_t len,
                                 const std::function<void(const_data_ptr_t, idx_t)> &callback,
                                 const HTTPHeaders &headers) {
	unique_ptr<HTTPResponse> failure;
	HTTPHeaders final_headers = WithAuth(headers);
	final_headers.Insert("Range", StringUtil::Format("bytes=%llu-%llu", offset, offset + len - 1));

	GetRequestInfo get(url, final_headers, *params,
	                   [&](const HTTPResponse &response) {
		                   auto status = static_cast<int>(response.status);
		                   if (status != 206 && status != 200) {
			                   failure = make_uniq<HTTPResponse>(response.status);
			                   failure->headers = response.headers;
			                   failure->body = response.body;
			                   failure->url = response.url;
			                   failure->request_error = response.request_error;
			                   return false;
		                   }
		                   return true;
	                   },
	                   [&](const_data_ptr_t data, idx_t data_length) {
		                   callback(data, data_length);
		                   return true;
	                   });
	get.try_request = true;
	auto response = SendRequest(get);
	if (response->HasRequestError()) {
		ThrowHTTPError(*response, "GET (range)", url);
	}
	if (failure) {
		ThrowHTTPError(*failure, "GET (range)", url);
	}
}

unique_ptr<HTTPResponse> HFClient::Head(const string &url, const HTTPHeaders &headers) {
	HTTPHeaders final_headers = WithAuth(headers);
	return RunWithRetry([&]() {
		HeadRequestInfo head(url, final_headers, *params);
		head.try_request = true;
		return SendRequest(head);
	});
}

unique_ptr<HTTPResponse> HFClient::Post(const string &url, const string &body, const HTTPHeaders &headers,
                                        bool with_auth) {
	HTTPHeaders final_headers = with_auth ? WithAuth(headers) : headers;
	PostRequestInfo post(url, final_headers, *params, const_data_ptr_cast(body.data()), body.size());
	post.try_request = true;
	return SendRequest(post);
}

unique_ptr<HTTPResponse> HFClient::Put(const string &url, const_data_ptr_t buffer, idx_t len,
                                       const HTTPHeaders &headers, bool with_auth) {
	HTTPHeaders final_headers = with_auth ? WithAuth(headers) : headers;
	static string EMPTY_CONTENT_TYPE;
	PutRequestInfo put(url, final_headers, *params, buffer, len, EMPTY_CONTENT_TYPE);
	put.try_request = true;
	return SendRequest(put);
}

unique_ptr<HTTPResponse> HFClient::Delete(const string &url, const HTTPHeaders &headers) {
	HTTPHeaders final_headers = WithAuth(headers);
	DeleteRequestInfo del(url, final_headers, *params);
	del.try_request = true;
	return SendRequest(del);
}

HFListPage HFClient::ListPage(const HFUrl &url, idx_t limit, bool recursive, const string &next_url,
                              bool tolerate_not_found) {
	HFListPage page;
	string request_url = next_url.empty() ? url.TreeUrl(limit, recursive) : next_url;
	string link_header;
	unique_ptr<HTTPResponse> response;

	GetRequestInfo get(request_url, WithAuth(HTTPHeaders()), *params,
	                   [&](const HTTPResponse &res) {
		                   response = make_uniq<HTTPResponse>(res.status);
		                   response->headers = res.headers;
		                   response->url = res.url;
		                   response->request_error = res.request_error;
		                   if (res.HasHeader("Link")) {
			                   link_header = res.GetHeaderValue("Link");
		                   }
		                   return true;
	                   },
	                   [&](const_data_ptr_t data, idx_t data_length) {
		                   page.body.append(const_char_ptr_cast(data), data_length);
		                   return true;
	                   });
	get.try_request = true;
	auto res = SendRequest(get);
	if (res->HasRequestError()) {
		ThrowHTTPError(*res, "GET (listing)", request_url);
	}
	if (tolerate_not_found && static_cast<int>(res->status) == 404) {
		page.not_found = true;
		return page;
	}
	if (static_cast<int>(res->status) != 200) {
		ThrowHTTPError(*res, "GET (listing)", request_url);
	}
	if (!link_header.empty()) {
		bool found = false;
		page.next_url = HFJson::NextLinkUrl(link_header, found);
		page.has_next = found;
	}
	return page;
}

} // namespace duckdb
