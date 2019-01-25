// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTL_SUPPORT
#error Internationalization is expected to be enabled.
#endif  // V8_INTL_SUPPORT

#include "src/objects/js-date-time-format.h"

#include <memory>
#include <string>
#include <vector>

#include "src/date.h"
#include "src/heap/factory.h"
#include "src/isolate.h"
#include "src/objects/intl-objects.h"
#include "src/objects/js-date-time-format-inl.h"

#include "unicode/calendar.h"
#include "unicode/dtptngen.h"
#include "unicode/gregocal.h"
#include "unicode/smpdtfmt.h"
#include "unicode/unistr.h"

namespace v8 {
namespace internal {

namespace {

class PatternMap {
 public:
  PatternMap(std::string pattern, std::string value)
      : pattern(std::move(pattern)), value(std::move(value)) {}
  virtual ~PatternMap() = default;
  std::string pattern;
  std::string value;
};

class PatternItem {
 public:
  PatternItem(const std::string property, std::vector<PatternMap> pairs,
              std::vector<const char*> allowed_values)
      : property(std::move(property)),
        pairs(std::move(pairs)),
        allowed_values(allowed_values) {}
  virtual ~PatternItem() = default;

  const std::string property;
  // It is important for the pattern in the pairs from longer one to shorter one
  // if the longer one contains substring of an shorter one.
  std::vector<PatternMap> pairs;
  std::vector<const char*> allowed_values;
};

const std::vector<PatternItem> GetPatternItems() {
  const std::vector<const char*> kLongShort = {"long", "short"};
  const std::vector<const char*> kNarrowLongShort = {"narrow", "long", "short"};
  const std::vector<const char*> k2DigitNumeric = {"2-digit", "numeric"};
  const std::vector<const char*> kNarrowLongShort2DigitNumeric = {
      "narrow", "long", "short", "2-digit", "numeric"};
  const std::vector<PatternItem> kPatternItems = {
      PatternItem("weekday",
                  {{"EEEEE", "narrow"},
                   {"EEEE", "long"},
                   {"EEE", "short"},
                   {"ccccc", "narrow"},
                   {"cccc", "long"},
                   {"ccc", "short"}},
                  kNarrowLongShort),
      PatternItem("era",
                  {{"GGGGG", "narrow"}, {"GGGG", "long"}, {"GGG", "short"}},
                  kNarrowLongShort),
      PatternItem("year", {{"yy", "2-digit"}, {"y", "numeric"}},
                  k2DigitNumeric),
      // Sometimes we get L instead of M for month - standalone name.
      PatternItem("month",
                  {{"MMMMM", "narrow"},
                   {"MMMM", "long"},
                   {"MMM", "short"},
                   {"MM", "2-digit"},
                   {"M", "numeric"},
                   {"LLLLL", "narrow"},
                   {"LLLL", "long"},
                   {"LLL", "short"},
                   {"LL", "2-digit"},
                   {"L", "numeric"}},
                  kNarrowLongShort2DigitNumeric),
      PatternItem("day", {{"dd", "2-digit"}, {"d", "numeric"}}, k2DigitNumeric),
      PatternItem("hour",
                  {{"HH", "2-digit"},
                   {"H", "numeric"},
                   {"hh", "2-digit"},
                   {"h", "numeric"},
                   {"kk", "2-digit"},
                   {"k", "numeric"},
                   {"KK", "2-digit"},
                   {"K", "numeric"}},
                  k2DigitNumeric),
      PatternItem("minute", {{"mm", "2-digit"}, {"m", "numeric"}},
                  k2DigitNumeric),
      PatternItem("second", {{"ss", "2-digit"}, {"s", "numeric"}},
                  k2DigitNumeric),
      PatternItem("timeZoneName", {{"zzzz", "long"}, {"z", "short"}},
                  kLongShort)};
  return kPatternItems;
}

class PatternData {
 public:
  PatternData(const std::string property, std::vector<PatternMap> pairs,
              std::vector<const char*> allowed_values)
      : property(std::move(property)), allowed_values(allowed_values) {
    for (const auto& pair : pairs) {
      map.insert(std::make_pair(pair.value, pair.pattern));
    }
  }
  virtual ~PatternData() = default;

