// chat_translate_impl.h  --  include it ONLY at the end of updater/update_checker.cpp
// (that file already works with the HTTP client; chat.cpp must not include it, because the HTTP
// library drags windows.h in, and windows.h defines macros (min, PostMessage, PlaySound) that break chat.cpp).
//
// Translates incoming chat messages inside the client: no plugin and no server of yours are needed.
//
// It is OFF by default (hud_translate 0): when it is on, the TEXT of the chat messages leaves your
// computer and is sent to a translation service (Google, then MyMemory), or to the address in
// hud_translate_url if you set one.
//
// The request goes through the client's own HTTP client (updater/http_client.h), which works in a
// separate thread, so the game never freezes. The answer is printed in the chat as a new line.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "updater/http_client.h"
#include "hud/arabic_text.h"

// these two live in hud/chat.cpp
void ChatTranslate_ShowLine(const char *text);
bool ChatTranslate_IsLocalPlayer(int client);

static ConVar hud_translate("hud_translate", "0", FCVAR_BHL_ARCHIVE,
    "Translate incoming chat messages. The text of the messages is sent to a translation service (Google / MyMemory, or hud_translate_url).");
static ConVar hud_translate_lang("hud_translate_lang", "ar", FCVAR_BHL_ARCHIVE,
    "Language to translate chat messages into (ar, en, fr, ru, tr, es, de...)");
static ConVar hud_translate_own("hud_translate_own", "0", FCVAR_BHL_ARCHIVE,
    "Also translate your own messages");
static ConVar hud_translate_url("hud_translate_url", "", FCVAR_BHL_ARCHIVE,
    "Optional address of your own translation script (translate.php). Empty = Google/MyMemory directly");
static ConVar hud_translate_email("hud_translate_email", "", FCVAR_BHL_ARCHIVE,
    "Optional e-mail address for MyMemory (raises its daily limit from 5000 to 50000 characters)");

namespace ChatTranslate
{

// ------------------------------------------------------------------ tiny JSON reader
// (only what the translation answers need; no external library)
struct JVal
{
	enum Type { Null, Bool, Num, Str, Arr, Obj };
	Type t = Null;
	std::string s;                 // string / number text / "true" "false"
	std::vector<JVal> a;           // array items, or object values
	std::vector<std::string> keys; // object keys (same order as a)

	const JVal *Get(const char *key) const
	{
		for (size_t i = 0; i < keys.size(); i++)
			if (keys[i] == key)
				return &a[i];
		return nullptr;
	}
};

class JsonParser
{
public:
	explicit JsonParser(const std::string &src)
	    : m_src(src) // own copy: the parser never points into a string that may be gone
	{
		m_p = m_src.data();
		m_e = m_src.data() + m_src.size();
	}

	bool Parse(JVal &out) { return Value(out); }

private:
	std::string m_src;
	const char *m_p = nullptr;
	const char *m_e = nullptr;
	int m_depth = 0;

	void Ws()
	{
		while (m_p < m_e && (*m_p == ' ' || *m_p == '\n' || *m_p == '\r' || *m_p == '\t'))
			++m_p;
	}

	bool Lit(const char *word, size_t n)
	{
		if ((size_t)(m_e - m_p) >= n && strncmp(m_p, word, n) == 0)
		{
			m_p += n;
			return true;
		}
		return false;
	}

	bool Value(JVal &v)
	{
		Ws();
		if (m_p >= m_e)
			return false;
		if (++m_depth > 32)
			return false;

		bool ok = false;
		const char c = *m_p;
		if (c == '[')       ok = Array(v);
		else if (c == '{')  ok = Object(v);
		else if (c == '"')  { v.t = JVal::Str; ok = String(v.s); }
		else if (c == 'n')  { v.t = JVal::Null; ok = Lit("null", 4); }
		else if (c == 't')  { v.t = JVal::Bool; v.s = "true"; ok = Lit("true", 4); }
		else if (c == 'f')  { v.t = JVal::Bool; v.s = "false"; ok = Lit("false", 5); }
		else                { v.t = JVal::Num; ok = Number(v.s); }

		--m_depth;
		return ok;
	}

