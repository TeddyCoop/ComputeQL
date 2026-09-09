internal S32
days_from_civil(S32 year, U32 month, U32 day)
{
  S32 y = year - (S32)(month <= 2);
  S32 era = (y >= 0 ? y : y - 399) / 400;
  U32 yoe = (U32)(y - era * 400);                                    // [0, 399]
  U32 doy = (153 * (month + (month > 2 ? (U32)-3 : 9)) + 2) / 5 + day - 1; // [0, 365]
  U32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                   // [0, 146096]
  return (S32)(era * 146097 + (S32)doe - 719468);
}

internal void
civil_from_days(S32 days_since_epoch, S32* out_year, U32* out_month, U32* out_day)
{
  S32 z = days_since_epoch + 719468;
  S32 era = (z >= 0 ? z : z - 146096) / 146097;
  U32 doe = (U32)(z - era * 146097);                                       // [0, 146096]
  U32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;         // [0, 399]
  S32 y = (S32)yoe + era * 400;
  U32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                       // [0, 365]
  U32 mp = (5 * doy + 2) / 153;                                            // [0, 11]
  U32 day = doy - (153 * mp + 2) / 5 + 1;                                  // [1, 31]
  U32 month = mp + (mp < 10 ? 3 : -9);                                     // [1, 12]
  y = y + (S32)(month <= 2);
  
  *out_year = y;
  *out_month = month;
  *out_day = day;
}

internal B32
parse_iso_date(String8 str, S32* out_days_since_epoch)
{
  if (str.size != 10) return 0;
  if (str.str[4] != '-' || str.str[7] != '-') return 0;
  
  String8 y_str = str8_prefix(str, 4);
  String8 m_str = str8_substr(str, r1u64(5, 7));
  String8 d_str = str8_substr(str, r1u64(8, 10));
  if (!str8_is_integer(y_str, 10) || !str8_is_integer(m_str, 10) || !str8_is_integer(d_str, 10)) return 0;
  
  S32 year = (S32)u64_from_str8(y_str, 10);
  U32 month = (U32)u64_from_str8(m_str, 10);
  U32 day = (U32)u64_from_str8(d_str, 10);
  if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
  
  *out_days_since_epoch = days_from_civil(year, month, day);
  return 1;
}

internal B32
parse_iso_timestamp(String8 str, S64* out_seconds_since_epoch)
{
  if (str.size == 10)
  {
    S32 days = 0;
    if (!parse_iso_date(str, &days)) return 0;
    *out_seconds_since_epoch = (S64)days * 86400;
    return 1;
  }
  
  if (str.size != 19) return 0;
  if (str.str[10] != ' ' && str.str[10] != 'T') return 0;
  if (str.str[13] != ':' || str.str[16] != ':') return 0;
  
  S32 days = 0;
  if (!parse_iso_date(str8_prefix(str, 10), &days)) return 0;
  
  String8 h_str = str8_substr(str, r1u64(11, 13));
  String8 mi_str = str8_substr(str, r1u64(14, 16));
  String8 s_str = str8_substr(str, r1u64(17, 19));
  if (!str8_is_integer(h_str, 10) || !str8_is_integer(mi_str, 10) || !str8_is_integer(s_str, 10)) return 0;
  
  U32 hour = (U32)u64_from_str8(h_str, 10);
  U32 minute = (U32)u64_from_str8(mi_str, 10);
  U32 second = (U32)u64_from_str8(s_str, 10);
  if (hour > 23 || minute > 59 || second > 59) return 0;
  
  *out_seconds_since_epoch = (S64)days * 86400 + (S64)hour * 3600 + (S64)minute * 60 + (S64)second;
  return 1;
}

internal String8
push_iso_date_string(Arena* arena, S32 days_since_epoch)
{
  S32 year = 0; U32 month = 0, day = 0;
  civil_from_days(days_since_epoch, &year, &month, &day);
  return push_str8f(arena, "%04d-%02u-%02u", year, month, day);
}

internal String8
push_iso_timestamp_string(Arena* arena, S64 seconds_since_epoch)
{
  // tec: floor division so a negative (pre-1970) timestamp still splits into a valid [0,86399] remainder
  S64 days = (seconds_since_epoch >= 0)
    ? (seconds_since_epoch / 86400) : -(((-seconds_since_epoch) + 86399) / 86400);
  S64 rem = seconds_since_epoch - days * 86400;
  
  S32 year = 0; U32 month = 0, day = 0;
  civil_from_days((S32)days, &year, &month, &day);
  
  U32 hour = (U32)(rem / 3600);
  U32 minute = (U32)((rem % 3600) / 60);
  U32 second = (U32)(rem % 60);
  
  return push_str8f(arena, "%04d-%02u-%02u %02u:%02u:%02u", year, month, day, hour, minute, second);
}
