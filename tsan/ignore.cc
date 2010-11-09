#include "stringmatch.h"

#include "ignore.h"

IgnoreLists *g_ignore_lists;

static void SplitStringIntoLinesAndRemoveBlanksAndComments(
    const string &str, vector<string> *lines) {
  string cur_line;
  bool in_comment = false;
  for (size_t pos = 0; pos < str.size(); pos++) {
    char ch = str[pos];
    if (ch == '\n') {
      if (!cur_line.empty()) {
        // Printf("++ %s\n", cur_line.c_str());
        lines->push_back(cur_line);
      }
      cur_line.clear();
      in_comment = false;
      continue;
    }
    if (ch == ' ' || ch == '\t') continue;
    if (ch == '#') {
      in_comment = true;
      continue;
    }
    if (!in_comment) {
      cur_line += ch;
    }
  }
}

static bool CutStringPrefixIfPresent(const string &input, const string &prefix,
                     /* OUT */ string *output) {
  if (input.find(prefix) == 0) {
    *output = input.substr(prefix.size());
    return true;
  } else {
    return false;
  }
}

static bool ReadIgnoreLine(string input_line, IgnoreLists *ignore_lists) {
  string tail;
  if (CutStringPrefixIfPresent(input_line, "obj:", &tail)) {
    ignore_lists->ignores.push_back(IgnoreObj(tail));
  } else if (CutStringPrefixIfPresent(input_line, "src:", &tail)) {
    ignore_lists->ignores.push_back(IgnoreFile(tail));
  } else if (CutStringPrefixIfPresent(input_line, "fun:", &tail)) {
    ignore_lists->ignores.push_back(IgnoreFun(tail));
  } else if (CutStringPrefixIfPresent(input_line, "fun_r:", &tail)) {
    ignore_lists->ignores_r.push_back(IgnoreFun(tail));
  } else if (CutStringPrefixIfPresent(input_line, "fun_hist:", &tail)) {
    ignore_lists->ignores_hist.push_back(IgnoreFun(tail));
  } else {
    return false;
  }
  return true;
}

void ReadIgnoresFromString(string ignoreString) {
  vector<string> lines;
  SplitStringIntoLinesAndRemoveBlanksAndComments(ignoreString, &lines);
  for (size_t j = 0; j < lines.size(); j++) {
    string &line = lines[j];
    bool line_parsed = ReadIgnoreLine(line, g_ignore_lists);
    if (!line_parsed) {
      Printf("Error reading ignore file line:\n%s\n", line.c_str());
      CHECK(0);
    }
  }
}

static bool StringVectorMatch(const vector<string>& v, const string& s) {
  for (size_t i = 0; i < v.size(); i++) {
    if (StringMatch(v[i], s))
      return true;
  }
  return false;
}

// True iff there exists a triple each of which components is either empty
// or matches the corresponding string.
bool TripleVectorMatchKnown(const vector<IgnoreTriple>& v,
                       const string& fun,
                       const string& obj,
                       const string& file) {
  for (size_t i = 0; i < v.size(); i++) {
    if ((fun.size() == 0 || StringMatch(v[i].fun, fun)) &&
        (obj.size() == 0 || StringMatch(v[i].obj, obj)) &&
        (file.size() == 0 || StringMatch(v[i].file, file))) {
      if ((fun.size() == 0 || v[i].fun == "*") &&
          (obj.size() == 0 || v[i].obj == "*") &&
          (file.size() == 0 || v[i].file == "*")) {
        // At least one of the matched features should be either non-empty
        // or match a non-trivial pattern.
        // For example, a <*, *, filename.ext> triple should NOT match
        // fun="fun", obj="obj.o", file="".
        continue;
      } else {
        return true;
      }
    }
  }
  return false;
}