	bool Number(std::string &out)
	{
		const char *b = m_p;
		while (m_p < m_e && ((*m_p >= '0' && *m_p <= '9') || *m_p == '-' || *m_p == '+' || *m_p == '.' || *m_p == 'e' || *m_p == 'E'))
			++m_p;
		if (m_p == b)
			return false;
		out.assign(b, m_p);
		return true;
	}

	bool Array(JVal &v)
	{
		v.t = JVal::Arr;
		++m_p; // [
		Ws();
		if (m_p < m_e && *m_p == ']')
		{
			++m_p;
			return true;
		}
		for (;;)
		{
			JVal item;
			if (!Value(item))
				return false;
			v.a.push_back(std::move(item));
			Ws();
			if (m_p >= m_e)
				return false;
			if (*m_p == ',') { ++m_p; continue; }
			if (*m_p == ']') { ++m_p; return true; }
			return false;
		}
	}

	bool Object(JVal &v)
	{
		v.t = JVal::Obj;
		++m_p; // {
		Ws();
		if (m_p < m_e && *m_p == '}')
		{
			++m_p;
			return true;
		}
		for (;;)
		{
			Ws();
			if (m_p >= m_e || *m_p != '"')
				return false;
			std::string key;
			if (!String(key))
				return false;
			Ws();
			if (m_p >= m_e || *m_p != ':')
				return false;
			++m_p;
			JVal item;
			if (!Value(item))
				return false;
			v.keys.push_back(std::move(key));
			v.a.push_back(std::move(item));
			Ws();
			if (m_p >= m_e)
				return false;
			if (*m_p == ',') { ++m_p; continue; }
			if (*m_p == '}') { ++m_p; return true; }
			return false;
		}
	}

	bool Hex4(unsigned &cp)
	{
		if (m_e - m_p < 4)
			return false;
		cp = 0;
		for (int i = 0; i < 4; i++)
		{
			const char h = *m_p++;
			cp <<= 4;
			if (h >= '0' && h <= '9')       cp |= (unsigned)(h - '0');
			else if (h >= 'a' && h <= 'f')  cp |= (unsigned)(h - 'a' + 10);
			else if (h >= 'A' && h <= 'F')  cp |= (unsigned)(h - 'A' + 10);
			else return false;
		}
		return true;
	}

