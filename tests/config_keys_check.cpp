// A config field declared in config.hpp must be REGISTERED in config.cpp.
//
// Adding a key takes three edits -- the struct in config.hpp, a row in one of the
// kInts/kFloats/kStrs/kColors tables, and a README row -- and nothing checked the seam.
// Putting a Float row in kInts fails to COMPILE (loud, fine). The silent one is adding a
// field to a struct and never registering it: the key does not exist, hyprctl answers
// "no such option", and the user is the one who finds out. This catches that, and only that.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

} // namespace

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : ".";
  const auto hpp = slurp(root + "/src/config/config.hpp");
  const auto cpp = slurp(root + "/src/config/config.cpp");

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
  std::puts("OK");
  return 0;
}