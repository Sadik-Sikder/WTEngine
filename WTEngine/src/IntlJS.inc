// The JS half of Intl (Intl.cpp has the ICU half and includes this as
// kIntlJS). It's split into raw-string pieces, each under MSVC's limit for
// one string literal.
R"JS(
(() => {
  'use strict';
  const g = globalThis;

  // --- Helpers -------------------------------------------------------------

  const hidden = (obj, props) => {
    for (const k of Reflect.ownKeys(props))
      Object.defineProperty(obj, k, { value: props[k], writable: true, configurable: true });
  };
  const getters = (obj, props) => {
    for (const k of Object.keys(props))
      Object.defineProperty(obj, k, { get: props[k], configurable: true });
  };
  const tag = (proto, name) =>
    Object.defineProperty(proto, Symbol.toStringTag, { value: name, configurable: true });

  // Options as the spec reads them: undefined is an empty object, anything
  // else must be an object.
  const toOptions = options => {
    if (options === undefined) return Object.create(null);
    if (options === null) throw new TypeError('Cannot convert undefined or null to object');
    return Object(options);
  };
  const getOption = (options, prop, type, values, fallback, ctor) => {
    let v = options[prop];
    if (v === undefined) return fallback;
    v = type === 'boolean' ? Boolean(v) : String(v);
    if (values && !values.includes(v))
      throw new RangeError('Value ' + v + ' out of range for ' + ctor + ' options property ' + prop);
    return v;
  };
  const numberOption = (options, prop, min, max, fallback) => {
    const v = options[prop];
    return defaultNumber(v, min, max, fallback, prop);
  };
  const defaultNumber = (v, min, max, fallback, prop) => {
    if (v === undefined) return fallback;
    const n = Number(v);
    if (Number.isNaN(n) || n < min || n > max) throw new RangeError(prop + ' value is out of range.');
    return Math.floor(n);
  };

  // Each Intl object's internal slots, out of the page's sight.
  const slots = new WeakMap();
  const slotOf = (obj, kind, method) => {
    const s = obj !== null && typeof obj === 'object' ? slots.get(obj) : undefined;
    if (!s || s.kind !== kind)
      throw new TypeError('Method Intl.' + kind + '.prototype.' + method + ' called on incompatible receiver ' + String(obj));
    return s;
  };

  // --- Locales -------------------------------------------------------------

  const defaultLocale = __wtIntlDefaultLocale();

  const canonicalizeLocaleList = locales => {
    if (locales === undefined) return [];
    const out = [];
    const list = typeof locales === 'string' || (locales && slots.get(locales) && slots.get(locales).kind === 'Locale')
      ? [locales] : Object(locales);
    const len = Math.min(Number(list.length) >>> 0, 2 ** 32 - 1);
    for (let k = 0; k < len; k++) {
      if (!(k in list)) continue;
      const v = list[k];
      if (typeof v !== 'string' && (typeof v !== 'object' || v === null))
        throw new TypeError('Language ID should be string or object.');
      const s = slots.get(v);
      const t = s && s.kind === 'Locale' ? s.tag : String(v);
      const c = __wtIntlCanonical(t);
      if (c === null) throw new RangeError('Invalid language tag: ' + t);
      if (!out.includes(c)) out.push(c);
    }
    return out;
  };

  // Splits a canonical tag at its Unicode extension: the rest, and the
  // extension's keywords ({ nu: 'thai', ca: 'buddhist' }).
  const splitExtension = t => {
    const subtags = t.split('-');
    const base = [], keywords = {};
    let i = 0;
    for (; i < subtags.length; i++) {
      if (subtags[i] === 'u' || subtags[i] === 'x') break;
      if (subtags[i].length === 1) { // another extension (t-, ...): keep it, it isn't ours
        base.push(subtags[i]);
        continue;
      }
      base.push(subtags[i]);
    }
    if (subtags[i] === 'u') {
      i++;
      let key = null;
      for (; i < subtags.length && subtags[i].length > 1; i++) {
        if (subtags[i].length === 2) { key = subtags[i]; keywords[key] = ''; }
        else if (key) keywords[key] = keywords[key] ? keywords[key] + '-' + subtags[i] : subtags[i];
      }
    }
    return { base: base.join('-'), keywords };
  };

  // The locale an Intl object uses: the first requested one ICU has data
  // for (or the default), with the Unicode extension keywords `keys` taken
  // from the tag, overridden by `options` ({ nu: ..., ca: ... }), each
  // checked by `valid`.
  const resolveLocale = (locales, keys, options = {}, valid = {}) => {
    const requested = canonicalizeLocaleList(locales);
    let found = null, keywords = {};
    for (const r of requested) {
      const { base, keywords: k } = splitExtension(r);
      const available = __wtIntlAvailable(base);
      if (available) { found = available; keywords = k; break; }
    }
    if (!found) found = __wtIntlAvailable(defaultLocale) || 'en-US';
    const values = {};
    let ext = '';
    for (const key of keys) {
      let v = keywords[key];
      let fromTag = v !== undefined;
      if (fromTag && v === '' ) v = 'true';
      if (fromTag && valid[key] && !valid[key](v)) { v = undefined; fromTag = false; }
      const opt = options[key];
      if (opt !== undefined && (!valid[key] || valid[key](opt))) {
        if (opt !== v) fromTag = false;
        v = opt;
      }
      if (v !== undefined) values[key] = v;
      if (fromTag) ext += '-' + key + (v === 'true' ? '' : '-' + v);
    }
    const locale = found + (ext ? '-u' + ext : '');
    // ICU sees every keyword in effect, from the tag or the options.
    let icuExt = '';
    for (const key of keys) if (values[key] !== undefined) icuExt += '-' + key + '-' + values[key];
    return { locale, dataLocale: found + (icuExt ? '-u' + icuExt : ''), values, base: found };
  };

  const supportedLocalesOf = (locales, options) => {
    const requested = canonicalizeLocaleList(locales);
    if (options !== undefined) getOption(toOptions(options), 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', 'Intl');
    return requested.filter(r => __wtIntlAvailable(splitExtension(r).base) !== '');
  };

  const typeName = /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/i;
  let numberingSystems = null, calendars = null, collations = null;
  const validNumberingSystem = v => {
    numberingSystems = numberingSystems || new Set(__wtIntlSupported('numberingSystem'));
    return numberingSystems.has(v);
  };
  const validCalendar = v => {
    calendars = calendars || new Set(__wtIntlSupported('calendar').map(c => c === 'gregorian' ? 'gregory' : c));
    return calendars.has(v);
  };
  // ICU lists some collations under its own names; JS uses BCP 47's.
  const bcp47Collation = c => ({ dictionary: 'dict', gb2312han: 'gb2312', phonebook: 'phonebk', traditional: 'trad' })[c] || c;
  const validCollation = v => {
    collations = collations || new Set(__wtIntlSupported('collation').map(bcp47Collation));
    return v !== 'standard' && v !== 'search' && collations.has(v);
  };
  const checkTypeName = (v, what) => {
    if (v !== undefined && !typeName.test(v)) throw new RangeError('Invalid ' + what + ' : ' + v);
    return v;
  };

  // --- formatToParts ---------------------------------------------------------
  // The natives return [string, spans]: every field ICU marked, as
  // (category, field, start, limit). Each character gets the type of the
  // innermost field around it; runs of the same field make one part. A
  // range span (category 0x1000 and up) says which side of a range the
  // characters are on.
  const buildParts = (str, spans, typeOf, ranged) => {
    const n = str.length;
    const type = new Array(n).fill(null), width = new Array(n).fill(Infinity);
    const span = new Array(n).fill(-1), side = new Array(n).fill('shared');
    for (let i = 0; i < spans.length; i += 4) {
      const [cat, field, s, e] = [spans[i], spans[i + 1], spans[i + 2], spans[i + 3]];
      if (cat >= 0x1000) {
        for (let k = s; k < e; k++) side[k] = field === 0 ? 'startRange' : 'endRange';
        continue;
      }
      const t = typeOf(cat, field, str.slice(s, e));
      if (!t) continue;
      for (let k = s; k < e; k++) if (e - s <= width[k]) { type[k] = t; width[k] = e - s; span[k] = i; }
    }
    const parts = [];
    for (let k = 0; k < n;) {
      let j = k + 1;
      while (j < n && span[j] === span[k] && side[j] === side[k]) j++;
      const part = { type: type[k] || 'literal', value: str.slice(k, j) };
      if (ranged) part.source = side[k];
      parts.push(part);
      k = j;
    }
    return parts;
  };

  const NUMBER_FIELDS = ['integer', 'fraction', 'decimal', 'exponentSeparator', 'exponentMinusSign', 'exponentInteger',
    'group', 'currency', 'percentSign', 'perMille', 'sign', 'unit', 'compact', 'approximatelySign'];
  const numberType = special => (cat, field, text) => {
    if (cat !== 2) return null;
    let t = NUMBER_FIELDS[field];
    if (t === 'sign') t = text === '+' ? 'plusSign' : 'minusSign';
    if (t === 'integer' && special) t = special;
    return t || 'literal';
  };
)JS" R"JS(
  // --- Intl.NumberFormat ------------------------------------------------------

  const SIMPLE_UNITS = ['acre', 'bit', 'byte', 'celsius', 'centimeter', 'day', 'degree', 'fahrenheit', 'fluid-ounce',
    'foot', 'gallon', 'gigabit', 'gigabyte', 'gram', 'hectare', 'hour', 'inch', 'kilobit', 'kilobyte', 'kilogram',
    'kilometer', 'liter', 'megabit', 'megabyte', 'meter', 'microsecond', 'mile', 'mile-scandinavian', 'milliliter',
    'millimeter', 'millisecond', 'minute', 'month', 'nanosecond', 'ounce', 'percent', 'petabyte', 'pound', 'second',
    'stone', 'terabit', 'terabyte', 'week', 'yard', 'year'];
  const wellFormedUnit = u => {
    if (SIMPLE_UNITS.includes(u)) return true;
    const per = u.indexOf('-per-');
    return per > 0 && SIMPLE_UNITS.includes(u.slice(0, per)) && SIMPLE_UNITS.includes(u.slice(per + 5));
  };

  // The digit options (minimumIntegerDigits ... trailingZeroDisplay), as
  // SetNumberFormatDigitOptions reads them.
  const digitOptions = (s, opts, mnfdDefault, mxfdDefault, notation) => {
    const mnid = numberOption(opts, 'minimumIntegerDigits', 1, 21, 1);
    let mnfd = opts.minimumFractionDigits, mxfd = opts.maximumFractionDigits;
    let mnsd = opts.minimumSignificantDigits, mxsd = opts.maximumSignificantDigits;
    s.minimumIntegerDigits = mnid;
    s.roundingIncrement = numberOption(opts, 'roundingIncrement', 1, 5000, 1);
    if (![1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000].includes(s.roundingIncrement))
      throw new RangeError('roundingIncrement value is out of range.');
    s.roundingMode = getOption(opts, 'roundingMode', 'string', ['ceil', 'floor', 'expand', 'trunc', 'halfCeil',
      'halfFloor', 'halfExpand', 'halfTrunc', 'halfEven'], 'halfExpand', 'Intl.NumberFormat');
    s.roundingPriority = getOption(opts, 'roundingPriority', 'string', ['auto', 'morePrecision', 'lessPrecision'],
      'auto', 'Intl.NumberFormat');
    s.trailingZeroDisplay = getOption(opts, 'trailingZeroDisplay', 'string', ['auto', 'stripIfInteger'], 'auto',
      'Intl.NumberFormat');
    const hasSd = mnsd !== undefined || mxsd !== undefined;
    const hasFd = mnfd !== undefined || mxfd !== undefined;
    let needSd = true, needFd = true;
    if (s.roundingPriority === 'auto') {
      needSd = hasSd;
      if (needSd || (!hasFd && notation === 'compact')) needFd = false;
    }
    if (needSd) {
      if (hasSd) {
        mnsd = defaultNumber(mnsd, 1, 21, 1, 'minimumSignificantDigits');
        mxsd = defaultNumber(mxsd, mnsd, 21, 21, 'maximumSignificantDigits');
      }
      else { mnsd = 1; mxsd = 21; }
      s.minimumSignificantDigits = mnsd;
      s.maximumSignificantDigits = mxsd;
    }
    if (needFd) {
      if (hasFd) {
        mnfd = defaultNumber(mnfd, 0, 100, undefined, 'minimumFractionDigits');
        mxfd = defaultNumber(mxfd, 0, 100, undefined, 'maximumFractionDigits');
        if (mnfd === undefined) mnfd = Math.min(mnfdDefault, mxfd);
        else if (mxfd === undefined) mxfd = Math.max(mxfdDefault, mnfd);
        else if (mnfd > mxfd) throw new RangeError('maximumFractionDigits value is out of range.');
      }
      else { mnfd = mnfdDefault; mxfd = mxfdDefault; }
      s.minimumFractionDigits = mnfd;
      s.maximumFractionDigits = mxfd;
    }
    if (!needSd && !needFd) { // compact notation's own rounding
      s.minimumFractionDigits = 0; s.maximumFractionDigits = 0;
      s.minimumSignificantDigits = 1; s.maximumSignificantDigits = 2;
      s.roundingType = 'compact';
      s.roundingPriority = 'morePrecision';
    }
    else if (needSd && needFd) s.roundingType = s.roundingPriority;
    else s.roundingType = needSd ? 'significantDigits' : 'fractionDigits';
  };

  // The rounding part of an ICU number skeleton.
  const precisionStem = s => {
    const frac = () => s.maximumFractionDigits === 0 ? 'precision-integer'
      : '.' + '0'.repeat(s.minimumFractionDigits) + '#'.repeat(s.maximumFractionDigits - s.minimumFractionDigits);
    const sig = () => '@'.repeat(s.minimumSignificantDigits) + '#'.repeat(s.maximumSignificantDigits - s.minimumSignificantDigits);
    let stem;
    if (s.roundingType === 'compact') stem = '';
    else if (s.roundingType === 'significantDigits') stem = sig();
    else if (s.roundingType === 'fractionDigits') stem = frac();
    else { // both: the one that keeps more (or fewer) digits
      const f = s.maximumFractionDigits === 0 ? '.' : frac();
      stem = f + '/' + sig() + (s.roundingType === 'morePrecision' ? 'r' : 's');
    }
    if (stem && s.trailingZeroDisplay === 'stripIfInteger') stem += '/w';
    if (stem && s.roundingIncrement !== 1) {
      const digits = s.maximumFractionDigits;
      const inc = String(s.roundingIncrement).padStart(digits + 1, '0');
      stem = 'precision-increment/' + (digits ? inc.slice(0, inc.length - digits) + '.' + inc.slice(-digits) : inc);
    }
    return stem;
  };

  const ROUNDING_MODES = { ceil: 'ceiling', floor: 'floor', expand: 'up', trunc: 'down', halfCeil: 'half-ceiling',
    halfFloor: 'half-floor', halfExpand: 'half-up', halfTrunc: 'half-down', halfEven: 'half-even' };

  const numberSkeleton = s => {
    const stems = [];
    let accounting = false;
    if (s.style === 'percent') stems.push('percent', 'scale/100');
    else if (s.style === 'currency') {
      stems.push('currency/' + s.currency, { code: 'unit-width-iso-code', symbol: 'unit-width-short',
        narrowSymbol: 'unit-width-narrow', name: 'unit-width-full-name' }[s.currencyDisplay]);
      accounting = s.currencySign === 'accounting';
    }
    else if (s.style === 'unit') {
      stems.push('unit/' + s.unit, { short: 'unit-width-short', narrow: 'unit-width-narrow',
        long: 'unit-width-full-name' }[s.unitDisplay]);
    }
    if (s.notation === 'scientific') stems.push('scientific');
    else if (s.notation === 'engineering') stems.push('engineering');
    else if (s.notation === 'compact') stems.push(s.compactDisplay === 'long' ? 'compact-long' : 'compact-short');
    if (s.minimumIntegerDigits > 1) stems.push('integer-width/*' + '0'.repeat(s.minimumIntegerDigits));
    const precision = precisionStem(s);
    if (precision) stems.push(precision);
    stems.push('rounding-mode-' + ROUNDING_MODES[s.roundingMode]);
    if (s.useGrouping === false) stems.push('group-off');
    else if (s.useGrouping === 'min2') stems.push('group-min2');
    else if (s.useGrouping === 'always') stems.push('group-on-aligned');
    const sign = {
      auto: accounting ? 'sign-accounting' : '',
      never: 'sign-never',
      always: accounting ? 'sign-accounting-always' : 'sign-always',
      exceptZero: accounting ? 'sign-accounting-except-zero' : 'sign-except-zero',
      negative: accounting ? 'sign-accounting-negative' : 'sign-negative',
    }[s.signDisplay];
    if (sign) stems.push(sign);
    return stems.join(' ');
  };

  // A value to format: a BigInt (or a numeric string) exactly, otherwise
  // a Number.
  const toIntlNumber = x => {
    if (typeof x === 'bigint') return String(x);
    if (typeof x === 'string') {
      const t = x.trim();
      if (/^[+-]?(\d+\.?\d*|\.\d+)(e[+-]?\d+)?$/i.test(t)) return t.replace(/^\+/, '');
    }
    return Number(x);
  };
  const specialOf = v => typeof v === 'number' ? (Number.isNaN(v) ? 'nan' : !Number.isFinite(v) ? 'infinity' : null) : null;

  const formatNumber = (s, x, parts) => {
    const v = toIntlNumber(x);
    const r = __wtIntlNumber(s.dataLocale, s.skeleton, v, parts);
    if (r === null) throw new RangeError('Incorrect locale information provided');
    if (!parts) return r;
    const special = specialOf(v);
    const out = buildParts(r[0], r[1], numberType(special), false);
    // NaN and infinity may not be marked as a field at all.
    if (special)
      for (const p of out) if (p.type === 'literal' && /NaN|[^\s\-+()]/.test(p.value) && p.value.trim()) p.type = special;
    return out;
  };

  function NumberFormat(locales, options) {
    if (!new.target) return new NumberFormat(locales, options);
    const opts = toOptions(options);
    const C = 'Intl.NumberFormat';
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const nu = checkTypeName(getOption(opts, 'numberingSystem', 'string'), 'numberingSystem');
    const r = resolveLocale(locales, ['nu'], { nu }, { nu: validNumberingSystem });
    const s = { kind: 'NumberFormat', locale: r.locale, dataLocale: r.dataLocale,
                numberingSystem: r.values.nu || __wtIntlNumberingSystem(r.base) };
    s.style = getOption(opts, 'style', 'string', ['decimal', 'percent', 'currency', 'unit'], 'decimal', C);
    let currency = getOption(opts, 'currency', 'string');
    if (currency !== undefined && !/^[a-z]{3}$/i.test(currency)) throw new RangeError('Invalid currency code : ' + currency);
    const currencyDisplay = getOption(opts, 'currencyDisplay', 'string', ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol', C);
    const currencySign = getOption(opts, 'currencySign', 'string', ['standard', 'accounting'], 'standard', C);
    const unit = getOption(opts, 'unit', 'string');
    if (unit !== undefined && !wellFormedUnit(unit)) throw new RangeError("Invalid unit argument for Intl.NumberFormat() '" + unit + "'");
    const unitDisplay = getOption(opts, 'unitDisplay', 'string', ['short', 'narrow', 'long'], 'short', C);
    if (s.style === 'currency') {
      if (currency === undefined) throw new TypeError('Currency code is required with currency style.');
      s.currency = currency.toUpperCase();
      s.currencyDisplay = currencyDisplay;
      s.currencySign = currencySign;
    }
    if (s.style === 'unit') {
      if (unit === undefined) throw new TypeError("Invalid unit argument for Intl.NumberFormat() 'undefined'");
      s.unit = unit;
      s.unitDisplay = unitDisplay;
    }
    s.notation = getOption(opts, 'notation', 'string', ['standard', 'scientific', 'engineering', 'compact'], 'standard', C);
    const cDigits = s.style === 'currency' ? __wtIntlCurrencyDigits(s.currency) : 0;
    const standardCurrency = s.style === 'currency' && s.notation === 'standard';
    digitOptions(s, opts, standardCurrency ? cDigits : 0,
                 standardCurrency ? cDigits : s.style === 'percent' ? 0 : 3, s.notation);
    s.compactDisplay = getOption(opts, 'compactDisplay', 'string', ['short', 'long'], 'short', C);
    const groupingDefault = s.notation === 'compact' ? 'min2' : 'auto';
    let grouping = opts.useGrouping;
    if (grouping === undefined) grouping = groupingDefault;
    else if (grouping === true) grouping = 'always';
    else if (grouping === false || grouping === null || grouping === 0 || grouping === '') grouping = false;
    else {
      grouping = String(grouping);
      if (grouping === 'true' || grouping === 'false') grouping = groupingDefault;
      else if (!['min2', 'auto', 'always'].includes(grouping))
        throw new RangeError('Value ' + grouping + ' out of range for Intl.NumberFormat options property useGrouping');
    }
    s.useGrouping = grouping;
    s.signDisplay = getOption(opts, 'signDisplay', 'string', ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto', C);
    s.skeleton = numberSkeleton(s);
    slots.set(this, s);
  }
  hidden(NumberFormat, { supportedLocalesOf });
  hidden(NumberFormat.prototype, {
    formatToParts(x) { return formatNumber(slotOf(this, 'NumberFormat', 'formatToParts'), x, true); },
    formatRange(a, b) { return numberRange(slotOf(this, 'NumberFormat', 'formatRange'), a, b, false); },
    formatRangeToParts(a, b) { return numberRange(slotOf(this, 'NumberFormat', 'formatRangeToParts'), a, b, true); },
    resolvedOptions() {
      const s = slotOf(this, 'NumberFormat', 'resolvedOptions');
      const o = { locale: s.locale, numberingSystem: s.numberingSystem, style: s.style };
      if (s.style === 'currency') Object.assign(o, { currency: s.currency, currencyDisplay: s.currencyDisplay, currencySign: s.currencySign });
      if (s.style === 'unit') Object.assign(o, { unit: s.unit, unitDisplay: s.unitDisplay });
      o.minimumIntegerDigits = s.minimumIntegerDigits;
      if (s.minimumFractionDigits !== undefined) {
        o.minimumFractionDigits = s.minimumFractionDigits;
        o.maximumFractionDigits = s.maximumFractionDigits;
      }
      if (s.minimumSignificantDigits !== undefined && s.roundingType !== 'fractionDigits') {
        o.minimumSignificantDigits = s.minimumSignificantDigits;
        o.maximumSignificantDigits = s.maximumSignificantDigits;
      }
      o.useGrouping = s.useGrouping;
      o.notation = s.notation;
      if (s.notation === 'compact') o.compactDisplay = s.compactDisplay;
      o.signDisplay = s.signDisplay;
      o.roundingIncrement = s.roundingIncrement;
      o.roundingMode = s.roundingMode;
      o.roundingPriority = s.roundingPriority;
      o.trailingZeroDisplay = s.trailingZeroDisplay;
      return o;
    },
  });
  // format is a getter returning a bound function, so it can be passed
  // around (array.map(nf.format)).
  getters(NumberFormat.prototype, {
    format() {
      const s = slotOf(this, 'NumberFormat', 'format');
      return s.bound || (s.bound = x => formatNumber(s, x, false));
    },
  });
  tag(NumberFormat.prototype, 'Intl.NumberFormat');

  const numberRange = (s, a, b, parts) => {
    if (a === undefined || b === undefined) throw new TypeError('start or end is undefined');
    const x = Number(toIntlNumber(a)), y = Number(toIntlNumber(b));
    if (Number.isNaN(x) || Number.isNaN(y)) throw new RangeError('Invalid time value');
    const r = __wtIntlNumberRange(s.dataLocale, s.skeleton, x, y, parts);
    if (r === null) throw new RangeError('Incorrect locale information provided');
    return parts ? buildParts(r[0], r[1], numberType(null), true) : r;
  };
)JS" R"JS(
  // --- Intl.DateTimeFormat ----------------------------------------------------

  const DATE_FIELDS = ['era', 'year', 'month', 'day', 'hour', 'hour', 'minute', 'second', 'fractionalSecond',
    'weekday', 'literal', 'literal', 'literal', 'literal', 'dayPeriod', 'hour', 'hour', 'timeZoneName', 'year',
    'weekday', 'year', 'literal', 'literal', 'timeZoneName', 'literal', 'literal', 'month', 'weekday',
    'timeZoneName', 'timeZoneName', 'yearName', 'timeZoneName', 'timeZoneName', 'timeZoneName', 'relatedYear',
    'dayPeriod', 'dayPeriod'];
  const dateType = (cat, field) => cat === 1 ? DATE_FIELDS[field] || 'literal' : cat === 2 ? 'literal' : null;

  const COMPONENTS = [
    ['weekday', ['narrow', 'short', 'long']],
    ['era', ['narrow', 'short', 'long']],
    ['year', ['2-digit', 'numeric']],
    ['month', ['2-digit', 'numeric', 'narrow', 'short', 'long']],
    ['day', ['2-digit', 'numeric']],
    ['dayPeriod', ['narrow', 'short', 'long']],
    ['hour', ['2-digit', 'numeric']],
    ['minute', ['2-digit', 'numeric']],
    ['second', ['2-digit', 'numeric']],
    ['fractionalSecondDigits', null],
    ['timeZoneName', ['short', 'long', 'shortOffset', 'longOffset', 'shortGeneric', 'longGeneric']],
  ];
  const STYLES = ['full', 'long', 'medium', 'short'];

  // The letters a pattern uses for each field, outside quoted text.
  const patternFields = pattern => {
    const out = [];
    for (let i = 0; i < pattern.length;) {
      const c = pattern[i];
      if (c === "'") {
        const end = pattern.indexOf("'", i + 1);
        i = end < 0 ? pattern.length : end + 1;
        continue;
      }
      let j = i + 1;
      while (j < pattern.length && pattern[j] === c) j++;
      if (/[A-Za-z]/.test(c)) out.push([c, j - i]);
      i = j;
    }
    return out;
  };
  // The hour letters in a pattern replaced by `letter` (an explicit hour
  // cycle), quoted text left alone.
  const withHourLetter = (pattern, letter) => {
    let out = '', quoted = false;
    for (const c of pattern) {
      if (c === "'") quoted = !quoted;
      out += !quoted && 'hHkK'.includes(c) ? letter : c;
    }
    return out;
  };
  const HOUR_CYCLES = { h: 'h12', H: 'h23', k: 'h24', K: 'h11' };
  const HOUR_LETTERS = { h11: 'K', h12: 'h', h23: 'H', h24: 'k' };

  // Which hour letter `locale` uses: by default (j), on a 12-hour clock
  // (h) and on a 24-hour one (H).
  const hourLetter = (locale, skeleton) => {
    const p = __wtIntlDatePattern(locale, skeleton) || '';
    const f = patternFields(p).find(([c]) => 'hHkK'.includes(c));
    return f ? f[0] : skeleton;
  };

  const createDateTimeFormat = (target, locales, options, required, defaults) => {
    const opts = toOptions(options);
    const C = 'Intl.DateTimeFormat';
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const ca = checkTypeName(getOption(opts, 'calendar', 'string'), 'calendar');
    const nu = checkTypeName(getOption(opts, 'numberingSystem', 'string'), 'numberingSystem');
    const hour12 = getOption(opts, 'hour12', 'boolean');
    let hc = getOption(opts, 'hourCycle', 'string', ['h11', 'h12', 'h23', 'h24'], undefined, C);
    if (hour12 !== undefined) hc = null; // hour12 wins over hourCycle
    const r = resolveLocale(locales, ['ca', 'hc', 'nu'], { ca, nu, hc: hc === null ? undefined : hc },
      { ca: validCalendar, nu: validNumberingSystem, hc: v => ['h11', 'h12', 'h23', 'h24'].includes(v) });
    const s = { kind: 'DateTimeFormat', locale: r.locale, dataLocale: r.dataLocale,
                calendar: r.values.ca || 'gregory', numberingSystem: r.values.nu || __wtIntlNumberingSystem(r.base) };
    if (hc === null) hc = undefined;
    else if (hc === undefined) hc = r.values.hc;

    let tz = opts.timeZone;
    if (tz === undefined) s.timeZone = __wtIntlTimeZone('');
    else {
      tz = String(tz);
      s.timeZone = __wtIntlTimeZone(tz);
      if (s.timeZone === null) throw new RangeError('Invalid time zone specified: ' + tz);
    }

    const fields = {};
    for (const [name, values] of COMPONENTS) {
      if (name === 'fractionalSecondDigits') {
        const v = numberOption(opts, name, 1, 3, undefined);
        if (v !== undefined) fields[name] = v;
        continue;
      }
      const v = getOption(opts, name, 'string', values, undefined, C);
      if (v !== undefined) fields[name] = v;
    }
    getOption(opts, 'formatMatcher', 'string', ['basic', 'best fit'], 'best fit', C);
    const dateStyle = getOption(opts, 'dateStyle', 'string', STYLES, undefined, C);
    const timeStyle = getOption(opts, 'timeStyle', 'string', STYLES, undefined, C);

    // The hour letter: from hour12/hourCycle, else the locale's own.
    let letter = null;
    if (hour12 !== undefined) letter = hour12 ? (hourLetter(r.dataLocale, 'h') === 'K' ? 'K' : 'h')
                                              : (hourLetter(r.dataLocale, 'H') === 'k' ? 'k' : 'H');
    else if (hc !== undefined) letter = HOUR_LETTERS[hc];

    let pattern;
    if (dateStyle !== undefined || timeStyle !== undefined) {
      if (Object.keys(fields).length) throw new TypeError('Invalid option : option');
      if (required === 'date' && dateStyle === undefined)
        throw new TypeError('Invalid option : timeStyle');
      if (required === 'time' && timeStyle === undefined)
        throw new TypeError('Invalid option : dateStyle');
      s.dateStyle = dateStyle;
      s.timeStyle = timeStyle;
      pattern = __wtIntlDateStylePattern(r.dataLocale, dateStyle === undefined ? -1 : STYLES.indexOf(dateStyle),
                                         timeStyle === undefined ? -1 : STYLES.indexOf(timeStyle));
      // An explicit hour cycle changes the pattern, not just its hour letter:
      // 24 hours drop the AM/PM, 12 hours gain one. As V8 does, the style's
      // fields are asked for again with the other hour.
      if (letter && pattern !== null && patternFields(pattern).some(([c]) => 'hHkK'.includes(c))) {
        let skeleton = __wtIntlSkeleton(pattern).replace(/[hHkKjJC]/g, letter);
        if ('Hk'.includes(letter)) skeleton = skeleton.replace(/[abB]/g, '');
        pattern = __wtIntlDatePattern(r.dataLocale, skeleton);
      }
    }
    else {
      // No fields asked for: the defaults (a date, a time, or both).
      const has = names => names.some(n => fields[n] !== undefined);
      let needDefaults = true;
      if ((required === 'date' || required === 'any') && has(['weekday', 'year', 'month', 'day'])) needDefaults = false;
      if ((required === 'time' || required === 'any') && has(['dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits'])) needDefaults = false;
      if (needDefaults && (defaults === 'date' || defaults === 'all')) Object.assign(fields, { year: 'numeric', month: 'numeric', day: 'numeric' });
      if (needDefaults && (defaults === 'time' || defaults === 'all')) Object.assign(fields, { hour: 'numeric', minute: 'numeric', second: 'numeric' });
      const sk = { weekday: { narrow: 'EEEEE', short: 'EEE', long: 'EEEE' }, era: { narrow: 'GGGGG', short: 'G', long: 'GGGG' },
        year: { '2-digit': 'yy', numeric: 'y' },
        month: { '2-digit': 'MM', numeric: 'M', narrow: 'MMMMM', short: 'MMM', long: 'MMMM' },
        day: { '2-digit': 'dd', numeric: 'd' }, dayPeriod: { narrow: 'BBBBB', short: 'B', long: 'BBBB' },
        minute: { '2-digit': 'mm', numeric: 'm' }, second: { '2-digit': 'ss', numeric: 's' },
        timeZoneName: { short: 'z', long: 'zzzz', shortOffset: 'O', longOffset: 'OOOO', shortGeneric: 'v', longGeneric: 'vvvv' } };
      let skeleton = '';
      for (const [name] of COMPONENTS) {
        const v = fields[name];
        if (v === undefined) continue;
        if (name === 'hour') skeleton += (letter || (fields.dayPeriod ? hourLetter(r.dataLocale, 'j') : 'j')).repeat(v === '2-digit' ? 2 : 1);
        else if (name === 'fractionalSecondDigits') skeleton += 'S'.repeat(v);
        else skeleton += sk[name][v];
      }
      pattern = __wtIntlDatePattern(r.dataLocale, skeleton);
    }
    if (pattern === null) throw new RangeError('Incorrect locale information provided');
    if (letter) pattern = withHourLetter(pattern, letter);
    s.pattern = pattern;
    s.fields = fields;
    slots.set(target, s);
    return target;
  };

  const timeValue = date => {
    const x = date === undefined ? Date.now() : Number(date);
    if (!Number.isFinite(x) || Math.abs(x) > 8.64e15) throw new RangeError('Invalid time value');
    return Math.trunc(x) + 0;
  };
  const formatDate = (s, date, parts) => {
    const r = __wtIntlDateFormat(s.dataLocale, s.pattern, s.timeZone, timeValue(date), parts);
    if (r === null) throw new RangeError('Incorrect locale information provided');
    return parts ? buildParts(r[0], r[1], dateType, false) : r;
  };
  const dateRange = (s, a, b, parts) => {
    if (a === undefined || b === undefined) throw new TypeError('startDate or endDate is undefined');
    const r = __wtIntlDateRange(s.dataLocale, s.pattern, s.timeZone, timeValue(a), timeValue(b), parts);
    if (r === null) throw new RangeError('Incorrect locale information provided');
    return parts ? buildParts(r[0], r[1], dateType, true) : r;
  };

  function DateTimeFormat(locales, options) {
    if (!new.target) return new DateTimeFormat(locales, options);
    createDateTimeFormat(this, locales, options, 'any', 'date');
  }
  hidden(DateTimeFormat, { supportedLocalesOf });
  hidden(DateTimeFormat.prototype, {
    formatToParts(date) { return formatDate(slotOf(this, 'DateTimeFormat', 'formatToParts'), date, true); },
    formatRange(a, b) { return dateRange(slotOf(this, 'DateTimeFormat', 'formatRange'), a, b, false); },
    formatRangeToParts(a, b) { return dateRange(slotOf(this, 'DateTimeFormat', 'formatRangeToParts'), a, b, true); },
    resolvedOptions() {
      const s = slotOf(this, 'DateTimeFormat', 'resolvedOptions');
      const o = { locale: s.locale, calendar: s.calendar, numberingSystem: s.numberingSystem, timeZone: s.timeZone };
      // What the pattern actually shows, which can differ from what was
      // asked for (ICU picks the locale's closest pattern).
      const f = {};
      for (const [c, n] of patternFields(s.pattern)) {
        if (c === 'G') f.era = n === 4 ? 'long' : n === 5 ? 'narrow' : 'short';
        else if (c === 'y' || c === 'Y' || c === 'u') f.year = n === 2 ? '2-digit' : 'numeric';
        else if (c === 'M' || c === 'L') f.month = ['numeric', '2-digit', 'short', 'long', 'narrow'][Math.min(n, 5) - 1];
        else if (c === 'd') f.day = n === 2 ? '2-digit' : 'numeric';
        else if (c === 'E' || c === 'c' || c === 'e') f.weekday = n === 4 ? 'long' : n === 5 ? 'narrow' : 'short';
        else if (c === 'B') f.dayPeriod = n === 4 ? 'long' : n === 5 ? 'narrow' : 'short';
        else if ('hHkK'.includes(c)) { f.hour = n === 2 ? '2-digit' : 'numeric'; f.hourCycle = HOUR_CYCLES[c]; }
        else if (c === 'm') f.minute = n === 2 ? '2-digit' : 'numeric';
        else if (c === 's') f.second = n === 2 ? '2-digit' : 'numeric';
        else if (c === 'S') f.fractionalSecondDigits = n;
        else if (c === 'z') f.timeZoneName = n === 4 ? 'long' : 'short';
        else if (c === 'O') f.timeZoneName = n === 4 ? 'longOffset' : 'shortOffset';
        else if (c === 'v') f.timeZoneName = n === 4 ? 'longGeneric' : 'shortGeneric';
      }
      if (f.hourCycle) { o.hourCycle = f.hourCycle; o.hour12 = f.hourCycle === 'h11' || f.hourCycle === 'h12'; }
      if (s.dateStyle === undefined && s.timeStyle === undefined) {
        for (const name of ['weekday', 'era', 'year', 'month', 'day', 'dayPeriod', 'hour', 'minute', 'second',
                            'fractionalSecondDigits', 'timeZoneName'])
          if (f[name] !== undefined) o[name] = f[name];
      }
      else {
        if (s.dateStyle !== undefined) o.dateStyle = s.dateStyle;
        if (s.timeStyle !== undefined) o.timeStyle = s.timeStyle;
      }
      return o;
    },
  });
  getters(DateTimeFormat.prototype, {
    format() {
      const s = slotOf(this, 'DateTimeFormat', 'format');
      return s.bound || (s.bound = date => formatDate(s, date, false));
    },
  });
  tag(DateTimeFormat.prototype, 'Intl.DateTimeFormat');
)JS" R"JS(
  // --- Intl.Collator ----------------------------------------------------------

  function Collator(locales, options) {
    if (!new.target) return new Collator(locales, options);
    const opts = toOptions(options);
    const C = 'Intl.Collator';
    const usage = getOption(opts, 'usage', 'string', ['sort', 'search'], 'sort', C);
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const co = checkTypeName(getOption(opts, 'collation', 'string'), 'collation');
    let kn = getOption(opts, 'numeric', 'boolean');
    if (kn !== undefined) kn = String(kn);
    const kf = getOption(opts, 'caseFirst', 'string', ['upper', 'lower', 'false'], undefined, C);
    const r = resolveLocale(locales, ['co', 'kf', 'kn'], { co, kn, kf },
      { co: validCollation, kn: v => v === 'true' || v === 'false', kf: v => ['upper', 'lower', 'false'].includes(v) });
    const sensitivity = getOption(opts, 'sensitivity', 'string', ['base', 'accent', 'case', 'variant'], 'variant', C);
    const ignorePunctuation = getOption(opts, 'ignorePunctuation', 'boolean', null, false);
    const s = { kind: 'Collator', locale: r.locale, usage, sensitivity, ignorePunctuation,
                collation: r.values.co || 'default', numeric: r.values.kn === 'true', caseFirst: r.values.kf || 'false' };
    s.dataLocale = r.base + (r.values.co ? '-u-co-' + r.values.co : usage === 'search' ? '-u-co-search' : '');
    s.code = sensitivity[0] + (s.numeric ? '1' : '0') + (ignorePunctuation ? '1' : '0') +
             (r.values.kf === undefined ? 'd' : r.values.kf === 'upper' ? 'u' : r.values.kf === 'lower' ? 'l' : 'f');
    s.collator = __wtIntlCollator(s.dataLocale, s.code);
    slots.set(this, s);
  }
  hidden(Collator, { supportedLocalesOf });
  hidden(Collator.prototype, {
    resolvedOptions() {
      const s = slotOf(this, 'Collator', 'resolvedOptions');
      return { locale: s.locale, usage: s.usage, sensitivity: s.sensitivity, ignorePunctuation: s.ignorePunctuation,
               collation: s.collation, numeric: s.numeric, caseFirst: s.caseFirst };
    },
  });
  getters(Collator.prototype, {
    compare() {
      const s = slotOf(this, 'Collator', 'compare');
      return s.bound || (s.bound = (a, b) => {
        a = String(a);
        b = String(b);
        return a === b ? 0 : __wtIntlCompare(s.collator, a, b);
      });
    },
  });
  tag(Collator.prototype, 'Intl.Collator');

  // --- Intl.PluralRules -------------------------------------------------------

  const PLURAL_ORDER = ['zero', 'one', 'two', 'few', 'many', 'other'];

  function PluralRules(locales, options) {
    if (!new.target) throw new TypeError("Constructor Intl.PluralRules requires 'new'");
    const opts = toOptions(options);
    const C = 'Intl.PluralRules';
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const r = resolveLocale(locales, []);
    const s = { kind: 'PluralRules', locale: r.locale, dataLocale: r.dataLocale };
    s.type = getOption(opts, 'type', 'string', ['cardinal', 'ordinal'], 'cardinal', C);
    s.notation = getOption(opts, 'notation', 'string', ['standard', 'scientific', 'engineering', 'compact'], 'standard', C);
    digitOptions(s, opts, 0, 3, s.notation);
    s.skeleton = precisionStem(s) + ' rounding-mode-' + ROUNDING_MODES[s.roundingMode];
    slots.set(this, s);
  }
  hidden(PluralRules, { supportedLocalesOf });
  hidden(PluralRules.prototype, {
    select(n) {
      const s = slotOf(this, 'PluralRules', 'select');
      return __wtIntlPlural(s.dataLocale, s.type === 'ordinal', s.skeleton, Number(n));
    },
    selectRange(a, b) {
      const s = slotOf(this, 'PluralRules', 'selectRange');
      if (a === undefined || b === undefined) throw new TypeError('start or end is undefined');
      if (Number.isNaN(Number(a)) || Number.isNaN(Number(b))) throw new RangeError('Invalid time value');
      return __wtIntlPlural(s.dataLocale, s.type === 'ordinal', s.skeleton, Number(b));
    },
    resolvedOptions() {
      const s = slotOf(this, 'PluralRules', 'resolvedOptions');
      const o = { locale: s.locale, type: s.type, notation: s.notation, minimumIntegerDigits: s.minimumIntegerDigits };
      if (s.minimumFractionDigits !== undefined) {
        o.minimumFractionDigits = s.minimumFractionDigits;
        o.maximumFractionDigits = s.maximumFractionDigits;
      }
      if (s.minimumSignificantDigits !== undefined && s.roundingType !== 'fractionDigits') {
        o.minimumSignificantDigits = s.minimumSignificantDigits;
        o.maximumSignificantDigits = s.maximumSignificantDigits;
      }
      const cats = __wtIntlPluralCategories(s.dataLocale, s.type === 'ordinal');
      o.pluralCategories = PLURAL_ORDER.filter(c => cats.includes(c));
      o.roundingIncrement = s.roundingIncrement;
      o.roundingMode = s.roundingMode;
      o.roundingPriority = s.roundingPriority;
      o.trailingZeroDisplay = s.trailingZeroDisplay;
      return o;
    },
  });
  tag(PluralRules.prototype, 'Intl.PluralRules');

  // --- Intl.RelativeTimeFormat ------------------------------------------------

  const RELATIVE_UNITS = ['year', 'quarter', 'month', 'week', 'day', 'hour', 'minute', 'second'];

  function RelativeTimeFormat(locales, options) {
    if (!new.target) throw new TypeError("Constructor Intl.RelativeTimeFormat requires 'new'");
    const opts = toOptions(options);
    const C = 'Intl.RelativeTimeFormat';
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const nu = checkTypeName(getOption(opts, 'numberingSystem', 'string'), 'numberingSystem');
    const r = resolveLocale(locales, ['nu'], { nu }, { nu: validNumberingSystem });
    const s = { kind: 'RelativeTimeFormat', locale: r.locale, dataLocale: r.dataLocale,
                numberingSystem: r.values.nu || __wtIntlNumberingSystem(r.base) };
    s.style = getOption(opts, 'style', 'string', ['long', 'short', 'narrow'], 'long', C);
    s.numeric = getOption(opts, 'numeric', 'string', ['always', 'auto'], 'always', C);
    slots.set(this, s);
  }
  const formatRelative = (s, value, unit, parts) => {
    const u = String(unit);
    const singular = u.endsWith('s') ? u.slice(0, -1) : u;
    const index = RELATIVE_UNITS.indexOf(singular);
    if (index < 0) throw new RangeError('Invalid unit argument for Intl.RelativeTimeFormat.prototype.' + (parts ? 'formatToParts' : 'format') + "() '" + u + "'");
    const v = Number(value);
    if (!Number.isFinite(v)) throw new RangeError('Invalid number value');
    const r = __wtIntlRelative(s.dataLocale, ['long', 'short', 'narrow'].indexOf(s.style), s.numeric === 'always', v, index, parts);
    if (r === null) throw new RangeError('Incorrect locale information provided');
    if (!parts) return r;
    const out = buildParts(r[0], r[1], numberType(null), false);
    for (const p of out) if (p.type !== 'literal') p.unit = singular;
    return out;
  };
  hidden(RelativeTimeFormat, { supportedLocalesOf });
  hidden(RelativeTimeFormat.prototype, {
    format(value, unit) { return formatRelative(slotOf(this, 'RelativeTimeFormat', 'format'), value, unit, false); },
    formatToParts(value, unit) { return formatRelative(slotOf(this, 'RelativeTimeFormat', 'formatToParts'), value, unit, true); },
    resolvedOptions() {
      const s = slotOf(this, 'RelativeTimeFormat', 'resolvedOptions');
      return { locale: s.locale, style: s.style, numeric: s.numeric, numberingSystem: s.numberingSystem };
    },
  });
  tag(RelativeTimeFormat.prototype, 'Intl.RelativeTimeFormat');

  // --- Intl.ListFormat --------------------------------------------------------

  function ListFormat(locales, options) {
    if (!new.target) throw new TypeError("Constructor Intl.ListFormat requires 'new'");
    const opts = toOptions(options);
    const C = 'Intl.ListFormat';
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const r = resolveLocale(locales, []);
    const s = { kind: 'ListFormat', locale: r.locale, dataLocale: r.dataLocale };
    s.type = getOption(opts, 'type', 'string', ['conjunction', 'disjunction', 'unit'], 'conjunction', C);
    s.style = getOption(opts, 'style', 'string', ['long', 'short', 'narrow'], 'long', C);
    slots.set(this, s);
  }
  const formatList = (s, list, parts) => {
    const items = [];
    if (list !== undefined) {
      for (const item of list) {
        if (typeof item !== 'string') throw new TypeError('Iterable yielded ' + String(item) + ' which is not a string');
        items.push(item);
      }
    }
    const r = __wtIntlList(s.dataLocale, ['conjunction', 'disjunction', 'unit'].indexOf(s.type),
                           ['long', 'short', 'narrow'].indexOf(s.style), items, parts);
    if (r === null) throw new RangeError('Incorrect locale information provided');
    return parts ? buildParts(r[0], r[1], (cat, field) => cat === 3 ? (field === 1 ? 'element' : 'literal') : null, false) : r;
  };
  hidden(ListFormat, { supportedLocalesOf });
  hidden(ListFormat.prototype, {
    format(list) { return formatList(slotOf(this, 'ListFormat', 'format'), list, false); },
    formatToParts(list) { return formatList(slotOf(this, 'ListFormat', 'formatToParts'), list, true); },
    resolvedOptions() {
      const s = slotOf(this, 'ListFormat', 'resolvedOptions');
      return { locale: s.locale, type: s.type, style: s.style };
    },
  });
  tag(ListFormat.prototype, 'Intl.ListFormat');
)JS" R"JS(
  // --- Intl.Segmenter ---------------------------------------------------------

  function Segmenter(locales, options) {
    if (!new.target) throw new TypeError("Constructor Intl.Segmenter requires 'new'");
    const opts = toOptions(options);
    const C = 'Intl.Segmenter';
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const r = resolveLocale(locales, []);
    const s = { kind: 'Segmenter', locale: r.locale, dataLocale: r.dataLocale };
    s.granularity = getOption(opts, 'granularity', 'string', ['grapheme', 'word', 'sentence'], 'grapheme', C);
    slots.set(this, s);
  }
  const segmentsProto = {};
  const segmentData = (segs, start, end, status) => {
    const d = { segment: segs.input.slice(start, end), index: start, input: segs.input };
    if (segs.granularity === 'word') d.isWordLike = status >= 100; // UBRK_WORD_NONE_LIMIT and above
    return d;
  };
  hidden(segmentsProto, {
    containing(index) {
      const segs = slots.get(this);
      let n = Math.trunc(Number(index)) || 0;
      if (n < 0 || n >= segs.input.length) return undefined;
      let start = 0;
      for (let i = 0; i < segs.breaks.length; i += 2) {
        if (n < segs.breaks[i]) return segmentData(segs, start, segs.breaks[i], segs.breaks[i + 1]);
        start = segs.breaks[i];
      }
      return undefined;
    },
    [Symbol.iterator]() {
      const segs = slots.get(this);
      let i = 0, start = 0;
      return {
        next() {
          if (i >= segs.breaks.length) return { value: undefined, done: true };
          const value = segmentData(segs, start, segs.breaks[i], segs.breaks[i + 1]);
          start = segs.breaks[i];
          i += 2;
          return { value, done: false };
        },
        [Symbol.iterator]() { return this; },
      };
    },
  });
  hidden(Segmenter, { supportedLocalesOf });
  hidden(Segmenter.prototype, {
    segment(text) {
      const s = slotOf(this, 'Segmenter', 'segment');
      const input = String(text);
      const segs = Object.create(segmentsProto);
      slots.set(segs, { kind: 'Segments', input, granularity: s.granularity,
        breaks: __wtIntlSegment(s.dataLocale, ['grapheme', 'word', 'sentence'].indexOf(s.granularity), input) });
      return segs;
    },
    resolvedOptions() {
      const s = slotOf(this, 'Segmenter', 'resolvedOptions');
      return { locale: s.locale, granularity: s.granularity };
    },
  });
  tag(Segmenter.prototype, 'Intl.Segmenter');

  // --- Intl.DisplayNames ------------------------------------------------------

  const DATE_TIME_FIELDS = ['era', 'year', 'quarter', 'month', 'weekOfYear', 'weekday', 'day', 'dayPeriod', 'hour',
    'minute', 'second', 'timeZoneName'];

  function DisplayNames(locales, options) {
    if (!new.target) throw new TypeError("Constructor Intl.DisplayNames requires 'new'");
    if (options === undefined) throw new TypeError("Cannot read properties of undefined (reading 'localeMatcher')");
    const opts = toOptions(options);
    const C = 'Intl.DisplayNames';
    getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', C);
    const r = resolveLocale(locales, []);
    const s = { kind: 'DisplayNames', locale: r.locale, dataLocale: r.dataLocale };
    s.style = getOption(opts, 'style', 'string', ['narrow', 'short', 'long'], 'long', C);
    s.type = getOption(opts, 'type', 'string', ['language', 'region', 'script', 'currency', 'calendar', 'dateTimeField'], undefined, C);
    if (s.type === undefined) throw new TypeError('invalid_argument');
    s.fallback = getOption(opts, 'fallback', 'string', ['code', 'none'], 'code', C);
    s.languageDisplay = getOption(opts, 'languageDisplay', 'string', ['dialect', 'standard'], 'dialect', C);
    slots.set(this, s);
  }
  hidden(DisplayNames, { supportedLocalesOf });
  hidden(DisplayNames.prototype, {
    of(code) {
      const s = slotOf(this, 'DisplayNames', 'of');
      let c = String(code);
      const bad = () => { throw new RangeError('invalid_argument'); };
      if (s.type === 'language') { c = __wtIntlCanonical(c) || bad(); }
      else if (s.type === 'region') { if (!/^([a-z]{2}|\d{3})$/i.test(c)) bad(); c = c.toUpperCase(); }
      else if (s.type === 'script') { if (!/^[a-z]{4}$/i.test(c)) bad(); c = c[0].toUpperCase() + c.slice(1).toLowerCase(); }
      else if (s.type === 'currency') { if (!/^[a-z]{3}$/i.test(c)) bad(); c = c.toUpperCase(); }
      else if (s.type === 'calendar') { if (!typeName.test(c)) bad(); c = c.toLowerCase(); if (c === 'gregory') c = 'gregorian'; }
      else if (s.type === 'dateTimeField') { if (!DATE_TIME_FIELDS.includes(c)) bad(); }
      const name = __wtIntlDisplayName(s.dataLocale, s.type, ['long', 'short', 'narrow'].indexOf(s.style),
                                       s.languageDisplay === 'dialect', c);
      if (name !== undefined) return name;
      return s.fallback === 'code' ? (s.type === 'calendar' && c === 'gregorian' ? 'gregory' : c) : undefined;
    },
    resolvedOptions() {
      const s = slotOf(this, 'DisplayNames', 'resolvedOptions');
      const o = { locale: s.locale, style: s.style, type: s.type, fallback: s.fallback };
      if (s.type === 'language') o.languageDisplay = s.languageDisplay;
      return o;
    },
  });
  tag(DisplayNames.prototype, 'Intl.DisplayNames');

  // --- Intl.Locale ----------------------------------------------------------------

  // A canonical tag's pieces: language, script, region, variants, the
  // Unicode extension's keywords, and any other extensions as they were.
  const parseTag = t => {
    const subtags = t.split('-');
    const p = { language: subtags[0], script: undefined, region: undefined, variants: [], keywords: {}, attributes: [], rest: [] };
    let i = 1;
    if (/^[a-z]{4}$/i.test(subtags[i] || '')) p.script = subtags[i++];
    if (/^([a-z]{2}|\d{3})$/i.test(subtags[i] || '')) p.region = subtags[i++];
    while (i < subtags.length && subtags[i].length > 1) p.variants.push(subtags[i++]);
    while (i < subtags.length) {
      if (subtags[i] === 'u') {
        i++;
        let key = null;
        for (; i < subtags.length && subtags[i].length > 1; i++) {
          if (subtags[i].length === 2) { key = subtags[i]; p.keywords[key] = ''; }
          else if (key) p.keywords[key] = p.keywords[key] ? p.keywords[key] + '-' + subtags[i] : subtags[i];
          else p.attributes.push(subtags[i]);
        }
      }
      else p.rest.push(subtags[i++]);
    }
    return p;
  };
  const baseNameOf = p => [p.language, p.script, p.region, ...p.variants].filter(Boolean).join('-');
  const buildTag = p => {
    let t = baseNameOf(p);
    const keys = Object.keys(p.keywords).sort();
    if (keys.length || p.attributes.length)
      t += '-u' + p.attributes.map(a => '-' + a).join('') + keys.map(k => '-' + k + (p.keywords[k] ? '-' + p.keywords[k] : '')).join('');
    if (p.rest.length) t += '-' + p.rest.join('-');
    return t;
  };

  function Locale(tagArg, options) {
    if (!new.target) throw new TypeError("Constructor Intl.Locale requires 'new'");
    if (typeof tagArg !== 'string' && (typeof tagArg !== 'object' || tagArg === null))
      throw new TypeError('First argument to Intl.Locale constructor can\'t be empty or missing');
    const own = slots.get(tagArg);
    let t = own && own.kind === 'Locale' ? own.tag : String(tagArg);
    const c = __wtIntlCanonical(t);
    if (c === null) throw new RangeError('Incorrect locale information provided');
    const opts = toOptions(options);
    const p = parseTag(c);
    const C = 'Intl.Locale';
    const language = getOption(opts, 'language', 'string');
    if (language !== undefined) { if (!/^[a-z]{2,3}$|^[a-z]{5,8}$/i.test(language)) throw new RangeError('Incorrect locale information provided'); p.language = language; }
    const script = getOption(opts, 'script', 'string');
    if (script !== undefined) { if (!/^[a-z]{4}$/i.test(script)) throw new RangeError('Incorrect locale information provided'); p.script = script; }
    const region = getOption(opts, 'region', 'string');
    if (region !== undefined) { if (!/^([a-z]{2}|\d{3})$/i.test(region)) throw new RangeError('Incorrect locale information provided'); p.region = region; }
    const set = (key, v) => { if (v !== undefined) p.keywords[key] = v; };
    set('ca', checkTypeName(getOption(opts, 'calendar', 'string'), 'calendar'));
    set('co', checkTypeName(getOption(opts, 'collation', 'string'), 'collation'));
    set('hc', getOption(opts, 'hourCycle', 'string', ['h11', 'h12', 'h23', 'h24'], undefined, C));
    set('kf', getOption(opts, 'caseFirst', 'string', ['upper', 'lower', 'false'], undefined, C));
    const kn = getOption(opts, 'numeric', 'boolean');
    if (kn !== undefined) p.keywords.kn = kn ? '' : 'false';
    set('nu', checkTypeName(getOption(opts, 'numberingSystem', 'string'), 'numberingSystem'));
    const final = __wtIntlCanonical(buildTag(p));
    if (final === null) throw new RangeError('Incorrect locale information provided');
    slots.set(this, { kind: 'Locale', tag: final, parts: parseTag(final) });
  }
  const localeSlot = (obj, m) => slotOf(obj, 'Locale', m);
  const keyword = k => function () {
    const v = localeSlot(this, k).parts.keywords[k];
    return v === undefined ? undefined : v === '' ? 'true' : v;
  };
  getters(Locale.prototype, {
    baseName() { return baseNameOf(localeSlot(this, 'baseName').parts); },
    language() { return localeSlot(this, 'language').parts.language; },
    script() { return localeSlot(this, 'script').parts.script; },
    region() { return localeSlot(this, 'region').parts.region; },
    calendar: keyword('ca'),
    collation: keyword('co'),
    hourCycle: keyword('hc'),
    caseFirst: keyword('kf'),
    numberingSystem: keyword('nu'),
    numeric() { const v = localeSlot(this, 'numeric').parts.keywords.kn; return v !== undefined && v !== 'false'; },
  });
  const likely = (obj, max) => {
    const s = localeSlot(obj, max ? 'maximize' : 'minimize');
    const p = parseTag(s.tag);
    const base = parseTag(__wtIntlLikely(baseNameOf(p), max));
    p.language = base.language; p.script = base.script; p.region = base.region;
    return new Locale(buildTag(p));
  };
  hidden(Locale.prototype, {
    maximize() { return likely(this, true); },
    minimize() { return likely(this, false); },
    toString() { return localeSlot(this, 'toString').tag; },
    toJSON() { return localeSlot(this, 'toJSON').tag; },
  });
  tag(Locale.prototype, 'Intl.Locale');
)JS" R"JS(
  // --- Intl -------------------------------------------------------------------

  const UNIT_LIST = SIMPLE_UNITS.slice();
  const Intl = {};
  hidden(Intl, {
    getCanonicalLocales(locales) { return canonicalizeLocaleList(locales); },
    supportedValuesOf(key) {
      const k = String(key);
      let list;
      if (k === 'calendar') list = __wtIntlSupported('calendar').map(c => c === 'gregorian' ? 'gregory' : c === 'ethiopic-amete-alem' ? 'ethioaa' : c);
      else if (k === 'collation') list = __wtIntlSupported('collation').map(bcp47Collation).filter(c => c !== 'standard' && c !== 'search');
      else if (k === 'currency') list = __wtIntlSupported('currency').map(c => c.toUpperCase());
      else if (k === 'numberingSystem') list = __wtIntlSupported('numberingSystem').filter(n => !['finance', 'native', 'traditio'].includes(n));
      else if (k === 'timeZone') list = __wtIntlSupported('timeZone').filter(z => z.includes('/') && __wtIntlTimeZone(z) === z && !z.startsWith('Etc/') && !z.startsWith('SystemV/')).concat(['UTC']);
      else if (k === 'unit') list = UNIT_LIST;
      else throw new RangeError('Invalid key : ' + k);
      return [...new Set(list)].sort();
    },
    Collator, DateTimeFormat, DisplayNames, ListFormat, Locale, NumberFormat, PluralRules, RelativeTimeFormat, Segmenter,
  });
  tag(Intl, 'Intl');
  for (const C of [Collator, DateTimeFormat, DisplayNames, ListFormat, Locale, NumberFormat, PluralRules, RelativeTimeFormat, Segmenter]) {
    Object.defineProperty(C.prototype, 'constructor', { value: C, writable: true, configurable: true });
    // The spec's lengths: the arguments are all optional but these.
    Object.defineProperty(C, 'length', { value: C === DisplayNames ? 2 : C === Locale ? 1 : 0, configurable: true });
  }
  hidden(g, { Intl });

  // --- The locale-sensitive methods of the built-ins ---------------------------
  // toLocaleString and friends with no options are called in loops (sorting
  // a table, formatting a list), so their formatter for the default options
  // is kept per locale argument.

  const cached = (cache, locales, options, make) => {
    if (options !== undefined || (locales !== undefined && typeof locales !== 'string')) return make();
    let f = cache.get(locales);
    if (!f) { f = make(); cache.set(locales, f); }
    return f;
  };
  const numberCache = new Map(), collatorCache = new Map();
  const dateCaches = { any: new Map(), date: new Map(), time: new Map() };

  const thisNumber = (v, name) => {
    if (typeof v === 'number') return v;
    if (v instanceof Number) return Number.prototype.valueOf.call(v);
    throw new TypeError('Number.prototype.' + name + ' requires that \'this\' be a Number');
  };
  hidden(Number.prototype, {
    toLocaleString(locales, options) {
      const x = thisNumber(this, 'toLocaleString');
      return cached(numberCache, locales, options, () => new NumberFormat(locales, options)).format(x);
    },
  });
  if (typeof BigInt === 'function') {
    hidden(BigInt.prototype, {
      toLocaleString(locales, options) {
        const x = BigInt.prototype.valueOf.call(this);
        return cached(numberCache, locales, options, () => new NumberFormat(locales, options)).format(x);
      },
    });
  }

  const dateMethod = (name, required, defaults) => function (locales, options) {
    const t = Date.prototype.getTime.call(this);
    if (Number.isNaN(t)) return 'Invalid Date';
    const make = () => createDateTimeFormat(Object.create(DateTimeFormat.prototype), locales, options, required, defaults);
    return cached(dateCaches[required], locales, options, make).format(t);
  };
  hidden(Date.prototype, {
    toLocaleString: dateMethod('toLocaleString', 'any', 'all'),
    toLocaleDateString: dateMethod('toLocaleDateString', 'date', 'date'),
    toLocaleTimeString: dateMethod('toLocaleTimeString', 'time', 'time'),
  });

  const firstLocale = locales => {
    const list = canonicalizeLocaleList(locales);
    return list.length ? list[0] : defaultLocale;
  };
  let defaultCompare = null; // a sort calls localeCompare once per pair: the common case goes straight there
  hidden(String.prototype, {
    localeCompare(that, locales, options) {
      if (this === undefined || this === null) throw new TypeError('String.prototype.localeCompare called on null or undefined');
      if (locales === undefined && options === undefined) {
        if (!defaultCompare) defaultCompare = new Collator().compare;
        return defaultCompare(String(this), String(that));
      }
      return cached(collatorCache, locales, options, () => new Collator(locales, options)).compare(String(this), String(that));
    },
    toLocaleUpperCase(locales) {
      if (this === undefined || this === null) throw new TypeError('String.prototype.toLocaleUpperCase called on null or undefined');
      return __wtIntlCase(String(this), firstLocale(locales), true);
    },
    toLocaleLowerCase(locales) {
      if (this === undefined || this === null) throw new TypeError('String.prototype.toLocaleLowerCase called on null or undefined');
      return __wtIntlCase(String(this), firstLocale(locales), false);
    },
  });
})();
)JS"
