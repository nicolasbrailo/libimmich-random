#include "immich.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Longest name pattern we can match. Immich caps album names well below this,
// so a longer pattern can't match anything anyway.
#define MAX_PATTERN 256

// Case-insensitive glob over the whole string. Only '*' and '?' are special;
// every other character is literal, including the '[' that fnmatch() reads as
// a set and the '\' it reads as an escape. That is what makes an album called
// "Trip [2019]" matchable by typing its name, which fnmatch() cannot do.
//
// Iterative with backtracking rather than recursive: the patterns come from
// whoever can publish to the broker, and a recursive matcher fed "*a*a*a*..."
// recurses once per star per character.
static bool glob_match(const char *pat, const char *str) {
  const char *star = NULL;  // Last '*' seen in pat, or NULL if none yet
  const char *retry = NULL; // Where str resumes if we backtrack to that '*'
  while (*str) {
    if (*pat == '?' || (*pat && tolower((unsigned char)*pat) ==
                                    tolower((unsigned char)*str))) {
      pat++;
      str++;
    } else if (*pat == '*') {
      star = pat++;
      retry = str;
    } else if (star) {
      // Mismatch, but an earlier '*' can absorb one more character
      pat = star + 1;
      str = ++retry;
    } else {
      return false;
    }
  }
  // Trailing stars may match nothing at all
  while (*pat == '*') {
    pat++;
  }
  return *pat == '\0';
}

// True if list holds at least one non-empty pattern. "" and " , ," don't, so
// they mean "no patterns" rather than "one empty pattern that matches nothing".
static bool has_patterns(const char *list) {
  for (; list && *list; list++) {
    if (*list != ',' && !isspace((unsigned char)*list)) {
      return true;
    }
  }
  return false;
}

// True if name matches any of the comma-separated patterns in list. An empty
// list matches nothing; callers decide what "no patterns" means, since it is
// "everything passes" for `name` and "nothing is dropped" for `exclude`.
static bool matches_any(const char *list, const char *name) {
  while (list && *list) {
    while (*list == ',') {
      list++;
    }
    const char *begin = list;
    while (*list && *list != ',') {
      list++;
    }
    const char *end = list; // One past the piece, before trimming
    while (begin < end && isspace((unsigned char)*begin)) {
      begin++;
    }
    while (end > begin && isspace((unsigned char)end[-1])) {
      end--;
    }

    size_t len = (size_t)(end - begin);
    // Truncating an over-long pattern would silently turn it into a different
    // one, which is worse than it matching nothing.
    if (len == 0 || len >= MAX_PATTERN) {
      continue;
    }

    // glob_match needs a NUL-terminated pattern, and the pieces are slices of
    // a caller-owned string we must not write to.
    char pat[MAX_PATTERN];
    memcpy(pat, begin, len);
    pat[len] = '\0';
    if (glob_match(pat, name)) {
      return true;
    }
  }
  return false;
}

// The year of an ISO 8601 timestamp ("2019-07-14T12:34:56.000Z"), or 0 if it
// doesn't start with four digits. 0 doubles as "no date": Immich has no year
// zero, and a missing date arrives here as "".
static unsigned iso_year(const char *iso) {
  if (!iso) {
    return 0;
  }
  unsigned year = 0;
  for (int i = 0; i < 4; i++) {
    // A short string stops here on its NUL, so this never reads past the end
    if (iso[i] < '0' || iso[i] > '9') {
      return 0;
    }
    year = year * 10 + (unsigned)(iso[i] - '0');
  }
  return year;
}

bool immich_album_filter_match(const struct immich_album_filter *f,
                               const struct immich_album *a) {
  // An empty album passes every test a dateless album passes and then yields
  // nothing, which shows up as a stall rather than as an error
  if (a->asset_count <= 0) {
    return false;
  }
  if (!f) {
    return true;
  }

  const char *name = a->name ? a->name : "";
  if (has_patterns(f->name) && !matches_any(f->name, name)) {
    return false;
  }
  // Evaluated after `name`, so exclude wins on a conflict
  if (matches_any(f->exclude, name)) {
    return false;
  }

  if (f->from_year == 0 && f->to_year == 0) {
    return true;
  }

  unsigned first = iso_year(a->start_date);
  unsigned last = iso_year(a->end_date);
  if (first == 0) {
    first = last;
  }
  if (last == 0) {
    last = first;
  }
  if (first == 0) {
    // No dates at all, and a bound is set
    return false;
  }

  // Overlap: keep the album if any part of its span falls inside the range
  if (f->from_year != 0 && last < f->from_year) {
    return false;
  }
  if (f->to_year != 0 && first > f->to_year) {
    return false;
  }
  return true;
}
