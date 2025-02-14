#include "parser.h"

#include <cinttypes>
#include <cstring>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <vector>

#include "config.h"
#include "curlhandle.h"
#include "exception.h"
#include "logger.h"
#include "remoteapi.h"
#include "rssparser.h"
#include "rssparserfactory.h"
#include "rsspp_uris.h"
#include "strprintf.h"
#include "utils.h"

using namespace newsboat;

static size_t my_write_data(void* buffer, size_t size, size_t nmemb,
	void* userp)
{
	std::string* pbuf = static_cast<std::string*>(userp);
	pbuf->append(static_cast<const char*>(buffer), size * nmemb);
	return size * nmemb;
}

namespace rsspp {

Parser::Parser(unsigned int timeout,
	const std::string& user_agent,
	const std::string& proxy,
	const std::string& proxy_auth,
	curl_proxytype proxy_type,
	const bool ssl_verify)
	: to(timeout)
	, ua(user_agent)
	, prx(proxy)
	, prxauth(proxy_auth)
	, prxtype(proxy_type)
	, verify_ssl(ssl_verify)
	, doc(0)
	, lm(0)
{
}

Parser::~Parser()
{
	if (doc) {
		xmlFreeDoc(doc);
	}
}

namespace {

struct HeaderValues {
	time_t lastmodified;
	std::string etag;
	std::string charset;

	HeaderValues()
	{
		reset();
	}

