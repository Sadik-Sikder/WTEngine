// Intl, on the ICU library that ships with Windows (icu.dll, Windows 10
// 1903 and later) - the same library Chrome builds its Intl on, so dates,
// numbers, plurals and collation come out the same. The natives here do
// the ICU work; kIntlJS (IntlJS.inc) builds the Intl objects on them -
// option handling, resolvedOptions, formatToParts - and routes
// toLocaleString, localeCompare and friends through them.
#include "Intl.h"
#include "quickjs.h"
#include "DevConsole.h"
#include "JSEngine.h"

#define NOMINMAX
#include <windows.h>
#include <icu.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "icu.lib")

namespace {

// --- Strings ----------------------------------------------------------------
// JS strings come in as UTF-8 (JS_ToCString) and go to ICU as UTF-16.

std::string toUtf8(JSContext* ctx, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return {};
    std::string out(s, len);
    JS_FreeCString(ctx, s);
    return out;
}

std::u16string toUtf16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::u16string out(n, u'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), reinterpret_cast<wchar_t*>(out.data()), n);
    return out;
}

std::u16string toUtf16(JSContext* ctx, JSValueConst v) { return toUtf16(toUtf8(ctx, v)); }

std::string utf8(const std::u16string& s) {
    if (s.empty()) return {};
    const wchar_t* w = reinterpret_cast<const wchar_t*>(s.data());
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

JSValue jsString(JSContext* ctx, const UChar* s, int32_t len) {
    if (!s || len <= 0) return JS_NewString(ctx, "");
    int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s), len, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s), len, out.data(), n, nullptr, nullptr);
    return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue jsString(JSContext* ctx, const std::u16string& s) { return jsString(ctx, s.data(), (int32_t)s.size()); }

int toInt(JSContext* ctx, JSValueConst v) {
    int32_t n = 0;
    JS_ToInt32(ctx, &n, v);
    return n;
}

double toDouble(JSContext* ctx, JSValueConst v) {
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return d;
}

// Calls an ICU "fill this buffer" function, growing the buffer if needed.
template <class F>
std::u16string icuString(F fill) {
    std::u16string out(64, u'\0');
    UErrorCode ec = U_ZERO_ERROR;
    int32_t n = fill(out.data(), (int32_t)out.size(), &ec);
    if (ec == U_BUFFER_OVERFLOW_ERROR) {
        out.assign(n + 1, u'\0');
        ec = U_ZERO_ERROR;
        n = fill(out.data(), (int32_t)out.size(), &ec);
    }
    if (U_FAILURE(ec) || n < 0) return {};
    out.resize(n);
    return out;
}

// --- Locales -----------------------------------------------------------------

// A BCP 47 tag ("de-DE-u-co-phonebk") as ICU's locale ID
// ("de_DE@collation=phonebook"), which the C API wants.
std::string icuLocale(const std::string& tag) {
    static std::map<std::string, std::string> cache;
    auto it = cache.find(tag);
    if (it != cache.end()) return it->second;
    char id[ULOC_FULLNAME_CAPACITY] = {};
    UErrorCode ec = U_ZERO_ERROR;
    int32_t parsed = 0;
    uloc_forLanguageTag(tag.c_str(), id, sizeof id, &parsed, &ec);
    std::string out = U_SUCCESS(ec) ? id : "en_US";
    if (cache.size() > 256) cache.clear();
    return cache[tag] = out;
}

// The tag in canonical form, or empty if it isn't a well-formed one.
std::string canonicalTag(const std::string& tag) {
    if (tag.empty()) return {};
    char id[ULOC_FULLNAME_CAPACITY] = {};
    UErrorCode ec = U_ZERO_ERROR;
    int32_t parsed = 0;
    uloc_forLanguageTag(tag.c_str(), id, sizeof id, &parsed, &ec);
    if (U_FAILURE(ec) || parsed != (int32_t)tag.size()) return {};
    char out[ULOC_FULLNAME_CAPACITY] = {};
    ec = U_ZERO_ERROR;
    uloc_toLanguageTag(id, out, sizeof out, true, &ec);
    if (U_FAILURE(ec)) return {};
    return out;
}

// The locales ICU has data for, as BCP 47 tags.
const std::set<std::string>& availableLocales() {
    static const std::set<std::string> all = [] {
        std::set<std::string> s;
        int32_t n = uloc_countAvailable();
        for (int32_t i = 0; i < n; i++) {
            char tag[ULOC_FULLNAME_CAPACITY] = {};
            UErrorCode ec = U_ZERO_ERROR;
            uloc_toLanguageTag(uloc_getAvailable(i), tag, sizeof tag, false, &ec);
            if (U_SUCCESS(ec)) s.insert(tag);
        }
        return s;
    }();
    return all;
}

// --- Cached ICU objects -----------------------------------------------------
// Opening one costs far more than using it, and pages format the same way
// over and over, so each is kept by everything that shapes it. A cache
// that grows past a few hundred (a page building endless variants) starts
// over.

template <class T, void (*Close)(T*)>
class Cache {
public:
    ~Cache() { clear(); }
    T* find(const std::string& key) {
        auto it = items_.find(key);
        return it == items_.end() ? nullptr : it->second;
    }
    T* add(const std::string& key, T* item) {
        if (!item) return nullptr;
        if (items_.size() > 256) clear();
        items_[key] = item;
        return item;
    }

private:
    void clear() {
        for (auto& [key, item] : items_) Close(item);
        items_.clear();
    }
    std::map<std::string, T*> items_;
};