  const std::string property;
  std::map<const std::string, const std::string> map;
  std::vector<const char*> allowed_values;
};

const std::vector<PatternData> CreateCommonData(const PatternData& hour_data) {
  std::vector<PatternData> build;
  for (const PatternItem& item : GetPatternItems()) {
    if (item.property == "hour") {
      build.push_back(hour_data);
    } else {
      build.push_back(
          PatternData(item.property, item.pairs, item.allowed_values));
    }
  }
  return build;
}

const std::vector<PatternData> CreateData(const char* digit2,
                                          const char* numeric) {
  return CreateCommonData(
      PatternData("hour", {{digit2, "2-digit"}, {numeric, "numeric"}},
                  {"2-digit", "numeric"}));
}

// According to "Date Field Symbol Table" in
// http://userguide.icu-project.org/formatparse/datetime
// Symbol | Meaning              | Example(s)
//   h      hour in am/pm (1~12)    h    7
//                                  hh   07
//   H      hour in day (0~23)      H    0
//                                  HH   00
//   k      hour in day (1~24)      k    24
//                                  kk   24
//   K      hour in am/pm (0~11)    K    0
//                                  KK   00
const std::vector<PatternData> GetPatternData(Intl::HourCycle hour_cycle) {
  const std::vector<PatternData> data = CreateData("jj", "j");
  const std::vector<PatternData> data_h11 = CreateData("KK", "K");
  const std::vector<PatternData> data_h12 = CreateData("hh", "h");
  const std::vector<PatternData> data_h23 = CreateData("HH", "H");
  const std::vector<PatternData> data_h24 = CreateData("kk", "k");
  switch (hour_cycle) {
    case Intl::HourCycle::kH11:
      return data_h11;
    case Intl::HourCycle::kH12:
      return data_h12;
    case Intl::HourCycle::kH23:
      return data_h23;
    case Intl::HourCycle::kH24:
      return data_h24;
    case Intl::HourCycle::kUndefined:
      return data;
    default:
      UNREACHABLE();
  }
}

std::string GetGMTTzID(Isolate* isolate, const std::string& input) {
  std::string ret = "Etc/GMT";
  switch (input.length()) {
    case 8:
      if (input[7] == '0') return ret + '0';
      break;
    case 9:
      if ((input[7] == '+' || input[7] == '-') &&
          IsInRange(input[8], '0', '9')) {
        return ret + input[7] + input[8];
      }
      break;
    case 10:
      if ((input[7] == '+' || input[7] == '-') && (input[8] == '1') &&
          IsInRange(input[9], '0', '4')) {
        return ret + input[7] + input[8] + input[9];
      }
      break;
  }
  return "";
}

// Locale independenty version of isalpha for ascii range. This will return
// false if the ch is alpha but not in ascii range.
bool IsAsciiAlpha(char ch) {
  return IsInRange(ch, 'A', 'Z') || IsInRange(ch, 'a', 'z');
}

// Locale independent toupper for ascii range. This will not return İ (dotted I)
// for i under Turkish locale while std::toupper may.
char LocaleIndependentAsciiToUpper(char ch) {
  return (IsInRange(ch, 'a', 'z')) ? (ch - 'a' + 'A') : ch;
}

// Locale independent tolower for ascii range.
char LocaleIndependentAsciiToLower(char ch) {
  return (IsInRange(ch, 'A', 'Z')) ? (ch - 'A' + 'a') : ch;
}

// Returns titlecased location, bueNos_airES -> Buenos_Aires
// or ho_cHi_minH -> Ho_Chi_Minh. It is locale-agnostic and only
// deals with ASCII only characters.
// 'of', 'au' and 'es' are special-cased and lowercased.
// ICU's timezone parsing is case sensitive, but ECMAScript is case insensitive
std::string ToTitleCaseTimezoneLocation(Isolate* isolate,
                                        const std::string& input) {
  std::string title_cased;
  int word_length = 0;
  for (char ch : input) {
    // Convert first char to upper case, the rest to lower case
    if (IsAsciiAlpha(ch)) {
      title_cased += word_length == 0 ? LocaleIndependentAsciiToUpper(ch)
                                      : LocaleIndependentAsciiToLower(ch);
      word_length++;
    } else if (ch == '_' || ch == '-' || ch == '/') {
      // Special case Au/Es/Of to be lower case.
      if (word_length == 2) {
        size_t pos = title_cased.length() - 2;
        std::string substr = title_cased.substr(pos, 2);
        if (substr == "Of" || substr == "Es" || substr == "Au") {
          title_cased[pos] = LocaleIndependentAsciiToLower(title_cased[pos]);
        }
      }
      title_cased += ch;
      word_length = 0;
    } else {
      // Invalid input
      return std::string();
    }
  }
  return title_cased;
}

}  // namespace

std::string JSDateTimeFormat::CanonicalizeTimeZoneID(Isolate* isolate,
                                                     const std::string& input) {
  std::string upper = input;
  transform(upper.begin(), upper.end(), upper.begin(),
            LocaleIndependentAsciiToUpper);
  if (upper == "UTC" || upper == "GMT" || upper == "ETC/UTC" ||
      upper == "ETC/GMT") {
    return "UTC";
  }
  // We expect only _, '-' and / beside ASCII letters.
  // All inputs should conform to Area/Location(/Location)*, or Etc/GMT* .
  // TODO(jshin): 1. Support 'GB-Eire", 'EST5EDT", "ROK', 'US/*', 'NZ' and many
  // other aliases/linked names when moving timezone validation code to C++.
  // See crbug.com/364374 and crbug.com/v8/8007 .
  // 2. Resolve the difference betwee CLDR/ICU and IANA time zone db.
  // See http://unicode.org/cldr/trac/ticket/9892 and crbug.com/645807 .
  if (strncmp(upper.c_str(), "ETC/GMT", 7) == 0) {
    return GetGMTTzID(isolate, input);
  }
  return ToTitleCaseTimezoneLocation(isolate, input);
}

// ecma402 #sec-intl.datetimeformat.prototype.resolvedoptions
MaybeHandle<JSObject> JSDateTimeFormat::ResolvedOptions(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format) {
  Factory* factory = isolate->factory();
  // 4. Let options be ! ObjectCreate(%ObjectPrototype%).
  Handle<JSObject> options = factory->NewJSObject(isolate->object_function());

  Handle<Object> resolved_obj;

  CHECK(!date_time_format->icu_locale().is_null());
  CHECK_NOT_NULL(date_time_format->icu_locale()->raw());
  icu::Locale* icu_locale = date_time_format->icu_locale()->raw();
  Maybe<std::string> maybe_locale_str = Intl::ToLanguageTag(*icu_locale);
  MAYBE_RETURN(maybe_locale_str, MaybeHandle<JSObject>());
  std::string locale_str = maybe_locale_str.FromJust();
  Handle<String> locale =
      factory->NewStringFromAsciiChecked(locale_str.c_str());

  icu::SimpleDateFormat* icu_simple_date_format =
      date_time_format->icu_simple_date_format()->raw();
  // calendar
  const icu::Calendar* calendar = icu_simple_date_format->getCalendar();
  // getType() returns legacy calendar type name instead of LDML/BCP47 calendar
  // key values. intl.js maps them to BCP47 values for key "ca".
  // TODO(jshin): Consider doing it here, instead.
  std::string calendar_str = calendar->getType();

  // Maps ICU calendar names to LDML/BCP47 types for key 'ca'.
  // See typeMap section in third_party/icu/source/data/misc/keyTypeData.txt
  // and
  // http://www.unicode.org/repos/cldr/tags/latest/common/bcp47/calendar.xml
  if (calendar_str == "gregorian") {
    calendar_str = "gregory";
  } else if (calendar_str == "ethiopic-amete-alem") {
    calendar_str = "ethioaa";
  }

  const icu::TimeZone& tz = calendar->getTimeZone();
  icu::UnicodeString time_zone;
  tz.getID(time_zone);
  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString canonical_time_zone;
  icu::TimeZone::getCanonicalID(time_zone, canonical_time_zone, status);
  Handle<Object> timezone_value;
  if (U_SUCCESS(status)) {
    // In CLDR (http://unicode.org/cldr/trac/ticket/9943), Etc/UTC is made
    // a separate timezone ID from Etc/GMT even though they're still the same
    // timezone. We have Etc/UTC because 'UTC', 'Etc/Universal',
    // 'Etc/Zulu' and others are turned to 'Etc/UTC' by ICU. Etc/GMT comes
    // from Etc/GMT0, Etc/GMT+0, Etc/GMT-0, Etc/Greenwich.
    // ecma402#sec-canonicalizetimezonename step 3
    if (canonical_time_zone == UNICODE_STRING_SIMPLE("Etc/UTC") ||
        canonical_time_zone == UNICODE_STRING_SIMPLE("Etc/GMT")) {
      timezone_value = factory->UTC_string();
    } else {
      ASSIGN_RETURN_ON_EXCEPTION(isolate, timezone_value,
                                 Intl::ToString(isolate, canonical_time_zone),
                                 JSObject);
    }
  } else {
    // Somehow on Windows we will reach here.
    timezone_value = factory->undefined_value();
  }

  // Ugly hack. ICU doesn't expose numbering system in any way, so we have
  // to assume that for given locale NumberingSystem constructor produces the
  // same digits as NumberFormat/Calendar would.
  // Tracked by https://unicode-org.atlassian.net/browse/ICU-13431
  std::string numbering_system = Intl::GetNumberingSystem(*icu_locale);

  icu::UnicodeString pattern_unicode;
  icu_simple_date_format->toPattern(pattern_unicode);
  std::string pattern;
  pattern_unicode.toUTF8String(pattern);

  // 5. For each row of Table 6, except the header row, in table order, do
  // Table 6: Resolved Options of DateTimeFormat Instances
  //  Internal Slot          Property
  //    [[Locale]]           "locale"
  //    [[Calendar]]         "calendar"
  //    [[NumberingSystem]]  "numberingSystem"
  //    [[TimeZone]]         "timeZone"
  //    [[HourCycle]]        "hourCycle"
  //                         "hour12"
  //    [[Weekday]]          "weekday"
  //    [[Era]]              "era"
  //    [[Year]]             "year"
  //    [[Month]]            "month"
  //    [[Day]]              "day"
  //    [[Hour]]             "hour"
  //    [[Minute]]           "minute"
  //    [[Second]]           "second"
  //    [[TimeZoneName]]     "timeZoneName"
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->locale_string(), locale, kDontThrow)
            .FromJust());
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->calendar_string(),
            factory->NewStringFromAsciiChecked(calendar_str.c_str()),
            kDontThrow)
            .FromJust());
  if (!numbering_system.empty()) {
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->numberingSystem_string(),
              factory->NewStringFromAsciiChecked(numbering_system.c_str()),
              kDontThrow)
              .FromJust());
  }
  CHECK(JSReceiver::CreateDataProperty(isolate, options,
                                       factory->timeZone_string(),
                                       timezone_value, kDontThrow)
            .FromJust());

  // 5.b.i. Let hc be dtf.[[HourCycle]].
  Intl::HourCycle hc = date_time_format->hour_cycle();

  if (hc != Intl::HourCycle::kUndefined) {
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->hourCycle_string(),
              date_time_format->HourCycleAsString(), kDontThrow)
              .FromJust());
    switch (hc) {
      //  ii. If hc is "h11" or "h12", let v be true.
      case Intl::HourCycle::kH11:
      case Intl::HourCycle::kH12:
        CHECK(JSReceiver::CreateDataProperty(isolate, options,
                                             factory->hour12_string(),
                                             factory->true_value(), kDontThrow)
                  .FromJust());
        break;
      // iii. Else if, hc is "h23" or "h24", let v be false.
      case Intl::HourCycle::kH23:
      case Intl::HourCycle::kH24:
        CHECK(JSReceiver::CreateDataProperty(isolate, options,
                                             factory->hour12_string(),
                                             factory->false_value(), kDontThrow)
                  .FromJust());
        break;
      // iv. Else, let v be undefined.
      case Intl::HourCycle::kUndefined:
        break;
    }
  }

  for (const auto& item : GetPatternItems()) {
    for (const auto& pair : item.pairs) {
      if (pattern.find(pair.pattern) != std::string::npos) {
        CHECK(JSReceiver::CreateDataProperty(
                  isolate, options,
                  factory->NewStringFromAsciiChecked(item.property.c_str()),
                  factory->NewStringFromAsciiChecked(pair.value.c_str()),
                  kDontThrow)
                  .FromJust());
        break;
      }
    }
  }

  return options;
}

