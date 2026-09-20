// Tests for immich_album_filter_match(). Pure logic, no server and no network:
// build and run with `make test`.

#include "immich.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

// The album list entries are never written to, so casting away const to fill
// immich_album's owned-string fields is safe here
static struct immich_album album(const char *name, int assets,
                                 const char *start, const char *end) {
  struct immich_album a = {
      .id = (char *)"00000000-0000-0000-0000-000000000000",
      .name = (char *)name,
      .asset_count = assets,
      .start_date = (char *)start,
      .end_date = (char *)end,
  };
  return a;
}

static void check(const char *what, bool got, bool want) {
  if (got == want) {
    return;
  }
  fprintf(stderr, "FAIL: %s: got %s, want %s\n", what, got ? "keep" : "drop",
          want ? "keep" : "drop");
  g_failures++;
}

// Asserts that f keeps (or drops) an album with this name, 3 assets and no
// dates
static void check_name(const struct immich_album_filter *f, const char *name,
                       bool want) {
  char what[256];
  snprintf(what, sizeof(what), "name=\"%s\" exclude=\"%s\" vs \"%s\"",
           f->name ? f->name : "", f->exclude ? f->exclude : "", name);
  struct immich_album a = album(name, 3, "", "");
  check(what, immich_album_filter_match(f, &a), want);
}

// Asserts that a year range keeps (or drops) an album spanning start..end
static void check_years(unsigned from, unsigned to, const char *start,
                        const char *end, bool want) {
  char what[256];
  snprintf(what, sizeof(what), "years %u..%u vs album %s..%s", from, to,
           start[0] ? start : "(none)", end[0] ? end : "(none)");
  struct immich_album_filter f = {.from_year = from, .to_year = to};
  struct immich_album a = album("Album", 3, start, end);
  check(what, immich_album_filter_match(&f, &a), want);
}

static void test_empty_filter_keeps_everything(void) {
  struct immich_album_filter f = {0};
  check_name(&f, "portalgo-kids", true);
  check_name(&f, "Holidays 2015", true);

  struct immich_album a = album("Anything", 3, "2019-07-14T12:00:00.000Z",
                                "2020-01-01T12:00:00.000Z");
  check("NULL filter keeps everything", immich_album_filter_match(NULL, &a),
        true);
}

static void test_blank_patterns_are_no_patterns(void) {
  struct immich_album_filter f = {.name = " , ,"};
  check_name(&f, "portalgo-kids", true);
  check_name(&f, "Screenshots", true);

  struct immich_album_filter empty = {.name = "", .exclude = ""};
  check_name(&empty, "portalgo-kids", true);
}

static void test_glob_is_anchored(void) {
  struct immich_album_filter star = {.name = "portalgo-*"};
  check_name(&star, "portalgo-kids", true);
  check_name(&star, "portalgo-pets", true);
  check_name(&star, "Screenshots", false);
  // A trailing '*' matches nothing at all
  check_name(&star, "portalgo-", true);

  // No wildcard means the whole name, so a prefix keeps nothing
  struct immich_album_filter plain = {.name = "portalgo"};
  check_name(&plain, "portalgo-kids", false);
  check_name(&plain, "portalgo", true);

  // ...and neither does a substring
  struct immich_album_filter substr = {.name = "Pets"};
  check_name(&substr, "Pets 2019 backup", false);
  check_name(&substr, "Pets", true);
}

static void test_case_insensitive(void) {
  struct immich_album_filter f = {.name = "PORTALGO-PETS, screen*"};
  check_name(&f, "portalgo-pets", true);
  check_name(&f, "Screenshots", true);
  check_name(&f, "portalgo-kids", false);
}

static void test_metacharacters_are_literal(void) {
  struct immich_album_filter paren = {.name = "Trip (2019)"};
  check_name(&paren, "Trip (2019)", true);
  check_name(&paren, "Trip 2019", false);

  // '[' would be a character set to fnmatch(), and '.' '+' a metacharacter to
  // a regex engine
  struct immich_album_filter bracket = {.name = "Trip [2019]"};
  check_name(&bracket, "Trip [2019]", true);
  check_name(&bracket, "Trip 2", false);

  struct immich_album_filter dot = {.name = "a.b+c"};
  check_name(&dot, "a.b+c", true);
  check_name(&dot, "axbxc", false);

  // '\' is an escape to fnmatch(); here it is just a character
  struct immich_album_filter esc = {.name = "a\\*b"};
  check_name(&esc, "a\\*b", true);
  check_name(&esc, "a*b", false);
}

