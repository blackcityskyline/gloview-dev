// The config seam, both directions.
//
// A key takes three edits: the struct field in config.hpp, a row in one of the
// kInts/kFloats/kStrs/kColors tables (or anim.cpp's kRows), and a README row. Two of
// the three mismatches are silent, and Hyprland does not paper over either of them:
//
//   1. Field declared, never registered. The key simply does not exist; hyprctl
//      answers "no such option" and the user finds out.
//   2. An example in the README names a key that was never registered. Hyprland
//      ERRORS on an unknown plugin key rather than ignoring it, so pasting the
//      documented example takes config load down with it. `duration` sat in both
//      the Lua and the hyprland.conf blocks that way.
//
// Registered is also necessary-but-not-sufficient for 2: a registered key missing
// from the README is a doc hole, reported last and only after the real failures.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string slurp(const std::string &p) {
  std::ifstream f(p);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", p.c_str());
    std::exit(2);
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

bool isIdent(const std::string &s) {
  return !s.empty() && (isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_') &&
         s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
             std::string::npos;
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else
      cur += c;
  }
  out.push_back(cur);
  return out;
}

// Every key name registered by the plugin, from both registration tables.
std::set<std::string> registeredKeys(const std::string &src) {
  std::set<std::string> out;
  const std::string needle = "\"plugin:gloview:";
  for (size_t p = src.find(needle); p != std::string::npos; p = src.find(needle, p + 1)) {
    const auto end = src.find('"', p + needle.size());
    if (end == std::string::npos)
      break;
    const auto key = src.substr(p + needle.size(), end - p - needle.size());
    // anim.cpp builds its keys by concatenation -- "plugin:gloview:" + stem + suffix --
    // so the literal alone names no option.
    if (isIdent(key))
      out.insert(key);
  }
  return out;
}

// `key = value` assignments in a config block. Only the flat ini form: the Lua
// example nests tables, so its `plugin = {` / `gloview = {` are structure, not keys.
std::set<std::string> assignedKeys(const std::string &block) {
  std::set<std::string> out;
  std::istringstream in(block);
  std::string line;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    // A config assignment is `key = value` with nothing but indent before the key:
    // key, spaces, '=', spaces. Anything else (a Lua table key, a comparison) is not
    // one, and matching on '=' alone swept in half the README.
    auto e = line.find_first_not_of(" \t");
    if (e == std::string::npos || e > eq)
      continue;
    auto kEnd = line.find_first_of(" \t=", e);
    if (kEnd == std::string::npos || kEnd > eq)
      continue;
    const auto key = line.substr(e, kEnd - e);
    if (isIdent(key))
      out.insert(key);
  }
  return out;
}

// Identifiers README presents as config keys: the `backticked` ones in the tables
// plus the `plugin:gloview:` spellings, which is how the tables are indexed.
std::set<std::string> documentedKeys(const std::string &readme) {
  std::set<std::string> out;
  for (size_t p = readme.find('`'); p != std::string::npos; p = readme.find('`', p + 1)) {
    const auto end = readme.find('`', p + 1);
    if (end == std::string::npos)
      break;
    const auto tok = readme.substr(p + 1, end - p - 1);
    // Leaf rows are documented as `<leaf>_ms`; keep the stem so they match.
    for (const auto &part : split(tok, '/'))
      for (const auto &w : split(part, ' '))
        if (isIdent(w))
          out.insert(w);
    if (end + 1 >= readme.size())
      break;
  }
  for (const auto &k : registeredKeys(readme))
    out.insert(k);
  return out;
}

std::string section(const std::string &doc, const std::string &begin, const std::string &end) {
  const auto b = doc.find(begin);
  if (b == std::string::npos)
    return {};
  // Search past the heading itself, or `end` matches the heading's own marker and
  // the block comes back empty.
  const auto e = doc.find(end, b + begin.size());
  return doc.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

} // namespace

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : ".";
  const auto hpp = slurp(root + "/src/config/config.hpp");
  const auto cpp = slurp(root + "/src/config/config.cpp");
  const auto anim = slurp(root + "/src/anim.cpp");
  const auto readme = slurp(root + "/README.md");

  int declared = 0, bad = 0;
  std::string current;
  std::istringstream in(hpp);
  std::string line;
  while (std::getline(in, line)) {
    const auto open = line.find("struct ");
    if (open != std::string::npos && line.find('{') != std::string::npos) {
      current = line.substr(open + 7, line.find('{') - open - 7);
      while (!current.empty() && (current.back() == ' ' || current.back() == '\t'))
        current.pop_back();
      continue; // a leaf/rule struct is not a config handle
    }
    if (current.empty())
      continue;
    if (line.find('}') != std::string::npos) {
      current.clear();
      continue;
    }
    const auto semi = line.find(';');
    if (semi == std::string::npos || line.find("//") != std::string::npos)
      continue;
    line = line.substr(0, semi); // the last token would otherwise carry the ';'

    if (current == "Leaf" || current == "LeafSpec" || current == "AnimRule")
      continue;
    std::istringstream ds(line);
    std::string type, field;
    if (!(ds >> type >> field))
      continue;
    ++declared;
    const std::string ref = "&" + current + "." + field;
    if (!contains(cpp, ref)) {
      std::printf("NOT REGISTERED: %s.%s  (%s)\n", current.c_str(), field.c_str(),
                  type.c_str());
      ++bad;
    }
  }
  std::printf("---\n%d fields declared, %d not registered\n", declared, bad);
  if (bad) {
    std::puts("FAIL: a config field exists but hyprctl would answer 'no such option'");
    return 1;
  }

  auto reg = registeredKeys(cpp);
  {
    const auto a = registeredKeys(anim);
    reg.insert(a.begin(), a.end());
  }

  // (2) An example naming an unregistered key. This one takes config load down:
  // Hyprland errors on an unknown plugin key instead of ignoring it.
  int ghost = 0;
  for (const auto &blk : {"### hyprland.conf", "### Lua"}) {
    const auto body = section(readme, blk, "###");
    for (const auto &k : assignedKeys(body)) {
      if (k == "plugin" || k == "gloview" || k == "require" || k == "pcall")
        continue;
      if (!reg.count(k)) {
        std::printf("UNKNOWN KEY: `%s` in the %s example is not registered "
                    "-- pasting it breaks config load\n", k.c_str(), blk);
        ++ghost;
      }
    }
  }
  if (ghost) {
    std::puts("FAIL: the documented example configures keys that do not exist");
    return 1;
  }

  // (3) A registered key the README never mentions. Not fatal to a session, so it
  // is reported rather than failed -- but it is a hole the user fills in blind.
  const auto doc = documentedKeys(readme);
  int undocumented = 0;
  for (const auto &k : reg) {
    if (!doc.count(k)) {
      std::printf("UNDOCUMENTED: %s is registered but has no README row\n", k.c_str());
      ++undocumented;
    }
  }
  std::printf("---\n%d keys registered, %d used in examples, %d undocumented\n", (int)reg.size(),
              (int)reg.size() - ghost, undocumented);
  if (undocumented) {
    std::puts("FAIL: a key exists that the README never mentions");
    return 1;
  }

  std::puts("OK");
  return 0;
}