namespace {

// ecma402/#sec-formatdatetime
// FormatDateTime( dateTimeFormat, x )
MaybeHandle<String> FormatDateTime(Isolate* isolate,
                                   const icu::SimpleDateFormat& date_format,
                                   double x) {
  double date_value = DateCache::TimeClip(x);
  if (std::isnan(date_value)) {
    THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kInvalidTimeValue),
                    String);
  }

  icu::UnicodeString result;
  date_format.format(date_value, result);

  return Intl::ToString(isolate, result);
}

}  // namespace

// ecma402/#sec-datetime-format-functions
// DateTime Format Functions
MaybeHandle<String> JSDateTimeFormat::DateTimeFormat(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format,
    Handle<Object> date) {
  // 2. Assert: Type(dtf) is Object and dtf has an [[InitializedDateTimeFormat]]
  // internal slot.

  // 3. If date is not provided or is undefined, then
  double x;
  if (date->IsUndefined()) {
    // 3.a Let x be Call(%Date_now%, undefined).
    x = JSDate::CurrentTimeValue(isolate);
  } else {
    // 4. Else,
    //    a. Let x be ? ToNumber(date).
    ASSIGN_RETURN_ON_EXCEPTION(isolate, date, Object::ToNumber(isolate, date),
                               String);
    CHECK(date->IsNumber());
    x = date->Number();
  }
  // 5. Return FormatDateTime(dtf, x).
  icu::SimpleDateFormat* format =
      date_time_format->icu_simple_date_format()->raw();
  return FormatDateTime(isolate, *format, x);
}