void closeNumber(UNumberFormatter* f) { unumf_close(f); }
void closeNumberRange(UNumberRangeFormatter* f) { unumrf_close(f); }
void closeDate(UDateFormat* f) { udat_close(f); }
void closeInterval(UDateIntervalFormat* f) { udtitvfmt_close(f); }
void closeGenerator(UDateTimePatternGenerator* g) { udatpg_close(g); }
void closePlural(UPluralRules* p) { uplrules_close(p); }
void closeRelative(URelativeDateTimeFormatter* f) { ureldatefmt_close(f); }
void closeList(UListFormatter* f) { ulistfmt_close(f); }
void closeNames(ULocaleDisplayNames* n) { uldn_close(n); }

UNumberFormatter* numberFormatter(const std::string& locale, const std::u16string& skeleton) {
    static Cache<UNumberFormatter, closeNumber> cache;
    std::string key = locale + '|' + utf8(skeleton);
    if (UNumberFormatter* f = cache.find(key)) return f;
    UErrorCode ec = U_ZERO_ERROR;
    UNumberFormatter* f = unumf_openForSkeletonAndLocale(skeleton.data(), (int32_t)skeleton.size(),
                                                         icuLocale(locale).c_str(), &ec);
    if (U_FAILURE(ec)) { if (f) unumf_close(f); return nullptr; }
    return cache.add(key, f);
}

UDateTimePatternGenerator* patternGenerator(const std::string& locale) {
    static Cache<UDateTimePatternGenerator, closeGenerator> cache;
    if (auto* g = cache.find(locale)) return g;
    UErrorCode ec = U_ZERO_ERROR;
    UDateTimePatternGenerator* g = udatpg_open(icuLocale(locale).c_str(), &ec);
    if (U_FAILURE(ec)) { if (g) udatpg_close(g); return nullptr; }
    return cache.add(locale, g);
}

// Dates before the Gregorian calendar's 1582 start are still Gregorian
// in JS (proleptic), not Julian as ICU's default calendar has them.
void makeProleptic(UDateFormat* f) {
    UErrorCode ec = U_ZERO_ERROR;
    UCalendar* cal = ucal_clone(udat_getCalendar(f), &ec);
    if (U_FAILURE(ec) || !cal) return;
    ucal_setGregorianChange(cal, -8.64e15, &ec);
    if (U_SUCCESS(ec)) udat_setCalendar(f, cal);
    ucal_close(cal);
}

UDateFormat* dateFormatter(const std::string& locale, const std::u16string& pattern, const std::u16string& tz) {
    static Cache<UDateFormat, closeDate> cache;
    std::string key = locale + '|' + utf8(pattern) + '|' + utf8(tz);
    if (UDateFormat* f = cache.find(key)) return f;
    UErrorCode ec = U_ZERO_ERROR;
    UDateFormat* f = udat_open(UDAT_PATTERN, UDAT_PATTERN, icuLocale(locale).c_str(), tz.data(), (int32_t)tz.size(),
                               pattern.data(), (int32_t)pattern.size(), &ec);
    if (U_FAILURE(ec)) { if (f) udat_close(f); return nullptr; }
    makeProleptic(f);
    return cache.add(key, f);
}

// --- Formatted results --------------------------------------------------------

// Dates and times as Chrome writes them: newer CLDR data puts a narrow
// no-break space before AM/PM and thin spaces around a range's dash,
// which browsers turn back into plain spaces - pages parse and compare
// these strings. One character for one, so field positions still hold.
void plainSpaces(std::u16string& s) {
    for (auto& c : s) if (c == 0x202F || c == 0x2009) c = u' '; // narrow no-break space, thin space
}

// What formatToParts needs: [string, [category, field, start, limit, ...]]
// - every field ICU marked, nested ones too; kIntlJS turns those into parts.
JSValue formattedValue(JSContext* ctx, const UFormattedValue* value, bool parts, bool date = false) {
    UErrorCode ec = U_ZERO_ERROR;
    int32_t len = 0;
    const UChar* s = ufmtval_getString(value, &len, &ec);
    if (U_FAILURE(ec)) return JS_NULL;
    std::u16string text(s, len);
    if (date) plainSpaces(text);
    JSValue str = jsString(ctx, text);
    if (!parts) return str;

    JSValue spans = JS_NewArray(ctx);
    UConstrainedFieldPosition* pos = ucfpos_open(&ec);
    uint32_t i = 0;
    while (U_SUCCESS(ec) && ufmtval_nextPosition(value, pos, &ec)) {
        int32_t start = 0, limit = 0;
        ucfpos_getIndexes(pos, &start, &limit, &ec);
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, ucfpos_getCategory(pos, &ec)));
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, ucfpos_getField(pos, &ec)));
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, start));
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, limit));
    }
    if (pos) ucfpos_close(pos);
    JSValue out = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, out, 0, str);
    JS_SetPropertyUint32(ctx, out, 1, spans);
    return out;
}

JSValue stringArray(JSContext* ctx, UEnumeration* e) {
    JSValue arr = JS_NewArray(ctx);
    if (!e) return arr;
    UErrorCode ec = U_ZERO_ERROR;
    uint32_t i = 0;
    int32_t len = 0;
    while (const UChar* s = uenum_unext(e, &len, &ec)) {
        if (U_FAILURE(ec)) break;
        JS_SetPropertyUint32(ctx, arr, i++, jsString(ctx, s, len));
    }
    uenum_close(e);
    return arr;
}

// --- Natives -------------------------------------------------------------------

// __wtIntlDefaultLocale() -> the user's locale ("en-US").
JSValue js_defaultLocale(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    static const std::string tag = [] {
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
        std::string s = "en-US";
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
            char utf8[LOCALE_NAME_MAX_LENGTH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, nullptr, nullptr);
            std::string c = canonicalTag(utf8);
            if (!c.empty()) s = c;
        }
        return s;
    }();
    return JS_NewString(ctx, tag.c_str());
}