	bool String(std::string &out)
	{
		++m_p; // opening quote
		while (m_p < m_e)
		{
			const char c = *m_p++;
			if (c == '"')
				return true;
			if (c != '\\')
			{
				out += c;
				continue;
			}
			if (m_p >= m_e)
				return false;
			const char e = *m_p++;
			switch (e)
			{
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case '/': case '\\': case '"': out += e; break;
			case 'u':
			{
				unsigned cp = 0;
				if (!Hex4(cp))
					return false;
				// surrogate pair
				if (cp >= 0xD800 && cp <= 0xDBFF && m_e - m_p >= 6 && m_p[0] == '\\' && m_p[1] == 'u')
				{
					m_p += 2;
					unsigned lo = 0;
					if (!Hex4(lo))
						return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				ArabicText::detail::AppendUtf8(out, (char32_t)cp);
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}
};

// ------------------------------------------------------------------ helpers
inline std::string UrlEncode(const std::string &s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(s.size() * 3);
	for (unsigned char c : s)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

inline std::string Trim(const std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && (unsigned char)s[b] <= ' ') ++b;
	while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
	return s.substr(b, e - b);
}

// removes "^1" style color codes and control characters
inline std::string StripCodes(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20)
			continue;
		if (c == '^' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9')
		{
			++i;
			continue;
		}
		out += (char)c;
	}
	return out;
}

// rough guess of the language of a text from its alphabet (enough to skip text that is already in
// the target language, and to give MyMemory a source language)
inline std::string GuessLang(const std::string &utf8)
{
	size_t ar = 0, cy = 0, cjk = 0;
	for (unsigned char c : utf8)
	{
		if (c >= 0xD8 && c <= 0xDB) ar++;
		else if (c >= 0xD0 && c <= 0xD3) cy++;
		else if (c >= 0xE4 && c <= 0xE9) cjk++;
	}
	if (ar) return "ar";
	if (cy) return "ru";
	if (cjk) return "zh";
	return "en";
}

// "KYC: some text"  ->  "some text"
inline std::string ExtractBody(const std::string &line, const std::string &name)
{
	size_t colon = std::string::npos;
	if (!name.empty())
	{
		size_t np = line.find(name);
		if (np != std::string::npos)
			colon = line.find(':', np + name.size());
	}
	if (colon == std::string::npos)
		colon = line.find(':');
	if (colon == std::string::npos)
		return std::string();
	return Trim(line.substr(colon + 1));
}

inline bool WorthTranslating(const std::string &body, const std::string &target)
{
	if (body.size() < 3 || body[0] == '/')
		return false;

	size_t letters = 0;
	for (unsigned char c : body)
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80)
			letters++;

	if (letters < 3)
		return false;
	if (body.size() > 5 && letters * 100 / body.size() < 30) // mostly symbols / smileys
		return false;

	// already written in the target language
	if (GuessLang(body).substr(0, 2) == target.substr(0, 2))
		return false;

	return true;
}

// ------------------------------------------------------------------ service answers
inline bool ParseGoogle(const std::string &data, std::string &out, std::string &detected)
{
	JVal root;
	JsonParser p(data);
	if (!p.Parse(root) || root.t != JVal::Arr || root.a.empty() || root.a[0].t != JVal::Arr)
		return false;

	for (const JVal &seg : root.a[0].a)
		if (seg.t == JVal::Arr && !seg.a.empty() && seg.a[0].t == JVal::Str)
			out += seg.a[0].s;

	if (root.a.size() > 2 && root.a[2].t == JVal::Str)
		detected = root.a[2].s;

	return !out.empty();
}

inline bool ParseMyMemory(const std::string &data, std::string &out)
{
	JVal root;
	JsonParser p(data);
	if (!p.Parse(root) || root.t != JVal::Obj)
		return false;

	const JVal *status = root.Get("responseStatus");
	if (!status || atoi(status->s.c_str()) != 200)
		return false;

	const JVal *rd = root.Get("responseData");
	if (!rd || rd->t != JVal::Obj)
		return false;

	const JVal *tt = rd->Get("translatedText");
	if (!tt || tt->t != JVal::Str || tt->s.empty())
		return false;

	if (tt->s.find("MYMEMORY WARNING") != std::string::npos) // daily limit reached
		return false;

	out = tt->s;
	return true;
}

// answer of translate.php:  RES|translated text|ar|END
inline bool ParseCustom(const std::string &data, std::string &out)
{
	size_t pos = data.find("RES|");
	if (pos == std::string::npos)
		return false;
	std::string rest = data.substr(pos + 4);
	size_t end = rest.rfind("|END");
	if (end == std::string::npos)
		return false;
	rest = rest.substr(0, end);
	size_t bar = rest.rfind('|');
	if (bar != std::string::npos)
		rest = rest.substr(0, bar);
	out = Trim(rest);
	return !out.empty();
}

// ------------------------------------------------------------------ the translation job
struct Job
{
	std::string key;    // cache key
	std::string body;   // text to translate (normal Arabic order)
	std::string who;    // player name
	std::string target; // target language
};

inline std::unordered_map<std::string, std::string> &Cache()
{
	static std::unordered_map<std::string, std::string> cache;
	return cache;
}

inline int &Pending()
{
	static int pending = 0;
	return pending;
}

inline double &GooglePausedUntil()
{
	static double until = 0.0;
	return until;
}

inline void Show(const Job &job, const std::string &translated)
{
	std::string line = "^8[ " + job.who + " > " + job.target + " ]^0 " + translated;
	ChatTranslate_ShowLine(line.c_str());
}

// every job ends here, exactly once
inline void Done(const Job &job, const std::string &translated, const std::string &detected)
{
	if (Pending() > 0)
		Pending()--;

	if (translated.empty() || translated == job.body)
		return;
	if (!detected.empty() && detected.substr(0, 2) == job.target.substr(0, 2))
		return; // it was already in the target language

	if (Cache().size() > 500)
		Cache().clear();
	Cache()[job.key] = translated;

	Show(job, translated);
}

inline void StartGoogle(const Job &job);
inline void StartMyMemory(const Job &job);

inline void StartCustom(const Job &job, const std::string &baseUrl)
{
	std::string url = baseUrl;
	url += (url.find('?') == std::string::npos) ? "?" : "&";
	url += "text=" + UrlEncode(job.body) + "&from=auto&to=" + UrlEncode(job.target);

	CHttpClient::Request req(url);
	req.SetCallback([job](CHttpClient::Response &resp) {
		std::string out;
		if (resp.IsSuccess() && ParseCustom(std::string(resp.GetResponseData().data(), resp.GetResponseData().size()), out))
			Done(job, out, std::string());
		else
			StartGoogle(job); // your script is down: use the public services
	});
	CHttpClient::Get().Get(req);
}

inline void StartGoogle(const Job &job)
{
	if (gEngfuncs.GetAbsoluteTime() < GooglePausedUntil())
	{
		StartMyMemory(job);
		return;
	}

	std::string url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=" + UrlEncode(job.target) + "&dt=t&q=" + UrlEncode(job.body);

	CHttpClient::Request req(url);
	req.AddHeader("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36");
	req.SetCallback([job](CHttpClient::Response &resp) {
		if (!resp.IsSuccess())
		{
			// 429: Google blocks this address for a while, do not insist
			if (resp.GetError().find("429") != std::string::npos)
				GooglePausedUntil() = gEngfuncs.GetAbsoluteTime() + 900.0;
			StartMyMemory(job);
			return;
		}

		std::string out, detected;
		if (ParseGoogle(std::string(resp.GetResponseData().data(), resp.GetResponseData().size()), out, detected))
			Done(job, out, detected);
		else
			StartMyMemory(job);
	});
	CHttpClient::Get().Get(req);
}

inline void StartMyMemory(const Job &job)
{
	const std::string src = GuessLang(job.body);
	if (src.substr(0, 2) == job.target.substr(0, 2))
	{
		Done(job, std::string(), std::string());
		return;
	}

	std::string url = "https://api.mymemory.translated.net/get?q=" + UrlEncode(job.body) + "&langpair=" + UrlEncode(src + "|" + job.target);
	const std::string email = hud_translate_email.GetString();
	if (!email.empty())
		url += "&de=" + UrlEncode(email);

	CHttpClient::Request req(url);
	req.SetCallback([job](CHttpClient::Response &resp) {
		std::string out;
		if (resp.IsSuccess() && ParseMyMemory(std::string(resp.GetResponseData().data(), resp.GetResponseData().size()), out))
			Done(job, out, std::string());
		else
			Done(job, std::string(), std::string());
	});
	CHttpClient::Get().Get(req);
}

// ------------------------------------------------------------------ entry point
// Call it for every chat line received from a player.
//   client  = index of the player who wrote it (0 = server / console: ignored)
//   line    = the chat line as received ("<color>Name: text")
//   name    = the name of the player
void OnChatLine(int client, const char *line, const char *name)
{
	if (!hud_translate.GetBool() || client <= 0 || !line || !name)
		return;

	if (!hud_translate_own.GetBool() && ChatTranslate_IsLocalPlayer(client))
		return;

	// target language: letters and "-" only, otherwise Arabic
	std::string target = hud_translate_lang.GetString();
	bool valid = target.size() >= 2 && target.size() <= 7;
	for (char c : target)
		if (!((c >= 'a' && c <= 'z') || c == '-'))
			valid = false;
	if (!valid)
		target = "ar";

	std::string body = StripCodes(ExtractBody(line, name));

	// text shaped by another client / the server plugin: back to normal order first
	body = ArabicText::ToLogicalUtf8(body);
	body = Trim(body);

	if (!WorthTranslating(body, target))
		return;

	Job job;
	job.key = target + "|" + body;
	job.body = body;
	job.who = name;
	job.target = target;

	auto it = Cache().find(job.key);
	if (it != Cache().end())
	{
		Show(job, it->second);
		return;
	}

	if (Pending() >= 6) // too many requests waiting: drop this one
		return;
	Pending()++;

	const std::string custom = hud_translate_url.GetString();
	if (!custom.empty())
		StartCustom(job, custom);
	else
		StartGoogle(job);
}

} // namespace ChatTranslate
