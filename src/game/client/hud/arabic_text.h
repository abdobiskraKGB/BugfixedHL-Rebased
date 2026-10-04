// arabic_text.h  --  header-only, no dependencies (C++11)
// Converts logical-order Arabic text (as typed) into the form a left-to-right
// renderer needs: contextual letter shaping (isolated/initial/medial/final,
// lam-alef ligatures) + simplified Unicode bidi reordering.
//
// Call it on every VISUAL LINE right before drawing (after line wrapping).
// Text with no Arabic characters is returned untouched (fast path).
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ArabicText
{

// true  = remove harakat (fatha, damma...) because LTR engines cannot place combining marks
static const bool kStripDiacritics = true;

namespace detail
{

enum JoinType : uint8_t { JT_U = 0, JT_R = 1, JT_D = 2 };

struct ShapeEntry
{
	char32_t base;
	JoinType jt;
	char32_t iso, fin, ini, med; // 0 = form does not exist
};

inline const ShapeEntry *FindShape(char32_t c)
{
	static const ShapeEntry table[] = {
		{ 0x0621, JT_U, 0xFE80, 0, 0, 0 },
		{ 0x0622, JT_R, 0xFE81, 0xFE82, 0, 0 },
		{ 0x0623, JT_R, 0xFE83, 0xFE84, 0, 0 },
		{ 0x0624, JT_R, 0xFE85, 0xFE86, 0, 0 },
		{ 0x0625, JT_R, 0xFE87, 0xFE88, 0, 0 },
		{ 0x0626, JT_D, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C },
		{ 0x0627, JT_R, 0xFE8D, 0xFE8E, 0, 0 },
		{ 0x0628, JT_D, 0xFE8F, 0xFE90, 0xFE91, 0xFE92 },
		{ 0x0629, JT_R, 0xFE93, 0xFE94, 0, 0 },
		{ 0x062A, JT_D, 0xFE95, 0xFE96, 0xFE97, 0xFE98 },
		{ 0x062B, JT_D, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C },
		{ 0x062C, JT_D, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 },
		{ 0x062D, JT_D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 },
		{ 0x062E, JT_D, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 },
		{ 0x062F, JT_R, 0xFEA9, 0xFEAA, 0, 0 },
		{ 0x0630, JT_R, 0xFEAB, 0xFEAC, 0, 0 },
		{ 0x0631, JT_R, 0xFEAD, 0xFEAE, 0, 0 },
		{ 0x0632, JT_R, 0xFEAF, 0xFEB0, 0, 0 },
		{ 0x0633, JT_D, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 },
		{ 0x0634, JT_D, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 },
		{ 0x0635, JT_D, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC },
		{ 0x0636, JT_D, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 },
		{ 0x0637, JT_D, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 },
		{ 0x0638, JT_D, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 },
		{ 0x0639, JT_D, 0xFEC9, 0xFECA, 0xFECB, 0xFECC },
		{ 0x063A, JT_D, 0xFECD, 0xFECE, 0xFECF, 0xFED0 },
		{ 0x0640, JT_D, 0x0640, 0x0640, 0x0640, 0x0640 }, // tatweel
		{ 0x0641, JT_D, 0xFED1, 0xFED2, 0xFED3, 0xFED4 },
		{ 0x0642, JT_D, 0xFED5, 0xFED6, 0xFED7, 0xFED8 },
		{ 0x0643, JT_D, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC },
		{ 0x0644, JT_D, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 },
		{ 0x0645, JT_D, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 },
		{ 0x0646, JT_D, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 },
		{ 0x0647, JT_D, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC },
		{ 0x0648, JT_R, 0xFEED, 0xFEEE, 0, 0 },
		{ 0x0649, JT_R, 0xFEEF, 0xFEF0, 0, 0 },
		{ 0x064A, JT_D, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 },
		// Persian / Urdu extras
		{ 0x067E, JT_D, 0xFB56, 0xFB57, 0xFB58, 0xFB59 }, // peh
		{ 0x0686, JT_D, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D }, // tcheh
		{ 0x0698, JT_R, 0xFB8A, 0xFB8B, 0, 0 },           // jeh
		{ 0x06A9, JT_D, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91 }, // keheh
		{ 0x06AF, JT_D, 0xFB92, 0xFB93, 0xFB94, 0xFB95 }, // gaf
		{ 0x06CC, JT_D, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF }, // farsi yeh
	};
	const ShapeEntry *end = table + sizeof(table) / sizeof(table[0]);
	const ShapeEntry *it = std::lower_bound(table, end, c,
		[](const ShapeEntry &e, char32_t v) { return e.base < v; });
	return (it != end && it->base == c) ? it : nullptr;
}

inline bool IsMark(char32_t c)
{
	return (c >= 0x064B && c <= 0x065F) || c == 0x0670;
}

inline bool IsArabicCp(char32_t c)
{
	return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) ||
	       (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

// lam + alef variants -> ligature {isolated, final}
inline bool LamAlef(char32_t alef, char32_t &iso, char32_t &fin)
{
	switch (alef)
	{
	case 0x0622: iso = 0xFEF5; fin = 0xFEF6; return true;
	case 0x0623: iso = 0xFEF7; fin = 0xFEF8; return true;
	case 0x0625: iso = 0xFEF9; fin = 0xFEFA; return true;
	case 0x0627: iso = 0xFEFB; fin = 0xFEFC; return true;
	}
	return false;
}

// ---- 1) contextual shaping -------------------------------------------------
inline std::u32string Shape(const std::u32string &in)
{
	std::u32string s;
	s.reserve(in.size());
	for (char32_t c : in)
		if (!(kStripDiacritics && IsMark(c)))
			s.push_back(c);

	const size_t n = s.size();
	auto prevJoinsForward = [&](size_t i) -> bool {
		while (i > 0)
		{
			--i;
			if (IsMark(s[i])) continue;
			const ShapeEntry *p = FindShape(s[i]);
			return p && p->jt == JT_D;
		}
		return false;
	};
	auto nextAcceptsPrev = [&](size_t i) -> bool {
		for (++i; i < n; ++i)
		{
			if (IsMark(s[i])) continue;
			const ShapeEntry *q = FindShape(s[i]);
			return q && (q->jt == JT_D || q->jt == JT_R);
		}
		return false;
	};

	std::u32string out;
	out.reserve(n);
	for (size_t i = 0; i < n; ++i)
	{
		const ShapeEntry *e = FindShape(s[i]);
		if (!e) { out.push_back(s[i]); continue; }

		bool prev = prevJoinsForward(i);

		if (s[i] == 0x0644 && i + 1 < n)
		{
			char32_t li, lf;
			if (LamAlef(s[i + 1], li, lf))
			{
				out.push_back(prev ? lf : li);
				++i;
				continue;
			}
		}

		bool next = (e->jt == JT_D) && nextAcceptsPrev(i);
		char32_t g = e->iso;
		if (e->jt == JT_D)
		{
			if (prev && next && e->med) g = e->med;
			else if (prev && e->fin)    g = e->fin;
			else if (next && e->ini)    g = e->ini;
		}
		else if (e->jt == JT_R)
		{
			if (prev && e->fin) g = e->fin;
		}
		out.push_back(g);
	}
	return out;
}

// ---- 2) simplified bidi (LTR paragraph, RTL runs reversed) -------------------
enum Cls : uint8_t { C_L, C_R, C_N, C_W }; // N = digit, W = neutral

inline Cls Classify(char32_t c)
{
	if ((c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9))
		return C_N;
	if (c == 0x060C || c == 0x061B || c == 0x061F || c == 0x066A || c == 0x066B || c == 0x066C)
		return C_W;
	if (IsArabicCp(c)) return C_R;
	if (c < 0x80)
		return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ? C_L : C_W;
	if (c >= 0x2000 && c <= 0x206F) return C_W; // general punctuation / spaces
	if (c == 0x00A0) return C_W;
	return C_L;
}

inline char32_t Mirror(char32_t c)
{
	switch (c)
	{
	case '(': return ')'; case ')': return '(';
	case '[': return ']'; case ']': return '[';
	case '{': return '}'; case '}': return '{';
	case '<': return '>'; case '>': return '<';
	case 0x00AB: return 0x00BB; case 0x00BB: return 0x00AB;
	}
	return c;
}

inline bool IsRtlParagraph(const std::u32string &s)
{
	for (char32_t c : s)
	{
		Cls k = Classify(c);
		if (k == C_L) return false;
		if (k == C_R) return true;
	}
	return false;
}

// baseRtl = paragraph direction (true: Arabic paragraph, false: Latin paragraph)
inline std::u32string Reorder(const std::u32string &s, bool baseRtl)
{
	const size_t n = s.size();
	std::vector<Cls> t(n);
	for (size_t i = 0; i < n; ++i) t[i] = Classify(s[i]);

	// digits: after an L (or at the start of an LTR paragraph) they behave as L,
	// after an R (or at the start of an RTL paragraph) they form a level-2 number
	{
		int lastStrong = baseRtl ? 1 : 0; // 0 = L, 1 = R
		for (size_t i = 0; i < n; ++i)
		{
			if (t[i] == C_L) lastStrong = 0;
			else if (t[i] == C_R) lastStrong = 1;
			else if (t[i] == C_N && lastStrong != 1) t[i] = C_L;
		}
	}

	const uint8_t baseLevel = baseRtl ? 1 : 0;
	const uint8_t lLevel = baseRtl ? 2 : 0;

	std::vector<uint8_t> lvl(n, 0);
	for (size_t i = 0; i < n; ++i)
		lvl[i] = (t[i] == C_R) ? 1 : (t[i] == C_N) ? 2 : lLevel;

	// neutrals take the direction of their surroundings if both sides agree, else the paragraph direction
	for (size_t i = 0; i < n;)
	{
		if (t[i] != C_W) { ++i; continue; }
		size_t j = i;
		while (j < n && t[j] == C_W) ++j;
		auto dirOf = [&](long k) -> int { // 1 = R-ish, 0 = L-ish
			if (k < 0 || k >= (long)n) return baseRtl ? 1 : 0;
			return (t[k] == C_R || t[k] == C_N) ? 1 : 0;
		};
		int a = dirOf((long)i - 1), b = dirOf((long)j);
		uint8_t l = (a == b) ? (a ? 1 : lLevel) : baseLevel;
		for (size_t k = i; k < j; ++k) lvl[k] = l;
		i = j;
	}

	// mirror brackets that ended up in RTL context
	std::u32string chars = s;
	for (size_t i = 0; i < n; ++i)
		if (lvl[i] & 1) chars[i] = Mirror(chars[i]);

	// rule L2: reverse runs from the highest level down to 1
	for (int level = 2; level >= 1; --level)
	{
		for (size_t i = 0; i < n;)
		{
			if (lvl[i] < level) { ++i; continue; }
			size_t j = i;
			while (j < n && lvl[j] >= level) ++j;
			std::reverse(chars.begin() + i, chars.begin() + j);
			std::reverse(lvl.begin() + i, lvl.begin() + j);
			i = j;
		}
	}
	return chars;
}

// ---- UTF-8 helpers --------------------------------------------------------------
inline bool DecodeUtf8(const std::string &in, std::u32string &out)
{
	out.clear();
	for (size_t i = 0; i < in.size();)
	{
		unsigned char c = (unsigned char)in[i];
		char32_t cp; int extra;
		if (c < 0x80) { cp = c; extra = 0; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
		else return false;
		for (int k = 1; k <= extra; ++k)
		{
			if (i + k >= in.size()) return false;
			unsigned char d = (unsigned char)in[i + k];
			if ((d & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (d & 0x3F);
		}
		out.push_back(cp);
		i += extra + 1;
	}
	return true;
}

inline void AppendUtf8(std::string &o, char32_t c)
{
	if (c < 0x80) o += (char)c;
	else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
	else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
	else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
}

} // namespace detail

// ---- public API ------------------------------------------------------------------
enum Dir { DIR_AUTO = 0, DIR_LTR = 1, DIR_RTL = 2 };

inline bool ContainsArabic(const std::u32string &s)
{
	for (char32_t c : s)
		if (detail::IsArabicCp(c)) return true;
	return false;
}

// text that already contains presentation forms (U+FB50-FDFF, U+FE70-FEFF) was shaped and
// reordered by someone else (e.g. the old server plugin): it must not be processed again
inline bool HasPresentationForms(const std::u32string &s)
{
	for (char32_t c : s)
		if ((c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return true;
	return false;
}

// logical -> visual (code points)
inline std::u32string ToVisual(const std::u32string &logical, Dir dir = DIR_AUTO)
{
	if (!ContainsArabic(logical) || HasPresentationForms(logical)) return logical;
	bool rtl = (dir == DIR_RTL) || (dir == DIR_AUTO && detail::IsRtlParagraph(logical));
	return detail::Reorder(detail::Shape(logical), rtl);
}

// logical UTF-8 -> visual UTF-8 (invalid UTF-8 is returned unchanged)
inline std::string ToVisualUtf8(const std::string &logical, Dir dir = DIR_AUTO)
{
	std::u32string cps;
	if (!detail::DecodeUtf8(logical, cps) || !ContainsArabic(cps) || HasPresentationForms(cps)) return logical;
	std::u32string v = ToVisual(cps, dir);
	std::string out;
	out.reserve(v.size() * 3);
	for (char32_t c : v) detail::AppendUtf8(out, c);
	return out;
}

// logical wide string (wchar_t, UTF-16 or UTF-32) -> visual wide string
inline std::wstring ToVisualWide(const std::wstring &logical, Dir dir = DIR_AUTO)
{
	std::u32string cps(logical.begin(), logical.end());
	if (!ContainsArabic(cps) || HasPresentationForms(cps)) return logical;
	std::u32string v = ToVisual(cps, dir);
	return std::wstring(v.begin(), v.end());
}

// A chat line looks like "<color>Name: message". The name and the message are separate paragraphs:
// the direction of the message must not depend on the (Latin) name in front of it.
inline std::string ToVisualChatLineUtf8(const std::string &line, const std::string &name)
{
	size_t np = name.empty() ? std::string::npos : line.find(name);
	size_t after = std::string::npos;
	if (np != std::string::npos)
	{
		size_t colon = line.find(':', np + name.size());
		if (colon != std::string::npos) after = colon + 1;
	}
	if (np == std::string::npos || after == std::string::npos)
		return ToVisualUtf8(line); // no "Name:" found: the whole line is one paragraph

	const std::string pre = line.substr(0, np);
	const std::string nm = line.substr(np, name.size());
	const std::string mid = line.substr(np + name.size(), after - (np + name.size())); // up to and including ':'
	std::string body = line.substr(after);

	size_t sp = 0;
	while (sp < body.size() && body[sp] == ' ') ++sp;

	return pre + ToVisualUtf8(nm) + mid + body.substr(0, sp) + ToVisualUtf8(body.substr(sp));
}

inline bool ContainsArabicWide(const std::wstring &s)
{
	std::u32string cps(s.begin(), s.end());
	return ContainsArabic(cps) && !HasPresentationForms(cps);
}

// Splits logical text into lines that fit the given pixel widths, then converts every line to
// visual order. Wrapping on the logical text keeps the reading order of the lines correct.
// measure(const std::wstring &visualLine) -> width in pixels.
template <class MeasureFn>
inline std::vector<std::wstring> WrapToVisualLines(const std::wstring &logical, int firstWidth, int fullWidth, MeasureFn measure)
{
	std::vector<std::wstring> lines;
	if (!ContainsArabicWide(logical))
	{
		lines.push_back(logical);
		return lines;
	}

	std::u32string all(logical.begin(), logical.end());
	const Dir dir = detail::IsRtlParagraph(all) ? DIR_RTL : DIR_LTR;

	std::wstring line;
	int avail = firstWidth;
	size_t i = 0;
	const size_t n = logical.size();
	for (;;)
	{
		size_t j = logical.find(L' ', i);
		if (j == std::wstring::npos) j = n;
		std::wstring word = logical.substr(i, j - i);
		std::wstring cand = line.empty() ? word : line + L" " + word;

		if (line.empty() || measure(ToVisualWide(cand, dir)) <= avail)
			line = cand;
		else
		{
			lines.push_back(ToVisualWide(line, dir));
			line = word;
			avail = fullWidth;
		}
		if (j >= n) break;
		i = j + 1;
	}
	lines.push_back(ToVisualWide(line, dir));
	return lines;
}

// Number of leading neutral characters (spaces, ':', color codes...) before the first letter or digit.
inline size_t LeadingNeutrals(const std::wstring &s)
{
	size_t k = 0;
	while (k < s.size() && detail::Classify((char32_t)s[k]) == detail::C_W) ++k;
	return k;
}

// One colored piece of a chat line -> text ready to insert (visual order, '\n' between wrapped lines).
// used = pixels already used on the current line; it is updated.
template <class MeasureFn>
inline std::wstring LayoutSegment(const std::wstring &logical, int maxWidth, int &used, MeasureFn measure)
{
	if (!ContainsArabicWide(logical))
	{
		used += measure(logical);
		return logical;
	}

	const size_t p = LeadingNeutrals(logical);
	const std::wstring prefix = logical.substr(0, p);
	const std::wstring body = logical.substr(p);
	used += measure(prefix);

	int first = maxWidth - used;
	std::vector<std::wstring> lines = WrapToVisualLines(body, first, maxWidth, measure);

	std::wstring out = prefix;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (i) out += L"\n";
		out += lines[i];
	}
	used = measure(lines.back());
	return out;
}

} // namespace ArabicText