namespace {
Isolate::ICUObjectCacheType ConvertToCacheType(
    JSDateTimeFormat::DefaultsOption type) {
  switch (type) {
    case JSDateTimeFormat::DefaultsOption::kDate:
      return Isolate::ICUObjectCacheType::kDefaultSimpleDateFormatForDate;
    case JSDateTimeFormat::DefaultsOption::kTime:
      return Isolate::ICUObjectCacheType::kDefaultSimpleDateFormatForTime;
    case JSDateTimeFormat::DefaultsOption::kAll:
      return Isolate::ICUObjectCacheType::kDefaultSimpleDateFormat;
  }
}
}  // namespace

MaybeHandle<String> JSDateTimeFormat::ToLocaleDateTime(
    Isolate* isolate, Handle<Object> date, Handle<Object> locales,
    Handle<Object> options, RequiredOption required, DefaultsOption defaults) {
  Isolate::ICUObjectCacheType cache_type = ConvertToCacheType(defaults);

  Factory* factory = isolate->factory();
  // 1. Let x be ? thisTimeValue(this value);
  if (!date->IsJSDate()) {
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                                 factory->Date_string()),
                    String);
  }

  double const x = Handle<JSDate>::cast(date)->value()->Number();
  // 2. If x is NaN, return "Invalid Date"
  if (std::isnan(x)) {
    return factory->Invalid_Date_string();
  }

  // We only cache the instance when both locales and options are undefined,
  // as that is the only case when the specified side-effects of examining
  // those arguments are unobservable.
  bool can_cache =
      locales->IsUndefined(isolate) && options->IsUndefined(isolate);
  if (can_cache) {
    // Both locales and options are undefined, check the cache.
    icu::SimpleDateFormat* cached_icu_simple_date_format =
        static_cast<icu::SimpleDateFormat*>(
            isolate->get_cached_icu_object(cache_type));
    if (cached_icu_simple_date_format != nullptr) {
      return FormatDateTime(isolate, *cached_icu_simple_date_format, x);
    }
  }
  // 3. Let options be ? ToDateTimeOptions(options, required, defaults).
  Handle<JSObject> internal_options;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, internal_options,
      ToDateTimeOptions(isolate, options, required, defaults), String);

  // 4. Let dateFormat be ? Construct(%DateTimeFormat%, « locales, options »).
  Handle<JSFunction> constructor = Handle<JSFunction>(
      JSFunction::cast(isolate->context()
                           ->native_context()
                           ->intl_date_time_format_function()),
      isolate);
  Handle<JSObject> obj;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, obj,
      JSObject::New(constructor, constructor, Handle<AllocationSite>::null()),
      String);
  Handle<JSDateTimeFormat> date_time_format;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, date_time_format,
      JSDateTimeFormat::Initialize(isolate, Handle<JSDateTimeFormat>::cast(obj),
                                   locales, internal_options),
      String);

  if (can_cache) {
    isolate->set_icu_object_in_cache(
        cache_type, std::static_pointer_cast<icu::UObject>(
                        date_time_format->icu_simple_date_format()->get()));
  }
  // 5. Return FormatDateTime(dateFormat, x).
  icu::SimpleDateFormat* format =
      date_time_format->icu_simple_date_format()->raw();
  return FormatDateTime(isolate, *format, x);
}

namespace {

Maybe<bool> IsPropertyUndefined(Isolate* isolate, Handle<JSObject> options,
                                const char* property) {
  Factory* factory = isolate->factory();
  // i. Let prop be the property name.
  // ii. Let value be ? Get(options, prop).
  Handle<Object> value;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, value,
      Object::GetPropertyOrElement(
          isolate, options, factory->NewStringFromAsciiChecked(property)),
      Nothing<bool>());
  return Just(value->IsUndefined(isolate));
}

Maybe<bool> NeedsDefault(Isolate* isolate, Handle<JSObject> options,
                         const std::vector<std::string>& props) {
  bool needs_default = true;
  for (const auto& prop : props) {
    //  i. Let prop be the property name.
    // ii. Let value be ? Get(options, prop)
    Maybe<bool> maybe_undefined =
        IsPropertyUndefined(isolate, options, prop.c_str());
    MAYBE_RETURN(maybe_undefined, Nothing<bool>());
    // iii. If value is not undefined, let needDefaults be false.
    if (!maybe_undefined.FromJust()) {
      needs_default = false;
    }
  }
  return Just(needs_default);
}

Maybe<bool> CreateDefault(Isolate* isolate, Handle<JSObject> options,
                          const std::vector<std::string>& props) {
  Factory* factory = isolate->factory();
  // i. Perform ? CreateDataPropertyOrThrow(options, prop, "numeric").
  for (const auto& prop : props) {
    MAYBE_RETURN(
        JSReceiver::CreateDataProperty(
            isolate, options, factory->NewStringFromAsciiChecked(prop.c_str()),
            factory->numeric_string(), kThrowOnError),
        Nothing<bool>());
  }
  return Just(true);
}

}  // namespace

