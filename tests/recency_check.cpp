// Recency is pure and shared, so it gets checked directly. Asserts only: the
// failure message names the invariant, not the line.
//
// build: c++ -std=c++23 -Wall -Wextra tests/recency_check.cpp
//        src/model/recency.cpp -o /tmp/recency_check

#include "../src/model/recency.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

using gloview::model::Recency;

namespace {

int g_fails = 0;

void check(const bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_fails++;
  }
}

const void *A = reinterpret_cast<const void *>(0xA0);
const void *B = reinterpret_cast<const void *>(0xB0);
const void *C = reinterpret_cast<const void *>(0xC0);

std::vector<std::size_t> orderOf(Recency &r,
                                 const std::vector<const void *> &keys) {
  std::vector<std::size_t> out;
  r.order(keys, out);
  return out;
}

void testPromoteOrdersFrontFirst() {
  Recency r;
  r.promote(A);
  r.promote(B);
  check(r.rank(A) == 1 && r.rank(B) == 0, "the last promoted is rank 0");

  // Re-promoting an existing window rotates it to the front; the other's rank
  // shifts down by one. This is the whole point: a window the user returns to
  // must outrank one they left.
  r.promote(A);
  check(r.rank(A) == 0 && r.rank(B) == 1, "re-promote rotates to the front");

  const auto keys = std::vector<const void *>{B, A};
  const auto ord = orderOf(r, keys);
  check((ord == std::vector<std::size_t>{1, 0}), "order follows recency, not input");
}

void testUnrankedSortsLast() {
  Recency r;
  r.promote(A);
  const auto keys = std::vector<const void *>{C, A};
  const auto ord = orderOf(r, keys);
  // C is unranked, so it lands after A rather than in front of it.
  check((ord == std::vector<std::size_t>{1, 0}), "unranked sorts after ranked");
  check(r.rank(C) == static_cast<std::size_t>(-1), "unranked rank is max");
}

void testUnrankedKeepInputOrder() {
  Recency r;
  r.promote(A);
  const auto keys = std::vector<const void *>{C, B};
  const auto ord = orderOf(r, keys);
  // Both unranked: the caller's order is the tie-break, so it must survive.
  check((ord == std::vector<std::size_t>{0, 1}), "equal ranks keep input order");
}

void testForgetDead() {
  Recency r;
  r.promote(A);
  r.promote(B);
  r.forgetDead({A});
  check(r.rank(B) == static_cast<std::size_t>(-1) && r.size() == 1,
        "a dead window loses its rank");
  // The address is reused by the next window; it must come in as NEW, not
  // inheriting the dead one's rank.
  r.promote(B);
  check(r.rank(B) == 0, "a reused address ranks as new");
}

void testSelfPromoteIsANoOp() {
  Recency r;
  r.promote(A);
  r.promote(A);
  check(r.size() == 1 && r.rank(A) == 0, "promoting twice does not duplicate");
  r.promote(nullptr);
  check(r.size() == 1, "a null window is ignored");
}

} // namespace

int main() {
  testPromoteOrdersFrontFirst();
  testUnrankedSortsLast();
  testUnrankedKeepInputOrder();
  testForgetDead();
  testSelfPromoteIsANoOp();
  if (g_fails) {
    std::printf("%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("recency ok\n");
  return 0;
}