#ifndef BASE_DATE_H
#define BASE_DATE_H

// tec: proleptic Gregorian calendar <-> days-since-1970-01-01 conversion and ISO-8601 date/timestamp parsing/formatting


// days_from_civil/civil_from_days are Howard Hinnant's constant time algorithm 
// http://howardhinnant.github.io/date_algorithms.html

internal S32  days_from_civil(S32 year, U32 month, U32 day);
internal void civil_from_days(S32 days_since_epoch, S32* out_year, U32* out_month, U32* out_day);

// tec: parse_iso_date expects exactly "YYYY-MM-DD"
internal B32 parse_iso_date(String8 str, S32* out_days_since_epoch);
// tec: parse_iso_timestamp additionally accepts "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS"
internal B32 parse_iso_timestamp(String8 str, S64* out_seconds_since_epoch);

// tec: inverse formatters
internal String8 push_iso_date_string(Arena* arena, S32 days_since_epoch);
internal String8 push_iso_timestamp_string(Arena* arena, S64 seconds_since_epoch);

#endif //BASE_DATE_H