// ecma-402/#sec-todatetimeoptions
MaybeHandle<JSObject> JSDateTimeFormat::ToDateTimeOptions(
    Isolate* isolate, Handle<Object> input_options, RequiredOption required,
    DefaultsOption defaults) {
  Factory* factory = isolate->factory();
  // 1. If options is undefined, let options be null; otherwise let options be ?
  //    ToObject(options).
  Handle<JSObject> options;
  if (input_options->IsUndefined(isolate)) {
    options = factory->NewJSObjectWithNullProto();
  } else {
    Handle<JSReceiver> options_obj;
    ASSIGN_RETURN_ON_EXCEPTION(isolate, options_obj,
                               Object::ToObject(isolate, input_options),
                               JSObject);
    // 2. Let options be ObjectCreate(options).
    ASSIGN_RETURN_ON_EXCEPTION(isolate, options,
                               JSObject::ObjectCreate(isolate, options_obj),
                               JSObject);
  }

  // 3. Let needDefaults be true.
  bool needs_default = true;

  // 4. If required is "date" or "any", then
  if (required == RequiredOption::kAny || required == RequiredOption::kDate) {
    // a. For each of the property names "weekday", "year", "month", "day", do
    const std::vector<std::string> list({"weekday", "year", "month", "day"});
    Maybe<bool> maybe_needs_default = NeedsDefault(isolate, options, list);
    MAYBE_RETURN(maybe_needs_default, Handle<JSObject>());
    needs_default = maybe_needs_default.FromJust();
  }

  // 5. If required is "time" or "any", then
  if (required == RequiredOption::kAny || required == RequiredOption::kTime) {
    // a. For each of the property names "hour", "minute", "second", do
    const std::vector<std::string> list({"hour", "minute", "second"});
    Maybe<bool> maybe_needs_default = NeedsDefault(isolate, options, list);
    MAYBE_RETURN(maybe_needs_default, Handle<JSObject>());
    needs_default &= maybe_needs_default.FromJust();
  }

  // 6. If needDefaults is true and defaults is either "date" or "all", then
  if (needs_default) {
    if (defaults == DefaultsOption::kAll || defaults == DefaultsOption::kDate) {
      // a. For each of the property names "year", "month", "day", do)
      const std::vector<std::string> list({"year", "month", "day"});
      MAYBE_RETURN(CreateDefault(isolate, options, list), Handle<JSObject>());
    }
    // 7. If needDefaults is true and defaults is either "time" or "all", then
    if (defaults == DefaultsOption::kAll || defaults == DefaultsOption::kTime) {
      // a. For each of the property names "hour", "minute", "second", do
      const std::vector<std::string> list({"hour", "minute", "second"});
      MAYBE_RETURN(CreateDefault(isolate, options, list), Handle<JSObject>());
    }
  }
  // 8. Return options.
  return options;
}

MaybeHandle<JSDateTimeFormat> JSDateTimeFormat::UnwrapDateTimeFormat(
    Isolate* isolate, Handle<JSReceiver> format_holder) {
  Handle<Context> native_context =
      Handle<Context>(isolate->context()->native_context(), isolate);
  Handle<JSFunction> constructor = Handle<JSFunction>(
      JSFunction::cast(native_context->intl_date_time_format_function()),
      isolate);
  Handle<Object> dtf;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, dtf,
      Intl::LegacyUnwrapReceiver(isolate, format_holder, constructor,
                                 format_holder->IsJSDateTimeFormat()),
      JSDateTimeFormat);
  // 2. If Type(dtf) is not Object or dtf does not have an
  //    [[InitializedDateTimeFormat]] internal slot, then
  if (!dtf->IsJSDateTimeFormat()) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kIncompatibleMethodReceiver,
                                 isolate->factory()->NewStringFromAsciiChecked(
                                     "UnwrapDateTimeFormat"),
                                 format_holder),
                    JSDateTimeFormat);
  }
  // 3. Return dtf.
  return Handle<JSDateTimeFormat>::cast(dtf);
}

namespace {

// ecma-402/#sec-isvalidtimezonename
bool IsValidTimeZoneName(const icu::TimeZone& tz) {
  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString id;
  tz.getID(id);
  icu::UnicodeString canonical;
  icu::TimeZone::getCanonicalID(id, canonical, status);
  return U_SUCCESS(status) &&
         canonical != icu::UnicodeString("Etc/Unknown", -1, US_INV);
}

std::unique_ptr<icu::TimeZone> CreateTimeZone(Isolate* isolate,
                                              const char* timezone) {
  // Create time zone as specified by the user. We have to re-create time zone
  // since calendar takes ownership.
  if (timezone == nullptr) {
    // 19.a. Else / Let timeZone be DefaultTimeZone().
    return std::unique_ptr<icu::TimeZone>(icu::TimeZone::createDefault());
  }
  std::string canonicalized =
      JSDateTimeFormat::CanonicalizeTimeZoneID(isolate, timezone);
  if (canonicalized.empty()) return std::unique_ptr<icu::TimeZone>();
  std::unique_ptr<icu::TimeZone> tz(
      icu::TimeZone::createTimeZone(canonicalized.c_str()));
  // 18.b If the result of IsValidTimeZoneName(timeZone) is false, then
  // i. Throw a RangeError exception.
  if (!IsValidTimeZoneName(*tz)) return std::unique_ptr<icu::TimeZone>();
  return tz;
}

std::unique_ptr<icu::Calendar> CreateCalendar(Isolate* isolate,
                                              const icu::Locale& icu_locale,
                                              const char* timezone) {
  std::unique_ptr<icu::TimeZone> tz = CreateTimeZone(isolate, timezone);
  if (tz.get() == nullptr) return std::unique_ptr<icu::Calendar>();

  // Create a calendar using locale, and apply time zone to it.
  UErrorCode status = U_ZERO_ERROR;
  std::unique_ptr<icu::Calendar> calendar(
      icu::Calendar::createInstance(tz.release(), icu_locale, status));
  CHECK(U_SUCCESS(status));
  CHECK_NOT_NULL(calendar.get());

  if (calendar->getDynamicClassID() ==
      icu::GregorianCalendar::getStaticClassID()) {
    icu::GregorianCalendar* gc =
        static_cast<icu::GregorianCalendar*>(calendar.get());
    UErrorCode status = U_ZERO_ERROR;
    // The beginning of ECMAScript time, namely -(2**53)
    const double start_of_time = -9007199254740992;
    gc->setGregorianChange(start_of_time, status);
    DCHECK(U_SUCCESS(status));
  }
  return calendar;
}

std::unique_ptr<icu::SimpleDateFormat> CreateICUDateFormat(
    Isolate* isolate, const icu::Locale& icu_locale,
    const std::string& skeleton) {
  // See https://github.com/tc39/ecma402/issues/225 . The best pattern
  // generation needs to be done in the base locale according to the
  // current spec however odd it may be. See also crbug.com/826549 .
  // This is a temporary work-around to get v8's external behavior to match
  // the current spec, but does not follow the spec provisions mentioned
  // in the above Ecma 402 issue.
  // TODO(jshin): The spec may need to be revised because using the base
  // locale for the pattern match is not quite right. Moreover, what to
  // do with 'related year' part when 'chinese/dangi' calendar is specified
  // has to be discussed. Revisit once the spec is clarified/revised.
  icu::Locale no_extension_locale(icu_locale.getBaseName());
  UErrorCode status = U_ZERO_ERROR;
  std::unique_ptr<icu::DateTimePatternGenerator> generator(
      icu::DateTimePatternGenerator::createInstance(no_extension_locale,
                                                    status));
  icu::UnicodeString pattern;
  if (U_SUCCESS(status)) {
    pattern =
        generator->getBestPattern(icu::UnicodeString(skeleton.c_str()), status);
  }

  // Make formatter from skeleton. Calendar and numbering system are added
  // to the locale as Unicode extension (if they were specified at all).
  status = U_ZERO_ERROR;
  std::unique_ptr<icu::SimpleDateFormat> date_format(
      new icu::SimpleDateFormat(pattern, icu_locale, status));
  if (U_FAILURE(status)) return std::unique_ptr<icu::SimpleDateFormat>();

  CHECK_NOT_NULL(date_format.get());
  return date_format;
}

Intl::HourCycle HourCycleDefault(icu::SimpleDateFormat* date_format) {
  icu::UnicodeString pattern;
  date_format->toPattern(pattern);
  if (pattern.indexOf('K') >= 0) {
    return Intl::HourCycle::kH11;
  } else if (pattern.indexOf('h') >= 0) {
    return Intl::HourCycle::kH12;
  } else if (pattern.indexOf('H') >= 0) {
    return Intl::HourCycle::kH23;
  } else if (pattern.indexOf('k') >= 0) {
    return Intl::HourCycle::kH24;
  }
  return Intl::HourCycle::kUndefined;
}

}  // namespace