// __wtIntlCanonical(tag) -> the canonical tag, or null if it's malformed.
JSValue js_canonical(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::string c = canonicalTag(toUtf8(ctx, argv[0]));
    return c.empty() ? JS_NULL : JS_NewString(ctx, c.c_str());
}

// __wtIntlAvailable(tag) -> the closest locale ICU has data for (the tag
// with subtags dropped from the end until one matches), or "". ICU lists
// some locales only in full - zh-Hant-TW, not zh-TW - so a tag whose
// likely full form is listed counts as itself.
JSValue js_available(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::string tag = toUtf8(ctx, argv[0]);
    const auto& all = availableLocales();
    if (!all.count(tag)) {
        char full[ULOC_FULLNAME_CAPACITY] = {}, fullTag[ULOC_FULLNAME_CAPACITY] = {};
        UErrorCode ec = U_ZERO_ERROR;
        uloc_addLikelySubtags(icuLocale(tag).c_str(), full, sizeof full, &ec);
        uloc_toLanguageTag(full, fullTag, sizeof fullTag, false, &ec);
        if (U_SUCCESS(ec) && all.count(fullTag)) return JS_NewString(ctx, tag.c_str());
    }
    while (!tag.empty()) {
        if (all.count(tag)) return JS_NewString(ctx, tag.c_str());
        size_t dash = tag.rfind('-');
        if (dash == std::string::npos) break;
        tag.resize(dash);
    }
    return JS_NewString(ctx, "");
}

// __wtIntlLikely(tag, maximize) -> Locale.prototype.maximize()/minimize().
JSValue js_likely(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::string id = icuLocale(toUtf8(ctx, argv[0]));
    char out[ULOC_FULLNAME_CAPACITY] = {}, tag[ULOC_FULLNAME_CAPACITY] = {};
    UErrorCode ec = U_ZERO_ERROR;
    if (JS_ToBool(ctx, argv[1])) uloc_addLikelySubtags(id.c_str(), out, sizeof out, &ec);
    else uloc_minimizeSubtags(id.c_str(), out, sizeof out, &ec);
    if (U_FAILURE(ec)) return JS_DupValue(ctx, argv[0]);
    uloc_toLanguageTag(out, tag, sizeof tag, false, &ec);
    return U_SUCCESS(ec) ? JS_NewString(ctx, tag) : JS_DupValue(ctx, argv[0]);
}

// __wtIntlNumberingSystem(locale) -> "latn", "arab", ...
JSValue js_numberingSystem(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    UErrorCode ec = U_ZERO_ERROR;
    UNumberingSystem* ns = unumsys_open(icuLocale(toUtf8(ctx, argv[0])).c_str(), &ec);
    JSValue out = JS_NewString(ctx, U_SUCCESS(ec) && ns ? unumsys_getName(ns) : "latn");
    if (ns) unumsys_close(ns);
    return out;
}

// __wtIntlCurrencyDigits(code) -> the currency's usual fraction digits.
JSValue js_currencyDigits(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::u16string code = toUtf16(ctx, argv[0]);
    UErrorCode ec = U_ZERO_ERROR;
    int32_t digits = ucurr_getDefaultFractionDigits(code.c_str(), &ec);
    return JS_NewInt32(ctx, U_SUCCESS(ec) ? digits : 2);
}

// __wtIntlNumber(locale, skeleton, value, parts) -> the formatted number
// (or [string, spans]); null if ICU refused the skeleton. A BigInt or a
// numeric string is formatted exactly.
JSValue js_number(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    UNumberFormatter* f = numberFormatter(toUtf8(ctx, argv[0]), toUtf16(ctx, argv[1]));
    if (!f) return JS_NULL;
    UErrorCode ec = U_ZERO_ERROR;
    UFormattedNumber* result = unumf_openResult(&ec);
    if (JS_IsNumber(argv[2])) {
        unumf_formatDouble(f, toDouble(ctx, argv[2]), result, &ec);
    }
    else {
        std::string digits = toUtf8(ctx, argv[2]);
        unumf_formatDecimal(f, digits.c_str(), (int32_t)digits.size(), result, &ec);
    }
    JSValue out = U_SUCCESS(ec) ? formattedValue(ctx, unumf_resultAsValue(result, &ec), JS_ToBool(ctx, argv[3])) : JS_NULL;
    unumf_closeResult(result);
    return out;
}

// __wtIntlNumberRange(locale, skeleton, start, end, parts).
JSValue js_numberRange(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    static Cache<UNumberRangeFormatter, closeNumberRange> cache;
    std::string locale = toUtf8(ctx, argv[0]);
    std::u16string skeleton = toUtf16(ctx, argv[1]);
    std::string key = locale + '|' + utf8(skeleton);
    UNumberRangeFormatter* f = cache.find(key);
    UErrorCode ec = U_ZERO_ERROR;
    if (!f) {
        f = unumrf_openForSkeletonWithCollapseAndIdentityFallback(
            skeleton.data(), (int32_t)skeleton.size(), UNUM_RANGE_COLLAPSE_AUTO, UNUM_IDENTITY_FALLBACK_APPROXIMATELY,
            icuLocale(locale).c_str(), nullptr, &ec);
        if (U_FAILURE(ec)) { if (f) unumrf_close(f); return JS_NULL; }
        cache.add(key, f);
    }
    UFormattedNumberRange* result = unumrf_openResult(&ec);
    unumrf_formatDoubleRange(f, toDouble(ctx, argv[2]), toDouble(ctx, argv[3]), result, &ec);
    JSValue out = U_SUCCESS(ec) ? formattedValue(ctx, unumrf_resultAsValue(result, &ec), JS_ToBool(ctx, argv[4])) : JS_NULL;
    unumrf_closeResult(result);
    return out;
}