	void reset()
	{
		lastmodified = 0;
		charset = "utf-8";
		etag = "";
	}
};

}

static size_t handle_headers(void* ptr, size_t size, size_t nmemb, void* data)
{
	const auto header = std::string(reinterpret_cast<const char*>(ptr), size * nmemb);
	HeaderValues* values = static_cast<HeaderValues*>(data);

	if (header.find("HTTP/") == 0) {
		// Reset headers if a new response is detected (there might be multiple responses per request in case of a redirect)
		values->reset();
	} else if (header.find("Last-Modified:") == 0) {
		const std::string header_value = header.substr(14);
		time_t r = curl_getdate(header_value.c_str(), nullptr);
		if (r == -1) {
			LOG(Level::DEBUG,
				"handle_headers: last-modified %s "
				"(curl_getdate "
				"FAILED)",
				header_value.c_str());
		} else {
			values->lastmodified = r;
			LOG(Level::DEBUG,
				"handle_headers: got last-modified %s (%" PRId64 ")",
				header_value,
				// On GCC, `time_t` is `long int`, which is at least 32 bits.
				// On x86_64, it's 64 bits. Thus, this cast is either a no-op,
				// or an up-cast which is always safe.
				static_cast<int64_t>(values->lastmodified));
		}
	} else if (header.find("ETag:") == 0) {
		values->etag = header.substr(5);
		utils::trim(values->etag);
		LOG(Level::DEBUG, "handle_headers: got etag %s", values->etag);
	} else if (header.find("Content-Type:") == 0) {
		const std::string key = "charset=";
		const auto charset_index = header.find(key);
		if (charset_index != std::string::npos) {
			auto charset = header.substr(charset_index + key.size());
			utils::trim(charset);
			if (charset.size() >= 2 && charset[0] == '"' && charset[charset.size() - 1] == '"') {
				charset = charset.substr(1, charset.size() - 2);
			}
			if (charset.size() > 0) {
				values->charset = charset;
			}
		}
	}

	return size * nmemb;
}

Feed Parser::parse_url(const std::string& url,
	time_t lastmodified,
	const std::string& etag,
	newsboat::RemoteApi* api,
	const std::string& cookie_cache)
{
	CurlHandle handle;
	return parse_url(url, handle, lastmodified, etag, api, cookie_cache);
}

Feed Parser::parse_url(const std::string& url,
	newsboat::CurlHandle& easyhandle,
	time_t lastmodified,
	const std::string& etag,
	newsboat::RemoteApi* api,
	const std::string& cookie_cache)
{
	std::string buf;
	CURLcode ret;
	curl_slist* custom_headers{};

	if (!ua.empty()) {
		curl_easy_setopt(easyhandle.ptr(), CURLOPT_USERAGENT, ua.c_str());
	}

	if (api) {
		api->add_custom_headers(&custom_headers);
	}
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_URL, url.c_str());
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_SSL_VERIFYPEER, verify_ssl);
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_WRITEFUNCTION, my_write_data);
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_MAXREDIRS, 10);
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_FAILONERROR, 1);
	// Accept all of curl's built-in encodings
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_ACCEPT_ENCODING, "");
	if (cookie_cache != "") {
		curl_easy_setopt(
			easyhandle.ptr(), CURLOPT_COOKIEFILE, cookie_cache.c_str());
		curl_easy_setopt(
			easyhandle.ptr(), CURLOPT_COOKIEJAR, cookie_cache.c_str());
	}
	if (to != 0) {
		curl_easy_setopt(easyhandle.ptr(), CURLOPT_TIMEOUT, to);
	}

	if (!prx.empty()) {
		curl_easy_setopt(easyhandle.ptr(), CURLOPT_PROXY, prx.c_str());
	}

	if (!prxauth.empty()) {
		curl_easy_setopt(easyhandle.ptr(), CURLOPT_PROXYAUTH, CURLAUTH_ANY);
		curl_easy_setopt(
			easyhandle.ptr(), CURLOPT_PROXYUSERPWD, prxauth.c_str());
	}

	curl_easy_setopt(easyhandle.ptr(), CURLOPT_PROXYTYPE, prxtype);

	const char* curl_ca_bundle = ::getenv("CURL_CA_BUNDLE");
	if (curl_ca_bundle != nullptr) {
		curl_easy_setopt(easyhandle.ptr(), CURLOPT_CAINFO, curl_ca_bundle);
	}

	HeaderValues hdrs;
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_HEADERDATA, &hdrs);
	curl_easy_setopt(easyhandle.ptr(), CURLOPT_HEADERFUNCTION, handle_headers);

	if (lastmodified != 0) {
		curl_easy_setopt(easyhandle.ptr(),
			CURLOPT_TIMECONDITION,
			CURL_TIMECOND_IFMODSINCE);
		curl_easy_setopt(easyhandle.ptr(), CURLOPT_TIMEVALUE, lastmodified);
	}

	if (etag.length() > 0) {
		auto header = strprintf::fmt("If-None-Match: %s", etag);
		custom_headers =
			curl_slist_append(custom_headers, header.c_str());
	}

	if (lastmodified != 0 || etag.length() > 0) {
		custom_headers =
			curl_slist_append(custom_headers, "A-IM: feed");
	}

	if (custom_headers) {
		curl_easy_setopt(
			easyhandle.ptr(), CURLOPT_HTTPHEADER, custom_headers);
	}

	ret = curl_easy_perform(easyhandle.ptr());

	lm = hdrs.lastmodified;
	et = hdrs.etag;

	if (custom_headers) {
		curl_easy_setopt(easyhandle.ptr(), CURLOPT_HTTPHEADER, 0);
		curl_slist_free_all(custom_headers);
	}

	LOG(Level::DEBUG,
		"rsspp::Parser::parse_url: ret = %d (%s)",
		ret,
		curl_easy_strerror(ret));

	long status;
	CURLcode infoOk =
		curl_easy_getinfo(easyhandle.ptr(), CURLINFO_RESPONSE_CODE, &status);

	curl_easy_reset(easyhandle.ptr());
	if (cookie_cache != "") {
		curl_easy_setopt(
			easyhandle.ptr(), CURLOPT_COOKIEJAR, cookie_cache.c_str());
	}

	if (ret != 0) {
		LOG(Level::ERROR,
			"rsspp::Parser::parse_url: curl_easy_perform returned "
			"err "
			"%d: %s",
			ret,
			curl_easy_strerror(ret));
		std::string msg;
		if (ret == CURLE_HTTP_RETURNED_ERROR && infoOk == CURLE_OK) {
			msg = strprintf::fmt(
					"%s %" PRIi64,
					curl_easy_strerror(ret),
					// `status` is `long`, which is at least 32 bits, and on x86_64
					// it's actually 64 bits. Thus casting to `int64_t` is either
					// a no-op, or an up-cast which are always safe.
					static_cast<int64_t>(status));
		} else {
			msg = curl_easy_strerror(ret);
		}
		throw Exception(msg);
	}

	LOG(Level::INFO,
		"Parser::parse_url: retrieved data for %s: %s",
		url,
		buf);

	if (buf.length() > 0) {
		LOG(Level::DEBUG, "Parser::parse_url: converting data from %s to utf-8", hdrs.charset);
		const auto utf8_buf = utils::convert_text(buf, "utf-8", hdrs.charset);

		LOG(Level::DEBUG, "Parser::parse_url: handing over data to parse_buffer()");
		return parse_buffer(utf8_buf, url);
	}

	return Feed();
}