enum FormatMatcherOption { kBestFit, kBasic };

// ecma402/#sec-initializedatetimeformat
MaybeHandle<JSDateTimeFormat> JSDateTimeFormat::Initialize(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format,
    Handle<Object> locales, Handle<Object> input_options) {
  date_time_format->set_flags(0);
  // 1. Let requestedLocales be ? CanonicalizeLocaleList(locales).
  Maybe<std::vector<std::string>> maybe_requested_locales =
      Intl::CanonicalizeLocaleList(isolate, locales);
  MAYBE_RETURN(maybe_requested_locales, Handle<JSDateTimeFormat>());
  std::vector<std::string> requested_locales =
      maybe_requested_locales.FromJust();
  // 2. Let options be ? ToDateTimeOptions(options, "any", "date").
  Handle<JSObject> options;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, options,
      JSDateTimeFormat::ToDateTimeOptions(
          isolate, input_options, RequiredOption::kAny, DefaultsOption::kDate),
      JSDateTimeFormat);

  // 4. Let matcher be ? GetOption(options, "localeMatcher", "string",
  // « "lookup", "best fit" », "best fit").
  // 5. Set opt.[[localeMatcher]] to matcher.
  Maybe<Intl::MatcherOption> maybe_locale_matcher =
      Intl::GetLocaleMatcher(isolate, options, "Intl.DateTimeFormat");
  MAYBE_RETURN(maybe_locale_matcher, MaybeHandle<JSDateTimeFormat>());
  Intl::MatcherOption locale_matcher = maybe_locale_matcher.FromJust();

  // 6. Let hour12 be ? GetOption(options, "hour12", "boolean", undefined,
  // undefined).
  bool hour12;
  Maybe<bool> maybe_get_hour12 = Intl::GetBoolOption(
      isolate, options, "hour12", "Intl.DateTimeFormat", &hour12);
  MAYBE_RETURN(maybe_get_hour12, Handle<JSDateTimeFormat>());

  // 7. Let hourCycle be ? GetOption(options, "hourCycle", "string", « "h11",
  // "h12", "h23", "h24" », undefined).
  Maybe<Intl::HourCycle> maybe_hour_cycle =
      Intl::GetHourCycle(isolate, options, "Intl.DateTimeFormat");
  MAYBE_RETURN(maybe_hour_cycle, MaybeHandle<JSDateTimeFormat>());
  Intl::HourCycle hour_cycle = maybe_hour_cycle.FromJust();

  // 8. If hour12 is not undefined, then
  if (maybe_get_hour12.FromJust()) {
    // a. Let hourCycle be null.
    hour_cycle = Intl::HourCycle::kUndefined;
  }
  // 9. Set opt.[[hc]] to hourCycle.

  // ecma402/#sec-intl.datetimeformat-internal-slots
  // The value of the [[RelevantExtensionKeys]] internal slot is
  // « "ca", "nu", "hc" ».
  std::set<std::string> relevant_extension_keys = {"nu", "ca", "hc"};

  // 10. Let localeData be %DateTimeFormat%.[[LocaleData]].
  // 11. Let r be ResolveLocale( %DateTimeFormat%.[[AvailableLocales]],
  //     requestedLocales, opt, %DateTimeFormat%.[[RelevantExtensionKeys]],
  //     localeData).
  //
  Intl::ResolvedLocale r = Intl::ResolveLocale(
      isolate, JSDateTimeFormat::GetAvailableLocales(), requested_locales,
      locale_matcher, relevant_extension_keys);

  icu::Locale icu_locale = r.icu_locale;
  DCHECK(!icu_locale.isBogus());

  if (!maybe_get_hour12.FromJust() &&
      hour_cycle == Intl::HourCycle::kUndefined) {
    auto hc_extension_it = r.extensions.find("hc");
    if (hc_extension_it != r.extensions.end()) {
      hour_cycle = Intl::ToHourCycle(hc_extension_it->second.c_str());
    }
  }

  // 17. Let timeZone be ? Get(options, "timeZone").
  const std::vector<const char*> empty_values;
  std::unique_ptr<char[]> timezone = nullptr;
  Maybe<bool> maybe_timezone =
      Intl::GetStringOption(isolate, options, "timeZone", empty_values,
                            "Intl.DateTimeFormat", &timezone);
  MAYBE_RETURN(maybe_timezone, Handle<JSDateTimeFormat>());

  std::unique_ptr<icu::Calendar> calendar(
      CreateCalendar(isolate, icu_locale, timezone.get()));

  // 18.b If the result of IsValidTimeZoneName(timeZone) is false, then
  // i. Throw a RangeError exception.
  if (calendar.get() == nullptr) {
    THROW_NEW_ERROR(isolate,
                    NewRangeError(MessageTemplate::kInvalidTimeZone,
                                  isolate->factory()->NewStringFromAsciiChecked(
                                      timezone.get())),
                    JSDateTimeFormat);
  }

  // 29.  If dateTimeFormat.[[Hour]] is not undefined, then
  if (hour_cycle == Intl::HourCycle::kUndefined) {
    // d. If hour12 is not undefined, then
    if (maybe_get_hour12.FromJust()) {
      // i. If hour12 is true, then
      if (hour12) {
        hour_cycle = Intl::HourCycle::kH12;
      } else {  // ii. Else,
        hour_cycle = Intl::HourCycle::kH23;
      }
    }
  }

  bool has_hour_option = false;
  // 22. For each row of Table 5, except the header row, do
  std::string skeleton;
  for (const PatternData& item : GetPatternData(hour_cycle)) {
    std::unique_ptr<char[]> input;
    // a. Let prop be the name given in the Property column of the row.
    // b. Let value be ? GetOption(options, prop, "string", « the strings given
    // in the Values column of the row », undefined).
    Maybe<bool> maybe_get_option = Intl::GetStringOption(
        isolate, options, item.property.c_str(), item.allowed_values,
        "Intl.DateTimeFormat", &input);
    MAYBE_RETURN(maybe_get_option, Handle<JSDateTimeFormat>());
    if (maybe_get_option.FromJust()) {
      if (item.property == "hour") {
        has_hour_option = true;
      }
      DCHECK_NOT_NULL(input.get());
      // c. Set opt.[[<prop>]] to value.
      skeleton += item.map.find(input.get())->second;
    }
  }

  enum FormatMatcherOption { kBestFit, kBasic };
  // We implement only best fit algorithm, but still need to check
  // if the formatMatcher values are in range.
  // 25. Let matcher be ? GetOption(options, "formatMatcher", "string",
  //     «  "basic", "best fit" », "best fit").
  Maybe<FormatMatcherOption> maybe_format_matcher =
      Intl::GetStringOption<FormatMatcherOption>(
          isolate, options, "formatMatcher", "Intl.DateTimeFormat",
          {"best fit", "basic"},
          {FormatMatcherOption::kBestFit, FormatMatcherOption::kBasic},
          FormatMatcherOption::kBestFit);
  MAYBE_RETURN(maybe_format_matcher, MaybeHandle<JSDateTimeFormat>());
  // TODO(ftang): uncomment the following line and handle format_matcher.
  // FormatMatcherOption format_matcher = maybe_format_matcher.FromJust();

  std::unique_ptr<icu::SimpleDateFormat> date_format(
      CreateICUDateFormat(isolate, icu_locale, skeleton));
  if (date_format.get() == nullptr) {
    // Remove extensions and try again.
    icu_locale = icu::Locale(icu_locale.getBaseName());
    date_format = CreateICUDateFormat(isolate, icu_locale, skeleton);
    if (date_format.get() == nullptr) {
      FATAL("Failed to create ICU date format, are ICU data files missing?");
    }
  }

  // The creation of Calendar depends on timeZone so we have to put 13 after 17.
  // Also date_format is not created until here.
  // 13. Set dateTimeFormat.[[Calendar]] to r.[[ca]].
  date_format->adoptCalendar(calendar.release());

  // 29. If dateTimeFormat.[[Hour]] is not undefined, then
  if (has_hour_option) {
    // a. Let hcDefault be dataLocaleData.[[hourCycle]].
    Intl::HourCycle hc_default = HourCycleDefault(date_format.get());
    // b. Let hc be dateTimeFormat.[[HourCycle]].
    Intl::HourCycle hc = hour_cycle;
    // c. If hc is null, then
    if (hc == Intl::HourCycle::kUndefined) {
      // i. Set hc to hcDefault.
      hc = hc_default;
    }
    // e.  Set dateTimeFormat.[[HourCycle]] to hc.
    date_time_format->set_hour_cycle(hc);
    // 30. Else
  } else {
    // a. Set dateTimeFormat.[[HourCycle]] to undefined.
    date_time_format->set_hour_cycle(Intl::HourCycle::kUndefined);
  }

  // 12.1.1 InitializeDateTimeFormat ( dateTimeFormat, locales, options )
  //
  // Steps 8-9 set opt.[[hc]] to value *other than undefined*
  // if "hour12" is set or "hourCycle" is set in the option.
  //
  // 9.2.6 ResolveLocale (... )
  // Step 8.h / 8.i and 8.k
  //
  // An hour12 option always overrides an hourCycle option.
  // Additionally hour12 and hourCycle both clear out any existing Unicode
  // extension key in the input locale.
  //
  // See details in https://github.com/tc39/test262/pull/2035
  if (maybe_get_hour12.FromJust() ||
      maybe_hour_cycle.FromJust() != Intl::HourCycle::kUndefined) {
    auto hc_extension_it = r.extensions.find("hc");
    if (hc_extension_it != r.extensions.end()) {
      if (date_time_format->hour_cycle() !=
          Intl::ToHourCycle(hc_extension_it->second.c_str())) {
        // Remove -hc- if it does not agree with what we used.
        UErrorCode status = U_ZERO_ERROR;
        icu_locale.setKeywordValue(uloc_toLegacyKey("hc"), nullptr, status);
        CHECK(U_SUCCESS(status));
      }
    }
  }

  Handle<Managed<icu::Locale>> managed_locale =
      Managed<icu::Locale>::FromRawPtr(isolate, 0, icu_locale.clone());

  date_time_format->set_icu_locale(*managed_locale);
  Handle<Managed<icu::SimpleDateFormat>> managed_format =
      Managed<icu::SimpleDateFormat>::FromUniquePtr(isolate, 0,
                                                    std::move(date_format));
  date_time_format->set_icu_simple_date_format(*managed_format);

  return date_time_format;
}