static void test_question_mark_is_exactly_one(void) {
  struct immich_album_filter f = {.name = "Holidays ????"};
  check_name(&f, "Holidays 2015", true);
  check_name(&f, "Holidays 15", false);
  check_name(&f, "Holidays 20155", false);
}

static void test_exclude_wins(void) {
  struct immich_album_filter f = {.name = "portalgo-*", .exclude = "*-pets"};
  check_name(&f, "portalgo-kids", true);
  check_name(&f, "portalgo-pets", false);

  // exclude on its own keeps everything else
  struct immich_album_filter only = {.exclude = "Screenshots, WhatsApp *"};
  check_name(&only, "Screenshots", false);
  check_name(&only, "WhatsApp Images", false);
  check_name(&only, "Holidays 2015", true);
}

static void test_year_overlap(void) {
  const char *y2010 = "2010-03-01T00:00:00.000Z";
  const char *y2026 = "2026-03-01T00:00:00.000Z";
  const char *y2015 = "2015-03-01T00:00:00.000Z";
  const char *y2016 = "2016-03-01T00:00:00.000Z";

  // Spans the whole requested range, though neither endpoint is inside it
  check_years(2019, 2021, y2010, y2026, true);
  // Entirely before / after
  check_years(2019, 2021, y2010, y2015, false);
  check_years(2019, 2021, "2022-01-01T00:00:00.000Z", y2026, false);
  // Touching either edge
  check_years(2016, 2021, y2010, y2016, true);
  check_years(2010, 2015, y2015, y2026, true);

  // Each bound constrains only its own end
  check_years(2022, 0, y2010, y2026, true);
  check_years(2022, 0, y2010, y2015, false);
  check_years(0, 2012, y2010, y2026, true);
  check_years(0, 2009, y2010, y2026, false);
}

static void test_dateless_albums(void) {
  // Kept when no bound is set, dropped as soon as either is
  check_years(0, 0, "", "", true);
  check_years(2019, 0, "", "", false);
  check_years(0, 2019, "", "", false);

  // One date missing: the other stands in for it
  check_years(2019, 2021, "", "2020-01-01T00:00:00.000Z", true);
  check_years(2019, 2021, "2020-01-01T00:00:00.000Z", "", true);
  check_years(2019, 2021, "", "2015-01-01T00:00:00.000Z", false);

  // A date that isn't an ISO timestamp counts as no date
  check_years(2019, 2021, "not-a-date", "", false);
}

static void test_empty_albums_never_appear(void) {
  struct immich_album_filter none = {0};
  struct immich_album a =
      album("Empty", 0, "2019-07-14T12:00:00.000Z", "2020-01-01T12:00:00.000Z");
  check("assetCount 0, no filter", immich_album_filter_match(&none, &a), false);
  check("assetCount 0, NULL filter", immich_album_filter_match(NULL, &a),
        false);

  struct immich_album_filter named = {.name = "Empty"};
  check("assetCount 0, name matches", immich_album_filter_match(&named, &a),
        false);
}

static void test_names_are_not_evidence_about_dates(void) {
  // A real album called "2024 - office" whose assets run 2019 to 2024
  struct immich_album a = album("2024 - office", 12, "2019-10-31T00:00:00.000Z",
                                "2024-08-01T00:00:00.000Z");
  struct immich_album_filter f = {.from_year = 2019, .to_year = 2019};
  check("name says 2024, dates say 2019", immich_album_filter_match(&f, &a),
        true);
}

int main(void) {
  test_empty_filter_keeps_everything();
  test_blank_patterns_are_no_patterns();
  test_glob_is_anchored();
  test_case_insensitive();
  test_metacharacters_are_literal();
  test_question_mark_is_exactly_one();
  test_exclude_wins();
  test_year_overlap();
  test_dateless_albums();
  test_empty_albums_never_appear();
  test_names_are_not_evidence_about_dates();

  if (g_failures > 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("album_filter: all checks passed\n");
  return 0;
}