Feed Parser::parse_buffer(const std::string& buffer, const std::string& url)
{
	doc = xmlReadMemory(buffer.c_str(),
			buffer.length(),
			url.c_str(),
			nullptr,
			XML_PARSE_RECOVER | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (doc == nullptr) {
		throw Exception(_("could not parse buffer"));
	}

	xmlNode* root_element = xmlDocGetRootElement(doc);

	Feed f = parse_xmlnode(root_element);

	if (doc->encoding) {
		f.encoding = (const char*)doc->encoding;
	}

	LOG(Level::INFO, "Parser::parse_buffer: encoding = %s", f.encoding);

	return f;
}

Feed Parser::parse_file(const std::string& filename)
{
	doc = xmlReadFile(filename.c_str(),
			nullptr,
			XML_PARSE_RECOVER | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	xmlNode* root_element = xmlDocGetRootElement(doc);

	if (root_element == nullptr) {
		throw Exception(_("could not parse file"));
	}

	Feed f = parse_xmlnode(root_element);

	if (doc->encoding) {
		f.encoding = (const char*)doc->encoding;
	}

	LOG(Level::INFO, "Parser::parse_file: encoding = %s", f.encoding);

	return f;
}

Feed Parser::parse_xmlnode(xmlNode* node)
{
	Feed f;

	if (node) {
		if (node->name && node->type == XML_ELEMENT_NODE) {
			if (strcmp((const char*)node->name, "rss") == 0) {
				const char* version = (const char*)xmlGetProp(
						node, (const xmlChar*)"version");
				if (!version) {
					xmlFree((void*)version);
					throw Exception(_("no RSS version"));
				}
				if (strcmp(version, "0.91") == 0) {
					f.rss_version = Feed::RSS_0_91;
				} else if (strcmp(version, "0.92") == 0) {
					f.rss_version = Feed::RSS_0_92;
				} else if (strcmp(version, "0.94") == 0) {
					f.rss_version = Feed::RSS_0_94;
				} else if (strcmp(version, "2.0") == 0 ||
					strcmp(version, "2") == 0) {
					f.rss_version = Feed::RSS_2_0;
				} else if (strcmp(version, "1.0") == 0) {
					f.rss_version = Feed::RSS_0_91;
				} else {
					xmlFree((void*)version);
					throw Exception(
						_("invalid RSS version"));
				}
				xmlFree((void*)version);
			} else if (strcmp((const char*)node->name, "RDF") ==
				0) {
				f.rss_version = Feed::RSS_1_0;
			} else if (strcmp((const char*)node->name, "feed") ==
				0) {
				if (node->ns && node->ns->href) {
					if (strcmp((const char*)node->ns->href,
							ATOM_0_3_URI) == 0) {
						f.rss_version = Feed::ATOM_0_3;
					} else if (strcmp((const char*)node->ns
							->href,
							ATOM_1_0_URI) == 0) {
						f.rss_version = Feed::ATOM_1_0;
					} else {
						const char* version = (const char*)xmlGetProp(node, (const xmlChar*)"version");
						if (!version) {
							xmlFree((void*)version);
							throw Exception(_(
									"invalid Atom "
									"version"));
						}
						if (strcmp(version, "0.3") ==
							0) {
							xmlFree((void*)version);
							f.rss_version =
								Feed::ATOM_0_3_NONS;
						} else {
							xmlFree((void*)version);
							throw Exception(_(
									"invalid Atom "
									"version"));
						}
					}
				} else {
					throw Exception(_("no Atom version"));
				}
			}

			std::shared_ptr<RssParser> parser =
				RssParserFactory::get_object(f.rss_version, doc);

			try {
				parser->parse_feed(f, node);
			} catch (Exception& e) {
				throw;
			}
		}
	} else {
		throw Exception(_("XML root node is NULL"));
	}

	return f;
}

void Parser::global_init()
{
	LIBXML_TEST_VERSION
	curl_global_init(CURL_GLOBAL_ALL);
}

void Parser::global_cleanup()
{
	xmlCleanupParser();
	curl_global_cleanup();
}

} // namespace rsspp