namespace {

// The list comes from third_party/icu/source/i18n/unicode/udat.h.
// They're mapped to DateTimeFormat components listed at
// https://tc39.github.io/ecma402/#sec-datetimeformat-abstracts .
Handle<String> IcuDateFieldIdToDateType(int32_t field_id, Isolate* isolate) {
  switch (field_id) {
    case -1:
      return isolate->factory()->literal_string();
    case UDAT_YEAR_FIELD:
    case UDAT_EXTENDED_YEAR_FIELD:
    case UDAT_YEAR_NAME_FIELD:
      return isolate->factory()->year_string();
    case UDAT_MONTH_FIELD:
    case UDAT_STANDALONE_MONTH_FIELD:
      return isolate->factory()->month_string();
    case UDAT_DATE_FIELD:
      return isolate->factory()->day_string();
    case UDAT_HOUR_OF_DAY1_FIELD:
    case UDAT_HOUR_OF_DAY0_FIELD:
    case UDAT_HOUR1_FIELD:
    case UDAT_HOUR0_FIELD:
      return isolate->factory()->hour_string();
    case UDAT_MINUTE_FIELD:
      return isolate->factory()->minute_string();
    case UDAT_SECOND_FIELD:
      return isolate->factory()->second_string();
    case UDAT_DAY_OF_WEEK_FIELD:
    case UDAT_DOW_LOCAL_FIELD:
    case UDAT_STANDALONE_DAY_FIELD:
      return isolate->factory()->weekday_string();
    case UDAT_AM_PM_FIELD:
      return isolate->factory()->dayPeriod_string();
    case UDAT_TIMEZONE_FIELD:
    case UDAT_TIMEZONE_RFC_FIELD:
    case UDAT_TIMEZONE_GENERIC_FIELD:
    case UDAT_TIMEZONE_SPECIAL_FIELD:
    case UDAT_TIMEZONE_LOCALIZED_GMT_OFFSET_FIELD:
    case UDAT_TIMEZONE_ISO_FIELD:
    case UDAT_TIMEZONE_ISO_LOCAL_FIELD:
      return isolate->factory()->timeZoneName_string();
    case UDAT_ERA_FIELD:
      return isolate->factory()->era_string();
    default:
      // Other UDAT_*_FIELD's cannot show up because there is no way to specify
      // them via options of Intl.DateTimeFormat.
      UNREACHABLE();
      // To prevent MSVC from issuing C4715 warning.
      return Handle<String>();
  }
}

}  // namespace