// __wtIntlDatePattern(locale, skeleton) -> the locale's pattern for those
// fields ("yMMMd" -> "MMM d, y" in English).
JSValue js_datePattern(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    UDateTimePatternGenerator* g = patternGenerator(toUtf8(ctx, argv[0]));
    if (!g) return JS_NULL;
    std::u16string skeleton = toUtf16(ctx, argv[1]);
    std::u16string pattern = icuString([&](UChar* buf, int32_t cap, UErrorCode* ec) {
        return udatpg_getBestPatternWithOptions(g, skeleton.data(), (int32_t)skeleton.size(),
                                                UDATPG_MATCH_HOUR_FIELD_LENGTH, buf, cap, ec);
    });
    return jsString(ctx, pattern);
}

// __wtIntlDateStylePattern(locale, dateStyle, timeStyle) -> the pattern
// for dateStyle/timeStyle (0 full .. 3 short, -1 none).
JSValue js_dateStylePattern(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    UErrorCode ec = U_ZERO_ERROR;
    UDateFormat* f = udat_open((UDateFormatStyle)toInt(ctx, argv[2]), (UDateFormatStyle)toInt(ctx, argv[1]),
                               icuLocale(toUtf8(ctx, argv[0])).c_str(), nullptr, -1, nullptr, -1, &ec);
    if (U_FAILURE(ec)) { if (f) udat_close(f); return JS_NULL; }
    std::u16string pattern = icuString([&](UChar* buf, int32_t cap, UErrorCode* e) {
        return udat_toPattern(f, false, buf, cap, e);
    });
    udat_close(f);
    return jsString(ctx, pattern);
}

// __wtIntlSkeleton(pattern) -> the fields a pattern shows, as a skeleton
// ("h:mm a" -> "hmma").
JSValue js_skeleton(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::u16string pattern = toUtf16(ctx, argv[0]);
    return jsString(ctx, icuString([&](UChar* buf, int32_t cap, UErrorCode* ec) {
        return udatpg_getSkeleton(nullptr, pattern.data(), (int32_t)pattern.size(), buf, cap, ec);
    }));
}

// __wtIntlTimeZone(name) -> the canonical time zone ("" = the system's),
// or null if there's no such zone.
JSValue js_timeZone(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::u16string name = toUtf16(ctx, argv[0]);
    std::u16string id;
    if (name.empty()) {
        id = icuString([](UChar* buf, int32_t cap, UErrorCode* ec) { return ucal_getDefaultTimeZone(buf, cap, ec); });
    }
    else {
        // Zone names match whatever their case ("america/new_york"); ICU's
        // lookup doesn't, so the name is first found in its list.
        static const std::map<std::u16string, std::u16string> byLowerName = [] {
            std::map<std::u16string, std::u16string> m;
            UErrorCode ec = U_ZERO_ERROR;
            UEnumeration* e = ucal_openTimeZones(&ec);
            int32_t len = 0;
            while (e && U_SUCCESS(ec)) {
                const UChar* z = uenum_unext(e, &len, &ec);
                if (!z) break;
                std::u16string zone(z, len), lower = zone;
                for (auto& c : lower) if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a';
                m.emplace(lower, zone);
            }
            if (e) uenum_close(e);
            return m;
        }();
        for (auto& c : name) if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a';
        auto found = byLowerName.find(name);
        if (found == byLowerName.end()) return JS_NULL;
        UBool system = false;
        id = icuString([&](UChar* buf, int32_t cap, UErrorCode* ec) {
            return ucal_getCanonicalTimeZoneID(found->second.data(), (int32_t)found->second.size(), buf, cap, &system, ec);
        });
        if (!system) id.clear();
    }
    if (id.empty()) return JS_NULL;
    // JS calls UTC "UTC" whichever of its aliases was asked for.
    static const char16_t* kUtc[] = { u"Etc/UTC", u"Etc/GMT", u"Etc/UCT", u"Etc/Universal", u"Etc/Zulu",
                                      u"Etc/Greenwich", u"GMT", u"UCT", u"UTC" };
    for (const char16_t* u : kUtc) if (id == u) return JS_NewString(ctx, "UTC");
    return jsString(ctx, id);
}

// __wtIntlDateFormat(locale, pattern, timeZone, time, parts).
JSValue js_dateFormat(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    UDateFormat* f = dateFormatter(toUtf8(ctx, argv[0]), toUtf16(ctx, argv[1]), toUtf16(ctx, argv[2]));
    if (!f) return JS_NULL;
    double time = toDouble(ctx, argv[3]);
    if (!JS_ToBool(ctx, argv[4])) {
        std::u16string s = icuString([&](UChar* buf, int32_t cap, UErrorCode* ec) {
            return udat_format(f, time, buf, cap, nullptr, ec);
        });
        plainSpaces(s);
        return jsString(ctx, s);
    }
    // Parts: the date fields, as spans of category 1 (date).
    UErrorCode ec = U_ZERO_ERROR;
    UFieldPositionIterator* it = ufieldpositer_open(&ec);
    std::u16string s(128, u'\0');
    int32_t n = udat_formatForFields(f, time, s.data(), (int32_t)s.size(), it, &ec);
    if (ec == U_BUFFER_OVERFLOW_ERROR) {
        ufieldpositer_close(it);
        ec = U_ZERO_ERROR;
        it = ufieldpositer_open(&ec);
        s.assign(n + 1, u'\0');
        n = udat_formatForFields(f, time, s.data(), (int32_t)s.size(), it, &ec);
    }
    if (U_FAILURE(ec)) { ufieldpositer_close(it); return JS_NULL; }
    s.resize(n);
    plainSpaces(s);
    JSValue spans = JS_NewArray(ctx);
    uint32_t i = 0;
    int32_t start = 0, limit = 0, field;
    while ((field = ufieldpositer_next(it, &start, &limit)) >= 0) {
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, UFIELD_CATEGORY_DATE));
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, field));
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, start));
        JS_SetPropertyUint32(ctx, spans, i++, JS_NewInt32(ctx, limit));
    }
    ufieldpositer_close(it);
    JSValue out = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, out, 0, jsString(ctx, s));
    JS_SetPropertyUint32(ctx, out, 1, spans);
    return out;
}

