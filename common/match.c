#include "match.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"

void
MatchNormalize(StrBuf *out, const char *text)
{
  u64 start = out->length;
  bool gap = false;
  for (const char *at = text; *at; at++) {
    char byte = *at;
    if (byte >= 'A' && byte <= 'Z') byte = (char)(byte - 'A' + 'a');

    bool word = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
    if (!word) {
      gap = true;
      continue;
    }
    if (gap && out->length > start) StrBufAppendByte(out, ' ');
    gap = false;
    StrBufAppendByte(out, byte);
  }
  if (!out->data) StrBufAppend(out, "", 0);
}

char *
MatchNormalized(const char *text)
{
  StrBuf out = {};
  MatchNormalize(&out, text);
  return out.data;
}

MatchQuery
MatchQueryParse(const char *query)
{
  MatchQuery parsed = {.text = MatchNormalized(query)};
  if (!*parsed.text) return parsed;

  // normalised text has single spaces between tokens, so there is one more token than space.
  u64 count = 1;
  for (const char *at = parsed.text; *at; at++) count += *at == ' ';
  parsed.tokens = MemoryResize(nullptr, count, sizeof(char *));

  char *token = parsed.text;
  while (true) {
    parsed.tokens[parsed.count++] = token;
    char *space = strchr(token, ' ');
    if (!space) break;
    *space = '\0';
    token = space + 1;
  }
  return parsed;
}

void
MatchQueryFree(MatchQuery *query)
{
  free(query->text);
  free(query->tokens);
  *query = (MatchQuery){};
}

bool
MatchWholeWord(const char *haystack, const char *token)
{
  u64 length = strlen(token);
  for (const char *at = strstr(haystack, token); at; at = strstr(at + 1, token)) {
    bool startsWord = at == haystack || at[-1] == ' ';
    bool endsWord = at[length] == '\0' || at[length] == ' ';
    if (startsWord && endsWord) return true;
  }
  return false;
}

i64
MatchTextScore(const char *haystack, const char *token)
{
  if (strcmp(haystack, token) == 0) return 120;

  const char *at = strstr(haystack, token);
  if (!at) return 0;
  if (at == haystack) return 90;

  // a later occurrence may start a word even when the first does not.
  for (const char *word = at; word; word = strstr(word + 1, token)) {
    if (word[-1] == ' ') return 70;
  }
  return 55;
}

// an entry and its score, while ranking.
typedef struct Ranked {
  u64 index;
  i64 score;
} Ranked;

// what the sort comparison needs besides the two entries.
typedef struct RankContext {
  MatchCompare compare;
  void *context;
} RankContext;

static int
CompareRanked(const void *left, const void *right, void *context)
{
  const Ranked *a = left;
  const Ranked *b = right;
  const RankContext *rank = context;
  if (a->score != b->score) return a->score > b->score ? -1 : 1;

  int order = rank->compare ? rank->compare(rank->context, a->index, b->index) : 0;
  if (order != 0) return order;
  return a->index < b->index ? -1 : a->index > b->index;
}

u64
MatchRank(
  u64 count,
  const MatchQuery *query,
  MatchScore score,
  MatchCompare compare,
  void *context,
  u64 *out
)
{
  Ranked *ranked = MemoryResize(nullptr, count, sizeof(Ranked));
  u64 matched = 0;
  for (u64 index = 0; index < count; index++) {
    i64 value = query->count ? score(context, index, query) : 1;
    if (value >= 0) ranked[matched++] = (Ranked){.index = index, .score = value};
  }

  RankContext rank = {.compare = compare, .context = context};
  qsort_r(ranked, matched, sizeof(Ranked), CompareRanked, &rank);
  for (u64 index = 0; index < matched; index++) out[index] = ranked[index].index;
  free(ranked);
  return matched;
}
