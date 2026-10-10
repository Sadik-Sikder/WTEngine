// Intl.h
#pragma once

struct JSContext;

// Installs Intl (NumberFormat, DateTimeFormat, Collator, PluralRules,
// RelativeTimeFormat, ListFormat, Segmenter, DisplayNames, Locale) into
// `ctx`, built on Windows' own ICU, and routes Number/BigInt/Date
// toLocaleString, String localeCompare and toLocaleUpperCase/LowerCase
// through it. False (and no Intl) on a Windows without ICU.
bool installIntl(JSContext* ctx);