// __wtIntlDateRange(locale, pattern, timeZone, start, end, parts).
JSValue js_dateRange(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    static Cache<UDateIntervalFormat, closeInterval> cache;
    std::string locale = toUtf8(ctx, argv[0]);
    std::u16string pattern = toUtf16(ctx, argv[1]), tz = toUtf16(ctx, argv[2]);
    std::string key = locale + '|' + utf8(pattern) + '|' + utf8(tz);
    UDateIntervalFormat* f = cache.find(key);
    UErrorCode ec = U_ZERO_ERROR;
    if (!f) {
        // Interval formats are chosen by skeleton: the pattern's fields.
        std::u16string skeleton = icuString([&](UChar* buf, int32_t cap, UErrorCode* e) {
            return udatpg_getSkeleton(nullptr, pattern.data(), (int32_t)pattern.size(), buf, cap, e);
        });
        f = udtitvfmt_open(icuLocale(locale).c_str(), skeleton.data(), (int32_t)skeleton.size(), tz.data(),
                           (int32_t)tz.size(), &ec);
        if (U_FAILURE(ec)) { if (f) udtitvfmt_close(f); return JS_NULL; }
        cache.add(key, f);
    }
    UFormattedDateInterval* result = udtitvfmt_openResult(&ec);
    udtitvfmt_formatToResult(f, toDouble(ctx, argv[3]), toDouble(ctx, argv[4]), result, &ec);
    JSValue out = U_SUCCESS(ec) ? formattedValue(ctx, udtitvfmt_resultAsValue(result, &ec), JS_ToBool(ctx, argv[5]), true) : JS_NULL;
    udtitvfmt_closeResult(result);
    return out;
}

// __wtIntlCollator(locale, options) -> a handle for __wtIntlCompare, one
// per locale and options for the life of the process. `options` is
// kIntlJS's compact form: sensitivity (b/a/c/v), numeric (0/1),
// ignorePunctuation (0/1), caseFirst (u/l/f, or d for the locale's own).
std::vector<UCollator*>& collators() {
    static std::vector<UCollator*> all;
    return all;
}

JSValue js_collator(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    static std::map<std::string, int> ids;
    std::string locale = toUtf8(ctx, argv[0]), options = toUtf8(ctx, argv[1]);
    std::string key = locale + '|' + options;
    auto it = ids.find(key);
    if (it != ids.end()) return JS_NewInt32(ctx, it->second);
    UErrorCode ec = U_ZERO_ERROR;
    UCollator* c = ucol_open(icuLocale(locale).c_str(), &ec);
    if (U_FAILURE(ec)) { if (c) ucol_close(c); return JS_NewInt32(ctx, -1); }
    char sensitivity = options.size() > 0 ? options[0] : 'v';
    ucol_setAttribute(c, UCOL_STRENGTH, sensitivity == 'b' || sensitivity == 'c' ? UCOL_PRIMARY
                                         : sensitivity == 'a' ? UCOL_SECONDARY : UCOL_TERTIARY, &ec);
    if (sensitivity == 'c') ucol_setAttribute(c, UCOL_CASE_LEVEL, UCOL_ON, &ec);
    if (options.size() > 1 && options[1] == '1') ucol_setAttribute(c, UCOL_NUMERIC_COLLATION, UCOL_ON, &ec);
    if (options.size() > 2 && options[2] == '1') ucol_setAttribute(c, UCOL_ALTERNATE_HANDLING, UCOL_SHIFTED, &ec);
    if (options.size() > 3 && options[3] != 'd')
        ucol_setAttribute(c, UCOL_CASE_FIRST, options[3] == 'u' ? UCOL_UPPER_FIRST
                                              : options[3] == 'l' ? UCOL_LOWER_FIRST : UCOL_OFF, &ec);
    collators().push_back(c);
    int id = (int)collators().size() - 1;
    ids[key] = id;
    return JS_NewInt32(ctx, id);
}

// __wtIntlCompare(collator, a, b) -> -1, 0 or 1. Called once per pair
// when a page sorts, so the strings go to ICU as UTF-8, unconverted.
JSValue js_compare(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    int id = toInt(ctx, argv[0]);
    if (id < 0 || id >= (int)collators().size()) return JS_NewInt32(ctx, 0);
    size_t la = 0, lb = 0;
    const char* a = JS_ToCStringLen(ctx, &la, argv[1]);
    const char* b = JS_ToCStringLen(ctx, &lb, argv[2]);
    int out = 0;
    if (a && b) {
        UErrorCode ec = U_ZERO_ERROR;
        UCollationResult r = ucol_strcollUTF8(collators()[id], a, (int32_t)la, b, (int32_t)lb, &ec);
        out = r == UCOL_LESS ? -1 : r == UCOL_GREATER ? 1 : 0;
    }
    if (a) JS_FreeCString(ctx, a);
    if (b) JS_FreeCString(ctx, b);
    return JS_NewInt32(ctx, out);
}