MaybeHandle<Object> JSDateTimeFormat::FormatToParts(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format,
    double date_value) {
  Factory* factory = isolate->factory();
  icu::SimpleDateFormat* format =
      date_time_format->icu_simple_date_format()->raw();
  CHECK_NOT_NULL(format);

  icu::UnicodeString formatted;
  icu::FieldPositionIterator fp_iter;
  icu::FieldPosition fp;
  UErrorCode status = U_ZERO_ERROR;
  format->format(date_value, formatted, &fp_iter, status);
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kIcuError), Object);
  }

  Handle<JSArray> result = factory->NewJSArray(0);
  int32_t length = formatted.length();
  if (length == 0) return result;

  int index = 0;
  int32_t previous_end_pos = 0;
  Handle<String> substring;
  while (fp_iter.next(fp)) {
    int32_t begin_pos = fp.getBeginIndex();
    int32_t end_pos = fp.getEndIndex();

    if (previous_end_pos < begin_pos) {
      ASSIGN_RETURN_ON_EXCEPTION(
          isolate, substring,
          Intl::ToString(isolate, formatted, previous_end_pos, begin_pos),
          Object);
      Intl::AddElement(isolate, result, index,
                       IcuDateFieldIdToDateType(-1, isolate), substring);
      ++index;
    }
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, substring,
        Intl::ToString(isolate, formatted, begin_pos, end_pos), Object);
    Intl::AddElement(isolate, result, index,
                     IcuDateFieldIdToDateType(fp.getField(), isolate),
                     substring);
    previous_end_pos = end_pos;
    ++index;
  }
  if (previous_end_pos < length) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, substring,
        Intl::ToString(isolate, formatted, previous_end_pos, length), Object);
    Intl::AddElement(isolate, result, index,
                     IcuDateFieldIdToDateType(-1, isolate), substring);
  }
  JSObject::ValidateElements(*result);
  return result;
}

std::set<std::string> JSDateTimeFormat::GetAvailableLocales() {
  int32_t num_locales = 0;
  const icu::Locale* icu_available_locales =
      icu::DateFormat::getAvailableLocales(num_locales);
  return Intl::BuildLocaleSet(icu_available_locales, num_locales);
}

Handle<String> JSDateTimeFormat::HourCycleAsString() const {
  switch (hour_cycle()) {
    case Intl::HourCycle::kUndefined:
      return GetReadOnlyRoots().undefined_string_handle();
    case Intl::HourCycle::kH11:
      return GetReadOnlyRoots().h11_string_handle();
    case Intl::HourCycle::kH12:
      return GetReadOnlyRoots().h12_string_handle();
    case Intl::HourCycle::kH23:
      return GetReadOnlyRoots().h23_string_handle();
    case Intl::HourCycle::kH24:
      return GetReadOnlyRoots().h24_string_handle();
    default:
      UNREACHABLE();
  }
}

}  // namespace internal
}  // namespace v8
