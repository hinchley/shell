#pragma once

// fuzzy-ish search shared by the launcher and hyprshorts: text is normalised to lowercase
// words, a query is split into tokens, and every token must match an entry for it to be
// listed.
// the scores are those of the quickshell config this replaces.
#include "strbuf.h"
#include "types.h"

// a query split into normalised tokens.
typedef struct MatchQuery {
  // the normalised query, with a nul after each token.
  char *text;
  char **tokens;
  u64 count;
} MatchQuery;

// how well an entry matches: -1 when it does not.
typedef i64 (*MatchScore)(void *context, u64 index, const MatchQuery *query);

// orders two entries that scored the same, like strcmp.
typedef int (*MatchCompare)(void *context, u64 left, u64 right);

// appends text normalised: ascii letters lowercased and digits kept, every run of other bytes
// turned into one space, and no space at either end.
void
MatchNormalize(StrBuf *out, const char *text);

// a heap copy of text normalised.
char *
MatchNormalized(const char *text);

MatchQuery
MatchQueryParse(const char *query);

void
MatchQueryFree(MatchQuery *query);

// how well a normalised token matches a normalised haystack: 120 when they are equal, 90 when
// the haystack starts with it, 70 when a word does, 55 when it appears anywhere, and 0
// otherwise.
i64
MatchTextScore(const char *haystack, const char *token);

// true when token appears in haystack as a whole word.
bool
MatchWholeWord(const char *haystack, const char *token);

// fills out (which has room for count) with the indexes of the entries that match the query,
// best first, ties ordered by compare and then by index.
// an empty query matches every entry equally.
// the number of indexes written.
u64
MatchRank(
  u64 count,
  const MatchQuery *query,
  MatchScore score,
  MatchCompare compare,
  void *context,
  u64 *out
);