UPluralRules* pluralRules(const std::string& locale, bool ordinal) {
    static Cache<UPluralRules, closePlural> cache;
    std::string key = locale + (ordinal ? "|o" : "|c");
    if (UPluralRules* p = cache.find(key)) return p;
    UErrorCode ec = U_ZERO_ERROR;
    UPluralRules* p = uplrules_openForType(icuLocale(locale).c_str(), ordinal ? UPLURAL_TYPE_ORDINAL : UPLURAL_TYPE_CARDINAL, &ec);
    if (U_FAILURE(ec)) { if (p) uplrules_close(p); return nullptr; }
    return cache.add(key, p);
}

// __wtIntlPlural(locale, ordinal, skeleton, n) -> "one", "other", ... The
// number is first rounded as the skeleton says ("1.0" can be "other").
JSValue js_plural(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::string locale = toUtf8(ctx, argv[0]);
    UPluralRules* p = pluralRules(locale, JS_ToBool(ctx, argv[1]));
    UNumberFormatter* f = numberFormatter(locale, toUtf16(ctx, argv[2]));
    if (!p || !f) return JS_NewString(ctx, "other");
    UErrorCode ec = U_ZERO_ERROR;
    UFormattedNumber* number = unumf_openResult(&ec);
    unumf_formatDouble(f, toDouble(ctx, argv[3]), number, &ec);
    std::u16string keyword = icuString([&](UChar* buf, int32_t cap, UErrorCode* e) {
        return uplrules_selectFormatted(p, number, buf, cap, e);
    });
    unumf_closeResult(number);
    return keyword.empty() ? JS_NewString(ctx, "other") : jsString(ctx, keyword);
}

// __wtIntlPluralCategories(locale, ordinal) -> the categories it uses.
JSValue js_pluralCategories(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    UPluralRules* p = pluralRules(toUtf8(ctx, argv[0]), JS_ToBool(ctx, argv[1]));
    if (!p) return JS_NewArray(ctx);
    UErrorCode ec = U_ZERO_ERROR;
    return stringArray(ctx, uplrules_getKeywords(p, &ec));
}

// __wtIntlRelative(locale, style, numericAlways, value, unit, parts):
// "in 3 days", "yesterday". style 0 long, 1 short, 2 narrow; unit 0 year,
// 1 quarter, 2 month, 3 week, 4 day, 5 hour, 6 minute, 7 second.
JSValue js_relative(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    static Cache<URelativeDateTimeFormatter, closeRelative> cache;
    std::string locale = toUtf8(ctx, argv[0]);
    int style = toInt(ctx, argv[1]);
    std::string key = locale + '|' + std::to_string(style);
    URelativeDateTimeFormatter* f = cache.find(key);
    UErrorCode ec = U_ZERO_ERROR;
    if (!f) {
        f = ureldatefmt_open(icuLocale(locale).c_str(), nullptr, (UDateRelativeDateTimeFormatterStyle)style,
                             UDISPCTX_CAPITALIZATION_NONE, &ec);
        if (U_FAILURE(ec)) { if (f) ureldatefmt_close(f); return JS_NULL; }
        cache.add(key, f);
    }
    UFormattedRelativeDateTime* result = ureldatefmt_openResult(&ec);
    double value = toDouble(ctx, argv[3]);
    auto unit = (URelativeDateTimeUnit)toInt(ctx, argv[4]);
    if (JS_ToBool(ctx, argv[2])) ureldatefmt_formatNumericToResult(f, value, unit, result, &ec);
    else ureldatefmt_formatToResult(f, value, unit, result, &ec);
    JSValue out = U_SUCCESS(ec) ? formattedValue(ctx, ureldatefmt_resultAsValue(result, &ec), JS_ToBool(ctx, argv[5])) : JS_NULL;
    ureldatefmt_closeResult(result);
    return out;
}

// __wtIntlList(locale, type, width, items, parts): "a, b, and c". type 0
// conjunction, 1 disjunction, 2 unit; width 0 long, 1 short, 2 narrow.
JSValue js_list(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    static Cache<UListFormatter, closeList> cache;
    std::string locale = toUtf8(ctx, argv[0]);
    int type = toInt(ctx, argv[1]), width = toInt(ctx, argv[2]);
    std::string key = locale + '|' + std::to_string(type) + std::to_string(width);
    UListFormatter* f = cache.find(key);
    UErrorCode ec = U_ZERO_ERROR;
    if (!f) {
        f = ulistfmt_openForType(icuLocale(locale).c_str(), (UListFormatterType)type, (UListFormatterWidth)width, &ec);
        if (U_FAILURE(ec)) { if (f) ulistfmt_close(f); return JS_NULL; }
        cache.add(key, f);
    }
    int64_t count = 0;
    JS_GetLength(ctx, argv[3], &count);
    std::vector<std::u16string> items((size_t)count);
    std::vector<const UChar*> ptrs((size_t)count);
    std::vector<int32_t> lens((size_t)count);
    for (int64_t i = 0; i < count; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[3], (uint32_t)i);
        items[i] = toUtf16(ctx, item);
        JS_FreeValue(ctx, item);
        ptrs[i] = items[i].c_str();
        lens[i] = (int32_t)items[i].size();
    }
    UFormattedList* result = ulistfmt_openResult(&ec);
    ulistfmt_formatStringsToResult(f, ptrs.data(), lens.data(), (int32_t)count, result, &ec);
    JSValue out = U_SUCCESS(ec) ? formattedValue(ctx, ulistfmt_resultAsValue(result, &ec), JS_ToBool(ctx, argv[4])) : JS_NULL;
    ulistfmt_closeResult(result);
    return out;
}

// __wtIntlSegment(locale, granularity, text) -> [end, ruleStatus, end, ...]
// for each segment in turn (granularity 0 grapheme, 1 word, 2 sentence).
JSValue js_segment(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::u16string text = toUtf16(ctx, argv[2]);
    int granularity = toInt(ctx, argv[1]);
    UBreakIteratorType type = granularity == 1 ? UBRK_WORD : granularity == 2 ? UBRK_SENTENCE : UBRK_CHARACTER;
    UErrorCode ec = U_ZERO_ERROR;
    UBreakIterator* bi = ubrk_open(type, icuLocale(toUtf8(ctx, argv[0])).c_str(), text.data(), (int32_t)text.size(), &ec);
    JSValue out = JS_NewArray(ctx);
    if (U_FAILURE(ec)) { if (bi) ubrk_close(bi); return out; }
    uint32_t i = 0;
    for (int32_t end = ubrk_next(bi); end != UBRK_DONE; end = ubrk_next(bi)) {
        JS_SetPropertyUint32(ctx, out, i++, JS_NewInt32(ctx, end));
        JS_SetPropertyUint32(ctx, out, i++, JS_NewInt32(ctx, ubrk_getRuleStatus(bi)));
    }
    ubrk_close(bi);
    return out;
}

// __wtIntlDisplayName(locale, type, style, dialect, code) -> the name of a
// language, region, script, currency, calendar or date field in `locale`,
// or undefined if ICU has none. style 0 long, 1 short, 2 narrow.
JSValue js_displayName(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::string locale = toUtf8(ctx, argv[0]), type = toUtf8(ctx, argv[1]), code = toUtf8(ctx, argv[4]);
    int style = toInt(ctx, argv[2]);
    bool dialect = JS_ToBool(ctx, argv[3]);
    std::u16string name;

    if (type == "currency") {
        std::u16string currency = toUtf16(code);
        UBool choice = false;
        int32_t len = 0;
        UErrorCode ec = U_ZERO_ERROR;
        UCurrNameStyle s = style == 0 ? UCURR_LONG_NAME : style == 1 ? UCURR_SYMBOL_NAME : UCURR_NARROW_SYMBOL_NAME;
        const UChar* n = ucurr_getName(currency.c_str(), icuLocale(locale).c_str(), s, &choice, &len, &ec);
        if (U_SUCCESS(ec) && ec != U_USING_DEFAULT_WARNING && n) name.assign(n, len);
    }
    else if (type == "dateTimeField") {
        static const char* kFields[] = { "era", "year", "quarter", "month", "weekOfYear", "weekOfMonth", "weekday",
                                         "dayOfYear", "dayOfWeekInMonth", "day", "dayPeriod", "hour", "minute",
                                         "second", "fractionalSecond", "timeZoneName" };
        int field = -1;
        for (int i = 0; i < 16; i++) if (code == kFields[i]) field = i;
        static const UDateTimePatternField kIcu[] = { // ICU's field for each of kFields
            UDATPG_ERA_FIELD, UDATPG_YEAR_FIELD, UDATPG_QUARTER_FIELD, UDATPG_MONTH_FIELD, UDATPG_WEEK_OF_YEAR_FIELD,
            UDATPG_WEEK_OF_MONTH_FIELD, UDATPG_WEEKDAY_FIELD, UDATPG_DAY_OF_YEAR_FIELD,
            UDATPG_DAY_OF_WEEK_IN_MONTH_FIELD, UDATPG_DAY_FIELD, UDATPG_DAYPERIOD_FIELD, UDATPG_HOUR_FIELD,
            UDATPG_MINUTE_FIELD, UDATPG_SECOND_FIELD, UDATPG_FRACTIONAL_SECOND_FIELD, UDATPG_ZONE_FIELD };
        UDateTimePatternGenerator* g = field >= 0 ? patternGenerator(locale) : nullptr;
        if (g) {
            name = icuString([&](UChar* buf, int32_t cap, UErrorCode* ec) {
                return udatpg_getFieldDisplayName(g, kIcu[field], style == 0 ? UDATPG_WIDE : style == 1 ? UDATPG_ABBREVIATED
                                                  : UDATPG_NARROW, buf, cap, ec);
            });
        }
    }
    else {
        static Cache<ULocaleDisplayNames, closeNames> cache;
        std::string key = locale + '|' + std::to_string(style) + (dialect ? "d" : "s");
        ULocaleDisplayNames* ldn = cache.find(key);
        if (!ldn) {
            UDisplayContext contexts[] = { dialect ? UDISPCTX_DIALECT_NAMES : UDISPCTX_STANDARD_NAMES,
                                           style == 0 ? UDISPCTX_LENGTH_FULL : UDISPCTX_LENGTH_SHORT,
                                           UDISPCTX_NO_SUBSTITUTE };
            UErrorCode ec = U_ZERO_ERROR;
            ldn = uldn_openForContext(icuLocale(locale).c_str(), contexts, 3, &ec);
            if (U_FAILURE(ec)) { if (ldn) uldn_close(ldn); return JS_UNDEFINED; }
            cache.add(key, ldn);
        }
        name = icuString([&](UChar* buf, int32_t cap, UErrorCode* ec) -> int32_t {
            if (type == "language") return uldn_localeDisplayName(ldn, icuLocale(code).c_str(), buf, cap, ec);
            if (type == "region") return uldn_regionDisplayName(ldn, code.c_str(), buf, cap, ec);
            if (type == "script") return uldn_scriptDisplayName(ldn, code.c_str(), buf, cap, ec);
            if (type == "calendar") return uldn_keyValueDisplayName(ldn, "calendar", code.c_str(), buf, cap, ec);
            *ec = U_ILLEGAL_ARGUMENT_ERROR;
            return 0;
        });
    }
    return name.empty() ? JS_UNDEFINED : jsString(ctx, name);
}

// __wtIntlSupported(key) -> Intl.supportedValuesOf(key)'s list, unsorted.
JSValue js_supported(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::string key = toUtf8(ctx, argv[0]);
    UErrorCode ec = U_ZERO_ERROR;
    if (key == "timeZone") return stringArray(ctx, ucal_openTimeZones(&ec));
    if (key == "currency") return stringArray(ctx, ucurr_openISOCurrencies(UCURR_COMMON | UCURR_NON_DEPRECATED, &ec));
    if (key == "numberingSystem") {
        // Only the ones with digits of their own, as in browsers - not the
        // algorithmic ones (Roman numerals, Armenian, ...).
        JSValue all = stringArray(ctx, unumsys_openAvailableNames(&ec));
        JSValue out = JS_NewArray(ctx);
        int64_t n = 0;
        JS_GetLength(ctx, all, &n);
        uint32_t k = 0;
        for (int64_t i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, all, (uint32_t)i);
            std::string name = toUtf8(ctx, v);
            UErrorCode e2 = U_ZERO_ERROR;
            UNumberingSystem* ns = unumsys_openByName(name.c_str(), &e2);
            if (U_SUCCESS(e2) && ns && !unumsys_isAlgorithmic(ns)) JS_SetPropertyUint32(ctx, out, k++, v);
            else JS_FreeValue(ctx, v);
            if (ns) unumsys_close(ns);
        }
        JS_FreeValue(ctx, all);
        return out;
    }
    if (key == "collation") return stringArray(ctx, ucol_getKeywordValues("collation", &ec)); // ICU's names: kIntlJS maps them
    if (key == "calendar") return stringArray(ctx, ucal_getKeywordValuesForLocale("calendar", "und", false, &ec)); // every calendar
    return JS_NewArray(ctx);
}

// __wtIntlCase(text, locale, upper) -> toLocaleUpperCase/toLocaleLowerCase
// (Turkish dotted i and the like).
JSValue js_case(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    std::u16string text = toUtf16(ctx, argv[0]);
    std::string locale = icuLocale(toUtf8(ctx, argv[1]));
    bool upper = JS_ToBool(ctx, argv[2]);
    std::u16string out = icuString([&](UChar* buf, int32_t cap, UErrorCode* ec) {
        return upper ? u_strToUpper(buf, cap, text.data(), (int32_t)text.size(), locale.c_str(), ec)
                     : u_strToLower(buf, cap, text.data(), (int32_t)text.size(), locale.c_str(), ec);
    });
    return jsString(ctx, out);
}

const JSCFunctionListEntry kNatives[] = {
    JS_CFUNC_DEF("__wtIntlDefaultLocale", 0, js_defaultLocale),
    JS_CFUNC_DEF("__wtIntlCanonical", 1, js_canonical),
    JS_CFUNC_DEF("__wtIntlAvailable", 1, js_available),
    JS_CFUNC_DEF("__wtIntlLikely", 2, js_likely),
    JS_CFUNC_DEF("__wtIntlNumberingSystem", 1, js_numberingSystem),
    JS_CFUNC_DEF("__wtIntlCurrencyDigits", 1, js_currencyDigits),
    JS_CFUNC_DEF("__wtIntlNumber", 4, js_number),
    JS_CFUNC_DEF("__wtIntlNumberRange", 5, js_numberRange),
    JS_CFUNC_DEF("__wtIntlDatePattern", 2, js_datePattern),
    JS_CFUNC_DEF("__wtIntlDateStylePattern", 3, js_dateStylePattern),
    JS_CFUNC_DEF("__wtIntlSkeleton", 1, js_skeleton),
    JS_CFUNC_DEF("__wtIntlTimeZone", 1, js_timeZone),
    JS_CFUNC_DEF("__wtIntlDateFormat", 5, js_dateFormat),
    JS_CFUNC_DEF("__wtIntlDateRange", 6, js_dateRange),
    JS_CFUNC_DEF("__wtIntlCollator", 2, js_collator),
    JS_CFUNC_DEF("__wtIntlCompare", 3, js_compare),
    JS_CFUNC_DEF("__wtIntlPlural", 4, js_plural),
    JS_CFUNC_DEF("__wtIntlPluralCategories", 2, js_pluralCategories),
    JS_CFUNC_DEF("__wtIntlRelative", 6, js_relative),
    JS_CFUNC_DEF("__wtIntlList", 5, js_list),
    JS_CFUNC_DEF("__wtIntlSegment", 3, js_segment),
    JS_CFUNC_DEF("__wtIntlDisplayName", 5, js_displayName),
    JS_CFUNC_DEF("__wtIntlSupported", 1, js_supported),
    JS_CFUNC_DEF("__wtIntlCase", 3, js_case),
};

const char kIntlJS[] =
#include "IntlJS.inc"
    ;

// icu.dll is delay-loaded: on a Windows without it (before 10 1903) pages
// just get no Intl, instead of the browser failing to start.
bool icuAvailable() {
    static const bool available = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
    return available;
}

} // namespace

bool installIntl(JSContext* ctx) {
    if (!icuAvailable()) return false;
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyFunctionList(ctx, global, kNatives, sizeof(kNatives) / sizeof(kNatives[0]));
    JS_FreeValue(ctx, global);
    ArmScriptWatchdog(ctx); // an entry into JS like any other (JSEngine.h)
    JSValue r = JS_Eval(ctx, kIntlJS, sizeof(kIntlJS) - 1, "<intl>", JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(r);
    if (!ok) { // a bug in kIntlJS: say so in the developer console
        JSValue e = JS_GetException(ctx);
        std::string what = toUtf8(ctx, e);
        std::u16string text = toUtf16("Intl setup failed: " + what);
        consoleLog().add(LogLevel::Error, L"script", std::wstring(text.begin(), text.end()));
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    return ok;
}
